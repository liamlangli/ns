// Win32 native window backend (Windows).
//
// Mirrors the control flow of view.osx.m: view_create() opens a window, runs the
// message loop, and drives the on_launch / on_frame / on_terminate callbacks
// stored on the view. Window events are translated into the view_on_* callbacks
// declared in view.h. The DX12 backend (gpu.dx12.c) reads the surface through the
// view_win_* helpers below.
#include "view.h"

#ifdef NS_WIN
#include <windows.h>
#include <string.h>

typedef void (*view_on_launch)(view *);
typedef void (*view_on_frame)(view *);
typedef void (*view_on_terminate)(view *);

static view _view;
static HWND _hwnd;
static HINSTANCE _hinstance;
static ns_bool _quit;
static ns_bool _no_title;

// Win32 virtual-key -> VIEW_KEY_* mapping (parallels view_osx_key_map).
static i32 view_win_key_map(i32 vk) {
    if (vk >= 'A' && vk <= 'Z') return VIEW_KEY_A + (vk - 'A');
    if (vk >= '0' && vk <= '9') return VIEW_KEY_0 + (vk - '0');
    switch (vk) {
        case VK_SPACE: return VIEW_KEY_SPACE;
        case VK_RETURN: return VIEW_KEY_ENTER;
        case VK_TAB: return VIEW_KEY_TAB;
        case VK_BACK: return VIEW_KEY_BACKSPACE;
        case VK_ESCAPE: return VIEW_KEY_ESCAPE;
        case VK_DELETE: return VIEW_KEY_DELETE;
        case VK_INSERT: return VIEW_KEY_INSERT;
        case VK_LEFT: return VIEW_KEY_LEFT;
        case VK_RIGHT: return VIEW_KEY_RIGHT;
        case VK_UP: return VIEW_KEY_UP;
        case VK_DOWN: return VIEW_KEY_DOWN;
        case VK_HOME: return VIEW_KEY_HOME;
        case VK_END: return VIEW_KEY_END;
        case VK_PRIOR: return VIEW_KEY_PAGE_UP;
        case VK_NEXT: return VIEW_KEY_PAGE_DOWN;
        case VK_LSHIFT: case VK_SHIFT: return VIEW_KEY_LEFT_SHIFT;
        case VK_LCONTROL: case VK_CONTROL: return VIEW_KEY_LEFT_CONTROL;
        case VK_LMENU: case VK_MENU: return VIEW_KEY_LEFT_ALT;
        case VK_LWIN: return VIEW_KEY_LEFT_SUPER;
        default: return -1;
    }
}

static ns_bool view_win_no_title_button(i32 x, i32 y, i32 *button) {
    if (!_no_title || y < 0 || y > 38) return false;

    const i32 centers[] = {18, 40, 62};
    for (i32 i = 0; i < 3; i++) {
        const i32 dx = x - centers[i];
        const i32 dy = y - 19;
        if (dx * dx + dy * dy <= 100) {
            *button = i;
            return true;
        }
    }
    return false;
}

static ns_bool view_win_handle_no_title_mouse_down(HWND hwnd, LPARAM lparam) {
    if (!_no_title) return false;

    const i32 x = (i32)GET_X_LPARAM(lparam);
    const i32 y = (i32)GET_Y_LPARAM(lparam);
    if (y < 0 || y > 38) return false;

    i32 button = -1;
    if (view_win_no_title_button(x, y, &button)) {
        if (button == 0) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        } else if (button == 1) {
            ShowWindow(hwnd, SW_MINIMIZE);
        } else {
            WINDOWPLACEMENT placement = {0};
            placement.length = sizeof(placement);
            GetWindowPlacement(hwnd, &placement);
            ShowWindow(hwnd, placement.showCmd == SW_SHOWMAXIMIZED ? SW_RESTORE : SW_MAXIMIZE);
        }
        return true;
    }

    ReleaseCapture();
    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    return true;
}

