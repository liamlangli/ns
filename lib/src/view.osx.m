#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <dispatch/semaphore.h>

#include "view.h"

void gpu_mtl_begin_frame(MTKView *view);
void gpu_mtl_end_frame(MTKView *view);

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

// Keeps keyboard focus when an application makes the window borderless (a
// fullscreen overlay); a plain borderless NSWindow can never become key. Main
// status still needs a visible window: AppKit throws for a hidden one.
@interface ViewWindow : NSWindow
@end

@implementation ViewWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return self.isVisible; }
@end

typedef void(*view_on_launch)(view*);
typedef void(*view_on_frame)(view*);
typedef void(*view_on_terminate)(view*);

static i32 view_width;
static i32 view_height;
static const char* view_title;
static NSWindow* view_window;
static id view_app_delegate;
static id view_window_delegate;
static id<MTLDevice> view_mtl_device;
static id view_mtk_view_delegate;
static MTKView* view_mtk_view;
// The shape view_set_cursor asked for; nil is the arrow.
static NSCursor* view_osx_cursor;
static dispatch_semaphore_t view_osx_done;
static ns_bool view_osx_hosted;
static ns_bool view_osx_finished;
static i32 view_osx_last_fb_w;
static i32 view_osx_last_fb_h;

static view _view;

static void view_osx_update_mouse(NSEvent *event);
static void view_osx_request_terminate(void);

static void view_osx_apply_frame_rate(void) {
    if (!view_mtk_view) return;
    const BOOL continuous = view_continuous_render() ? YES : NO;
    [view_mtk_view setPaused: continuous ? NO : YES];
    [view_mtk_view setEnableSetNeedsDisplay: continuous ? NO : YES];
    const i32 fps = view_frame_per_second();
    [view_mtk_view setPreferredFramesPerSecond: fps > 0 ? fps : 0];
}

static void view_osx_activate_window(void) {
    if (!view_window) return;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [[NSRunningApplication currentApplication]
        activateWithOptions:NSApplicationActivateAllWindows];
    [NSApp activateIgnoringOtherApps:YES];
    // Order the window in before making it main: AppKit throws when a window
    // that is not visible is made main.
    [view_window makeKeyAndOrderFront:nil];
    if (view_window.canBecomeMainWindow) [view_window makeMainWindow];
    if (view_mtk_view) [view_window makeFirstResponder:view_mtk_view];
}

static void view_osx_request_activation(void) {
    void (^activate)(void) = ^{ view_osx_activate_window(); };
    if ([NSThread isMainThread]) activate();
    else dispatch_async(dispatch_get_main_queue(), activate);
}

static void view_osx_finish(void) {
    if (view_osx_finished) return;
    view_osx_finished = true;
    view_on_terminate terminate = (view_on_terminate)_view.on_terminate;
    if (terminate) terminate(&_view);
    if (view_osx_done) dispatch_semaphore_signal(view_osx_done);
}

static void view_osx_sync_modifiers(NSEventModifierFlags flags) {
    const ns_bool command_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_SUPER);
    const ns_bool control_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_CONTROL);
    const ns_bool shift_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_SHIFT);
    const ns_bool alt_pressed = view_is_key_pressed(&_view, VIEW_KEY_LEFT_ALT);

    if ((flags & NSEventModifierFlagCommand) && !command_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_SUPER, VIEW_BUTTON_ACTION_PRESS);
    if (!(flags & NSEventModifierFlagCommand) && command_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_SUPER, VIEW_BUTTON_ACTION_RELEASE);
    if ((flags & NSEventModifierFlagControl) && !control_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_CONTROL, VIEW_BUTTON_ACTION_PRESS);
    if (!(flags & NSEventModifierFlagControl) && control_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_CONTROL, VIEW_BUTTON_ACTION_RELEASE);
    if ((flags & NSEventModifierFlagShift) && !shift_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_SHIFT, VIEW_BUTTON_ACTION_PRESS);
    if (!(flags & NSEventModifierFlagShift) && shift_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_SHIFT, VIEW_BUTTON_ACTION_RELEASE);
    if ((flags & NSEventModifierFlagOption) && !alt_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_ALT, VIEW_BUTTON_ACTION_PRESS);
    if (!(flags & NSEventModifierFlagOption) && alt_pressed)
        view_on_key_action(&_view, VIEW_KEY_LEFT_ALT, VIEW_BUTTON_ACTION_RELEASE);
}

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

