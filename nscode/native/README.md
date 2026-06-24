# nscode/native - native agentic coding shell

A native NSCode shell written in Nano Script and rendered through the `ui`
module of the standard library (`lib/ui.ns`). The current runtime view is an
agentic coding app layout: chat history on the left, an agent dialog/timeline in
the center, and a local Git diff review pane on the right.

The `ui` renderer is bound to the view's GPU device, whose backend is selected
automatically per platform (see `lib/include/gpu.h`): **Metal on Apple, DirectX
12 on Windows**. Linux has no native backend yet, so the window/GPU calls are
no-ops there and the program exits cleanly. Only amd64 and aarch64 are
supported.

## Build & run

```bash
make
bin/ns run nscode/native/main.ns
```

On macOS/Windows this opens a window, acquires the GPU device, installs the
Nano Script frame callback, and renders the agentic coding shell each frame.
On Linux `view_create` returns a no-op view, `gpu_request_device` returns
false, the renderer is skipped, and the program prints that no GPU backend is
available.

## Files

- `main.ns` - entry point: seeds the editor model, creates the window, requests
  the GPU device, creates the UI renderer, and installs Nano Script callbacks.
- `render.ns` - active app renderer. It owns the agent shell, code view, mode
  switch, mouse selection, and pane scroll state.
- `editor.ns` - text document model used by the code view.
- `lib/src/ui.c` - low-level UI kernel only: renderer lifecycle, clipping,
  shapes, text measurement, and text drawing primitives.

## Git diff pane

The right pane is currently a Nano Script-rendered review preview. There is no
general shell/process API exposed to Nano Script yet, so it does not run
`git diff` directly.

## Status / known limitations

- The agent conversation and timeline are seeded/static. There is no real LLM
  chat backend or prompt entry yet.
- The review preview is static until Nano Script gets a narrow process or Git
  API.
- Runtime appearance changes should be checked visually in the running app.
