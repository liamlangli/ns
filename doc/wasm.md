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

`ns build` writes `<bundle>/<safe-name>.wasm`, `<bundle>/<safe-name>.wasm.map`,
`<bundle>/ns-wasm.js`, and `<bundle>/index.html`, where `<bundle>` is `bin/` for
the manifest above and `bin/<target name>` for a browser `[[targets]]` table,
which keeps two browser targets of one project apart. The generated page title is the manifest
`name`. Its favicon is copied from the manifest `icon`; without one, the
official `ns.svg` installed with Nano Script is copied into the bundle and
used instead. A canvas `view_create` title is retained as its accessibility
label and does not replace the page title. If `<source>/assets` exists, its tree is synchronized into
`<bundle>/assets` without removing unrelated output. `-o path/app.wasm` puts all
browser artifacts and the selected favicon beside that path. Wasm replacement is atomic: a failed
compile leaves the previous runnable artifact intact. The bundle is also
incremental: `ns build` keeps it when the module is in place and every recorded
input — sources, manifest, assets, shell, icon, the installed `ns-wasm.js`, and
the `ns` executable — is unchanged since the recorded build. `--force` rebuilds
it anyway, and `ns clean` removes the whole `bin/` directory.

For a custom browser UI, set `shell` to an HTML file relative to the manifest.
The builder copies it to `<bundle>/index.html` after expanding three stable markers:
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
portable `std` imports, `simd`, WGSL shader metadata, browser `gpu`,
canvas-backed `view`, and the `ui` module. Arrays use a wasm32 descriptor
containing data, length, and capacity; plain structs use compiler-resolved
field offsets in linear memory. Unsupported dynamic or host-only operations
produce source-located build diagnostics; in particular arbitrary `any`,
unions, task/async, closures, dicts/sets, and the `io`, `net`, `http`,
`audio`, `compress`, `storage`, and `dynamic` modules are not browser
features, and `os` is limited to the documented portable subset. The browser
event loop remains owned by the generated shell, so `view_run` is nonblocking
and the exported `frame` function is the frame callback.

Every struct is laid out the same compact wasm32 way, including the structs a
lib module such as `view` or `ui` declares: fields sit in declaration order,
each aligned to its own size capped at four bytes, so a `str`, array or fn
handle takes four bytes, an `any` handle eight, a bool a four-byte slot, and an
f64 keeps its eight-byte payload at four-byte alignment. A native build gives a
lib struct the C layout the module published, because the library itself may
have allocated it; nothing does in the browser, where the middleware allocates
every struct out of the module's own linear memory, so the browser target uses
the layout above and `ns-wasm.js` reads the fields at those offsets.

## The `ui` module in the browser

Every `ui` entry point `lib/src/ui.c` implements is available on the Wasm
target, backed by a Canvas 2D renderer in `ns-wasm.js`: the batched renderer
(shapes, arcs, triangles, polylines, textured atlases, clipping, retained
rectangle batches), text (single line, wrapped, arced, vertical, measurement,
caret hit-testing), the safe-area and layout helpers, the immediate-mode widget
layer (buttons, sliders, colour pickers, hit regions), and the selectable
read-only label helpers. A `ui` project drives the canvas in 2D mode, so it
does not also hold a WebGPU context on the same canvas.

Two differences from a native build are inherent to the browser and are worth
designing around. Glyphs come from the page's font stack rather than the
signed-distance-field atlas, so `ui_text_width` measures with that stack while
the line box and cap-band metrics follow the shipped atlas ratios, keeping
vertical placement in step; and a face load (`ui_load_font`,
`ui_load_bitmap_font`, and the rest) reports the same failure a native renderer
reports for a missing atlas file, leaving text on the fallback face
`ui_primary_font` selects. The declarations `lib/ui.ns` carries that no native
library implements — themes, text views, dropdowns, toggles, lists, managed
textures, recorded layers — have no browser backend either.

Standalone `ns --wasm file.ns -o file.wasm` uses the same validated emitter but
does not generate a browser shell.

The browser shell follows the [WebAssembly Web API](https://webassembly.github.io/spec/web-api/),
the reload channel uses [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html),
and adapter/device setup and loss handling follow the current
[WebGPU specification](https://gpuweb.github.io/gpuweb/).
