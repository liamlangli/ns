#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_path.h"

#include <dlfcn.h>
#include <ffi.h>

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

ffi_type ns_ffi_map_type(ns_type t) {
    switch (t.type)
    {
        case NS_TYPE_I8: return ffi_type_sint8;
        case NS_TYPE_I16: return ffi_type_sint16; break;
        case NS_TYPE_I32: return ffi_type_sint32; break;
        case NS_TYPE_I64: return ffi_type_sint64; break;
        case NS_TYPE_U8: return ffi_type_uint8; break;
        case NS_TYPE_U16: return ffi_type_uint16; break;
        case NS_TYPE_U32: return ffi_type_uint32; break;
        case NS_TYPE_U64: return ffi_type_uint64; break;
        case NS_TYPE_F32: return ffi_type_float; break;
        case NS_TYPE_F64: return ffi_type_double; break;
        case NS_TYPE_STRING: return ffi_type_pointer; break;
        case NS_TYPE_STRUCT: return ffi_type_pointer; break;
        case NS_TYPE_INFER:
        case NS_TYPE_VOID: return ffi_type_void; break;
        default:
            ns_error("ffi error", "unknown type %s\n", ns_fmt_type_str(t).data);
        break;
    }
    return ffi_type_void;
}

void ns_ffi_set_value(ns_vm* vm, ns_value v, void **values) {
    if (ns_type_is_ref(v.t)) {
        if (ns_type_in_heap(v.t)) {
            *values = (void*)v.o;
        } else {
            *values = vm->stack + v.o;
        }
    } else {
        i32 size = ns_type_size(vm, v.t);
        u64 offset = ns_eval_alloc(vm, size);
        ns_value arg = (ns_value){.o = offset};
        v = ns_eval_copy(vm, arg, v, size);
    }

    switch (v.t.type)
    {
    case NS_TYPE_STRING: {
        ns_str s = ns_eval_str(vm, v);
        *values = s.data;
    } break;
    case NS_TYPE_ARRAY: {
        void *data = ns_eval_array_raw(vm, v);
        *values = data;
    } break;
    default:
        *values = vm->stack + v.o;
        break;
    }
}

ns_bool ns_vm_call_ffi(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->fn;

    ffi_cif cif;
    static ffi_type *args = ns_null;
    static void *values = ns_null;

    ns_array_set_length(args, call->arg_count);
    ns_array_set_length(values, call->arg_count);

    for (i32 i = 0; i < call->arg_count; i++) {
        ns_value arg = vm->symbol_stack[call->arg_offset + i].val;
        args[i] = ns_ffi_map_type(arg.t);
    }

    ffi_type ret = ns_ffi_map_type(fn->fn.ret);
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, call->arg_count, &ret, &args) != FFI_OK) {
        ns_error("vm call", "failed to prep ffi\n");
    }

    ns_enter_scope(vm);
    for (i32 i = 0; i < call->arg_count; i++) {
        ns_value arg = vm->symbol_stack[call->arg_offset + i].val;
        ns_ffi_set_value(vm, arg, &values[i]);
    }
    ns_exit_scope(vm);

    void *ret_ptr = ns_null;
    ffi_call(&cif, fn->fn.fn_ptr, ret_ptr, values);

    if (ret.type != NS_TYPE_VOID) {
        // u64 ret_offset = ns_eval_alloc(vm, ns_type_size(vm, fn->fn.ret));
        ns_value ret = (ns_value){.t = ns_type_set_store(fn->fn.ret, NS_STORE_STACK), .o = (u64)ret_ptr};
        call->ret = ret;
    }
    return true;
}

ns_bool ns_vm_call_ref(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->fn;

    if (ns_str_equals(fn->lib, ns_str_cstr("std"))) {
        return ns_vm_call_std(vm);
    }

    if (fn->fn.fn_ptr) {
        return ns_vm_call_ffi(vm);
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
    return ns_vm_call_ffi(vm);
}