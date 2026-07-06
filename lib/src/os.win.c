#include "os.h"

#ifdef NS_WIN
#include <windows.h>
#include <commdlg.h>

// Windows-specific OS functions
// File system, process management, etc.

#endif // NS_WIN

const char *os_open_file_dialog(const char *title) {
    static char path[MAX_PATH];
    path[0] = '\0';

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = title && title[0] ? title : "Open Profile";
    ofn.lpstrFilter = "Profile Files\0*.profile;*.txt;*.*\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameA(&ofn)) {
        path[0] = '\0';
    }

    return path;
}
