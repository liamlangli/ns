#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)

#include "os.h"

// Sandboxed Apple mobile apps have no process launcher or synchronous native
// file panels. File access itself is provided by os.c inside the app sandbox;
// these host-oriented services report unavailable without aborting the VM.
void os_watch_stop(void) {}
i32 os_watch_start(const char *path) {
    ns_unused(path);
    return 0;
}
i32 os_watch_poll(void) { return 0; }

const char *os_open_file_dialog(const char *title) {
    ns_unused(title);
    return "";
}

const char *os_save_file_dialog(const char *title, const char *suggested_name) {
    ns_unused(title);
    ns_unused(suggested_name);
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

#endif
