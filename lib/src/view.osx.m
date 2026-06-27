#include <TargetConditionals.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <dispatch/semaphore.h>

#include "view.h"

void gpu_mtl_begin_frame(MTKView *view);

@interface ViewApp : NSApplication
@end

@interface ViewAppDelegate : NSObject<NSApplicationDelegate>
@end

@interface ViewWindowDelegate : NSObject<NSWindowDelegate>
@end

@interface ViewDelegate : NSObject<MTKViewDelegate>
@end

@interface ViewMTKView : MTKView
@end

typedef void(*view_on_launch)(view*);
typedef void(*view_on_frame)(view*);
typedef void(*view_on_terminate)(view*);

static i32 view_width;
static i32 view_height;
static const char* view_title;
static NSWindow* view_window;
static id view_window_delegate;
static id<MTLDevice> view_mtl_device;
static id view_mtk_view_delegate;
static MTKView* view_mtk_view;
static ns_bool view_no_title;

static view _view;

static void view_osx_update_mouse(NSEvent *event);

static f64 view_osx_current_display_ratio(void) {
    CGFloat ratio = 0.0;
    if (view_window) {
        ratio = [view_window backingScaleFactor];
        if (ratio <= 0.0 && [view_window screen]) {
            ratio = [[view_window screen] backingScaleFactor];
        }
    }
    if (ratio <= 0.0 && [NSScreen mainScreen]) {
        ratio = [[NSScreen mainScreen] backingScaleFactor];
    }
    return ratio > 0.0 ? (f64)ratio : 1.0;
}

static void view_osx_sync_metrics(f64 width, f64 height) {
    const f64 ratio = view_osx_current_display_ratio();
    _view.display_ratio = ratio;
    _view.ui_scale = ratio;
    _view.width = (i32)width;
    _view.height = (i32)height;
    _view.framebuffer_width = (i32)(width * ratio + 0.5);
    _view.framebuffer_height = (i32)(height * ratio + 0.5);
}

static void view_osx_apply_manifest_icon(void) {
    const char *icon_path = getenv("NS_APP_ICON");
    if (!icon_path || icon_path[0] == '\0') return;

    NSString *path = [NSString stringWithUTF8String:icon_path];
    NSImage *image = [[NSImage alloc] initWithContentsOfFile:path];
    if (image) {
        [NSApp setApplicationIconImage:image];
    }
}

static ns_bool view_osx_no_title_button(NSPoint p, f64 content_height, i32 *button) {
    if (!view_no_title) return false;

    const f64 y = content_height - p.y;
    if (y < 0.0 || y > 38.0) return false;

    const f64 centers[] = {18.0, 40.0, 62.0};
    const f64 cy = 19.0;
    for (i32 i = 0; i < 3; i++) {
        const f64 dx = p.x - centers[i];
        const f64 dy = y - cy;
        if (dx * dx + dy * dy <= 100.0) {
            *button = i;
            return true;
        }
    }
    return false;
}

static void view_osx_drag_no_title_window(NSEvent *event) {
    NSWindow *window = [event window];
    NSPoint start_mouse = [NSEvent mouseLocation];
    NSRect start_frame = [window frame];

    for (;;) {
        NSEvent *drag_event = [NSApp nextEventMatchingMask:(NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp)
                                                untilDate:[NSDate distantFuture]
                                                   inMode:NSEventTrackingRunLoopMode
                                                  dequeue:YES];
        if (!drag_event) return;
        if ([drag_event type] == NSEventTypeLeftMouseUp) return;
        if ([drag_event type] != NSEventTypeLeftMouseDragged) continue;

        NSPoint mouse = [NSEvent mouseLocation];
        NSPoint origin = NSMakePoint(start_frame.origin.x + mouse.x - start_mouse.x,
                                     start_frame.origin.y + mouse.y - start_mouse.y);
        [window setFrameOrigin:origin];
        view_osx_update_mouse(drag_event);
    }
}

static ns_bool view_osx_handle_no_title_mouse_down(NSEvent *event) {
    if (!view_no_title) return false;

    const NSPoint p = [event locationInWindow];
    const NSRect content_rect = [[event window] contentRectForFrameRect:[[event window] frame]];
    const f64 y = content_rect.size.height - p.y;
    if (y < 0.0 || y > 38.0) return false;

    i32 button = -1;
    if (view_osx_no_title_button(p, content_rect.size.height, &button)) {
        if (button == 0) {
            [[event window] performClose:nil];
        } else if (button == 1) {
            [[event window] miniaturize:nil];
        } else {
            [[event window] toggleFullScreen:nil];
        }
        return true;
    }

    view_osx_drag_no_title_window(event);
    return true;
}

