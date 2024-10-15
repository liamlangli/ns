#include "ns_vm.h"
#include "ns_fmt.h"

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
    print->fn.fn = (ns_value){.p = print_p, .type = (ns_type){.type = NS_TYPE_FN, .i = print_p }};
}

ns_value ns_vm_eval_std(ns_vm *vm) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    if (ns_str_equals_STR(call->fn->name, "print")) {
        ns_value arg = call->args[0];
        ns_str s = ns_fmt_eval(vm, vm->str_list[arg.p]);
        ns_str_printf(s);
        ns_str_free(s);
    }

    return ns_nil;
}