// AppKit reports insets for the notch of a built-in display and for a
// full-size content view that runs under the title bar; ordinary windows
// report zero.
static void view_osx_sync_safe_area(void) {
    NSEdgeInsets insets = NSEdgeInsetsMake(0.0, 0.0, 0.0, 0.0);
    if (@available(macOS 11.0, *)) {
        NSView *content = view_mtk_view ? (NSView *)view_mtk_view : [view_window contentView];
        if (content) insets = [content safeAreaInsets];
    }
    view_set_safe_area(&_view, insets.top, insets.right, insets.bottom, insets.left);
}

static void view_osx_sync_metrics(f64 width, f64 height) {
    const f64 ratio = view_osx_current_display_ratio();
    _view.display_ratio = ratio;
    _view.ui_scale = ratio;
    _view.width = (i32)width;
    _view.height = (i32)height;
    _view.framebuffer_width = (i32)(width * ratio + 0.5);
    _view.framebuffer_height = (i32)(height * ratio + 0.5);
    view_osx_sync_safe_area();
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
    if (drawable.width > 0.0 && drawable.height > 0.0) {
        _view.framebuffer_width = (i32)(drawable.width + 0.5);
        _view.framebuffer_height = (i32)(drawable.height + 0.5);
    }
}

static void view_osx_note_window_size(void) {
    if (!view_window) return;
    const NSRect content_rect = [view_window contentRectForFrameRect:[view_window frame]];
    view_osx_sync_metrics(content_rect.size.width, content_rect.size.height);
    // On-demand MTKView sleeps until invalidated. A size change with no
    // request leaves the new drawable uncleared (black borders).
    view_request_frame(&_view, 2);
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
    [NSApp terminate:nil];
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
    else if ([event type] == NSEventTypeKeyDown ||
             [event type] == NSEventTypeKeyUp ||
             [event type] == NSEventTypeFlagsChanged) {
        // This backend has no AppKit menu and ViewMTKView owns keyboard input.
        // Sending key events through NSApplication needlessly invokes AppKit's
        // menu-key translation path.  On newer macOS builds that path also
        // emits an NSSoftLinking warning for a private HIToolbox symbol.
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
    // A script launched by `ns run` is hosted by the interpreter executable.
    // Claim foreground activation only after AppKit has finished launching,
    // then make the target view the key input responder.
    view_osx_activate_window();
    view_on_launch launch = (view_on_launch)_view.on_launch;
    if (launch) {
        launch(&_view);
    }
}

// The program's view_set_dock_menu titles, rebuilt each time the Dock asks.
- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
    (void)sender;
    NSString *items = [NSString stringWithUTF8String:view_dock_menu_items()];
    if (!items || [items length] == 0) return nil;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    NSInteger index = 0;
    for (NSString *title in [items componentsSeparatedByString:@"\n"]) {
        if ([title isEqualToString:@"-"]) {
            [menu addItem:[NSMenuItem separatorItem]];
        } else if ([title length] > 0) {
            NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                          action:@selector(dockMenuPicked:)
                                                   keyEquivalent:@""];
            [item setTarget:self];
            [item setTag:index];
            [menu addItem:item];
#if !__has_feature(objc_arc)
            [item release];
#endif
        }
        index++;
    }
#if !__has_feature(objc_arc)
    [menu autorelease];
#endif
    return menu;
}

- (void)dockMenuPicked:(NSMenuItem*)item {
    view_on_dock_menu(&_view, (i32)[item tag]);
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    view_osx_finish();
    return NSTerminateNow;
}
@end

//------------------------------------------------------------------------------
@implementation ViewWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    view_osx_finish();
    view_osx_request_terminate();
    return YES;
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    view_request_frame(&_view, 1);
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
    (void)notification;
    view_request_frame(&_view, 1);
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    view_osx_note_window_size();
}

