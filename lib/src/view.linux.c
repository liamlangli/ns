// Wayland view backend (Linux).
//
// Opens an xdg-shell toplevel on the compositor named by WAYLAND_DISPLAY and
// drives frames from its own event loop. The GPU backend is reached through
// dlsym: gpu_vk_begin_frame/gpu_vk_end_frame wrap the application's frame
// callback so the swapchain image is acquired before it and presented after
// it, exactly as the Metal backend's begin/end frame pair does. A program that
// never requests a GPU device still gets a working window and input loop.
//
// Touch arrives through the seat's wl_touch, so a handheld such as the Steam
// Deck gets taps and drags. Gamepads are read straight from evdev under
// /dev/input: Wayland has no gamepad protocol, and both the Deck's own
// controller (hid-steam) and the virtual pad Steam Input creates are ordinary
// evdev devices there.
#include "view.h"

#ifdef NS_LINUX

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "xdg-shell-client-protocol.h"

typedef void (*view_on_launch)(view*);
typedef void (*view_on_frame)(view*);
typedef void (*view_on_terminate)(view*);

#define VIEW_LINUX_TOUCH_CAPACITY 16

typedef struct view_linux_touch {
    i32 id;
    f64 x, y;
} view_linux_touch;

// What one evdev absolute axis drives on the standard layout.
typedef enum view_linux_abs_role {
    VIEW_LINUX_ABS_NONE = -1,
    // 0..3 are VIEW_GAMEPAD_AXIS_* directly.
    VIEW_LINUX_ABS_LEFT_TRIGGER = 4,
    VIEW_LINUX_ABS_RIGHT_TRIGGER = 5,
    VIEW_LINUX_ABS_HAT_X = 6,
    VIEW_LINUX_ABS_HAT_Y = 7,
} view_linux_abs_role;

typedef struct view_linux_gamepad {
    // -1 while the slot is free.
    int fd;
    // The device node, so a rescan never opens the same pad twice.
    dev_t device;
    // hid-steam (the Deck's built-in controller without Steam running): the
    // triggers are HAT2Y/HAT2X and HAT0/HAT1 are the trackpads, not a d-pad.
    ns_bool hid_steam;
    // hid-playstation names BTN_NORTH/BTN_WEST by position. Xbox-style
    // drivers, including Steam's virtual pad, send BTN_X/BTN_Y by label, and
    // those share the same codes the other way round.
    ns_bool positional_face;
    // A joystick without BTN_GAMEPAD: its BTN_TRIGGER.. buttons map in order.
    ns_bool joystick_buttons;
    ns_bool analog_trigger[2];
    // Events after SYN_DROPPED are discarded until the next SYN_REPORT, then
    // the whole state is read back from the device.
    ns_bool dropped;
    i32 abs_role[ABS_CNT];
    i32 abs_min[ABS_CNT];
    i32 abs_max[ABS_CNT];
    ns_bool hat_left, hat_right, hat_up, hat_down;
} view_linux_gamepad;

typedef struct view_linux_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct wl_data_source *data_source;
    struct wl_data_offer *selection_offer;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor *cursor;
    struct wl_surface *cursor_surface;

    int event_fd;
    ns_bool running;
    ns_bool finished;
    ns_bool configured;
    ns_bool closed;
    int pending_width;
    int pending_height;

    // Frames requested by the application since the last draw.
    i32 frame_requests;
    // view_platform_request_frame_after deadline, in CLOCK_MONOTONIC ms.
    i64 timer_deadline;

    f64 pointer_x;
    f64 pointer_y;
    ns_bool pointer_inside;
    uint32_t last_serial;
    uint32_t enter_serial;

    // Pending wheel travel for the current pointer frame. A compositor that
    // speaks axis_value120 sends both events; the discrete form wins so a
    // notch is never counted twice.
    f64 axis_x;
    f64 axis_y;
    f64 discrete_x;
    f64 discrete_y;
    ns_bool axis_discrete_x;
    ns_bool axis_discrete_y;

    // Pressed view keycodes, so focus loss can release them all.
    i32 pressed_keys[64];
    i32 pressed_key_count;

    char *clipboard;
    char *selection_mime;

    // The keymap must stay mapped for as long as the keyboard exists.
    void *keymap;
    size_t keymap_size;

    // Fingers currently down, with the last position each one reported:
    // wl_touch.up carries no coordinates.
    view_linux_touch touches[VIEW_LINUX_TOUCH_CAPACITY];
    i32 touch_count;

    // inotify watch on /dev/input, so pads plugged in (or granted access by
    // udev) after launch are picked up.
    int input_notify_fd;
    view_linux_gamepad gamepads[VIEW_GAMEPAD_CAPACITY];
} view_linux_state;

static view _view;
static view_linux_state _state;

static void view_linux_draw_frame(view *v);
static void view_linux_finish(view *v);
static void view_linux_set_cursor(void);

static i64 view_linux_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void view_linux_wake(void) {
    if (_state.event_fd >= 0) {
        u64 one = 1;
        ssize_t ignored = write(_state.event_fd, &one, sizeof(one));
        ns_unused(ignored);
    }
}

// ---- registry ---------------------------------------------------------------

static void view_linux_registry_global(void *data, struct wl_registry *registry,
                                       uint32_t name, const char *interface, uint32_t version) {
    view_linux_state *s = (view_linux_state *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        s->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        s->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        s->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 5 ? version : 5);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        s->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        s->data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    }
}

static void view_linux_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    ns_unused(data);
    ns_unused(registry);
    ns_unused(name);
}

static const struct wl_registry_listener view_linux_registry_listener = {
    .global = view_linux_registry_global,
    .global_remove = view_linux_registry_global_remove,
};

// ---- xdg shell --------------------------------------------------------------

static void view_linux_wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    ns_unused(data);
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener view_linux_wm_base_listener = {
    .ping = view_linux_wm_ping,
};

static void view_linux_apply_size(void) {
    if (_state.pending_width <= 0 || _state.pending_height <= 0) return;
    if (_state.pending_width == _view.width && _state.pending_height == _view.height) return;
    view_on_resize(&_view, _state.pending_width, _state.pending_height);
    view_request_frame(&_view, 1);
}

static void view_linux_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    ns_unused(data);
    xdg_surface_ack_configure(xdg_surface, serial);
    _state.configured = true;
    view_linux_apply_size();
}

static const struct xdg_surface_listener view_linux_surface_listener = {
    .configure = view_linux_surface_configure,
};

static void view_linux_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                          int32_t width, int32_t height, struct wl_array *states) {
    ns_unused(data);
    ns_unused(toplevel);
    ns_unused(states);
    if (width > 0 && height > 0) {
        _state.pending_width = width;
        _state.pending_height = height;
    }
}

static void view_linux_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    ns_unused(data);
    ns_unused(toplevel);
    _state.closed = true;
    _state.running = false;
    view_linux_wake();
}

static void view_linux_toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                                 int32_t width, int32_t height) {
    ns_unused(data);
    ns_unused(toplevel);
    ns_unused(width);
    ns_unused(height);
}

static void view_linux_toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *capabilities) {
    ns_unused(data);
    ns_unused(toplevel);
    ns_unused(capabilities);
}

static const struct xdg_toplevel_listener view_linux_toplevel_listener = {
    .configure = view_linux_toplevel_configure,
    .close = view_linux_toplevel_close,
    .configure_bounds = view_linux_toplevel_configure_bounds,
    .wm_capabilities = view_linux_toplevel_wm_capabilities,
};

// ---- keyboard ---------------------------------------------------------------

