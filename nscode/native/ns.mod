schema = "ns.mod/v1"
name = "nscode-native"
version = "0.1.0"
author = "lang <lilang8936@gmail.com>"
type = "app"
description = "Native view and GPU code editor for Nano Script."
source = "."
entry = "main.ns"
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
