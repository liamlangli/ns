// term.win.c
// ---------------------------------------------------------------------------
// Windows implementation of the scalar terminal primitives declared in
// lib/include/term.h. Uses the Win32 console API: SetConsoleMode for raw input
// and virtual-terminal (ANSI) output, ReadConsoleInput for key events, and
// GetConsoleScreenBufferInfo for size. File / env I/O use portable stdio. All
// cross-FFI state lives in file-scope statics so the interface stays scalar.
#include "term.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static DWORD g_orig_in_mode = 0;
static DWORD g_orig_out_mode = 0;
static int g_raw_enabled = 0;

#define TERM_OBUF_CAP 65536
static unsigned char g_obuf[TERM_OBUF_CAP];
static int g_olen = 0;

#define TERM_PATH_CAP 4096
static char g_path[TERM_PATH_CAP];
static int g_path_len = 0;

static FILE *g_read_file = NULL;
static FILE *g_write_file = NULL;

static HANDLE term_in(void)  { return GetStdHandle(STD_INPUT_HANDLE); }
static HANDLE term_out(void) { return GetStdHandle(STD_OUTPUT_HANDLE); }

int term_enable_raw(void) {
    if (g_raw_enabled) return 0;
    HANDLE hin = term_in();
    HANDLE hout = term_out();
    if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE) return -1;

    if (!GetConsoleMode(hin, &g_orig_in_mode)) return -1;
    if (!GetConsoleMode(hout, &g_orig_out_mode)) return -1;

    DWORD in_mode = g_orig_in_mode;
    in_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    SetConsoleMode(hin, in_mode);

    DWORD out_mode = g_orig_out_mode;
    out_mode |= (ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleMode(hout, out_mode);

    g_raw_enabled = 1;
    return 0;
}

int term_disable_raw(void) {
    if (!g_raw_enabled) return 0;
    SetConsoleMode(term_in(), g_orig_in_mode);
    SetConsoleMode(term_out(), g_orig_out_mode);
    g_raw_enabled = 0;
    return 0;
}

int term_read_key(void) {
    HANDLE hin = term_in();
    INPUT_RECORD rec;
    DWORD read_n = 0;

    for (;;) {
        if (!ReadConsoleInputW(hin, &rec, 1, &read_n) || read_n == 0) return -1;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        switch (vk) {
            case VK_LEFT:   return NS_KEY_ARROW_LEFT;
            case VK_RIGHT:  return NS_KEY_ARROW_RIGHT;
            case VK_UP:     return NS_KEY_ARROW_UP;
            case VK_DOWN:   return NS_KEY_ARROW_DOWN;
            case VK_DELETE: return NS_KEY_DEL;
            case VK_HOME:   return NS_KEY_HOME;
            case VK_END:    return NS_KEY_END;
            case VK_PRIOR:  return NS_KEY_PAGE_UP;
            case VK_NEXT:   return NS_KEY_PAGE_DOWN;
            default: break;
        }

        WCHAR wc = rec.Event.KeyEvent.uChar.UnicodeChar;
        if (wc != 0 && wc < 128) return (int)(unsigned char)wc;
    }
}

int term_width(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(term_out(), &csbi)) return 80;
    int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return (w > 0) ? w : 80;
}

int term_height(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(term_out(), &csbi)) return 24;
    int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return (h > 0) ? h : 24;
}

int term_flush(void) {
    HANDLE hout = term_out();
    int off = 0;
    while (off < g_olen) {
        DWORD written = 0;
        if (!WriteFile(hout, g_obuf + off, (DWORD)(g_olen - off), &written, NULL)) break;
        if (written == 0) break;
        off += (int)written;
    }
    g_olen = 0;
    return 0;
}

int term_putc(int c) {
    if (g_olen >= TERM_OBUF_CAP) term_flush();
    g_obuf[g_olen++] = (unsigned char)c;
    return 0;
}

int term_path_reset(void) {
    g_path_len = 0;
    return 0;
}

int term_path_push(int c) {
    if (g_path_len < TERM_PATH_CAP - 1) g_path[g_path_len++] = (char)c;
    return 0;
}

int term_open_read(void) {
    g_path[g_path_len] = '\0';
    if (g_read_file) fclose(g_read_file);
    g_read_file = fopen(g_path, "rb");
    return g_read_file ? 0 : -1;
}

int term_getc(void) {
    if (!g_read_file) return -1;
    int ch = fgetc(g_read_file);
    return (ch == EOF) ? -1 : ch;
}

int term_close_read(void) {
    if (g_read_file) fclose(g_read_file);
    g_read_file = NULL;
    return 0;
}

int term_open_write(void) {
    g_path[g_path_len] = '\0';
    if (g_write_file) fclose(g_write_file);
    g_write_file = fopen(g_path, "wb");
    return g_write_file ? 0 : -1;
}

int term_putc_file(int c) {
    if (!g_write_file) return -1;
    return (fputc(c, g_write_file) == EOF) ? -1 : 0;
}

int term_close_write(void) {
    if (g_write_file) fclose(g_write_file);
    g_write_file = NULL;
    return 0;
}

int term_env_byte(int idx) {
    const char *v = getenv("NSCODE_FILE");
    if (!v) return -1;
    int n = (int)strlen(v);
    if (idx < 0 || idx >= n) return -1;
    return (unsigned char)v[idx];
}