// Key mapping function
i32 view_osx_key_map(i32 k) {
    switch (k) {
        case 51: return VIEW_KEY_BACKSPACE;
        case 53: return VIEW_KEY_ESCAPE;
        case 123: return VIEW_KEY_LEFT;
        case 124: return VIEW_KEY_RIGHT;
        case 125: return VIEW_KEY_DOWN;
        case 126: return VIEW_KEY_UP;
        case 36: return VIEW_KEY_ENTER;
        case 48: return VIEW_KEY_TAB;
        case 49: return VIEW_KEY_SPACE;
        case 117: return VIEW_KEY_DELETE;
        case 115: return VIEW_KEY_HOME;
        case 119: return VIEW_KEY_END;
        case 116: return VIEW_KEY_PAGE_UP;
        case 121: return VIEW_KEY_PAGE_DOWN;
        case 122: return VIEW_KEY_F1;
        case 120: return VIEW_KEY_F2;
        case 99: return VIEW_KEY_F3;
        case 118: return VIEW_KEY_F4;
        case 96: return VIEW_KEY_F5;
        case 97: return VIEW_KEY_F6;
        case 98: return VIEW_KEY_F7;
        case 100: return VIEW_KEY_F8;
        case 101: return VIEW_KEY_F9;
        case 109: return VIEW_KEY_F10;
        case 103: return VIEW_KEY_F11;
        case 111: return VIEW_KEY_F12;
        default:
            return -1;
    }
}

//------------------------------------------------------------------------------
@implementation ViewApp
- (void)sendEvent:(NSEvent*) event {
    if ([event type] == NSEventTypeKeyUp && ([event modifierFlags] & NSEventModifierFlagCommand)) {
        [[self keyWindow] sendEvent:event];
    }
    else {
        [super sendEvent:event];
    }
}
@end

//------------------------------------------------------------------------------
@implementation ViewAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)aNotification {
    (void)aNotification;
    // The window is created in view_create(); the run loop only needs to fire
    // the ns on_launch callback (set between view_create and view_run).
    view_on_launch launch = (view_on_launch)_view.on_launch;
    if (launch) {
        launch(&_view);
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    view_on_terminate terminate = (view_on_terminate)_view.on_terminate;
    if (terminate) {
        terminate(&_view);
    }
    return NSTerminateNow;
}
@end

//------------------------------------------------------------------------------
@implementation ViewWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    [NSApp terminate:nil];
    return YES;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    const NSRect content_rect = [view_window contentRectForFrameRect:[view_window frame]];
    view_osx_sync_metrics(content_rect.size.width, content_rect.size.height);
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
    (void)notification;
    const NSRect content_rect = [view_window contentRectForFrameRect:[view_window frame]];
    view_osx_sync_metrics(content_rect.size.width, content_rect.size.height);
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    const NSRect content_rect = [view_window contentRectForFrameRect:[view_window frame]];
    view_osx_sync_metrics(content_rect.size.width, content_rect.size.height);
}
@end

static void view_osx_update_mouse(NSEvent *event) {
    const NSPoint p = [event locationInWindow];
    const NSRect content_rect = [[event window] contentRectForFrameRect:[[event window] frame]];
    const f64 x = p.x;
    const f64 y = content_rect.size.height - p.y;
    view_on_mouse_move(&_view, x, y);
}

//------------------------------------------------------------------------------
@implementation ViewDelegate
- (void)mtkView:(nonnull MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
    // Handle drawable size change
}

- (void)drawInMTKView:(nonnull MTKView*)view {
    view_on_frame frame = (view_on_frame)_view.on_frame;
    if (frame) {
        gpu_mtl_begin_frame(view);
        frame(&_view);
    }
}
@end

//------------------------------------------------------------------------------
@implementation ViewMTKView
- (void)mouseDown:(NSEvent*)event {
    view_osx_update_mouse(event);
    if (view_osx_handle_no_title_mouse_down(event)) return;
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_PRESS);
}

- (void)mouseUp:(NSEvent*)event {
    view_osx_update_mouse(event);
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_RELEASE);
}

- (void)mouseMoved:(NSEvent*)event {
    view_osx_update_mouse(event);
}

- (void)mouseDragged:(NSEvent*)event {
    view_osx_update_mouse(event);
}

- (void)rightMouseDown:(NSEvent*)event {
    view_osx_update_mouse(event);
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_RIGHT, VIEW_BUTTON_ACTION_PRESS);
}

- (void)rightMouseUp:(NSEvent*)event {
    view_osx_update_mouse(event);
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_RIGHT, VIEW_BUTTON_ACTION_RELEASE);
}

- (void)rightMouseDragged:(NSEvent*)event {
    view_osx_update_mouse(event);
}

- (void)keyDown:(NSEvent*)event {
    const i32 key = view_osx_key_map((i32)[event keyCode]);
    if (key >= 0 && ![event isARepeat]) {
        view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_PRESS);
    }
}

