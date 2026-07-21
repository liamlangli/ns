# AGENTS.md

## Nano Script at a glance

Nano Script (`ns`) is a small, explicitly typed, data-oriented functional
programming language. The same `ns` tool can interpret source directly, build
applications or static libraries, run tests, emit selected native/WebAssembly
formats, transpile shader functions, and generate a host IDE project.

The language deliberately favors a compact syntax, concrete types, values and
plain data over class hierarchies or a large generic type system. Source files
use the `.ns` extension. A project is described by a TOML `ns.mod` manifest.

## Project shape and commands

A conventional application has this shape:

```text
project/
|-- AGENTS.md
|-- ns.mod
|-- main.ns
|-- README.md
`-- .gitignore
```

The common commands are:

- `ns init [path]`: scaffold a project in an existing directory, keeping files
  that are already present.
- `ns create <name>`: create a new directory and scaffold a project in it.
- `ns run [file.ns]`: interpret an explicit file; without an argument, use the
  current project's manifest entry and otherwise fall back to `main.ns`.
- `ns test [path]`: without a path, run every `*_test.ns` in the `test/`
  directory beside the nearest `ns.mod`; a project-directory path does the
  same. An explicit test file or non-project directory is also supported.
- `ns build [path]`: build a script or module. Manifest type `app` produces an
  executable and type `library` produces a static library.
- `ns project [path]`: generate the supported host-native IDE project below
  `bin/` from `ns.mod`.
- `ns --help`: show compiler targets and all current flags.

The manifest schema is `ns.mod/v1`. Important fields are `name`, `version`,
`type`, `source`, and `entry` (or `entries`). Local modules are discovered from
the project source tree. Only external runtime dependencies need entries under
`[[dependencies.runtime]]`.

```toml
schema = "ns.mod/v1"
name = "example"
version = "0.1.0"
type = "app"
source = "."
entry = "main.ns"

[[dependencies.runtime]]
name = "std"
version = ">=0.1.0"
```

Build output, generated IDE projects, profiles, and other generated artifacts
belong in `bin/` and should not be treated as source.

## Language guide

### Modules and entry points

- Declare a module with `mod name` and import one with `use name`.
- A local `use foo` resolves to the project's own `foo.ns` before external
  runtime modules.
- Applications normally define `fn main() { ... }`.
- `//` starts a line comment.
- Statements are newline-terminated; semicolons are not required.

```ns
use std

fn main() {
    print("hello from ns\n")
}
```

### Values and types

- Bind values with `let name = expression`; add `: type` when an explicit label
  is useful or required.
- Integer literals default to `i32`; floating-point literals default to `f32`.
- Scalar types are `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`,
  `f32`, `f64`, `bool`, `str`, `any`, and `void`.
- `str` is UTF-8. Standard substring offsets are byte offsets; use `utf8_len`
  when code-point count is required.
- Arrays use `[T]`, dictionaries use `[K: V]`, and sets use `set[T]`.
  Constructors take a capacity/count hint, for example `[u8](1024)`,
  `[str: i32](64)`, and `set[str](64)`.
- Dictionaries currently use fixed-capacity open addressing. Reading a missing
  key is an error; assigning a missing key inserts it.
- Define plain aggregate data with `struct`, aliases or unions with `type`, and
  function types with `(Args) -> Result`.
- Define a nominal integer-backed type with `enum`. Its underlying type defaults
  to `i32` and may be any builtin signed or unsigned integer type. Access members
  as `Enum.member`; enums implicitly lower to their underlying integer, while an
  integer requires an explicit `as Enum` cast.
- A union such as `type number = i32 | f64` accepts any member. Narrow or
  convert explicitly with `as`; there is no match-based union destructuring.
- `ref value` creates a mutable reference to mutable data. A copied plain value
  is independent, while refs and reference-backed containers share storage.
- `nil` is the empty reference value. Use `any` only at dynamic or FFI
  boundaries when a concrete type cannot describe the value.

```ns
struct point {
    x: f32,
    y: f32
}

enum os_platform: u8 {
    unknown = 0,
    macos,
}

type number = i32 | f64
type transform = (point) -> point

let origin = point(0, 0)
let samples = [f32](128)
let counts = [str: i32](32)
let n: number = 42
let exact = n as i32
let platform = os_platform.macos
let raw: u8 = platform
```

### Functions, blocks, and operators

- Define functions with `fn name(args): result { ... }`. The colon before the
  result is accepted; declarations also commonly spell it as
  `fn name(args) result`.
- Functions are values and may be passed where a compatible function type is
  expected.
- Blocks/closures use `{ arg, ... in ... }`, capture referenced outer values,
  and can be stored in an explicitly typed binding.
- Define external/native functions with `ref fn`; these are resolved as VM
  intrinsics or through FFI rather than implemented by an ns body.
- Overload an operator for a user type with `fn ops(operator)(...)` and provide
  `fn to_str(value): str` for formatted string conversion.
- Backtick strings interpolate expressions, for example
  `` `result = {value}` ``.
- Supported operator families include arithmetic (`+ - * / %`), comparison,
  equality, logical (`! && ||`), bitwise (`~ & | ^ << >>`), assignment, and
  explicit `as` casts. Parenthesize mixed expressions when intent is not
  obvious.

```ns
type binary_op = (i32, i32) -> i32

let add: binary_op = { a, b in
    return a + b
}

fn apply(a: i32, b: i32, op: binary_op): i32 {
    return op(a, b)
}
```

