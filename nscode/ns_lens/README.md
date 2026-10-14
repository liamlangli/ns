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
and text documentation. Directories only group a weighted binary treemap and
are not drawn; a file's area is proportional to its actual full-file line count. The
map is laid out flat, so leaves tile edge to edge. Flat, the camera is
orthographic; middle drag tilts the map plane up to 60 degrees and the camera
becomes a perspective one looking across it. A tilt released under 5 degrees
snaps back to orthographic.

- The interface is monochrome. Syntax highlighting and Git change backgrounds
  use color; leaf outlines are rounded rectangles with a 3-point radius.
- Minimap bars keep their syntax colours. A bar runs across gaps under four
  blanks and ends where the colour changes for more than two characters; a
  shorter run of another colour joins the bar before it. In every leaf, rows
  Git reports as added fill green and deleted lines appear as red ghost rows,
  so the size of each file's change reads from far away. Up close the same
  rows keep the editor's green and red backgrounds. A worker diffs every changed file against HEAD
  after file changes, after a commit, and every 30 seconds.
- Wheel or trackpad pinch: zoom toward the pointer with camera smoothing.
- Left drag: pan. Right click: select a leaf; right drag selects text in its editor.
  Middle drag: tilt the map into perspective.
- Zoom close to a leaf: its code becomes a syntax-highlighted, editable surface
  inside that leaf. The map keeps the entire canvas; no separate editor opens.
- Shift+wheel: scroll code inside the leaf.
- **Open project** selects a project folder.
- **File** (top left) holds the Git and search commands:
  - **Search** (Cmd+P) matches file names at once and scans file contents on
    a worker; a query without capitals ignores case. Matching leaves are
    outlined, and Enter or a click flies to the file or the matching line.
  - **Commit** (Cmd+Enter) asks for a one-line message and commits every
    change under the opened folder, untracked files included. The change view
    and the open file's diff refresh when it finishes.
  - **Push** (Cmd+O) pushes the branch, publishing it to `origin` when it has
    no upstream. Git runs without a terminal, so a missing credential fails
    with Git's message instead of waiting on a prompt.
- The controls are listed at the bottom left, above an activity row that shows
  the running search, commit or push with its progress, and the latest message.
- The menu beside **Open project** switches the map. **code** shows recognized
  source files (`.c`, `.h`, `.ns`, `.ts`, `.js`, and the other languages Lens
  already knows). **change** shows files Git reports as staged, unstaged, or
  untracked in the opened folder. **full** shows every scanned file. A file
  that is not text is a name tile; a deleted file has no leaf.
- Text and minimap share their line order (including inline Git deletion rows),
  column flow, font scale and perspective squeeze, so LOD switches preserve geometry.
  Glyphs appear at 0.18 scale and editing starts at 0.5, bringing code into view farther away.
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
  whole file shows. Each file takes the largest font its leaf holds, with line
  height in fixed proportion to the glyphs; spare width spreads the columns,
  and editing keeps the scale until the text outgrows the leaf. A column is as
  wide as 90% of the file's non-empty lines; longer lines wrap onto the
  following rows, like Vim's `wrap`, numbered only on their first row. Glyphs, minimap bars and editing overlays share that row grid; past a readable size the hovered file
  becomes an editor at the same scale, with selection, undo/redo and
  Cmd/Ctrl+S. Each file's path relative to the opened folder is the leaf's top
  row, in bold at the code font size, scaling with the code and showing
  alongside its glyphs. The path underlines under the pointer, and a left
  click copies it to the clipboard. While a leaf shows minimap bars, its file
  name sits in small bright text at the leaf's top left instead.
- Right-clicking a leaf focuses it at a readable code size, framing its top. Zoom
  stops once code reaches 2.5 times its editor size.
- The status bar progress bar is the precision still loading: minimap bars across
  the project, or code for the leaves in view once the camera is that close.