// evdev keycode -> VIEW_KEY_*. The names follow the US layout; the physical
// key is what reaches the application, exactly as the macOS ANSI mapping does.
static i32 view_linux_key_map(uint32_t code) {
    switch (code) {
        case KEY_ESC: return VIEW_KEY_ESCAPE;
        case KEY_1: return VIEW_KEY_1;
        case KEY_2: return VIEW_KEY_2;
        case KEY_3: return VIEW_KEY_3;
        case KEY_4: return VIEW_KEY_4;
        case KEY_5: return VIEW_KEY_5;
        case KEY_6: return VIEW_KEY_6;
        case KEY_7: return VIEW_KEY_7;
        case KEY_8: return VIEW_KEY_8;
        case KEY_9: return VIEW_KEY_9;
        case KEY_0: return VIEW_KEY_0;
        case KEY_MINUS: return VIEW_KEY_MINUS;
        case KEY_EQUAL: return VIEW_KEY_EQUAL;
        case KEY_BACKSPACE: return VIEW_KEY_BACKSPACE;
        case KEY_TAB: return VIEW_KEY_TAB;
        case KEY_Q: return VIEW_KEY_Q;
        case KEY_W: return VIEW_KEY_W;
        case KEY_E: return VIEW_KEY_E;
        case KEY_R: return VIEW_KEY_R;
        case KEY_T: return VIEW_KEY_T;
        case KEY_Y: return VIEW_KEY_Y;
        case KEY_U: return VIEW_KEY_U;
        case KEY_I: return VIEW_KEY_I;
        case KEY_O: return VIEW_KEY_O;
        case KEY_P: return VIEW_KEY_P;
        case KEY_LEFTBRACE: return VIEW_KEY_LEFT_BRACKET;
        case KEY_RIGHTBRACE: return VIEW_KEY_RIGHT_BRACKET;
        case KEY_ENTER: return VIEW_KEY_ENTER;
        case KEY_LEFTCTRL: return VIEW_KEY_LEFT_CONTROL;
        case KEY_A: return VIEW_KEY_A;
        case KEY_S: return VIEW_KEY_S;
        case KEY_D: return VIEW_KEY_D;
        case KEY_F: return VIEW_KEY_F;
        case KEY_G: return VIEW_KEY_G;
        case KEY_H: return VIEW_KEY_H;
        case KEY_J: return VIEW_KEY_J;
        case KEY_K: return VIEW_KEY_K;
        case KEY_L: return VIEW_KEY_L;
        case KEY_SEMICOLON: return VIEW_KEY_SEMICOLON;
        case KEY_APOSTROPHE: return VIEW_KEY_APOSTROPHE;
        case KEY_GRAVE: return VIEW_KEY_GRAVE_ACCENT;
        case KEY_LEFTSHIFT: return VIEW_KEY_LEFT_SHIFT;
        case KEY_BACKSLASH: return VIEW_KEY_BACKSLASH;
        case KEY_Z: return VIEW_KEY_Z;
        case KEY_X: return VIEW_KEY_X;
        case KEY_C: return VIEW_KEY_C;
        case KEY_V: return VIEW_KEY_V;
        case KEY_B: return VIEW_KEY_B;
        case KEY_N: return VIEW_KEY_N;
        case KEY_M: return VIEW_KEY_M;
        case KEY_COMMA: return VIEW_KEY_COMMA;
        case KEY_DOT: return VIEW_KEY_PERIOD;
        case KEY_SLASH: return VIEW_KEY_SLASH;
        case KEY_RIGHTSHIFT: return VIEW_KEY_RIGHT_SHIFT;
        case KEY_KPASTERISK: return VIEW_KEY_KP_MULTIPLY;
        case KEY_LEFTALT: return VIEW_KEY_LEFT_ALT;
        case KEY_SPACE: return VIEW_KEY_SPACE;
        case KEY_CAPSLOCK: return VIEW_KEY_CAPS_LOCK;
        case KEY_F1: return VIEW_KEY_F1;
        case KEY_F2: return VIEW_KEY_F2;
        case KEY_F3: return VIEW_KEY_F3;
        case KEY_F4: return VIEW_KEY_F4;
        case KEY_F5: return VIEW_KEY_F5;
        case KEY_F6: return VIEW_KEY_F6;
        case KEY_F7: return VIEW_KEY_F7;
        case KEY_F8: return VIEW_KEY_F8;
        case KEY_F9: return VIEW_KEY_F9;
        case KEY_F10: return VIEW_KEY_F10;
        case KEY_F11: return VIEW_KEY_F11;
        case KEY_F12: return VIEW_KEY_F12;
        case KEY_F13: return VIEW_KEY_F13;
        case KEY_F14: return VIEW_KEY_F14;
        case KEY_F15: return VIEW_KEY_F15;
        case KEY_F16: return VIEW_KEY_F16;
        case KEY_F17: return VIEW_KEY_F17;
        case KEY_F18: return VIEW_KEY_F18;
        case KEY_F19: return VIEW_KEY_F19;
        case KEY_F20: return VIEW_KEY_F20;
        case KEY_F21: return VIEW_KEY_F21;
        case KEY_F22: return VIEW_KEY_F22;
        case KEY_F23: return VIEW_KEY_F23;
        case KEY_F24: return VIEW_KEY_F24;
        case KEY_NUMLOCK: return VIEW_KEY_NUM_LOCK;
        case KEY_SCROLLLOCK: return VIEW_KEY_SCROLL_LOCK;
        case KEY_KP0: return VIEW_KEY_KP_0;
        case KEY_KP1: return VIEW_KEY_KP_1;
        case KEY_KP2: return VIEW_KEY_KP_2;
        case KEY_KP3: return VIEW_KEY_KP_3;
        case KEY_KP4: return VIEW_KEY_KP_4;
        case KEY_KP5: return VIEW_KEY_KP_5;
        case KEY_KP6: return VIEW_KEY_KP_6;
        case KEY_KP7: return VIEW_KEY_KP_7;
        case KEY_KP8: return VIEW_KEY_KP_8;
        case KEY_KP9: return VIEW_KEY_KP_9;
        case KEY_KPDOT: return VIEW_KEY_KP_DECIMAL;
        case KEY_KPMINUS: return VIEW_KEY_KP_SUBTRACT;
        case KEY_KPPLUS: return VIEW_KEY_KP_ADD;
        case KEY_KPSLASH: return VIEW_KEY_KP_DIVIDE;
        case KEY_KPENTER: return VIEW_KEY_KP_ENTER;
        case KEY_KPEQUAL: return VIEW_KEY_KP_EQUAL;
        case KEY_RIGHTCTRL: return VIEW_KEY_RIGHT_CONTROL;
        case KEY_RIGHTALT: return VIEW_KEY_RIGHT_ALT;
        case KEY_LEFTMETA: return VIEW_KEY_LEFT_SUPER;
        case KEY_RIGHTMETA: return VIEW_KEY_RIGHT_SUPER;
        case KEY_MENU: return VIEW_KEY_MENU;
        case KEY_SYSRQ: return VIEW_KEY_PRINT_SCREEN;
        case KEY_PAUSE: return VIEW_KEY_PAUSE;
        case KEY_INSERT: return VIEW_KEY_INSERT;
        case KEY_DELETE: return VIEW_KEY_DELETE;
        case KEY_HOME: return VIEW_KEY_HOME;
        case KEY_END: return VIEW_KEY_END;
        case KEY_PAGEUP: return VIEW_KEY_PAGE_UP;
        case KEY_PAGEDOWN: return VIEW_KEY_PAGE_DOWN;
        case KEY_UP: return VIEW_KEY_UP;
        case KEY_DOWN: return VIEW_KEY_DOWN;
        case KEY_LEFT: return VIEW_KEY_LEFT;
        case KEY_RIGHT: return VIEW_KEY_RIGHT;
        default: return -1;
    }
}

