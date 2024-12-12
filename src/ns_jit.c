#ifdef NS_JIT
#include "ns_jit.h"

#define NS_JIT_CACHE_PATH ".cache/ns/jit"

ns_jit_fn ns_jit_compile_fn(ns_vm *vm, ns_ast_ctx *ctx, ns_symbol *symbol) {
    return (ns_jit_fn) { .symbol = symbol, .text = NULL };
}

#endif // NS_JIT