- (void)windowDidEndLiveResize:(NSNotification*)notification {
    (void)notification;
    view_osx_note_window_size();
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
    (void)notification;
    view_osx_note_window_size();
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    view_osx_note_window_size();
}
@end

static void view_osx_update_mouse(NSEvent *event) {
    const NSPoint p = [event locationInWindow];
    const NSRect content_rect = [[event window] contentRectForFrameRect:[[event window] frame]];
    const f64 x = p.x;
    const f64 y = content_rect.size.height - p.y;
    view_on_mouse_move(&_view, x, y);
}

// AppKit reports a trackpad rotation in degrees; the gesture state carries
// radians, as the iOS backend's recognizer already does.
#define VIEW_OSX_DEGREES_TO_RADIANS 0.017453292519943295

// The orbit gesture is exactly three fingers on an indirect device. One and two
// finger travel already belongs to scroll, magnify and rotate, so a set of any
// other size ends the tracking instead of publishing an orbit.
#define VIEW_OSX_ORBIT_TOUCHES 3

// A tap is every finger lifted within VIEW_OSX_TAP_TIME of the first landing,
// none of them farther than VIEW_OSX_TAP_SLOP of the trackpad from where it
// landed. A cancelled touch means the system took the gesture: no tap.
#define VIEW_OSX_TAP_TIME 0.5
#define VIEW_OSX_TAP_SLOP 0.04

static ns_bool view_osx_orbit_tracking;
static NSMutableDictionary *view_osx_tap_start;
static f64 view_osx_tap_begin;
static i32 view_osx_tap_peak;
static ns_bool view_osx_tap_spoiled;
static f64 view_osx_orbit_last_x;
static f64 view_osx_orbit_last_y;

// The travel is measured from the centroid of the touching set. A centroid does
// not depend on the order the set is enumerated in, so no per-finger identity
// has to be carried between events. Normalized positions are fractions of the
// trackpad surface with the origin at its lower left, so the device size turns
// them into points and the vertical axis is flipped to the view's downward one.
static void view_osx_touch_orbit(NSSet<NSTouch*> *touches, ns_bool publish) {
    if ([touches count] != VIEW_OSX_ORBIT_TOUCHES) {
        view_osx_orbit_tracking = false;
        return;
    }
    f64 x = 0.0;
    f64 y = 0.0;
    CGSize device = CGSizeMake(0.0, 0.0);
    for (NSTouch *touch in touches) {
        const NSPoint position = [touch normalizedPosition];
        x += position.x;
        y += position.y;
        device = [touch deviceSize];
    }
    x = x / (f64)VIEW_OSX_ORBIT_TOUCHES * device.width;
    y = y / (f64)VIEW_OSX_ORBIT_TOUCHES * device.height;
    if (view_osx_orbit_tracking && publish) {
        view_on_orbit_gesture(&_view, x - view_osx_orbit_last_x, view_osx_orbit_last_y - y);
    }
    view_osx_orbit_last_x = x;
    view_osx_orbit_last_y = y;
    view_osx_orbit_tracking = true;
}

// Counts the fingers resting on the trackpad for the gesture state and
// recognizes a tap: the gesture runs from the first finger landing to the last
// lifting, each touch is remembered where it landed, and the tap reports the
// most fingers that rested together.
static void view_osx_touch_tap(NSView *view, NSEvent *event, NSSet<NSTouch*> *touching, ns_bool cancelled) {
    const i32 count = (i32)[touching count];
    const f64 now = [event timestamp];
    if (!view_osx_tap_start) view_osx_tap_start = [[NSMutableDictionary alloc] init];
    if (view_osx_tap_peak == 0 && count > 0) {
        [view_osx_tap_start removeAllObjects];
        view_osx_tap_begin = now;
        view_osx_tap_spoiled = false;
    }
    if (cancelled || now - view_osx_tap_begin > VIEW_OSX_TAP_TIME) view_osx_tap_spoiled = true;
    for (NSTouch *touch in [event touchesMatchingPhase:NSTouchPhaseAny inView:view]) {
        const NSPoint position = [touch normalizedPosition];
        NSValue *start = view_osx_tap_start[[touch identity]];
        if (!start) {
            view_osx_tap_start[[touch identity]] = [NSValue valueWithPoint:position];
            continue;
        }
        const NSPoint from = [start pointValue];
        if (fabs(position.x - from.x) > VIEW_OSX_TAP_SLOP || fabs(position.y - from.y) > VIEW_OSX_TAP_SLOP) {
            view_osx_tap_spoiled = true;
        }
    }
    if (count > view_osx_tap_peak) view_osx_tap_peak = count;
    view_on_trackpad_fingers(&_view, count);
    if (count == 0 && view_osx_tap_peak > 0) {
        if (!view_osx_tap_spoiled) view_on_trackpad_tap(&_view, view_osx_tap_peak);
        view_osx_tap_peak = 0;
        [view_osx_tap_start removeAllObjects];
    }
}

