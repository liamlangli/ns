# NSCode

Editors for Nano Script. This directory groups the different NSCode front-ends:

- **`web/`** — the WebGPU browser playground/editor (TypeScript + Vite). This is
  what is deployed to GitHub Pages under `/nscode/`. See `web/README.md`.
- **`cli/`** — a terminal text editor written **in ns itself** (a "kilo"-style
  editor). See `cli/README.md`.
- **`app/`** — *(planned)* a native GUI editor.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files (TextMate grammar + language configuration). See `nslang/README.md`.

## Terminal editor quick start

```bash
make                                            # build bin/ns (includes the term module)
NSCODE_FILE=notes.txt bin/ns run nscode/cli/main.ns
```

The terminal editor relies on the native `term` module (`lib/term.ns`,
`lib/src/term.posix.c`, `lib/src/term.win.c`) for raw-mode terminal control,
which is compiled into `bin/ns` and works on Linux, macOS and Windows.
