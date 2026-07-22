# Nano Script Wasm Ray Tracer

From this directory, run `ns run --port 0` and open the printed loopback URL.
The sample compiles Nano Script vertex and fragment functions to WGSL and ray
traces a Cornell box on the full-page WebGPU canvas. The open-front room, two
boxes, and ceiling light are meshes of explicitly wound, one-sided triangles.
The fragment shader computes four-sample area-light shadows, tone mapping, and
vignetting. Drag the canvas with the left mouse button to orbit the camera;
the camera eases toward the dragged angle and does not rotate on its own. A single full-canvas primitive
only launches the per-pixel rays; no triangle is present in the rendered scene.

The canvas-backed `ref view` is passed to `gpu_request_device`. The sample
continues safely when WebGPU is unavailable.

Editing any project input triggers a debounced rebuild and browser reload.
The emitted `.wasm.map` lets Chrome DevTools show the original `.ns` source.
