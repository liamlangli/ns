# ns_lens

A standalone perspective code atlas.

Open a project directory (default: the current directory):

```sh
ns lens [path]
```

`make` builds the app to `bin/ns_lens.app` beside `ns`, and `make install`
installs it with the source in `share/nscode/ns_lens`; without a compiled app,
`ns lens` builds and runs this project. From the repository root it also runs
directly with `bin/ns run nscode/ns_lens /absolute/path/to/project`.

The scan follows the project's root `.gitignore`, excludes Git metadata, and
recognizes Nano Script, common native/web/shader languages, scripts, manifests,
and text documentation. Each directory is a titled frame holding its children
in a weighted binary treemap; a file's area is proportional to its actual full-file line count. The
map is laid out flat, so leaves tile edge to edge. Flat, the camera is
orthographic; middle drag tilts the map plane up to 60 degrees and the camera
becomes a perspective one looking across it. A tilt released under 5 degrees
snaps back to orthographic.

- The interface is monochrome. Syntax highlighting and Git change backgrounds
  use color; leaf outlines are rounded rectangles with a 3-point radius.
- Wheel: zoom toward the pointer with camera smoothing.
- Left drag: pan. Right click: select a leaf; right drag selects text in its editor.
  Middle drag: tilt the map into perspective.
- Zoom close to a leaf: its code becomes a syntax-highlighted, editable surface
  inside that leaf. The map keeps the entire canvas; no separate editor opens.
- Shift+wheel: scroll code inside the leaf; horizontal travel scrolls columns.
- **Open project** selects a project folder.
- Every file keeps a resident minimap (one bar per code segment),
  built once from its text and rebuilt only when the text or its column flow
  changes, so bars never blank while the camera moves; a leaf whose glyphs
  are not ready yet shows its bars instead.
- Code has no fixed size. Each cached page is laid out once into retained
  `ui` batches (syntax-highlighted glyphs, and token-coloured minimap bars)
  and drawn scaled by the camera, so it zooms with its leaf. One
  code-pixel-to-world ratio is shared by every leaf. By that zoom the code
  shows as glyphs, as minimap bars (one per code segment), or as the same resident minimap. Lines flow
  down newspaper columns across the leaf, preview and editor alike, so the
  whole file (up to 256 KiB) shows; column count and font scale maximize readability, while column and row spacing
  fill the available leaf; past a readable size the hovered file
  becomes an editor at the same scale, with selection, undo/redo and
  Cmd/Ctrl+S. Filenames appear once a leaf is wide enough.
- Right-clicking a leaf focuses it at a readable code size, framing its top. Zoom
  stops once code reaches 2.5 times its editor size.
- Without a path argument the map opens the shell's working directory.
- Files over 256 KiB use a read-only paged surface on the leaf. **Prev / Next**
  moves through 8 KiB ranges; Shift+wheel reaches every line in the page.
- Save edits before switching the Lens editor to another file.

The application runs as a compiled native `exec` target. File metrics are streamed through a bounded 64 KiB buffer during the initial scan. Layout and source caches live under the opened
project's `bin/ns_lens/`, including `index.cache` and `files/*.cache`.
Layout reuse checks ordered paths, line counts, code scale and ancestry. Line/column metrics are cached with source hashes. Every source
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

Text and minimap still switch with code size. Below readable text size the
same resident minimap keeps rendering at every distance: there is no hidden
state or coarse/fine minimap switch. Minimap bars cover every source line and share the glyphs' line spacing,
column breaks, gutter, origin and camera scale. There is no row sampling or
bar-count truncation. The batch is retained independently of the glyph/page
cache and only rebuilt when source or layout changes. Pending files show a loading marker; empty files
have an empty-content mark. Folder appearance is unchanged.
