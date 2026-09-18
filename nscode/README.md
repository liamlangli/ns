# NSCode

Editors for Nano Script. This directory groups the NSCode app and its supporting
tools:

- **`native/`** — the NSCode editor and agent shell, written in Nano Script
  and rendered through the native view/UI backends. See `native/README.md`.
- **`dynamic/`** — a 3D test bench for the Box3D-backed `dynamic` built-in
  module: convex bodies simulated natively and drawn with the `gpu` module.
  See `dynamic/README.md`.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files, project run/build buttons, the native NSCode color theme, and TOML
  language-mode association for `ns.mod` manifests.
