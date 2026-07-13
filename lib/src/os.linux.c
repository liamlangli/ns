#include "os.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

static int os_watch_fd = -1;

void os_watch_stop(void) {
    if (os_watch_fd >= 0) close(os_watch_fd);
    os_watch_fd = -1;
}

static void os_watch_add_tree(const char *path) {
    if (!path || os_watch_fd < 0) return;
    inotify_add_watch(os_watch_fd, path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE);
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 || strcmp(item->d_name, ".git") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
        struct stat info;
        if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode)) os_watch_add_tree(child);
    }
    closedir(dir);
}

i32 os_watch_start(const char *path) {
    os_watch_stop();
    os_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (os_watch_fd < 0) return 0;
    os_watch_add_tree(path);
    return 1;
}

i32 os_watch_poll(void) {
    if (os_watch_fd < 0) return 0;
    char events[16384];
    ssize_t count = read(os_watch_fd, events, sizeof(events));
    return count > 0;
}

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