static void view_linux_note_key(i32 key, ns_bool pressed) {
    if (key < 0) return;
    if (pressed) {
        for (i32 i = 0; i < _state.pressed_key_count; i++) {
            if (_state.pressed_keys[i] == key) return;
        }
        if (_state.pressed_key_count < (i32)(sizeof(_state.pressed_keys) / sizeof(_state.pressed_keys[0]))) {
            _state.pressed_keys[_state.pressed_key_count++] = key;
        }
    } else {
        for (i32 i = 0; i < _state.pressed_key_count; i++) {
            if (_state.pressed_keys[i] == key) {
                _state.pressed_keys[i] = _state.pressed_keys[--_state.pressed_key_count];
                break;
            }
        }
    }
}

static void view_linux_keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                                       uint32_t format, int32_t fd, uint32_t size) {
    ns_unused(data);
    ns_unused(keyboard);
    ns_unused(format);
    if (_state.keymap) {
        munmap(_state.keymap, _state.keymap_size);
        _state.keymap = ns_null;
        _state.keymap_size = 0;
    }
    if (size > 0) {
        void *mapped = mmap(ns_null, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) {
            _state.keymap = mapped;
            _state.keymap_size = size;
        }
    }
    close(fd);
}

static void view_linux_keyboard_enter(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    ns_unused(data);
    ns_unused(keyboard);
    ns_unused(surface);
    _state.enter_serial = serial;
    _state.last_serial = serial;
    ns_unused(keys);
}

static void view_linux_keyboard_leave(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, struct wl_surface *surface) {
    ns_unused(data);
    ns_unused(keyboard);
    ns_unused(surface);
    _state.last_serial = serial;
    // A key released while another window had focus would otherwise stay down.
    while (_state.pressed_key_count > 0) {
        i32 key = _state.pressed_keys[--_state.pressed_key_count];
        view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_RELEASE);
    }
}

static void view_linux_keyboard_key(void *data, struct wl_keyboard *keyboard,
                                    uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    ns_unused(data);
    ns_unused(keyboard);
    ns_unused(time);
    _state.last_serial = serial;
    i32 mapped = view_linux_key_map(key);
    if (mapped < 0) return;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Wayland delivers one event per press; repeats are the client's to
        // synthesize, and the applications here poll held keys instead.
        if (_state.pressed_key_count > 0) {
            for (i32 i = 0; i < _state.pressed_key_count; i++) {
                if (_state.pressed_keys[i] == mapped) return;
            }
        }
        view_linux_note_key(mapped, true);
        view_on_key_action(&_view, mapped, VIEW_BUTTON_ACTION_PRESS);
    } else {
        view_linux_note_key(mapped, false);
        view_on_key_action(&_view, mapped, VIEW_BUTTON_ACTION_RELEASE);
    }
}

static void view_linux_keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                                          uint32_t serial, uint32_t depressed,
                                          uint32_t latched, uint32_t locked, uint32_t group) {
    ns_unused(data);
    ns_unused(keyboard);
    ns_unused(depressed);
    ns_unused(latched);
    ns_unused(locked);
    ns_unused(group);
    _state.last_serial = serial;
}

static void view_linux_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    ns_unused(data);
    ns_unused(keyboard);
    ns_unused(rate);
    ns_unused(delay);
}

static const struct wl_keyboard_listener view_linux_keyboard_listener = {
    .keymap = view_linux_keyboard_keymap,
    .enter = view_linux_keyboard_enter,
    .leave = view_linux_keyboard_leave,
    .key = view_linux_keyboard_key,
    .modifiers = view_linux_keyboard_modifiers,
    .repeat_info = view_linux_keyboard_repeat_info,
};

// ---- pointer ----------------------------------------------------------------

static void view_linux_pointer_enter(void *data, struct wl_pointer *pointer,
                                     uint32_t serial, struct wl_surface *surface,
                                     wl_fixed_t sx, wl_fixed_t sy) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(surface);
    _state.last_serial = serial;
    _state.pointer_inside = true;
    _state.pointer_x = wl_fixed_to_double(sx);
    _state.pointer_y = wl_fixed_to_double(sy);
    view_linux_set_cursor();
    view_on_mouse_move(&_view, _state.pointer_x, _state.pointer_y);
}

static void view_linux_pointer_leave(void *data, struct wl_pointer *pointer,
                                     uint32_t serial, struct wl_surface *surface) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(surface);
    _state.last_serial = serial;
    _state.pointer_inside = false;
}

static void view_linux_pointer_motion(void *data, struct wl_pointer *pointer,
                                      uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(time);
    _state.pointer_x = wl_fixed_to_double(sx);
    _state.pointer_y = wl_fixed_to_double(sy);
    view_on_mouse_move(&_view, _state.pointer_x, _state.pointer_y);
}

static void view_linux_pointer_button(void *data, struct wl_pointer *pointer,
                                      uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(time);
    _state.last_serial = serial;
    i32 mapped = -1;
    if (button == BTN_LEFT) mapped = VIEW_MOUSE_BUTTON_LEFT;
    else if (button == BTN_RIGHT) mapped = VIEW_MOUSE_BUTTON_RIGHT;
    else if (button == BTN_MIDDLE) mapped = VIEW_MOUSE_BUTTON_MIDDLE;
    if (mapped < 0) return;
    view_on_mouse_btn(&_view, mapped,
                      state == WL_POINTER_BUTTON_STATE_PRESSED
                          ? VIEW_BUTTON_ACTION_PRESS
                          : VIEW_BUTTON_ACTION_RELEASE);
}

static void view_linux_pointer_axis(void *data, struct wl_pointer *pointer,
                                    uint32_t time, uint32_t axis, wl_fixed_t value) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(time);
    f64 amount = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        _state.axis_x += amount;
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        _state.axis_y += amount;
    }
}

static void view_linux_pointer_frame(void *data, struct wl_pointer *pointer) {
    ns_unused(data);
    ns_unused(pointer);
    // A discrete notch names the same travel the continuous axis value does;
    // when the compositor sends both, the notch is the exact answer.
    f64 x = _state.axis_discrete_x ? _state.discrete_x : _state.axis_x;
    f64 y = _state.axis_discrete_y ? _state.discrete_y : _state.axis_y;
    if (x != 0.0 || y != 0.0) view_on_scroll(&_view, x, y);
    _state.axis_x = 0.0;
    _state.axis_y = 0.0;
    _state.discrete_x = 0.0;
    _state.discrete_y = 0.0;
    _state.axis_discrete_x = false;
    _state.axis_discrete_y = false;
}

static void view_linux_pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(axis_source);
}

static void view_linux_pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    ns_unused(data);
    ns_unused(pointer);
    ns_unused(time);
    ns_unused(axis);
}

static void view_linux_pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                             uint32_t axis, int32_t discrete) {
    ns_unused(data);
    ns_unused(pointer);
    // A discrete notch is exactly one turn of the wheel away from the hand.
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        _state.discrete_x += (f64)discrete;
        _state.axis_discrete_x = true;
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        _state.discrete_y += (f64)discrete;
        _state.axis_discrete_y = true;
    }
}

static void view_linux_pointer_axis_value120(void *data, struct wl_pointer *pointer,
                                             uint32_t axis, int32_t value120) {
    ns_unused(data);
    ns_unused(pointer);
    f64 amount = (f64)value120 / 120.0;
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        _state.discrete_x += amount;
        _state.axis_discrete_x = true;
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        _state.discrete_y += amount;
        _state.axis_discrete_y = true;
    }
}

static const struct wl_pointer_listener view_linux_pointer_listener = {
    .enter = view_linux_pointer_enter,
    .leave = view_linux_pointer_leave,
    .motion = view_linux_pointer_motion,
    .button = view_linux_pointer_button,
    .axis = view_linux_pointer_axis,
    .frame = view_linux_pointer_frame,
    .axis_source = view_linux_pointer_axis_source,
    .axis_stop = view_linux_pointer_axis_stop,
    .axis_discrete = view_linux_pointer_axis_discrete,
    .axis_value120 = view_linux_pointer_axis_value120,
};

