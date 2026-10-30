#include "ns_def.h"

void ns_vm_def_fn(ns_vm *vm, ns_fn_def *fn) {
    ns_value fn_val = {.t = ns_type_encode(NS_TYPE_FN, (u32)ns_array_length(vm->symbols), true, false, true)};
    ns_fn_symbol fn_sym = {.fn_ptr = fn->fn, .args = nil, .ret = fn->ret, .ast = 0, .fn = fn_val};
    ns_array_set_length(fn_sym.args, fn->arg_count);
    for (i32 i = 0; i < fn->arg_count; ++i) {
        fn_sym.args[i].name = fn->args[i].name;
        fn_sym.args[i].val.t = fn->args[i].type;
    }
    ns_symbol sym = {.name = fn->name, .type = NS_SYMBOL_FN, .parsed = true, .fn = fn_sym, .lib = fn->lib};
    ns_array_push(vm->symbols, sym);
}
