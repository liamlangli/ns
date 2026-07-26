# Dynamic Module

`dynamic` is a rigid-body simulation module for convex shapes, written as four
GPU compute kernels: **broad phase**, **narrow phase**, **constraint solve**,
and **integration**. It is pure Nano Script — there is no `dynamic.dylib` and
no new FFI. `lib/dynamic.ns` is the whole module.

```ns
use dynamic

let world = dynamic_world_create(32, dynamic_config_default())

let cube = dynamic_body_box(float3 { x: 0.5, y: 0.5, z: 0.5 }, 1.0)
cube.position = float3 { x: 0.0, y: 3.0, z: 0.0 }
let index = dynamic_world_add(world, cube)
dynamic_world_set_hull(world, index, corners, 8)   // eight body-space points

dynamic_world_step(world)                          // advances config.dt
let body = dynamic_world_body(world, index)        // position, orientation, ...
```

`nscode/dynamic` is a 3D sample that drives a world of cubes, tetrahedra, a
hexagonal prism and spheres, draws them through the `gpu` module, and reports
what each stage costs.

## Shapes are one thing: a point hull with a margin

Every body is a convex point hull plus a sphere-sweep radius, so one support
function covers the whole shape catalogue:

| Shape | Hull | Margin |
| --- | --- | --- |
| sphere | one point at the origin | the radius |
| box / cube | its eight corners | 0 |
| convex mesh | its vertices, up to `DYNAMIC_MAX_HULL_VERTICES` | 0 |
| capsule, rounded box | the segment or corner points | the round-off radius |

`dynamic_body_sphere`, `dynamic_body_box` and `dynamic_body_hull` fill in the
inverse mass and the diagonal inverse inertia; `dynamic_world_set_hull`
installs the points. A hull's inertia is approximated by the box of its
extents, which is the usual fallback without a mesh integrator.

## The state image

The world lives in one float4 image of `capacity` columns — one column per
body — and `DYNAMIC_ROW_COUNT` rows:

| Row | Contents |
| --- | --- |
| 0 | configuration: substep dt, slop, bias, relaxation, gravity, margin, bounds |
| 1 | position.xyz, bounding radius |
| 2 | orientation quaternion |
| 3 | linear velocity.xyz, inverse mass |
| 4 | angular velocity.xyz, margin radius |
| 5 | restitution, friction, linear damping, angular damping |
| 6 | inverse inertia diagonal (body space), hull vertex count |
| 7 + k | pair slot k: partner + 1 (0 = empty), accumulated normal and friction impulses |
| 11 + k | contact slot k: normal.xyz (partner → body), penetration depth |
| 15 + k | contact slot k: contact point.xyz |
| 19 + i | hull vertex i, in body space |

Configuration travels in the image rather than in the root argument, so every
stage dispatches with the same four root words: read texture, write texture,
capacity, body count.

Each stage dispatches over the whole image, and **every texel is written by
exactly one invocation**. The first row of a block owns it: that invocation
computes once and writes the whole block, the other rows of the block write
nothing, and every row the stage does not own is copied through. No stage needs
an atomic or a prefix scan, which is what makes the pipeline a plain sequence
of dispatches.

## The pipeline

### Broad phase — `cs_dynamic_broad_phase`

One invocation per body walks the other bodies and keeps up to
`DYNAMIC_MAX_CONTACTS` whose bounding spheres overlap within
`config.margin`, writing them into its own pair slots. A partner that was
already in a slot keeps its accumulated impulses, which is what warm-starts a
resting contact.

The scan is O(n) per body with no spatial structure. A uniform grid or a sorted
sweep needs atomics or a prefix scan across invocations, neither of which the
current compute surface exposes; the bounding-sphere reject is cheap enough
that a few hundred bodies stay comfortable.

### Narrow phase — `cs_dynamic_narrow_phase`

One invocation per contact slot runs **GJK** on the Minkowski difference of the
two hulls, and **EPA** on the enclosing tetrahedron GJK leaves behind. EPA
expands the polytope toward the nearest face of the difference, which gives the
contact normal and the penetration depth; each polytope vertex carries the
witness points on both bodies, so the contact point is the barycentric mix of
the closest face's witnesses.

Both run in one fn, `dynamic_collide`. A shader has no way to hand a simplex or
a polytope across a call — local arrays are values in registers and no target
passes them as parameters — so GJK, EPA and the support function share one
scope. The two hulls are rotated into world space once into local arrays, and
support then costs one dot product per point.

Flat faces converge exactly: a box against a box reports the axis normal and
the depth to the last decimal. A sphere is smooth, so its normal is only as
precise as the polytope EPA had budget to build (`DYNAMIC_EPA_*`); the witness
points stay exact because they come from the analytic support function.

