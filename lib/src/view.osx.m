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

static view _view;

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
    view_on_resize(&_view, content_rect.size.width, content_rect.size.height);
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
void view_osx_create(i32 w, i32 h, const char* title) {
    view_width = w;
    view_height = h;
    view_title = title;

    [ViewApp sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    id delegate = [[ViewAppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    view_window_delegate = [[ViewWindowDelegate alloc] init];
    const NSUInteger style =
        NSWindowStyleMaskTitled |
        NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable;
    view_window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, view_width, view_height)
        styleMask: style
        backing: NSBackingStoreBuffered
        defer: NO];
    [view_window setTitle:[NSString stringWithUTF8String: view_title]];
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
    _view.ui_scale = 2.0;
    _view.framebuffer_width = width * _view.ui_scale;
    _view.framebuffer_height = height * _view.ui_scale;
    _view.title = ns_str_cstr((char*)title);
    view_osx_create(width, height, title);
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
