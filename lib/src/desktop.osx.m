#include "desktop.h"
#include "gpu.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreMedia/CoreMedia.h>
#include <unistd.h>

// Services only: no camera, layout, launcher ordering, or rendering policy.
@interface NSDesktopCapture : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, strong) SCStream *stream;
@property(nonatomic) CVPixelBufferRef pixel;
@property(nonatomic) i32 state;
@property(nonatomic) u32 texture;
@property(nonatomic) BOOL stopped;
@property(nonatomic) BOOL paused;
@property(nonatomic) BOOL changing;
- (void)applyPause;
@end
static NSMutableDictionary<NSNumber *, NSDesktopCapture *> *captures;
static NSArray<NSDictionary *> *windows;
static NSArray<NSDictionary *> *apps;
static NSString *last_error = @"";
static i32 refresh_state = 1, generation;
static u64 next_handle = 1;
static BOOL refreshing;
static view *shortcut_view;
static i32 pending_action;
static EventHotKeyRef hotkeys[5];
static EventHandlerRef hotkey_handler;
static dispatch_queue_t capture_queue;
static void desktop_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        captures = [NSMutableDictionary dictionary];
        capture_queue = dispatch_queue_create("ns.desktop.frames", DISPATCH_QUEUE_SERIAL);
    });
}
@implementation NSDesktopCapture
- (void)applyPause {
    if (self.stopped || self.changing) return;
    self.changing = YES;
    BOOL desired = self.paused;
    void (^completion)(NSError *) = ^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.changing = NO;
            if (self.stopped) return;
            if (error) { @synchronized(self) { self.state = -2; } last_error = error.localizedDescription; return; }
            if (self.paused != desired) [self applyPause];
        });
    };
    if (desired) [self.stream stopCaptureWithCompletionHandler:completion];
    else [self.stream startCaptureWithCompletionHandler:completion];
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    (void)stream;
    @synchronized(self) { if (!self.stopped) self.state = -2; }
    dispatch_async(dispatch_get_main_queue(), ^{ last_error = error.localizedDescription ?: @"Window capture stopped"; });
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    if ([info[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    if (!pixel) return;
    @synchronized(self) {
        if (self.stopped) return;
        CVPixelBufferRetain(pixel);
        if (self.pixel) CVPixelBufferRelease(self.pixel);
        self.pixel = pixel;
        self.state = 1;
    }
}
- (void)dealloc { if (_pixel) CVPixelBufferRelease(_pixel); }
@end
@interface NSDesktopDragCard : NSView <NSDraggingSource>
@property(nonatomic, strong) NSURL *appURL;
@property(nonatomic) NSPoint dragOrigin;
@property(nonatomic) BOOL dragging;
@end

@implementation NSDesktopDragCard
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}
- (NSView *)hitTest:(NSPoint)point {
    return [super hitTest:point] ? self : nil;
}
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:[NSCursor openHandCursor]];
}
- (void)mouseDown:(NSEvent *)event {
    self.dragOrigin = [self convertPoint:event.locationInWindow fromView:nil];
    self.dragging = NO;
}
- (void)mouseDragged:(NSEvent *)event {
    if (self.dragging || !self.appURL) return;
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat dx = point.x - self.dragOrigin.x;
    CGFloat dy = point.y - self.dragOrigin.y;
    if (dx * dx + dy * dy < 9.0) return;
    self.dragging = YES;
    NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:self.appURL];
    NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:self.appURL.path];
    icon.size = NSMakeSize(64, 64);
    [item setDraggingFrame:NSMakeRect(point.x - 32.0, point.y - 32.0, 64.0, 64.0) contents:icon];
    [self beginDraggingSessionWithItems:@[item] event:event source:self];
}
- (void)mouseUp:(NSEvent *)event {
    (void)event;
    self.dragging = NO;
}
- (NSDragOperation)draggingSession:(NSDraggingSession *)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    return context == NSDraggingContextOutsideApplication ? NSDragOperationEvery : NSDragOperationNone;
}
- (void)draggingSession:(NSDraggingSession *)session endedAtPoint:(NSPoint)screenPoint operation:(NSDragOperation)operation {
    (void)session;
    (void)screenPoint;
    (void)operation;
    self.dragging = NO;
}
@end