### Constraint solve — `cs_dynamic_solve`

One invocation per body solves its contact slots and the six world-bound
half-spaces, in `config.iterations` passes. Each contact applies a normal
impulse with a Baumgarte positional bias and restitution above a rest speed,
then Coulomb friction along the tangent it is actually sliding on, clamped by
the normal impulse the contact has accumulated.

Both bodies of a pair solve the same contact independently and each applies
only its own share, which keeps a dispatch free of atomics; `relaxation` bounds
the overshoot that parallel solving costs. The world bounds contribute one
contact per hull vertex below a plane, which is what gives a resting box a
manifold instead of a single wobbling point.

Contacts between two bodies are single-point (EPA's closest feature), so a box
resting on another box is stable but not as stiff as a clipped face manifold
would be.

### Integration — `cs_dynamic_integrate`

Semi-implicit Euler: gravity and damping enter the velocity, the position
follows it, and the orientation integrates the angular velocity as a quaternion
derivative and is renormalized. Gravity enters here so the next step's solver
sees it before any contact is resolved.

## Backends

The kernels are ordinary ns fns, and there are two ways to run them:

- **Dispatched.** With `config.prefer_gpu`, `dynamic_world_create` compiles the
  four kernels through `gpu_shader_compute` and allocates two RGBA32F state
  textures. `dynamic_world_step` then ping-pongs them with `gpu_dispatch`.
- **Host.** Otherwise `dynamic_world_step` runs the very same fns through the
  `shader_host_*` intrinsics, one invocation at a time, against the host copy
  of the image.

There is no second implementation of the physics to keep in sync: only the loop
that drives the invocations differs. That is also what makes the pipeline
testable without a device — `test/dynamic_test.ns` runs the real kernels.

`prefer_gpu` is **off by default**, because no backend can carry the state
image yet:

- No native backend registers the gpu v2 ops at all
  (`gpu_v2_set_backend` is never called from `gpu.metal.m` or `gpu.dx12.c`), so
  on macOS and Windows every v2 texture and dispatch call is a safe no-op.
- The WebGPU path in `lib/ns-wasm.js` implements v2, but the compute
  write-texture format is hard-coded to `rg11b10ufloat` (falling back to
  `rgba16float`) in the WGSL and GLSL emitters, which cannot hold signed,
  full-range positions and velocities.

MSL declares a compute write target without a format, so the Metal path works
as soon as a backend registers v2 compute; WGSL and GLSL additionally need the
storage format to follow the bound texture. Until then, `dynamic_world_backend`
reports `cpu` and the same simulation runs host-side.

The v2 surface has no texture readback, so a dispatched world cannot be queried
with `dynamic_world_body`. Render it from `dynamic_world_state_texture`
instead — the body rows are the transforms.

## Configuration

`dynamic_config_default()` is a reasonable starting point:

| Field | Default | Meaning |
| --- | --- | --- |
| `gravity` | (0, -9.81, 0) | acceleration added per substep |
| `dt` | 1/60 | seconds one `dynamic_world_step` advances |
| `substeps` | 1 | how many times the pipeline runs per step |
| `iterations` | 4 | solver passes per substep |
| `slop` | 0.005 | penetration tolerated before the bias acts |
| `bias` | 0.2 | Baumgarte factor of the positional correction |
| `relaxation` | 0.8 | under-relaxation of the impulses |
| `margin` | 0.08 | broad-phase bounds inflation |
| `bounds_min` / `bounds_max` | ±3, 0..7 | the containing box |
| `bounds_enabled` | true | solve the six bound planes |
| `bounds_restitution` | 1.0 | scales a body's restitution against the bounds |
| `prefer_gpu` | false | dispatch the kernels when the device can run them |

## What the module leans on

Two pieces of the language surface exist because this module needs them, and
both are useful on their own:

- **Fixed-capacity local arrays in shader fns.** `let faces = [i32](72)` inside
  a shader fn declares `array<i32, 72>` in WGSL and `int faces[72]` in
  MSL/HLSL/GLSL. The length must be a constant expression over literals and
  `lit` bindings, indexing is by `i32`, and `.len` folds to the constant. Array
  parameters, returns and growth stay unsupported. EPA's polytope cannot be
  written without them.
- **Host execution of compute fns** (`shader_host_bind`, `shader_host_root`,
  `shader_host_invocation`, `shader_host_swap`, `shader_host_release`). Binding
  the image pair, the root words and an invocation coordinate lets the
  interpreter run a compute fn exactly as a device would, which is both this
  module's CPU backend and the way any ns compute shader can be tested
  headlessly.

`lit` bindings also fold into generated shader source, and a fn a shader calls
may now live in a `use`d module rather than only in the file being transpiled —
which is what lets a *library* ship GPU kernels.
