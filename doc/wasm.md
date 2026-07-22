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
```

`ns build` writes `bin/<safe-name>.wasm`, `bin/ns-wasm.js`, and
`bin/index.html`. If `<source>/assets` exists, its tree is synchronized into
`bin/assets` without removing unrelated output. `-o path/app.wasm` puts all
three browser artifacts beside that path. Wasm replacement is atomic: a failed
compile leaves the previous runnable artifact intact.

The application must export `fn main()` or `fn main() i32`. The middleware
initializes its full-page canvas and WebGPU device first, calls `__ns_init`, and
then calls `main` once. A project may also declare:

```ns
fn frame(time_ms: f64, width: i32, height: i32) {
    // width and height are framebuffer pixels, including device pixel ratio.
}
```

The middleware invokes `frame` from `requestAnimationFrame`. WebGPU is the only
GPU backend; there is no WebGL fallback. `gpu_request_device(nil)` reports
whether an adapter/device is available, and GPU imports return failure values
or safely do nothing when it is not. Device loss triggers a new adapter/device
request. The Wasm module exports `memory`, `__ns_alloc`, `__ns_init`, `main`,
and optional `frame`. Shader functions named `vs_*`, `fs_*`, `ps_*`, or `cs_*`
are transpiled to WGSL during the build and stored with vertex reflection in
the `ns.shaders` custom section.

`ns run [path] --port <n>` builds first, binds only `127.0.0.1`, and serves the
bundle without opening a browser. The default port is 8080; port 0 asks the OS
for a free port and prints the selected URL. Responses use `Cache-Control:
no-store`; `.wasm` is served as `application/wasm`; GET, HEAD, and traversal
rejection are handled explicitly.

The installed `wasm_dev.ns` module owns the accept loop, project fingerprint,
100 ms change debounce, rebuild scheduling, WebSocket client list, and reload
broadcast. Its narrow native companion provides loopback/nonblocking sockets,
bounded HTTP header input, static-file transfer, RFC 6455 SHA-1/Base64 upgrade,
SIGPIPE-safe frames, stable path/size/nanosecond-mtime fingerprints, and a
synchronous `ns build`. `/__ns/reload` sends `{"type":"reload"}` after a good
build or `{"type":"build-error"}` after a failed one. The browser keeps the
last good app running and shows an error overlay until the next successful
reload.

The browser ABI supports typed scalar and enum computation, mutable globals,
functions/control flow, UTF-8 literal strings, checked arrays, plain structs,
portable `std` imports, `simd`, WGSL shader metadata, and browser `gpu`
imports. Arrays use a wasm32 descriptor containing data, length, and capacity;
plain structs use compiler-resolved field offsets in linear memory. Unsupported
dynamic or host-only operations produce source-located build diagnostics; in
particular arbitrary `any`, unions, task/async, closures, dicts/sets, and the
`view`, `ui`, `os`, `io`, `net`, `http`, and `audio` modules are not browser
features.

Standalone `ns --wasm file.ns -o file.wasm` uses the same validated emitter but
does not generate a browser shell.

The browser shell follows the [WebAssembly Web API](https://webassembly.github.io/spec/web-api/),
the reload channel uses [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html),
and adapter/device setup and loss handling follow the current
[WebGPU specification](https://gpuweb.github.io/gpuweb/).
