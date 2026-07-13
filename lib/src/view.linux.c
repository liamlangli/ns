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

view* view_create_no_title(const char *title, i32 width, i32 height) {
    return view_create(title, width, height);
}

void view_run(view *v) {
    ns_unused(v);
    // No event loop without a native backend.
}

const char *view_get_clipboard(view *v) {
    ns_unused(v);
    return ns_null;
}

void view_set_clipboard(view *v, const char *text) {
    ns_unused(v);
    ns_unused(text);
}

#endif // NS_LINUX