// ---- touch ------------------------------------------------------------------

static view_linux_touch *view_linux_touch_find(i32 id) {
    for (i32 i = 0; i < _state.touch_count; i++) {
        if (_state.touches[i].id == id) return &_state.touches[i];
    }
    return ns_null;
}

// Publish one contact on the unified pointer stream, the way the iOS backend
// does. Touch ids are offset by one so the first finger never shares the
// mouse's pointer slot. The legacy mouse fields report an edge for every
// contact, while mouse_down stays true until the last finger lifts.
static void view_linux_touch_event(i32 id, i32 phase, f64 x, f64 y, uint32_t time) {
    view_on_pointer_event(&_view, VIEW_INPUT_DEVICE_TOUCH, phase, id + 1, x, y,
                          phase == VIEW_INPUT_PHASE_ENDED || phase == VIEW_INPUT_PHASE_CANCELLED ? 0.0 : 1.0,
                          0.0, 0.0, (f64)time / 1000.0, 0);
    _view.mouse_x = x;
    _view.mouse_y = y;
    if (phase == VIEW_INPUT_PHASE_BEGAN) {
        _view.mouse_pressed = true;
    } else if (phase == VIEW_INPUT_PHASE_ENDED || phase == VIEW_INPUT_PHASE_CANCELLED) {
        _view.mouse_released = true;
    }
    _view.mouse_down = _state.touch_count > 0;
}

static void view_linux_touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                                  struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    ns_unused(data);
    ns_unused(touch);
    _state.last_serial = serial;
    if (surface != _state.surface) return;
    view_linux_touch *contact = view_linux_touch_find(id);
    if (!contact) {
        if (_state.touch_count >= VIEW_LINUX_TOUCH_CAPACITY) return;
        contact = &_state.touches[_state.touch_count++];
        contact->id = id;
    }
    contact->x = wl_fixed_to_double(x);
    contact->y = wl_fixed_to_double(y);
    view_linux_touch_event(id, VIEW_INPUT_PHASE_BEGAN, contact->x, contact->y, time);
}

static void view_linux_touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id) {
    ns_unused(data);
    ns_unused(touch);
    _state.last_serial = serial;
    view_linux_touch *contact = view_linux_touch_find(id);
    if (!contact) return;
    f64 x = contact->x;
    f64 y = contact->y;
    *contact = _state.touches[--_state.touch_count];
    view_linux_touch_event(id, VIEW_INPUT_PHASE_ENDED, x, y, time);
}

static void view_linux_touch_motion(void *data, struct wl_touch *touch, uint32_t time,
                                    int32_t id, wl_fixed_t x, wl_fixed_t y) {
    ns_unused(data);
    ns_unused(touch);
    view_linux_touch *contact = view_linux_touch_find(id);
    if (!contact) return;
    contact->x = wl_fixed_to_double(x);
    contact->y = wl_fixed_to_double(y);
    view_linux_touch_event(id, VIEW_INPUT_PHASE_MOVED, contact->x, contact->y, time);
}

static void view_linux_touch_frame(void *data, struct wl_touch *touch) {
    ns_unused(data);
    ns_unused(touch);
}

// The compositor took the whole sequence over (a system gesture, say): every
// finger still down ends as cancelled.
static void view_linux_touch_cancel(void *data, struct wl_touch *touch) {
    ns_unused(data);
    ns_unused(touch);
    while (_state.touch_count > 0) {
        view_linux_touch contact = _state.touches[--_state.touch_count];
        view_linux_touch_event(contact.id, VIEW_INPUT_PHASE_CANCELLED, contact.x, contact.y, 0);
    }
}

static void view_linux_touch_shape(void *data, struct wl_touch *touch, int32_t id,
                                   wl_fixed_t major, wl_fixed_t minor) {
    ns_unused(data);
    ns_unused(touch);
    ns_unused(id);
    ns_unused(major);
    ns_unused(minor);
}

static void view_linux_touch_orientation(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t orientation) {
    ns_unused(data);
    ns_unused(touch);
    ns_unused(id);
    ns_unused(orientation);
}

static const struct wl_touch_listener view_linux_touch_listener = {
    .down = view_linux_touch_down,
    .up = view_linux_touch_up,
    .motion = view_linux_touch_motion,
    .frame = view_linux_touch_frame,
    .cancel = view_linux_touch_cancel,
    .shape = view_linux_touch_shape,
    .orientation = view_linux_touch_orientation,
};

// ---- seat -------------------------------------------------------------------

static void view_linux_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    ns_unused(data);
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !_state.keyboard) {
        _state.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(_state.keyboard, &view_linux_keyboard_listener, &_state);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && _state.keyboard) {
        wl_keyboard_destroy(_state.keyboard);
        _state.keyboard = ns_null;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !_state.pointer) {
        _state.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(_state.pointer, &view_linux_pointer_listener, &_state);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && _state.pointer) {
        wl_pointer_destroy(_state.pointer);
        _state.pointer = ns_null;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !_state.touch) {
        _state.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(_state.touch, &view_linux_touch_listener, &_state);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_TOUCH) && _state.touch) {
        view_linux_touch_cancel(ns_null, _state.touch);
        wl_touch_destroy(_state.touch);
        _state.touch = ns_null;
    }
}

static void view_linux_seat_name(void *data, struct wl_seat *seat, const char *name) {
    ns_unused(data);
    ns_unused(seat);
    ns_unused(name);
}

static const struct wl_seat_listener view_linux_seat_listener = {
    .capabilities = view_linux_seat_capabilities,
    .name = view_linux_seat_name,
};

// ---- clipboard --------------------------------------------------------------

static void view_linux_data_source_target(void *data, struct wl_data_source *source, const char *mime_type) {
    ns_unused(data);
    ns_unused(source);
    ns_unused(mime_type);
}

static void view_linux_data_source_send(void *data, struct wl_data_source *source, const char *mime_type, int32_t fd) {
    ns_unused(data);
    ns_unused(source);
    ns_unused(mime_type);
    const char *text = _state.clipboard ? _state.clipboard : "";
    size_t length = strlen(text);
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, text + written, length - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    close(fd);
}

static void view_linux_data_source_cancelled(void *data, struct wl_data_source *source) {
    ns_unused(data);
    if (_state.data_source == source) {
        wl_data_source_destroy(_state.data_source);
        _state.data_source = ns_null;
    }
}

static void view_linux_data_source_dnd_drop_performed(void *data, struct wl_data_source *source) {
    ns_unused(data);
    ns_unused(source);
}

static void view_linux_data_source_dnd_finished(void *data, struct wl_data_source *source) {
    ns_unused(data);
    ns_unused(source);
}

static void view_linux_data_source_action(void *data, struct wl_data_source *source, uint32_t action) {
    ns_unused(data);
    ns_unused(source);
    ns_unused(action);
}

static const struct wl_data_source_listener view_linux_data_source_listener = {
    .target = view_linux_data_source_target,
    .send = view_linux_data_source_send,
    .cancelled = view_linux_data_source_cancelled,
    .dnd_drop_performed = view_linux_data_source_dnd_drop_performed,
    .dnd_finished = view_linux_data_source_dnd_finished,
    .action = view_linux_data_source_action,
};

static void view_linux_data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime_type) {
    ns_unused(data);
    ns_unused(offer);
    if (!_state.selection_mime && mime_type && strstr(mime_type, "text/plain")) {
        _state.selection_mime = strdup(mime_type);
    }
}

static void view_linux_data_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions) {
    ns_unused(data);
    ns_unused(offer);
    ns_unused(source_actions);
}

static void view_linux_data_offer_action(void *data, struct wl_data_offer *offer, uint32_t dnd_action) {
    ns_unused(data);
    ns_unused(offer);
    ns_unused(dnd_action);
}

