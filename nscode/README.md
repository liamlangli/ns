# NSCode

Editors for Nano Script. This directory groups the NSCode app and its supporting
tools:

- **`ns_lens/`** — a perspective treemap code atlas with distance LOD, bounded
  source streaming and a focused editor. See `ns_lens/README.md`.
- **`ns_profiler/`** — the native timeline viewer for `bin/ns.profile` reports
  and live profile streams (`ns profiler`, `ns profiler --live`).
- **`ns_tex/`** — the texture cooker used by `ns cook` and `ns patch`; it reads
  PNG/JPEG/EXR sources and emits GPU-ready atlases and compressed textures.
  See `ns_tex/README.md`.
- **`ns_vision/`** — a live desktop window dome written in Nano Script, used as
  a showcase for the desktop capture and GPU passes. See `ns_vision/README.md`.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files, project run/build buttons, the native NSCode color theme, and TOML
  language-mode association for `ns.mod` manifests.
