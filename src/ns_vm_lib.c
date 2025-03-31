#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_os.h"

#include <dlfcn.h>
#ifndef NS_DARWIN
    #include <ffi.h>
#else
    #include <ffi/ffi.h>
#endif

#ifdef NS_DEBUG
    #define NS_LIB_PATH "bin"
    #define NS_REF_PATH "lib"
#else
    #define NS_LIB_PATH ".cache/ns/lib"
    #define NS_REF_PATH ".cache/ns/ref"
#endif // NS_DEBUG

#define NS_MAX_FFI_ARGS 32
#define NS_MAX_FFI_STACK 256

typedef struct ffi_ctx {
    ffi_type type_refs[NS_MAX_FFI_ARGS];
    ffi_type *types[NS_MAX_FFI_ARGS];
    void *refs[NS_MAX_FFI_ARGS];
    void *values[NS_MAX_FFI_ARGS];

    i8 stack[NS_MAX_FFI_STACK];
    u64 stack_offset;
} ffi_ctx;

static ffi_ctx _ffi_ctx = {0};

ns_return_bool ns_vm_call_std(ns_vm *vm) {
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
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "unknown std fn.");
    }
    return ns_return_ok(bool, true);
}

ns_lib* ns_lib_find(ns_vm *vm, ns_str lib) {
    for (i32 i = 0, l = ns_array_length(vm->libs); i < l; i++) {
        if (ns_str_equals(vm->libs[i].name, lib)) return &vm->libs[i];
    }
    return ns_null;
}

ns_lib* ns_lib_import(ns_vm *vm, ns_str lib_name) {
    ns_ast_ctx ctx = {0};
#ifdef NS_DEBUG
    ns_str ref_path = ns_str_cstr(NS_REF_PATH);
#else
    ns_str home = ns_path_home();
    ns_str ref_path = ns_path_join(home, ns_str_cstr(NS_REF_PATH));
#endif

    ns_lib *lib = ns_lib_find(vm, lib_name);
    if (lib) return lib;

    ns_str path = ns_path_join(ref_path, ns_str_concat(lib_name, ns_str_cstr(".ns")));
    ns_str source = ns_fs_read_file(path);
    ns_ast_parse(&ctx, source, path);

    ns_str prev = vm->lib;
    vm->lib = lib_name;

    ns_return_bool ret = ns_vm_parse(vm, &ctx);
    if (ns_return_is_error(ret)) {
        ns_error("vm import", "failed to parse lib %.*s\n", lib_name.len, lib_name.data);
    }
    vm->lib = prev;

    if (ns_str_equals(lib_name, ns_str_cstr("std"))) {
        ns_lib _lib = { .name = lib_name, .path = ns_str_null, .lib = ns_null };
        ns_array_push(vm->libs, _lib);
        return ns_array_last(vm->libs);
    } else {
#ifdef NS_DEBUG
        ns_str lib_path = ns_str_cstr(NS_LIB_PATH);
#else
        ns_str lib_path = ns_path_join(home, ns_str_cstr(NS_LIB_PATH));
#endif // NS_DEBUG
        ns_str lib_link_path = ns_path_join(lib_path, ns_str_concat(lib_name, ns_lib_ext));
        void* lib_ptr = dlopen(lib_link_path.data, RTLD_LAZY);
        ns_lib _lib = { .name = lib_name, .path = lib_link_path, .lib = lib_ptr };
        ns_array_push(vm->libs, _lib);
        return ns_array_last(vm->libs);
    }
}

ffi_type ns_ffi_map_type(ns_type t) {
    switch (t.type)
    {
        case NS_TYPE_I8: return ffi_type_sint8;
        case NS_TYPE_I16: return ffi_type_sint16; break;
        case NS_TYPE_BOOL:
        case NS_TYPE_I32: return ffi_type_sint32; break;
        case NS_TYPE_I64: return ffi_type_sint64; break;
        case NS_TYPE_U8: return ffi_type_uint8; break;
        case NS_TYPE_U16: return ffi_type_uint16;
        case NS_TYPE_U32: return ffi_type_uint32;
        case NS_TYPE_U64: return ffi_type_uint64;
        case NS_TYPE_F32: return ffi_type_float;
        case NS_TYPE_F64: return ffi_type_double;
        case NS_TYPE_STRING: return ffi_type_pointer;
        case NS_TYPE_STRUCT: return ffi_type_pointer;
        case NS_TYPE_INFER:
        case NS_TYPE_VOID: return ffi_type_void;
        default:
            ns_error("ffi error", "unknown type %s\n", ns_fmt_type_str(t).data);
        break;
    }
    return ffi_type_void;
}

