#pragma once

#ifdef NS_JIT

#include "ns_type.h"
#include "ns_os.h"
#include "ns_ast.h"
#include "ns_vm.h"

typedef struct ns_jit_fn {
    ns_symbol *symbol;
    void *text;
} ns_jit_fn;

ns_jit_fn ns_jit_compile_fn(ns_vm *vm, ns_ast_ctx *ctx, ns_symbol *symbol);

#endif // NS_JIT
