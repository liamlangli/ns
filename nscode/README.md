# NSCode

Editors for Nano Script. This directory groups the NSCode app and its supporting
tools:

- **`native/`** — the shared NSCode editor and agent shell, written in Nano
  Script. `main.ns` runs through the native view/UI backends, while
  `web_main.ns` packages the same editor and renderer for GitHub Pages through
  Wasm and the browser Canvas UI backend. See `native/README.md`.
- **`nslang/`** — a VS Code extension providing syntax highlighting for `.ns`
  files, project run/build buttons, the native NSCode color theme, and TOML
  language-mode association for `ns.mod` manifests.
