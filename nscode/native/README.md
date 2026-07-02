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

## Code view (Code mode)

The code pane is an editable, syntax-highlighted view of the document model in
`editor.ns`:

- **Syntax highlighting** - a pure-ns per-line tokenizer (`tokenize_line` in
  `editor.ns`) classifies comments, strings, numbers, keywords, builtin types,
  calls and punctuation; `render.ns` maps each token kind to a palette color.
- **Mouse** - click places the caret (the gutter click clamps to the line
  start), dragging extends a multiline selection, double click selects the
  identifier under the pointer, triple click selects the whole line.
- **Keyboard** - arrows/Home/End/PageUp/PageDown move the caret (with Shift
  they extend the selection), Enter/Backspace/Delete/Tab edit, printable keys
  insert text (US layout), and Ctrl/Cmd+A/C/X/V do select-all/copy/cut/paste
  through the native clipboard.
- The view auto-scrolls to keep the caret visible after keyboard edits, and
  the wheel scrolls the full document height.

Clicks in the custom title strip are delivered to the app (the Code/Agent
switch is clickable); moving the mouse past a small threshold while pressed
drags the window instead, and the traffic-light buttons keep their meaning.

## Files

- `main.ns` - entry point: seeds the editor model, creates the window, requests
  the GPU device, creates the UI renderer, and installs Nano Script callbacks.
- `render.ns` - active app renderer. It owns the agent shell, code view, mode
  switch, mouse/keyboard editor input, and pane scroll state.
- `editor.ns` - text document model used by the code view: text buffer, caret,
  selection, syntax tokenizer, and typed-key character mapping.
- `editor_test.ns` - headless tests for the document model (runs anywhere:
  `bin/ns run nscode/native/editor_test.ns`).
- `frame_test.ns` - headless interaction tests that drive render.ns frame by
  frame through the no-op Linux view backend
  (`bin/ns run nscode/native/frame_test.ns`).
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
- Typing assumes a US keyboard layout (keys are polled as VIEW_KEY_* codes;
  there is no OS text-input/IME event surfaced to ns yet), and key repeat is
  not synthesized while a key is held.
- Runtime appearance changes should be checked visually in the running app.
