#include "storage.internal.h"

#include <stdio.h>
#include <string.h>

#ifdef NS_WIN
    #include <windows.h>
    #include <direct.h>
    #define STORAGE_CACHE_SEP "\\"
#else
    #include <dirent.h>
    #include <unistd.h>
    #define STORAGE_CACHE_SEP "/"
#endif

// Content-addressed blob cache below the app data directory. Entries are files
// rather than KV values or SQLite blobs because their consumers are platform
// APIs that read and write a path: a Metal binary archive serializes to a URL,
// and a decoded image is memory-mapped. `name` identifies what is cached and
// `hash` identifies the exact bytes it was produced from, so a changed input
// misses instead of returning a stale entry.

#define STORAGE_CACHE_DIR "cache"
#define STORAGE_CACHE_SUFFIX ".bin"
#define STORAGE_CACHE_NAME_CAPACITY 128

static char storage_cache_result[STORAGE_PATH_CAPACITY];

// Entry files carry the name verbatim, so anything that could escape the cache
// directory or upset a filesystem collapses to '_'. Distinct names can fold
// together here; the hash still separates their contents.
static i32 storage_cache_safe_name(const char *name, char *out, size_t capacity) {
    if (!name || !name[0] || capacity < 2) {
        storage_set_error("cache name must not be empty");
        return 0;
    }
    size_t length = 0;
    for (const unsigned char *input = (const unsigned char *)name; *input && length + 1 < capacity; ++input) {
        unsigned char c = *input;
        i32 keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[length++] = keep ? (char)c : '_';
    }
    out[length] = '\0';
    return 1;
}

// The cache directory is created on demand so a program that never caches
// anything leaves no directory behind.
static i32 storage_cache_dir(char *out, size_t capacity, i32 create) {
    const char *root = storage_app_data_dir();
    if (!root || !root[0]) return 0;
    i32 count = snprintf(out, capacity, "%s" STORAGE_CACHE_SEP STORAGE_CACHE_DIR, root);
    if (count < 0 || (size_t)count >= capacity) {
        storage_set_error("cache directory path is too long");
        return 0;
    }
    return create ? storage_make_dirs(out) : 1;
}

static i32 storage_cache_entry_path(const char *name, u64 hash, char *out, size_t capacity, i32 create) {
    char safe[STORAGE_CACHE_NAME_CAPACITY];
    char dir[STORAGE_PATH_CAPACITY];
    if (!storage_cache_safe_name(name, safe, sizeof(safe))) return 0;
    if (!storage_cache_dir(dir, sizeof(dir), create)) return 0;
    i32 count = snprintf(out, capacity, "%s" STORAGE_CACHE_SEP "%s.%016llx" STORAGE_CACHE_SUFFIX, dir, safe,
                         (unsigned long long)hash);
    if (count < 0 || (size_t)count >= capacity) {
        storage_set_error("cache entry path is too long");
        return 0;
    }
    return 1;
}

// Drop every generation of `name` except `keep`, so a cache entry rebuilt after
// its input changed replaces the old one instead of accumulating beside it.
// `keep` is the bare filename to preserve, or NULL to remove them all.
static i32 storage_cache_sweep(const char *name, const char *keep) {
    char safe[STORAGE_CACHE_NAME_CAPACITY];
    char dir[STORAGE_PATH_CAPACITY];
    char entry[STORAGE_PATH_CAPACITY];
    if (!storage_cache_safe_name(name, safe, sizeof(safe))) return 0;
    if (!storage_cache_dir(dir, sizeof(dir), 0)) return 0;
    size_t prefix = strlen(safe);
    safe[prefix] = '.';
    safe[prefix + 1] = '\0';
    prefix += 1;

#ifdef NS_WIN
    char pattern[STORAGE_PATH_CAPACITY];
    if (snprintf(pattern, sizeof(pattern), "%s" STORAGE_CACHE_SEP "*", dir) < 0) return 0;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 1;
    do {
        const char *file = found.cFileName;
#else
    DIR *search = opendir(dir);
    if (!search) return 1;
    for (struct dirent *item = readdir(search); item; item = readdir(search)) {
        const char *file = item->d_name;
#endif
        if (strncmp(file, safe, prefix) != 0) continue;
        if (keep && strcmp(file, keep) == 0) continue;
        if (snprintf(entry, sizeof(entry), "%s" STORAGE_CACHE_SEP "%s", dir, file) > 0) remove(entry);
#ifdef NS_WIN
    } while (FindNextFileA(search, &found));
    FindClose(search);
#else
    }
    closedir(search);
#endif
    return 1;
}

