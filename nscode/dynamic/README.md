# NSCode Dynamic

A 3D test bench for the Box3D-backed [`dynamic`](../../doc/dynamic.md) built-in
module: a world of convex bodies — cubes, tetrahedra, a hexagonal prism and
spheres — simulated natively and drawn with the `gpu` module.

```bash
bin/ns run nscode/dynamic
```

## What it shows

- **Box3D simulation, per frame.** The HUD reports the wall-clock cost of one
  `dynamic_world_step`, the mirrored contact count, and the active backend.
- **Simulation and drawing from the same data.** `shapes.ns` builds each shape
  once as the point hull the module collides and as the triangle list the
  renderer transforms, so what you see is what is simulated.
- **Continuous frames.** Frames are drawn on demand on every platform, so the
  frame callback asks for the next one with `view_request_frame`. The
  simulation keeps running with no input, and the HUD's fps readout is what the
  pipeline and the renderer together cost.

## Layout

| File | Contents |
| --- | --- |
| `main.ns` | window and device lifecycle, camera, input, the frame, the `ui` overlay |
| `render.ns` | the `gpu` triangle pipeline: shaders, vertex buffer, screen pass |
| `shapes.ns` | shape catalogue: hull points for physics, triangles for drawing |
| `matrix.ns` | the 4x4 transforms, applied host-side |

The vertex shader is a pass-through: the `shader` module does not lower mat4
operators yet, so `matrix.ns` applies the model, view and projection transforms
and hands the GPU clip-space positions with a flat-shaded colour. `ui` is used
only for the overlay text.

## Controls

| Input | Action |
| --- | --- |
| drag | orbit the camera |
| scroll | zoom |
| left click | drop a body |
| space | pause and resume |
| `R` | reseed the scene |

## Backend

The simulation runs through the pinned Box3D v0.1.0 download in
`dynamic.dylib` or `dynamic.so`; the HUD reports `box3d`. The module mirrors
transforms and contacts into its Nano Script state array so this sample can
keep rendering with the existing code. See
[doc/dynamic.md](../../doc/dynamic.md) for shape, configuration, and platform
details.