- (void)flagsChanged:(NSEvent*)event {
    NSEventModifierFlags flags = [event modifierFlags];
    ns_bool command_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_SUPER);
    ns_bool control_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_CONTROL);
    ns_bool shift_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_SHIFT);
    ns_bool alt_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_ALT);
    
    if (flags & NSEventModifierFlagCommand && !command_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_SUPER, VIEW_BUTTON_ACTION_PRESS);
    }

    if (!(flags & NSEventModifierFlagCommand) && command_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_SUPER, VIEW_BUTTON_ACTION_RELEASE);
    }

    if (flags & NSEventModifierFlagControl && !control_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_CONTROL, VIEW_BUTTON_ACTION_PRESS);
    }

    if (!(flags & NSEventModifierFlagControl) && control_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_CONTROL, VIEW_BUTTON_ACTION_RELEASE);
    }

    if (flags & NSEventModifierFlagShift && !shift_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_SHIFT, VIEW_BUTTON_ACTION_PRESS);
    }

    if (!(flags & NSEventModifierFlagShift) && shift_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_SHIFT, VIEW_BUTTON_ACTION_RELEASE);
    }

    if (flags & NSEventModifierFlagOption && !alt_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_ALT, VIEW_BUTTON_ACTION_PRESS);
    }

    if (!(flags & NSEventModifierFlagOption) && alt_pressed) {
        view_on_key_action(&_view, VIEW_KEY_LEFT_ALT, VIEW_BUTTON_ACTION_RELEASE);
    }
}

- (void)keyUp:(NSEvent*)event {
    const i32 key = view_osx_key_map((i32)[event keyCode]);
    if (key >= 0) {
        view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_RELEASE);
    }
}

- (void)scrollWheel:(NSEvent*)event {
    const f64 dx = [event scrollingDeltaX];
    const f64 dy = [event scrollingDeltaY];
    view_osx_update_mouse(event);
    view_on_scroll(&_view, dx, dy);
}

- (BOOL)acceptsFirstResponder {
    return YES;
}
@end

//------------------------------------------------------------------------------
// Create the application, window and Metal view. Does NOT enter the run loop so
// the caller can attach callbacks first; view_run() then drives frames.
void view_osx_create(i32 w, i32 h, const char* title, ns_bool no_title) {
    view_width = w;
    view_height = h;
    view_title = title;
    view_no_title = no_title;

    [ViewApp sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    view_osx_apply_manifest_icon();
    id delegate = [[ViewAppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    view_window_delegate = [[ViewWindowDelegate alloc] init];
    const NSUInteger style =
        NSWindowStyleMaskTitled |
        NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable |
        (no_title ? NSWindowStyleMaskFullSizeContentView : 0);
    view_window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, view_width, view_height)
        styleMask: style
        backing: NSBackingStoreBuffered
        defer: NO];
    [view_window setTitle:[NSString stringWithUTF8String: view_title]];
    if (no_title) {
        [view_window setTitleVisibility:NSWindowTitleHidden];
        [view_window setTitlebarAppearsTransparent:YES];
        [view_window setMovableByWindowBackground:NO];
        [[view_window standardWindowButton:NSWindowCloseButton] setHidden:YES];
        [[view_window standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
        [[view_window standardWindowButton:NSWindowZoomButton] setHidden:YES];
    }
    [view_window setAcceptsMouseMovedEvents: YES];
    [view_window center];
    [view_window setRestorable: YES];
    [view_window setDelegate: view_window_delegate];

    view_mtl_device = MTLCreateSystemDefaultDevice();
    view_mtk_view = [[ViewMTKView alloc] init];
    [view_mtk_view setDevice: view_mtl_device];
    [view_mtk_view setColorPixelFormat: MTLPixelFormatBGRA8Unorm];
    [view_mtk_view setDepthStencilPixelFormat: MTLPixelFormatDepth32Float];
    view_mtk_view_delegate = [[ViewDelegate alloc] init];
    [view_mtk_view setDelegate: view_mtk_view_delegate];
    [view_window setContentView: view_mtk_view];
    [view_window makeKeyAndOrderFront: nil];
    const NSRect content_rect = [view_window contentRectForFrameRect:[view_window frame]];
    view_osx_sync_metrics(content_rect.size.width, content_rect.size.height);

    _view.native_window = (__bridge void*)view_window;
    _view.gpu_device = (__bridge void*)view_mtl_device;
}

/* return current MTKView drawable width */
i32 view_osx_width() {
    return (int) [view_mtk_view drawableSize].width;
}

/* return current MTKView drawable height */
i32 view_osx_height() {
    return (int) [view_mtk_view drawableSize].height;
}

#if defined(__OBJC__)
id<MTLDevice> view_osx_mtl_device() {
    return view_mtl_device;
}
#endif

view* view_create(const char *title, i32 width, i32 height) {
    _view.width = width;
    _view.height = height;
    _view.display_ratio = 1.0;
    _view.ui_scale = 1.0;
    _view.framebuffer_width = width;
    _view.framebuffer_height = height;
    _view.title = ns_str_cstr((char*)title);
    view_osx_create(width, height, title, false);
    return &_view;
}

view* view_create_no_title(const char *title, i32 width, i32 height) {
    _view.width = width;
    _view.height = height;
    _view.display_ratio = 1.0;
    _view.ui_scale = 1.0;
    _view.framebuffer_width = width;
    _view.framebuffer_height = height;
    _view.title = ns_str_cstr((char*)title);
    view_osx_create(width, height, title, true);
    return &_view;
}

void view_run(view *v) {
    (void)v;
    [NSApp run];
}

ns_str view_get_clipboard(view *v) {
    ns_unused(v);
    return ns_str_null;
}

void view_set_clipboard(view *v, ns_str text) {
    ns_unused(v);
    ns_unused(text);
}
