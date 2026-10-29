#include "desktop.h"
ns_bool desktop_supported(void) { return false; }
ns_bool desktop_permission(void) { return false; }
ns_bool desktop_request_permission(void) { return false; }
ns_bool desktop_accessibility(ns_bool request) { (void)request; return false; }
void desktop_refresh(void) {}
i32 desktop_status(void) { return -1; }
i32 desktop_generation(void) { return 0; }
i32 desktop_window_count(void) { return 0; }
desktop_window *desktop_window_at(i32 index) { (void)index; static desktop_window empty; return &empty; }
const char *desktop_window_title(i32 index) { (void)index; return ""; }
const char *desktop_window_key(i32 index) { (void)index; return ""; }
const char *desktop_window_path(i32 index) { (void)index; return ""; }
u64 desktop_capture_open(u64 window, i32 width, i32 fps) { (void)window; (void)width; (void)fps; return 0; }
i32 desktop_capture_status(u64 handle) { (void)handle; return -1; }
u32 desktop_capture_texture(u64 handle) { (void)handle; return 0; }
void desktop_capture_pause(u64 handle, ns_bool paused) { (void)handle; (void)paused; }
void desktop_capture_close(u64 handle) { (void)handle; }
ns_bool desktop_window_activate(u64 window, i32 pid) { (void)window; (void)pid; return false; }
ns_bool desktop_window_close(u64 window, i32 pid) { (void)window; (void)pid; return false; }
i32 desktop_apps_refresh(void) { return 0; }
const char *desktop_app_name(i32 index) { (void)index; return ""; }
const char *desktop_app_path(i32 index) { (void)index; return ""; }
ns_bool desktop_app_launch(const char *path) { (void)path; return false; }
u32 desktop_app_icon(const char *path, i32 size) { (void)path; (void)size; return 0; }
const char *desktop_pick_application(void) { return ""; }
ns_bool desktop_shortcuts_start(view *v) { (void)v; return false; }
void desktop_shortcuts_stop(void) {}
i32 desktop_take_action(void) { return 0; }
void desktop_overlay_visible(view *v, ns_bool visible) { (void)v; (void)visible; }
void desktop_overlay_fullscreen(view *v, ns_bool fullscreen) { view_set_fullscreen(v, fullscreen); }
const char *desktop_last_error(void) { return "Desktop capture requires macOS 13 or later"; }
