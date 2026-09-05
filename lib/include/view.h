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

typedef enum view_key_modifier {
    VIEW_KEY_MOD_SHIFT = 1 << 0,
    VIEW_KEY_MOD_CONTROL = 1 << 1,
    VIEW_KEY_MOD_ALT = 1 << 2,
    VIEW_KEY_MOD_SUPER = 1 << 3,
} view_key_modifier;

// Unified pointer stream used by native editors and drawing applications.
// Mouse compatibility fields remain on `view`; this queue adds multi-touch,
// Pencil metadata and indirect-pointer events without changing that API.
typedef enum view_input_device {
    VIEW_INPUT_DEVICE_MOUSE = 0,
    VIEW_INPUT_DEVICE_TOUCH = 1,
    VIEW_INPUT_DEVICE_PENCIL = 2,
    VIEW_INPUT_DEVICE_INDIRECT = 3,
    VIEW_INPUT_DEVICE_TOOL = 4,
} view_input_device;

typedef enum view_input_phase {
    VIEW_INPUT_PHASE_HOVER = 0,
    VIEW_INPUT_PHASE_BEGAN = 1,
    VIEW_INPUT_PHASE_MOVED = 2,
    VIEW_INPUT_PHASE_ENDED = 3,
    VIEW_INPUT_PHASE_CANCELLED = 4,
    VIEW_INPUT_PHASE_TOOL_ACTION = 5,
} view_input_phase;

typedef enum view_tool_action {
    VIEW_TOOL_ACTION_NONE = 0,
    VIEW_TOOL_ACTION_PENCIL_DOUBLE_TAP = 1,
    VIEW_TOOL_ACTION_PENCIL_SQUEEZE = 2,
} view_tool_action;

#define VIEW_GAMEPAD_CAPACITY 4
#define VIEW_GAMEPAD_AXIS_CAPACITY 4
#define VIEW_GAMEPAD_BUTTON_CAPACITY 17

typedef enum view_gamepad_axis_id {
    VIEW_GAMEPAD_AXIS_LEFT_X = 0,
    VIEW_GAMEPAD_AXIS_LEFT_Y = 1,
    VIEW_GAMEPAD_AXIS_RIGHT_X = 2,
    VIEW_GAMEPAD_AXIS_RIGHT_Y = 3,
} view_gamepad_axis_id;

typedef enum view_gamepad_button_id {
    VIEW_GAMEPAD_BUTTON_SOUTH = 0,
    VIEW_GAMEPAD_BUTTON_EAST = 1,
    VIEW_GAMEPAD_BUTTON_WEST = 2,
    VIEW_GAMEPAD_BUTTON_NORTH = 3,
    VIEW_GAMEPAD_BUTTON_LEFT_SHOULDER = 4,
    VIEW_GAMEPAD_BUTTON_RIGHT_SHOULDER = 5,
    VIEW_GAMEPAD_BUTTON_LEFT_TRIGGER = 6,
    VIEW_GAMEPAD_BUTTON_RIGHT_TRIGGER = 7,
    VIEW_GAMEPAD_BUTTON_SELECT = 8,
    VIEW_GAMEPAD_BUTTON_START = 9,
    VIEW_GAMEPAD_BUTTON_LEFT_STICK = 10,
    VIEW_GAMEPAD_BUTTON_RIGHT_STICK = 11,
    VIEW_GAMEPAD_BUTTON_DPAD_UP = 12,
    VIEW_GAMEPAD_BUTTON_DPAD_DOWN = 13,
    VIEW_GAMEPAD_BUTTON_DPAD_LEFT = 14,
    VIEW_GAMEPAD_BUTTON_DPAD_RIGHT = 15,
    VIEW_GAMEPAD_BUTTON_HOME = 16,
} view_gamepad_button_id;

typedef struct view_input_event {
    i32 device;
    i32 phase;
    i32 pointer_id;
    i32 modifiers;
    f64 x, y;
    f64 dx, dy;
    f64 pressure;
    f64 altitude;
    f64 azimuth;
    f64 timestamp;
    i32 tool_action;
} view_input_event;

typedef struct view_gesture_state {
    f64 pan_x, pan_y;
    f64 zoom_factor;
    f64 rotation;
    f64 orbit_x, orbit_y;
} view_gesture_state;

typedef struct view {
    ns_str title;
    i32 width;
    i32 height;
    i32 framebuffer_width;
    i32 framebuffer_height;

    f64 mouse_x, mouse_y;
    f64 scroll_x, scroll_y;
    ns_bool mouse_down, mouse_pressed, mouse_released;
    ns_bool mouse_right_down, mouse_right_pressed, mouse_right_released;
    ns_bool mouse_middle_down, mouse_middle_pressed, mouse_middle_released;

    f64 display_ratio;
    f64 ui_scale;

    // Device safe-area insets in logical points: the margins of the drawable
    // that platform chrome (notch / status bar / home indicator / rounded
    // display corners) may cover. Backends without such chrome leave them 0.
    f64 safe_area_top, safe_area_right, safe_area_bottom, safe_area_left;

    void *native_window;
    void *gpu_device;

    ns_bool capture_required, capture_started;

    void *on_launch;
    void *on_frame;
    void *on_terminate;
} view;

