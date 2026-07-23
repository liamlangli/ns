# Nano Script Wasm Compute Path Tracer

From this directory, run `ns run --port 0` and open the printed loopback URL.
The sample compiles Nano Script compute, vertex, and fragment functions to WGSL.
The compute shader traces a Cornell box with multi-bounce diffuse transport,
next-event estimation, shadow rays, Russian roulette, and power-heuristic MIS.
Two RG11B10F textures hold a progressive running average while the camera is
still. A fullscreen pass applies ACES-style tone mapping and writes the result
to the browser's 8-bit swapchain.

Drag with the left mouse button to orbit. Camera motion is applied directly,
without damping, and immediately resets accumulation. Resize also recreates
and resets the accumulation textures.

The canvas-backed `ref view` is passed to `gpu_request_device`. Browsers with
`texture-formats-tier1` use RG11B10F as a storage texture directly; the Wasm
bridge falls back internally to RGBA16F when that optional feature is missing.
The public Nano Script code remains on the v2 GPU API in either case.

Editing any project input triggers a debounced rebuild and browser reload.
The emitted `.wasm.map` lets Chrome DevTools show the original `.ns` source.
