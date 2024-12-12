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

// fs
ns_str ns_fs_read_file(ns_str path);