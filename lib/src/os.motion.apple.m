#include "os.h"

#include <TargetConditionals.h>

#if TARGET_OS_IOS && __has_include(<CoreMotion/CoreMotion.h>)

#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>

#include <math.h>

// One shared manager in pull mode: CoreMotion fuses the gyroscope with the
// accelerometer on its own thread and os_motion_attitude reads the latest
// sample, so no callback ever enters the VM.
static CMMotionManager *os_motion_manager;

ns_bool os_motion_start(f64 hz) {
    @autoreleasepool {
        @synchronized([CMMotionManager class]) {
            if (!os_motion_manager) os_motion_manager = [[CMMotionManager alloc] init];
            if (![os_motion_manager isDeviceMotionAvailable]) return false;
            if (!isfinite(hz) || hz < 10.0) hz = 10.0;
            if (hz > 120.0) hz = 120.0;
            [os_motion_manager setDeviceMotionUpdateInterval:1.0 / hz];
            if (![os_motion_manager isDeviceMotionActive]) {
                [os_motion_manager startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXArbitraryZVertical];
            }
            return true;
        }
    }
}

void os_motion_stop(void) {
    @synchronized([CMMotionManager class]) {
        if (os_motion_manager && [os_motion_manager isDeviceMotionActive]) [os_motion_manager stopDeviceMotionUpdates];
    }
}

ns_bool os_motion_attitude(f64 *values) {
    if (!values) return false;
    @autoreleasepool {
        @synchronized([CMMotionManager class]) {
            if (!os_motion_manager || ![os_motion_manager isDeviceMotionActive]) return false;
            CMDeviceMotion *motion = [os_motion_manager deviceMotion];
            if (!motion) return false;
            CMQuaternion q = [[motion attitude] quaternion];
            values[0] = q.x;
            values[1] = q.y;
            values[2] = q.z;
            values[3] = q.w;
            return true;
        }
    }
}

#elif defined(__APPLE__)

ns_bool os_motion_start(f64 hz) {
    ns_unused(hz);
    return false;
}

void os_motion_stop(void) {}

ns_bool os_motion_attitude(f64 *values) {
    ns_unused(values);
    return false;
}

#endif