static const struct wl_data_offer_listener view_linux_data_offer_listener = {
    .offer = view_linux_data_offer_offer,
    .source_actions = view_linux_data_offer_source_actions,
    .action = view_linux_data_offer_action,
};

static void view_linux_data_device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    ns_unused(data);
    ns_unused(device);
    wl_data_offer_add_listener(offer, &view_linux_data_offer_listener, &_state);
}

static void view_linux_data_device_enter(void *data, struct wl_data_device *device, uint32_t serial,
                                         struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                                         struct wl_data_offer *offer) {
    ns_unused(data);
    ns_unused(device);
    ns_unused(serial);
    ns_unused(surface);
    ns_unused(x);
    ns_unused(y);
    ns_unused(offer);
}

static void view_linux_data_device_leave(void *data, struct wl_data_device *device) {
    ns_unused(data);
    ns_unused(device);
}

static void view_linux_data_device_motion(void *data, struct wl_data_device *device, uint32_t time,
                                          wl_fixed_t x, wl_fixed_t y) {
    ns_unused(data);
    ns_unused(device);
    ns_unused(time);
    ns_unused(x);
    ns_unused(y);
}

static void view_linux_data_device_drop(void *data, struct wl_data_device *device) {
    ns_unused(data);
    ns_unused(device);
}

static void view_linux_data_device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    ns_unused(data);
    ns_unused(device);
    if (_state.selection_offer) {
        wl_data_offer_destroy(_state.selection_offer);
        _state.selection_offer = ns_null;
    }
    free(_state.selection_mime);
    _state.selection_mime = ns_null;
    if (offer) {
        _state.selection_offer = offer;
    }
}

static const struct wl_data_device_listener view_linux_data_device_listener = {
    .data_offer = view_linux_data_device_data_offer,
    .enter = view_linux_data_device_enter,
    .leave = view_linux_data_device_leave,
    .motion = view_linux_data_device_motion,
    .drop = view_linux_data_device_drop,
    .selection = view_linux_data_device_selection,
};

// ---- cursor -----------------------------------------------------------------

