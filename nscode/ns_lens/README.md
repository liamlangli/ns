# ns_lens

A standalone perspective code atlas.

Open a project directory (default: the current directory):

```sh
ns lens [path]
```

`make` builds the app to `bin/ns_lens.app` beside `ns`, and `make install`
installs it with the source in `share/nscode/ns_lens`. `ns lens` is no command
of its own: `ns` opens the installed app named `ns_lens`, and without a
compiled app it runs this project. From the repository root it also runs
directly with `bin/ns run nscode/ns_lens /absolute/path/to/project`.

Opened from Finder or the Dock, the app starts in its own resource directory
(`Contents/Resources`, or the directory a Linux package marks with
`.ns-resources`). That directory is never mapped or added to Open Recent;
instead the most recent project from Open Recent opens again. With nothing
remembered (or when that folder no longer exists, which also drops it from the
list) the window stays empty except for an **Open Folder...** button in the
centre, which (like Cmd+O) picks the project folder to map.

Every window is its own process with its own project, panels and terminal.
**File > New Window** (Cmd+Shift+N) and **New Window** in the Dock icon's
right-click menu start another Lens the way this one was started (the compiled
app relaunches itself, `ns run` reruns the interpreter); the new window opens
empty. Each window shows its own Dock icon.

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
- The map covers the whole window and is drawn first. The top bar (the **File**,
  **View**, and map menus) and the status bar float over it on translucent
  panels; pointer input over them never reaches the map. Menu dropdowns, the
  search box, and the commit and ignore dialogs use the same 80% background as
  the View panels, so the map stays visible behind them.
- Minimap bars keep their syntax colours. A bar runs across gaps under four
  blanks and ends where the colour changes for more than two characters; a
  shorter run of another colour joins the bar before it. In every leaf, rows
  Git reports as added fill green and deleted lines appear as red ghost rows,
  so the size of each file's change reads from far away. Up close the same
  rows keep the editor's green and red backgrounds. A worker diffs every changed file against HEAD
  after file changes, after a commit, and every 30 seconds.
- Wheel or trackpad pinch: zoom toward the pointer with camera smoothing.
- Two-finger turn (trackpad or touch screen): spin the map about the fingers.
  On a touch screen the map starts turning once the fingers have turned about
  ten degrees, so a pinch does not twist it. A map let go within five degrees
  of upright settles there. A tilt recedes up the screen whatever the spin.
- Left drag: pan. Right click: select a leaf; right drag selects text in its editor.
  Middle drag, or a three-finger drag on a trackpad or touch screen: tilt the
  map into perspective. Within five degrees of level it settles flat. On a Mac
  the trackpad's three-finger swipes must not be taken by Mission Control or
  Accessibility's three-finger drag (System Settings > Trackpad > More
  Gestures).
- Cmd+\ (Ctrl+\ on Linux and Windows), or a four-finger tap on a Mac trackpad
  or a touch screen: hide every control but the map, and bring them back. It
  works while code is being edited.
- Zoom close to a leaf: its code becomes a syntax-highlighted, editable surface
  inside that leaf. The map keeps the entire canvas; no separate editor opens.
