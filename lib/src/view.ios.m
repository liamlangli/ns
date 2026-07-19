#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <string.h>

#include "view.h"

void gpu_mtl_begin_frame(MTKView *view);

typedef void (*view_callback)(view *);

static view view_ios_state;
static MTKView *view_ios_metal_view;
static id<MTLDevice> view_ios_device;
static id view_ios_delegate;
static dispatch_semaphore_t view_ios_done;

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
    view_on_pointer_event(&view_ios_state, view_ios_input_device(touch), phase, view_ios_pointer_id(touch),
                          point.x, point.y, pressure, touch.altitudeAngle,
                          [touch azimuthAngleInView:view_ios_metal_view], touch.timestamp, 0);
    view_ios_state.mouse_x = point.x;
    view_ios_state.mouse_y = point.y;
    if (phase == VIEW_INPUT_PHASE_BEGAN) {
        view_ios_state.mouse_down = true;
        view_ios_state.mouse_pressed = true;
    } else if (phase == VIEW_INPUT_PHASE_ENDED || phase == VIEW_INPUT_PHASE_CANCELLED) {
        view_ios_state.mouse_down = false;
        view_ios_state.mouse_released = true;
    }
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
@end

@implementation NSIOSViewDelegate
- (void)mtkView:(MTKView *)metal_view drawableSizeWillChange:(CGSize)size {
    (void)size;
    view_ios_sync_metrics(metal_view);
    view_request_frame(&view_ios_state, 1);
}
- (void)drawInMTKView:(MTKView *)metal_view {
    if (!view_take_frame_request(&view_ios_state)) return;
    view_ios_sync_metrics(metal_view);
    view_callback frame = (view_callback)view_ios_state.on_frame;
    if (frame) {
        gpu_mtl_begin_frame(metal_view);
        frame(&view_ios_state);
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
@end

@implementation NSIOSGestureTarget
- (void)cameraPan:(UIPanGestureRecognizer *)gesture { view_ios_camera_pan(gesture); }
- (void)orbit:(UIPanGestureRecognizer *)gesture { view_ios_orbit(gesture); }
- (void)pinch:(UIPinchGestureRecognizer *)gesture { view_ios_pinch(gesture); }
- (void)rotate:(UIRotationGestureRecognizer *)gesture { view_ios_rotate(gesture); }
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture
        shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)other {
    (void)gesture;
    (void)other;
    return YES;
}
@end

static NSIOSGestureTarget *view_ios_gesture_target;

view *view_create(const char *title, i32 width, i32 height) {
    memset(&view_ios_state, 0, sizeof(view_ios_state));
    view_ios_state.title = ns_str_cstr((char *)(title ? title : ""));
    view_ios_state.width = width;
    view_ios_state.height = height;
    view_ios_state.display_ratio = 1.0;
    view_ios_state.ui_scale = view_ios_state.display_ratio;
    view_ios_done = dispatch_semaphore_create(0);

    void (^create_view)(void) = ^{
        UIWindow *window = view_ios_key_window();
        if (!window) return;
        UIView *container = window;
        view_ios_device = MTLCreateSystemDefaultDevice();
        view_ios_metal_view = [[NSIOSMetalView alloc] initWithFrame:container.bounds device:view_ios_device];
        view_ios_metal_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        view_ios_metal_view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view_ios_metal_view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        view_ios_metal_view.paused = YES;
        view_ios_metal_view.enableSetNeedsDisplay = YES;
        view_ios_delegate = [[NSIOSViewDelegate alloc] init];
        view_ios_metal_view.delegate = view_ios_delegate;
        view_ios_gesture_target = [[NSIOSGestureTarget alloc] init];
        UIPanGestureRecognizer *orbit = [[UIPanGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(orbit:)];
        orbit.minimumNumberOfTouches = 1;
        orbit.maximumNumberOfTouches = 1;
        orbit.cancelsTouchesInView = NO;
        orbit.delegate = view_ios_gesture_target;
        [view_ios_metal_view addGestureRecognizer:orbit];
        UIPanGestureRecognizer *camera_pan = [[UIPanGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(cameraPan:)];
        camera_pan.minimumNumberOfTouches = 2;
        camera_pan.maximumNumberOfTouches = 2;
        camera_pan.cancelsTouchesInView = NO;
        camera_pan.delegate = view_ios_gesture_target;
        [view_ios_metal_view addGestureRecognizer:camera_pan];
        UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(pinch:)];
        pinch.cancelsTouchesInView = NO;
        pinch.delegate = view_ios_gesture_target;
        [view_ios_metal_view addGestureRecognizer:pinch];
        UIRotationGestureRecognizer *rotate = [[UIRotationGestureRecognizer alloc] initWithTarget:view_ios_gesture_target action:@selector(rotate:)];
        rotate.cancelsTouchesInView = NO;
        rotate.delegate = view_ios_gesture_target;
        [view_ios_metal_view addGestureRecognizer:rotate];
        [container addSubview:view_ios_metal_view];
        view_ios_sync_metrics(view_ios_metal_view);
        view_ios_state.native_window = (__bridge void *)view_ios_metal_view;
        view_ios_state.gpu_device = (__bridge void *)view_ios_device;
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
