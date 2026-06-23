# nscode/native — native, view-based code editor (in ns)

A native code editor written in Nano Script that renders through the `view`
(native window) and `gpu` modules of the standard library, rather than the
terminal. It shares its document model design with the terminal editor in
`nscode/cli`.

The GPU backend is selected automatically per platform (see
`lib/include/gpu.h`): **Metal on Apple, DirectX 12 on Windows**. Linux has no
native backend yet, so the window/GPU calls are no-ops there. Only amd64 and
aarch64 are supported.

## Build & run

```bash
make                       # builds bin/ns with the view + gpu modules
bin/ns run nscode/native/main.ns
```

On macOS/Windows this opens a window and acquires the GPU device. On Linux
`view_create` returns a no-op view and the program prints that no GPU backend is
available, then exits.

## Files

- `main.ns` — entry point: seed a document, create the window, request the GPU
  device, draw a frame, and run the event loop (`view_create` → configure →
  `view_run`).
- `editor.ns` — the document model: a flat byte buffer (`g_buf`/`g_len`), the
  cursor as a byte offset (`g_cur`), line geometry derived by scanning for
  `\n`, editing (`insert_byte`/`delete_back`/`delete_forward`), cursor movement,
  scrolling, and `process_view_key()` keyed by the `VIEW_KEY_*` codes from
  `lib/include/view.h`.
- `render.ns` — the per-frame GPU render layer: opens the screen render pass and
  sets the viewport. Submitting one textured quad per glyph from a font atlas is
  scaffolded with TODOs (it needs the descriptor-driven `gpu_create_*` calls,
  which require a scalar helper layer over the GPU API — see `lib/gpu.ns`).

## Status / known limitations

This is a working scaffold. The C-side infrastructure (per-platform GPU backend
selection, the DirectX 12 backend skeleton, the Win32 window, and the
`view_create`/`view_run` split) is in place, and the ns document model is fully
functional. The remaining pieces are follow-ups, blocked on interpreter gaps:

- **Per-frame & input callbacks.** `view` exposes `on_launch`/`on_frame`/
  `on_terminate` hooks, but attaching them from ns needs closure support, which
  currently has a pre-existing bug (`sample/ns/block.ns` also fails to parse).
  Until that lands, `main.ns` draws a single frame before `view_run` instead of
  rendering from an `on_frame` callback. Live key input additionally needs an
  `on_key` hook on the `view` struct plus native-layer plumbing.
- **Glyph rendering.** Requires the descriptor-based GPU creation calls to be
  reachable from ns (a scalar helper layer over `gpu_create_shader` /
  `gpu_create_pipeline` / `gpu_create_mesh`).
- **Linux backend.** No native window/GPU backend yet (a Vulkan + X11/Wayland
  backend is future work).