u64 storage_cache_hash(const u8 *data, i32 size) {
    // FNV-1a 64, the same hash `ns build` stamps its inputs with.
    u64 hash = 14695981039346656037ull;
    if (!data || size < 0) return hash;
    for (i32 i = 0; i < size; ++i) {
        hash ^= (u64)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

u64 storage_cache_hash_str(const char *text) {
    return storage_cache_hash((const u8 *)text, text ? (i32)strlen(text) : 0);
}

const char *storage_cache_path(const char *name, u64 hash) {
    storage_clear_error();
    if (!storage_cache_entry_path(name, hash, storage_cache_result, sizeof(storage_cache_result), 1)) {
        storage_cache_result[0] = '\0';
    }
    return storage_cache_result;
}

i32 storage_cache_has(const char *name, u64 hash) {
    storage_clear_error();
    char path[STORAGE_PATH_CAPACITY];
    if (!storage_cache_entry_path(name, hash, path, sizeof(path), 0)) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

i32 storage_cache_size(const char *name, u64 hash) {
    storage_clear_error();
    char path[STORAGE_PATH_CAPACITY];
    if (!storage_cache_entry_path(name, hash, path, sizeof(path), 0)) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    i32 size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        long end = ftell(file);
        if (end >= 0 && end <= 0x7fffffffL) size = (i32)end;
    }
    fclose(file);
    if (size < 0) storage_set_error("could not measure the cache entry");
    return size;
}

i32 storage_cache_read(const char *name, u64 hash, u8 *data, i32 capacity) {
    storage_clear_error();
    char path[STORAGE_PATH_CAPACITY];
    if (!data || capacity < 0) {
        storage_set_error("cache read needs a destination buffer");
        return -1;
    }
    if (!storage_cache_entry_path(name, hash, path, sizeof(path), 0)) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t read = fread(data, 1, (size_t)capacity, file);
    // A short buffer would silently truncate the entry, which a caller cannot
    // tell from a genuinely small one; report it instead.
    i32 complete = feof(file) || fgetc(file) == EOF;
    fclose(file);
    if (!complete) {
        storage_set_error("cache entry is larger than the destination buffer");
        return -1;
    }
    return (i32)read;
}

i32 storage_cache_write(const char *name, u64 hash, const u8 *data, i32 size) {
    storage_clear_error();
    char path[STORAGE_PATH_CAPACITY];
    char temp[STORAGE_PATH_CAPACITY];
    if (!data || size < 0) {
        storage_set_error("cache write needs data");
        return 0;
    }
    if (!storage_cache_entry_path(name, hash, path, sizeof(path), 1)) return 0;
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) < 0) {
        storage_set_error("cache entry path is too long");
        return 0;
    }
    // Written aside and renamed so a crash mid-write cannot leave a truncated
    // entry that later reads would trust.
    FILE *file = fopen(temp, "wb");
    if (!file) {
        storage_set_errorf("could not write the cache entry %s", path);
        return 0;
    }
    i32 ok = size == 0 || fwrite(data, 1, (size_t)size, file) == (size_t)size;
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        remove(temp);
        storage_set_errorf("could not write the cache entry %s", path);
        return 0;
    }
    remove(path);
    if (rename(temp, path) != 0) {
        remove(temp);
        storage_set_errorf("could not replace the cache entry %s", path);
        return 0;
    }
    storage_cache_retire(name, hash);
    return 1;
}

// Adopt a file the caller wrote itself, which is how APIs that only emit to a
// path enter the cache: a Metal binary archive serializes to a URL, so gpu
// serializes beside the entry and hands the result over here.
i32 storage_cache_adopt(const char *name, u64 hash, const char *path) {
    storage_clear_error();
    char entry[STORAGE_PATH_CAPACITY];
    if (!path || !path[0]) {
        storage_set_error("cache adopt needs a source path");
        return 0;
    }
    if (!storage_cache_entry_path(name, hash, entry, sizeof(entry), 1)) return 0;
    if (strcmp(entry, path) != 0) {
        remove(entry);
        if (rename(path, entry) != 0) {
            remove(path);
            storage_set_errorf("could not move %s into the cache", path);
            return 0;
        }
    }
    return storage_cache_retire(name, hash);
}

// Drop the generations of `name` that `hash` supersedes.
i32 storage_cache_retire(const char *name, u64 hash) {
    char safe[STORAGE_CACHE_NAME_CAPACITY];
    char keep[STORAGE_PATH_CAPACITY];
    if (!storage_cache_safe_name(name, safe, sizeof(safe))) return 0;
    if (snprintf(keep, sizeof(keep), "%s.%016llx" STORAGE_CACHE_SUFFIX, safe, (unsigned long long)hash) < 0) return 0;
    return storage_cache_sweep(name, keep);
}

i32 storage_cache_remove(const char *name) {
    storage_clear_error();
    return storage_cache_sweep(name, NULL);
}

i32 storage_cache_clear(void) {
    storage_clear_error();
    char dir[STORAGE_PATH_CAPACITY];
    char entry[STORAGE_PATH_CAPACITY];
    if (!storage_cache_dir(dir, sizeof(dir), 0)) return 0;

#ifdef NS_WIN
    char pattern[STORAGE_PATH_CAPACITY];
    if (snprintf(pattern, sizeof(pattern), "%s" STORAGE_CACHE_SEP "*", dir) < 0) return 0;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return 1;
    do {
        const char *file = found.cFileName;
#else
    DIR *search = opendir(dir);
    if (!search) return 1;
    for (struct dirent *item = readdir(search); item; item = readdir(search)) {
        const char *file = item->d_name;
#endif
        if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0) continue;
        if (snprintf(entry, sizeof(entry), "%s" STORAGE_CACHE_SEP "%s", dir, file) > 0) remove(entry);
#ifdef NS_WIN
    } while (FindNextFileA(search, &found));
    FindClose(search);
#else
    }
    closedir(search);
#endif
    return 1;
}
