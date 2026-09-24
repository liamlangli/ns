# ns_cpu: register bytecode interpreter

`ns_cpu` is a third way to run Nano Script, next to the AST interpreter
(`ns run`) and native code (`ns build`). It lowers the SSA module the native
backends consume (doc/ssa.md) into the instruction set of a small virtual CPU,
and a threaded interpreter runs that code. It exists for two reasons:

- **Speed without JIT.** iOS and consoles do not allow a process to create
  executable memory, so `ns build` output must be compiled ahead of time and
  the AST interpreter is the only thing left at run time. On the benchmarks
  below ns_cpu runs 27-58x faster than the AST interpreter and 1.4-3x slower
  than the native backends, while never generating machine code.
- **Hot update.** A program image (`.nsc`) is plain data. It can be built on a
  development machine, shipped over the network, verified and loaded into a
  running process to replace the code that process runs, keeping its state.

| Benchmark (`bench/`, Linux x86-64, `make NS_DEBUG=0`) | AST | ns_cpu (`.nsc`) | native |
| --- | --- | --- | --- |
| eval_arithmetic_hot | 4.97 s | 0.15 s | 0.08 s |
| eval_call_hot | 1.99 s | 0.06 s | 0.02 s |
| eval_literal_hot | 2.96 s | 0.07 s | 0.04 s |
| eval_member_hot | 5.19 s | 0.19 s | 0.14 s |
| eval_types_hot | 3.49 s | 0.06 s | 0.04 s |

## Commands

```sh
ns run --cpu [file | target]     # compile to ns_cpu code in memory and run it
ns test --cpu [path]             # run test entries on ns_cpu
ns build --cpu [path | target]   # write bin/<name>.nsc for a project or target
ns --cpu file.ns -o out.nsc      # write the image of one file (default bin/a.nsc)
ns run out.nsc                   # run an image; `ns run` detects the format
ns --cpu-dis file.ns|file.nsc    # print the code of every function
```

A project whose `ns.mod` sets `target = "emu"` (at the top level or on one
`[[targets]]` table) runs this way without the flag: `ns run` and `ns test`
use ns_cpu, `ns build` writes the image, and `ns project` embeds the image in
the generated Apple apps, whose runtime runs it on ns_cpu (see doc/nsm.md).

`ns run --cpu` behaves like `ns run`: it links the project, runs the module
globals and then `main`, and exits 0. A fault (failed `assert`, array index out
of bounds, division by zero, a bad address, a missing native symbol) prints
`ns_cpu: <reason> in fn <name>` with the source location for asserts, and exits
with status 1.

## Semantics

ns_cpu follows the AMD64 backend (src/ns_amd64.c) decision for decision. The
same SSA instruction becomes the same `ns_rt_*` runtime call or its inline
equivalent, loads and stores have the same width and signedness, integer
arithmetic is 64-bit, and floats are bit patterns in 64-bit registers (an f32 in
the low half). Heap values live in the ns_rt linear memory a native executable
uses, so strings, arrays, maps, unions, closures, tasks and scope reclamation
behave exactly as in `ns build` output. `test/ns_parity_test.sh` runs every
parity program through the interpreter, a native build and ns_cpu and requires
the three to agree.

Two consequences follow. A program that differs between `ns run` and
`ns build` differs the same way under ns_cpu, and an SSA lowering fix helps
both compiled paths at once. And where the AMD64 backend leaves the upper half
of an f32 slot unspecified, ns_cpu keeps it zero.

## Instruction set

The instruction list lives in `include/ns_cpu_isa.h` as one X-macro that the
generator, verifier, interpreter and disassembler all expand. Code is a stream
of 16-bit units: an opcode, then the operands its format string names.

- Every SSA value is a 64-bit register of its function's frame. Parameters are
  the first registers, so a call copies its arguments straight into the callee's
  frame. Constants are registers too, above the values; the loader fills them
  on frame entry, so reading a constant inside a loop costs nothing.
- A compare that only feeds the branch ending its block is fused into it
  (`BLTS a b @then @else`), and `GT`/`GE` swap operands so only the `LT`/`LE`
  forms exist, which also keeps unordered float compares false.
- A `COPY` shares its source's register. Phi inputs are copied on the edge
  that selects them, as parallel copies through scratch registers when one copy
  would overwrite another's input (a swap across a loop edge).
- Struct field access, array indexing and stores are inline instructions with
  the runtime's bounds checks. Allocation, strings, maps, pins and scope
  release call the runtime (`RT`), native modules go through `FFI`.
