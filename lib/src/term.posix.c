// term.posix.c
// ---------------------------------------------------------------------------
// POSIX (Linux + macOS) implementation of the scalar terminal primitives
// declared in lib/include/term.h. Uses termios for raw mode, ioctl(TIOCGWINSZ)
// for size, and plain read()/write()/stdio for I/O. All cross-FFI state lives in
// file-scope statics so the interface can stay purely scalar.
#include "term.h"

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static struct termios g_orig_termios;
static int g_raw_enabled = 0;

// Buffered screen output (one write() per frame).
#define TERM_OBUF_CAP 65536
static unsigned char g_obuf[TERM_OBUF_CAP];
static int g_olen = 0;

// Path assembled byte-by-byte for the next file open.
#define TERM_PATH_CAP 4096
static char g_path[TERM_PATH_CAP];
static int g_path_len = 0;

static FILE *g_read_file = NULL;
static FILE *g_write_file = NULL;

int term_enable_raw(void) {
    if (g_raw_enabled) return 0;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return -1;

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
    g_raw_enabled = 1;
    return 0;
}

int term_disable_raw(void) {
    if (!g_raw_enabled) return 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_raw_enabled = 0;
    return 0;
}

int term_read_key(void) {
    char c;
    int n;
    while ((n = (int)read(STDIN_FILENO, &c, 1)) != 1) {
        if (n == -1 && errno != EAGAIN) return -1;
    }

    if (c != '\x1b') return (unsigned char)c;

    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return NS_KEY_HOME;
                    case '3': return NS_KEY_DEL;
                    case '4': return NS_KEY_END;
                    case '5': return NS_KEY_PAGE_UP;
                    case '6': return NS_KEY_PAGE_DOWN;
                    case '7': return NS_KEY_HOME;
                    case '8': return NS_KEY_END;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return NS_KEY_ARROW_UP;
                case 'B': return NS_KEY_ARROW_DOWN;
                case 'C': return NS_KEY_ARROW_RIGHT;
                case 'D': return NS_KEY_ARROW_LEFT;
                case 'H': return NS_KEY_HOME;
                case 'F': return NS_KEY_END;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return NS_KEY_HOME;
            case 'F': return NS_KEY_END;
        }
    }
    return '\x1b';
}

int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return 80;
    return (int)ws.ws_col;
}

int term_height(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0) return 24;
    return (int)ws.ws_row;
}

int term_flush(void) {
    int off = 0;
    while (off < g_olen) {
        ssize_t w = write(STDOUT_FILENO, g_obuf + off, (size_t)(g_olen - off));
        if (w <= 0) {
            if (w == -1 && errno == EINTR) continue;
            break;
        }
        off += (int)w;
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