static LRESULT CALLBACK view_win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_MOUSEMOVE:
            view_on_mouse_move(&_view, (f64)GET_X_LPARAM(lparam), (f64)GET_Y_LPARAM(lparam));
            return 0;
        case WM_LBUTTONDOWN:
            view_on_mouse_move(&_view, (f64)GET_X_LPARAM(lparam), (f64)GET_Y_LPARAM(lparam));
            if (view_win_handle_no_title_mouse_down(hwnd, lparam)) return 0;
            view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_PRESS);
            return 0;
        case WM_LBUTTONUP:
            view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_LEFT, VIEW_BUTTON_ACTION_RELEASE);
            return 0;
        case WM_RBUTTONDOWN:
            view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_RIGHT, VIEW_BUTTON_ACTION_PRESS);
            return 0;
        case WM_RBUTTONUP:
            view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_RIGHT, VIEW_BUTTON_ACTION_RELEASE);
            return 0;
        case WM_MBUTTONDOWN:
            view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_MIDDLE, VIEW_BUTTON_ACTION_PRESS);
            return 0;
        case WM_MBUTTONUP:
            view_on_mouse_btn(&_view, VIEW_MOUSE_BUTTON_MIDDLE, VIEW_BUTTON_ACTION_RELEASE);
            return 0;
        case WM_MOUSEWHEEL:
            view_on_scroll(&_view, 0.0, (f64)(i32)(i16)HIWORD(wparam) / (f64)WHEEL_DELTA);
            return 0;
        case WM_MOUSEHWHEEL:
            view_on_scroll(&_view, (f64)(i32)(i16)HIWORD(wparam) / (f64)WHEEL_DELTA, 0.0);
            return 0;
        case WM_KEYDOWN: {
            i32 key = view_win_key_map((i32)wparam);
            if (key >= 0) view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_PRESS);
            return 0;
        }
        case WM_KEYUP: {
            i32 key = view_win_key_map((i32)wparam);
            if (key >= 0) view_on_key_action(&_view, key, VIEW_BUTTON_ACTION_RELEASE);
            return 0;
        }
        case WM_SIZE: {
            const i32 w = (i32)LOWORD(lparam);
            const i32 h = (i32)HIWORD(lparam);
            view_on_resize(&_view, w, h);
            return 0;
        }
        case WM_DESTROY: {
            view_on_terminate terminate = (view_on_terminate)_view.on_terminate;
            if (terminate) terminate(&_view);
            _quit = true;
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

static view *view_win_create(const char *title, i32 width, i32 height, ns_bool no_title) {
    _no_title = no_title;
    _hinstance = GetModuleHandle(NULL);

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = view_win_proc;
    wc.hInstance = _hinstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ns_view_window";
    RegisterClassEx(&wc);

    RECT rect = {0, 0, width, height};
    const DWORD style = no_title
        ? (WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
        : WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);
    _hwnd = CreateWindowEx(0, wc.lpszClassName, title, style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rect.right - rect.left, rect.bottom - rect.top,
                           NULL, NULL, _hinstance, NULL);
    if (!_hwnd) return ns_null;

    _view.width = width;
    _view.height = height;
    _view.ui_scale = 1.0;
    _view.display_ratio = 1.0;
    _view.framebuffer_width = width;
    _view.framebuffer_height = height;
    _view.title = ns_str_cstr((char *)title);
    _view.native_window = (void *)_hwnd;

    ShowWindow(_hwnd, SW_SHOW);
    UpdateWindow(_hwnd);

    // Note: the event loop runs in view_run(), so the caller can attach
    // on_launch / on_frame / on_terminate callbacks first.
    return &_view;
}

view *view_create(const char *title, i32 width, i32 height) {
    return view_win_create(title, width, height, false);
}

view *view_create_no_title(const char *title, i32 width, i32 height) {
    return view_win_create(title, width, height, true);
}

void view_run(view *v) {
    if (!v || !_hwnd) return;

    view_on_launch launch = (view_on_launch)_view.on_launch;
    if (launch) launch(&_view);

    // Main loop: pump messages, then render a frame.
    MSG message;
    _quit = false;
    while (!_quit) {
        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { _quit = true; break; }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        if (_quit) break;
        view_on_frame frame = (view_on_frame)_view.on_frame;
        if (frame) frame(&_view);
    }
}

// view_close / view_capture_require are provided generically by view.c.

// ---- surface accessors for the DX12 backend --------------------------------

void *view_win_hwnd(void) { return (void *)_hwnd; }

int view_win_width(void) {
    RECT rect;
    if (_hwnd && GetClientRect(_hwnd, &rect)) return (int)(rect.right - rect.left);
    return _view.framebuffer_width;
}

int view_win_height(void) {
    RECT rect;
    if (_hwnd && GetClientRect(_hwnd, &rect)) return (int)(rect.bottom - rect.top);
    return _view.framebuffer_height;
}

// ---- clipboard -------------------------------------------------------------

ns_str view_get_clipboard(view *v) {
    ns_unused(v);
    ns_str result = (ns_str){0};
    if (!OpenClipboard(NULL)) return result;
    HANDLE handle = GetClipboardData(CF_TEXT);
    if (handle) {
        char *text = (char *)GlobalLock(handle);
        if (text) {
            result = ns_str_cstr(text);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return result;
}

void view_set_clipboard(view *v, ns_str text) {
    ns_unused(v);
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    const size_t len = (size_t)text.len;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (mem) {
        char *dst = (char *)GlobalLock(mem);
        if (dst) {
            memcpy(dst, text.data, len);
            dst[len] = '\0';
            GlobalUnlock(mem);
            SetClipboardData(CF_TEXT, mem);
        }
    }
    CloseClipboard();
}

#endif // NS_WIN
