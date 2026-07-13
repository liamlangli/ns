#include "view.h"

static ns_bool view_keys[VIEW_KEY_MENU + 1];
// Stored as modifiers + 1 so zero remains the no-event sentinel.
static i32 view_key_presses[VIEW_KEY_MENU + 1];

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
