#include "os.h"

void os_window_on_mouse_move(os_window* window, f64 x, f64 y) {
    // script_t *ctx = script_shared();
    // ui_state_t *state = ui_state_get();
    // state->pointer_location.x = (f32)x;
    // state->pointer_location.y = (f32)y;
    window->mouse_x = x;
    window->mouse_y = y;
    // script_mouse_move((f32)x, (f32)y);
}

void os_window_on_scroll(os_window* window, f64 x, f64 y) {
    ns_unused(window);
    ns_unused(x);
    ns_unused(y);

    // script_t *ctx = script_shared();
    // ui_state_t *state = ui_state_get();
    // const bool shift = ui_state_is_key_pressed(KEY_LEFT_SHIFT) || ui_state_is_key_pressed(KEY_RIGHT_SHIFT);
    // state->pointer_scroll.x = (f32)(x * (shift ? state->smooth_factor : 1.f));
    // state->pointer_scroll.y = (f32)(-y * (shift ? state->smooth_factor : 1.f));
}

void os_window_on_mouse_btn(os_window* window, os_mouse_button button, os_button_action action) {
    ns_unused(window);
    ns_unused(button);
    ns_unused(action);


    // script_t *ctx = script_shared();
    // if (ctx == NULL) return;

    // if (action == BUTTON_ACTION_PRESS) {
    //     ui_state_mouse_down(button);
    // } else if (action == BUTTON_ACTION_RELEASE) {
    //     ui_state_mouse_up(button);
    // }
    // script_mouse_button(button, action);
    // ui_state_set_mouse_location(window->mouse_x, window->mouse_y);
}

void os_window_on_key_action(os_window* window, os_keycode key, os_button_action action) {
    ns_unused(window);
    ns_unused(key);
    ns_unused(action);


    // script_t *ctx = script_shared();
    // if (ctx == NULL) return;

    // if (action == BUTTON_ACTION_PRESS) {
    //     ui_state_key_press(key);
    //     script_key_action(key, BUTTON_ACTION_PRESS);
    // } else if (action == BUTTON_ACTION_RELEASE) {
    //     ui_state_key_release(key);
    //     script_key_action(key, BUTTON_ACTION_RELEASE);
    // }
}

ns_bool os_window_is_key_pressed(os_window* window, os_keycode key) {
    ns_unused(window);
    ns_unused(key);
    // script_t *ctx = script_shared();
    // if (ctx == NULL) return false;
    // return ui_state_is_key_pressed(key);
    return false;
}

void os_window_on_resize(os_window *window, i32 width, i32 height) {
    ns_unused(window);
    ns_unused(width);
    ns_unused(height);

    // script_t *ctx = script_shared();
    // window->width = width;
    // window->height = height;
    // ui_renderer_set_size(width, height);
    // ui_state_t* state = ui_state_get();
    // state->window_rect = (ui_rect){
    //     .x = 0.f,
    //     .y = 0.f,
    //     .w = width,
    //     .h = height
    // };
    // script_resize(width, height);
}

void os_window_close(os_window *window) {
    ns_unused(window);
}

void os_window_capture_require(os_window *window) {
    ns_unused(window);
}