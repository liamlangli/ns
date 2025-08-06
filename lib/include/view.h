#pragma once

#include "ns_type.h"

typedef enum view_mouse_button {
    VIEW_MOUSE_BUTTON_LEFT = 0,
    VIEW_MOUSE_BUTTON_RIGHT = 1,
    VIEW_MOUSE_BUTTON_MIDDLE = 2
} view_mouse_button;

typedef enum view_button_action {
    VIEW_BUTTON_ACTION_PRESS = 0,
    VIEW_BUTTON_ACTION_RELEASE = 1
} view_button_action;

typedef enum view_keycode {
    VIEW_KEY_SPACE = 32,
    VIEW_KEY_APOSTROPHE = 39 /* ' */,
    VIEW_KEY_LEFT_PARENTHESIS = 40 /* ( */,
    VIEW_KEY_RIGHT_PARENTHESIS = 41 /* ) */,
    VIEW_KEY_COMMA = 44 /* */,
    VIEW_KEY_MINUS = 45 /* - */,
    VIEW_KEY_PERIOD = 46 /* . */,
    VIEW_KEY_SLASH = 47 /* / */,
    VIEW_KEY_0 = 48,
    VIEW_KEY_1 = 49,
    VIEW_KEY_2 = 50,
    VIEW_KEY_3 = 51,
    VIEW_KEY_4 = 52,
    VIEW_KEY_5 = 53,
    VIEW_KEY_6 = 54,
    VIEW_KEY_7 = 55,
    VIEW_KEY_8 = 56,
    VIEW_KEY_9 = 57,
    VIEW_KEY_SEMICOLON = 59 /* ; */,
    VIEW_KEY_EQUAL = 61 /* = */,
    VIEW_KEY_A = 65,
    VIEW_KEY_B = 66,
    VIEW_KEY_C = 67,
    VIEW_KEY_D = 68,
    VIEW_KEY_E = 69,
    VIEW_KEY_F = 70,
    VIEW_KEY_G = 71,
    VIEW_KEY_H = 72,
    VIEW_KEY_I = 73,
    VIEW_KEY_J = 74,
    VIEW_KEY_K = 75,
    VIEW_KEY_L = 76,
    VIEW_KEY_M = 77,
    VIEW_KEY_N = 78,
    VIEW_KEY_O = 79,
    VIEW_KEY_P = 80,
    VIEW_KEY_Q = 81,
    VIEW_KEY_R = 82,
    VIEW_KEY_S = 83,
    VIEW_KEY_T = 84,
    VIEW_KEY_U = 85,
    VIEW_KEY_V = 86,
    VIEW_KEY_W = 87,
    VIEW_KEY_X = 88,
    VIEW_KEY_Y = 89,
    VIEW_KEY_Z = 90,
    VIEW_KEY_LEFT_BRACKET = 91 /* [ */,
    VIEW_KEY_BACKSLASH = 92 /* \ */,
    VIEW_KEY_RIGHT_BRACKET = 93 /* ] */,
    VIEW_KEY_GRAVE_ACCENT = 96 /* ` */,
    VIEW_KEY_WORLD_1 = 161 /* non-US #1 */,
    VIEW_KEY_WORLD_2 = 162 /* non-US #2 */,
    VIEW_KEY_ESCAPE = 256,
    VIEW_KEY_ENTER = 257,
    VIEW_KEY_TAB = 258,
    VIEW_KEY_BACKSPACE = 259,
    VIEW_KEY_INSERT = 260,
    VIEW_KEY_DELETE = 261,
    VIEW_KEY_RIGHT = 262,
    VIEW_KEY_LEFT = 263,
    VIEW_KEY_DOWN = 264,
    VIEW_KEY_UP = 265,
    VIEW_KEY_PAGE_UP = 266,
    VIEW_KEY_PAGE_DOWN = 267,
    VIEW_KEY_HOME = 268,
    VIEW_KEY_END = 269,
    VIEW_KEY_CAPS_LOCK = 280,
    VIEW_KEY_SCROLL_LOCK = 281,
    VIEW_KEY_NUM_LOCK = 282,
    VIEW_KEY_PRINT_SCREEN = 283,
    VIEW_KEY_PAUSE = 284,
    VIEW_KEY_F1 = 290,
    VIEW_KEY_F2 = 291,
    VIEW_KEY_F3 = 292,
    VIEW_KEY_F4 = 293,
    VIEW_KEY_F5 = 294,
    VIEW_KEY_F6 = 295,
    VIEW_KEY_F7 = 296,
    VIEW_KEY_F8 = 297,
    VIEW_KEY_F9 = 298,
    VIEW_KEY_F10 = 299,
    VIEW_KEY_F11 = 300,
    VIEW_KEY_F12 = 301,
    VIEW_KEY_F13 = 302,
    VIEW_KEY_F14 = 303,
    VIEW_KEY_F15 = 304,
    VIEW_KEY_F16 = 305,
    VIEW_KEY_F17 = 306,
    VIEW_KEY_F18 = 307,
    VIEW_KEY_F19 = 308,
    VIEW_KEY_F20 = 309,
    VIEW_KEY_F21 = 310,
    VIEW_KEY_F22 = 311,
    VIEW_KEY_F23 = 312,
    VIEW_KEY_F24 = 313,
    VIEW_KEY_F25 = 314,
    VIEW_KEY_KP_0 = 320,
    VIEW_KEY_KP_1 = 321,
    VIEW_KEY_KP_2 = 322,
    VIEW_KEY_KP_3 = 323,
    VIEW_KEY_KP_4 = 324,
    VIEW_KEY_KP_5 = 325,
    VIEW_KEY_KP_6 = 326,
    VIEW_KEY_KP_7 = 327,
    VIEW_KEY_KP_8 = 328,
    VIEW_KEY_KP_9 = 329,
    VIEW_KEY_KP_DECIMAL = 330,
    VIEW_KEY_KP_DIVIDE = 331,
    VIEW_KEY_KP_MULTIPLY = 332,
    VIEW_KEY_KP_SUBTRACT = 333,
    VIEW_KEY_KP_ADD = 334,
    VIEW_KEY_KP_ENTER = 335,
    VIEW_KEY_KP_EQUAL = 336,
    VIEW_KEY_LEFT_SHIFT = 340,
    VIEW_KEY_LEFT_CONTROL = 341,
    VIEW_KEY_LEFT_ALT = 342,
    VIEW_KEY_LEFT_SUPER = 343,
    VIEW_KEY_RIGHT_SHIFT = 344,
    VIEW_KEY_RIGHT_CONTROL = 345,
    VIEW_KEY_RIGHT_ALT = 346,
    VIEW_KEY_RIGHT_SUPER = 347,
    VIEW_KEY_MENU = 348,
} view_keycode;

typedef struct view {
    ns_str title;
    i32 width;
    i32 height;
    i32 framebuffer_width;
    i32 framebuffer_height;

    f64 mouse_x, mouse_y;

    f64 display_ratio;
    f64 ui_scale;
    void *native_window;
    void *gpu_device;

    ns_bool capture_required, capture_started;

    void *on_launch;
    void *on_frame;
    void *on_terminate;
} view;

// View management functions
view* view_create(const char *title, i32 width, i32 height);
void view_close(view *v);
void view_capture_require(view *v);

// View event handling functions
void view_on_scroll(view *v, f64 x, f64 y);
void view_on_resize(view *v, i32 width, i32 height);
void view_on_mouse_move(view *v, f64 x, f64 y);
void view_on_mouse_btn(view *v, view_mouse_button button, view_button_action action);
void view_on_key_action(view *v, view_keycode key, view_button_action action);
ns_bool view_is_key_pressed(view *v, view_keycode key);

// Clipboard functions
ns_str view_get_clipboard(view *v);
void view_set_clipboard(view *v, ns_str text);
