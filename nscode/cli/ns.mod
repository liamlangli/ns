schema = "ns.mod/v1"
name = "nscode-cli"
version = "0.1.0"
author = "lang <lilang8936@gmail.com>"
type = "app"
description = "Terminal text editor for Nano Script."
source = "."
entry = "main.ns"
tests = ["buffer_test.ns"]
exclude = ["README.md"]

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "term"
version = ">=0.1.0"
