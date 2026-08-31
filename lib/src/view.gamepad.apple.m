#include <TargetConditionals.h>
#if TARGET_OS_OSX || TARGET_OS_IOS || TARGET_OS_TV || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)

#import <GameController/GameController.h>

#include "view.h"

static view *view_apple_gamepad_view;
static id view_apple_gamepad_connect_observer;
static id view_apple_gamepad_disconnect_observer;

static void view_apple_gamepad_watch(GCController *controller) {
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
        (void)gamepad;
        (void)element;
        view_request_frame(view_apple_gamepad_view, 1);
    };
    controller.microGamepad.valueChangedHandler = ^(GCMicroGamepad *gamepad, GCControllerElement *element) {
        (void)gamepad;
        (void)element;
        view_request_frame(view_apple_gamepad_view, 1);
    };
}

void view_apple_gamepad_start(view *v) {
    view_apple_gamepad_view = v;
    for (GCController *controller in GCController.controllers) view_apple_gamepad_watch(controller);
    if (view_apple_gamepad_connect_observer) return;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    view_apple_gamepad_connect_observer =
        [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                       usingBlock:^(NSNotification *note) {
                           view_apple_gamepad_watch((GCController *)note.object);
                           view_request_frame(view_apple_gamepad_view, 1);
                       }];
    view_apple_gamepad_disconnect_observer =
        [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                       usingBlock:^(NSNotification *note) {
                           (void)note;
                           view_request_frame(view_apple_gamepad_view, 1);
                       }];
}

static void view_apple_gamepad_button(view *v, i32 slot, i32 button, GCControllerButtonInput *input) {
    if (!input) {
        view_on_gamepad_button(v, slot, button, 0.0f, false);
        return;
    }
    view_on_gamepad_button(v, slot, button, input.value, input.isPressed);
}

void view_apple_gamepad_poll(view *v) {
    NSArray<GCController *> *controllers = GCController.controllers;
    for (i32 slot = 0; slot < VIEW_GAMEPAD_CAPACITY; slot++) {
        if (slot >= (i32)controllers.count) {
            view_on_gamepad_connected(v, slot, false);
            continue;
        }
        GCController *controller = controllers[(NSUInteger)slot];
        GCExtendedGamepad *pad = controller.extendedGamepad;
        GCMicroGamepad *micro = controller.microGamepad;
        if (!pad && !micro) {
            view_on_gamepad_connected(v, slot, false);
            continue;
        }
        view_on_gamepad_connected(v, slot, true);
        if (pad) {
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_LEFT_X, pad.leftThumbstick.xAxis.value);
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_LEFT_Y, -pad.leftThumbstick.yAxis.value);
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_RIGHT_X, pad.rightThumbstick.xAxis.value);
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_RIGHT_Y, -pad.rightThumbstick.yAxis.value);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_SOUTH, pad.buttonA);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_EAST, pad.buttonB);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_WEST, pad.buttonX);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_NORTH, pad.buttonY);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_LEFT_SHOULDER, pad.leftShoulder);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_RIGHT_SHOULDER, pad.rightShoulder);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_LEFT_TRIGGER, pad.leftTrigger);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_RIGHT_TRIGGER, pad.rightTrigger);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_SELECT, pad.buttonOptions);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_START, pad.buttonMenu);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_LEFT_STICK, pad.leftThumbstickButton);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_RIGHT_STICK, pad.rightThumbstickButton);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_UP, pad.dpad.up);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_DOWN, pad.dpad.down);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_LEFT, pad.dpad.left);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_RIGHT, pad.dpad.right);
            if (@available(macOS 11.0, iOS 14.0, tvOS 14.0, *)) {
                view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_HOME, pad.buttonHome);
            } else {
                view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_HOME, nil);
            }
        } else {
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_LEFT_X, micro.dpad.xAxis.value);
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_LEFT_Y, -micro.dpad.yAxis.value);
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_RIGHT_X, 0.0f);
            view_on_gamepad_axis(v, slot, VIEW_GAMEPAD_AXIS_RIGHT_Y, 0.0f);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_SOUTH, micro.buttonA);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_EAST, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_WEST, micro.buttonX);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_NORTH, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_LEFT_SHOULDER, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_RIGHT_SHOULDER, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_LEFT_TRIGGER, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_RIGHT_TRIGGER, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_SELECT, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_START, micro.buttonMenu);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_LEFT_STICK, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_RIGHT_STICK, nil);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_UP, micro.dpad.up);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_DOWN, micro.dpad.down);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_LEFT, micro.dpad.left);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_DPAD_RIGHT, micro.dpad.right);
            view_apple_gamepad_button(v, slot, VIEW_GAMEPAD_BUTTON_HOME, nil);
        }
    }
}

#endif
