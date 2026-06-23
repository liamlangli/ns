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