//------------------------------------------------------------------------------
@implementation ViewDelegate
- (void)mtkView:(nonnull MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)size;
    view_osx_sync_mtk_view_metrics(view);
    view_request_frame(&_view, 2);
}

- (void)drawInMTKView:(nonnull MTKView*)view {
    view_osx_sync_mtk_view_metrics(view);
    const ns_bool size_changed = _view.framebuffer_width != view_osx_last_fb_w ||
                                 _view.framebuffer_height != view_osx_last_fb_h;
    if (!view_take_frame_request(&_view)) {
        if (!size_changed) return;
    }
    view_on_frame frame = (view_on_frame)_view.on_frame;
    if (frame) {
        view_apple_gamepad_poll(&_view);
        gpu_mtl_begin_frame(view);
        frame(&_view);
        // The frame owns the drawable between these two calls: whatever the
        // frame committed in between, the present happens here and once.
        gpu_mtl_end_frame(view);
    }
    view_osx_last_fb_w = _view.framebuffer_width;
    view_osx_last_fb_h = _view.framebuffer_height;
    view_complete_frame(&_view);
}
@end

//------------------------------------------------------------------------------
@implementation ViewMTKView
// This view consumes physical key events directly and is not an
// NSTextInputClient.  Explicitly opt out of AppKit's input-method context so
// recent macOS releases do not try to wake the InputMethodKit Mach port for a
// client that cannot accept composed text.
- (NSTextInputContext*)inputContext {
    return nil;
}

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
    if (view_osx_cursor) [view_osx_cursor set];
}

- (void)mouseDragged:(NSEvent*)event {
    view_osx_update_mouse(event);
    if (view_osx_cursor) [view_osx_cursor set];
}

// AppKit resets the cursor from the view's cursor rects whenever the pointer
// enters it or the window reshapes, so the rect carries the chosen shape.
- (void)resetCursorRects {
    [self addCursorRect:[self bounds] cursor:view_osx_cursor ? view_osx_cursor : [NSCursor arrowCursor]];
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

- (void)otherMouseDown:(NSEvent*)event {
    if ([event buttonNumber] != 2) return;
    view_osx_update_mouse(event);
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_MIDDLE, VIEW_BUTTON_ACTION_PRESS);
}

- (void)otherMouseUp:(NSEvent*)event {
    if ([event buttonNumber] != 2) return;
    view_osx_update_mouse(event);
    view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_MIDDLE, VIEW_BUTTON_ACTION_RELEASE);
}

- (void)otherMouseDragged:(NSEvent*)event {
    if ([event buttonNumber] != 2) return;
    view_osx_update_mouse(event);
}

- (void)keyDown:(NSEvent*)event {
    // Capture the modifiers carried by this exact key event. Relying only on a
    // preceding flagsChanged delivery can make a quick Cmd+Z look like plain Z.
    view_osx_sync_modifiers([event modifierFlags]);
    const i32 key = view_osx_key_map((i32)[event keyCode]);
    if (key >= 0 && ![event isARepeat]) {
        view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_PRESS);
    }
}

- (void)flagsChanged:(NSEvent*)event {
    view_osx_sync_modifiers([event modifierFlags]);
}

