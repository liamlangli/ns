#include "ns_build_cache.h"
#include "ns_os.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define NS_BUILD_CACHE_HEADER "ns-build-cache/v2"
#define NS_BUILD_CACHE_DIR ".ns-build"
#define NS_BUILD_CACHE_SUFFIX ".cache"

#define NS_BUILD_HASH_BASIS 1469598103934665603ull
#define NS_BUILD_HASH_PRIME 1099511628211ull

static u64 ns_build_hash(const i8 *data, i32 len) {
    u64 hash = NS_BUILD_HASH_BASIS;
    for (i32 i = 0; i < len; i++) {
        hash ^= (u64)(u8)data[i];
        hash *= NS_BUILD_HASH_PRIME;
    }
    return hash;
}

static ns_str ns_build_copy(ns_str s) {
    return ns_str_concat(s, ns_str_cstr(""));
}

static ns_str ns_build_dirname(ns_str path) {
    for (i32 i = path.len - 1; i >= 0; i--) {
        if (path.data[i] == '/' || path.data[i] == '\\') {
            return i == 0 ? ns_build_copy(ns_str_cstr("/")) : ns_str_slice(path, 0, i);
        }
    }
    return ns_build_copy(ns_str_cstr("."));
}

static ns_str ns_build_basename(ns_str path) {
    i32 start = 0;
    for (i32 i = 0; i < path.len; i++) {
        if (path.data[i] == '/' || path.data[i] == '\\') start = i + 1;
    }
    return ns_str_slice(path, start, path.len);
}

// A file or a directory is present at `path`. App bundles and Wasm bundles are
// directories, so the artifact check cannot assume a regular file.
static ns_bool ns_build_path_exists(ns_str path) {
    if (path.data == ns_null || path.len == 0) return false;
#if defined(_WIN32)
    return GetFileAttributesA(path.data) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.data, &st) == 0;
#endif
}

// Last modify time and size of a regular file. The time is split into whole
// seconds and the nanoseconds within that second so it stays exact where i64
// is a 32-bit long, and so two edits inside the same second are still told
// apart on a filesystem with sub-second timestamps.
static ns_bool ns_build_file_status(ns_str path, i64 *seconds, i32 *nanoseconds, i64 *size) {
    if (path.data == ns_null || path.len == 0) return false;
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path.data, GetFileExInfoStandard, &info)) return false;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    unsigned long long ticks = ((unsigned long long)info.ftLastWriteTime.dwHighDateTime << 32) |
                               (unsigned long long)info.ftLastWriteTime.dwLowDateTime;
    // FILETIME counts 100ns intervals since 1601-01-01; shift it to the epoch.
    *seconds = (i64)(ticks / 10000000ull) - 11644473600ll;
    *nanoseconds = (i32)((ticks % 10000000ull) * 100ull);
    *size = (i64)(((unsigned long long)info.nFileSizeHigh << 32) | (unsigned long long)info.nFileSizeLow);
    return true;
