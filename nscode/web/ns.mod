schema = "ns.mod/v1"
name = "nscode"
version = "0.2.0"
type = "app"
target = "wasm"
source = "."
entry = "backend.ns"
shell = "index.html"
exclude = ["bin/**"]

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"
