#pragma once
#include "view.h"
typedef struct desktop_window {
    u64 id;
    i32 pid, width, height, flags;
} desktop_window;
ns_bool desktop_supported(void);
ns_bool desktop_permission(void);
ns_bool desktop_request_permission(void);
ns_bool desktop_accessibility(ns_bool request);
void desktop_refresh(void);
i32 desktop_status(void);
i32 desktop_generation(void);
i32 desktop_window_count(void);
desktop_window *desktop_window_at(i32 index);
const char *desktop_window_title(i32 index);
const char *desktop_window_key(i32 index);
const char *desktop_window_path(i32 index);
u64 desktop_capture_open(u64 window, i32 max_width, i32 fps);
i32 desktop_capture_status(u64 handle);
u32 desktop_capture_texture(u64 handle);
void desktop_capture_pause(u64 handle, ns_bool paused);
void desktop_capture_close(u64 handle);
ns_bool desktop_window_activate(u64 window, i32 pid);
ns_bool desktop_window_close(u64 window, i32 pid);
i32 desktop_apps_refresh(void);
const char *desktop_app_name(i32 index);
const char *desktop_app_path(i32 index);
ns_bool desktop_app_launch(const char *path);
u32 desktop_app_icon(const char *path, i32 size);
const char *desktop_pick_application(void);
ns_bool desktop_shortcuts_start(view *v);
void desktop_shortcuts_stop(void);
i32 desktop_take_action(void);
void desktop_overlay_visible(view *v, ns_bool visible);
void desktop_overlay_fullscreen(view *v, ns_bool fullscreen);
const char *desktop_last_error(void);
