// term.h
// ---------------------------------------------------------------------------
// Native terminal primitives for the nscode/cli editor, exposed to ns programs
// via `ref fn` declarations in lib/term.ns and the static symbol table in
// src/ns_vm_lib.c.
//
// The entire interface is SCALAR: every function takes and returns plain i32.
// This is deliberate -- on the ns interpreter only scalar FFI marshals reliably
// from inside a function body (passing `[u8]` arrays or `ref struct` arguments,
// or returning a `ref struct`, misbehaves there). So bytes move one at a time:
// output is buffered with term_putc/term_flush, files are read/written with
// term_getc/term_putc_file over a path assembled via term_path_push, and the
// target filename comes from term_env_byte (the NSCODE_FILE environment var).
//
// Two implementations provide the same symbols:
//   * lib/src/term.posix.c  -- Linux + macOS (termios / ioctl)
//   * lib/src/term.win.c    -- Windows (Win32 console API)
#ifndef NS_TERM_H
#define NS_TERM_H

// Special key codes returned by term_read_key(). Plain byte values (0..255) are
// returned as-is for printable and control keys; these escape-decoded keys use
// values >= 1000. Keep in sync with nscode/cli/keys.ns.
#define NS_KEY_ARROW_LEFT  1000
#define NS_KEY_ARROW_RIGHT 1001
#define NS_KEY_ARROW_UP    1002
#define NS_KEY_ARROW_DOWN  1003
#define NS_KEY_DEL         1004
#define NS_KEY_HOME        1005
#define NS_KEY_END         1006
#define NS_KEY_PAGE_UP     1007
#define NS_KEY_PAGE_DOWN   1008

// Raw mode on/off (idempotent). Return 0.
int term_enable_raw(void);
int term_disable_raw(void);

// Block for one key: a byte (0..255) or an NS_KEY_* code; -1 on end-of-input.
int term_read_key(void);

// Terminal size in columns / rows; fallbacks (80 / 24) when unavailable.
int term_width(void);
int term_height(void);

// Screen output, buffered: term_putc appends one byte to an internal buffer
// (auto-flushing when full); term_flush writes the buffer to stdout in one go.
int term_putc(int c);
int term_flush(void);

// Build the path used by the next term_open_read/term_open_write call, one byte
// at a time. term_path_reset clears it; term_path_push appends a byte.
int term_path_reset(void);
int term_path_push(int c);

// File reading: open the assembled path ("rb"); read bytes with term_getc
// (returns -1 at EOF). term_open_read returns 0 on success, -1 on failure.
int term_open_read(void);
int term_getc(void);
int term_close_read(void);

// File writing: open the assembled path ("wb", truncating); write bytes with
// term_putc_file. term_open_write returns 0 on success, -1 on failure.
int term_open_write(void);
int term_putc_file(int c);
int term_close_write(void);

// Read byte `idx` of the NSCODE_FILE environment variable, or -1 past its end /
// when unset. Lets ns read the startup filename without a string crossing FFI.
int term_env_byte(int idx);

#endif // NS_TERM_H
