# SSA Module Design

Lower Nano Script AST (`ns_ast_ctx`) into a CFG of SSA-style blocks and
instructions. Native AArch64 (and Wasm) consume this IR.

## Public API

- `ns_return_ptr ns_ssa_build(ns_ast_ctx *ctx)`
- `ns_return_ptr ns_ssa_build_with_runtime_paths(...)`
- `void ns_ssa_print(ns_ssa_module *m)`
- `void ns_ssa_module_free(ns_ssa_module *m)`
- `i32 ns_ssa_phi_incoming(ns_ssa_inst *phi, i32 pred)`
- `void ns_ssa_phi_set_incoming(ns_ssa_inst *phi, i32 pred, i32 value)`

Defined in `include/ns_ssa.h` and `src/ns_ssa.c`.

## Pipeline (Darwin AArch64)

```
AST → SSA → AArch64 bytes → Mach-O object → clang + ns_native_rt.c + strtab → executable
```

`ns build` / `ns build --exe` is this path. `ns run` is the interpreter and is
the semantic spec; compiled programs must match it.

## IR Shape

- `ns_ssa_module`: functions, imports, globals, shaders, owned strings.
- `ns_ssa_fn`: `blocks`, flat `insts`, params, return type.
- `ns_ssa_inst`: opcode, `dst`/`a`/`b`/`c`, branch targets, type, name, module.
- Phi nodes may carry `phi_edges` (pred, value) for more than two predecessors.
  `a`/`b`/`target0`/`target1` stay in sync for the first two edges (Wasm).

## Lowering

One SSA function per `NS_AST_FN_DEF` and `NS_AST_OP_FN_DEF`. A synthetic
`__module_init` holds top-level statements and seeds globals. Locals are
versioned in an environment map (`name → value`).

`&&` / `||` on bools lower to `BR` + a merge `PHI` (short-circuit). Arithmetic
`AND`/`OR` remain bitwise. `do { } loop cond` jumps to the body first.
`for`-range continue increments the iterator and records a phi edge from the
continue block. `for v in` over arrays, strings, sets, dicts, and iterator
structs (`next(it): bool` plus a `value` field) lowers to a counted or
protocol-driven loop. Array element writes through `v` store back; `next`
receives the subject without cloning so field updates persist.

`fn ops(+)` is emitted as `L_add_R` (see `ns_ops_override_name`). Several
`fn next` / `fn to_str` overloads are emitted as `next$Type` / `to_str$Type`.
`point(0, 0)` is a positional constructor (ALLOC + field STORE), not a call.

## AArch64 ABI

Internal ns→ns calls:

- Values live in 8-byte stack slots, addressed from `x29` once stack args exist.
- Arguments 0–7 in `x0–x7` (floats as bit patterns). Argument 8+ on the stack
  at `[sp, #8*k]` (AAPCS64), `sp` 16-byte aligned. Extra args are not an error.
- Return in `x0`.
- `std.*` maps to `ns_rt_*` wrappers so libm/file I/O stay bit-pattern safe.

External `ref fn` (later): real AAPCS64, including `d0–d7` for floats, and
dylibs linked from `ssa->imports`.

Heap addresses are 32-bit offsets into `ns_rt` linear memory (Wasm32 layout):
array `{ptr,u32; len,u32; cap,u32}`, string `{bytes,u32; len,u32}`.

## Opcode coverage (AArch64)

Integer/bool/enum arithmetic and compares, shifts (signed `ASRV` / unsigned
`LSRV`), casts (int↔float, f32↔f64), control flow, globals, alloc/clone,
load/store (struct memcpy), arrays, strings (intern, concat, compare, index),
`std` math/file/string helpers, float negate/add/sub/mul/div/mod (`fmod`).

Function values are heap objects `{code_ptr, captures...}`. A `{ ... }`
block becomes `$bN(env, args...)`; a named `fn` used as a value gets a
`$vName` trampoline that ignores `env`. Indirect calls pass the object as
the first argument and `BLR` the code pointer. `ADRP`+`ADD` materializes
addresses (no absolute text relocations).

Still lowering or ABI work: `ref`/`union`, `async`/`task`, full `ref fn`
FFI. Wasm continues to reject those types.