@interface NSDesktopPermissionHelper : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSPanel *panel;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSURL *appURL;
@property(nonatomic, strong) NSURL *settingsURL;
@property(nonatomic) BOOL accessibility;
- (void)copyPath:(id)sender;
- (void)revealInFinder:(id)sender;
- (void)openSettings:(id)sender;
- (void)pollPermission:(NSTimer *)timer;
@end

static NSDesktopPermissionHelper *permission_helper;

@implementation NSDesktopPermissionHelper
- (void)copyPath:(id)sender {
    (void)sender;
    if (self.appURL.path.length) {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:self.appURL.path forType:NSPasteboardTypeString];
    }
}
- (void)revealInFinder:(id)sender {
    (void)sender;
    if (self.appURL) {
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[self.appURL]];
    }
}
- (void)openSettings:(id)sender {
    (void)sender;
    if (self.settingsURL) {
        [[NSWorkspace sharedWorkspace] openURL:self.settingsURL];
    }
}
- (void)pollPermission:(NSTimer *)timer {
    (void)timer;
    BOOL granted = self.accessibility ? AXIsProcessTrusted() : CGPreflightScreenCaptureAccess();
    if (granted) {
        [self.panel close];
    }
}
- (void)windowWillClose:(NSNotification *)note {
    (void)note;
    [self.timer invalidate];
    self.timer = nil;
    if (permission_helper == self) {
        permission_helper = nil;
    }
}
@end

static NSURL *desktop_app_bundle_url(void) {
    NSURL *bundle = NSBundle.mainBundle.bundleURL;
    if ([bundle.pathExtension.lowercaseString isEqualToString:@"app"]) {
        return bundle;
    }
    NSString *exe = NSBundle.mainBundle.executablePath;
    if (exe.length) {
        NSURL *cur = [NSURL fileURLWithPath:exe];
        for (int i = 0; i < 4; ++i) {
            cur = [cur URLByDeletingLastPathComponent];
            if ([cur.pathExtension.lowercaseString isEqualToString:@"app"]) {
                return cur;
            }
        }
        return [NSURL fileURLWithPath:exe];
    }
    return bundle;
}

static void desktop_close_permission_helper(void) {
    NSDesktopPermissionHelper *helper = permission_helper;
    if (!helper) return;
    permission_helper = nil;
    [helper.timer invalidate];
    helper.timer = nil;
    [helper.panel close];
}