#else
    struct stat st;
    if (stat(path.data, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    *seconds = (i64)st.st_mtime;
#if defined(__APPLE__)
    *nanoseconds = (i32)st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    *nanoseconds = (i32)st.st_mtim.tv_nsec;
#else
    *nanoseconds = 0;
#endif
    *size = (i64)st.st_size;
    return true;
#endif
}

// Stamp `path`. A recorded stamp with the same modify time and size supplies
// the content hash, so an untouched file is never read again; anything else is
// hashed, which keeps a touched but unmodified file from forcing a rebuild.
static ns_build_stamp ns_build_stamp_file(ns_str path, const ns_build_stamp *recorded) {
    ns_build_stamp stamp = {.path = ns_build_copy(path), .mtime = 0, .mtime_nsec = 0, .size = -1, .hash = 0};
    i64 seconds = 0, size = -1;
    i32 nanoseconds = 0;
    if (!ns_build_file_status(path, &seconds, &nanoseconds, &size)) return stamp;

    stamp.mtime = seconds;
    stamp.mtime_nsec = nanoseconds;
    stamp.size = size;
    if (recorded != ns_null && recorded->size == size && recorded->mtime == seconds &&
        recorded->mtime_nsec == nanoseconds) {
        stamp.hash = recorded->hash;
        return stamp;
    }

    ns_str data = ns_os_read_file(path);
    if (data.data == ns_null) {
        stamp.mtime = 0;
        stamp.mtime_nsec = 0;
        stamp.size = -1;
        return stamp;
    }
    stamp.hash = ns_build_hash(data.data, data.len);
    ns_str_free(data);
    return stamp;
}

static ns_build_stamp *ns_build_stamp_find(ns_build_stamp *stamps, ns_str path) {
    for (i32 i = 0, l = (i32)ns_array_length(stamps); i < l; i++) {
        if (ns_str_equals(stamps[i].path, path)) return &stamps[i];
    }
    return ns_null;
}

static void ns_build_stamps_free(ns_build_stamp *stamps) {
    for (i32 i = 0, l = (i32)ns_array_length(stamps); i < l; i++) {
        ns_str_free(stamps[i].path);
    }
    ns_array_free(stamps);
}

// Consume the decimal integer at `*cursor`, or return false when the field is
// missing or malformed. Every numeric field of a stamp line is non-negative
// except a missing file's size, which is written as -1.
static ns_bool ns_build_parse_i64(ns_str line, i32 *cursor, i64 *out) {
    i32 i = *cursor;
    while (i < line.len && line.data[i] == ' ') i++;
    ns_bool negative = i < line.len && line.data[i] == '-';
    if (negative) i++;
    i32 start = i;
    u64 value = 0;
    while (i < line.len && line.data[i] >= '0' && line.data[i] <= '9') {
        value = value * 10 + (u64)(line.data[i] - '0');
        i++;
    }
    if (i == start) return false;
    *out = negative ? -(i64)value : (i64)value;
    *cursor = i;
    return true;
}

static ns_bool ns_build_parse_u64(ns_str line, i32 *cursor, u64 *out) {
    i32 i = *cursor;
    while (i < line.len && line.data[i] == ' ') i++;
    i32 start = i;
    u64 value = 0;
    while (i < line.len && line.data[i] >= '0' && line.data[i] <= '9') {
        value = value * 10 + (u64)(line.data[i] - '0');
        i++;
    }
    if (i == start) return false;
    *out = value;
    *cursor = i;
    return true;
}

static ns_bool ns_build_line_starts_with(ns_str line, const char *prefix, i32 *cursor) {
    i32 len = (i32)strlen(prefix);
    if (line.len < len || strncmp(line.data, prefix, (size_t)len) != 0) return false;
    *cursor = len;
    return true;
}

// Read the stamps of the previous build. A cache file that is missing, written
// by another version, or truncated simply leaves the build without a record.
//
// One `stamp` line records a compiled input and one `package` line records a
// packaged one; both carry the same four numeric fields and path.
static ns_bool ns_build_parse_stamp(ns_str line, i32 cursor, ns_build_stamp **stamps) {
    ns_build_stamp stamp = {0};
    i64 nanoseconds = 0;
    ns_bool valid = ns_build_parse_i64(line, &cursor, &stamp.mtime) &&
                    ns_build_parse_i64(line, &cursor, &nanoseconds) &&
                    ns_build_parse_i64(line, &cursor, &stamp.size) &&
                    ns_build_parse_u64(line, &cursor, &stamp.hash) &&
                    cursor + 1 < line.len && line.data[cursor] == ' ';
    if (!valid) return false;
    stamp.mtime_nsec = (i32)nanoseconds;
    stamp.path = ns_str_slice(line, cursor + 1, line.len);
    ns_array_push(*stamps, stamp);
    return true;
}

static void ns_build_cache_read(ns_build_cache *cache) {
    ns_str text = ns_os_read_file(cache->path);
    if (text.data == ns_null) return;

    ns_build_stamp *recorded = ns_null;
    ns_build_stamp *recorded_package = ns_null;
    ns_str config = ns_str_null;
    ns_bool header = false;
    ns_bool valid = true;
    for (i32 i = 0; i < text.len && valid;) {
        i32 start = i;
        while (i < text.len && text.data[i] != '\n') i++;
        i32 end = i;
        if (end > start && text.data[end - 1] == '\r') end--;
        if (i < text.len) i++;

        ns_str line = ns_str_slice(text, start, end);
        i32 cursor = 0;
        if (!header) {
            header = ns_str_equals(line, ns_str_cstr(NS_BUILD_CACHE_HEADER));
            valid = header;
        } else if (line.len == 0) {
            // tolerate a trailing newline
        } else if (ns_build_line_starts_with(line, "config ", &cursor)) {
            ns_str_free(config);
            config = ns_str_slice(line, cursor, line.len);
        } else if (ns_build_line_starts_with(line, "artifact ", &cursor)) {
            // recorded for readability; the live artifact path is authoritative
        } else if (ns_build_line_starts_with(line, "stamp ", &cursor)) {
            valid = ns_build_parse_stamp(line, cursor, &recorded);
        } else if (ns_build_line_starts_with(line, "package ", &cursor)) {
            valid = ns_build_parse_stamp(line, cursor, &recorded_package);
        } else {
            valid = false;
        }
        ns_str_free(line);
    }
    ns_str_free(text);

    if (!valid || !header) {
        ns_build_stamps_free(recorded);
        ns_build_stamps_free(recorded_package);
        ns_str_free(config);
        return;
    }
    cache->recorded = recorded;
    cache->recorded_package_inputs = recorded_package;
    cache->recorded_config = config;
    cache->loaded = true;
}

ns_str ns_build_cache_path(ns_str artifact) {
    ns_str dir = ns_build_dirname(artifact);
    ns_str name = ns_build_basename(artifact);
    ns_str cache_dir = ns_path_join(dir, ns_str_cstr(NS_BUILD_CACHE_DIR));
    ns_str file = ns_str_concat(name, ns_str_cstr(NS_BUILD_CACHE_SUFFIX));
    ns_str path = ns_path_join(cache_dir, file);
    ns_str_free(dir);
    ns_str_free(name);
    ns_str_free(cache_dir);
    ns_str_free(file);
    return path;
}

void ns_build_cache_open(ns_build_cache *cache, ns_str artifact, ns_str config) {
    memset(cache, 0, sizeof(ns_build_cache));
    cache->artifact = ns_build_copy(artifact);
    cache->config = ns_build_copy(config);
    cache->path = ns_build_cache_path(artifact);
    ns_build_cache_read(cache);
}

// Stamp `path` into one input group unless it is already there. A recorded
// stamp with the same modify time and size supplies the content hash, so an
// input this build did not collect again is not read a second time.
static void ns_build_cache_add_to(ns_build_stamp **group, ns_build_stamp *recorded, ns_str path) {
    if (path.data == ns_null || path.len == 0) return;
    if (ns_build_stamp_find(*group, path) != ns_null) return;
    ns_build_stamp stamp = ns_build_stamp_file(path, ns_build_stamp_find(recorded, path));
    ns_array_push(*group, stamp);
}

void ns_build_cache_add(ns_build_cache *cache, ns_str path) {
    ns_build_cache_add_to(&cache->inputs, cache->recorded, path);
}

void ns_build_cache_add_package(ns_build_cache *cache, ns_str path) {
    ns_build_cache_add_to(&cache->package_inputs, cache->recorded_package_inputs, path);
}

static void ns_build_cache_add_tree_into(ns_build_cache *cache, ns_str dir, ns_bool package) {
    if (dir.data == ns_null || dir.len == 0) return;
#if defined(_WIN32)
    ns_str pattern = ns_path_join(dir, ns_str_cstr("*"));
    WIN32_FIND_DATAA fd;
    HANDLE handle = FindFirstFileA(pattern.data, &fd);
    ns_str_free(pattern);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        ns_str child = ns_path_join(dir, ns_str_cstr((char *)name));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ns_build_cache_add_tree_into(cache, child, package);
        } else if (package) {
            ns_build_cache_add_package(cache, child);
        } else {
            ns_build_cache_add(cache, child);
        }
        ns_str_free(child);
    } while (FindNextFileA(handle, &fd));
    FindClose(handle);
