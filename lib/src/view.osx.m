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
static ns_bool view_title_click_pending;
static ns_bool view_title_dragging;
static NSPoint view_title_start_mouse;
static NSRect view_title_start_frame;
static ns_bool view_traffic_button_pending;
static i32 view_traffic_button;

static view _view;

static void view_osx_update_mouse(NSEvent *event);
static void view_osx_request_terminate(void);

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

static void view_osx_sync_mtk_view_metrics(MTKView *mtk_view) {
    if (!mtk_view) return;
    NSRect bounds = [mtk_view bounds];
    f64 width = bounds.size.width;
    f64 height = bounds.size.height;
    CGSize drawable = [mtk_view drawableSize];
    if (width <= 0.0 || height <= 0.0) {
        const f64 ratio = view_osx_current_display_ratio();
        width = ratio > 0.0 ? drawable.width / ratio : drawable.width;
        height = ratio > 0.0 ? drawable.height / ratio : drawable.height;
    }
    view_osx_sync_metrics(width, height);
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

static void view_osx_request_terminate(void) {
    view_title_click_pending = false;
    view_title_dragging = false;
    view_traffic_button_pending = false;
    view_traffic_button = -1;
    [NSApp terminate:nil];
}

static NSPoint view_osx_event_content_point(NSEvent *event) {
    const NSPoint p = [event locationInWindow];
    const NSRect content_rect = [[event window] contentRectForFrameRect:[[event window] frame]];
    return NSMakePoint(p.x, content_rect.size.height - p.y);
}

static ns_bool view_osx_no_title_button(NSPoint p, i32 *button) {
    if (!view_no_title || p.y < 0.0 || p.y > 38.0) return false;

    const f64 centers[] = {18.0, 40.0, 62.0};
    const f64 cy = 19.0;
    for (i32 i = 0; i < 3; i++) {
        const f64 dx = p.x - centers[i];
        const f64 dy = p.y - cy;
        if (dx * dx + dy * dy <= 100.0) {
            *button = i;
            return true;
        }
    }
    return false;
}

static ns_bool view_osx_handle_no_title_mouse_down(NSEvent *event) {
    if (!view_no_title) return false;

    const NSPoint p = view_osx_event_content_point(event);
    if (p.y < 0.0 || p.y > 38.0) return false;

    i32 button = -1;
    if (view_osx_no_title_button(p, &button)) {
        view_title_click_pending = false;
        view_title_dragging = false;
        view_traffic_button_pending = true;
        view_traffic_button = button;
        view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_PRESS);
        return true;
    }

    if ([event clickCount] == 2) {
        view_title_click_pending = false;
        view_title_dragging = false;
        view_traffic_button_pending = false;
        view_traffic_button = -1;
        [[event window] zoom:nil];
        return true;
    }

    view_title_click_pending = true;
    view_title_dragging = false;
    view_title_start_mouse = [NSEvent mouseLocation];
    view_title_start_frame = [[event window] frame];
    return true;
}

static ns_bool view_osx_handle_no_title_mouse_dragged(NSEvent *event) {
    if (view_traffic_button_pending) return true;
    if (!view_title_click_pending) return false;

    NSPoint mouse = [NSEvent mouseLocation];
    const f64 dx = mouse.x - view_title_start_mouse.x;
    const f64 dy = mouse.y - view_title_start_mouse.y;
    if (!view_title_dragging && (dx * dx + dy * dy) > 9.0) {
        view_title_dragging = true;
    }
    if (view_title_dragging) {
        NSPoint origin = NSMakePoint(view_title_start_frame.origin.x + dx, view_title_start_frame.origin.y + dy);
        [[event window] setFrameOrigin:origin];
    }
    return true;
}

