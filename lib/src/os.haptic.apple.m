#include "os.h"

#include <TargetConditionals.h>

#if __has_include(<CoreHaptics/CoreHaptics.h>) && \
    !(defined(TARGET_OS_VISION) && TARGET_OS_VISION) && !TARGET_OS_WATCH

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>

#include <math.h>

#if __has_feature(objc_arc)
#define OS_HAPTIC_RETAIN(object) (object)
#define OS_HAPTIC_RELEASE(object) ((void)0)
#else
#define OS_HAPTIC_RETAIN(object) [(object) retain]
#define OS_HAPTIC_RELEASE(object) [(object) release]
#endif

static CHHapticEngine *os_haptic_engine;
static id<CHHapticPatternPlayer> os_haptic_player;

void os_vibrate(f64 intensity, f64 duration) {
    if (!isfinite(intensity) || !isfinite(duration) || !(intensity > 0.0) || !(duration > 0.0)) return;
    if (intensity > 1.0) intensity = 1.0;

    @autoreleasepool {
        id<CHHapticDeviceCapability> capability = [CHHapticEngine capabilitiesForHardware];
        if (![capability supportsHaptics]) return;

        @synchronized([CHHapticEngine class]) {
            NSError *error = NULL;
            if (!os_haptic_engine) {
                os_haptic_engine = [[CHHapticEngine alloc] initAndReturnError:&error];
                if (!os_haptic_engine) return;
                [os_haptic_engine setPlaysHapticsOnly:YES];
                [os_haptic_engine setAutoShutdownEnabled:YES];
            }

            if (os_haptic_player) {
                [os_haptic_player stopAtTime:CHHapticTimeImmediate error:NULL];
                OS_HAPTIC_RELEASE(os_haptic_player);
                os_haptic_player = NULL;
            }

            CHHapticEventParameter *strength = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticIntensity
                              value:(float)intensity];
            CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticSharpness
                              value:0.5f];
            CHHapticEvent *event = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                       parameters:@[strength, sharpness]
                     relativeTime:0.0
                         duration:duration];
            CHHapticPattern *pattern = [[CHHapticPattern alloc]
                initWithEvents:@[event]
                    parameters:@[]
                         error:&error];
            id<CHHapticPatternPlayer> player = pattern
                ? [os_haptic_engine createPlayerWithPattern:pattern error:&error]
                : NULL;

            if (player && [os_haptic_engine startAndReturnError:&error] &&
                [player startAtTime:CHHapticTimeImmediate error:&error]) {
                os_haptic_player = OS_HAPTIC_RETAIN(player);
            }

            OS_HAPTIC_RELEASE(pattern);
            OS_HAPTIC_RELEASE(event);
            OS_HAPTIC_RELEASE(sharpness);
            OS_HAPTIC_RELEASE(strength);
        }
    }
}

#else

void os_vibrate(f64 intensity, f64 duration) {
    ns_unused(intensity);
    ns_unused(duration);
}

#endif
