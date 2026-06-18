#include "ns_os.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
#endif

// os
ns_str ns_os_exec(ns_str cmd) {
    FILE *pipe = popen(cmd.data, "r");
    if (!pipe) {
        return ns_str_null;
    }
    i8 buffer[128];
    ns_str data = ns_str_null;
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL) {
            data = ns_str_concat(data, ns_str_cstr(buffer));
        }
    }
    pclose(pipe);
    return data;
}

ns_str ns_os_mkdir(ns_str path) {
    // "mkdir -p " (9) + path + NUL
    i8 *buffer = (i8 *)ns_malloc(path.len + 10);
    memcpy(buffer, "mkdir -p ", 9);
    memcpy(buffer + 9, path.data, path.len);
    buffer[path.len + 9] = '\0';
    ns_str cmd = ns_str_cstr(buffer);
    ns_str data = ns_os_exec(cmd);
    free(buffer);
    return data;
}

// path
ns_str ns_path_filename(ns_str src) {
    // filename between path separator and extention separator
    // create new buffer to save filename
    i32 start = 0;
    i32 end = src.len;
    for (i32 i = 0; i < src.len; i++) {
        if (src.data[i] == NS_PATH_SEPARATOR) {
            start = i + 1;
        }

        if (src.data[i] == NS_PATH_FILE_EXT_SEPARATOR) {
            end = i;
            break;
        }
    }

    return ns_str_slice(src, start, end);
}

ns_str ns_path_dirname(ns_str src) {
    // dirname is the path before the last path separator
    // create new buffer to save dirname
    i32 end = src.len;
    for (i32 i = src.len - 1; i >= 0; i--) {
        if (src.data[i] == NS_PATH_SEPARATOR) {
            end = i;
            break;
        }
    }

    return ns_str_slice(src, 0, end);
}

ns_str ns_path_join(ns_str lhs, ns_str rhs) {
    // join two path with path separator
    // create new buffer to save joined path
    char *buffer = (char *)ns_malloc(lhs.len + rhs.len + 2);
    memcpy(buffer, lhs.data, lhs.len);
    buffer[lhs.len] = NS_PATH_SEPARATOR;
    memcpy(buffer + lhs.len + 1, rhs.data, rhs.len);
    buffer[lhs.len + rhs.len + 1] = '\0';
    ns_str data = ns_str_range(buffer, lhs.len + rhs.len + 1);
    data.dynamic = true;
    return data;
}

ns_str ns_path_home() {
    char *home = getenv("HOME");
    if (!home) {
        return ns_str_null;
    }
    ns_str data = ns_str_cstr(home);
    return data;
}

// fs
ns_str ns_fs_read_file(ns_str path) {
    FILE *file = fopen(path.data, "rb");
    if (!file) {
        return ns_str_null;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = (char *)ns_malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    ns_str data = ns_str_range(buffer, size);
    data.dynamic = true;
    return data;
}

ns_bool ns_fs_exists(ns_str path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path.data);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.data, &st) == 0;
#endif
}

ns_bool ns_fs_is_dir(ns_str path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path.data);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path.data, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

static ns_bool ns_str_ends_with(ns_str s, ns_str suffix) {
    return s.len >= suffix.len && strncmp(s.data + s.len - suffix.len, suffix.data, suffix.len) == 0;
}

// copy `s` into a fresh NUL-terminated dynamic ns_str
static ns_str ns_str_dup(ns_str s) {
    i8 *buf = (i8 *)ns_malloc(s.len + 1);
    memcpy(buf, s.data, s.len);
    buf[s.len] = '\0';
    ns_str out = ns_str_range(buf, s.len);
    out.dynamic = true;
    return out;
}

static void ns_fs_collect(ns_str dir, ns_str ext, ns_str **list) {
#ifdef _WIN32
    ns_str pattern = ns_path_join(dir, ns_str_cstr("*"));
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.data, &fd);
    ns_str_free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const i8 *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        ns_str child = ns_path_join(dir, ns_str_cstr((char *)name));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ns_fs_collect(child, ext, list);
            ns_str_free(child);
        } else if (ns_str_ends_with(child, ext)) {
            ns_array_push(*list, child); // ownership transferred to list
        } else {
            ns_str_free(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir.data);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        ns_str child = ns_path_join(dir, ns_str_cstr(e->d_name));
        if (ns_fs_is_dir(child)) {
            ns_fs_collect(child, ext, list);
            ns_str_free(child);
        } else if (ns_str_ends_with(child, ext)) {
            ns_array_push(*list, child); // ownership transferred to list
        } else {
            ns_str_free(child);
        }
    }
    closedir(d);
#endif
}

ns_str *ns_fs_list_ext(ns_str dir, ns_str ext) {
    ns_str *list = ns_null;
    if (dir.len == 0) dir = ns_str_cstr(".");
    // normalize a duplicate so we own a NUL-terminated buffer for opendir
    ns_str root = ns_str_dup(dir);
    ns_fs_collect(root, ext, &list);
    ns_str_free(root);

    // sort lexicographically for deterministic link order
    i32 n = (i32)ns_array_length(list);
    for (i32 i = 1; i < n; i++) {
        ns_str key = list[i];
        i32 j = i - 1;
        while (j >= 0 && strcmp(list[j].data, key.data) > 0) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
    return list;
}

void ns_fs_list_free(ns_str *list) {
    for (i32 i = 0, l = (i32)ns_array_length(list); i < l; i++) {
        ns_str_free(list[i]);
    }
    ns_array_free(list);
}