static void view_linux_set_cursor(void) {
    if (!_state.pointer || !_state.cursor_surface) return;
    if (!_state.cursor || _state.cursor->image_count == 0) return;
    struct wl_cursor_image *image = _state.cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    wl_pointer_set_cursor(_state.pointer, _state.enter_serial, _state.cursor_surface,
                          (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
    wl_surface_attach(_state.cursor_surface, buffer, 0, 0);
    wl_surface_damage(_state.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(_state.cursor_surface);
}

// ---- gamepads ---------------------------------------------------------------

#define VIEW_LINUX_LONG_BITS (8 * sizeof(unsigned long))
#define VIEW_LINUX_BIT_WORDS(n) (((n) + VIEW_LINUX_LONG_BITS - 1) / VIEW_LINUX_LONG_BITS)
// The same press threshold XInput uses for its triggers (30 / 255).
#define VIEW_LINUX_TRIGGER_THRESHOLD 0.12f
#define VIEW_LINUX_VENDOR_VALVE 0x28de
#define VIEW_LINUX_PRODUCT_STEAM_VIRTUAL_PAD 0x11ff
#define VIEW_LINUX_VENDOR_SONY 0x054c

static ns_bool view_linux_bit(const unsigned long *bits, i32 bit) {
    return (bits[bit / VIEW_LINUX_LONG_BITS] >> (bit % VIEW_LINUX_LONG_BITS)) & 1UL;
}

static i32 view_linux_gamepad_button_map(const view_linux_gamepad *pad, i32 code) {
    if (pad->joystick_buttons) {
        i32 index = code - BTN_JOYSTICK;
        return index >= 0 && index < 16 ? index : -1;
    }
    switch (code) {
        case BTN_SOUTH: return VIEW_GAMEPAD_BUTTON_SOUTH;
        case BTN_EAST: return VIEW_GAMEPAD_BUTTON_EAST;
        case BTN_X: return pad->positional_face ? VIEW_GAMEPAD_BUTTON_NORTH : VIEW_GAMEPAD_BUTTON_WEST;
        case BTN_Y: return pad->positional_face ? VIEW_GAMEPAD_BUTTON_WEST : VIEW_GAMEPAD_BUTTON_NORTH;
        case BTN_TL: return VIEW_GAMEPAD_BUTTON_LEFT_SHOULDER;
        case BTN_TR: return VIEW_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        case BTN_TL2: return VIEW_GAMEPAD_BUTTON_LEFT_TRIGGER;
        case BTN_TR2: return VIEW_GAMEPAD_BUTTON_RIGHT_TRIGGER;
        case BTN_SELECT: return VIEW_GAMEPAD_BUTTON_SELECT;
        case BTN_START: return VIEW_GAMEPAD_BUTTON_START;
        case BTN_MODE: return VIEW_GAMEPAD_BUTTON_HOME;
        case BTN_THUMBL: return VIEW_GAMEPAD_BUTTON_LEFT_STICK;
        case BTN_THUMBR: return VIEW_GAMEPAD_BUTTON_RIGHT_STICK;
        case BTN_DPAD_UP: return VIEW_GAMEPAD_BUTTON_DPAD_UP;
        case BTN_DPAD_DOWN: return VIEW_GAMEPAD_BUTTON_DPAD_DOWN;
        case BTN_DPAD_LEFT: return VIEW_GAMEPAD_BUTTON_DPAD_LEFT;
        case BTN_DPAD_RIGHT: return VIEW_GAMEPAD_BUTTON_DPAD_RIGHT;
        default: return -1;
    }
}

static void view_linux_gamepad_key(i32 slot, i32 code, i32 value) {
    view_linux_gamepad *pad = &_state.gamepads[slot];
    i32 button = view_linux_gamepad_button_map(pad, code);
    if (button < 0) return;
    // An analog trigger axis already publishes both the value and the press;
    // the digital click some drivers add would only fight it.
    if (!pad->joystick_buttons) {
        if (button == VIEW_GAMEPAD_BUTTON_LEFT_TRIGGER && pad->analog_trigger[0]) return;
        if (button == VIEW_GAMEPAD_BUTTON_RIGHT_TRIGGER && pad->analog_trigger[1]) return;
    }
    // value 2 is autorepeat, still held.
    ns_bool pressed = value != 0;
    view_on_gamepad_button(&_view, slot, button, pressed ? 1.0f : 0.0f, pressed);
}

static void view_linux_gamepad_abs(i32 slot, i32 code, i32 value) {
    if (code < 0 || code >= ABS_CNT) return;
    view_linux_gamepad *pad = &_state.gamepads[slot];
    i32 role = pad->abs_role[code];
    if (role == VIEW_LINUX_ABS_NONE) return;
    i32 min = pad->abs_min[code];
    i32 max = pad->abs_max[code];
    if (max <= min) return;
    f32 t = (f32)((f64)(value - min) / (f64)(max - min));
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (role >= VIEW_GAMEPAD_AXIS_LEFT_X && role <= VIEW_GAMEPAD_AXIS_RIGHT_Y) {
        // evdev sticks already grow right and down, as the standard layout does.
        view_on_gamepad_axis(&_view, slot, role, t * 2.0f - 1.0f);
    } else if (role == VIEW_LINUX_ABS_LEFT_TRIGGER || role == VIEW_LINUX_ABS_RIGHT_TRIGGER) {
        i32 button = role == VIEW_LINUX_ABS_LEFT_TRIGGER ? VIEW_GAMEPAD_BUTTON_LEFT_TRIGGER
                                                         : VIEW_GAMEPAD_BUTTON_RIGHT_TRIGGER;
        view_on_gamepad_button(&_view, slot, button, t, t > VIEW_LINUX_TRIGGER_THRESHOLD);
    } else {
        // A hat reports -1 / 0 / 1 around the middle of its range.
        f64 center = ((f64)min + (f64)max) * 0.5;
        ns_bool negative = (f64)value < center - 0.25;
        ns_bool positive = (f64)value > center + 0.25;
        i32 low = role == VIEW_LINUX_ABS_HAT_X ? VIEW_GAMEPAD_BUTTON_DPAD_LEFT : VIEW_GAMEPAD_BUTTON_DPAD_UP;
        i32 high = role == VIEW_LINUX_ABS_HAT_X ? VIEW_GAMEPAD_BUTTON_DPAD_RIGHT : VIEW_GAMEPAD_BUTTON_DPAD_DOWN;
        view_on_gamepad_button(&_view, slot, low, negative ? 1.0f : 0.0f, negative);
        view_on_gamepad_button(&_view, slot, high, positive ? 1.0f : 0.0f, positive);
    }
}

// Read the complete key and axis state back from the device: on connect, and
// after the kernel dropped events because the queue overflowed.
static void view_linux_gamepad_sync(i32 slot) {
    view_linux_gamepad *pad = &_state.gamepads[slot];
    unsigned long keys[VIEW_LINUX_BIT_WORDS(KEY_CNT)];
    memset(keys, 0, sizeof(keys));
    if (ioctl(pad->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
        for (i32 code = BTN_JOYSTICK; code <= BTN_THUMBR; code++) {
            view_linux_gamepad_key(slot, code, view_linux_bit(keys, code) ? 1 : 0);
        }
        for (i32 code = BTN_DPAD_UP; code <= BTN_DPAD_RIGHT; code++) {
            view_linux_gamepad_key(slot, code, view_linux_bit(keys, code) ? 1 : 0);
        }
    }
    for (i32 code = 0; code < ABS_CNT; code++) {
        if (pad->abs_role[code] == VIEW_LINUX_ABS_NONE) continue;
        struct input_absinfo info;
        if (ioctl(pad->fd, EVIOCGABS(code), &info) < 0) continue;
        view_linux_gamepad_abs(slot, code, info.value);
    }
}

static void view_linux_gamepad_close(i32 slot) {
    view_linux_gamepad *pad = &_state.gamepads[slot];
    if (pad->fd < 0) return;
    close(pad->fd);
    pad->fd = -1;
    pad->device = 0;
    view_on_gamepad_connected(&_view, slot, false);
}

static void view_linux_gamepad_assign(view_linux_gamepad *pad, const unsigned long *abs_bits, i32 code, i32 role) {
    if (!view_linux_bit(abs_bits, code) || pad->abs_role[code] != VIEW_LINUX_ABS_NONE) return;
    struct input_absinfo info;
    if (ioctl(pad->fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum) return;
    pad->abs_role[code] = role;
    pad->abs_min[code] = info.minimum;
    pad->abs_max[code] = info.maximum;
    if (role == VIEW_LINUX_ABS_LEFT_TRIGGER) pad->analog_trigger[0] = true;
    if (role == VIEW_LINUX_ABS_RIGHT_TRIGGER) pad->analog_trigger[1] = true;
}

// Open one event node into a free slot if it is a gamepad or joystick; any
// other device (keyboard, touchpad, motion sensors) is closed again.
static ns_bool view_linux_gamepad_open(i32 slot, const char *path, dev_t device) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    unsigned long ev_bits[VIEW_LINUX_BIT_WORDS(EV_CNT)];
    unsigned long key_bits[VIEW_LINUX_BIT_WORDS(KEY_CNT)];
    unsigned long abs_bits[VIEW_LINUX_BIT_WORDS(ABS_CNT)];
    memset(ev_bits, 0, sizeof(ev_bits));
    memset(key_bits, 0, sizeof(key_bits));
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        close(fd);
        return false;
    }
    ns_bool gamepad = view_linux_bit(key_bits, BTN_GAMEPAD);
    ns_bool joystick = view_linux_bit(key_bits, BTN_JOYSTICK);
    if (!view_linux_bit(ev_bits, EV_KEY) || !view_linux_bit(ev_bits, EV_ABS) ||
        !view_linux_bit(abs_bits, ABS_X) || (!gamepad && !joystick)) {
        close(fd);
        return false;
    }
    struct input_id id;
    memset(&id, 0, sizeof(id));
    ioctl(fd, EVIOCGID, &id);

    view_linux_gamepad *pad = &_state.gamepads[slot];
    memset(pad, 0, sizeof(*pad));
    pad->fd = fd;
    pad->device = device;
    pad->hid_steam = id.vendor == VIEW_LINUX_VENDOR_VALVE && id.product != VIEW_LINUX_PRODUCT_STEAM_VIRTUAL_PAD;
    pad->positional_face = id.vendor == VIEW_LINUX_VENDOR_SONY;
    pad->joystick_buttons = !gamepad;
    for (i32 code = 0; code < ABS_CNT; code++) pad->abs_role[code] = VIEW_LINUX_ABS_NONE;

    view_linux_gamepad_assign(pad, abs_bits, ABS_X, VIEW_GAMEPAD_AXIS_LEFT_X);
    view_linux_gamepad_assign(pad, abs_bits, ABS_Y, VIEW_GAMEPAD_AXIS_LEFT_Y);
    if (view_linux_bit(abs_bits, ABS_RX)) {
        view_linux_gamepad_assign(pad, abs_bits, ABS_RX, VIEW_GAMEPAD_AXIS_RIGHT_X);
        view_linux_gamepad_assign(pad, abs_bits, ABS_RY, VIEW_GAMEPAD_AXIS_RIGHT_Y);
        if (pad->hid_steam) {
            view_linux_gamepad_assign(pad, abs_bits, ABS_HAT2Y, VIEW_LINUX_ABS_LEFT_TRIGGER);
            view_linux_gamepad_assign(pad, abs_bits, ABS_HAT2X, VIEW_LINUX_ABS_RIGHT_TRIGGER);
        } else {
            view_linux_gamepad_assign(pad, abs_bits, ABS_Z, VIEW_LINUX_ABS_LEFT_TRIGGER);
            view_linux_gamepad_assign(pad, abs_bits, ABS_RZ, VIEW_LINUX_ABS_RIGHT_TRIGGER);
        }
    } else {
        // DirectInput-style pads put the right stick on Z / RZ.
        view_linux_gamepad_assign(pad, abs_bits, ABS_Z, VIEW_GAMEPAD_AXIS_RIGHT_X);
        view_linux_gamepad_assign(pad, abs_bits, ABS_RZ, VIEW_GAMEPAD_AXIS_RIGHT_Y);
    }
    view_linux_gamepad_assign(pad, abs_bits, ABS_BRAKE, VIEW_LINUX_ABS_LEFT_TRIGGER);
    view_linux_gamepad_assign(pad, abs_bits, ABS_GAS, VIEW_LINUX_ABS_RIGHT_TRIGGER);
    if (!pad->hid_steam) {
        view_linux_gamepad_assign(pad, abs_bits, ABS_HAT0X, VIEW_LINUX_ABS_HAT_X);
        view_linux_gamepad_assign(pad, abs_bits, ABS_HAT0Y, VIEW_LINUX_ABS_HAT_Y);
    }

    view_on_gamepad_connected(&_view, slot, true);
    view_linux_gamepad_sync(slot);
    return true;
}

static int view_linux_compare_i32(const void *a, const void *b) {
    i32 x = *(const i32 *)a;
    i32 y = *(const i32 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Fill free slots from /dev/input/event*, lowest node first, so the first pad
// the system found stays gamepad 0.
static void view_linux_gamepad_scan(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    i32 nodes[256];
    i32 node_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && node_count < (i32)(sizeof(nodes) / sizeof(nodes[0]))) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char *end = ns_null;
        long index = strtol(entry->d_name + 5, &end, 10);
        if (end == entry->d_name + 5 || *end != '\0' || index < 0) continue;
        nodes[node_count++] = (i32)index;
    }
    closedir(dir);
    qsort(nodes, (size_t)node_count, sizeof(nodes[0]), view_linux_compare_i32);

    for (i32 n = 0; n < node_count; n++) {
        i32 slot = -1;
        for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) {
            if (_state.gamepads[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", nodes[n]);
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISCHR(info.st_mode)) continue;
        ns_bool open_already = false;
        for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) {
            if (_state.gamepads[i].fd >= 0 && _state.gamepads[i].device == info.st_rdev) open_already = true;
        }
        if (open_already) continue;
        view_linux_gamepad_open(slot, path, info.st_rdev);
    }
}

static void view_linux_gamepad_read(i32 slot) {
    view_linux_gamepad *pad = &_state.gamepads[slot];
    struct input_event events[64];
    while (pad->fd >= 0) {
        ssize_t n = read(pad->fd, events, sizeof(events));
        if (n < 0) {
            if (errno == EINTR) continue;
            // ENODEV: unplugged. Anything else leaves nothing to read either.
            if (errno != EAGAIN) view_linux_gamepad_close(slot);
            return;
        }
        if (n == 0) return;
        i32 count = (i32)((size_t)n / sizeof(events[0]));
        for (i32 i = 0; i < count; i++) {
            struct input_event *event = &events[i];
            if (event->type == EV_SYN) {
                if (event->code == SYN_DROPPED) {
                    pad->dropped = true;
                } else if (event->code == SYN_REPORT && pad->dropped) {
                    pad->dropped = false;
                    view_linux_gamepad_sync(slot);
                }
                continue;
            }
            if (pad->dropped) continue;
            if (event->type == EV_KEY) {
                view_linux_gamepad_key(slot, event->code, event->value);
            } else if (event->type == EV_ABS) {
                view_linux_gamepad_abs(slot, event->code, event->value);
            }
        }
    }
}

static void view_linux_gamepad_start(void) {
    for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) _state.gamepads[i].fd = -1;
    // IN_ATTRIB matters: udev creates the node first and grants the seat's user
    // access to it a moment later.
    _state.input_notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (_state.input_notify_fd >= 0 &&
        inotify_add_watch(_state.input_notify_fd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_MOVED_TO) < 0) {
        close(_state.input_notify_fd);
        _state.input_notify_fd = -1;
    }
    view_linux_gamepad_scan();
}

static void view_linux_gamepad_stop(void) {
    for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) view_linux_gamepad_close(i);
    if (_state.input_notify_fd >= 0) close(_state.input_notify_fd);
    _state.input_notify_fd = -1;
}

// Drain every input fd without blocking: hotplug notices first, then each
// pad's queued events. Called on every loop turn, since a free-running loop
// never reaches the poll below.
static void view_linux_gamepad_pump(void) {
    if (_state.input_notify_fd >= 0) {
        char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        ns_bool rescan = false;
        for (;;) {
            ssize_t n = read(_state.input_notify_fd, buffer, sizeof(buffer));
            if (n <= 0) break;
            for (char *cursor = buffer; cursor < buffer + n;) {
                struct inotify_event *event = (struct inotify_event *)cursor;
                if (event->len > 0 && strncmp(event->name, "event", 5) == 0) rescan = true;
                cursor += sizeof(struct inotify_event) + event->len;
            }
        }
        if (rescan) view_linux_gamepad_scan();
    }
    for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) {
        if (_state.gamepads[i].fd >= 0) view_linux_gamepad_read(i);
    }
}