- (void)keyUp:(NSEvent*)event {
    const i32 key = view_osx_key_map((i32)[event keyCode]);
    if (key >= 0) {
        view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_RELEASE);
    }
}

- (void)scrollWheel:(NSEvent*)event {
    // Three or more resting fingers drive the orbit (and the system's own
    // swipes); the scroll their first two fingers began is not theirs.
    if (view_gesture(&_view)->fingers >= VIEW_OSX_ORBIT_TOUCHES) return;
    const f64 dx = [event scrollingDeltaX];
    const f64 dy = [event scrollingDeltaY];
    view_osx_update_mouse(event);
    view_on_scroll(&_view, dx, dy);
}

// A pinch reports the magnification added since the last event, so the factor
// the gesture state multiplies is that step around 1.
- (void)magnifyWithEvent:(NSEvent*)event {
    const f64 factor = 1.0 + [event magnification];
    view_osx_update_mouse(event);
    if (factor > 0.0) view_on_gesture(&_view, 0.0, 0.0, factor, 0.0);
}

// Counterclockwise finger travel is positive in both AppKit's degrees and the
// radians the gesture state accumulates.
- (void)rotateWithEvent:(NSEvent*)event {
    view_osx_update_mouse(event);
    view_on_gesture(&_view, 0.0, 0.0, 1.0, [event rotation] * VIEW_OSX_DEGREES_TO_RADIANS);
}

- (void)touchesBeganWithEvent:(NSEvent*)event {
    NSSet<NSTouch*> *touching = [event touchesMatchingPhase:NSTouchPhaseTouching inView:self];
    view_osx_touch_orbit(touching, false);
    view_osx_touch_tap(self, event, touching, false);
}

- (void)touchesMovedWithEvent:(NSEvent*)event {
    NSSet<NSTouch*> *touching = [event touchesMatchingPhase:NSTouchPhaseTouching inView:self];
    view_osx_touch_orbit(touching, true);
    view_osx_touch_tap(self, event, touching, false);
}

- (void)touchesEndedWithEvent:(NSEvent*)event {
    view_osx_orbit_tracking = false;
    view_osx_touch_tap(self, event, [event touchesMatchingPhase:NSTouchPhaseTouching inView:self], false);
}

- (void)touchesCancelledWithEvent:(NSEvent*)event {
    view_osx_orbit_tracking = false;
    view_osx_touch_tap(self, event, [event touchesMatchingPhase:NSTouchPhaseTouching inView:self], true);
}

- (BOOL)acceptsFirstResponder {
    return YES;
}
@end

// Give the window the content size view_create asked for, centred, and cut to
// the screen's visible frame (below the menu bar, beside the Dock) when the
// request is larger than that. A hosted application's window was sized by its
// host's own content, so this applies to it as well.
static void view_osx_size_window(NSWindow *window, CGFloat width, CGFloat height) {
    [window setContentSize:NSMakeSize(width, height)];
    [window center];
    NSScreen *screen = [window screen] ? [window screen] : [NSScreen mainScreen];
    if (!screen) return;
    const NSRect visible = [screen visibleFrame];
    NSRect frame = [window frame];
    if (frame.size.width <= visible.size.width && frame.size.height <= visible.size.height) return;
    frame.size.width = MIN(frame.size.width, visible.size.width);
    frame.size.height = MIN(frame.size.height, visible.size.height);
    frame.origin.x = NSMidX(visible) - frame.size.width * 0.5;
    frame.origin.y = NSMidY(visible) - frame.size.height * 0.5;
    [window setFrame:frame display:NO];
}

