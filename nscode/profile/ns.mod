schema = "ns.mod/v1"
name = "nscode-profile"
version = "0.1.0"
author = "liamlangli <lilang8936@gmail.com>"
type = "app"
description = "Flame-chart viewer for ns --profile chrome trace files (ns.trace)."
source = "."
entry = "main.ns"
tests = ["trace_test.ns"]
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
