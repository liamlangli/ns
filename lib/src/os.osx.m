#include "os.h"

#include <Cocoa/Cocoa.h>
#include <CoreServices/CoreServices.h>
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

static FSEventStreamRef os_watch_stream = NULL;
static dispatch_queue_t os_watch_queue = NULL;
static _Atomic i32 os_watch_changed = 0;

static void os_watch_callback(ConstFSEventStreamRef stream, void *info, size_t count,
                              void *event_paths, const FSEventStreamEventFlags flags[],
                              const FSEventStreamEventId ids[]) {
    ns_unused(stream);
    ns_unused(info);
    ns_unused(flags);
    ns_unused(ids);
    char **paths = (char**)event_paths;
    for (size_t i = 0; i < count; i++) {
        const char *path = paths[i];
        if (!path) continue;
        if (strstr(path, "/.git/") || strstr(path, "/.ns/chat/")) continue;
        atomic_store(&os_watch_changed, 1);
        break;
    }
}

void os_watch_stop(void) {
    if (os_watch_stream) {
        FSEventStreamStop(os_watch_stream);
        FSEventStreamInvalidate(os_watch_stream);
        FSEventStreamRelease(os_watch_stream);
        os_watch_stream = NULL;
    }
    atomic_store(&os_watch_changed, 0);
}

i32 os_watch_start(const char *path) {
    os_watch_stop();
    if (!path || !path[0]) return 0;
    CFStringRef root = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    if (!root) return 0;
    const void *values[] = {root};
    CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext context = {0, NULL, NULL, NULL, NULL};
    os_watch_stream = FSEventStreamCreate(kCFAllocatorDefault, os_watch_callback, &context, paths,
        kFSEventStreamEventIdSinceNow, 0.05,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    CFRelease(root);
    if (!os_watch_stream) return 0;
    if (!os_watch_queue) os_watch_queue = dispatch_queue_create("ns.workspace.watch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(os_watch_stream, os_watch_queue);
    if (!FSEventStreamStart(os_watch_stream)) {
        os_watch_stop();
        return 0;
    }
    return 1;
}

i32 os_watch_poll(void) {
    return atomic_exchange(&os_watch_changed, 0);
}

#ifdef NS_MACOS
// macOS-specific OS functions
// File system, process management, etc.

#endif // NS_MACOS

const char *os_open_file_dialog(const char *title) {
    static char path[4096];
    path[0] = '\0';

    @autoreleasepool {
        __block NSString *selected = nil;
        void (^show_panel)(void) = ^{
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            if (title && title[0] != '\0') {
                panel.message = [NSString stringWithUTF8String:title];
            }

            if ([panel runModal] == NSModalResponseOK) {
                selected = [[panel URL] path];
                [selected retain];
            }
        };

        if ([NSThread isMainThread]) {
            show_panel();
        } else {
            dispatch_sync(dispatch_get_main_queue(), show_panel);
        }

        if (selected) {
            const char *fs_path = [selected fileSystemRepresentation];
            if (fs_path) {
                strncpy(path, fs_path, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
            [selected release];
        }
    }

    return path;
}

const char *os_save_file_dialog(const char *title, const char *suggested_name) {
    static char path[4096];
    path[0] = '\0';
    @autoreleasepool {
        __block NSString *selected = nil;
        void (^show_panel)(void) = ^{
            NSSavePanel *panel = [NSSavePanel savePanel];
            panel.canCreateDirectories = YES;
            if (title && title[0]) panel.message = [NSString stringWithUTF8String:title];
            if (suggested_name && suggested_name[0]) panel.nameFieldStringValue = [NSString stringWithUTF8String:suggested_name];
            if ([panel runModal] == NSModalResponseOK) selected = [[[panel URL] path] retain];
        };
        if ([NSThread isMainThread]) show_panel();
        else dispatch_sync(dispatch_get_main_queue(), show_panel);
        if (selected) {
            snprintf(path, sizeof(path), "%s", [selected fileSystemRepresentation]);
            [selected release];
        }
    }
    return path;
}

const char *os_open_folder_dialog(const char *title) {
    static char path[4096];
    path[0] = '\0';
    @autoreleasepool {
        __block NSString *selected = nil;
        void (^show_panel)(void) = ^{
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = NO;
            panel.canChooseDirectories = YES;
            panel.canCreateDirectories = YES;
            panel.allowsMultipleSelection = NO;
            if (title && title[0]) panel.message = [NSString stringWithUTF8String:title];
            if ([panel runModal] == NSModalResponseOK) {
                selected = [[[panel URL] path] retain];
            }
        };
        if ([NSThread isMainThread]) show_panel();
        else dispatch_sync(dispatch_get_main_queue(), show_panel);
        if (selected) {
            snprintf(path, sizeof(path), "%s", [selected fileSystemRepresentation]);
            [selected release];
        }
    }
    return path;
}

i32 os_launch_ns_project(const char *folder, const char *entry) {
    if (!folder || !folder[0] || !entry || !entry[0]) return 0;
    @autoreleasepool {
        NSString *executable = [[[NSProcessInfo processInfo] arguments] firstObject];
        if (![executable isAbsolutePath]) {
            executable = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:executable];
        }
        NSTask *task = [[NSTask alloc] init];
        task.launchPath = executable;
        task.arguments = @[@"run", [NSString stringWithUTF8String:entry]];
        task.currentDirectoryPath = [NSString stringWithUTF8String:folder];
        NSMutableDictionary *environment = [[[NSProcessInfo processInfo] environment] mutableCopy];
        environment[@"NSCODE_NATIVE_ENTRY"] = [NSString stringWithUTF8String:entry];
        task.environment = environment;
        [environment release];
        @try {
            [task launch];
        } @catch (NSException *exception) {
            ns_unused(exception);
            [task release];
            return 0;
        }
        [task release];
    }
    return 1;
}
