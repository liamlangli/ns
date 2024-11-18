#include "ns_vm.h"
#include "ns_fmt.h"

ns_value ns_vm_eval_std(ns_vm *vm) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    if (ns_str_equals_STR(call->fn->name, "print")) {
        ns_value arg = vm->symbol_stack[call->arg_offset].val;
        ns_str s = ns_fmt_eval(vm, vm->str_list[arg.o]);
        ns_str_printf(s);
        ns_str_free(s);
        return call->ret = ns_nil;
    } else if (ns_str_equals_STR(call->fn->name, "open")) {
        ns_str path = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        ns_str mode = ns_eval_str(vm, vm->symbol_stack[call->arg_offset + 1].val);
        u64 fd = (u64)fopen(path.data, mode.data);
        return call->ret = (ns_value){.t = ns_type_u64, .o = fd};
    } else if (ns_str_equals_STR(call->fn->name, "write")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        ns_value data = vm->symbol_stack[call->arg_offset + 1].val;
        ns_str s = ns_eval_str(vm, data);
        ns_str ss = ns_str_unescape(s);
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        i32 len = fwrite(ss.data, ss.len, 1, f);
        return call->ret = (ns_value){.t = ns_type_u64, .o = len};
    } else if (ns_str_equals_STR(call->fn->name, "read")) {
        // ns_value fd = call->args[0];
        // ns_value size = call->args[1];
        // char *buf = malloc(size.o);
        // i32 len = read(fd.o, buf, size.o);
        // ns_str s = ns_str_new(buf, len);
        // free(buf);
        // return (ns_value){.t = ns_type_str, .o = ns_array_push(vm->str_list, s)};
        return ns_nil;
    } else if (ns_str_equals_STR(call->fn->name, "close")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        fclose((FILE*)fd.o);
        return call->ret = ns_nil;
    } else if (ns_str_equals_STR(call->fn->name, "sqrt")) {
        ns_value x = vm->symbol_stack[call->arg_offset].val;
        return call->ret = (ns_value){.t = ns_type_f64, .f64 = sqrt(x.f64)};
    }

    ns_error("eval error", "unknown std fn %.*s\n", call->fn->name.len, call->fn->name.data);
    return ns_nil;
}