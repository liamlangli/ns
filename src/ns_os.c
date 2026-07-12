#include "ns_os.h"

#include <limits.h>

// os
ns_str ns_os_exec(ns_str cmd) {
#ifdef NS_XCLIB
    // Embedded Apple runtimes are language-only and cannot spawn host
    // processes. Keep path/file helpers available without importing popen,
    // which is unavailable on iOS and visionOS.
    (void)cmd;
    return ns_str_null;
#else
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
#endif
}

ns_str ns_os_mkdir(ns_str path) {
#ifdef NS_XCLIB
    (void)path;
    return ns_str_null;
#else
    i8 *buffer = (i8 *)ns_malloc(path.len + 8);
    memcpy(buffer, "mkdir -p ", 9);
    memcpy(buffer + 9, path.data, path.len);
    buffer[path.len + 9] = '\0';
    ns_str cmd = ns_str_cstr(buffer);
    ns_str data = ns_os_exec(cmd);
    free(buffer);
    return data;
#endif
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

static ns_bool ns_os_file_seek(FILE *file, i64 offset, i32 origin) {
#ifdef _WIN32
    return _fseeki64(file, offset, origin) == 0;
#else
    return fseeko(file, (off_t)offset, origin) == 0;
#endif
}

static i64 ns_os_file_tell(FILE *file) {
#ifdef _WIN32
    return (i64)_ftelli64(file);
#else
    return (i64)ftello(file);
#endif
}

i64 ns_os_file_size(ns_str path) {
    FILE *file = fopen(path.data, "rb");
    if (!file) return -1;
    if (!ns_os_file_seek(file, 0, SEEK_END)) {
        fclose(file);
        return -1;
    }
    i64 size = ns_os_file_tell(file);
    fclose(file);
    return size;
}

ns_str ns_os_read_file_part(ns_str path, i64 offset, i64 size) {
    if (offset < 0 || size < 0 || size > INT32_MAX) return ns_str_null;

    FILE *file = fopen(path.data, "rb");
    if (!file) return ns_str_null;
    if (!ns_os_file_seek(file, 0, SEEK_END)) {
        fclose(file);
        return ns_str_null;
    }

    i64 file_size = ns_os_file_tell(file);
    if (file_size < 0 || offset > file_size) {
        fclose(file);
        return ns_str_null;
    }
    if (size > file_size - offset) size = file_size - offset;
    if (!ns_os_file_seek(file, offset, SEEK_SET)) {
        fclose(file);
        return ns_str_null;
    }

    char *buffer = (char *)ns_malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return ns_str_null;
    }
    size_t read_size = fread(buffer, 1, (size_t)size, file);
    if (read_size < (size_t)size && ferror(file)) {
        free(buffer);
        fclose(file);
        return ns_str_null;
    }
    fclose(file);
    buffer[read_size] = '\0';
    ns_str data = ns_str_range(buffer, (i32)read_size);
    data.dynamic = true;
    return data;
}

ns_str ns_os_read_file(ns_str path) {
    i64 size = ns_os_file_size(path);
    if (size < 0) return ns_str_null;
    return ns_os_read_file_part(path, 0, size);
}
