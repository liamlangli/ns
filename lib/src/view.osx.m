#include <TargetConditionals.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <dispatch/semaphore.h>

#include "view.h"

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
static view_on_launch launch_func = NULL;
static view_on_frame frame_func = NULL;
static view_on_terminate terminate_func = NULL;

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

    if (launch_func) {
        launch_func(&_view);
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (terminate_func) {
        terminate_func(&_view);
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

//------------------------------------------------------------------------------
@implementation ViewDelegate
- (void)mtkView:(nonnull MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
    // Handle drawable size change
}

- (void)drawInMTKView:(nonnull MTKView*)view {
    (void)view;
    if (frame_func) {
        frame_func(&_view);
    }
}
@end

//------------------------------------------------------------------------------
@implementation ViewMTKView
- (void)mouseDown:(NSEvent*)event {
    (void)event;
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_PRESS);
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_RELEASE);
}

- (void)mouseMoved:(NSEvent*)event {
    const NSPoint p = [event locationInWindow];
    const NSRect content_rect = [[event window] contentRectForFrameRect:[[event window] frame]];
    const f64 x = p.x;
    const f64 y = content_rect.size.height - p.y;
    view_on_mouse_move(&_view, x, y);
}

- (void)rightMouseDown:(NSEvent*)event {
    (void)event;
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_RIGHT, VIEW_BUTTON_ACTION_PRESS);
}

- (void)rightMouseUp:(NSEvent*)event {
    (void)event;
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_RIGHT, VIEW_BUTTON_ACTION_RELEASE);
}

- (void)keyDown:(NSEvent*)event {
    (void)event;
    // Key down handling - currently commented out to match original
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
    (void)event;
    // Key up handling - currently commented out to match original
}

- (void)scrollWheel:(NSEvent*)event {
    const f64 dx = [event scrollingDeltaX];
    const f64 dy = [event scrollingDeltaY];
    ns_unused(dx);
    ns_unused(dy);
    // view_on_scroll(&_view, dx, dy);
}

- (BOOL)acceptsFirstResponder {
    return YES;
}
@end

//------------------------------------------------------------------------------
void view_osx_start(i32 w, i32 h, const char* title) {
    view_width = w;
    view_height = h;
    view_title = title;
    [ViewApp sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    id delegate = [[ViewAppDelegate alloc] init];
    [NSApp setDelegate:delegate];
    [NSApp run];
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
    view_osx_start(width, height, title);
    return &_view;
}

ns_str view_get_clipboard(view *v) {
    ns_unused(v);
    return ns_str_null;
}

void view_set_clipboard(view *v, ns_str text) {
    ns_unused(v);
    ns_unused(text);
}