### Control flow and validation

- Conditions are explicit boolean expressions: `if condition { ... } else
  { ... }`.
- Iterate ranges/generators with `for value in start to end { ... }`.
- Use `loop condition { ... }` for a pre-test loop and `do { ... } loop
  condition` for a post-test loop.
- `break`, `continue`, and `return` have their usual structured-control roles.
- Use `assert condition` in tests and for executable invariants.

### Async work and tasks

Import `task` for language-level concurrency. Calling an `async fn` produces a
typed task handle; `await` suspends until it completes and yields its result.
`dispatch(queue, closure)` runs a zero-argument closure at `queue_main`,
`queue_worker`, or `queue_idle`. The module also supplies `wait`, `cancel`,
`done`, `cancelled`, and cooperative `sleep`.

Tasks are stackful and execution is coordinated by a cooperative interpreter
lock. Plain captured values are copied; refs, strings, arrays, dictionaries,
and sets share their backing storage. Shared mutable containers are not
automatically synchronized. Use await ordering or the lock/semaphore helpers in
`os`. Cancellation is cooperative, so long native calls can delay it.

```ns
use task

async fn compute(n: i32): i32 {
    sleep(1)
    return n * 2
}

fn main() {
    let pending = compute(21)
    let answer = await pending
    assert answer == 42
}
```

## Built-in library modules

Built-in declarations live in `lib/*.ns` (installed into the runtime reference
directory). Import them with `use <module>`. Read the relevant declaration file
before relying on exact signatures, constants, platform support, or ownership
rules.

| Module | Purpose and important constraints |
| --- | --- |
| `std` | Core libc/libm and string helpers: printing, basic file descriptors, math, numeric/string conversion, substring/unescape, and UTF-8 length. |
| `task` | VM-internal async task, dispatch, waiting, cancellation, status, queue, and sleep primitives. |
| `simd` | Pure-ns data types `float2`, `float3`, `float4`, `quatf`, and `mat4`, also used at shader boundaries. |
| `shader` | VM-internal transpilation of ordinary ns functions to MSL, GLSL 450, or HLSL shader source and entry names. Shader code accepts only the supported numeric/struct subset. |
| `os` | Native time/date, file and directory operations, environment/app-data paths, recursive scans/watches, dialogs, child project launch, device vibration, locks, and semaphores. |
| `io` | Native image loading and saving through `io_image` (`width`, `height`, `channels`, byte data). |
| `net` | Blocking native TCP/UDP sockets, shared receive-buffer access, file sending, and socket lifecycle. File descriptors are integer handles. |
| `http` | Minimal blocking HTTP/1.1 request parsing, responses/files/status helpers, client GET, and a complete static-file server built on `net`. |
| `term` | Native raw-terminal input, dimensions, buffered byte output, file byte I/O, and startup filename access for terminal apps. |
| `audio` | Apple native music/SFX loading, playback, pause/resume/seek, volume, duration/position, and error reporting. Handles are opaque integers. |
| `view` | Native application window/view lifecycle plus keyboard, pointer, gesture, clipboard, frame callback, size, scale, and GPU-device state. |
| `gpu` | Platform GPU access. The v2 surface (doc/gpu.md) treats the GPU as a processor with memory: 64-bit addresses from `gpu_malloc`, bindless u32 texture/sampler indices, one root pointer per draw/dispatch, and a frame ring for transient data; it runs host-side headless. The legacy v1 surface (buffers, pipelines, meshes, bindings, render passes) remains during migration. Apple uses Metal, Windows uses DirectX 12, and unsupported backends may return failure. |
| `ui` | Immediate-mode native UI rendering, layout, themes, input snapshots, widgets, scrolling, text editing/views, images, and renderer primitives on top of `view` and `gpu`. |

`std`, `task`, and `shader` are backed by interpreter intrinsics; `simd` is pure
Nano Script. The other feature modules are declarations backed by dynamically
loaded native libraries. Their APIs intentionally stay within the supported
FFI surface: scalars, strings, refs, arrays, small structs, and opaque numeric
or pointer handles.

Native module availability varies by host and by generated application target.
Do not assume that a module which is present in the source tree is linked into
every executable or IDE-generated app. Handle documented failure values and
check the implementation/declaration for the target platform.

## Guidance for changes

- Preserve Nano Script's explicit, minimal, data-oriented design. Prefer a
  concrete type and a small function over hidden behavior or a generic
  abstraction.
- Follow nearby `.ns` syntax and naming; do not import syntax from Swift,
  JavaScript, Rust, or another language unless the parser already supports it.
- Put reusable pure-language code in `.ns` modules. Put platform services behind
  `lib/*.ns` declarations and FFI-loaded dynamic libraries.
- Keep the `ns` interpreter language-only. Do not link UI, terminal, view, GPU,
  network, HTTP, image, audio, or other non-language runtime features directly
  into `bin/ns`.
- When adding or changing a built-in module, update its `lib/<module>.ns`
  declaration and keep native ABI signatures FFI-safe and synchronized with the
  backend.
- Add focused `*_test.ns` or C tests for behavior changes and run `make test`.
  For an application project, at minimum run `ns test <path>` and/or `ns run`
  as appropriate.
- Never edit generated files in `bin/` as the source of a fix.
- For runtime appearance changes, ask the user to check the running app
  visually; automated tests cannot fully validate rendering and native UI.