- Calls do not recurse on the C stack: each thread has a register stack and a
  frame stack, so recursion depth is bounded by those (1M registers, 64K
  frames), not by the thread's C stack.

The interpreter dispatches with computed goto on GCC and Clang (one indirect
jump per instruction) and a `switch` elsewhere.

## Calls into and out of native code

`ref fn` declarations of feature modules are foreign call sites. The loader
resolves each symbol through the host resolver (default: `dlopen` of
`<lib_path>/<module>.so|.dylib|.dll`, then `dlsym`) and prepares a libffi call
interface once per site. A symbol that is missing only faults when a call
reaches it, so an image still loads on a host without a module it does not use.
Without libffi (`NS_XCLIB` builds) integer and pointer signatures of up to eight
arguments are called directly; a float signature is refused at load time.

Native code calls ns code back through a code pointer it reads from a function
value: a task body (`async fn`, `dispatch`), a view callback. `FNADDR` hands out
one of 256 C thunks compiled into ns_cpu and bound to the function when the
image loads; a thunk re-enters the interpreter on whatever thread calls it,
with its own register stack on a thread that has none. No code is generated at
run time.

## Image format

Little-endian, version 1:

```
"NSCPU" 0, u16 version
u32 nstrings, then u32 length + bytes each      names, literals, file names
u32 nglobals, then (u32 name, u32 type) each    for hot update
u32 nhelpers, then u32 name each                ns_rt_* helpers, bound by name
u32 nffi, then (u32 module, u32 name, u8 ret, u8 nparams, u8 kind...) each
u32 ncache                                      indirect call cache slots
u32 nlocs, then (u32 file, u32 line) each       assert and trap locations
u32 nfn, then per function:
    u32 name, u16 nparams, u16 nregs, u16 nconst, u32 ncode
    nconst x (u8 kind, u64 value)               kind 1: string literal index
    ncode x u16                                 code
u32 main, u32 __module_init                     0xffffffff when absent
```

The loader verifies every function before anything runs: each opcode exists,
operands fit in the code, each register lies in its frame (and destinations
below the constants), each table index exists, each branch lands on an
instruction boundary, and the code cannot fall off its end. A truncated or
damaged image is rejected with a reason instead of being executed. Runtime
helpers are bound by name, so an image does not depend on the order of anything
inside the `ns` binary that runs it.

Verification is about integrity, not trust. Like native code, ns code can hold
host pointers (`ref T` from a native module), so an image can reach any memory
the process can. Load only images you built, and authenticate an image that
arrives over a network (a signature over its bytes) before loading it.

Struct layouts are resolved when the image is built, for the host that builds
it; images are portable between 64-bit little-endian hosts with the same C
layout (for example macOS arm64 and iOS arm64).

## Embedding and hot update

`include/ns_cpu.h` is the whole API. `src/ns_cpu.c` (loader and interpreter)
does not depend on the parser or the SSA builder, so a host that only runs
shipped images links it alone with the ns_rt runtime.

```c
ns_cpu_host host = {.lib_path = ns_str_cstr("lib")};
ns_return_ptr loaded = ns_cpu_load(bytes, size, &host, ns_null);
if (ns_return_is_error(loaded)) { /* rejected: keep running what you had */ }
ns_cpu_module *game = loaded.r;
ns_cpu_run_init(game);                       // module globals
ns_cpu_call(game, "frame", args, 1, &out);   // any fn, 64-bit arguments

// Later, a new image arrives:
ns_return_ptr next = ns_cpu_load(new_bytes, new_size, &host, game);
if (!ns_return_is_error(next)) {
    ns_cpu_unload(game);                     // globals were carried over
    game = next.r;
}
```

Passing the running module as `prev` copies every global the new image declares
with the same name and type, so the program keeps its state across the update;
the new image's `__module_init` is not run unless the host asks. Faults and
load failures are returned as errors, never by exiting the process, so a host
can reject a bad update and carry on.

Limits of this version:

- The heap is the process-wide ns_rt heap: an unloaded image's data stays
  allocated, and objects built by an old image keep their old layout after an
  update. A hot update that changes a struct should reset the state holding it.
- 256 functions can have their address taken at once (the thunk pool), across
  all loaded images.
- Loading binds thunks without a lock: load and unload images from one thread.
- A fault inside ns code that native code called back (a task body, a view
  callback) aborts the process, as it would in a native build: unwinding
  through the native frames in between is not safe.
