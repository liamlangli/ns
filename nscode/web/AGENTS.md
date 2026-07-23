# AGENTS.md — NSCode web

The web editor is a Nano Script Wasm project with a dependency-free browser
shell. The repository-level `AGENTS.md` applies in addition to these rules.

## Architecture boundaries

- Keep tokenization, validation, and execution in `*.ns` and compile them with
  the repository's `ns` executable.
- Keep `assets/app.js` a small browser adapter. It may manage DOM editing,
  files, persistence, and presentation, but language behavior belongs in the
  Nano Script backend.
- Do not add npm, package-manager metadata, a bundler, TypeScript, WebGPU, or
  the `ui` module. The app must remain buildable with `bin/ns build nscode/web`.
- Do not check generated `bin/` artifacts into source or edit them as the
  source of a fix.
- Preserve the standard `ns-wasm.js` string ABI when adding exported backend
  functions.

## Product behavior

- Keep the editor usable with keyboard-only input and at narrow viewport
  widths.
- Keep source code local to the browser. Do not introduce network execution or
  analytics without an explicit product decision.
- Treat the full Nano Script compiler as authoritative. Clearly document any
  smaller subset implemented by the in-browser evaluator.

## Verification

Run compile and automated checks for changes in this directory:

```sh
make bin/ns
bin/ns test nscode/web
bin/ns build nscode/web
node --check nscode/web/assets/app.js
```

Leave interactive and visual browser validation to the user unless they ask
for it explicitly.
