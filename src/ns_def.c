#include "ns_def.h"

void ns_vm_def_fn(ns_vm *vm, ns_fn_def *fn) {
    ns_value fn_val = {.t = ns_type_encode(ns_type_fn, (u32)ns_array_length(vm->symbols), true, 0)};
    ns_fn_symbol fn_sym = {.fn_ptr = fn->fn, .args = nil, .ret = fn->ret, .ast = 0, .fn = fn_val};
    ns_array_set_length(fn_sym.args, fn->arg_count);
    for (i32 i = 0; i < fn->arg_count; ++i) {
        fn_sym.args[i].name = fn->args[i].name;
        fn_sym.args[i].val.t = fn->args[i].type;
    }
    ns_symbol sym = {.name = fn->name, .type = NS_SYMBOL_FN, .parsed = true, .fn = fn_sym, .lib = fn->lib};
    ns_array_push(vm->symbols, sym);
}

void ns_vm_def_struct(ns_vm *vm, ns_struct_def *st) {
    ns_value st_val = {.t = ns_type_encode(ns_type_struct, (u32)ns_array_length(vm->symbols), true, 0)};
    ns_struct_symbol st_sym = {.st = st_val, .fields = nil, .stride = 0, .ast = 0};
    u64 stride = 0;
    ns_array_set_length(st_sym.fields, st->field_count);
    for (i32 i = 0; i < st->field_count; ++i) {
        ns_struct_field *f = &st_sym.fields[i];
        f->name = st->fields[i].name;
        f->t = st->fields[i].type;
        f->o = stride;
        f->s = ns_type_size(vm, f->t);
        stride += f->s;
    }
    st_sym.stride = stride;
    ns_symbol sym = {.name = st->name, .type = NS_SYMBOL_STRUCT, .parsed = true, .st = st_sym, .lib = st->lib};
    ns_array_push(vm->symbols, sym);
}