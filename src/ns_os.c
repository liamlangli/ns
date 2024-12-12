#include "ns_os.h"

// os
ns_str ns_os_exec(ns_str cmd) {
    FILE *pipe = popen(cmd.data, "r");
    if (!pipe) {
        return ns_str_null;
    }
    char buffer[128];
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
    char *buffer = (char *)malloc(path.len + 8);
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
    char *buffer = (char *)malloc(lhs.len + rhs.len + 2);
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
    char *buffer = (char *)malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    ns_str data = ns_str_range(buffer, size);
    data.dynamic = true;
    return data;
}