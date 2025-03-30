#include "ns_def.h"

void ns_vm_def_fn(ns_vm *vm, ns_fn_def *fn) {
    ns_unused(vm);
    ns_unused(fn);

    ns_symbol sym = {.name = fn->name, .type = NS_SYMBOL_FN, .parsed = true};
    sym.fn.fn_ptr = fn->fn;

    
}

void ns_vm_def_struct(ns_vm *vm, ns_struct_def *st) {
    ns_unused(vm);
    ns_unused(st);
}