// ---- frame loop -------------------------------------------------------------

typedef void (*view_linux_gpu_frame_fn)(view *v);

static view_linux_gpu_frame_fn view_linux_gpu_begin;
static view_linux_gpu_frame_fn view_linux_gpu_end;
static ns_bool view_linux_gpu_resolved;

static void view_linux_resolve_gpu(void) {
    if (view_linux_gpu_resolved) return;
    view_linux_gpu_resolved = true;
    view_linux_gpu_begin = (view_linux_gpu_frame_fn)dlsym(RTLD_DEFAULT, "gpu_vk_begin_frame");
    view_linux_gpu_end = (view_linux_gpu_frame_fn)dlsym(RTLD_DEFAULT, "gpu_vk_end_frame");
}

static void view_linux_draw_frame(view *v) {
    view_linux_resolve_gpu();
    view_on_frame frame = (view_on_frame)v->on_frame;
    if (frame) {
        if (view_linux_gpu_begin) view_linux_gpu_begin(v);
        frame(v);
        if (view_linux_gpu_end) view_linux_gpu_end(v);
    }
    view_complete_frame(v);
}

// Dispatch everything the compositor has queued without blocking.
static void view_linux_pump_events(void) {
    view_linux_gamepad_pump();
    if (wl_display_dispatch_pending(_state.display) < 0) {
        _state.running = false;
        return;
    }
    if (wl_display_flush(_state.display) < 0 && errno != EAGAIN) {
        _state.running = false;
    }
}

// Block until the compositor has something, the application wakes the loop,
// or a scheduled frame falls due.
static void view_linux_wait(void) {
    while (wl_display_prepare_read(_state.display) != 0) {
        if (wl_display_dispatch_pending(_state.display) < 0) {
            _state.running = false;
            return;
        }
    }
    if (wl_display_flush(_state.display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(_state.display);
        _state.running = false;
        return;
    }
    struct pollfd fds[3 + VIEW_GAMEPAD_CAPACITY] = {
        {.fd = wl_display_get_fd(_state.display), .events = POLLIN},
        {.fd = _state.event_fd, .events = POLLIN},
    };
    nfds_t fd_count = 2;
    if (_state.input_notify_fd >= 0) fds[fd_count++] = (struct pollfd){.fd = _state.input_notify_fd, .events = POLLIN};
    for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) {
        if (_state.gamepads[i].fd >= 0) fds[fd_count++] = (struct pollfd){.fd = _state.gamepads[i].fd, .events = POLLIN};
    }
    i32 timeout = -1;
    if (_state.timer_deadline > 0) {
        i64 remaining = _state.timer_deadline - view_linux_now_ms();
        timeout = remaining > 0 ? (i32)remaining : 0;
    }
    if (_state.frame_requests > 0) timeout = 0;
    i32 ready = poll(fds, fd_count, timeout);
    if (ready < 0 && errno != EINTR) {
        wl_display_cancel_read(_state.display);
        _state.running = false;
        return;
    }
    if (ready > 0 && (fds[0].revents & POLLIN)) {
        if (wl_display_read_events(_state.display) < 0) {
            _state.running = false;
            return;
        }
    } else {
        wl_display_cancel_read(_state.display);
    }
    if (ready > 0 && (fds[1].revents & POLLIN)) {
        u64 drain;
        while (read(_state.event_fd, &drain, sizeof(drain)) > 0) {}
    }
    view_linux_gamepad_pump();
    if (wl_display_dispatch_pending(_state.display) < 0) {
        _state.running = false;
    }
    if (_state.timer_deadline > 0 && view_linux_now_ms() >= _state.timer_deadline) {
        _state.timer_deadline = 0;
        view_request_frame(&_view, 1);
    }
}

static void view_linux_finish(view *v) {
    if (_state.finished) return;
    _state.finished = true;
    view_on_terminate terminate = (view_on_terminate)v->on_terminate;
    if (terminate) terminate(v);
    view_linux_gamepad_stop();
}

// ---- public backend surface -------------------------------------------------

