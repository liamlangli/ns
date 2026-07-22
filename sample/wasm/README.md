# Nano Script Wasm + WebGPU

From this directory, run `ns run --port 0` and open the printed loopback URL.
The sample compiles Nano Script vertex and fragment functions to WGSL, uploads
a colored triangle, creates a canvas-backed `ref view`, passes that view to
`gpu_request_device`, and draws on the full-page WebGPU canvas. It continues
safely when WebGPU is unavailable.

Editing any project input triggers a debounced rebuild and browser reload.
