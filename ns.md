# Nano Script (ns) Project Guide for AI Agents

This file is an onboarding map for the whole repository. It is meant to help an AI agent quickly understand where things live, how the toolchain fits together, and what to inspect first for a given task.

## 1) What this repository is

**Nano Script** is a minimal, data-oriented functional programming language with:
- an interpreter/runtime,
- a compiler pipeline with multiple backend emitters,
- and a browser playground.

At repo level, the project is polyglot but mostly **C** for core tooling and **TypeScript/JavaScript** for web/editor integrations.

---

## 2) High-level architecture

The core executable (`ns`) follows this pipeline:

1. **Tokenize** source (`ns_token.c`)
2. **Parse AST** (`ns_ast*.c`)
3. **Build VM symbols / bytecode-like structures** (`ns_vm_parse.c`)
4. **Evaluate / run** (`ns_vm_eval.c`)
5. Optional compiler lowering chain:
   - AST → **SSA** (`ns_ssa.c`)
   - SSA → target backend(s):
     - AArch64 words (`ns_aarch.c`)
     - Mach-O (`ns_macho.c`)
     - PE (`ns_pe.c`)
     - WebAssembly (`ns_wasm.c`)

Entrypoint and CLI routing are in `src/ns.c`.

---

## 3) Repository layout (what each top-level area does)

- `src/` — Core compiler/interpreter/runtime implementation in C.
- `include/` — Public headers for lexer/parser/VM/SSA/backends/platform utils.
- `lib/` — Standard/runtime dynamic libraries and Nano Script standard modules (`*.ns`) plus OS/GPU/view C implementations.
- `nscode/` — NSCode editors. `nscode/web/` is the WebGPU browser playground;
  `nscode/cli/` is the terminal editor written in ns (uses the `term` module).
- `sample/ns/` — Language feature examples (expressions, parse, fib, GUI, etc.).
- `sample/c/` — C embedding sample.
- `test/` — C tests (currently JSON-focused test target in root Makefile).
- `doc/` — Design/reference notes (tokens, block semantics, SSA notes, async/task, module manager).

---

## 4) Build system and binaries

The root `Makefile` is the orchestrator:

- Builds `bin/ns` (main CLI), `bin/libns.a`, and std dynamic libs via included makefiles.
- Supports platform-specific flags and outputs for Linux/macOS/Windows.
- Includes optional Apple XCFramework packaging flow (`make xc`) for macOS+iOS arm64 static libs.
- Main knobs:
  - `NS_DEBUG=1|0`

Common commands:

- `make` — build everything (core + libs + std libs)
- `make test` — build and run tests (currently `bin/ns_json_test`)
- `make run` — run `ns`
- `make clean`
- `make install` — install binaries/libs into `~/ns/...` and ask the user to add `~/ns/bin` to `PATH`

---

## 5) `ns` CLI behavior quick reference

`src/ns.c` parses flags and routes execution:

- `-t|--token` tokenization only
- `-a|--ast` print AST
- `--ssa` print SSA lowering
- `--aarch` print AArch64 lowering
- `--macho` emit Mach-O executable
- `--macho-o` emit Mach-O object
- `--wasm` emit WebAssembly
- `--pe` emit PE executable
- `--shader <msl|glsl|hlsl>` transpile shader fns (see `src/ns_shader.c`) to
  target shader source; `--entry <name>` picks one entry (default: every
  `vs_*`/`fs_*`/`ps_*` fn), GLSL emits one file per stage (`<out>.vert` /
  `<out>.frag`); `--shader-bin` additionally compiles the source when the
  platform toolchain is installed (`xcrun metal` → metallib, `glslc` /
  `glslangValidator` → SPIR-V, `dxc` → DXIL) and only warns when it is not
- `-s|--symbol` print symbol table
- `-v|--version`, `-h|--help`
- `-o|--output` output path for emitters
- `build [path]` compile and link a script/module to an executable or static
  library; infers module artifact type from `ns.mod` and accepts `--exe` /
  `--app` / `--lib`
- `ns.mod` app manifests may set `icon = "path/to/image.png"`; the path is
  relative to the manifest root. `ns run` passes it to the native view runtime,
  and Darwin app builds package it as the bundle icon.

No filename defaults to version + REPL.
With filename and no analysis/emit flag, it evaluates and prints result.

---

## 6) Core modules in `src/` (mental map)

- **Language front-end**
  - `ns_token.c` — lexer/tokenizer
  - `ns_ast.c`, `ns_ast_stmt.c`, `ns_ast_expr.c`, `ns_ast_print.c` — parser + AST printing
  - `ns_type.c` — type system helpers

- **Execution**
  - `ns_vm_parse.c` — maps AST into VM structures/symbols
  - `ns_vm_eval.c` — evaluator/runtime execution
  - `ns_vm_lib.c` — runtime library bindings/integration
  - `ns_vm_print.c` — VM state/symbol print helpers
  - `ns_repl.c` — REPL shell behavior

- **IR/backend/toolchain**
  - `ns_ssa.c` — AST to SSA lowering
  - `ns_aarch.c`, `ns_amd64.c` — architecture-related lowering/assembly helpers
  - `ns_macho.c`, `ns_pe.c`, `ns_wasm.c` — file format emitters
  - `ns_asm.c` — asm target abstraction/helpers
  - `ns_shader.c` — transpiles ns fns to shader source (MSL / Vulkan GLSL /
    HLSL); powers both the `--shader` CLI mode and the runtime `use shader`
    module (`shader_transpile(fn, target)`), so logic and shaders can both be
    written in ns (see `sample/ns/shader.ns` and `lib/shader.ns`)

