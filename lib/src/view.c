#include "view.h"

static ns_bool view_keys[VIEW_KEY_MENU + 1];
// Stored as modifiers + 1 so zero remains the no-event sentinel.
static i32 view_key_presses[VIEW_KEY_MENU + 1];

#define VIEW_INPUT_CAPACITY 512
#define VIEW_POINTER_SLOTS 32
static view_input_event view_events[VIEW_INPUT_CAPACITY];
static i32 view_event_count;
static view_gesture_state view_gestures = {.zoom_factor = 1.0};
static f64 view_pointer_x[VIEW_POINTER_SLOTS];
static f64 view_pointer_y[VIEW_POINTER_SLOTS];
static ns_bool view_pointer_known[VIEW_POINTER_SLOTS];

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
    view_on_pointer_event(v, VIEW_INPUT_DEVICE_MOUSE,
                          v->mouse_down ? VIEW_INPUT_PHASE_MOVED : VIEW_INPUT_PHASE_HOVER,
                          0, x, y, v->mouse_down ? 1.0 : 0.0, 0.0, 0.0, 0.0,
                          view_key_modifiers());
}

void view_on_scroll(view* v, f64 x, f64 y) {
    if (!v) return;
    v->scroll_x += x;
    v->scroll_y += y;
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
}

void view_on_tool_action(view *v, i32 action, f64 timestamp) {
    if (!v || view_event_count >= VIEW_INPUT_CAPACITY) return;
    view_events[view_event_count++] = (view_input_event){
        .device = VIEW_INPUT_DEVICE_TOOL,
        .phase = VIEW_INPUT_PHASE_TOOL_ACTION,
        .timestamp = timestamp,
        .tool_action = action,
    };
}

void view_on_gesture(view *v, f64 pan_x, f64 pan_y, f64 zoom_factor, f64 rotation) {
    if (!v) return;
    view_gestures.pan_x += pan_x;
    view_gestures.pan_y += pan_y;
    if (zoom_factor > 0.0) view_gestures.zoom_factor *= zoom_factor;
    view_gestures.rotation += rotation;
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
}

void view_on_key_action(view* v, view_keycode key, view_button_action action) {
    if (!v || key < 0 || key > VIEW_KEY_MENU) return;
    view_keys[key] = action == VIEW_BUTTON_ACTION_PRESS;
    if (action == VIEW_BUTTON_ACTION_PRESS) {
        view_key_presses[key] = view_key_modifiers() + 1;
    }
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

void view_on_resize(view *v, i32 width, i32 height) {
    if (!v) return;
    v->width = width;
    v->height = height;
    v->framebuffer_width = (i32)((f64)width * v->ui_scale);
    v->framebuffer_height = (i32)((f64)height * v->ui_scale);
}

void view_close(view *v) {
    ns_unused(v);
    // View cleanup implementation
}

void view_capture_require(view *v) {
    if (!v) return;
    v->capture_required = true;
}
