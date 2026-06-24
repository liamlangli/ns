# nscode/native — native code editor (in ns)

A native code editor written in Nano Script that renders through the new `ui`
module of the standard library (`lib/ui.ns` — the Nano Script surface of the
[`@liamlangli/ui`](https://github.com/liamlangli/ui) WebGPU toolkit), rather
than the terminal. The on-screen editor is a native port of that toolkit's
`code_editor` plugin: a line-number gutter, selection highlight, document text
and caret, all drawn through the batched `ui_renderer`.

The `ui` renderer is bound to the view's GPU device, whose backend is selected
automatically per platform (see `lib/include/gpu.h`): **Metal on Apple, DirectX
12 on Windows**. Linux has no native backend yet, so the window/GPU calls are
no-ops there and the `ui` renderer is never created. Only amd64 and aarch64 are
supported.

## Build & run

```bash
make                       # builds bin/ns with the view + gpu modules
bin/ns run nscode/native/main.ns
```

On macOS/Windows this opens a window, acquires the GPU device, creates the `ui`
renderer and draws one frame of the code editor. On Linux `view_create` returns
a no-op view, `gpu_request_device` returns false, the renderer is skipped, and
the program prints that no GPU backend is available, then exits.

## Files

- `main.ns` — entry point: seed a document, create the window, request the GPU
  device, create the `ui` renderer on it (`ui_renderer_create` → `ui_resize`),
  draw one frame (`ui_begin_frame` → `render_frame` → `ui_flush`), and run the
  event loop.
- `editor.ns` — the document model: the text as one `str` (`g_doc`), the caret
  as a byte offset (`g_cur`) and an optional selection anchor (`g_anchor`), line
  geometry derived by scanning for `\n`, editing (`insert_str` / `delete_back` /
  `delete_forward` / `delete_selection`), cursor movement with shift-selection,
  scrolling, and `process_view_key()` keyed by the `VIEW_KEY_*` codes from
  `lib/include/view.h`. It mirrors the `text_buffer` model of the `ui` toolkit's
  `code_editor` plugin and does no rendering.
- `render.ns` — the per-frame render layer, a native port of the toolkit's
  `code_editor` plugin built on `ui_renderer`. Each frame it paints the
  background, the line-number gutter, the selection highlight, the visible
  document lines (`ui_draw_text` in the FONT_MONO atlas) and the caret, and
  overlays a vertical scrollbar — all batched into one GPU submission on flush.

## Status / known limitations

The ns side is complete: the document model is fully functional and the renderer
draws the whole editor through the `ui` module's builder API. The remaining
pieces are follow-ups:

- **`ui` native backend.** The core renderer path used by this sample is now
  linked natively: rectangles, rounded rectangles, clipping, packed colours and
  MSDF text are batched and submitted through the platform GPU backend using the
  bundled Latin/mono font atlas. The broader `lib/ui.ns` widget/theme surface is
  still a follow-up. On Linux the renderer is skipped entirely, so the program
  runs and exits cleanly.
- **Per-frame & input callbacks.** `view` exposes `on_launch`/`on_frame`/
  `on_terminate` hooks, but attaching them from ns needs closure support, which
  currently has a pre-existing bug (`sample/ns/block.ns` also fails to parse).
  Until that lands, `main.ns` draws a single frame before `view_run` instead of
  rendering from an `on_frame` callback. Live editing additionally needs an
  `on_key` hook on the `view` struct plus character/shift plumbing into
  `editor.ns` (`process_view_key` / `set_shift` / `insert_str` are ready for it).
- **Linux backend.** No native window/GPU backend yet (a Vulkan + X11/Wayland
  backend is future work).