#else
    DIR *handle = opendir(dir.data);
    if (!handle) return;
    struct dirent *entry;
    while ((entry = readdir(handle)) != ns_null) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        ns_str child = ns_path_join(dir, ns_str_cstr(entry->d_name));
        struct stat st;
        if (lstat(child.data, &st) == 0 && S_ISDIR(st.st_mode)) {
            ns_build_cache_add_tree_into(cache, child, package);
        } else if (lstat(child.data, &st) == 0 && S_ISREG(st.st_mode)) {
            if (package) ns_build_cache_add_package(cache, child);
            else ns_build_cache_add(cache, child);
        }
        ns_str_free(child);
    }
    closedir(handle);
#endif
}

void ns_build_cache_add_tree(ns_build_cache *cache, ns_str dir) {
    ns_build_cache_add_tree_into(cache, dir, false);
}

void ns_build_cache_add_package_tree(ns_build_cache *cache, ns_str dir) {
    ns_build_cache_add_tree_into(cache, dir, true);
}

// True when one input group differs from what the recorded build stamped.
// Recorded inputs the current build did not collect are stamped here: a file
// the linker discovered while linking, an installed module, or the middleware
// a bundle copies in. An unchanged one joins `collected` so the cache this
// build writes keeps guarding it; an edited or deleted one forces the caller
// to redo the work that produced the group.
static ns_bool ns_build_cache_group_changed(ns_build_stamp **collected, ns_build_stamp *recorded) {
    // Every input of this build must be recorded with the same content: a
    // source added to the project is not in the record and forces a rebuild.
    for (i32 i = 0, l = (i32)ns_array_length(*collected); i < l; i++) {
        ns_build_stamp *input = &(*collected)[i];
        ns_build_stamp *was = ns_build_stamp_find(recorded, input->path);
        if (was == ns_null || was->hash != input->hash || was->size != input->size) return true;
    }

    ns_bool changed = false;
    for (i32 i = 0, l = (i32)ns_array_length(recorded); i < l; i++) {
        ns_build_stamp *was = &recorded[i];
        if (ns_build_stamp_find(*collected, was->path) != ns_null) continue;
        ns_build_stamp current = ns_build_stamp_file(was->path, was);
        if (current.hash == was->hash && current.size == was->size) {
            ns_array_push(*collected, current);
        } else {
            ns_str_free(current.path);
            changed = true;
        }
    }
    return changed;
}

