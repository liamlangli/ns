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
# Set true to make `ns run` build, link, and launch the native artifact.
link = false
# Optional browser target; omit for a native project.
# target = "wasm"
# Optional custom Wasm page with {{wasm}}, {{title}}, and {{favicon}} markers.
# shell = "index.html"
description = "Example module."
source = "src"
entry = "main.ns"
exclude = ["generated/**"]
# Files and directories a bundle packages; omit to package `assets`.
assets = ["res"]
# Mobile orientations a generated project enables; omit to enable them all.
orientation = ["portrait", "landscape_left", "landscape_right"]
```

Project source is recursive: every `.ns` file below `source` is compiled and
linked without a local `use` declaration. `exclude` removes project-relative or
source-relative files, directories (a trailing `/`), and glob patterns from
that source set. Generated `bin/` output is always ignored. A local `use` is
accepted for compatibility but is redundant; `use` is needed for built-in and
external modules.

The built-in runtime modules — `std`, `view`, `gpu`, `ui`, `os`, `io`,
`storage`, `compress`, `audio`, `net`, `http`, `task`, `term`, `simd`,
`shader`, `dynamic` — ship with the toolchain and resolve from the installed
SDK. A source file reaches them with `use <name>` alone; the manifest does not
declare them, and a `[[dependencies.runtime]]` table listing one is ignored.

### Packaged assets

A program reads its own files through relative paths, and those paths have to
mean the same thing when the project is interpreted from its root and when it
is launched as a built bundle. `assets` names the files and directories that
travel with the artifact, as project-relative paths. A manifest that declares
none keeps the conventional `assets` directory beside the manifest and below
`source`.

`ns build` copies each path into a macOS app bundle's `Contents/Resources`
under the name it has in the project, and the runtime enters that directory
before the program's `main` runs. A project with `assets = ["res"]` therefore
reads `res/house.vox` from the project directory under `ns run` and from the
bundle's resources once built, with no path handling in the program. A plain
`cli` executable is not a bundle and keeps the working directory it was started
from. A browser bundle copies declared paths beside its page, and continues to
sync `<source>/assets` into the `assets` directory of that bundle. Every packaged file is a build input,
so editing one triggers the next incremental build.

Test sources do not need manifest exclusions. Normal project compilation skips
directories named `test` and files named `*_test.ns` automatically. `ns test`
adds only the selected test entry to the project's non-test source set.

Running `ns run` with no file argument first checks for `ns.mod` in the current
directory and executes the `entry` (or first of `entries`) it declares, resolved
against the `source` dir. If the current directory has no `ns.mod`, it runs
`main.ns` there instead. It reports an error only when neither file exists.
By default the native entry is evaluated by the interpreter. Set `link = true`
to make `ns run` use the incremental native build and launch its artifact
instead. `link = false` and an omitted `link` keep the interpreted behavior.

### Mobile orientation

A mobile application usually supports one way of holding the device. The
`orientation` key names the ones it does support:

```toml
orientation = ["landscape_left", "landscape_right"]
```

The four names are `portrait`, `portrait_upside_down`, `landscape_left`, and
`landscape_right`. `ns project` enables exactly the declared ones in the
generated mobile application and disables every orientation the manifest leaves
out, so the manifest above produces a landscape-only iPhone and iPad app. A
manifest that declares no `orientation` keeps all four enabled, and a name that
is not one of the four is reported as a manifest error rather than ignored.
Desktop targets have no orientation, so the key only changes what a mobile
project generates.

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
| `link`        | Build and launch this native target from `ns run`              |
| `exclude`     | Sources removed for this target only, added to the project `exclude` |
| `orientation` | Mobile orientations this target enables; defaults to the top-level `orientation` |

Every target compiles the whole project source set minus the entries owned by
the other targets, so each target declares its own `main` and shares every
other module. A top-level `entry` declared beside `[[targets]]` is treated the
same way: it is removed from the source set of any target that does not own it.
Every declared target owns the directory `bin/<target name>` and writes all of
its output there - the artifact, the bundle it packages, and its build cache -
so targets never overwrite each other, not even when they package files under
the same names. The artifact inside that directory is named after the target, or
after `output` when the table sets it: target `web` with `output = "viewer"`
writes `bin/web/viewer`. A manifest that declares no `[[targets]]` has one
implicit target and keeps `bin/` itself.
Like the other target settings, `link` inherits its top-level value; an
explicit `link = false` on a target disables linking inherited from the project.

`ns build` with no target name builds *every* declared target, each with its
own `type`, so one manifest can ship a windowed app, a command-line tool and a
static library side by side:

| `type`                  | Artifact                                                     |
|-------------------------|--------------------------------------------------------------|
| `app` / `application`   | Host app bundle (`bin/<target>/<name>.app` on Darwin)        |
| `cli` / `exe`           | Plain executable `bin/<target>/<name>`                       |
| `library` / `lib`       | Static library `bin/<target>/lib<name>.a`                    |

`ns build <name>` builds that one target independently. `-o` applies to a
single target only, so name the target when overriding the output path.
`ns project` generates the IDE project of the default target; a `cli` target
gets host build/test targets rather than platform application targets. An app
target still gets the platform application targets when it sets `link = true`;
that setting controls `ns run`, while the generated Apple apps embed the linked
source.

A bare word selects a target: `ns run web`. An argument that looks like a path
stays a path, so `ns run ./web`, `ns run src/web_main.ns` and any argument
ending in `.ns` still name files. Passing the declared entry path of a target
selects that target's settings too. A bare word that matches neither a target
nor a file is reported with the list of targets the manifest declares. Target
lookup uses the nearest `ns.mod` at or above the current directory.

`ns run` and `ns profile` take one file or target. Everything after that is
for the program, including flags that `ns` itself also understands. Those
arguments are published as `NS_ARGC` plus `NS_ARG0`, `NS_ARG1`, ... and a
script reads them with `os_env`. A leading `--` after the file is stripped, and
`ns run -- arg...` runs the default target with only those program arguments.
`ns` options such as `--port` belong before the file or target.
`ns profile` always evaluates through the interpreter so it can collect VM and
FFI scopes, even when the selected target sets `link = true`. Wasm targets keep
their existing build-and-serve behavior regardless of `link`. The report is
written to `bin/ns.profile`: the project's own `bin/` when the run resolves a
project, otherwise `bin/` beside the working directory. Nothing is ever written
to the root of the project folder. The text format is `ns-profile-v5`: every
timeline event carries a thread name (`main`, or `callee#id` for async/dispatch
tasks), the open stack is parked per task across VM-lock handoffs, and
`ns profiler` draws Time-view lanes by that name. Older `ns-profile-v1`…`v4`
files still open; missing thread fields default to `main`.

