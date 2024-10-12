#include "ns_vm.h"

void ns_vm_import_std_records(ns_vm *vm) {
    // print fn
    ns_record print = (ns_record){.type = NS_RECORD_FN, .fn = {.ast = -1}};
    print.name = ns_str_cstr("print");
    print.fn.ret = (ns_type){.type = NS_TYPE_EMPTY };
    ns_array_set_length(print.fn.args, 1);
    print.fn.args[0] = (ns_record){.type = NS_RECORD_VALUE, .val = {.type = (ns_type){.type = NS_TYPE_STRING}}};
    ns_vm_push_record(vm, print);
}
