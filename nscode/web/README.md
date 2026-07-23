# NSCode for the web

NSCode is a dependency-free Nano Script playground. Its tokenizer, diagnostics,
and execution engine are written in Nano Script and compiled to WebAssembly by
`ns build`; a small plain-JavaScript adapter connects that backend to the browser
editor.

There is no npm install, bundler, WebGPU, or `ui` module requirement.

## Build and test

From the repository root:

```sh
make bin/ns
bin/ns test nscode/web
bin/ns build nscode/web
```

The static site is written to `nscode/web/bin/`. To build it and start the
loopback live-reload server:

```sh
cd nscode/web
../../bin/ns run --port 8443
```

Then open `http://localhost:8443`. Any modern browser with WebAssembly and ES
module support is sufficient.

## Architecture

```text
web/
|-- ns.mod             # Wasm application manifest; no external dependencies
|-- backend.ns         # Public backend entry points
|-- tokenizer.ns       # Nano Script lexer and highlight spans
|-- runtime.ns         # Nano Script playground evaluator and diagnostics
|-- test/
|   `-- backend_test.ns
|-- index.html          # Custom ns Wasm HTML shell
`-- assets/
    |-- app.js          # DOM/Wasm adapter
    `-- app.css         # Responsive editor presentation
```

The browser calls `ns_highlight`, `ns_diagnostics`, and `ns_run` in
`nscode.wasm`. Strings cross the standard `ns-wasm.js` descriptor ABI. Source
stays local to the browser; Open and Save use local files, and editing state is
kept in `localStorage`.

The embedded evaluator intentionally targets an educational language subset:
typed function declarations and calls, recursion, `let`/`lit`, assignment,
numbers, strings, booleans, interpolation, arithmetic and comparisons,
`if`/`else`, half-open `for ... in ... to ...` ranges, and common numeric
helpers. The normal `ns` compiler remains the authority for complete language
and project builds.

## Generated output

`bin/` is generated and must not be edited. `ns build` copies the custom HTML
shell and `assets/`, then writes `nscode.wasm`, its source map, the standard
Wasm browser adapter, and the default Nano Script favicon beside them.
