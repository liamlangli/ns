schema = "ns.mod/v1"
name = "nscode-native"
version = "0.1.0"
author = "liamlangli <lilang8936@gmail.com>"
type = "app"
description = "Native code editor for Nano Script, rendered through the ui module."
source = "."
entry = "main.ns"
icon = "icon.png"
tests = ["editor_test.ns", "workspace_test.ns", "frame_test.ns", "ui_layout_test.ns"]
exclude = ["README.md"]

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
