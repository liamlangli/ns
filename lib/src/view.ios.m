#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <string.h>

#include "view.h"

void gpu_mtl_begin_frame(MTKView *view);
void gpu_mtl_end_frame(MTKView *view);

typedef void (*view_callback)(view *);

static view view_ios_state;
static MTKView *view_ios_metal_view;
static id<MTLDevice> view_ios_device;
static id view_ios_delegate;
static dispatch_semaphore_t view_ios_done;
static i32 view_ios_active_touch_count;

static UIWindow *view_ios_key_window(void);
static void view_ios_sync_metrics(MTKView *metal_view);
static UIView *view_ios_host_view;

void view_ios_set_host_view(void *view) {
    view_ios_host_view = (__bridge UIView *)view;
    if (!view_ios_metal_view || !view_ios_host_view) return;
    if (view_ios_metal_view.superview != view_ios_host_view) {
        view_ios_metal_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [view_ios_host_view addSubview:view_ios_metal_view];
    }
    view_ios_metal_view.frame = view_ios_host_view.bounds;
    view_ios_sync_metrics(view_ios_metal_view);
    view_request_frame(&view_ios_state, 1);
}

static void view_ios_apply_frame_rate(void) {
    if (!view_ios_metal_view) return;
    if (view_immersive_status() == 2) {
        view_ios_metal_view.paused = YES;
        view_ios_metal_view.hidden = YES;
        return;
    }
    view_ios_metal_view.hidden = NO;
    BOOL continuous = view_continuous_render() ? YES : NO;
    view_ios_metal_view.paused = continuous ? NO : YES;
    view_ios_metal_view.enableSetNeedsDisplay = continuous ? NO : YES;
    i32 fps = view_frame_per_second();
    view_ios_metal_view.preferredFramesPerSecond = fps > 0 ? fps : 0;
}

static void view_ios_sync_metrics(MTKView *metal_view) {
    if (!metal_view) return;
    CGFloat scale = metal_view.contentScaleFactor;
    if (scale <= 0.0) scale = 1.0;
    CGSize size = metal_view.bounds.size;
    CGSize drawable = metal_view.drawableSize;
    view_ios_state.display_ratio = scale;
    view_ios_state.ui_scale = scale;
    view_ios_state.width = (i32)size.width;
    view_ios_state.height = (i32)size.height;
    // drawableSize is the render-pass extent. Multiplying bounds by the
    // nominal content scale can differ by one pixel on scaled iPad displays.
    view_ios_state.framebuffer_width = (i32)(drawable.width + 0.5);
    view_ios_state.framebuffer_height = (i32)(drawable.height + 0.5);
    // The Metal view fills the window, so its own insets already describe the
    // status bar / dynamic island, the home indicator and the rounded display
    // corners. Fall back to the window when the view is not in a hierarchy yet.
    UIEdgeInsets insets = metal_view.safeAreaInsets;
    if (!metal_view.window) {
        UIWindow *window = view_ios_key_window();
        if (window) insets = window.safeAreaInsets;
    }
    view_set_safe_area(&view_ios_state, insets.top, insets.right, insets.bottom, insets.left);
}

static UIWindow *view_ios_key_window(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *window_scene = (UIWindowScene *)scene;
        for (UIWindow *window in window_scene.windows) {
            if (window.isKeyWindow) return window;
        }
        if (window_scene.windows.count > 0) return window_scene.windows.firstObject;
    }
    return nil;
}

static i32 view_ios_pointer_id(UITouch *touch) {
    return (i32)(touch.hash & 0x7fffffffU);
}

static i32 view_ios_input_device(UITouch *touch) {
    switch (touch.type) {
        case UITouchTypePencil: return VIEW_INPUT_DEVICE_PENCIL;
        case UITouchTypeIndirectPointer: return VIEW_INPUT_DEVICE_INDIRECT;
        default: return VIEW_INPUT_DEVICE_TOUCH;
    }
}

