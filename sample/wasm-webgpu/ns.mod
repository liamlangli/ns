schema = "ns.mod/v1"
name = "wasm-webgpu"
version = "0.1.0"
type = "app"
target = "wasm"
source = "."
entry = "main.ns"

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "gpu"
version = ">=0.1.0"