ns_return_bool ns_vm_call_ffi(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->fn;

    ffi_cif cif;

    if (call->arg_count > NS_MAX_FFI_ARGS) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "too many args.");
    }

    // copy args to ffi values
    ns_enter_scope(vm);
    for (i32 i = 0; i < call->arg_count; i++) {
        ns_value v = vm->symbol_stack[call->arg_offset + i].val;

        if (ns_type_is_const(v.t)) {
            i32 size = ns_type_size(vm, v.t);
            u64 offset = ns_eval_alloc(vm, size);
            ns_value dst = (ns_value){.t = ns_type_set_store(v.t, NS_STORE_STACK), .o = offset};
            ns_return_value ret_v = ns_eval_copy(vm, dst, v, size);
            if (ns_return_is_error(ret_v)) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "failed to copy arg.");
            v = ret_v.r;
        }

        switch (v.t.type)
        {
        case NS_TYPE_STRING: {
            ns_str s = ns_eval_str(vm, v);
            _ffi_ctx.refs[i] = (void*)s.data;
            _ffi_ctx.values[i] = &_ffi_ctx.refs[i];
        } break;
        case NS_TYPE_ARRAY: {
            void *data = ns_eval_array_raw(vm, v);
            _ffi_ctx.refs[i] = data;
            _ffi_ctx.values[i] = &_ffi_ctx.refs[i];
        } break;
        default: {
            u64 offset = (u64)ns_type_size(vm, v.t);
            u64 start = (_ffi_ctx.stack_offset + 7) & ~7; // Align to 8 bytes
            u64 end = start + offset; 
            if (end > NS_MAX_FFI_STACK) {
                return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "ffi stack overflow.");
            }
            i8* dst = (i8*)_ffi_ctx.stack + start;
            memcpy(dst, (i8*)vm->stack + v.o, offset);
            _ffi_ctx.stack_offset = end;
            _ffi_ctx.values[i] = dst;
        }break;
        }
        _ffi_ctx.type_refs[i] = ns_ffi_map_type(v.t);
        _ffi_ctx.types[i] = &_ffi_ctx.type_refs[i];
    }
    ns_exit_scope(vm);

    ffi_type ret_type = ns_ffi_map_type(fn->fn.ret);
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, call->arg_count, &ret_type, _ffi_ctx.types);
    if (status != FFI_OK) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "failed to prep ffi call.");
    }

    ffi_arg ret_ptr;
    ffi_call(&cif, FFI_FN(fn->fn.fn_ptr), &ret_ptr, _ffi_ctx.values);

    if (!ns_type_is(fn->fn.ret, NS_TYPE_VOID)) {
        ns_type ret_type = fn->fn.ret;
        if (ns_type_is_const(ret_type)) {
            i32 size = ns_type_size(vm, ret_type);
            u64 offset = ns_eval_alloc(vm, size);
            ns_value dst = (ns_value){.t = ns_type_set_store(ret_type, NS_STORE_STACK), .o = offset};
            ns_return_value ret_v = ns_eval_copy(vm, dst, (ns_value){.t = ret_type, .o = ret_ptr}, size);
            if (ns_return_is_error(ret_v)) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "failed to copy ret.");
            call->ret = ret_v.r;
        } else if (ns_type_is_ref(ret_type)) {
            call->ret = (ns_value){.t = ret_type, .o = ret_ptr};
        } else {
            return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "invalid ret type.");
        }
    }
    return ns_return_ok(bool, true);
}

ns_return_bool ns_vm_call_ref(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->fn;

    if (ns_str_equals(fn->lib, ns_str_cstr("std"))) return ns_vm_call_std(vm);
    if (fn->fn.fn_ptr)  return ns_vm_call_ffi(vm);

    ns_lib *lib = ns_lib_find(vm, fn->lib);
    if (!lib) lib = ns_lib_import(vm, fn->lib);
    if (!lib) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "ref lib not found.");

    ns_str fn_name = ns_str_slice(fn->name, 0, fn->name.len);
    void *fn_ptr = dlsym(lib->lib, fn_name.data);
    if (!fn_ptr) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "ref fn not found.");

    fn->fn.fn_ptr = fn_ptr;
    return ns_vm_call_ffi(vm);
}