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


#if TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_VISION
#import <UIKit/UIKit.h>
i32 os_share_file(const char *path) {
    if (!path || !path[0]) return 0;
    NSString *name = [NSString stringWithUTF8String:path];
    if (!name || ![[NSFileManager defaultManager] fileExistsAtPath:name]) return 0;
    NSURL *url = [NSURL fileURLWithPath:name];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *window = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (scene.activationState != UISceneActivationStateForegroundActive || ![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow *candidate in ((UIWindowScene *)scene).windows) {
                if (candidate.isKeyWindow) { window = candidate; break; }
            }
        }
        UIViewController *host = window.rootViewController;
        while (host.presentedViewController) host = host.presentedViewController;
        if (!host) return;
        UIActivityViewController *sheet = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
        // iPad requires an explicit popover anchor.
        sheet.popoverPresentationController.sourceView = host.view;
        sheet.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(host.view.bounds), CGRectGetMidY(host.view.bounds), 1, 1);
        [host presentViewController:sheet animated:YES completion:nil];
#if !__has_feature(objc_arc)
        [sheet release];
#endif
    });
    return 1;
}
#else
i32 os_share_file(const char *path) { ns_unused(path); return 0; }
#endif

#endif
