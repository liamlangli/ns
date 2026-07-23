# nscode/native - native agentic coding shell

A native NSCode shell written in Nano Script and rendered through the `ui`
module of the standard library (`lib/ui.ns`). The current runtime view is an
agentic coding app layout in the codex style: chat history on the left and a
centered conversation column with a composer pinned at the bottom center of
the screen.

The `ui` renderer is bound to the view's GPU device, whose backend is selected
automatically per platform (see `lib/include/gpu.h`): **Metal on Apple, DirectX
12 on Windows**, and the browser Canvas backend in the Wasm build used by
GitHub Pages. Linux has no native backend yet, so the window/GPU calls are
no-ops there and the program exits cleanly. Only amd64 and aarch64 are
supported for desktop builds.

## Build & run

```bash
make
bin/ns run nscode/native
```

On macOS/Windows this opens a window, acquires the GPU device, installs the
Nano Script frame callback, and renders the agentic coding shell each frame.
On Linux `view_create` returns a no-op view, `gpu_request_device` returns
false, the renderer is skipped, and the program prints that no GPU backend is
available.

The Pages workflow runs the native headless suites, overlays `ns.web.mod`, and
builds the same directory as a Wasm app. `web_main.ns` supplies the browser
frame entry without changing the native callback-based entry point.

## Code view (Code mode)

The code pane is an editable, syntax-highlighted view of the document model in
`editor.ns`:

- **Syntax highlighting** - a pure-ns per-line tokenizer (`tokenize_line` in
  `editor.ns`) classifies comments, strings, numbers, keywords, builtin types,
  calls and punctuation; `render.ns` maps each token kind through the default
  **nanoscript** highlight. Keywords use bright ns green (`#61d394`), variables
  use dim white, functions use bright white, and string literals use
  yellow-green.
- **Mouse** - click places the caret (the gutter click clamps to the line
  start), dragging extends a multiline selection, double click selects the
  identifier under the pointer, triple click selects the whole line.
- **Keyboard** - arrows/Home/End/PageUp/PageDown move the caret (with Shift
  they extend the selection), Enter/Backspace/Delete/Tab edit, printable keys
  insert text (US layout), Ctrl/Cmd+Z and Ctrl/Cmd+Shift+Z (or Ctrl/Cmd+Y)
  undo and redo, and Ctrl/Cmd+A/C/X/V do select-all/copy/cut/paste through the
  native clipboard. Ctrl/Cmd+S saves and Ctrl/Cmd+W closes the active tab.
- Undo histories are bounded, coalesce consecutive typing/deletion, discard
  redo branches after a new edit, and remain attached to each workspace file
  while switching tabs.
- **Minimap** - the right edge renders a compact, syntax-colored map of the
  document from token fill rectangles. A translucent frame tracks the visible
  code viewport; clicking or dragging the map scrolls the editor.
- The view auto-scrolls to keep the caret visible after keyboard edits, and
  the wheel scrolls the full document height.

The window titlebar, platform window controls, and drag behavior are native.
A shared in-app main menu (`File`, `Edit`, `View`, `Window`, `Help`) sits above
both Code and Agent modes and opens app-rendered dropdowns. The Code/Agent
switch is rendered inside the app content below that native chrome. File >
Open Folder replaces the current project through the native folder dialog;
File > New Window asks for a project folder and launches an independent NSCode
window rooted there. File > Settings opens a dedicated preferences view. These
project/settings actions are separated from Save/Close.

### Settings

Settings are global and persist in `~/.ns/nscode/settings.db`:

- **Interface font** - Monospace or Sans UI; source code stays monospaced so
  cursor and selection columns remain exact.
- **Font size** - Compact, Default, or Large, applied immediately to editor,
  chat, menus, and their line spacing.
- **Theme** - Dark, Light, or High Contrast across the complete app surface.
- **Default language** - Nano Script, TypeScript, or Python. This updates the
  status bar, syntax palette, keyword/type recognition, and Python `#` comments.

### File explorer (Code mode)

Code mode includes a left-hand TreeView backed by a live scan of the project
root (the launcher's current working directory). The initially opened file is
revealed while unrelated folders stay collapsed; the root and nested folders
can be expanded or collapsed, rows highlight on hover, and the active file is
marked with an accent edge. Clicking a text file opens it in the editor; binary
assets remain visible but muted. Files ignored by the root `.gitignore` (and
the `.git` metadata directory) are omitted from the tree by default. Each
opened file keeps its own in-memory text buffer,
selection, cursor, horizontal offset, and vertical scroll position, so unsaved
edits survive file switches. Dirty files show a yellow dot in the tree and an
asterisk on the editor tab.

## Files

- `main.ns` - entry point: seeds the editor model, creates the window, requests
  the GPU device, creates the UI renderer, and installs Nano Script callbacks.
- `web_main.ns` - Wasm entry point used by GitHub Pages.
- `startup.ns` - initial editor document shared by native and browser entries.
- `ns.web.mod` - browser build manifest overlaid by the Pages workflow.
- `render.ns` - active app renderer. It owns the agent shell, code view, mode
  switch, TreeView, mouse/keyboard editor input, and pane scroll state.
- `workspace.ns` - filesystem snapshot and multi-file buffer state used by the
  Code view.
- `editor.ns` - text document model used by the code view: text buffer, caret,
  selection, syntax tokenizer, and typed-key character mapping.
- `test/editor_test.ns` - headless tests for the document model (runs anywhere).
- `test/workspace_test.ns` - headless tests for directory discovery, file switching,
  and preservation of unsaved buffers.
- `chat_test.ns` - headless persistence round-trip for project conversations.
- `settings_test.ns` - headless persistence round-trip for global preferences.
- `test/frame_test.ns` - headless interaction tests that drive render.ns frame
  by frame through the no-op Linux view backend. Run the project suites with
  `bin/ns test nscode/native`.
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
  session targets (nscode/native, nscode/cli, lib/ui).
- **Send** - the round accent button appends the real composer content to the
  selected project conversation. It does not manufacture a mock response.

Clicking the composer focuses it (typing goes to the input); clicking the
transcript blurs it. The transcript auto-scrolls to follow new messages and
the wheel scrolls the history.

### Chat history rail

Each history entry is a single-line row. Titles that exceed the row width are
cut at the container edge and the last few glyphs fade to transparent instead
of ending in a hard clip. Hovering a row reveals a `...` menu button at its
end which opens a per-chat menu:

- **Pin / Unpin** - pins the chat to the top of the list (pinned rows carry a
  small accent dot; unpinning drops the row below the remaining pinned block).
- **Archive** - removes the row and counts it under an `ARCHIVED n` label
  (the counter is persisted with the project conversation index).
- **Delete** - removes the conversation from the list.

Conversation history is loaded from and saved to
`<project-root>/.ns/chat/conversations.db`. A project without that store starts
with an empty history; the first sent message creates its first conversation.
Titles, message roles/text, pin state, selection, and archive count survive
restarts. The UI never seeds demo chats or generates canned agent replies.

## Status / known limitations

- Conversation persistence is real and project-local, but no LLM transport is
  wired yet; only messages supplied by the user or a future backend appear.
- Attachments currently name sample workspace files; their picker is not yet
  connected to the existing native file-dialog API.
- Typing assumes a US keyboard layout (keys are polled as VIEW_KEY_* codes;
  there is no OS text-input/IME event surfaced to ns yet), and key repeat is
  not synthesized while a key is held.
- Runtime appearance changes should be checked visually in the running app.