static void view_ios_touch(UITouch *touch, i32 phase) {
    CGPoint point = [touch locationInView:view_ios_metal_view];
    f64 pressure = touch.maximumPossibleForce > 0.0 ? touch.force / touch.maximumPossibleForce : 0.0;
    i32 pointer_id = view_ios_pointer_id(touch);
    view_on_pointer_event(&view_ios_state, view_ios_input_device(touch), phase, pointer_id,
                          point.x, point.y, pressure, touch.altitudeAngle,
                          [touch azimuthAngleInView:view_ios_metal_view], touch.timestamp, 0);
    // The complete pointer queue above identifies every finger. The legacy
    // mouse fields expose an edge for every contact too, while mouse_down stays
    // true until the last held finger leaves the view.
    view_ios_state.mouse_x = point.x;
    view_ios_state.mouse_y = point.y;
    if (phase == VIEW_INPUT_PHASE_BEGAN) {
        view_ios_active_touch_count++;
        view_ios_state.mouse_pressed = true;
    } else if (phase == VIEW_INPUT_PHASE_ENDED || phase == VIEW_INPUT_PHASE_CANCELLED) {
        if (view_ios_active_touch_count > 0) view_ios_active_touch_count--;
        view_ios_state.mouse_released = true;
    }
    view_ios_state.mouse_down = view_ios_active_touch_count > 0;
}

@interface NSIOSMetalView : MTKView
@end

@interface NSIOSViewDelegate : NSObject <MTKViewDelegate>
@end

@implementation NSIOSMetalView
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches) view_ios_touch(touch, VIEW_INPUT_PHASE_BEGAN);
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches) view_ios_touch(touch, VIEW_INPUT_PHASE_MOVED);
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches) view_ios_touch(touch, VIEW_INPUT_PHASE_ENDED);
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *touch in touches) view_ios_touch(touch, VIEW_INPUT_PHASE_CANCELLED);
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    // Insets also move without a drawable resize: the status bar grows during a
    // call, the keyboard docks, an iPad window is split.
    view_ios_sync_metrics(self);
    view_request_frame(&view_ios_state, 1);
}
@end

@implementation NSIOSViewDelegate
- (void)mtkView:(MTKView *)metal_view drawableSizeWillChange:(CGSize)size {
    (void)size;
    view_ios_sync_metrics(metal_view);
    view_request_frame(&view_ios_state, 1);
}
- (void)drawInMTKView:(MTKView *)metal_view {
    if (view_immersive_status() == 2) return;
    if (!view_take_frame_request(&view_ios_state)) return;
    view_ios_sync_metrics(metal_view);
    view_callback frame = (view_callback)view_ios_state.on_frame;
    if (frame) {
        view_apple_gamepad_poll(&view_ios_state);
        gpu_mtl_begin_frame(metal_view);
        frame(&view_ios_state);
        // The frame owns the drawable between these two calls: whatever the
        // frame committed in between, the present happens here and once.
        gpu_mtl_end_frame(metal_view);
    }
    view_complete_frame(&view_ios_state);
}
@end

static void view_ios_camera_pan(UIPanGestureRecognizer *gesture) {
    CGPoint delta = [gesture translationInView:view_ios_metal_view];
    view_on_gesture(&view_ios_state, delta.x, delta.y, 1.0, 0.0);
    [gesture setTranslation:CGPointZero inView:view_ios_metal_view];
}

static void view_ios_orbit(UIPanGestureRecognizer *gesture) {
    CGPoint delta = [gesture translationInView:view_ios_metal_view];
    view_on_orbit_gesture(&view_ios_state, delta.x, delta.y);
    [gesture setTranslation:CGPointZero inView:view_ios_metal_view];
}

static void view_ios_pinch(UIPinchGestureRecognizer *gesture) {
    view_on_gesture(&view_ios_state, 0.0, 0.0, gesture.scale, 0.0);
    gesture.scale = 1.0;
}

static void view_ios_rotate(UIRotationGestureRecognizer *gesture) {
    view_on_gesture(&view_ios_state, 0.0, 0.0, 1.0, gesture.rotation);
    gesture.rotation = 0.0;
}

@interface NSIOSGestureTarget : NSObject <UIGestureRecognizerDelegate>
- (void)cameraPan:(UIPanGestureRecognizer *)gesture;
- (void)orbit:(UIPanGestureRecognizer *)gesture;
- (void)pinch:(UIPinchGestureRecognizer *)gesture;
- (void)rotate:(UIRotationGestureRecognizer *)gesture;
- (void)captureFrame:(UITapGestureRecognizer *)gesture;
@end

