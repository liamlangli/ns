# nscode/cli — terminal text editor (in ns)

A small, "kilo"-style terminal text editor written entirely in Nano Script.
The editor logic is pure ns; all OS interaction (raw terminal mode, keystroke
input, screen output, file and environment access) goes through the native
`term` module shipped in the standard library (`lib/term.ns`). Supported on
Linux, macOS and Windows.

## Build & run

```bash
make                                            # builds bin/ns with the term module
NSCODE_FILE=path/to/file bin/ns run nscode/cli/main.ns
```

The file to edit is provided via the `NSCODE_FILE` environment variable (the ns
CLI does not forward extra command-line arguments to programs). If it is unset,
the editor starts with an empty, unnamed buffer; saving then does nothing until
a name exists. If the named file does not exist yet, it opens empty and `Ctrl-S`
creates it.

## Keys

| Key | Action |
| --- | --- |
| Arrow keys | Move the cursor |
| Home / End | Start / end of line |
| PageUp / PageDown | Move up / down one screen |
| Enter | Insert a newline |
| Backspace / Delete | Delete before / at the cursor |
| Printable chars | Insert text |
| `Ctrl-S` | Save |
| `Ctrl-Q` | Quit |

## Files

- `main.ns` — entry point (`editor_init` then `editor_run`).
- `editor.ns` — editor state and all logic: a flat byte buffer (`g_buf`), line
  geometry derived by scanning for `\n`, cursor as a byte offset, rendering with
  ANSI escapes, input dispatch, and file I/O.
- `keys.ns` — key-code constants matching `lib/include/term.h`.
- `buffer_test.ns` — headless tests for the buffer logic (no TTY needed):
  `bin/ns test nscode/cli/buffer_test.ns`.

## Implementation notes

The native layer is deliberately **scalar-only** (every `term_*` function takes
and returns `i32`): output is buffered one byte at a time via
`term_putc`/`term_flush`, files are streamed with `term_getc`/`term_putc_file`
over a path assembled with `term_path_push`, and the startup filename is read
byte-by-byte with `term_env_byte`. This avoids passing arrays or structs across
the ns FFI boundary, which is the only marshalling that proved reliable.

The document is a single byte buffer with a fixed capacity (256 KiB in this
version); editing shifts bytes in place. This is intentionally simple; a
growable/gap buffer is a natural future improvement.
