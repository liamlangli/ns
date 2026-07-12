#pragma once

#include "ns_type.h"

#ifdef _WIN32
#define NS_PATH_SEPARATOR '\\'
#else
#define NS_PATH_SEPARATOR '/'
#endif

#define NS_PATH_FILE_EXT_SEPARATOR '.'

// os
ns_str ns_os_exec(ns_str cmd);
ns_str ns_os_mkdir(ns_str path);

// path
ns_str ns_path_filename(ns_str src);
ns_str ns_path_dirname(ns_str src);
ns_str ns_path_join(ns_str lhs, ns_str rhs);
ns_str ns_path_home();

// Return the file size in bytes, or -1 when the file cannot be inspected.
i64 ns_os_file_size(ns_str path);
// Read up to `size` bytes starting at `offset`. Reads are truncated at EOF;
// invalid ranges and I/O failures return ns_str_null.
ns_str ns_os_read_file_part(ns_str path, i64 offset, i64 size);
// Read the complete file.
ns_str ns_os_read_file(ns_str path);
