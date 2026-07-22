# Nano Script Wasm + WebGPU

From this directory, run `ns run --port 0` and open the printed loopback URL.
The sample compiles Nano Script vertex and fragment functions to WGSL, uploads
a colored triangle, and draws it on a full-page WebGPU canvas. It continues
safely when WebGPU is unavailable.

Editing any project input triggers a debounced rebuild and browser reload.
