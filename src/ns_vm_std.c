#include "ns_vm.h"
#include "ns_fmt.h"

#define ns_type_ref_fn(i) ns_type_encode(NS_TYPE_FN, i, 1, NS_STORE_CONST)

void ns_vm_import_std_symbols(ns_vm *vm) {
    ns_str std = ns_str_cstr("std");

    // fn print(fmt: str): nil
    i32 print_p = ns_vm_push_symbol(vm, (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = ns_ast_nil }, .parsed = true});
    ns_symbol* print= &vm->symbols[print_p];
    print->name = ns_str_cstr("print");
    print->fn.ret = ns_type_void;
    ns_array_set_length(print->fn.args, 1);
    print->fn.args[0] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_str}};
    print->lib = std;
    print->fn.fn = (ns_value){.t = ns_type_ref_fn(print_p)};

    // fn open(path: str, mode: str): i32
    i32 open_p = ns_vm_push_symbol(vm, (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = ns_ast_nil }, .parsed = true});
    ns_symbol *open = &vm->symbols[open_p];
    open->name = ns_str_cstr("open");
    open->fn.ret = ns_type_i32;
    ns_array_set_length(open->fn.args, 2);
    open->fn.args[0] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_str}};
    open->fn.args[1] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_str}};
    open->lib = std;
    open->fn.fn = (ns_value){.t = ns_type_ref_fn(open_p)};

    // fn write(fd: i32, data: str): i32
    i32 write_p = ns_vm_push_symbol(vm, (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = ns_ast_nil }, .parsed = true});
    ns_symbol *write = &vm->symbols[write_p];
    write->name = ns_str_cstr("write");
    write->fn.ret = ns_type_i32;
    ns_array_set_length(write->fn.args, 2);
    write->fn.args[0] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_i32}};
    write->fn.args[1] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_str}};
    write->lib = std;
    write->fn.fn = (ns_value){.t = ns_type_ref_fn(write_p)};

    // fn read(fd: i32, size: i32): str
    i32 read_p = ns_vm_push_symbol(vm, (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = ns_ast_nil }, .parsed = true});
    ns_symbol *read = &vm->symbols[read_p];
    read->name = ns_str_cstr("read");
    read->fn.ret = ns_type_str;
    ns_array_set_length(read->fn.args, 2);
    read->fn.args[0] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_i32}};
    read->fn.args[1] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_i32}};
    read->lib = std;
    read->fn.fn = (ns_value){.t = ns_type_ref_fn(read_p)};

    // fn close(fd: i32): nil
    i32 close_p = ns_vm_push_symbol(vm, (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = ns_ast_nil }, .parsed = true});
    ns_symbol *close = &vm->symbols[close_p];
    close->name = ns_str_cstr("close");
    close->fn.ret = ns_type_void;
    ns_array_set_length(close->fn.args, 1);
    close->fn.args[0] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_i32}};
    close->lib = std;
    close->fn.fn = (ns_value){.t = ns_type_ref_fn(close_p)};

    // fn sqrt(x: f64): f64
    i32 sqrt_p = ns_vm_push_symbol(vm, (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = ns_ast_nil }, .parsed = true});
    ns_symbol *sqrt = &vm->symbols[sqrt_p];
    sqrt->name = ns_str_cstr("sqrt");
    sqrt->fn.ret = ns_type_f64;
    ns_array_set_length(sqrt->fn.args, 1);
    sqrt->fn.args[0] = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = {.type = ns_type_f64}};
    sqrt->lib = std;
    sqrt->fn.fn = (ns_value){.t = ns_type_ref_fn(sqrt_p)};
}

ns_value ns_vm_eval_std(ns_vm *vm) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    if (ns_str_equals_STR(call->fn->name, "print")) {
        ns_value arg = call->args[0];
        ns_str s = ns_fmt_eval(vm, vm->str_list[arg.o]);
        ns_str_printf(s);
        ns_str_free(s);
        return ns_nil;
    }

    ns_error("eval error", "unknown std fn %.*s\n", call->fn->name.len, call->fn->name.data);
    return ns_nil;
}