Running `ns build` with no file argument compiles the current module into
artifacts under `<module>/bin`: one per declared target, or a single artifact
from the top-level `type` when the manifest declares no targets. `type = "app"`
produces a host app bundle, `type = "cli"` a plain executable, and
`type = "library"` a static library. Any build input inside a manifest
project uses that project's recursive source set. A file outside a project is
built as a standalone script and may link local sibling modules it imports. Use
`-o <path>` to set the output path, or `--exe` / `--lib` to force the artifact
kind. Independent native targets build concurrently, bounded by the host's
logical CPU count. Browser targets and targets that resolve to the same
artifact stay serial because they share generated output. Profiled builds also
stay serial so their nested compiler timeline remains complete.

Add `--profile` to a build to write the same `bin/ns.profile` and print a
hot-path summary for input resolution, cache validation, source linking,
parsing, SSA lowering, artifact emission, system linking, and packaging. The
phases use the `compiler::` prefix in the existing profile tables, timeline,
and flamegraph, so `ns profiler` opens build profiles as well as runtime
profiles. With no file argument, the viewer prefers `bin/ns.profile` and falls
back to a legacy `ns.profile`. Use
`ns build --profile --force` to measure a full compilation; without `--force`,
an up-to-date build intentionally profiles only target resolution and cache
validation. SSA lowering expands into `compiler.ssa::` semantic, literal,
metadata, reference, shader, function, module-init, and imported-function
phases. Each lowered function and transpiled shader is also recorded under
`compiler.ssa.fn::` and `compiler.ssa.shader::` respectively.

A build records every input it reads under `<output dir>/.ns-build/<artifact>.cache`
with that input's last modify time, size, and content hash. The next build
re-stats the same inputs, hashes only the ones whose time or size changed, and
keeps the existing artifact when the artifact is still in place and every
recorded input hashes to its recorded value; otherwise it recompiles. The
record covers the manifest, the project source set, sibling modules and
installed module declarations the linker read, packaged assets, and the `ns`
executable itself, along with the artifact kind, host target, and output path.
`--force` skips the check and compiles unconditionally.

`ns clean [path]` removes what builds generate for the nearest project: the
`bin/` directory, including generated IDE projects, the build cache, and the
build profile, plus legacy `ns.profile` beside the manifest.

For a browser project, keep `type = "app"` and set `target = "wasm"` (or
`platform = "wasm"` on one `[[targets]]` table).
`ns build` then emits a browser bundle (`.wasm`, `.wasm.map`, `ns-wasm.js`, and
`index.html`) under the target's output directory - `bin/<target name>` for a
declared target, `bin/` for a manifest without targets - while
`ns run --port 9001` builds, serves that directory, and starts the
loopback-only live-reload server. Port 0 selects an available port. See
`doc/wasm.md` for the lifecycle, browser ABI, WebGPU middleware, and supported
language subset. The HTML title uses the manifest `name`; `icon` becomes the
favicon, falling back to Nano Script's installed `ns.svg` when omitted. Set
`shell` to use a custom project HTML page; the build expands its `{{wasm}}`,
`{{title}}`, and `{{favicon}}` placeholders and copies it as the bundle's
`index.html`.

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
`net`, `storage`, `compress`, and `audio` modules. Other external or dynamically loaded FFI modules
are not available; generation falls back to host build/test targets instead of
silently producing a broken app.

The generated iOS target declares the orientations the manifest `orientation`
key names, for both the phone and the tablet idiom. An app that declares
`orientation = ["landscape_left", "landscape_right"]` therefore never rotates
into portrait, and one that declares no `orientation` supports all four.

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

The toolchain's native feature modules are also available as static archives.
`make static` writes one host archive per module to `bin/lib<module>.a`, beside
the dynamic modules used by the interpreter. On Darwin, `make ios_static`
writes the core runtime and the feature archives for an arm64 iOS device below
`bin/apple/ios-arm64/`. These archives contain module implementations only;
the final application still links the Apple frameworks required by the modules
it selects, and each module's documented platform restrictions still apply.

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
