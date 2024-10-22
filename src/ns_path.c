#include "ns_path.h"

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