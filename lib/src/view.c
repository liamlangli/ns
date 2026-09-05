#include "view.h"

#include <stdlib.h>

static ns_bool view_keys[VIEW_KEY_MENU + 1];
// Stored as modifiers + 1 so zero remains the no-event sentinel.
static i32 view_key_presses[VIEW_KEY_MENU + 1];
static ns_bool view_gamepads[VIEW_GAMEPAD_CAPACITY];
static f32 view_gamepad_axes[VIEW_GAMEPAD_CAPACITY][VIEW_GAMEPAD_AXIS_CAPACITY];
static f32 view_gamepad_buttons[VIEW_GAMEPAD_CAPACITY][VIEW_GAMEPAD_BUTTON_CAPACITY];
static ns_bool view_gamepad_buttons_down[VIEW_GAMEPAD_CAPACITY][VIEW_GAMEPAD_BUTTON_CAPACITY];
static ns_bool view_gamepad_button_presses[VIEW_GAMEPAD_CAPACITY][VIEW_GAMEPAD_BUTTON_CAPACITY];

#define VIEW_INPUT_CAPACITY 512
#define VIEW_POINTER_SLOTS 32
static view_input_event view_events[VIEW_INPUT_CAPACITY];
static i32 view_event_count;
static view_gesture_state view_gestures = {.zoom_factor = 1.0};
static f64 view_pointer_x[VIEW_POINTER_SLOTS];
static f64 view_pointer_y[VIEW_POINTER_SLOTS];
static ns_bool view_pointer_known[VIEW_POINTER_SLOTS];
static i32 view_requested_frames;
static i32 view_fps;

void view_request_frame(view *v, i32 frames) {
    if (!v || frames <= 0) return;
    ns_bool was_idle = view_requested_frames <= 0;
    if (frames > view_requested_frames) view_requested_frames = frames;
    if (was_idle) view_platform_request_frame(v);
}

void view_request_frame_after(view *v, i32 milliseconds) {
    if (!v) return;
    if (milliseconds <= 0) {
        view_request_frame(v, 1);
        return;
    }
    view_platform_request_frame_after(v, milliseconds);
}

i32 view_frame_per_second(void) {
    return view_fps;
}

void view_set_frame_per_second(view *v, i32 frames) {
    if (frames < 0) frames = 0;
    view_fps = frames;
    view_platform_set_frame_per_second(v, frames);
}

// GPU frame capture arms on a frame boundary and waits for the next present.
// An on-demand backend that is idle never reaches one, so the capture hangs
// with nothing to delimit. Setting NS_VIEW_CONTINUOUS=1 makes the backends
// free-run for the length of a debugging session; read once so the per-frame
// path stays a load. A positive view_set_frame_per_second rate is the same
// kind of vsync-paced loop, at the requested cap.
ns_bool view_continuous_render(void) {
    if (view_fps > 0) return true;
    static i32 enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("NS_VIEW_CONTINUOUS");
        enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return enabled != 0;
}

ns_bool view_take_frame_request(view *v) {
    if (!v) return false;
    if (view_requested_frames > 0) {
        view_requested_frames--;
        return true;
    }
    // Free-running backends draw whether or not anything asked them to, but
    // pending requests above still drain so the app's own pacing is unchanged.
    return view_continuous_render();
}

void view_complete_frame(view *v) {
    if (v && view_requested_frames > 0) view_platform_request_frame(v);
}

static i32 view_key_modifiers(void) {
    i32 mods = 0;
    if (view_keys[VIEW_KEY_LEFT_SHIFT] || view_keys[VIEW_KEY_RIGHT_SHIFT]) mods |= VIEW_KEY_MOD_SHIFT;
    if (view_keys[VIEW_KEY_LEFT_CONTROL] || view_keys[VIEW_KEY_RIGHT_CONTROL]) mods |= VIEW_KEY_MOD_CONTROL;
    if (view_keys[VIEW_KEY_LEFT_ALT] || view_keys[VIEW_KEY_RIGHT_ALT]) mods |= VIEW_KEY_MOD_ALT;
    if (view_keys[VIEW_KEY_LEFT_SUPER] || view_keys[VIEW_KEY_RIGHT_SUPER]) mods |= VIEW_KEY_MOD_SUPER;
    return mods;
}

