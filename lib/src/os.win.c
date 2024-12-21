#include "os.h"

#ifdef NS_WIN
#include <windows.h>

os_window* os_window_create(const char *title, i32 width, i32 height) {
    return ns_null;
}
#endif // NS_WIN