- The status bar starts with a down arrow counting upstream commits not pulled
  yet and an up arrow counting local commits not pushed. Lens fetches in the
  background every five minutes, and after opening a project. Both are
  buttons: the down arrow runs `git fetch` then `git pull --rebase` (Git
  refuses while the work tree has uncommitted changes, and Lens shows why),
  and the up arrow pushes.
- The status bar also shows actual FPS and process resident RAM in MiB, sampled
  once per second. The RAM bar and percentage use total physical memory as the
  scale. Idle polling naturally reports a lower FPS. Narrow windows hide the
  loading indicator first to leave room for these measurements.
- Without a path argument the map opens the shell's working directory.
- Every source file is read, opened and shown in full. Its minimap covers every
  line, and zooming in shows that same text.
- Save edits before switching the Lens editor to another file.

The application runs as a compiled native `exec` target. File metrics are streamed through a bounded 64 KiB buffer during the initial scan. Layout and source caches live under the opened
project's `bin/ns_lens/`, including `index.cache` and `files/*.cache`.
Layout reuse checks ordered paths, line counts, code scale and ancestry. Line/column metrics are cached with source hashes. Every source
cache read first hashes the entire local file with streamed SHA-256, then
checks the source digest and cached payload digest; same-size changes and
corruption rebuild the record. Writes are atomic and validate the source again
after reading. A single background worker reads one whole source file at a time
for its minimap. Dragging defers new detail loads;
preview line strings are split once on cache insertion. Camera motion redraws at
the display's refresh rate; a still view close enough to show glyphs keeps
16 ms frame requests. An idle overview
polls files every 100 ms.

In the nearest LOD, changes compare HEAD against the current edit buffer,
including staged, unstaged and unsaved changes. Added lines have green
backgrounds; removed lines appear as read-only red ghost rows immediately
before their replacement or following context. Source line numbers and mouse
navigation retain their actual document positions. Edits refresh the diff after
200 ms of quiet; HEAD is checked every two seconds. Git baseline and patches
are cached against HEAD and buffer hashes. Git must be on PATH (macOS/Linux);
untracked files are entirely added. Entirely deleted files do not yet receive
their own treemap leaf.


The preview LRU has 48 slots, each holding one whole file, with at most one
preview disk read per frame and up to 48 visible previews. Every leaf in the
viewport switches from minimap bars to glyphs at the same camera height.
Offscreen cards do not request detailed glyph chunks. A close view pauses
background minimap warming until the camera pulls back. Lens keeps one owned
edit buffer. Its undo history has 64 snapshots. These are not a total RSS claim:
UI resources, metadata, cached preview lines and one pending worker result also
consume memory.
Filesystem changes invalidate the layout and preview cache.

Current limits: the shared filesystem API snapshots at most 4096 entries;
Lens displays a warning at that limit. Source hashing and range loading run on the worker. Commit, push, fetch, search and
the change view's file list run on workers; the open file's diff and initial
metadata/layout work currently run on the main thread. The scene is a perspective atlas with billboard cards.
Syntax highlighting uses NSCode's existing supported language rules.

Validation:

```sh
bin/ns test nscode/ns_lens
bin/ns lint nscode/ns_lens
```

Each leaf shows either glyphs or its minimap. Code replaces the bars for that
file; the bars return only when this frame did not draw code. Below readable text size the
same resident minimap keeps rendering at every distance: there is no hidden
state or coarse/fine minimap switch. Minimap bars cover every source line and share the glyphs' line spacing,
column breaks, gutter, origin and camera scale. There is no row sampling or
bar-count truncation. The batch is retained independently of the glyph/page
cache and only rebuilt when source or layout changes. Pending files show a loading marker; empty files
have an empty-content mark. Directories are not drawn.

Full rows use retained glyph batches, including the active editor. The immediate
UI stream draws selection, Git backgrounds and the caret. Cached glyphs share
a two-million-vertex budget; current-frame draws survive until submission.
There is no 100-column truncation.
