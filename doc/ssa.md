# SSA Module Design

## Goal
Lower Nano Script AST (`ns_ast_ctx`) into a control-flow graph of SSA-style blocks and instructions.

## Public API
- `ns_return_ptr ns_ssa_build(ns_ast_ctx *ctx)`
- `void ns_ssa_print(ns_ssa_module *m)`
- `void ns_ssa_module_free(ns_ssa_module *m)`

Defined in:
- `/Users/lang/os/ns/include/ns_ssa.h`
- `/Users/lang/os/ns/src/ns_ssa.c`

## AArch64 Backend
- A new backend module lowers SSA to AArch64 instruction bytes:
  - `/Users/lang/os/ns/include/ns_aarch.h`
  - `/Users/lang/os/ns/src/ns_aarch.c`
- Current support:
  - `PARAM`, `CONST` (u16 decimal), `COPY`, `ADD`, `SUB`, `RET`
  - unsupported ops emit `NOP` with warning
- CLI:
  - `ns --aarch <file.ns>`

## IR Shape
- `ns_ssa_module`: list of functions.
- `ns_ssa_fn`: SSA function with:
  - `blocks`: CFG blocks (`preds`, `succs`, `terminated`).
  - `insts`: flat instruction storage.
- `ns_ssa_block`: list of instruction indices.
- `ns_ssa_inst`: opcode + operands + optional branch targets + debug links (`ast`).

## Lowering Strategy
- Build one SSA function per:
  - `NS_AST_FN_DEF`
  - `NS_AST_OP_FN_DEF`
- Build synthetic `__module_init` function for non-function top-level sections.
- Lower expressions to SSA values (`n0`, `n1`, ...).
- Lower statements into block-local instructions and branch terminators.
- Track current variable version via local environment map (`name -> SSA value`).

## Print Format
- Value definition pattern: `n<id> = ...`
- Branch/jump fields are named:
  - `BR cond=nX then=bY else=bZ`
  - `JMP target=bY`
- Common named operands:
  - binary ops: `lhs=nX rhs=nY`
  - call: `callee=nX arg_count=k`
  - arg: `value=nX`
  - ret/assert: `value=nX` (or `void`)

## Control Flow
- `if`: emits `BR` + `then`/`else`/`merge` blocks; emits `PHI` when both branches produce different versions.
- `loop`: emits `cond`/`body`/`exit` blocks with back-edge.
- `for` (range): emits `cond`/`body`/`step`/`exit` blocks.
- `return`: emits `RET`.

## Notes
- This is a first-pass SSA builder focused on CFG shape and value versioning.
- Rich type propagation is currently conservative (`ns_type_unknown` for most ops).
- Loop-carried PHI placement is not fully implemented yet.
