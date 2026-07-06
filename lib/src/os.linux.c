#include "os.h"

#ifdef NS_LINUX
// Linux-specific OS functions
// File system, process management, etc.

#endif // NS_LINUX

const char *os_open_file_dialog(const char *title) {
    ns_unused(title);
    return "";
}
