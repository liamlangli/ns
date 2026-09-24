# SSA Module Design

Lower Nano Script AST (`ns_ast_ctx`) into a CFG of SSA-style blocks and
instructions. Native AArch64, native AMD64, Wasm, and the ns_cpu bytecode
interpreter (doc/cpu.md) consume this IR.

## Public API

- `ns_return_ptr ns_ssa_build(ns_ast_ctx *ctx)`
- `ns_return_ptr ns_ssa_build_with_runtime_paths(...)`
- `ns_return_ptr ns_ssa_build_with_runtime_paths_options(..., wasm_target)`
- `void ns_ssa_print(ns_ssa_module *m)`
- `void ns_ssa_module_free(ns_ssa_module *m)`
- `i32 ns_ssa_phi_incoming(ns_ssa_inst *phi, i32 pred)`
- `void ns_ssa_phi_set_incoming(ns_ssa_inst *phi, i32 pred, i32 value)`

Defined in `include/ns_ssa.h` and `src/ns_ssa.c`.

## Pipeline (native)

```
Darwin: AST → SSA → AArch64 bytes → Mach-O object → clang + ns_native_rt.c + strtab → executable
Linux:  AST → SSA → AMD64 bytes   → ELF object    → cc    + ns_native_rt.c + strtab → executable
```

Both hosts end in the system toolchain, which links the emitted object against
the native runtime, the generated string table, and one shared library per
imported feature module. Only the object format and the instruction encoder
differ; every lowering decision above them is shared.

`ns build` / `ns build --exe` is this path. `ns run` is the interpreter and is
the semantic spec; compiled programs must match it.

Native lowering leaves `wasm_target` false: direct shader transpilation
calls still fold to the host shader language, but the SSA module does not also
transpile every shader to WGSL and attach Wasm-only metadata. Wasm lowering
sets it true, which additionally lays out the structs a lib module declares in
the compact wasm32 layout instead of the C layout of the host running the
compiler: nothing allocates those structs natively in the browser, so the
browser middleware and the emitted module have to agree on one host-independent
layout.

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

`fn ops(+)` is emitted as `L_add_R` (see `ns_ops_override_name`). A later
`fn` with the same name is registered as `name_FirstArgType` (for example
`length_float2`); call lookup tries the bare name, then the mangled label.
`fn next` / `fn to_str` use the same rule.
`point(0, 0)` is a positional constructor (ALLOC + field STORE), not a call.

## AArch64 ABI

Internal ns→ns calls:

- Values live in 8-byte stack slots, addressed from `x29` once stack args exist.
- Arguments 0–7 in `x0–x7` (floats as bit patterns). Argument 8+ on the stack
  at `[sp, #8*k]` (AAPCS64), `sp` 16-byte aligned. Extra args are not an error.
- Return in `x0`.
- `std.*` maps to `ns_rt_*` wrappers so libm/file I/O stay bit-pattern safe.

External `ref fn` uses real AAPCS64 (`x0–x7` plus `d0–d7` for floats, stack
after that). `str` arguments become C `char*` via `ns_rt_to_cstr`; string
returns are wrapped with `ns_rt_from_cstr`. Darwin `ns build` links each
imported native module's `.dylib` from the runtime `lib`/`bin` directory.

Language `ref` boxes a scalar local on first take (`ALLOC` + `STORE`) and
rebinds the name so later reads `LOAD` and writes `STORE` through the box.
`ref` of an already-ref value is identity. Heap values (structs, arrays,
strings) keep their pointer and only set the ref bit.

Unions are heap boxes `{i32 tag, i64 payload}`. Assigning or `as` to a union
calls `ns_rt_union_new`; narrowing `u as T` calls `ns_rt_union_as` (numeric
members convert).

`async fn` calls lower to `ns_rt_task_spawn` (worker thread). `await` is
`ns_rt_task_await`. The `task` module maps onto `ns_rt_task_*` /
`ns_rt_queue_*`: `queue_main` runs inline, `queue_worker`/`queue_idle` start
a pthread. `cancel` is cooperative at `sleep`/`await`.

Heap addresses are 32-bit offsets into `ns_rt` linear memory (Wasm32 layout):
array `{ptr,u32; len,u32; cap,u32}`, string `{bytes,u32; len,u32}`.

## AMD64 ABI (System V)

Internal ns→ns calls mirror the AArch64 rules with the registers this ABI has:

- Values live in 8-byte stack slots addressed from `rbp`; slot `v` is at
  `[rbp - 8*(v+1)]`, so outgoing stack arguments may move `rsp` freely.
- Arguments 0–5 in `rdi, rsi, rdx, rcx, r8, r9` (floats as bit patterns).
  Argument 6+ at `[rsp + 8*k]`, `rsp` 16-byte aligned at every call.
- Return in `rax`. `rax` and `r11` are the operand scratch pair, `rcx` holds a
  shift count, `rdx` the division remainder, `xmm0`/`xmm1` the float operands.
- Floats are kept as bit patterns in the integer slots and move into SSE with
  `movq` for arithmetic, so an f32 occupies the low half of its slot.
- `std.*` maps to `ns_rt_*` wrappers, as on AArch64.

External `ref fn` uses the real System V convention: integer and pointer
arguments in the six registers above, floats in `xmm0–xmm7`, the rest on the
stack, and `al` set to the number of vector registers used so a variadic callee
reads it. `str` arguments become C `char*` via `ns_rt_to_cstr` and string
returns are wrapped with `ns_rt_from_cstr`. Linux `ns build` links each imported
native module's `.so` from the runtime `lib`/`bin` directory and records an
`$ORIGIN` rpath beside it.

A Windows PE build reuses the same encoder with the Microsoft x64 registers
(`rcx, rdx, r8, r9`, aliased positionally with `xmm0–xmm3`) and the 32 bytes of
shadow space that ABI reserves below the outgoing arguments.

The ELF object (`src/ns_elf.c`) holds one `.text` section with every function,
a `.rela.text` naming the runtime helpers and FFI symbols, and a symbol table
that exports each function. Calls inside the module are resolved at emit time,
so only what the linker must supply carries a relocation: `R_X86_64_PLT32` for
a call, `R_X86_64_PC32` for the `lea` that materializes a function address.

## Opcode coverage (AArch64 and AMD64)

Integer/bool/enum arithmetic and compares, shifts (signed `ASRV` / unsigned
`LSRV`), casts (int↔float, f32↔f64), control flow, globals, alloc/clone,
load/store (struct memcpy), arrays, strings (intern, concat, compare, index),
`std` math/file/string helpers, float negate/add/sub/mul/div/mod (`fmod`).

Function values are heap objects `{code_ptr, captures...}`. A `{ ... }`
block becomes `$bN(env, args...)`; a named `fn` used as a value gets a
`$vName` trampoline that ignores `env`. Indirect calls pass the object as
the first argument and `BLR` the code pointer. `ADRP`+`ADD` materializes
addresses (no absolute text relocations).

Both encoders cover the same opcodes; where AArch64 uses `ADRP`+`ADD` for an
address, AMD64 uses a RIP-relative `lea`, and where it uses `BLR`, AMD64 calls
through the slot the callee address was parked in.

`v.on_frame({ ... })` stores an `ns_rt_callback` trampoline into the native
function-pointer slot. Field access on a `ref` struct uses the VM's C layout
(`field.o` / `field.s`), not the Wasm32 heap layout. Wasm continues to reject
`ref` / `union` / `async` / `task`.