static void desktop_show_permission_helper(BOOL accessibility) {
    desktop_close_permission_helper();
    NSURL *appURL = desktop_app_bundle_url();
    if (!appURL) return;
    NSString *paneString = accessibility
        ? @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"
        : @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
    NSURL *settingsURL = [NSURL URLWithString:paneString];

    NSDesktopPermissionHelper *helper = [NSDesktopPermissionHelper new];
    helper.appURL = appURL;
    helper.settingsURL = settingsURL;
    helper.accessibility = accessibility;

    const CGFloat width = 360.0;
    const CGFloat height = 256.0;
    NSPanel *panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered
        defer:NO];
    panel.title = accessibility ? @"NS Vision — Accessibility" : @"NS Vision — Screen Recording";
    panel.floatingPanel = YES;
    panel.hidesOnDeactivate = NO;
    panel.level = NSModalPanelWindowLevel + 1;
    panel.becomesKeyOnlyIfNeeded = YES;
    panel.releasedWhenClosed = NO;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    panel.delegate = helper;

    NSView *content = panel.contentView;
    NSString *appName = appURL.lastPathComponent ?: @"ns_vision.app";
    NSTextField *hint = [NSTextField wrappingLabelWithString:
        [NSString stringWithFormat:@"In System Settings, click '+' then drag %@ below into the file picker and click Open.", appName]];
    hint.frame = NSMakeRect(20, 200, 320, 40);
    hint.font = [NSFont systemFontOfSize:13.0];
    [content addSubview:hint];

    NSDesktopDragCard *card = [[NSDesktopDragCard alloc] initWithFrame:NSMakeRect(20, 100, 320, 88)];
    card.appURL = appURL;
    card.wantsLayer = YES;
    card.layer.cornerRadius = 10.0;
    card.layer.borderWidth = 1.0;
    card.layer.borderColor = [NSColor separatorColor].CGColor;
    card.layer.backgroundColor = [NSColor controlBackgroundColor].CGColor;

    NSImageView *iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(14, 12, 64, 64)];
    iconView.image = [[NSWorkspace sharedWorkspace] iconForFile:appURL.path];
    iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
    [card addSubview:iconView];

    NSTextField *nameLabel = [NSTextField labelWithString:appName];
    nameLabel.frame = NSMakeRect(90, 46, 214, 22);
    nameLabel.font = [NSFont boldSystemFontOfSize:14.0];
    nameLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [card addSubview:nameLabel];

    NSTextField *subLabel = [NSTextField labelWithString:@"Drag into the '+' file picker \u2192"];
    subLabel.frame = NSMakeRect(90, 22, 214, 18);
    subLabel.font = [NSFont systemFontOfSize:12.0];
    subLabel.textColor = [NSColor secondaryLabelColor];
    [card addSubview:subLabel];
    [content addSubview:card];

    NSTextField *pathLabel = [NSTextField labelWithString:appURL.path ?: @""];
    pathLabel.frame = NSMakeRect(20, 64, 320, 24);
    pathLabel.selectable = YES;
    pathLabel.font = [NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightRegular];
    pathLabel.textColor = [NSColor secondaryLabelColor];
    pathLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [content addSubview:pathLabel];

    NSButton *copy = [NSButton buttonWithTitle:@"Copy Path" target:helper action:@selector(copyPath:)];
    copy.frame = NSMakeRect(20, 16, 84, 32);
    copy.font = [NSFont systemFontOfSize:12.0];
    [content addSubview:copy];

    NSButton *reveal = [NSButton buttonWithTitle:@"Reveal in Finder" target:helper action:@selector(revealInFinder:)];
    reveal.frame = NSMakeRect(108, 16, 120, 32);
    reveal.font = [NSFont systemFontOfSize:12.0];
    [content addSubview:reveal];

    NSButton *open = [NSButton buttonWithTitle:@"Open Settings" target:helper action:@selector(openSettings:)];
    open.frame = NSMakeRect(232, 16, 108, 32);
    open.font = [NSFont systemFontOfSize:12.0];
    [content addSubview:open];

    NSScreen *screen = NSScreen.mainScreen;
    if (screen) {
        NSRect visible = screen.visibleFrame;
        CGFloat x = visible.origin.x + 80.0;
        CGFloat y = visible.origin.y + (visible.size.height - height) * 0.5;
        [panel setFrameOrigin:NSMakePoint(x, y)];
    }

    helper.panel = panel;
    helper.timer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:helper selector:@selector(pollPermission:) userInfo:nil repeats:YES];
    permission_helper = helper;

    [panel orderFrontRegardless];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (permission_helper == helper) {
            [helper.panel orderFrontRegardless];
        }
    });
}