- **Runtime/platform utilities**
  - `ns_os.c`, `ns_net.c`, `ns_json.c`, `ns_fmt.c`, `ns_def.c`, `ns_jit.c`

---

## 7) Language/tooling ecosystem in this repo

### Browser playground (`nscode/web/`)
- WebGPU-based in-browser Nano Script playground/editor.
- Uses the Vite dev server on `localhost`, which satisfies WebGPU's secure context requirement without a local certificate.
- Includes interpreter/editor/rendering modules in `nscode/web/src/`.

### Terminal editor (`nscode/cli/`)
- A "kilo"-style text editor written in ns, run with `bin/ns run nscode/cli/main.ns`.
- Uses the native `term` module (`lib/term.ns`) for raw mode, key input and I/O.

### Standard library modules (`lib/`)
- `*.ns` modules declare `ref fn` bindings to C implementations in `lib/src/`,
  compiled position-independent into the binary and resolved through the static
  symbol table in `src/ns_vm_lib.c`.
- `std`, `io`, `view`, `gpu`, `term`, and the networking pair `net` / `http`.
- `net` (`lib/net.ns`, `lib/src/net.c`) provides TCP/UDP socket primitives;
  `http` (`lib/http.ns`, `lib/src/http.c`) adds HTTP/1.1 helpers and a complete
  static file server (`http_serve_static`). See `doc/net.md` and the
  `sample/ns/http_server.ns` / `sample/ns/tcp_echo.ns` examples.

---

## 8) Docs and samples worth reading first

For fast onboarding, recommended order:

1. `README.md` — project goals + syntax + components
2. `src/ns.c` — authoritative CLI flow / execution modes
3. `Makefile` (+ included makefiles) — build outputs and platform branching
4. `doc/ssa.md` — SSA & backend design intent
5. `doc/block.md`, `doc/ref.md`, `doc/operators.md`, `doc/token.md` — language semantics cheatsheets
6. `sample/ns/*.ns` — practical examples to test parsing/execution and language features
7. `nscode/web/README.md` — browser playground setup

---

## 9) Typical task playbooks for future AI agents

### A) “Add a language feature”
Touch likely areas in this order:
1. `src/ns_token.c` (if syntax/token change)
2. `src/ns_ast_expr.c` / `src/ns_ast_stmt.c` (parse changes)
3. `src/ns_type.c` (typing rules)
4. `src/ns_vm_parse.c` + `src/ns_vm_eval.c` (runtime semantics)
5. `src/ns_ast_print.c`, `src/ns_vm_print.c` (debug print updates)
6. `doc/*.md` + `sample/ns/*.ns` (documentation/examples)

### B) “Add a new compiler emitter or improve lowering”
1. `src/ns_ssa.c` (IR shape)
2. backend file (`src/ns_wasm.c` / `src/ns_macho.c` / `src/ns_pe.c` / arch file)
3. `src/ns.c` option routing
4. headers in `include/`
5. tests/samples for emitted artifact flow

### C) “Work on web playground UX/rendering”
- `nscode/web/src/editor.ts`, `ui.ts`, `renderer.js`, `gpu.ts`, `syntax.ts`
- local run via `npm run dev`

---

## 10) Current signals about maturity / caveats

- Docs indicate some advanced features are still evolving (e.g., SSA notes mention conservative typing and incomplete loop PHI handling).
- Test suite appears lightweight from top-level target (`ns_json_test`), so many changes should be validated with direct sample runs.
- Multi-platform build support exists, but backend capabilities vary by target format/architecture.

---

## 11) Fast command cheat sheet

From repo root:

```bash
# Build everything
make

# Run CLI with help
./bin/ns --help

# Try parser/IR stages
./bin/ns -t sample/ns/main.ns
./bin/ns -a sample/ns/main.ns
./bin/ns --ssa sample/ns/main.ns

# Run tests
make test

# Build browser playground (in nscode)
cd nscode/web && npm install && npm run dev
```

---

## 12) One-paragraph summary

This repo is a full Nano Script toolchain: a C-based language core (lexer/parser/type/VM), SSA-based lowering pipeline with multiple emitters (AArch64/Mach-O/PE/WASM), runtime/std modules, and a WebGPU browser playground. For most engineering tasks, start at `src/ns.c` + `Makefile`, then jump to front-end (`ns_token`/`ns_ast*`) and execution (`ns_vm_*`) or backend (`ns_ssa` + emitter files) depending on whether the task is language semantics or code generation.
---

## 12) Naming convention baseline

For the `nscode/web/` codebase, use **snake_case** consistently for:
- filenames,
- variable names, and
- function names.

When refactoring or adding new code in `nscode/web/`, prefer snake_case names and avoid introducing new camelCase/PascalCase identifiers unless required by external APIs.

## 13) NSCode memory note: runtime timing telemetry

When working in `nscode/web/src/main.js`, preserve the output-panel timing contract added for script runs:

- Every run should print timing rows for:
  - `CPU parse: ... ms`
  - `GPU parse: ... ms` (or `N/A` if GPU parser is unavailable)
  - `Execute: ... ms` (or `N/A` on parse failure)
  - `Total: ... ms`
- GPU parser status should be visible to users (unavailable / parse failure / overflow warning).
- This timing output applies to example scripts loaded from the file tree as well as arbitrary edited code, because all are executed through the same `run_code()` path.

If future refactors touch interpreter or parser integration, keep these output lines and semantics stable unless explicitly requested otherwise.
