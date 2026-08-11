# Dynamic Module

`dynamic` is Nano Script's native 3D rigid-body module. Its simulation backend
is [Box3D](https://github.com/erincatto/box3d) v0.1.0, pinned at commit
`8441b4a06d6d09dcfb0b0f704df4d847d1437b92`. The build downloads it into the
ignored `third_party/box3d/` directory under the MIT license.

The Nano Script API stays explicitly typed and data-oriented. A world contains
plain `dynamic_body` and `dynamic_config` values plus an opaque native handle;
`dynamic.dylib`/`dynamic.so` owns Box3D's broad phase, collision detection,
continuous collision, islands, sleeping machinery, and Soft Step constraint
solver.

```ns
use dynamic

let world = dynamic_world_create(32, dynamic_config_default())

let cube = dynamic_body_box(float3 { x: 0.5, y: 0.5, z: 0.5 }, 1.0)
cube.position = float3 { x: 0.0, y: 3.0, z: 0.0 }
let index = dynamic_world_add(world, cube)
dynamic_world_set_hull(world, index, corners, 8)

dynamic_world_step(world)
let body = dynamic_world_body(world, index)

dynamic_world_release(world)
```

`nscode/dynamic` is a native sample that simulates cubes, tetrahedra, a
hexagonal prism, and spheres and renders the mirrored body transforms through
the `gpu` module.

## Shapes

`dynamic_world_set_hull` accepts body-local points as a flat `[f32]` array.
The adapter maps them to Box3D geometry as follows:

| Point count | `body.margin` | Box3D geometry |
| --- | --- | --- |
| 1 | greater than zero | sphere centered at the point |
| 2 | greater than zero | capsule between the points |
| 4–16 | any value | convex hull |

`dynamic_body_sphere`, `dynamic_body_box`, and `dynamic_body_hull` fill the
mass and diagonal inertia values. A sphere is attached immediately by
`dynamic_world_add`; boxes and other hulls are attached by
`dynamic_world_set_hull`.

Box3D v0.1.0 has sphere and capsule primitives but no generic rounded-convex
primitive. A margin on a hull with three or more points is therefore retained
in the public body data but does not round the native hull. Degenerate point
sets that cannot form a Box3D convex hull remain bodies without a collision
shape.

Zero or negative mass maps to a Box3D static body. Positive mass maps to a
dynamic body, and the adapter applies the `dynamic_body` mass and diagonal
inertia explicitly instead of allowing shape density to replace them.

## Stepping and configuration

`dynamic_world_step` advances Box3D by `config.dt`, then mirrors transforms,
velocities, and dynamic-body contacts back to Nano Script.

Box3D exposes one solver substep count rather than the old module's separate
substep and iteration loops. The adapter passes:

```text
Box3D substeps = max(config.substeps, 1) * max(config.iterations, 1)
```

This keeps the default solver budget at four, Box3D's recommended starting
point.

| Field | Default | Box3D mapping |
| --- | --- | --- |
| `gravity` | (0, -9.81, 0) | world gravity, refreshed before each step |
| `dt` | 1/60 | Box3D time step |
| `substeps` | 1 | multiplied into the native substep count |
| `iterations` | 4 | multiplied into the native substep count |
| `bounds_min` / `bounds_max` | (-3, 0, -3) / (3, 7, 3) | six static Box3D hulls |
| `bounds_enabled` | true | creates or removes those hulls |
| `bounds_restitution` | 1 | boundary material restitution |
| `slop`, `bias`, `relaxation`, `margin` | legacy values | retained for source compatibility; Box3D owns these policies |
| `prefer_gpu` | false | retained but ignored |

Restitution is combined by multiplication so a body's restitution remains the
controlling value against the default boundary material. Friction uses
Box3D's default mixer.

## State mirror and compatibility

The native backend keeps the previous `dynamic_world.state` layout as a host
mirror. Existing rendering and query code can continue to use:

- `dynamic_world_body`
- `dynamic_world_hull_count` and `dynamic_world_hull_vertex`
- `dynamic_world_contact_count`
- `dynamic_state_read`

Rows 1–6 contain body transform, velocity, material, and inertia metadata.
Rows 7–18 contain up to `DYNAMIC_MAX_CONTACTS` dynamic-body pairs, Box3D
manifold normals/depths, and contact points. Contacts against the six implicit
world-bound bodies are intentionally not exposed as body-to-body contacts.

The state array is a compatibility mirror, not Box3D's source of truth.
Use `dynamic_world_set_body` for ordinary edits. If code edits compatible body
rows directly, call `dynamic_world_upload` to reapply transforms, velocities,
mass, inertia, damping, and material values. Change geometry with
`dynamic_world_set_hull`.

`dynamic_world_state_texture` always returns zero. `DYNAMIC_BACKEND_CPU`
remains an alias for `DYNAMIC_BACKEND_BOX3D`, while
`dynamic_world_backend_name` returns `"box3d"`.

## Lifecycle

`dynamic_world_create` allocates both the Nano Script state mirror and a native
Box3D world. `dynamic_world_clear` destroys all user bodies while preserving
the world and configured bounds. Always call `dynamic_world_release` when a
world is no longer needed; it destroys the Box3D world and clears the opaque
handle. A copied `dynamic_world` shares that handle, so only one copy may own
and release it.

## Build and platform support

Box3D is portable C17 and is compiled directly into the dynamic feature
library by `lib/Makefile`. Run `make box3d` to download the pinned source
explicitly; the normal `make`, `make std`, and install flows also download it
when absent. They produce and install `dynamic.dylib` on macOS or `dynamic.so`
on Linux/Windows. The core Box3D library needs only the C runtime and `libm` on
Unix.

The previous module was pure Nano Script and could be lowered to Wasm or
embedded in generated Apple IDE projects. The Box3D backend uses external
native FFI, so those two targets now reject `use dynamic` instead of silently
building an application with no usable backend.

Run the focused regression coverage with:

```bash
bin/ns run test/dynamic_test.ns
```