view *view_create(const char *title, i32 width, i32 height) {
    memset(&_state, 0, sizeof(_state));
    _state.event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    _state.input_notify_fd = -1;
    for (i32 i = 0; i < VIEW_GAMEPAD_CAPACITY; i++) _state.gamepads[i].fd = -1;

    _state.display = wl_display_connect(ns_null);
    if (!_state.display) {
        // No compositor: keep the no-op behavior so headless programs run.
        _view.width = width;
        _view.height = height;
        _view.framebuffer_width = width;
        _view.framebuffer_height = height;
        _view.display_ratio = 1.0;
        _view.ui_scale = 1.0;
        _view.title = ns_str_cstr((char *)title);
        _view.native_window = ns_null;
        return &_view;
    }

    _state.registry = wl_display_get_registry(_state.display);
    wl_registry_add_listener(_state.registry, &view_linux_registry_listener, &_state);
    wl_display_roundtrip(_state.display);

    if (!_state.compositor || !_state.wm_base) {
        wl_display_disconnect(_state.display);
        _state.display = ns_null;
        _view.width = width;
        _view.height = height;
        _view.framebuffer_width = width;
        _view.framebuffer_height = height;
        _view.display_ratio = 1.0;
        _view.ui_scale = 1.0;
        _view.title = ns_str_cstr((char *)title);
        _view.native_window = ns_null;
        return &_view;
    }

    if (_state.wm_base) xdg_wm_base_add_listener(_state.wm_base, &view_linux_wm_base_listener, &_state);
    if (_state.seat) wl_seat_add_listener(_state.seat, &view_linux_seat_listener, &_state);

    _state.surface = wl_compositor_create_surface(_state.compositor);
    _state.xdg_surface = xdg_wm_base_get_xdg_surface(_state.wm_base, _state.surface);
    xdg_surface_add_listener(_state.xdg_surface, &view_linux_surface_listener, &_state);
    _state.toplevel = xdg_surface_get_toplevel(_state.xdg_surface);
    xdg_toplevel_add_listener(_state.toplevel, &view_linux_toplevel_listener, &_state);
    if (title && title[0]) xdg_toplevel_set_title(_state.toplevel, title);
    xdg_toplevel_set_app_id(_state.toplevel, "ns");
    xdg_toplevel_set_min_size(_state.toplevel, 320, 200);
    wl_surface_commit(_state.surface);

    if (_state.data_device_manager && _state.seat) {
        _state.data_device = wl_data_device_manager_get_data_device(_state.data_device_manager, _state.seat);
        if (_state.data_device) {
            wl_data_device_add_listener(_state.data_device, &view_linux_data_device_listener, &_state);
        }
    }

    if (_state.shm) {
        _state.cursor_theme = wl_cursor_theme_load(ns_null, 24, _state.shm);
        if (_state.cursor_theme) {
            _state.cursor = wl_cursor_theme_get_cursor(_state.cursor_theme, "left_ptr");
            if (_state.cursor && _state.compositor) {
                _state.cursor_surface = wl_compositor_create_surface(_state.compositor);
            }
        }
    }

    // Wait for the first configure so the drawable size is known before the
    // caller creates a swapchain for it.
    _state.pending_width = width;
    _state.pending_height = height;
    for (i32 i = 0; i < 4 && !_state.configured && _state.display; i++) {
        if (wl_display_roundtrip(_state.display) < 0) break;
    }

    _view.width = _state.pending_width > 0 ? _state.pending_width : width;
    _view.height = _state.pending_height > 0 ? _state.pending_height : height;
    _view.display_ratio = 1.0;
    _view.ui_scale = 1.0;
    _view.framebuffer_width = _view.width;
    _view.framebuffer_height = _view.height;
    _view.title = ns_str_cstr((char *)title);
    _view.native_window = _state.surface;
    _view.safe_area_top = 0.0;
    _view.safe_area_right = 0.0;
    _view.safe_area_bottom = 0.0;
    _view.safe_area_left = 0.0;
    // Pads already plugged in are connected before on_launch runs.
    view_linux_gamepad_start();
    return &_view;
}

view *view_create_no_title(const char *title, i32 width, i32 height) {
    return view_create(title, width, height);
}

// The Vulkan backend builds its surface from the display and the view's
// native_window (the wl_surface).
void *view_linux_display(void) {
    return _state.display;
}

void view_run(view *v) {
    if (!v || !_state.display) return;
    view_linux_resolve_gpu();
    view_on_launch launch = (view_on_launch)v->on_launch;
    if (launch) launch(v);
    _state.running = true;
    _state.closed = false;
    view_request_frame(v, 1);
    while (_state.running) {
        view_linux_pump_events();
        if (!_state.running) break;
        if (view_take_frame_request(v)) {
            _state.frame_requests = 0;
            view_linux_draw_frame(v);
            continue;
        }
        view_linux_wait();
    }
    view_linux_finish(v);
}

void view_platform_request_frame(view *v) {
    ns_unused(v);
    _state.frame_requests++;
    view_linux_wake();
}

void view_platform_request_frame_after(view *v, i32 milliseconds) {
    ns_unused(v);
    if (milliseconds <= 0) {
        view_platform_request_frame(v);
        return;
    }
    _state.timer_deadline = view_linux_now_ms() + milliseconds;
    view_linux_wake();
}

void view_platform_set_frame_per_second(view *v, i32 frames) {
    ns_unused(v);
    ns_unused(frames);
    // FIFO presentation paces continuous frames to the display's refresh.
    view_linux_wake();
}

void view_platform_close(view *v) {
    ns_unused(v);
    _state.running = false;
    _state.closed = true;
    view_linux_wake();
}

void view_platform_set_fullscreen(view *v, ns_bool fullscreen) {
    ns_unused(v);
    if (!_state.toplevel) return;
    if (fullscreen) {
        xdg_toplevel_set_fullscreen(_state.toplevel, ns_null);
    } else {
        xdg_toplevel_unset_fullscreen(_state.toplevel);
    }
    // The request is buffered, so send it before the event loop starts; the
    // configure that carries the new size then arrives through the normal
    // surface resize path.
    if (_state.display) wl_display_flush(_state.display);
}

const char *view_get_clipboard(view *v) {
    ns_unused(v);
    if (!_state.selection_offer) return ns_null;
    const char *mime = _state.selection_mime ? _state.selection_mime : "text/plain;charset=utf-8";
    int fds[2];
    if (pipe(fds) != 0) return ns_null;
    wl_data_offer_receive(_state.selection_offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(_state.display);

    size_t capacity = 4096;
    size_t length = 0;
    char *text = (char *)malloc(capacity);
    if (!text) {
        close(fds[0]);
        return ns_null;
    }
    for (;;) {
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        i32 ready = poll(&pfd, 1, 250);
        if (ready <= 0) break;
        if (length + 1024 + 1 > capacity) {
            capacity *= 2;
            char *grown = (char *)realloc(text, capacity);
            if (!grown) break;
            text = grown;
        }
        ssize_t n = read(fds[0], text + length, 1024);
        if (n <= 0) break;
        length += (size_t)n;
    }
    close(fds[0]);
    text[length] = '\0';
    free(_state.clipboard);
    _state.clipboard = text;
    return _state.clipboard;
}

void view_set_clipboard(view *v, const char *text) {
    ns_unused(v);
    if (!_state.data_device_manager || !_state.data_device) return;
    free(_state.clipboard);
    _state.clipboard = strdup(text ? text : "");
    if (_state.data_source) {
        wl_data_source_destroy(_state.data_source);
        _state.data_source = ns_null;
    }
    _state.data_source = wl_data_device_manager_create_data_source(_state.data_device_manager);
    wl_data_source_add_listener(_state.data_source, &view_linux_data_source_listener, &_state);
    wl_data_source_offer(_state.data_source, "text/plain;charset=utf-8");
    wl_data_source_offer(_state.data_source, "text/plain");
    wl_data_source_offer(_state.data_source, "UTF8_STRING");
    wl_data_device_set_selection(_state.data_device, _state.data_source,
                                 _state.last_serial ? _state.last_serial : _state.enter_serial);
    wl_display_flush(_state.display);
}

#endif // NS_LINUX
