# NSCode

Editors for Nano Script. This directory groups the NSCode app and its supporting
tools:

- **`nn/`** — MNIST handwritten digit recognition in pure Nano Script: a
  convolutional network whose forward, backward and Adam kernels are ns
  functions transpiled to GPU compute shaders (99.29% held-out accuracy). It
  needs a GPU; its window shows the model structure as instanced 3D cubes.
  See `nn/README.md`.

- **`ns_lens/`** — a perspective treemap code atlas with distance LOD, bounded
  source streaming and a focused editor. See `ns_lens/README.md`.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files, project run/build buttons, the native NSCode color theme, and TOML
  language-mode association for `ns.mod` manifests.
