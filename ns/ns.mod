schema = "ns.mod/v1"
name = "ns-in-ns"
version = "0.1.0"
author = "lang <lilang8936@gmail.com>"
type = "library"
description = "Self-hosted Nano Script front-end and test harness."
source = "."
entries = ["demo_main.ns"]
exclude = ["README.md", ".gitignore"]

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"
