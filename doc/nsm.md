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
description = "Example module."
source = "src"
entry = "main.ns"
exclude = ["README.md", "build/"]

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"
```

Local modules need not be declared: every `use <name>` is resolved against the
project's own source first, so any sibling `<name>.ns` is included in the build
automatically. Only external `dependencies.runtime` need listing.

Running `ns run` with no file argument first checks for `ns.mod` in the current
directory and executes the `entry` (or first of `entries`) it declares, resolved
against the `source` dir. If the current directory has no `ns.mod`, it runs
`main.ns` there instead. It reports an error only when neither file exists.

Running `ns build` with no file argument compiles the current module into an
artifact under `<module>/bin`: `type = "app"` produces an executable, while
`type = "library"` produces a static library. `ns build <file.ns>` builds a
single script and links any local sibling modules it imports. Use `-o <path>` to
set the output path, or `--exe` / `--lib` to force the artifact kind.

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
output and diagnostics appear in the Xcode console. The app bundles `std.ns`,
`shader.ns`, and the pure-language `simd.ns`; it otherwise supports only the
language runtime and pure-language modules. Native UI, view, GPU, terminal,
network/HTTP, and external or dynamically loaded FFI modules are not available;
generation reports unsupported dependencies instead of silently producing a
broken app.

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
