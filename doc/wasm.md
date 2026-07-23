# WebAssembly browser projects

Nano Script browser applications use the normal `app` module type plus the
orthogonal Wasm target:

```toml
schema = "ns.mod/v1"
name = "example"
version = "0.1.0"
type = "app"
target = "wasm"
source = "."
entry = "main.ns"
# Optional custom browser page:
# shell = "index.html"
```

`ns build` writes `bin/<safe-name>.wasm`, `bin/<safe-name>.wasm.map`,
`bin/ns-wasm.js`, and `bin/index.html`. The generated page title is the manifest
`name`. Its favicon is copied from the manifest `icon`; without one, the
official `ns.svg` installed with Nano Script is copied into the bundle and
used instead. A canvas `view_create` title is retained as its accessibility
label and does not replace the page title. If `<source>/assets` exists, its tree is synchronized into
`bin/assets` without removing unrelated output. `-o path/app.wasm` puts all
browser artifacts and the selected favicon beside that path. Wasm replacement is atomic: a failed
compile leaves the previous runnable artifact intact.

For a custom browser UI, set `shell` to an HTML file relative to the manifest.
The builder copies it to `bin/index.html` after expanding three stable markers:
`{{wasm}}` is the generated module filename, `{{title}}` is the HTML-escaped
manifest name, and `{{favicon}}` is the copied favicon filename. The standard
`ns-wasm.js` module remains available beside the page, and a project `assets/`
tree is still synchronized into the bundle. Omitting `shell` retains the
generated full-page canvas shell.

The Wasm module carries the standard `sourceMappingURL` custom section pointing
to its sibling Source Map v3 file. Generated columns are absolute byte offsets
in the Wasm binary, as required by the WebAssembly debugging convention. The
map preserves original project filenames and line/column locations across the
merged translation unit and embeds `sourcesContent`, so Chrome DevTools can
display and debug `.ns` sources without exposing the project source tree as
ordinary static files.

The application must export `fn main()` or `fn main() i32`. The middleware
initializes its full-page canvas and WebGPU device first, calls `__ns_init`, and
then calls `main` once. A project may also declare:

```ns
fn frame(time_ms: f64, width: i32, height: i32) {
    // width and height are framebuffer pixels, including device pixel ratio.
}
```

The middleware invokes `frame` from `requestAnimationFrame`. WebGPU is the only
GPU backend; there is no WebGL fallback. `view_create` returns a `ref view`
backed by the generated HTML canvas. Its logical/framebuffer dimensions,
display ratio, pointer/buttons/scroll, keyboard edges, gesture state, and
clipboard cache are maintained by the browser middleware. The shell suppresses
the canvas focus outline and context menu; pointer drags retain capture until
release, including when the pointer moves outside the canvas. Pass that view to
the normal typed `gpu_request_device(v: ref view)` API; it reports whether the
automatically requested adapter/device is available. GPU calls return failure
values or safely do nothing when it is not. Device loss triggers a new
adapter/device request. The Wasm module exports `memory`, `__ns_alloc`,
`__ns_init`, `main`, and optional `frame`. Shader functions named `vs_*`,
`fs_*`, `ps_*`, or `cs_*` are transpiled to WGSL during the build and stored
with vertex reflection in the `ns.shaders` custom section.

`ns run [path] --port <n>` builds first, binds only to localhost, and serves the
bundle without opening a browser. The default port is 9001; port 0 asks the OS
for a free port and prints the selected URL. Responses use `Cache-Control:
no-store`; `.wasm` is served as `application/wasm`, `.wasm.map` as
`application/json`; GET, HEAD, and traversal
rejection are handled explicitly.

The installed `wasm_dev.ns` module owns the accept loop, project fingerprint,
100 ms change debounce, rebuild scheduling, WebSocket client list, and reload
broadcast. Its narrow native companion provides loopback/nonblocking sockets,
bounded HTTP header input, static-file transfer, RFC 6455 SHA-1/Base64 upgrade,
SIGPIPE-safe frames, stable path/size/nanosecond-mtime fingerprints, and a
synchronous `ns build`. `/__ns/reload` sends `{"type":"reload"}` after a good
build or `{"type":"build-error"}` after a failed one. The browser keeps the
last good app running and shows an error overlay until the next successful
reload. The middleware opens this development socket only on loopback pages;
deployed static bundles do not reconnect to a nonexistent endpoint.

The browser ABI supports typed scalar and enum computation, mutable globals,
functions/control flow, UTF-8 strings including runtime concatenation and
lexicographic comparison, checked arrays, plain structs,
portable `std` imports, `simd`, WGSL shader metadata, and browser `gpu`
and canvas-backed `view` imports. Arrays use a wasm32 descriptor containing
data, length, and capacity; plain structs use compiler-resolved field offsets
in linear memory. Unsupported dynamic or host-only operations produce
source-located build diagnostics; in particular arbitrary `any`, unions,
task/async, closures, dicts/sets, and the `ui`, `os`, `io`, `net`, `http`, and
`audio` modules are not browser features. The browser event loop remains owned
by the generated shell, so `view_run` is nonblocking and the exported `frame`
function is the frame callback.

Standalone `ns --wasm file.ns -o file.wasm` uses the same validated emitter but
does not generate a browser shell.

The browser shell follows the [WebAssembly Web API](https://webassembly.github.io/spec/web-api/),
the reload channel uses [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html),
and adapter/device setup and loss handling follow the current
[WebGPU specification](https://gpuweb.github.io/gpuweb/).
