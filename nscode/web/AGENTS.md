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

### Migration status

Done — both former DOM fallbacks now render through `@liamlangli/ui`:

- ✅ output/console panel → `ui_widgets.text_view` (`UI.output_text_view`),
  with selection, scroll, and Ctrl/Cmd+C copy handled by the widget.
- ✅ parse/token texture → GPU texture via `ui_renderer.create_texture` /
  `update_texture` / `draw_texture` (`UI.create_texture` … `draw_texture`),
  nearest-neighbour sampled.

Remaining non-renderer surfaces are limited to the pre-init "WebGPU not
available" fallback and the WebGPU preview canvas (`webgpu_module.ts`, itself a
GPU surface). Keep new UI on the renderer path; do not reintroduce DOM widgets.

## Use `ui_dock` for an editor-like layout

The window layout is driven by `@liamlangli/ui`'s dock module (`compute_dock_frame`
+ the `dock_*` state helpers), not hand-rolled split offsets. Panels live in a
dock tree — **Files | Editor | Output**, with contextual **Preview** (WebGPU
examples) and **Parse** (GPU parse data) tabs — with:

- rounded tabs (click to activate, drag to move/split across leaves),
- draggable splitters (`set_dock_split_ratio`),
- layout persisted to `localStorage` (`ns.dockLayout`).

Chrome is rendered via `UI.dock_tabbar` / `UI.dock_splitter`; geometry comes from
`compute_dock_frame(root, x, y, w, h, 1)` in **logical px** (the renderer adapter
applies dpr). Add new panels as dock tabs/leaves — never as ad-hoc
absolutely-positioned rects. The default tree lives in `main.ts:make_ns_layout`.

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

## Theming is live — drive everything from the `C` palette

The app ships a **theme picker** (toolbar, top-right) backed by `@liamlangli/ui`'s
built-in `default_themes` and `lerp_theme` linear cross-fade. The active theme is
applied through `apply_theme()` in `src/ui.ts`, which mutates the shared `C`
palette **in place** and swaps `NS_THEME.palette`. Because `C` slots are arrays
read at draw time, any UI that reads its colours from `C` (and any widget themed
through `NS_THEME`) re-themes automatically, including a smooth cross-fade.

So: never hard-code colours at a call site. Always read from `C` (or a
`NS_THEME` slot for `@liamlangli/ui` widgets) so theme switching reaches your UI.
Syntax-token colours (`SYN_*`) are intentionally left out of the preset mapping —
highlighting stays consistent across themes. New chrome colours that should
follow the theme must be added to the `apply_theme` slot mapping. The selected
preset persists to `localStorage` (`ns.theme`).

The renderer runs in `mode: 'realtime'` (set in `UI.init`) because nscode owns
its own redraw gating via the `dirty` flag in `main.ts`; don't switch it to the
renderer's `adaptive` default unless you also call `request_render()` on changes.

Interactive helpers report a CSS cursor via `this._want_cursor` (applied with the
renderer's `set_cursor` in `UI.render`): set it when you add new hit-tested UI so
hover/resize/text feedback stays consistent.

## Checklist before adding/changing UI

1. Is it drawn purely via `@liamlangli/ui` (`ui_renderer`)? No new DOM/2D-canvas.
2. Does it live inside a `ui_dock` region (if it's a panel)?
3. Are its backgrounds/highlights/overlays rounded rects with the shared radius?
4. Do colours come from the `C` palette in `src/ui.ts` (so theming reaches it)?
5. Does any new hit-tested control set `this._want_cursor` for cursor feedback?

## Verification scope

Agents should run compile/build/type checks only. Leave runtime interaction and
visual appearance checks to the user unless the user explicitly asks the agent
to perform them.
