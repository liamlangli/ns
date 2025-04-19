#include "ns_vm.h"

ns_str ns_symbol_type_to_string(ns_symbol_type type) {
    switch (type) {
        ns_str_case(NS_SYMBOL_INVALID)
        ns_str_case(NS_SYMBOL_VALUE)
        ns_str_case(NS_SYMBOL_FN)
        ns_str_case(NS_SYMBOL_STRUCT)
        ns_str_case(NS_SYMBOL_BLOCK)
        ns_str_case(NS_SYMBOL_TYPE)
    default:
        ns_error("ast", "unknown symbol type %d\n", type);
        return ns_str_cstr("NS_SYMBOL_INVALID");
    }
}

void ns_vm_symbol_print(ns_vm *vm) {
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        ns_str type = ns_symbol_type_to_string(s->type);
        ns_str name = s->name;
        ns_str lib = s->lib;
        printf("%4d [type: %-20.*s mod: %5.*s] ", i, type.len, type.data, lib.len, lib.data);
        printf("%.*s", name.len, name.data);
        switch (s->type)
        {
        case NS_SYMBOL_VALUE: break;
        case NS_SYMBOL_FN: break;
        case NS_SYMBOL_STRUCT: break;
        default: break;
        }
        printf("\n");
    }
}