#include "os.h"

#ifdef NS_WIN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

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

const char *os_open_folder_dialog(const char *title) {
    static char path[MAX_PATH];
    path[0] = '\0';
    HRESULT ole = OleInitialize(NULL);
    BROWSEINFOA info;
    ZeroMemory(&info, sizeof(info));
    info.lpszTitle = title && title[0] ? title : "Open Folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST item = SHBrowseForFolderA(&info);
    if (item) {
        SHGetPathFromIDListA(item, path);
        CoTaskMemFree(item);
    }
    if (ole == S_OK || ole == S_FALSE) OleUninitialize();
    return path;
}

i32 os_launch_ns_project(const char *folder, const char *entry) {
    if (!folder || !folder[0] || !entry || !entry[0]) return 0;
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return 0;
    char command[8192];
    snprintf(command, sizeof(command), "\"%s\" run \"%s\"", exe, entry);
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    char old_entry[4096];
    DWORD old_len = GetEnvironmentVariableA("NSCODE_NATIVE_ENTRY", old_entry, sizeof(old_entry));
    SetEnvironmentVariableA("NSCODE_NATIVE_ENTRY", entry);
    BOOL launched = CreateProcessA(exe, command, NULL, NULL, FALSE, 0, NULL, folder, &startup, &process);
    if (old_len > 0 && old_len < (DWORD)sizeof(old_entry)) SetEnvironmentVariableA("NSCODE_NATIVE_ENTRY", old_entry);
    else SetEnvironmentVariableA("NSCODE_NATIVE_ENTRY", NULL);
    if (!launched) return 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}
