#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_path.h"

#include <dlfcn.h>
// #include <ffi.h>

ns_bool ns_vm_call_std(ns_vm *vm) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    if (ns_str_equals_STR(call->fn->name, "print")) {
        ns_value arg = vm->symbol_stack[call->arg_offset].val;
        ns_str s = ns_fmt_eval(vm, vm->str_list[arg.o]);
        ns_str_printf(s);
        ns_str_free(s);
        call->ret = ns_nil;
    } else if (ns_str_equals_STR(call->fn->name, "open")) {
        ns_str path = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        ns_str mode = ns_eval_str(vm, vm->symbol_stack[call->arg_offset + 1].val);
        u64 fd = (u64)fopen(path.data, mode.data);
        call->ret = (ns_value){.t = ns_type_u64, .o = fd};
    } else if (ns_str_equals_STR(call->fn->name, "write")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        ns_value data = vm->symbol_stack[call->arg_offset + 1].val;
        ns_str s = ns_eval_str(vm, data);
        ns_str ss = ns_str_unescape(s);
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        i32 len = fwrite(ss.data, ss.len, 1, f);
        call->ret = (ns_value){.t = ns_type_u64, .o = len};
    } else if (ns_str_equals_STR(call->fn->name, "read")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        ns_value size = vm->symbol_stack[call->arg_offset + 1].val;
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        i32 s = ns_eval_number_i32(vm, size);
        char *buff = malloc(s);
        i32 len = fread(buff, s, 1, f);
        ns_str ret = (ns_str){.data = buff, .len = len};
        call->ret = (ns_value){.t = ns_type_str, .o = (u64)ret.data};

    } else if (ns_str_equals_STR(call->fn->name, "close")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        fclose((FILE*)fd.o);
        call->ret = ns_nil;
    } else if (ns_str_equals_STR(call->fn->name, "sqrt")) {
        ns_value x = vm->symbol_stack[call->arg_offset].val;
        call->ret = (ns_value){.t = ns_type_f64, .f64 = sqrt(x.f64)};
    } else {
        ns_error("eval error", "unknown std fn %.*s\n", call->fn->name.len, call->fn->name.data);
    }
    return false;
}

ns_lib* ns_lib_find(ns_vm *vm, ns_str lib) {
    for (i32 i = 0, l = ns_array_length(vm->libs); i < l; i++) {
        if (ns_str_equals(vm->libs[i].name, lib)) return &vm->libs[i];
    }
    return ns_null;
}

ns_lib* ns_lib_import(ns_vm *vm, ns_str lib) {
    ns_ast_ctx ctx = {0};
    ns_str path = ns_path_join(ns_str_cstr("lib"), ns_str_concat(lib, ns_str_cstr(".ns")));
    ns_str source = ns_read_file(path);
    ns_ast_parse(&ctx, source, path);

    ns_str cur_lib = vm->lib;
    vm->lib = lib;
    if (!ns_vm_parse(vm, &ctx)) {
        ns_error("vm import", "failed to import lib %.*s\n", lib.len, lib.data);
    }
    vm->lib = cur_lib;

    if (ns_str_equals(lib, ns_str_cstr("std"))) {
        ns_lib _lib = { .name = lib, .path = ns_str_null, .lib = ns_null };
        ns_array_push(vm->libs, _lib);
        return ns_array_last(vm->libs);
    } else {
        ns_str lib_path = ns_path_join(ns_str_cstr("lib"), ns_str_concat(lib, ns_lib_ext));
        void* lib_ptr = dlopen(lib_path.data, RTLD_LAZY);
        ns_lib _lib = { .name = lib, .path = lib_path, .lib = lib_ptr };
        ns_array_push(vm->libs, _lib);
        return ns_array_last(vm->libs);
    }
}

ns_bool ns_vm_call_ref(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->fn;

    if (ns_str_equals(fn->lib, ns_str_cstr("std"))) {
        return ns_vm_call_std(vm);
    }

    if (fn->fn.fn_ptr) {
        goto do_call;
    }

    ns_lib *lib = ns_lib_find(vm, fn->lib);
    if (!lib) {
       lib = ns_lib_import(vm, fn->lib);
    }

    if (!lib) {
        ns_error("vm call", "failed to find lib %.*s\n", fn->lib.len, fn->lib.data);
    }

    ns_str fn_name = ns_str_slice(fn->name, 0, fn->name.len);
    void *fn_ptr = dlsym(lib->lib, fn_name.data);
    if (!fn_ptr) {
        ns_error("vm call", "failed to find fn %.*s\n", fn->name.len, fn->name.data);
    }

    fn->fn.fn_ptr = fn_ptr;
do_call:


    return true;
}