static ns_bool view_osx_handle_no_title_mouse_up(NSEvent *event) {
    if (view_traffic_button_pending) {
        i32 button = -1;
        const ns_bool activate = view_osx_no_title_button(view_osx_event_content_point(event), &button) && button == view_traffic_button;
        view_traffic_button_pending = false;
        view_traffic_button = -1;
        view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_RELEASE);
        if (activate) {
            if (button == 0) {
                view_osx_request_terminate();
            } else if (button == 1) {
                [[event window] miniaturize:nil];
            } else {
                [[event window] toggleFullScreen:nil];
            }
        }
        return true;
    }

    if (!view_title_click_pending) return false;

    const ns_bool clicked = !view_title_dragging;
    view_title_click_pending = false;
    view_title_dragging = false;
    if (clicked) {
        view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_PRESS);
        view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_RELEASE);
    }
    return true;
}

// Key mapping function (ANSI layout keycodes -> VIEW_KEY_*)
i32 view_osx_key_map(i32 k) {
    switch (k) {
        case 0: return VIEW_KEY_A;
        case 1: return VIEW_KEY_S;
        case 2: return VIEW_KEY_D;
        case 3: return VIEW_KEY_F;
        case 4: return VIEW_KEY_H;
        case 5: return VIEW_KEY_G;
        case 6: return VIEW_KEY_Z;
        case 7: return VIEW_KEY_X;
        case 8: return VIEW_KEY_C;
        case 9: return VIEW_KEY_V;
        case 11: return VIEW_KEY_B;
        case 12: return VIEW_KEY_Q;
        case 13: return VIEW_KEY_W;
        case 14: return VIEW_KEY_E;
        case 15: return VIEW_KEY_R;
        case 16: return VIEW_KEY_Y;
        case 17: return VIEW_KEY_T;
        case 18: return VIEW_KEY_1;
        case 19: return VIEW_KEY_2;
        case 20: return VIEW_KEY_3;
        case 21: return VIEW_KEY_4;
        case 22: return VIEW_KEY_6;
        case 23: return VIEW_KEY_5;
        case 24: return VIEW_KEY_EQUAL;
        case 25: return VIEW_KEY_9;
        case 26: return VIEW_KEY_7;
        case 27: return VIEW_KEY_MINUS;
        case 28: return VIEW_KEY_8;
        case 29: return VIEW_KEY_0;
        case 30: return VIEW_KEY_RIGHT_BRACKET;
        case 31: return VIEW_KEY_O;
        case 32: return VIEW_KEY_U;
        case 33: return VIEW_KEY_LEFT_BRACKET;
        case 34: return VIEW_KEY_I;
        case 35: return VIEW_KEY_P;
        case 37: return VIEW_KEY_L;
        case 38: return VIEW_KEY_J;
        case 39: return VIEW_KEY_APOSTROPHE;
        case 40: return VIEW_KEY_K;
        case 41: return VIEW_KEY_SEMICOLON;
        case 42: return VIEW_KEY_BACKSLASH;
        case 43: return VIEW_KEY_COMMA;
        case 44: return VIEW_KEY_SLASH;
        case 45: return VIEW_KEY_N;
        case 46: return VIEW_KEY_M;
        case 47: return VIEW_KEY_PERIOD;
        case 50: return VIEW_KEY_GRAVE_ACCENT;
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
    if ([event type] == NSEventTypeKeyDown &&
        ([event modifierFlags] & NSEventModifierFlagCommand) &&
        [event keyCode] == 12 &&
        ![event isARepeat]) {
        view_osx_request_terminate();
    }
    else if ([event type] == NSEventTypeKeyUp && ([event modifierFlags] & NSEventModifierFlagCommand)) {
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
    view_osx_request_terminate();
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
    (void)size;
    view_osx_sync_mtk_view_metrics(view);
}

- (void)drawInMTKView:(nonnull MTKView*)view {
    view_osx_sync_mtk_view_metrics(view);
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
    if (view_osx_handle_no_title_mouse_up(event)) return;
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_RELEASE);
}

- (void)mouseMoved:(NSEvent*)event {
    view_osx_update_mouse(event);
}

- (void)mouseDragged:(NSEvent*)event {
    view_osx_update_mouse(event);
    if (view_osx_handle_no_title_mouse_dragged(event)) return;
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