@implementation NSIOSGestureTarget
- (void)cameraPan:(UIPanGestureRecognizer *)gesture { view_ios_camera_pan(gesture); }
- (void)orbit:(UIPanGestureRecognizer *)gesture { view_ios_orbit(gesture); }
- (void)pinch:(UIPinchGestureRecognizer *)gesture { view_ios_pinch(gesture); }
- (void)rotate:(UIRotationGestureRecognizer *)gesture { view_ios_rotate(gesture); }
// Four fingers at once: the device has no F12, so this is how a GPU capture is
// asked for on a phone. The frame request is what actually gives the capture
// something to record - an idle on-demand view would otherwise arm and wait.
- (void)captureFrame:(UITapGestureRecognizer *)gesture {
    (void)gesture;
    view_capture_require(&view_ios_state);
    view_request_frame(&view_ios_state, 1);
}
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture
        shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)other {
    (void)gesture;
    (void)other;
    return YES;
}
@end

static NSIOSGestureTarget *view_ios_gesture_target;

static void view_ios_add_gesture(UIGestureRecognizer *gesture) {
    // The raw pointer stream drives controls such as a held stick plus a jump
    // button. UIKit otherwise delays touchesEnded by default while any gesture
    // recognizer is still analyzing the multi-touch sequence, which leaves the
    // second control held until the first finger also lifts.
    gesture.cancelsTouchesInView = NO;
    gesture.delaysTouchesEnded = NO;
    gesture.delegate = view_ios_gesture_target;
    [view_ios_metal_view addGestureRecognizer:gesture];
}

view *view_create(const char *title, i32 width, i32 height) {
    memset(&view_ios_state, 0, sizeof(view_ios_state));
    view_ios_active_touch_count = 0;
    view_ios_state.title = ns_str_cstr((char *)(title ? title : ""));
    view_ios_state.width = width;
    view_ios_state.height = height;
    view_ios_state.display_ratio = 1.0;
    view_ios_state.ui_scale = view_ios_state.display_ratio;
    view_ios_done = dispatch_semaphore_create(0);

    void (^create_view)(void) = ^{
        UIView *container = view_ios_host_view;
        if (!container) {
            UIWindow *window = view_ios_key_window();
            if (window) container = window.rootViewController.view ?: window;
        }
        if (!container) return;
        view_ios_device = MTLCreateSystemDefaultDevice();
        view_ios_metal_view = [[NSIOSMetalView alloc] initWithFrame:container.bounds device:view_ios_device];
        // UIView defaults this to NO, which makes UIKit deliver only the first
        // finger even though the touch callbacks and the view event queue both
        // preserve a pointer id. Games need a held stick and an action button
        // to remain independent contacts.
        view_ios_metal_view.multipleTouchEnabled = YES;
        view_ios_metal_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        view_ios_metal_view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view_ios_metal_view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        // On demand by default: the view sleeps until view_request_frame()
        // invalidates it. NS_VIEW_CONTINUOUS or view_set_frame_per_second
        // switches to vsync-paced drawing so a GPU frame capture has a
        // boundary to arm on.
        view_ios_apply_frame_rate();
        view_ios_delegate = [[NSIOSViewDelegate alloc] init];
        view_ios_metal_view.delegate = view_ios_delegate;
        view_ios_gesture_target = [[NSIOSGestureTarget alloc] init];
        UIPanGestureRecognizer *orbit = [[UIPanGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(orbit:)];
        orbit.minimumNumberOfTouches = 1;
        orbit.maximumNumberOfTouches = 1;
        view_ios_add_gesture(orbit);
        UIPanGestureRecognizer *camera_pan = [[UIPanGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(cameraPan:)];
        camera_pan.minimumNumberOfTouches = 2;
        camera_pan.maximumNumberOfTouches = 2;
        view_ios_add_gesture(camera_pan);
        UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(pinch:)];
        view_ios_add_gesture(pinch);
        UIRotationGestureRecognizer *rotate = [[UIRotationGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(rotate:)];
        view_ios_add_gesture(rotate);
        UITapGestureRecognizer *capture = [[UITapGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(captureFrame:)];
        capture.numberOfTouchesRequired = 4;
        capture.numberOfTapsRequired = 1;
        view_ios_add_gesture(capture);
        [container addSubview:view_ios_metal_view];
        view_ios_sync_metrics(view_ios_metal_view);
        view_ios_state.native_window = (__bridge void *)view_ios_metal_view;
        view_ios_state.gpu_device = (__bridge void *)view_ios_device;
        view_apple_gamepad_start(&view_ios_state);
    };
    if (NSThread.isMainThread) create_view(); else dispatch_sync(dispatch_get_main_queue(), create_view);
    return &view_ios_state;
}

view *view_create_no_title(const char *title, i32 width, i32 height) {
    return view_create(title, width, height);
}

void view_run(view *value) {
    if (!value) return;
    view_callback launch = (view_callback)value->on_launch;
    if (launch) launch(value);
    view_request_frame(value, 1);
    dispatch_semaphore_wait(view_ios_done, DISPATCH_TIME_FOREVER);
    view_callback terminate = (view_callback)value->on_terminate;
    if (terminate) terminate(value);
}

// iOS applications do not exit themselves, so this only releases view_run(),
// which publishes on_terminate and lets the program finish its own teardown.
void view_platform_close(view *value) {
    ns_unused(value);
    if (view_ios_done) dispatch_semaphore_signal(view_ios_done);
}

void view_platform_request_frame(view *value) {
    ns_unused(value);
    if (!view_ios_metal_view) return;
    dispatch_async(dispatch_get_main_queue(), ^{ [view_ios_metal_view setNeedsDisplay]; });
}

void view_platform_request_frame_after(view *value, i32 milliseconds) {
    ns_unused(value);
    if (!view_ios_metal_view) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)milliseconds * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ view_request_frame(&view_ios_state, 1); });
}

