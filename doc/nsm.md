nanoscript module manager
-------------------------

> `ns.mod` - nanoscript module manager config file

Scope-based projects require an `ns.mod` file at the project root. The file is
TOML so tools can read project metadata before compiling any Nano Script source.

```toml
schema = "ns.mod/v1"
name = "example"
version = "0.1.0"
author = "Example Author <author@example.com>"
type = "app"
# Optional browser target; omit for a native project.
# target = "wasm"
# Optional custom Wasm page with {{wasm}}, {{title}}, and {{favicon}} markers.
# shell = "index.html"
description = "Example module."
source = "src"
entry = "main.ns"
exclude = ["generated/**"]

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"
```

Project source is recursive: every `.ns` file below `source` is compiled and
linked without a local `use` declaration. `exclude` removes project-relative or
source-relative files, directories (a trailing `/`), and glob patterns from
that source set. Generated `bin/` output is always ignored. A local `use` is
accepted for compatibility but is redundant; `use` is needed for built-in and
external modules. Only external `dependencies.runtime` need listing.

Test sources do not need manifest exclusions. Normal project compilation skips
directories named `test` and files named `*_test.ns` automatically. `ns test`
adds only the selected test entry to the project's non-test source set.

Running `ns run` with no file argument first checks for `ns.mod` in the current
directory and executes the `entry` (or first of `entries`) it declares, resolved
against the `source` dir. If the current directory has no `ns.mod`, it runs
`main.ns` there instead. It reports an error only when neither file exists.

### Targets

A manifest may declare several runnable targets, each with its own entry:

```toml
schema = "ns.mod/v1"
name = "example"
version = "0.1.0"
type = "app"
source = "src"

[[targets]]
name = "example"
entry = "main.ns"
default = true

[[targets]]
name = "example-web"
entry = "web_main.ns"
platform = "wasm"
shell = "web/index.html"
exclude = ["desktop/"]
```

`ns run <name>` and `ns build <name>` select one target by name. Without a
name, the target marked `default = true` is used, otherwise the first one
declared. A manifest that declares no `[[targets]]` keeps using its top-level
`entry`/`entries`, so existing projects are unaffected.

| Key           | Meaning                                                        |
|---------------|----------------------------------------------------------------|
| `name`        | Selector for `ns run` / `ns build`, and the artifact name      |
| `entry`       | Entry source, relative to the manifest `source` dir            |
| `type`        | `app`, `cli` or `library`; defaults to the top-level `type`     |
| `platform`    | `wasm` for a browser target; defaults to the top-level `target` |
| `icon`        | Defaults to the top-level `icon`                               |
| `shell`       | Custom Wasm HTML page; defaults to the top-level `shell`       |
| `output`      | Artifact and display name; defaults to `name`                  |
| `default`     | `true` marks the target `ns run` picks with no name            |
| `exclude`     | Sources removed for this target only, added to the project `exclude` |

Every target compiles the whole project source set minus the entries owned by
the other targets, so each target declares its own `main` and shares every
other module. A top-level `entry` declared beside `[[targets]]` is treated the
same way: it is removed from the source set of any target that does not own it.
The build artifact of a target is written to `bin/<name>` (or `bin/<output>`),
so targets never overwrite each other.

`ns build` with no target name builds *every* declared target, each with its
own `type`, so one manifest can ship a windowed app, a command-line tool and a
static library side by side:

| `type`                  | Artifact                                          |
|-------------------------|---------------------------------------------------|
| `app` / `application`   | Host app bundle (`bin/<name>.app` on Darwin)      |
| `cli` / `exe`           | Plain executable `bin/<name>`                     |
| `library` / `lib`       | Static library `bin/lib<name>.a`                  |

`ns build <name>` builds that one target independently. `-o` applies to a
single target only, so name the target when overriding the output path.
`ns project` generates the IDE project of the default target; a `cli` target
gets host build/test targets rather than platform application targets.

A bare word selects a target: `ns run web`. An argument that looks like a path
stays a path, so `ns run ./web`, `ns run src/web_main.ns` and any argument
ending in `.ns` still name files. Passing the declared entry path of a target
selects that target's settings too. A bare word that matches neither a target
nor a file is reported with the list of targets the manifest declares. Target
lookup uses the nearest `ns.mod` at or above the current directory.

Running `ns build` with no file argument compiles the current module into
artifacts under `<module>/bin`: one per declared target, or a single artifact
from the top-level `type` when the manifest declares no targets. `type = "app"`
produces a host app bundle, `type = "cli"` a plain executable, and
`type = "library"` a static library. Any build input inside a manifest
project uses that project's recursive source set. A file outside a project is
built as a standalone script and may link local sibling modules it imports. Use
`-o <path>` to set the output path, or `--exe` / `--lib` to force the artifact
kind.

A build records every input it reads under `bin/.ns-build/<artifact>.cache`
with that input's last modify time, size, and content hash. The next build
re-stats the same inputs, hashes only the ones whose time or size changed, and
keeps the existing artifact when the artifact is still in place and every
recorded input hashes to its recorded value; otherwise it recompiles. The
record covers the manifest, the project source set, sibling modules and
installed module declarations the linker read, packaged assets, and the `ns`
executable itself, along with the artifact kind, host target, and output path.
`--force` skips the check and compiles unconditionally.