// View management functions.
//
// view_create() opens the window and returns the view WITHOUT entering the event
// loop, so callers can attach on_launch / on_frame / on_terminate callbacks
// before driving frames with view_run(). view_run() blocks until the window
// closes. On platforms without a native backend (Linux) view_create() returns a
// no-op view and view_run() returns immediately.
view* view_create(const char *title, i32 width, i32 height);
// Compatibility alias. Current native backends use platform titlebars and
// window controls rather than app-drawn chrome.
view* view_create_no_title(const char *title, i32 width, i32 height);
void view_run(view *v);
// Ask the window to close, which is the same path the title bar's close button
// takes: on_terminate runs and view_run() returns, so a program can end itself
// rather than only being ended from outside. Backends without an event loop
// have nothing to leave and ignore it.
void view_close(view *v);
// Publish the platform safe-area insets (logical points). Negative values are
// clamped to 0. Backends call this whenever their metrics change; the UI
// module reads the fields to keep application content clear of native chrome.
void view_set_safe_area(view *v, f64 top, f64 right, f64 bottom, f64 left);
void view_capture_require(view *v);

// Schedule a bounded burst of frames. Native backends keep the last presented
// drawable on screen and sleep when no requests remain.
void view_request_frame(view *v, i32 frames);
void view_request_frame_after(view *v, i32 milliseconds);
// Cap presentation at this many frames per second. A positive value switches
// the backend to vsync-paced drawing at that rate. Zero restores on-demand
// drawing, unless NS_VIEW_CONTINUOUS is set.
void view_set_frame_per_second(view *v, i32 frames);
// Last value passed to view_set_frame_per_second; 0 means no cap.
i32 view_frame_per_second(void);
ns_bool view_take_frame_request(view *v);
// True when NS_VIEW_CONTINUOUS asks the backend to render every vsync
// instead of only on request, so a GPU frame capture always finds a
// frame boundary to arm on. Also true after view_set_frame_per_second
// with a positive rate.
ns_bool view_continuous_render(void);
void view_complete_frame(view *v);
void view_platform_request_frame(view *v);
void view_platform_request_frame_after(view *v, i32 milliseconds);
void view_platform_set_frame_per_second(view *v, i32 frames);
void view_platform_close(view *v);

// View event handling functions
void view_on_scroll(view *v, f64 x, f64 y);
void view_on_resize(view *v, i32 width, i32 height);
void view_on_mouse_move(view *v, f64 x, f64 y);
void view_on_mouse_btn(view *v, view_mouse_button button, view_button_action action);
void view_on_key_action(view *v, view_keycode key, view_button_action action);
ns_bool view_is_key_pressed(view *v, view_keycode key);
// Consume one latched key-down edge and return its modifier mask, or -1 when
// no press arrived since the previous frame. Unlike level polling, this keeps
// short press/release pairs from being lost between rendered frames.
i32 view_take_key_press(view *v, view_keycode key);
void view_clear_key_presses(view *v);

// Standard-layout gamepad state. Platform backends publish snapshots through
// view_on_gamepad_* before the application frame runs.
i32 view_gamepad_count(view *v);
ns_bool view_gamepad_connected(view *v, i32 gamepad);
f32 view_gamepad_axis(view *v, i32 gamepad, i32 axis);
f32 view_gamepad_button(view *v, i32 gamepad, i32 button);
ns_bool view_gamepad_button_pressed(view *v, i32 gamepad, i32 button);
ns_bool view_take_gamepad_button_press(view *v, i32 gamepad, i32 button);
void view_on_gamepad_connected(view *v, i32 gamepad, ns_bool connected);
void view_on_gamepad_axis(view *v, i32 gamepad, i32 axis, f32 value);
void view_on_gamepad_button(view *v, i32 gamepad, i32 button, f32 value, ns_bool pressed);

// Shared Apple adapter. Start installs lightweight wake handlers; poll publishes
// one coherent snapshot immediately before each application frame.
void view_apple_gamepad_start(view *v);
void view_apple_gamepad_poll(view *v);

// Platform backends feed the unified stream through these helpers. Events are
// valid until view_input_reset(), normally called after the ns frame callback.
void view_on_pointer_event(view *v, i32 device, i32 phase, i32 pointer_id,
                           f64 x, f64 y, f64 pressure, f64 altitude,
                           f64 azimuth, f64 timestamp, i32 modifiers);
void view_on_tool_action(view *v, i32 action, f64 timestamp);
void view_on_gesture(view *v, f64 pan_x, f64 pan_y, f64 zoom_factor, f64 rotation);
void view_on_orbit_gesture(view *v, f64 orbit_x, f64 orbit_y);
i32 view_input_count(view *v);
view_input_event *view_input_at(view *v, i32 index);
view_gesture_state *view_gesture(view *v);
ns_bool view_input_pending(view *v);
void view_input_reset(view *v);

// Clipboard functions
// `str` crosses the ns FFI as a UTF-8 C string pointer. Returned text only
// needs to remain valid until the FFI bridge copies it into the VM.
const char *view_get_clipboard(view *v);
void view_set_clipboard(view *v, const char *text);

// Immersive hosts publish all state on the view/main thread. Other hosts keep
// support disabled. Pose words: eye transform (column-major 4x4), projection
// (4x4), then head transform (4x4); metres in the tracking coordinate space.
ns_bool view_immersive_supported(void);
ns_bool view_immersive_request(ns_bool enabled);
i32 view_immersive_status(void);
i32 view_immersive_eye(void);
f64 view_immersive_value(i32 index);
void view_immersive_host_support(ns_bool supported);
ns_bool view_immersive_host_requested(void);
void view_immersive_host_status(i32 status);
void view_immersive_host_pose(i32 eye, const float *pose);