void view_platform_set_frame_per_second(view *value, i32 frames) {
    ns_unused(value);
    ns_unused(frames);
    void (^apply)(void) = ^{ view_ios_apply_frame_rate(); };
    if (NSThread.isMainThread) apply(); else dispatch_async(dispatch_get_main_queue(), apply);
}

const char *view_get_clipboard(view *value) {
    ns_unused(value);
    __block NSString *text = nil;
    void (^read)(void) = ^{ text = UIPasteboard.generalPasteboard.string; };
    if (NSThread.isMainThread) read(); else dispatch_sync(dispatch_get_main_queue(), read);
    return text ? text.UTF8String : ns_null;
}

void view_set_clipboard(view *value, const char *text) {
    ns_unused(value);
    NSString *string = [NSString stringWithUTF8String:text ? text : ""];
    dispatch_async(dispatch_get_main_queue(), ^{ UIPasteboard.generalPasteboard.string = string; });
}

#endif

#if defined(TARGET_OS_VISION) && TARGET_OS_VISION
void gpu_mtl_immersive_begin(id<MTLCommandBuffer>, id<MTLTexture>, id<MTLTexture>, u32);
void gpu_mtl_immersive_end(void);
void gpu_mtl_immersive_complete(void);

// Called by the Swift compositor exclusively on the main thread. The Metal
// window and immersive callbacks therefore never execute application code at
// the same time, including while opening or dismissing the immersive space.
void ns_immersive_render_eye(void * _Nonnull buffer, void * _Nonnull color, void * _Nonnull depth, int eye, const float * _Nonnull pose, int slice) {
    view_immersive_host_pose(eye, pose);
    id<MTLTexture> target = (__bridge id<MTLTexture>)color;
    view_ios_state.framebuffer_width = (i32)target.width;
    view_ios_state.framebuffer_height = (i32)target.height;
    view_ios_state.width = (i32)target.width;
    view_ios_state.height = (i32)target.height;
    view_ios_state.display_ratio = 1;
    view_ios_state.ui_scale = 1;
    view_set_safe_area(&view_ios_state, 0, 0, 0, 0);
    if (eye == 0) view_apple_gamepad_poll(&view_ios_state);
    gpu_mtl_immersive_begin((__bridge id<MTLCommandBuffer>)buffer, target, (__bridge id<MTLTexture>)depth, slice >= 0 ? (u32)slice : 0);
    view_callback frame = (view_callback)view_ios_state.on_frame;
    if (frame) frame(&view_ios_state);
    gpu_mtl_immersive_end();
}
void ns_immersive_complete(void) { gpu_mtl_immersive_complete(); }
void ns_immersive_state(int status) {
    view_immersive_host_status(status);
    if (status != 2) {
        view_ios_sync_metrics(view_ios_metal_view);
        view_input_reset(&view_ios_state);
    }
    view_ios_apply_frame_rate();
    view_request_frame(&view_ios_state, 2);
}
void ns_immersive_pointer(double x, double y, int phase) {
    view_ios_state.mouse_x = x * view_ios_state.width;
    view_ios_state.mouse_y = y * view_ios_state.height;
    view_ios_state.mouse_pressed = phase == 0;
    view_ios_state.mouse_released = phase == 2;
    view_ios_state.mouse_down = phase != 2;
}
#endif