void view_on_mouse_move(view* v, f64 x, f64 y) {
    if (!v) return;
    v->mouse_x = x;
    v->mouse_y = y;
    // AppKit can redeliver a stationary hover when an on-demand Metal view is
    // invalidated. Do not turn that duplicate into another frame request.
    if (view_pointer_known[0] && view_pointer_x[0] == x && view_pointer_y[0] == y) return;
    ns_bool dragging = v->mouse_down || v->mouse_right_down || v->mouse_middle_down;
    view_on_pointer_event(v, VIEW_INPUT_DEVICE_MOUSE,
                          dragging ? VIEW_INPUT_PHASE_MOVED : VIEW_INPUT_PHASE_HOVER,
                          0, x, y, dragging ? 1.0 : 0.0, 0.0, 0.0, 0.0,
                          view_key_modifiers());
}

void view_on_scroll(view* v, f64 x, f64 y) {
    if (!v) return;
    v->scroll_x += x;
    v->scroll_y += y;
    view_request_frame(v, 1);
}

void view_on_mouse_btn(view* v, view_mouse_button button, view_button_action action) {
    if (!v) return;
    ns_bool pressed = action == VIEW_BUTTON_ACTION_PRESS;
    if (button == VIEW_MOUSE_BUTTON_LEFT) {
        v->mouse_pressed = v->mouse_pressed || pressed;
        v->mouse_released = v->mouse_released || !pressed;
        v->mouse_down = pressed;
        view_on_pointer_event(v, VIEW_INPUT_DEVICE_MOUSE,
                              pressed ? VIEW_INPUT_PHASE_BEGAN : VIEW_INPUT_PHASE_ENDED,
                              0, v->mouse_x, v->mouse_y, pressed ? 1.0 : 0.0,
                              0.0, 0.0, 0.0, view_key_modifiers());
    } else if (button == VIEW_MOUSE_BUTTON_RIGHT) {
        v->mouse_right_pressed = v->mouse_right_pressed || pressed;
        v->mouse_right_released = v->mouse_right_released || !pressed;
        v->mouse_right_down = pressed;
    } else if (button == VIEW_MOUSE_BUTTON_MIDDLE) {
        v->mouse_middle_pressed = v->mouse_middle_pressed || pressed;
        v->mouse_middle_released = v->mouse_middle_released || !pressed;
        v->mouse_middle_down = pressed;
    }
    view_request_frame(v, 1);
}

void view_on_pointer_event(view *v, i32 device, i32 phase, i32 pointer_id,
                           f64 x, f64 y, f64 pressure, f64 altitude,
                           f64 azimuth, f64 timestamp, i32 modifiers) {
    if (!v || view_event_count >= VIEW_INPUT_CAPACITY) return;
    i32 slot = pointer_id;
    if (slot < 0) slot = -slot;
    slot %= VIEW_POINTER_SLOTS;
    f64 dx = 0.0, dy = 0.0;
    if (view_pointer_known[slot]) {
        dx = x - view_pointer_x[slot];
        dy = y - view_pointer_y[slot];
    }
    view_pointer_x[slot] = x;
    view_pointer_y[slot] = y;
    view_pointer_known[slot] = phase != VIEW_INPUT_PHASE_ENDED && phase != VIEW_INPUT_PHASE_CANCELLED;
    view_events[view_event_count++] = (view_input_event){
        .device = device,
        .phase = phase,
        .pointer_id = pointer_id,
        .modifiers = modifiers,
        .x = x, .y = y, .dx = dx, .dy = dy,
        .pressure = pressure,
        .altitude = altitude,
        .azimuth = azimuth,
        .timestamp = timestamp,
        .tool_action = VIEW_TOOL_ACTION_NONE,
    };
    view_request_frame(v, 1);
}

void view_on_tool_action(view *v, i32 action, f64 timestamp) {
    if (!v || view_event_count >= VIEW_INPUT_CAPACITY) return;
    view_events[view_event_count++] = (view_input_event){
        .device = VIEW_INPUT_DEVICE_TOOL,
        .phase = VIEW_INPUT_PHASE_TOOL_ACTION,
        .timestamp = timestamp,
        .tool_action = action,
    };
    view_request_frame(v, 1);
}

void view_on_gesture(view *v, f64 pan_x, f64 pan_y, f64 zoom_factor, f64 rotation) {
    if (!v) return;
    view_gestures.pan_x += pan_x;
    view_gestures.pan_y += pan_y;
    if (zoom_factor > 0.0) view_gestures.zoom_factor *= zoom_factor;
    view_gestures.rotation += rotation;
    view_request_frame(v, 1);
}

void view_on_orbit_gesture(view *v, f64 orbit_x, f64 orbit_y) {
    if (!v) return;
    view_gestures.orbit_x += orbit_x;
    view_gestures.orbit_y += orbit_y;
    view_request_frame(v, 1);
}

i32 view_input_count(view *v) {
    return v ? view_event_count : 0;
}

view_input_event *view_input_at(view *v, i32 index) {
    if (!v || index < 0 || index >= view_event_count) return ns_null;
    return &view_events[index];
}

