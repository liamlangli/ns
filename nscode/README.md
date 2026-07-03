# NSCode

Editors for Nano Script. This directory groups the different NSCode front-ends:

- **`web/`** — the WebGPU browser playground/editor (TypeScript + Vite). This is
  what is deployed to GitHub Pages under `/nscode/`. See `web/README.md`.
- **`cli/`** — a terminal text editor written **in ns itself** (a "kilo"-style
  editor). See `cli/README.md`.
- **`profile/`** — a flame-chart viewer written **in ns** for the Chrome trace
  files produced by `ns run --profile`. See the profiler quick start below.
- **`app/`** — *(planned)* a native GUI editor.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files (TextMate grammar + language configuration). See `nslang/README.md`.

## Profiler quick start

```bash
make                                   # build bin/ns
bin/ns run --profile your_script.ns    # writes ns.trace (one event per function call)
bin/ns run nscode/profile ns.trace     # open the flame-chart viewer
```

Everything after the run target is passed through to the script's
`fn main(args: [str])`, so the viewer receives the trace path as `args[0]`
(default: `ns.trace` in the current directory). The trace is standard Chrome
Trace Event Format JSON, so it also loads in Perfetto (https://ui.perfetto.dev)
or `chrome://tracing`.

## Terminal editor quick start

```bash
make                                            # build bin/ns (includes the term module)
NSCODE_FILE=notes.txt bin/ns run nscode/cli/main.ns
```

The terminal editor relies on the native `term` module (`lib/term.ns`,
`lib/src/term.posix.c`, `lib/src/term.win.c`) for raw-mode terminal control,
which is compiled into `bin/ns` and works on Linux, macOS and Windows.