//------------------------------------------------------------------------------
// Create the application, window and Metal view. Does NOT enter the run loop so
// the caller can attach callbacks first; view_run() then drives frames.
void view_osx_create(i32 w, i32 h, const char* title) {
    view_width = w;
    view_height = h;
    view_title = title;

    [ViewApp sharedApplication];
    view_osx_hosted = [NSApp isRunning];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (!view_osx_hosted && view_title && view_title[0] != '\0') {
        NSString *application_name = [NSString stringWithUTF8String:view_title];
        if (application_name) [[NSProcessInfo processInfo] setProcessName:application_name];
    }
    view_osx_apply_manifest_icon();
    if (!view_osx_hosted) {
        view_app_delegate = [[ViewAppDelegate alloc] init];
        [NSApp setDelegate:view_app_delegate];
    }

    view_window_delegate = [[ViewWindowDelegate alloc] init];
    const NSUInteger style =
        NSWindowStyleMaskTitled |
        NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable;
    view_window = view_osx_hosted ? [NSApp keyWindow] : nil;
    if (!view_window && view_osx_hosted) view_window = [[NSApp windows] firstObject];
    if (!view_window) {
        view_window = [[ViewWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, view_width, view_height)
            styleMask: style
            backing: NSBackingStoreBuffered
            defer: NO];
    }
    [view_window setTitle:[NSString stringWithUTF8String: view_title]];
    [view_window setAcceptsMouseMovedEvents: YES];
    view_osx_size_window(view_window, view_width, view_height);
    [view_window setRestorable: YES];
    [view_window setDelegate: view_window_delegate];

    view_mtl_device = MTLCreateSystemDefaultDevice();
    view_mtk_view = [[ViewMTKView alloc] init];
    [view_mtk_view setDevice: view_mtl_device];
    [view_mtk_view setColorPixelFormat: MTLPixelFormatBGRA8Unorm];
    [view_mtk_view setDepthStencilPixelFormat: MTLPixelFormatInvalid];
    // On demand by default: the view sleeps until view_request_frame()
    // invalidates it. NS_VIEW_CONTINUOUS or view_set_frame_per_second switches
    // to vsync-paced drawing so a GPU frame capture has a boundary to arm on.
    view_osx_apply_frame_rate();
    // Trackpad fingers are only delivered as touches once the view opts in.
    // Magnify and rotate arrive without it; the three-finger orbit does not.
    [view_mtk_view setAllowedTouchTypes:NSTouchTypeMaskIndirect];
    view_mtk_view_delegate = [[ViewDelegate alloc] init];
    [view_mtk_view setDelegate: view_mtk_view_delegate];
    [view_window setContentView: view_mtk_view];
    if (view_osx_hosted) {
        // Hosted applications are already running and do not receive this
        // backend's applicationDidFinishLaunching callback.
        view_osx_activate_window();
    } else {
        [view_window orderFront:nil];
    }
    const NSRect content_rect = [view_window contentRectForFrameRect:[view_window frame]];
    view_osx_sync_metrics(content_rect.size.width, content_rect.size.height);

    _view.native_window = (__bridge void*)view_window;
    _view.gpu_device = (__bridge void*)view_mtl_device;
    view_apple_gamepad_start(&_view);
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
    _view.title = view_keep_title(title);
    view_osx_finished = false;
    view_osx_done = dispatch_semaphore_create(0);
    void (^create_view)(void) = ^{ view_osx_create(width, height, (const char *)_view.title.data); };
    if ([NSThread isMainThread]) create_view();
    else dispatch_sync(dispatch_get_main_queue(), create_view);
    return &_view;
}

view* view_create_no_title(const char *title, i32 width, i32 height) {
    return view_create(title, width, height);
}

void view_run(view *v) {
    if (!v) return;
    if (view_osx_hosted) {
        view_on_launch launch = (view_on_launch)v->on_launch;
        if (launch) launch(v);
        view_request_frame(v, 1);
        view_osx_request_activation();
        dispatch_semaphore_wait(view_osx_done, DISPATCH_TIME_FOREVER);
        return;
    }
    // finishLaunching delivers applicationDidFinishLaunching while the
    // application's callbacks are already installed. Reassert activation on
    // the next main-loop turn because terminal-launched processes otherwise
    // commonly leave Terminal as the active application.
    [NSApp finishLaunching];
    view_osx_activate_window();
    view_request_frame(v, 1);
    // The first frames may already have hidden the window on purpose (an
    // overlay stepping aside for System Settings); do not bring it back.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (view_window && view_window.isVisible) view_osx_activate_window();
    });
    [NSApp run];
}

