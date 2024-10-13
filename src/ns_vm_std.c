#include "ns_vm.h"

void ns_vm_import_std_records(ns_vm *vm) {
    ns_str std = ns_str_cstr("std");

    // print fn
    int print_p = ns_vm_push_record(vm, (ns_record){.type = NS_RECORD_FN, .fn = {.ast = -1}});
    ns_record *print = &vm->records[print_p];
    print->name = ns_str_cstr("print");
    print->fn.ret = (ns_type){.type = NS_TYPE_EMPTY };
    ns_array_set_length(print->fn.args, 1);
    print->fn.args[0] = (ns_record){.type = NS_RECORD_VALUE, .val = {.type = (ns_type){.type = NS_TYPE_STRING}}};
    print->lib = std;
    print->fn.fn = (ns_value){.p = print_p, .type = (ns_type){.type = NS_TYPE_FN, .name = print->name}};
}
