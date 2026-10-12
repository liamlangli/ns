# ns_lens

A standalone perspective code atlas.

From the repository root:

```sh
bin/ns run nscode/ns_lens /absolute/path/to/project
bin/ns build nscode/ns_lens
```

The scan follows the project's root `.gitignore`, excludes Git metadata, and
recognizes Nano Script, common native/web/shader languages, scripts, manifests,
and text documentation. Directory siblings stay together in a weighted binary
treemap; file area follows byte size (empty files have a 128-byte floor).
Directory depth adds elevation to the tilted plane. The UI renderer draws
camera-facing cards using camera-space perspective projection.

- The interface is monochrome. Syntax highlighting and Git change backgrounds
  use color; leaf outlines are rounded rectangles with a 3-point radius.
- Wheel: zoom toward the pointer with camera smoothing.
- Right drag: pan. Middle drag: tilt.
- Zoom close to a leaf: its code becomes a syntax-highlighted, editable surface
  inside that leaf. The map keeps the entire canvas; no separate editor opens.
- Shift+wheel: scroll code inside the leaf; horizontal travel scrolls columns.
- Click a distant leaf to focus it. **Open project** selects a project folder.
- LOD 3: blocks; LOD 2: filenames; LOD 1: source previews;
  LOD 0: an editor directly on the leaf, selection, undo/redo and Cmd/Ctrl+S.
- Files over 256 KiB use a read-only paged surface on the leaf. **Prev / Next**
  moves through 8 KiB ranges; Shift+wheel reaches every line in the page.
- Save edits before switching the Lens editor to another file.

The application runs as a compiled native `exec` target. Only metadata is
loaded during its initial scan. Layout and source caches live under the opened
project's `bin/ns_lens/`, including `index.cache` and `files/*.cache`.
Layout reuse checks the ordered paths, byte sizes and ancestry. Every source
cache read first hashes the entire local file with streamed SHA-256, then
checks the source digest and cached payload digest; same-size changes and
corruption rebuild the record. Writes are atomic and validate the source again
after reading. A single background worker warms one 8 KiB preview per source
file and streams requested pages/details. Dragging defers new detail loads;
preview line strings are split once on cache insertion. Camera motion keeps
16 ms frame requests, while idle file watching uses 100 ms polling.

In the nearest LOD, changes compare HEAD against the current edit buffer,
including staged, unstaged and unsaved changes. Added lines have green
backgrounds; removed lines appear as read-only red ghost rows immediately
before their replacement or following context. Source line numbers and mouse
navigation retain their actual document positions. Edits refresh the diff after
200 ms of quiet; HEAD is checked every two seconds. Git baseline and patches
are cached against HEAD and buffer hashes. Git must be on PATH (macOS/Linux);
untracked files are entirely added. Entirely deleted files do not yet receive
their own treemap leaf. Large paged files currently omit inline changes.


The preview LRU has 24 slots of at most 8 KiB (192 KiB source payload), with at
most one preview disk read per frame and up to 24 visible previews. Offscreen
cards never read source. Lens keeps one owned edit buffer and rejects edits
that would grow it past 256 KiB; its existing undo history has 64 snapshots
(up to 16 MiB source payload). These are payload budgets, not a total RSS claim:
UI resources, metadata, cached preview lines and one pending worker result also
consume memory.
Filesystem changes invalidate the layout and preview cache.

Current limits: the shared filesystem API snapshots at most 4096 entries;
Lens displays a warning at that limit. Source hashing and range loading run on the worker. Git commands and initial
metadata/layout work currently run on the main thread. The scene is a perspective atlas with billboard cards. Large files support paged viewing,
not partial-file editing; a page can begin inside a UTF-8 character or a syntax
token. Syntax highlighting uses NSCode's existing supported language rules.

Validation:

```sh
bin/ns test nscode/ns_lens
bin/ns lint nscode/ns_lens
```
