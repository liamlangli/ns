#include "os.h"

#ifdef NS_LINUX
// Linux-specific OS functions
// File system, process management, etc.

#endif // NS_LINUX

const char *os_open_file_dialog(const char *title) {
    ns_unused(title);
    return "";
}

const char *os_open_folder_dialog(const char *title) {
    ns_unused(title);
    return "";
}

i32 os_launch_ns_project(const char *folder, const char *entry) {
    ns_unused(folder);
    ns_unused(entry);
    return 0;
}
