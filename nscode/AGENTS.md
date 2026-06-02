# AGENTS — nscode UI conventions

Guidance for any agent (or human) working inside `nscode/`. These rules are
**mandatory** for all UI work in this module.

## Core rule: every pixel of UI goes through `@liamlangli/ui`

`nscode` is a WebGPU-rendered editor. **All** user-facing UI — panels, lists,
buttons, the editor surface, the output console, overlays, tooltips, the file
tree, the parse/token visualiser, status bars, dialogs — **must** be drawn with
the `@liamlangli/ui` module (`ui_renderer` and friends), via the adapter in
`src/ui.ts`.

Do **not** introduce non-WebGPU UI. That means **no**:

- `document.createElement` for visible widgets (`<div>`, `<button>`, `<input>`,
  `<pre>`, …)
- `innerHTML` / `textContent` used to render UI content
- inline `style` / CSS classes for layout or theming of UI
- secondary 2D `<canvas>` (`getContext('2d')`, `putImageData`, …) overlays
- DOM scroll containers, DOM selection, `contentEditable`, etc.

The only DOM permitted is the single WebGPU `<canvas>` the renderer owns, plus
the offscreen hidden `<input>` used purely to capture IME/keyboard text (it is
never visible). A bare pre-init fallback message (e.g. "WebGPU not available")
is acceptable *only* because the renderer cannot run yet.

### Known violations to migrate

These currently bypass the renderer and must be reimplemented in
`@liamlangli/ui`:

- `src/main.ts` — `output_view` (`<div>` + `innerHTML`): the output/console
  panel. Render text lines, scrolling, and selection through `ui_renderer`.
- `src/main.ts` — `parse_tex_canvas` (2D `<canvas>` + `putImageData`): the
  token/parse texture overlay. Draw it as a GPU texture/rects via the renderer.

## Use `ui_dock` for an editor-like layout

Compose the window from `@liamlangli/ui`'s `ui_dock` (docking) primitives rather
than hand-rolled `*_ref` split offsets. The result should read like a real IDE:
dockable, resizable regions for the file tree, editor, output/console, and the
parse visualiser, with draggable splitters and persistable layout. New panels
should be added as docked regions, not as ad-hoc absolutely-positioned rects.

## Prefer rounded rectangles

Favour rounded rectangles (round rect) over hard-cornered fills to soften and
modernise the look. Apply a consistent corner radius to:

- panels, cards, and dock region backgrounds
- buttons, list-item hover/selection highlights
- overlays/dialogs (command palette, find/replace, go-to-line, keybindings)
- input fields and the editor gutter/selection where it reads well

Use the renderer's rounded-rect fill (e.g. a `fill_round_rect`-style call from
`@liamlangli/ui`) instead of the plain `fill_rect` path in
`webgpu_draw_adapter.rect`. Keep the radius defined once in the shared palette
(`src/ui.ts`, alongside `C`) so it stays consistent across the app.

## Checklist before adding/changing UI

1. Is it drawn purely via `@liamlangli/ui` (`ui_renderer`)? No new DOM/2D-canvas.
2. Does it live inside a `ui_dock` region (if it's a panel)?
3. Are its backgrounds/highlights/overlays rounded rects with the shared radius?
4. Do colours come from the `C` palette in `src/ui.ts`?
