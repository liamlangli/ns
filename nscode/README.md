# NSCode

Editors for Nano Script. This directory groups the NSCode app and its supporting
tools:

- **`native/`** — the shared NSCode editor and agent shell, written in Nano
  Script. `main.ns` runs through the native view/UI backends, while
  `web_main.ns` packages the same editor and renderer for GitHub Pages through
  Wasm and the browser Canvas UI backend. See `native/README.md`.
- **`dynamic/`** — a 3D test bench for the Box3D-backed `dynamic` built-in
  module: convex bodies simulated natively and drawn with the `gpu` module.
  See `dynamic/README.md`.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files, project run/build buttons, the native NSCode color theme, and TOML
  language-mode association for `ns.mod` manifests.