ns_bool view_is_active(view *v) {
    if (!v) return false;
    __block BOOL active = NO;
    void (^read)(void) = ^{
        active = [NSApp isActive] && [view_window isKeyWindow] &&
                 [view_window isVisible] && ![view_window isMiniaturized] &&
                 ([view_window occlusionState] & NSWindowOcclusionStateVisible) != 0;
    };
    if ([NSThread isMainThread]) read();
    else dispatch_sync(dispatch_get_main_queue(), read);
    return active;
}

void view_platform_request_frame(view *v) {
    (void)v;
    if (!view_mtk_view) return;
    void (^request)(void) = ^{ [view_mtk_view setNeedsDisplay:YES]; };
    if ([NSThread isMainThread]) request();
    else dispatch_async(dispatch_get_main_queue(), request);
}

// The close button's own path: publish the terminate callback, then let AppKit
// unwind [NSApp run] so view_run() returns.
//
// It is always deferred to the next main-loop turn, never run inline. A program
// closes itself from inside its frame callback, and that callback is exactly
// where the resources on_terminate releases are still in use - tearing down the
// GPU device from within a draw would take the renderer out from under the
// frame that asked. Waiting one turn lets that frame finish first.
void view_platform_close(view *v) {
    ns_unused(v);
    dispatch_async(dispatch_get_main_queue(), ^{
        view_osx_finish();
        view_osx_request_terminate();
    });
}

// AppKit has no public way to rename a running application; the Dock reads
// the name LaunchServices holds for the process. These private entry points are
// looked up by name, so a system without them leaves the bundle's name.
typedef CFTypeRef (*view_osx_ls_asn_fn)(void);
typedef OSStatus (*view_osx_ls_set_fn)(int, CFTypeRef, CFStringRef, CFStringRef, CFDictionaryRef *);

static void view_osx_set_display_name(NSString *name) {
    CFBundleRef services = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.LaunchServices"));
    if (!services) return;
    view_osx_ls_asn_fn current_asn =
        (view_osx_ls_asn_fn)CFBundleGetFunctionPointerForName(services, CFSTR("_LSGetCurrentApplicationASN"));
    view_osx_ls_set_fn set_item =
        (view_osx_ls_set_fn)CFBundleGetFunctionPointerForName(services, CFSTR("_LSSetApplicationInformationItem"));
    CFStringRef *display_name_key =
        (CFStringRef *)CFBundleGetDataPointerForName(services, CFSTR("_kLSDisplayNameKey"));
    if (!current_asn || !set_item || !display_name_key || !*display_name_key) return;
    CFTypeRef asn = current_asn();
    if (!asn) return;
    // -2 is kLSDefaultSessionID.
    set_item(-2, asn, *display_name_key, (__bridge CFStringRef)name, NULL);
}