- Shift+wheel: scroll code inside the leaf.
- **File** (top left) opens a project and holds search, Git commands and ignore settings:
  - **New Window** (Cmd+Shift+N) opens another, empty Lens window.
  - **Open** (Cmd+O) selects a project folder to map.
  - **Open Recent** opens a submenu of the 10 projects opened most recently,
    newest first; each row names the folder and the folder that holds it, and
    the open project is marked **open**. Pointing at the row (or clicking it) shows the
    submenu, and clicking a project maps it. The list is shared by every
    project in `ns_lens/recent` under the per-user application-support
    directory (`~/Library/Application Support` on macOS), one absolute path
    per line. A project that no longer exists keeps the open project mapped
    and leaves the list. **Clear Recent**, below a separator at the end of the
    submenu, empties the list, the open project included; it is dimmed while
    the list is empty.
  - **Search** (Cmd+P) matches file names at once and scans file contents on
    a worker; a query without capitals ignores case. Matching leaves are
    outlined, and Enter or a click flies to the file or the matching line.
  - **Commit** (Cmd+Enter) asks for a one-line message and lists every
    changed file under the opened folder, untracked files included, below the
    message field. Each row has a tick box, a path colored by its Git state
    (modified amber, added green, deleted red) and that state's letter, `M`,
    `A` or `D`, at its end; a rename lists its new path as added and its old
    path as deleted. Clicking a row ticks or unticks it, the box in the title
    ticks or unticks them all, and the wheel scrolls a list longer than twelve
    rows. With every file ticked the commit takes the whole work tree; with
    some unticked it stages the ticked new files and commits only the ticked
    paths, leaving every other change as it was. The change view and the open
    file's diff refresh when it finishes.
  - **Push** (no shortcut) pushes the branch, publishing it to `origin` when it has
    no upstream. Git runs without a terminal, so a missing credential fails
    with Git's message instead of waiting on a prompt.
  - **Ignore** opens a multiline editor for the opened project's
    `bin/ns_lens/ignore`. **Save** or Cmd/Ctrl+S writes the file and refreshes
    the map immediately; Escape closes the editor. Save other edited files
    before applying rules. On macOS/Linux, each nonempty line is a POSIX
    extended regular expression matched against the project-relative path
    (with `/` separators); matching a directory excludes all its children.
    For example, `(^|/)node_modules(/|$)` excludes dependency folders and
    `_test[.]ns$` excludes test sources. Lines starting with `#` are comments;
    invalid expressions are logged and skipped. These rules add to `.gitignore`
    and apply in every map mode and search.
- The controls are listed at the bottom left, above an activity row that shows
  the running search, commit, push, pull or branch command with its progress,
  and the latest message.
- The menu bar contains dropdown menus only; other controls belong in floating
  panels, dialogs, the canvas or the status area.
- The menu right of **View** switches the map. **code** shows recognized
  source files (`.c`, `.h`, `.ns`, `.ts`, `.js`, and the other languages Lens
  already knows). **change** shows files Git reports as staged, unstaged, or
  untracked in the opened folder. **full** shows every scanned file. A file
  that is not text is a name tile; a deleted file has no leaf. **history**
  shows the folder's commits (below). Cmd+1, Cmd+2, Cmd+3, Cmd+4 and Cmd+5
  select **code**, **ref**, **change**, **full** and **history** directly;
  the dropdown labels each shortcut.
- **View** (right of **File**) opens two floating panels, each of which can
  be dragged by its title bar, resized from every edge and corner (the grip in
  the lower-right corner marks it), and closed with its **x**. **Outline** and
  **Terminal** are independent toggles. Both float over the map on the shared
  80% background, as every popup does, so the code behind them stays faintly
  visible. Their open state, position and size are written to the project's
  `bin/ns_lens/layout` and restored when that project opens again; deleting
  that file returns both panels to their built-in defaults.
