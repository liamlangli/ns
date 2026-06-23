#include "view.h"

#ifdef NS_LINUX

// No native windowing backend on Linux yet (a Vulkan + X11/Wayland backend is
// future work). view_create returns a no-op view rather than null so shared ns
// programs run without a null dereference; view_run returns immediately, so no
// frames are produced.
static view _view;

view* view_create(const char *title, i32 width, i32 height) {
    _view.width = width;
    _view.height = height;
    _view.ui_scale = 1.0;
    _view.display_ratio = 1.0;
    _view.framebuffer_width = width;
    _view.framebuffer_height = height;
    _view.title = ns_str_cstr((char*)title);
    _view.native_window = ns_null;
    return &_view;
}

void view_run(view *v) {
    ns_unused(v);
    // No event loop without a native backend.
}

ns_str view_get_clipboard(view *v) {
    ns_unused(v);
    return (ns_str){0};
}

void view_set_clipboard(view *v, ns_str text) {
    ns_unused(v);
    ns_unused(text);
}

#endif // NS_LINUX
