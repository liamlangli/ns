schema = "ns.mod/v1"
name = "nscode-dynamic"
version = "0.1.0"
author = "liamlangli <lilang8936@gmail.com>"
type = "app"
description = "Test bench for the Box3D-backed dynamic module: convex rigid bodies simulated natively and rendered through gpu."
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

[[dependencies.runtime]]
name = "ui"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "os"
version = ">=0.1.0"

[[dependencies.runtime]]
name = "dynamic"
version = ">=0.1.0"
