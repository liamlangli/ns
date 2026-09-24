#pragma once

#include "ns_type.h"

/*
 * ns_cpu: a register-machine bytecode for Nano Script and the interpreter that
 * runs it (doc/cpu.md).
 *
 * The AST evaluator walks the tree for every expression it runs. ns_cpu instead
 * lowers the SSA module the native backends consume into the instruction set of
 * a small virtual CPU: every SSA value becomes a register of its function's
 * frame, operands are register numbers, branches are code offsets, and the
 * compare feeding a branch is fused into it. The interpreter runs that code with
 * one dispatch per instruction, calls the same ns_rt_* runtime a native build
 * links, and so matches the semantics of `ns build` output without generating
 * machine code at run time. That keeps it usable where executable memory is not
 * available (iOS, consoles), and a program image is plain data: it can be
 * written to disk, shipped, verified and loaded into a running process to
 * replace the code it runs (hot update).
 *
 * An image is produced by ns_cpu_image_from_ssa (src/ns_cpu_gen.c) and loaded by
 * ns_cpu_load (src/ns_cpu.c), which verifies every instruction before anything
 * runs. The loader and interpreter do not depend on the parser or the SSA
 * builder, so a host that only runs shipped images links src/ns_cpu.c alone.
 */

#define NS_CPU_IMAGE_MAGIC "NSCPU"
#define NS_CPU_IMAGE_VERSION 1

typedef struct ns_cpu_module ns_cpu_module;

// Resolve the native symbol a `ref fn` of `module` names. `user` is the
// pointer given with the resolver. Return NULL when the symbol is unknown.
typedef void *(*ns_cpu_resolve_fn)(void *user, const char *module, const char *name);

typedef struct ns_cpu_host {
    ns_cpu_resolve_fn resolve;
    void *user;
    // Directories searched for `<module>.so|.dylib|.dll` by the default
    // resolver, used when `resolve` is NULL.
    ns_str lib_path;
    ns_str lib_fallback_path;
} ns_cpu_host;

// Lower an SSA module (ns_ssa_module *) into a serialized image. The result is
// an ns_array of bytes; free it with ns_array_free.
ns_return_ptr ns_cpu_image_from_ssa(void *ssa);

// Verify and link an image. `host` may be NULL. With `prev`, globals whose name
// and kind match are carried over from the module being replaced, so a hot
// update keeps the program state it had.
ns_return_ptr ns_cpu_load(const u8 *data, szt size, const ns_cpu_host *host, ns_cpu_module *prev);
void ns_cpu_unload(ns_cpu_module *m);

// Run `__module_init` (when present) and then `main`. `status` receives main's
// result. Returns an error when the program faults.
ns_return_bool ns_cpu_run_main(ns_cpu_module *m, i64 *status);
ns_return_bool ns_cpu_run_init(ns_cpu_module *m);

// Call a function of the module by name with 64-bit arguments (floats as their
// bit pattern). Returns an error when the function is missing or faults.
ns_return_bool ns_cpu_call(ns_cpu_module *m, const char *name, const i64 *args, i32 nargs, i64 *result);

// Print the instructions of every function.
void ns_cpu_disasm(ns_cpu_module *m);

// Read and write whole image files.
ns_return_bool ns_cpu_image_write(ns_str path, const u8 *data, szt size);
ns_bool ns_cpu_image_is(const u8 *data, szt size);