view_gesture_state *view_gesture(view *v) {
    return v ? &view_gestures : ns_null;
}

ns_bool view_input_pending(view *v) {
    if (!v) return false;
    if (view_event_count > 0 || v->scroll_x != 0.0 || v->scroll_y != 0.0 ||
        v->mouse_pressed || v->mouse_released ||
        v->mouse_right_pressed || v->mouse_right_released ||
        v->mouse_middle_pressed || v->mouse_middle_released ||
        view_gestures.pan_x != 0.0 || view_gestures.pan_y != 0.0 ||
        view_gestures.zoom_factor != 1.0 || view_gestures.rotation != 0.0 ||
        view_gestures.orbit_x != 0.0 || view_gestures.orbit_y != 0.0) return true;
    for (i32 key = 0; key <= VIEW_KEY_MENU; key++) {
        if (view_key_presses[key] != 0) return true;
    }
    for (i32 gamepad = 0; gamepad < VIEW_GAMEPAD_CAPACITY; gamepad++) {
        for (i32 button = 0; button < VIEW_GAMEPAD_BUTTON_CAPACITY; button++) {
            if (view_gamepad_button_presses[gamepad][button]) return true;
        }
    }
    return false;
}

void view_input_reset(view *v) {
    if (!v) return;
    v->mouse_pressed = false;
    v->mouse_released = false;
    v->mouse_right_pressed = false;
    v->mouse_right_released = false;
    v->mouse_middle_pressed = false;
    v->mouse_middle_released = false;
    v->scroll_x = 0.0;
    v->scroll_y = 0.0;
    view_event_count = 0;
    view_gestures = (view_gesture_state){.zoom_factor = 1.0};
    view_clear_key_presses(v);
    memset(view_gamepad_button_presses, 0, sizeof(view_gamepad_button_presses));
}

void view_on_key_action(view* v, view_keycode key, view_button_action action) {
    if (!v || key < 0 || key > VIEW_KEY_MENU) return;
    view_keys[key] = action == VIEW_BUTTON_ACTION_PRESS;
    if (action == VIEW_BUTTON_ACTION_PRESS) {
        view_key_presses[key] = view_key_modifiers() + 1;
    }
    view_request_frame(v, 1);
    if (key == VIEW_KEY_F12 && action == VIEW_BUTTON_ACTION_PRESS) {
        view_capture_require(v);
    }
}

ns_bool view_is_key_pressed(view* v, view_keycode key) {
    if (!v || key < 0 || key > VIEW_KEY_MENU) return false;
    return view_keys[key];
}

i32 view_take_key_press(view *v, view_keycode key) {
    if (!v || key < 0 || key > VIEW_KEY_MENU) return -1;
    i32 press = view_key_presses[key];
    if (press == 0) return -1;
    view_key_presses[key] = 0;
    return press - 1;
}

void view_clear_key_presses(view *v) {
    if (!v) return;
    memset(view_key_presses, 0, sizeof(view_key_presses));
}

static ns_bool view_gamepad_index_valid(i32 gamepad) {
    return gamepad >= 0 && gamepad < VIEW_GAMEPAD_CAPACITY;
}

static ns_bool view_gamepad_axis_valid(i32 axis) {
    return axis >= 0 && axis < VIEW_GAMEPAD_AXIS_CAPACITY;
}

static ns_bool view_gamepad_button_valid(i32 button) {
    return button >= 0 && button < VIEW_GAMEPAD_BUTTON_CAPACITY;
}

i32 view_gamepad_count(view *v) {
    if (!v) return 0;
    i32 count = 0;
    for (i32 gamepad = 0; gamepad < VIEW_GAMEPAD_CAPACITY; gamepad++) {
        if (view_gamepads[gamepad]) count++;
    }
    return count;
}

ns_bool view_gamepad_connected(view *v, i32 gamepad) {
    return v && view_gamepad_index_valid(gamepad) && view_gamepads[gamepad];
}

f32 view_gamepad_axis(view *v, i32 gamepad, i32 axis) {
    if (!view_gamepad_connected(v, gamepad) || !view_gamepad_axis_valid(axis)) return 0.0f;
    return view_gamepad_axes[gamepad][axis];
}

f32 view_gamepad_button(view *v, i32 gamepad, i32 button) {
    if (!view_gamepad_connected(v, gamepad) || !view_gamepad_button_valid(button)) return 0.0f;
    return view_gamepad_buttons[gamepad][button];
}

ns_bool view_gamepad_button_pressed(view *v, i32 gamepad, i32 button) {
    if (!view_gamepad_connected(v, gamepad) || !view_gamepad_button_valid(button)) return false;
    return view_gamepad_buttons_down[gamepad][button];
}