- The same file keeps the map's view: the camera's offset, zoom and tilt, the
  map mode, the scoped folder, the selected file and the outline's scroll and
  selection (and, in **ref**, its root file and the map camera it returns
  to). It is written a second after the camera comes to rest, when another
  project is opened and on quit, so launching or reopening a project starts
  where it was left. A saved folder or file the project no longer has is
  dropped, and a **ref** view whose root file is gone opens on **code**.
  - **Outline** is a tree of every scanned folder and file. Every folder starts
    folded; the button left of a folder unfolds and folds it. The open folders
    are remembered in the project's `bin/ns_lens/outline` (one
    project-relative folder path per line), so reopening the project restores
    the tree; deleting that file returns to all folders folded. Clicking a
    folder's name scopes the map to that folder's contents, and clicking it
    again (or **all**) returns to the whole project. The scoped folder is
    marked in the panel and named in its title bar. **+ file** and **+ folder**
    create an entry in the selected folder (or the selected file's folder), and
    **rename** renames the selected entry. A right click on a row starts a
    rename. Names cannot contain a path separator, and a rename never replaces
    an existing entry or discards unsaved editor text. A changed file takes
    the commit dialog's color for its Git state and ends its row with the
    state's letter; a folder holding changes does too, with the one kind its
    changes share or `M` when they differ.
  - **Terminal** runs the user's `$SHELL` as a login shell in the opened
    project root through a pseudo-terminal. It renders the shell's VT output
    (colours, cursor addressing, erase, scroll regions, alternate screens,
    wide characters) and sends keys back, so prompts, `vim`, `less` and build
    commands work. Click the panel to give it the keyboard; Cmd+V pastes,
    Cmd+K clears, and the wheel scrolls the retained scrollback. Drag across
    the text to select it (past the top or bottom edge scrolls the history),
    double-click to pick a word, triple-click for the whole line, and Cmd+C
    copies the selection. **restart**
    starts a new shell, and closing the panel keeps the running shell alive so
    a build continues while the panel is hidden. Reopen it from **View**. The
    panel's size is sent to the shell, so terminal-aware programs reflow.
    Windows hosts have no terminal backend and report that in the panel.
- **ref**, immediately below **code**, shows the selected source file between
  two sets of columns: files that reference it on the left, and files it
  references on the right. Dependencies continue rightward and callers continue
  leftward as **<** / **>** changes the depth (0–16). The depth group floats
  below the menu bar at the upper right and keeps its right margin when resized.
  Ref zoom-out starts with a 60000 camera-distance limit and expands to twice
  the distance needed to frame the whole graph, including dragged cells.
  A faint world-anchored dot grid provides motion cues in the map's empty areas;
  dot density changes gradually with zoom and follows the tilted plane.
  Files have a 72-unit vertical gap and 240-unit column gap. A layer with
  more than six dependencies or callers alternates between two columns,
  staggered vertically by half a row. Later depths move outward to leave room
  for both columns.
  Each file appears once; in a cycle a file reachable in both directions goes
  on the dependency side. Left-drag a cell to move it; left-drag empty canvas
  to pan. Cells retain their positions when depth changes unless their side,
  column, or layer width changes, and dragging keeps a 24-unit minimum gap. Click a cell to
  make it the new source; search also chooses a source.
  A translucent rounded outline marks one representative `use` or symbol use
  per file pair. Smooth cubic Bezier curves connect the vertical midpoints of
  facing left/right file borders, with horizontal tangents at each end.
  Curves follow dragging,
  camera zoom and the tilted plane, and remain visible over minimaps.
  Reference analysis covers `.ns` files in the scanned workspace: `use`
  resolves a module declaration or filename, and direct names resolve unique
  top-level functions, types, enums and bindings. Comments, strings, member
  names and local declarations are excluded; ambiguous names and SDK files
  outside the workspace are omitted. This is a lexical dependency view.
  Ref code is read-only and preserves an open editor's unsaved buffer.
  Filesystem changes refresh the graph; **code** restores the treemap.
- **history** is a read-only view of the commits that touched the opened
  folder (`git log -- .`), newest first, in a floating list at the left: each
  commit is one line with its short hash, message, author and commit time; a
  narrow list cuts the message first. Drag the list's right edge to change its
  width, which is saved in `bin/ns_lens/layout`. The
  list loads 150 commits at a time and reads the next page as it scrolls near
  the end. A click or Up/Down (Page Up/Down a page) selects a commit, and the
  map becomes a treemap of the files that commit changed, paths relative to
  the opened folder, each leaf's area following its changed lines. A merge is
  compared with its first parent; a rename shows its old path deleted and its
  new path added. A floating panel at the right holds the commit's message,
  author, date, parents and line counts, with **copy hash**, **copy message**
  and **copy commit** buttons. Details are lazy: the list only reads one line
  per commit, a commit's message and file list are read when it is selected,
  and a file's diff is read on a worker only once its leaf is on screen large
  enough for its rows to show, or when it is selected. Before that a leaf
  shows its added and deleted share in green and red; after, its diff rows
  (hunks, added and deleted lines with context) flow in columns, as bars from
  afar and as text up close. Right click selects a file and flies to it, a
  left click selects it, and a left click on its path row copies the path.
  The selected file adds **copy path** and **copy diff** to the panel;
  Cmd+C copies the selected file's diff, or the whole commit when no file is
  selected, and Escape clears the file selection. Nothing in this mode can be
  edited. A commit, pull or branch switch made in Lens reloads the list. A
  diff past 64 KiB keeps its last part. Leaving history returns to the map
  and camera it came from; history mode itself is not restored when the
  project opens again.
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
  stops once code reaches 50 times its editor size.
- The status bar progress bar is the precision still loading: minimap bars across
  the project, or code for the leaves in view once the camera is that close.
- The status bar starts with a down arrow counting upstream commits not pulled
  yet and an up arrow counting local commits not pushed. Lens fetches in the
  background every five minutes, and after opening a project. Both are
  buttons: the down arrow runs `git fetch` then `git pull --rebase` (Git
  refuses while the work tree has uncommitted changes, and Lens shows why),
  and the up arrow pushes. A branch without an upstream counts as ahead the
  commits no remote has yet and nothing behind; its up arrow publishes it to
  `origin`, and its down arrow says there is nothing to pull from.
- Right of the arrows, the branch button names the checked-out branch (or the
  commit of a detached HEAD), shortening a long name in the middle so its
  start and end stay readable; Lens reads it again every five seconds and after
  every Git command, so a switch made in a terminal shows up. Clicking it opens
  a menu above it:
  - **Create Branch...** asks for a name and creates the branch at HEAD,
    switching to it. Blanks in the name become dashes; a name that is taken or
    starts with `-` keeps the dialog open, and Git reports any other name it
    refuses.
  - **Pull Remote Branch...** runs `git fetch --all --prune` and opens a search
    over the remote-tracking branches, laid out like Cmd+P: typing filters by
    name (a query without capitals ignores case), Up/Down select, and Enter or a
    click checks the branch out. A remote branch without a local branch of its
    name gets one that tracks it; one with a local branch switches to it and
    fast-forwards it to the remote, which Git refuses if the two have diverged.
  - The local branches, the current one marked `current`. Clicking another
    switches to it; the wheel scrolls a list longer than twelve rows.
  - **Delete Current Branch** switches to the default branch (the one
    `origin/HEAD` names, else `main` or `master`), then deletes the branch it
    left with `git branch -d`, so Git keeps a branch whose commits are on
    neither its upstream nor the default branch, and says so. It is dimmed on
    the default branch and while HEAD is detached.

  Switching, pulling a remote branch in and deleting refuse while an editor
  tab has unsaved edits, which a later save would write over the other
  branch's files. Escape or a click outside closes the menu.
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
`ns clean` leaves that directory in place: it holds the project's Lens state and a
running window keeps writing to it, so remove it by hand for a full reset.
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
Lens displays a warning at that limit. Source hashing and range loading run on the worker. Commit, push, fetch, search,
the change list (for the change view, the commit dialog's files and the
outline's Git states) and the
open file's HEAD check and diff run on workers, so no Git process runs on the
main thread; initial metadata/layout work currently runs there. The scene is a perspective atlas with billboard cards.
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

Lens keeps the previous frame while idle. Foreground file/terminal polling runs
at 100 ms, with a maintenance redraw at most once a second; input, resizing and
ongoing work redraw sooner. Animations - progress bars, the indeterminate activity sweep and a focused terminal's caret fade - draw at the display rate, and a progress bar eases between the coarse values Git reports. Inactive, minimized or occluded windows skip UI
updates and rendering and poll file/terminal changes once a second. Returning
to the foreground refreshes the view, including changes received in background.

## iOS editor

The manifest remains `target = "exec"`. Generate the Apple project with
`bin/ns project nscode/ns_lens`, select **ns-lens iOS**, and set your signing
team in Xcode. The first iOS build requires CMake, curl and network access;
it builds checksum-pinned libgit2 1.9.1, libssh2 1.11.1 and OpenSSL 3.5.4
under the generated project's `Dependencies/Git`. Subsequent builds reuse them.
Device and simulator builds are arm64. Git licenses are bundled with the app.
Other applications only acquire these dependencies when their linked source
explicitly calls `os_git_embedded()`.

Repository and SSH identity actions are in the **Repo** menu next to Edit.
Repo > Generate SSH Key creates an unencrypted ed25519 key in OpenSSH format,
stores it in Keychain and copies its public key for your Git host or a
machine's `authorized_keys`; it refuses to replace an existing key. Repo >
Copy Public Key copies it again (OpenSSH-format keys only). Repo > Import SSH
Key selects an OpenSSH/PEM private key from Files, then asks for its
passphrase. The file is read into memory without a copied key file; the
identity is saved as a single device-only, when-unlocked Keychain record. An
encrypted OpenSSH passphrase is checked when connecting. Repo > Remove SSH Key
removes the stored identity. One identity is active at a time, and the private
key never leaves Keychain.
First connections show the SSH host's SHA-256 fingerprint in OpenSSH Base64 notation; verify it
with the server before trusting it. A changed fingerprint is refused rather
than replacing the trusted key. HTTPS certificate validation remains enabled;
HTTPS cloning currently supports public repositories, not token entry.

Repo > Clone Repository accepts an SSH/HTTPS URL and a local folder name.
Cloned repositories and imported folders live in the app's Documents folder,
so the Files app shows every file under On My iPhone > ns-lens; they reopen
through Open Recent, which the empty screen lists under Open Folder. Clones and
imports an older build kept in Application Support move there on launch.
File > Open imports a folder as a copy; changes do not automatically write
back to the original Files location. Repo > Commit Identity sets the author name and email once for every
repository, in the app's global Git config (`Library/Application Support/Git`);
it also clears an identity an older clone stored in the open repository.

The existing editor, syntax highlighting, outline, project search, Git diffs,
history and commit/push panels are shared with desktop. The Edit dropdown
exposes save, undo/redo, selection, clipboard, tab insertion and keyboard
visibility to touch users. System input methods commit UTF-8 text; cursor
movement and deletion preserve UTF-8 scalar boundaries. Dialogs adapt to the
phone's content width and keyboard. This does not yet implement grapheme-cluster
selection for multi-scalar emoji or a native text-selection handle overlay.

The mobile Git adapter covers the commands these panels issue. Pull currently
fetches and fast-forwards; divergent histories are refused without discarding
local commits. Resolve a divergent rebase on a desktop. Git hooks, submodule
recursion, Git LFS and arbitrary Git command execution are not provided.

iOS has no local shell, so the View menu lists only the outline there; the
terminal panel is desktop-only. The embedded SSH transport stays in use for Git.

Validation:

```sh
bin/ns test nscode/ns_lens
sh scripts/test_embedded_git.sh
```

The latter builds the same embedded backend on an arm64 Mac and runs it with
AddressSanitizer and UndefinedBehaviorSanitizer against temporary repositories
and a throwaway loopback sshd.
A real-device acceptance pass should import a throwaway encrypted key, verify
and trust a test host, clone/edit/save/commit/push a test repository, restart the
app, and check Chinese input and keyboard/rotation behavior on iPhone and iPad.