`ns clean [path]` removes what builds generate for the nearest project: the
`bin/` directory, including generated IDE projects and the build cache, and
`ns.profile` beside the manifest.

For a browser project, keep `type = "app"` and set `target = "wasm"` (or
`platform = "wasm"` on one `[[targets]]` table).
`ns build` then emits a browser bundle (`.wasm`, `.wasm.map`, `ns-wasm.js`, and
`index.html`) under `bin`, while `ns run --port 9001` builds and starts the
loopback-only live-reload server. Port 0 selects an available port. See
`doc/wasm.md` for the lifecycle, browser ABI, WebGPU middleware, and supported
language subset. The HTML title uses the manifest `name`; `icon` becomes the
favicon, falling back to Nano Script's installed `ns.svg` when omitted. Set
`shell` to use a custom project HTML page; the build expands its `{{wasm}}`,
`{{title}}`, and `{{favicon}}` placeholders and copies it as `bin/index.html`.

Running `ns update [path]` finds the nearest `ns.mod` and migrates project
metadata to the format bundled with the current executable. It preserves
custom manifest fields and source files, upgrades a missing or `ns.mod/v0`
schema marker to `ns.mod/v1`, refreshes `AGENTS.md`, and additively merges the
standard generated-file rules into `.gitignore`. Before replacing an existing
file, it keeps the original under `bin/ns-update-backup/`. Distinct later
revisions use numbered backup names. Unknown newer schemas are rejected, and
an already-current project is left unchanged.

### IDE projects

`ns project [path]` finds the nearest `ns.mod`, starting at `path` or the current
directory, validates it, and generates host-native IDE files under the module's
`bin` directory. Names that are not valid IDE identifiers are normalized to a
safe name. Generation requires schema `ns.mod/v1`, a nonempty name, a recognized
app/application or lib/library type, and a valid entry for an app.

On Darwin, it creates `bin/<safe-name>.xcodeproj`. An app manifest gets SwiftUI
application targets named `<safe-name> macOS`, `<safe-name> iOS`, and
`<safe-name> visionOS`, with automatic signing and the default bundle identifier
`ns.<safe-name>`; no development team is preset. The embedded interpreter runs
the linked entry on a background task; status appears in the app, while `print`
output and diagnostics appear in the Xcode console. Generated Apple apps embed
the official `std`, `task`, `shader`, `simd`, `view`, `ui`, `os`, `gpu`, `io`,
`net`, and `dynamic` modules. Other external or dynamically loaded FFI modules
are not available; generation falls back to host build/test targets instead of
silently producing a broken app.

When an app manifest declares `icon = "path/to/image.png"`, `ns project`
generates `Assets.xcassets` below the managed `.nsproject` directory. It resizes
the source image for every macOS icon slot, supplies the iOS 1024-pixel icon,
creates a visionOS image stack, and configures each Xcode target to compile the
`AppIcon` asset.

On Windows, it creates `bin/<safe-name>.sln` and
`bin/<safe-name>.vcxproj` for Visual Studio 2022. App projects are x64 NMake
projects in Debug and Release configurations: Build invokes
`ns build <module> --exe -o <output>`, Clean removes that executable, and
debugging launches it. Nano Script source files are visible in Solution Explorer,
excluding `bin`. Library manifests produce browsing/utility projects; their
Build and Rebuild actions run `ns test` rather than claiming a native Windows
library artifact.

Library manifests similarly produce `NS Build` and `NS Test` utility targets on
Darwin. They do not create iOS or visionOS library artifacts; portable native
library output remains dependent on a stable NS ABI and the required object and
archive backends.

Generated IDE skeletons remain editable. Later `ns project` runs preserve the
Xcode `project.pbxproj`, Visual Studio solution, and project files unless a
generated-project schema upgrade or structural resource change is required.
Generated support files are refreshed in
the clearly marked `bin/<safe-name>.nsproject` directory. Files named
`Config/NS.Generated.xcconfig` or `Config/NS.Generated.props` are managed and
refreshed; the matching `NS.Local.xcconfig` or `NS.Local.props` overrides are
created once and preserved. Xcode builds rerun `ns project` to refresh the fixed
`LinkedProject.ns` resource and other managed inputs without replacing the IDE
project.

Other host operating systems report that IDE project generation is unsupported.

The `nsm` module is a module manager for nanoscript. It allows you to create, install, and manage modules for your nanoscript projects. The `nsm` module is a core module and is included with the nanoscript runtime.

### Usage

| Command                          | Description                          |
|----------------------------------|--------------------------------------|
| `nsm create example`             | Create a new app module              |
| `nsm create lib_example --lib`   | Create a new lib module              |
| `nsm build`                      | Build the current module             |
| `nsm run`                        | Run the app module                   |
| `nsm lint`                       | Lint the module                      |
| `nsm add [mod_name]`             | Add a module to the current module   |
| `nsm remove [mod_name]`          | Remove a module from the current module |
