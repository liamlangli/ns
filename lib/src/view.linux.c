#include "view.h"

#ifdef NS_LINUX

view* view_create(const char *title, i32 width, i32 height) {
    ns_unused(title);
    ns_unused(width);
    ns_unused(height);
    return ns_null;
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