ns_build_cache_state ns_build_cache_state_of(ns_build_cache *cache) {
    ns_build_cache_state state = {.compile = true, .package = true};
    if (!cache->loaded) return state;
    if (!ns_str_equals(cache->recorded_config, cache->config)) return state;
    if (!ns_build_path_exists(cache->artifact)) return state;

    state.compile = ns_build_cache_group_changed(&cache->inputs, cache->recorded);
    state.package = state.compile
                        ? true
                        : ns_build_cache_group_changed(&cache->package_inputs,
                                                       cache->recorded_package_inputs);
    return state;
}

ns_bool ns_build_cache_fresh(ns_build_cache *cache) {
    ns_build_cache_state state = ns_build_cache_state_of(cache);
    return !state.compile && !state.package;
}

static void ns_build_cache_ensure_dir(ns_str path) {
    ns_str dir = ns_build_dirname(path);
#if defined(_WIN32)
    CreateDirectoryA(dir.data, NULL);
#else
    mkdir(dir.data, 0755);
#endif
    ns_str_free(dir);
}

ns_bool ns_build_cache_write(ns_build_cache *cache) {
    ns_build_cache_ensure_dir(cache->path);
    FILE *file = fopen(cache->path.data, "wb");
    if (!file) return false;

    fprintf(file, "%s\n", NS_BUILD_CACHE_HEADER);
    fprintf(file, "config %.*s\n", cache->config.len, cache->config.data);
    fprintf(file, "artifact %.*s\n", cache->artifact.len, cache->artifact.data);
    for (i32 i = 0, l = (i32)ns_array_length(cache->inputs); i < l; i++) {
        ns_build_stamp *stamp = &cache->inputs[i];
        fprintf(file, "stamp %lld %lld %lld %llu %.*s\n", (long long)stamp->mtime,
                (long long)stamp->mtime_nsec, (long long)stamp->size, (unsigned long long)stamp->hash,
                stamp->path.len, stamp->path.data);
    }
    for (i32 i = 0, l = (i32)ns_array_length(cache->package_inputs); i < l; i++) {
        ns_build_stamp *stamp = &cache->package_inputs[i];
        fprintf(file, "package %lld %lld %lld %llu %.*s\n", (long long)stamp->mtime,
                (long long)stamp->mtime_nsec, (long long)stamp->size, (unsigned long long)stamp->hash,
                stamp->path.len, stamp->path.data);
    }
    return fclose(file) == 0;
}

void ns_build_cache_free(ns_build_cache *cache) {
    ns_build_stamps_free(cache->inputs);
    ns_build_stamps_free(cache->recorded);
    ns_build_stamps_free(cache->package_inputs);
    ns_build_stamps_free(cache->recorded_package_inputs);
    ns_str_free(cache->path);
    ns_str_free(cache->artifact);
    ns_str_free(cache->config);
    ns_str_free(cache->recorded_config);
    memset(cache, 0, sizeof(ns_build_cache));
}
