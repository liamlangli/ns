#include "os.h"

#include <Cocoa/Cocoa.h>
#include <TargetConditionals.h>

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