ns_bool view_take_gamepad_button_press(view *v, i32 gamepad, i32 button) {
    if (!view_gamepad_connected(v, gamepad) || !view_gamepad_button_valid(button)) return false;
    ns_bool pressed = view_gamepad_button_presses[gamepad][button];
    view_gamepad_button_presses[gamepad][button] = false;
    return pressed;
}

void view_on_gamepad_connected(view *v, i32 gamepad, ns_bool connected) {
    if (!v || !view_gamepad_index_valid(gamepad)) return;
    if (view_gamepads[gamepad] == connected) return;
    view_gamepads[gamepad] = connected;
    memset(view_gamepad_axes[gamepad], 0, sizeof(view_gamepad_axes[gamepad]));
    memset(view_gamepad_buttons[gamepad], 0, sizeof(view_gamepad_buttons[gamepad]));
    memset(view_gamepad_buttons_down[gamepad], 0, sizeof(view_gamepad_buttons_down[gamepad]));
    memset(view_gamepad_button_presses[gamepad], 0, sizeof(view_gamepad_button_presses[gamepad]));
    view_request_frame(v, 1);
}

void view_on_gamepad_axis(view *v, i32 gamepad, i32 axis, f32 value) {
    if (!view_gamepad_connected(v, gamepad) || !view_gamepad_axis_valid(axis)) return;
    if (value < -1.0f) value = -1.0f;
    if (value > 1.0f) value = 1.0f;
    if (view_gamepad_axes[gamepad][axis] == value) return;
    view_gamepad_axes[gamepad][axis] = value;
    view_request_frame(v, 1);
}

void view_on_gamepad_button(view *v, i32 gamepad, i32 button, f32 value, ns_bool pressed) {
    if (!view_gamepad_connected(v, gamepad) || !view_gamepad_button_valid(button)) return;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    ns_bool changed = view_gamepad_buttons[gamepad][button] != value ||
                      view_gamepad_buttons_down[gamepad][button] != pressed;
    if (pressed && !view_gamepad_buttons_down[gamepad][button]) {
        view_gamepad_button_presses[gamepad][button] = true;
    }
    view_gamepad_buttons[gamepad][button] = value;
    view_gamepad_buttons_down[gamepad][button] = pressed;
    if (changed) view_request_frame(v, 1);
}

void view_on_resize(view *v, i32 width, i32 height) {
    if (!v) return;
    v->width = width;
    v->height = height;
    v->framebuffer_width = (i32)((f64)width * v->ui_scale);
    v->framebuffer_height = (i32)((f64)height * v->ui_scale);
    view_request_frame(v, 1);
}

// A close is idempotent: a program that asks twice, or that asks from inside
// the frame callback the close tears down, must not re-enter the backend.
static ns_bool view_closing;

void view_close(view *v) {
    if (!v) return;
    if (view_closing) return;
    view_closing = true;
    view_platform_close(v);
}

void view_set_safe_area(view *v, f64 top, f64 right, f64 bottom, f64 left) {
    if (!v) return;
    v->safe_area_top = top > 0.0 ? top : 0.0;
    v->safe_area_right = right > 0.0 ? right : 0.0;
    v->safe_area_bottom = bottom > 0.0 ? bottom : 0.0;
    v->safe_area_left = left > 0.0 ? left : 0.0;
}

void view_capture_require(view *v) {
    if (!v) return;
    v->capture_required = true;
    view_request_frame(v, 1);
}

static ns_bool immersive_supported;
static ns_bool immersive_requested;
static i32 immersive_status;
static i32 immersive_eye = -1;
static float immersive_pose[48];

ns_bool view_immersive_supported(void) { return immersive_supported; }
ns_bool view_immersive_request(ns_bool enabled) {
    if (!immersive_supported) return false;
    immersive_requested = enabled;
    return true;
}
i32 view_immersive_status(void) { return immersive_status; }
i32 view_immersive_eye(void) { return immersive_eye; }
f64 view_immersive_value(i32 index) {
    return index >= 0 && index < 48 && immersive_eye >= 0 ? immersive_pose[index] : 0.0;
}
void view_immersive_host_support(ns_bool supported) { immersive_supported = supported; }
ns_bool view_immersive_host_requested(void) { return immersive_requested; }
void view_immersive_host_status(i32 status) {
    immersive_status = status;
    if (status <= 0) { immersive_requested = false; immersive_eye = -1; }
}
void view_immersive_host_pose(i32 eye, const float *pose) {
    immersive_eye = eye;
    if (pose) for (i32 i = 0; i < 48; ++i) immersive_pose[i] = pose[i];
}
