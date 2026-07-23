# NSCode

Editors for Nano Script. This directory groups the NSCode app and its supporting
tools:

- **`native/`** — the shared NSCode editor and agent shell, written in Nano
  Script. `main.ns` runs through the native view/UI backends, while
  `web_main.ns` packages the same editor and renderer for GitHub Pages through
  Wasm and the browser Canvas UI backend. See `native/README.md`.
- **`cli/`** — a terminal text editor written **in ns itself** (a "kilo"-style
  editor). See `cli/README.md`.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files, project run/build buttons, the native NSCode color theme, and TOML
  language-mode association for `ns.mod` manifests.

## Terminal editor quick start

```bash
make                                            # build bin/ns (includes the term module)
NSCODE_FILE=notes.txt bin/ns run nscode/cli/main.ns
```

The terminal editor relies on the native `term` module (`lib/term.ns`,
`lib/src/term.posix.c`, `lib/src/term.win.c`) for raw-mode terminal control,
which is compiled into `bin/ns` and works on Linux, macOS and Windows.