void view_platform_set_app_name(view *v, const char *name) {
    ns_unused(v);
    NSString *value = [NSString stringWithUTF8String:name ? name : ""];
    if (!value || [value length] == 0) return;
    void (^apply)(void) = ^{ view_osx_set_display_name(value); };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

// macOS 15 has the diagonal frame-resize cursors; earlier systems fall back
// to the private ones AppKit itself uses, then to the arrow.
static NSCursor* view_osx_diagonal_cursor(ns_bool nwse) {
    if (@available(macOS 15.0, *)) {
        return [NSCursor frameResizeCursorFromPosition:nwse ? NSCursorFrameResizePositionTopLeft : NSCursorFrameResizePositionTopRight
                                          inDirections:NSCursorFrameResizeDirectionsAll];
    }
    SEL legacy = nwse ? NSSelectorFromString(@"_windowResizeNorthWestSouthEastCursor")
                      : NSSelectorFromString(@"_windowResizeNorthEastSouthWestCursor");
    if ([NSCursor respondsToSelector:legacy]) {
        NSCursor* (*call)(id, SEL) = (NSCursor* (*)(id, SEL))[NSCursor methodForSelector:legacy];
        return call([NSCursor class], legacy);
    }
    return [NSCursor arrowCursor];
}

static NSCursor* view_osx_cursor_for(i32 cursor) {
    switch (cursor) {
        case VIEW_CURSOR_TEXT: return [NSCursor IBeamCursor];
        case VIEW_CURSOR_POINTER: return [NSCursor pointingHandCursor];
        case VIEW_CURSOR_RESIZE_EW: return [NSCursor resizeLeftRightCursor];
        case VIEW_CURSOR_RESIZE_NS: return [NSCursor resizeUpDownCursor];
        case VIEW_CURSOR_RESIZE_NWSE: return view_osx_diagonal_cursor(true);
        case VIEW_CURSOR_RESIZE_NESW: return view_osx_diagonal_cursor(false);
        case VIEW_CURSOR_MOVE: return [NSCursor openHandCursor];
        default: return nil;
    }
}

void view_platform_set_cursor(view *v, i32 cursor) {
    ns_unused(v);
    void (^apply)(void) = ^{
        view_osx_cursor = view_osx_cursor_for(cursor);
        if (view_window && view_mtk_view) [view_window invalidateCursorRectsForView:view_mtk_view];
        NSPoint at = view_window ? [view_window mouseLocationOutsideOfEventStream] : NSZeroPoint;
        if (view_mtk_view && NSPointInRect([view_mtk_view convertPoint:at fromView:nil], [view_mtk_view bounds])) {
            [(view_osx_cursor ? view_osx_cursor : [NSCursor arrowCursor]) set];
        }
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

void view_platform_set_title(view *v, const char *title) {
    ns_unused(v);
    NSString *value = [NSString stringWithUTF8String:title ? title : ""];
    if (!value) return;
    void (^apply)(void) = ^{ if (view_window) [view_window setTitle:value]; };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

void view_platform_set_fullscreen(view *v, ns_bool fullscreen) {
    ns_unused(v);
    if (!view_window) return;
    void (^apply)(void) = ^{
        ns_bool is_fullscreen = ([view_window styleMask] & NSWindowStyleMaskFullScreen) != 0;
        if ((fullscreen != 0) != is_fullscreen) [view_window toggleFullScreen:nil];
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

void view_platform_request_frame_after(view *v, i32 milliseconds) {
    (void)v;
    if (!view_mtk_view) return;
    // Input can render before an idle timer fires. Keep one earliest wake,
    // otherwise every input frame seeds another perpetual polling chain.
    void (^schedule)(void) = ^{
        static NSTimeInterval deadline;
        static uint64_t generation;
        NSTimeInterval next = [NSProcessInfo processInfo].systemUptime + milliseconds / 1000.0;
        if (deadline > 0 && deadline <= next) return;
        deadline = next;
        uint64_t token = ++generation;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)milliseconds * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            if (token != generation) return;
            deadline = 0;
            view_request_frame(&_view, 1);
        });
    };
    if ([NSThread isMainThread]) schedule();
    else dispatch_async(dispatch_get_main_queue(), schedule);
}

void view_platform_set_frame_per_second(view *v, i32 frames) {
    (void)v;
    (void)frames;
    void (^apply)(void) = ^{ view_osx_apply_frame_rate(); };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

static char *view_clipboard_result;

const char *view_get_clipboard(view *v) {
    ns_unused(v);
    __block NSString *value = nil;
    void (^read)(void) = ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        value = [pasteboard stringForType:NSPasteboardTypeString];
    };
    if ([NSThread isMainThread]) read();
    else dispatch_sync(dispatch_get_main_queue(), read);
    if (!value) return ns_null;
    const char *utf8 = [value UTF8String];
    if (!utf8) return ns_null;
    size_t len = strlen(utf8);
    view_clipboard_result = ns_realloc(view_clipboard_result, len + 1);
    if (!view_clipboard_result) return ns_null;
    memcpy(view_clipboard_result, utf8, len + 1);
    return view_clipboard_result;
}

void view_set_clipboard(view *v, const char *text) {
    ns_unused(v);
    NSString *value = [NSString stringWithUTF8String:text ? text : ""];
    if (!value) return;
    void (^write)(void) = ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        [pasteboard setString:value forType:NSPasteboardTypeString];
    };
    if ([NSThread isMainThread]) write();
    else dispatch_sync(dispatch_get_main_queue(), write);
}
#endif
