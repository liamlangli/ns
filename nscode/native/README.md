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
temporary native bridge with `ui_agentic_coding_attach`, and renders the
agentic coding shell each frame. On Linux `view_create` returns a no-op view,
`gpu_request_device` returns false, the renderer is skipped, and the program
prints that no GPU backend is available.

## Files

- `main.ns` - entry point: seeds the editor model, creates the window, requests
  the GPU device, and attaches the native agentic renderer bridge.
- `render.ns` - Nano Script mirror of the intended agentic layout. It is kept
  aligned with the C bridge so future `view.on_frame` callback support has a
  ready NS-side render path.
- `editor.ns` - the existing text document model. It remains available for
  future live editing work, but the first agentic shell pass uses seeded chat
  and agent activity rather than a real chat backend.
- `lib/src/ui.c` - the active runtime renderer while NS callback assignment is
  incomplete. It owns the frame callback, seeded chat/activity UI, mouse
  selection for the chat list, scrolling for the dialog and diff panes, and the
  local Git diff loader.

## Git diff pane

The right pane runs a narrow internal command:

```bash
git diff --no-ext-diff --
```

Output is capped at 256 KB. Empty output shows a "No local changes" state;
Git errors or non-repository directories show an error state instead of
crashing. This does not expose a general shell/process API to Nano Script.

## Status / known limitations

- The agent conversation and timeline are seeded/static. There is no real LLM
  chat backend or prompt entry yet.
- The Git diff is live runtime data from the current working directory and is
  refreshed periodically by the native bridge.
- `view.on_frame` callbacks from Nano Script are still unreliable, so
  `ui_agentic_coding_attach` remains the active bridge.
- Runtime appearance changes should be checked visually in the running app.
