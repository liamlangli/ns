# nscode/native - native agentic coding shell

A native NSCode shell written in Nano Script and rendered through the `ui`
module of the standard library (`lib/ui.ns`). The current runtime view is an
agentic coding app layout in the codex style: chat history on the left and a
centered conversation column with a composer pinned at the bottom center of
the screen.

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

The window titlebar, platform window controls, and drag behavior are native.
The Code/Agent switch is rendered inside the app content below that native
chrome.

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

## Agent view (Agent mode)

The agent view mirrors the codex UI: the transcript flows down a centered
column (user messages in bubbles, agent replies as plain rows, session events
as centered lines) and a multi-line composer stays pinned at the bottom center
of the pane:

- **Multi-line input** - printable keys append, Backspace deletes,
  Cmd/Ctrl+V pastes, Shift+Enter inserts a newline and the box grows up to six
  wrapped lines; Enter (or the round send button) posts the message.
- **Attachments** - the `+` button adds a file chip above the input (sampled
  from the workspace until a file picker API exists); each chip carries a
  close cross to remove it.
- **Permission level** - a selector pill (Read Only / Auto / Full Access)
  that opens an upward popup menu.
- **Module** - a second selector pill choosing the workspace module the
  session targets (nscode/native, nscode/cli, nscode/web, lib/ui).
- **Send / terminate** - the round accent button sends; while a session is
  running it turns into a red stop button, and Escape also interrupts. The
  transcript shows an animated `working...` row while the session runs.

Clicking the composer focuses it (typing goes to the input); clicking the
transcript blurs it. The transcript auto-scrolls to follow new messages and
the wheel scrolls the history.

## Status / known limitations

- The conversation backend is simulated: a running session posts a canned
  agent reply after a short delay (`g_reply_delay`). There is no real LLM
  chat backend yet.
- Attachments name workspace files; there is no OS file picker API surfaced
  to Nano Script yet.
- Typing assumes a US keyboard layout (keys are polled as VIEW_KEY_* codes;
  there is no OS text-input/IME event surfaced to ns yet), and key repeat is
  not synthesized while a key is held.
- Runtime appearance changes should be checked visually in the running app.
