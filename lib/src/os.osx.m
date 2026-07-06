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