ns_bool desktop_supported(void) { if (@available(macOS 13.0, *)) return true; return false; }
ns_bool desktop_permission(void) { return CGPreflightScreenCaptureAccess(); }
// Registers the app with Screen Recording, then opens that Privacy pane so the
// user can switch it on. The system prompt appears only once per app.
ns_bool desktop_request_permission(void) {
    if (CGRequestScreenCaptureAccess()) return true;
    NSURL *pane = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    [[NSWorkspace sharedWorkspace] openURL:pane];
    desktop_show_permission_helper(NO);
    return false;
}
ns_bool desktop_accessibility(ns_bool request) {
    if (AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @(request)})) return true;
    if (request) {
        NSURL *pane = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
        [[NSWorkspace sharedWorkspace] openURL:pane];
        desktop_show_permission_helper(YES);
    }
    return false;
}
// System UI that presents as an application or overlay but is not one of the
// user's windows: System Settings (where NS Vision sends the user for
// permissions) and macOS system chrome never belong on the dome or in the
// background list.
static bool desktop_system_app(NSString *bundle) {
    if (!bundle) return false;
    static NSSet<NSString *> *system_bundles;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        system_bundles = [NSSet setWithObjects:
            @"com.apple.systempreferences",
            @"com.apple.dock",
            @"com.apple.WindowManager",
            @"com.apple.controlcenter",
            @"com.apple.notificationcenterui",
            @"com.apple.SystemUIServer",
            @"com.apple.Spotlight",
            @"com.apple.WallpaperAgent",
            @"com.apple.loginwindow",
            @"com.apple.campo",
            @"com.apple.Siri",
            @"com.apple.TextInputMenuAgent",
            @"com.apple.TextInputSwitcher",
            @"com.apple.UserNotificationCenter",
            @"com.apple.screencaptureui",
            @"com.apple.universalcontrol",
            @"com.apple.CoreLocationAgent",
            nil];
    });
    return [system_bundles containsObject:bundle];
}
static bool desktop_capture_policy(NSApplicationActivationPolicy policy) {
    return policy == NSApplicationActivationPolicyRegular ||
           policy == NSApplicationActivationPolicyAccessory;
}
void desktop_refresh(void) {
    desktop_init();
    if (refreshing) return;
    if (!desktop_supported()) { refresh_state = -1; return; }
    if (!desktop_permission()) { refresh_state = -2; last_error = @"Allow Screen Recording for NS Vision in System Settings, then refresh"; return; }
    refreshing = YES; refresh_state = 0;
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO completionHandler:^(SCShareableContent *content, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            refreshing = NO;
            if (!content) { refresh_state = -2; last_error = error.localizedDescription ?: @"Cannot enumerate desktop windows"; return; }
            NSMutableArray *result = [NSMutableArray array];
            NSMutableSet *visible_pids = [NSMutableSet set];
            for (SCWindow *win in content.windows) {
                SCRunningApplication *owner = win.owningApplication;
                if (!owner || owner.processID == getpid() || win.windowLayer < 0 || win.windowLayer >= kCGDockWindowLevel || win.frame.size.width < 160 || win.frame.size.height < 44) continue;
                NSRunningApplication *running = [NSRunningApplication runningApplicationWithProcessIdentifier:owner.processID];
                if (!running || !desktop_capture_policy(running.activationPolicy) || desktop_system_app(owner.bundleIdentifier)) continue;
                BOOL on_screen = win.isOnScreen;
                BOOL has_title = win.title.length > 0;
                if (!on_screen && !has_title) continue;
                NSString *path = running.bundleURL.path ?: @"";
                NSString *title = has_title ? win.title : owner.applicationName;
                [visible_pids addObject:@(owner.processID)];
                [result addObject:@{@"id": @(win.windowID), @"pid": @(owner.processID), @"width": @((int)win.frame.size.width), @"height": @((int)win.frame.size.height), @"flags": @1, @"title": title ?: @"Window", @"path": path, @"key": [NSString stringWithFormat:@"%@\t%@", owner.bundleIdentifier ?: path, title ?: @""], @"window": win}];
            }
            // Apps with no remaining window can still be activated without
            // launching a second instance (the macOS equivalent of tray apps).
            NSMutableSet *background_pids = [NSMutableSet set];
            NSMutableArray *filtered = [NSMutableArray array];
            for (NSDictionary *row in result) {
                if ([row[@"flags"] intValue] == 1) { [filtered addObject:row]; continue; }
                NSNumber *pid = row[@"pid"];
                if ([visible_pids containsObject:pid] || [background_pids containsObject:pid]) continue;
                [background_pids addObject:pid]; [filtered addObject:row];
            }
            for (NSRunningApplication *running in NSWorkspace.sharedWorkspace.runningApplications) {
                NSNumber *pid = @(running.processIdentifier);
                if (running.processIdentifier == getpid() || running.activationPolicy != NSApplicationActivationPolicyRegular || desktop_system_app(running.bundleIdentifier) || [visible_pids containsObject:pid] || [background_pids containsObject:pid]) continue;
                NSString *path = running.bundleURL.path ?: @"";
                [filtered addObject:@{@"id": @0, @"pid": pid, @"width": @0, @"height": @0, @"flags": @2, @"title": running.localizedName ?: @"Application", @"path": path, @"key": running.bundleIdentifier ?: path}];
            }
            windows = [filtered copy]; generation++; refresh_state = 1; last_error = @"";
        });
    }];
}
i32 desktop_status(void) { return refresh_state; }
i32 desktop_generation(void) { return generation; }
i32 desktop_window_count(void) { return (i32)windows.count; }
static NSDictionary *window_at(i32 index) { return index >= 0 && (NSUInteger)index < windows.count ? windows[index] : @{}; }
desktop_window *desktop_window_at(i32 index) {
    static desktop_window info;
    NSDictionary *w = window_at(index);
    info = (desktop_window){[w[@"id"] unsignedLongLongValue], [w[@"pid"] intValue], [w[@"width"] intValue], [w[@"height"] intValue], [w[@"flags"] intValue]};
    return &info;
}
const char *desktop_window_title(i32 index) { return [window_at(index)[@"title"] UTF8String] ?: ""; }
const char *desktop_window_key(i32 index) { return [window_at(index)[@"key"] UTF8String] ?: ""; }
const char *desktop_window_path(i32 index) { return [window_at(index)[@"path"] UTF8String] ?: ""; }
u64 desktop_capture_open(u64 window, i32 max_width, i32 fps) {
    desktop_init();
    if (!desktop_permission() || captures.count >= 64) return 0;
    SCWindow *target = nil;
    for (NSDictionary *w in windows) if ([w[@"id"] unsignedLongLongValue] == window) { target = w[@"window"]; break; }
    if (!target) return 0;
    NSDesktopCapture *cap = [NSDesktopCapture new];
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
    SCStreamConfiguration *config = [SCStreamConfiguration new];
    max_width = MAX(320, MIN(4096, max_width));
    double ratio = MIN(2.0, (double)max_width / MAX(1.0, target.frame.size.width));
    config.width = MAX(1, (size_t)(target.frame.size.width * ratio));
    config.height = MAX(1, (size_t)(target.frame.size.height * ratio));
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.minimumFrameInterval = CMTimeMake(1, fps == 15 ? 15 : 30);
    config.queueDepth = 3; config.showsCursor = NO; config.capturesAudio = NO;
    if (@available(macOS 14.0, *)) { config.ignoreShadowsSingleWindow = YES; }
    cap.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:cap];
    NSError *error = nil;
    if (![cap.stream addStreamOutput:cap type:SCStreamOutputTypeScreen sampleHandlerQueue:capture_queue error:&error]) { last_error = error.localizedDescription; return 0; }
    u64 handle = next_handle++;
    captures[@(handle)] = cap;
    [cap.stream startCaptureWithCompletionHandler:^(NSError *start_error) {
        if (start_error) {
            @synchronized(cap) { if (!cap.stopped) cap.state = -2; }
            dispatch_async(dispatch_get_main_queue(), ^{ last_error = start_error.localizedDescription; });
        }
    }];
    return handle;
}
i32 desktop_capture_status(u64 handle) { NSDesktopCapture *cap = captures[@(handle)]; if (!cap) return -1; @synchronized(cap) { return cap.state; } }
u32 desktop_capture_texture(u64 handle) {
    NSDesktopCapture *cap = captures[@(handle)];
    if (!cap) return 0;
    CVPixelBufferRef pixel = nil;
    @synchronized(cap) { if (cap.pixel) pixel = CVPixelBufferRetain(cap.pixel); }
    if (!pixel) return 0;
    if (!cap.texture) cap.texture = gpu_texture_create(1, 1, 1, PIXELFORMAT_BGRA8, TEXTURE_USAGE_READ, 1, TEXTURE_2D, "desktop captured window");
    BOOL ok = cap.texture && gpu_metal_import_pixel_buffer(cap.texture, pixel, "desktop captured window");
    CVPixelBufferRelease(pixel);
    return ok ? cap.texture : 0;
}
void desktop_capture_close(u64 handle) {
    NSDesktopCapture *cap = captures[@(handle)];
    if (!cap) return;
    @synchronized(cap) { cap.stopped = YES; cap.state = -1; }
    // Break the stream/output ownership cycle after stop completes.
    SCStream *stream = cap.stream;
    [stream stopCaptureWithCompletionHandler:^(NSError *error) {
        (void)error; NSError *remove_error = nil;
        [stream removeStreamOutput:cap type:SCStreamOutputTypeScreen error:&remove_error];
        cap.stream = nil;
    }];
    if (cap.texture) { gpu_texture_destroy(cap.texture); cap.texture = 0; }
    [captures removeObjectForKey:@(handle)];
}
void desktop_capture_pause(u64 handle, ns_bool paused) {
    NSDesktopCapture *cap = captures[@(handle)];
    if (!cap || cap.stopped || cap.paused == paused) return;
    cap.paused = paused; [cap applyPause];
}
static AXUIElementRef find_ax_window(u64 window, i32 pid) {
    if (!AXIsProcessTrusted()) return NULL;
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    CFArrayRef list = NULL;
    AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&list);
    CFRelease(app);
    if (!list) return NULL;
    // Match the WindowServer id when available; title fallback stays confined
    // to this exact process and fails safely when the window is ambiguous.
    extern AXError _AXUIElementGetWindow(AXUIElementRef, CGWindowID *);
    AXUIElementRef found = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
        AXUIElementRef candidate = (AXUIElementRef)CFArrayGetValueAtIndex(list, i);
        CGWindowID wid = 0;
        if (_AXUIElementGetWindow(candidate, &wid) == kAXErrorSuccess && wid == window) { found = (AXUIElementRef)CFRetain(candidate); break; }
    }
    CFRelease(list); return found;
}
ns_bool desktop_window_activate(u64 window, i32 pid) {
    NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!app || app.terminated) return false;
    AXUIElementRef win = find_ax_window(window, pid);
    if (win) { AXUIElementSetAttributeValue(win, kAXMinimizedAttribute, kCFBooleanFalse); AXUIElementPerformAction(win, kAXRaiseAction); CFRelease(win); }
    [app unhide];
    BOOL activated = [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    if (!win && app.bundleURL) {
        NSWorkspaceOpenConfiguration *config = [NSWorkspaceOpenConfiguration configuration];
        config.activates = YES;
        [NSWorkspace.sharedWorkspace openApplicationAtURL:app.bundleURL configuration:config completionHandler:nil];
        activated = YES;
    }
    return activated;
}
ns_bool desktop_window_close(u64 window, i32 pid) {
    AXUIElementRef win = find_ax_window(window, pid);
    if (!win) { last_error = @"Enable Accessibility for NS Vision to close or raise a specific window"; return false; }
    AXUIElementRef button = NULL;
    AXError error = AXUIElementCopyAttributeValue(win, kAXCloseButtonAttribute, (CFTypeRef *)&button);
    if (error == kAXErrorSuccess && button) { error = AXUIElementPerformAction(button, kAXPressAction); CFRelease(button); }
    CFRelease(win); return error == kAXErrorSuccess;
}
i32 desktop_apps_refresh(void) {
    NSMutableDictionary<NSString *, NSDictionary *> *catalog = [NSMutableDictionary dictionary];
    for (NSString *root in @[@"/Applications", @"/System/Applications", [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"]]) {
        NSDirectoryEnumerator *files = [NSFileManager.defaultManager enumeratorAtURL:[NSURL fileURLWithPath:root] includingPropertiesForKeys:nil options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:nil];
        for (NSURL *url in files) {
            if (![url.pathExtension.lowercaseString isEqualToString:@"app"]) continue;
            [files skipDescendants];
            NSBundle *bundle = [NSBundle bundleWithURL:url];
            NSString *key = bundle.bundleIdentifier ?: url.path;
            if (catalog[key]) continue;
            NSString *name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: [bundle objectForInfoDictionaryKey:@"CFBundleName"] ?: url.lastPathComponent.stringByDeletingPathExtension;
            catalog[key] = @{@"name": name, @"path": url.path};
        }
    }
    apps = [catalog.allValues sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) { return [a[@"name"] localizedCaseInsensitiveCompare:b[@"name"]]; }];
    return (i32)apps.count;
}
const char *desktop_app_name(i32 index) { return index >= 0 && (NSUInteger)index < apps.count ? [apps[index][@"name"] UTF8String] : ""; }
const char *desktop_app_path(i32 index) { return index >= 0 && (NSUInteger)index < apps.count ? [apps[index][@"path"] UTF8String] : ""; }
ns_bool desktop_app_launch(const char *path) {
    NSString *name = path ? [NSString stringWithUTF8String:path] : nil;
    if (!name.length || ![NSFileManager.defaultManager fileExistsAtPath:name]) return false;
    NSWorkspaceOpenConfiguration *config = [NSWorkspaceOpenConfiguration configuration]; config.activates = YES;
    [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:name] configuration:config completionHandler:^(NSRunningApplication *app, NSError *error) { (void)app; if (error) dispatch_async(dispatch_get_main_queue(), ^{ last_error = error.localizedDescription; }); }];
    return true;
}
u32 desktop_app_icon(const char *path, i32 size) {
    if (!path || !*path || size < 16 || size > 512) return 0;
    NSImage *image = [NSWorkspace.sharedWorkspace iconForFile:[NSString stringWithUTF8String:path]];
    if (!image) return 0;
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)size * size * 4];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(data.mutableBytes, size, size, 8, size * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space); if (!context) return 0;
    NSGraphicsContext *previous = NSGraphicsContext.currentContext;
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:context flipped:NO];
    [image drawInRect:NSMakeRect(0, 0, size, size) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0 respectFlipped:YES hints:nil];
    NSGraphicsContext.currentContext = previous; CGContextRelease(context);
    // A bitmap context stores its rows top-down in memory even though its
    // drawing origin is bottom-left, so the bytes already match texture rows.
    u8 *bytes = data.mutableBytes;
    // GPU_BLEND_ALPHA expects straight alpha.
    for (size_t i = 0; i < data.length; i += 4) if (bytes[i + 3]) for (int c = 0; c < 3; ++c) bytes[i + c] = (u8)MIN(255, (int)bytes[i + c] * 255 / bytes[i + 3]);
    u32 texture = gpu_texture_create(size, size, 1, PIXELFORMAT_RGBA8, TEXTURE_USAGE_READ, 1, TEXTURE_2D, "desktop application icon");
    if (texture) gpu_texture_upload(texture, 0, 0, bytes, data.length);
    return texture;
}
const char *desktop_pick_application(void) {
    NSOpenPanel *panel = [NSOpenPanel openPanel]; panel.title = @"Add an application";
    panel.canChooseFiles = YES; panel.canChooseDirectories = NO; panel.treatsFilePackagesAsDirectories = NO;
    panel.directoryURL = [NSURL fileURLWithPath:@"/Applications"];
    if ([panel runModal] != NSModalResponseOK) return "";
    static NSString *selected; selected = panel.URL.path;
    if (![selected.pathExtension.lowercaseString isEqualToString:@"app"]) { last_error = @"Choose an .app application bundle"; return ""; }
    return selected.UTF8String;
}
static OSStatus hotkey_event(EventHandlerCallRef next, EventRef event, void *user) {
    (void)next; (void)user; EventHotKeyID key;
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, NULL, sizeof(key), NULL, &key) != noErr) return eventNotHandledErr;
    pending_action = key.id;
    // MTKView does not draw an ordered-out window. Show it to deliver the
    // pending event; application code owns the actual hide/restore state.
    if (shortcut_view) { desktop_overlay_visible(shortcut_view, true); view_request_frame(shortcut_view, 1); }
    return noErr;
}
static void desktop_shortcuts_clear(void) {
    for (int i = 0; i < 5; ++i) if (hotkeys[i]) { UnregisterEventHotKey(hotkeys[i]); hotkeys[i] = NULL; }
    if (hotkey_handler) { RemoveEventHandler(hotkey_handler); hotkey_handler = NULL; }
    shortcut_view = NULL; pending_action = 0;
}
ns_bool desktop_shortcuts_start(view *v) {
    desktop_shortcuts_clear(); shortcut_view = v;
    EventTypeSpec type = {kEventClassKeyboard, kEventHotKeyPressed};
    if (InstallApplicationEventHandler(hotkey_event, 1, &type, NULL, &hotkey_handler) != noErr) return false;
    UInt32 keys[5] = {kVK_ANSI_W, kVK_ANSI_E, kVK_ANSI_B, kVK_ANSI_Q, kVK_F1};
    BOOL ok = YES;
    for (int i = 0; i < 5; ++i) {
        EventHotKeyID id = {'nsvs', (UInt32)(i == 4 ? 1 : i + 1)};
        if (RegisterEventHotKey(keys[i], optionKey | (i == 4 ? controlKey : 0), id, GetApplicationEventTarget(), 0, &hotkeys[i]) != noErr) ok = NO;
    }
    return ok;
}
void desktop_shortcuts_stop(void) {
    desktop_close_permission_helper();
    desktop_shortcuts_clear();
}
i32 desktop_take_action(void) { i32 result = pending_action; pending_action = 0; return result; }
static bool overlay_fullscreen = false;
// While the fullscreen overlay is showing, the menu bar and Dock step aside so
// the window can cover the whole display; macOS otherwise keeps a titled
// window below the menu bar. Hiding the overlay gives them back.
static void overlay_cover(NSWindow *window) {
    NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    [window setFrame:(window.screen ?: NSScreen.mainScreen).frame display:window.isVisible];
}
void desktop_overlay_visible(view *v, ns_bool visible) {
    if (!v || !v->native_window) return;
    NSWindow *window = (__bridge NSWindow *)v->native_window;
    if (visible) {
        desktop_close_permission_helper();
        [NSApp unhide:nil]; [NSApp activateIgnoringOtherApps:YES]; [window makeKeyAndOrderFront:nil];
        if (overlay_fullscreen) overlay_cover(window);
    } else {
        if (overlay_fullscreen) NSApp.presentationOptions = NSApplicationPresentationDefault;
        [window orderOut:nil];
    }
}
const char *desktop_last_error(void) { return last_error.UTF8String ?: ""; }
void desktop_overlay_fullscreen(view *v, ns_bool fullscreen) {
    if (!v || !v->native_window) return;
    NSWindow *window = (__bridge NSWindow *)v->native_window;
    if (fullscreen) {
        // Borderless: square display corners and no shadow. The view window
        // class keeps keyboard focus without a title bar.
        window.styleMask = NSWindowStyleMaskBorderless;
        window.hasShadow = NO;
        window.backgroundColor = [NSColor colorWithSRGBRed:0.012 green:0.012 blue:0.012 alpha:1.0];
        window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
        window.level = NSMainMenuWindowLevel + 1;
        overlay_fullscreen = true;
        overlay_cover(window);
    }
}
