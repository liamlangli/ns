schema = "ns.mod/v1"
name = "nscode"
version = "0.1.0"
author = "liamlangli <lilang8936@gmail.com>"
type = "app"
target = "wasm"
description = "NSCode browser build, sharing the native editor and renderer."
source = "."
entry = "web_main.ns"
icon = "icon.png"
exclude = ["README.md", "main.ns"]

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "view"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "gpu"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "ui"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "os"
version = ">=0.1.0"
