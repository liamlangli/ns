#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_os.h"
#include "ns_profile.h"

#ifndef NS_XCLIB
#include <dlfcn.h>
#ifndef NS_DARWIN
    #include <ffi.h>
#else
    #include <ffi/ffi.h>
#endif
#endif // NS_XCLIB

#ifdef NS_DEBUG
    #define NS_LIB_PATH "bin"
    #define NS_REF_PATH "lib"
#else
    #define NS_LIB_PATH "ns/lib"
    #define NS_REF_PATH "ns/ref"
#endif // NS_DEBUG

#ifndef NS_XCLIB
#define NS_MAX_FFI_ARGS 32
#define NS_MAX_FFI_STACK 256

typedef struct ffi_ctx {
    ffi_type type_refs[NS_MAX_FFI_ARGS];
    ffi_type *types[NS_MAX_FFI_ARGS];
    void *value_refs[NS_MAX_FFI_ARGS];
    void *values[NS_MAX_FFI_ARGS];

    i8 stack[NS_MAX_FFI_STACK];
    u64 stack_offset;
} ffi_ctx;

static ffi_ctx _ffi_ctx = {0};
#endif // NS_XCLIB

ns_return_bool ns_vm_call_std(ns_vm *vm) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    if (ns_str_equals_STR(call->callee->name, "print")) {
        ns_value arg = vm->symbol_stack[call->arg_offset].val;
        // Resolve through ns_eval_str so both immediate (str_list index in .o)
        // and stack-resident strings (index stored in a stack slot, e.g. the
        // result of string concatenation) are handled correctly.
        ns_str s = ns_fmt_eval(vm, ns_eval_str(vm, arg));
        ns_str_printf(s);
        ns_str_free(s);
        call->ret = ns_nil;
    } else if (ns_str_equals_STR(call->callee->name, "open")) {
        ns_str path = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        ns_str mode = ns_eval_str(vm, vm->symbol_stack[call->arg_offset + 1].val);
        u64 fd = (u64)fopen(path.data, mode.data);
        call->ret = (ns_value){.t = ns_type_u64, .u64 = fd};
    } else if (ns_str_equals_STR(call->callee->name, "write")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        ns_value data = vm->symbol_stack[call->arg_offset + 1].val;
        ns_str s = ns_eval_str(vm, data);
        ns_str ss = ns_str_unescape(s);
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        i32 len = fwrite(ss.data, ss.len, 1, f);
        call->ret = (ns_value){.t = ns_type_u64, .u64 = (u64)len};
    } else if (ns_str_equals_STR(call->callee->name, "read")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        ns_str ret = {.data = ns_null, .len = 0, .dynamic = true};
        i32 requested = -1;
        if (call->arg_count > 1) requested = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 1].val);
        if (requested < 0 && f) {
            long pos = ftell(f);
            if (pos >= 0 && fseek(f, 0, SEEK_END) == 0) {
                long end = ftell(f);
                if (end >= pos && fseek(f, pos, SEEK_SET) == 0) requested = (i32)(end - pos);
            } else if (pos >= 0) {
                fseek(f, pos, SEEK_SET);
            }
        }
        if (requested >= 0) {
            ns_array_set_length(ret.data, requested);
            szt n = f && requested > 0 ? fread(ret.data, 1, (szt)requested, f) : 0;
            ns_array_set_length(ret.data, n);
        } else {
            char chunk[4096];
            while (f) {
                szt n = fread(chunk, 1, sizeof(chunk), f);
                if (n == 0) break;
                for (szt j = 0; j < n; ++j) ns_array_push(ret.data, chunk[j]);
            }
        }
        ret.len = (i32)ns_array_length(ret.data);
        call->ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, ret)};
        ns_array_free(ret.data);
    } else if (ns_str_equals_STR(call->callee->name, "close")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        if (f) fclose(f);
        call->ret = ns_nil;
    } else if (ns_str_equals_STR(call->callee->name, "sqrt")) {
        ns_value x = vm->symbol_stack[call->arg_offset].val;
        call->ret = (ns_value){.t = ns_type_f64, .f64 = sqrt(x.f64)};
    } else if (ns_str_equals_STR(call->callee->name, "substr")) {
        // substr(s, start, len) -> str : a copy of the [start, start+len) slice,
        // clamped to the bounds of s. Lets ns code turn a source span into text.
        ns_str s = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        i32 start = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 1].val);
        i32 len = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 2].val);
        if (start < 0) start = 0;
        if (start > s.len) start = s.len;
        if (len < 0) len = 0;
        if (start + len > s.len) len = s.len - start;
        ns_str sub = ns_str_range(s.data + start, len);
        call->ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, sub)};
    } else if (ns_str_equals_STR(call->callee->name, "unescape")) {
        // unescape(s) -> str : resolve backslash escapes (\n, \t, ...) into the
        // bytes they denote. String literals keep their raw spelling until this
        // is applied, so this lets ns code build strings with real control chars.
        ns_str s = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        ns_str u = ns_str_unescape(s);
        call->ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, u)};
        ns_str_free(u);
    } else if (ns_str_equals_STR(call->callee->name, "utf8_len")) {
        // utf8_len(s) -> i32 : number of utf8 codepoints in s. str data is
        // utf8-encoded by default, so this is the user-visible character
        // count, while the byte count stays the storage-level length.
        ns_str s = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        call->ret = (ns_value){.t = ns_type_i32, .i32 = ns_str_utf8_len(s)};
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

    ns_lib pending = { .name = lib_name, .path = ns_str_null, .lib = ns_null };
    ns_array_push(vm->libs, pending);
    i32 lib_index = (i32)ns_array_length(vm->libs) - 1;

    ns_str path = ns_path_join(ref_path, ns_str_concat(lib_name, ns_str_cstr(".ns")));
    ns_str source = ns_fs_read_file(path);
    ns_ast_parse(&ctx, source, path);

    ns_str prev = vm->lib;
    vm->lib = lib_name;

    ns_return_bool ret = ns_vm_parse(vm, &ctx);
    ns_return_assert(ret);
    ns_return_value init_ret = ns_eval_module_globals(vm, &ctx);
    ns_return_assert(init_ret);
    vm->lib = prev;

    // std and shader are VM-internal (dispatched in the interpreter, no dylib)
    if (ns_str_equals(lib_name, ns_str_cstr("std")) || ns_str_equals(lib_name, ns_str_cstr("shader"))) {
        vm->libs[lib_index].path = ns_str_null;
        vm->libs[lib_index].lib = ns_null;
        return &vm->libs[lib_index];
    } else {
#ifndef NS_XCLIB
#ifdef NS_DEBUG
        ns_str lib_path = ns_str_cstr(NS_LIB_PATH);
#else
        ns_str lib_path = ns_path_join(home, ns_str_cstr(NS_LIB_PATH));
#endif // NS_DEBUG
        ns_str lib_link_path = ns_path_join(lib_path, ns_str_concat(lib_name, ns_lib_ext));
        dlerror();
        void* lib_ptr = dlopen(lib_link_path.data, RTLD_LAZY | RTLD_GLOBAL);
        vm->libs[lib_index].path = lib_link_path;
        vm->libs[lib_index].lib = lib_ptr;
        return &vm->libs[lib_index];
#else
        // XCFramework build: no dynamic library loading
        vm->libs[lib_index].path = ns_str_null;
        vm->libs[lib_index].lib = ns_null;
        return &vm->libs[lib_index];
#endif // NS_XCLIB
    }
}

#ifndef NS_XCLIB
ffi_type ns_ffi_map_type(ns_type t) {
    if (ns_type_is_array(t)) return ffi_type_pointer;
    if (t.ref) return ffi_type_pointer;
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
    ns_symbol *fn = call->callee;

    ffi_cif cif;

    if (call->arg_count > NS_MAX_FFI_ARGS) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "too many args.");
    }

    // Reset the scratch stack used to stage by-value scalar args. It is a
    // persistent static, so without this every call would leak its arg bytes and
    // eventually trip the "ffi stack overflow" guard below (fatal once a program
    // makes enough native calls, e.g. an interactive loop).
    _ffi_ctx.stack_offset = 0;

    // copy args to ffi values
    ns_scope_enter(vm);
    for (i32 i = 0; i < call->arg_count; i++) {
        ns_value v = vm->symbol_stack[call->arg_offset + i].val;

        if (ns_type_is_const(v.t)) {
            i32 size = ns_type_size(vm, v.t);
            u64 offset = ns_eval_alloc(vm, size);
            ns_value dst = (ns_value){.t = ns_type_set_stack(v.t, true), .o = offset};
            ns_return_value ret_v = ns_eval_copy(vm, dst, v, size);
            if (ns_return_is_error(ret_v)) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "failed to copy arg.");
            v = ret_v.r;
        }

        if (ns_type_is_ref(v.t)) {
            // ffi expects values[i] to point at the pointer being passed. The
            // referent address is either stack-relative (vm->stack + offset) or
            // an absolute address (e.g. a heap pointer returned by a previous
            // native call); stash it so values[i] points at a live pointer.
            _ffi_ctx.value_refs[i] = ns_type_in_stack(v.t) ? (void*)((i8*)vm->stack + v.o) : (void*)v.o;
            _ffi_ctx.values[i] = &_ffi_ctx.value_refs[i];
            _ffi_ctx.type_refs[i] = ns_ffi_map_type(v.t);
            _ffi_ctx.types[i] = &_ffi_ctx.type_refs[i];
            continue;
        }

        if (ns_type_is_array(v.t)) {
            void *data = ns_eval_array_raw(vm, v);
            _ffi_ctx.value_refs[i] = data;
            _ffi_ctx.values[i] = &_ffi_ctx.value_refs[i];
            _ffi_ctx.type_refs[i] = ns_ffi_map_type(v.t);
            _ffi_ctx.types[i] = &_ffi_ctx.type_refs[i];
            continue;
        }

        switch (v.t.type)
        {
            case NS_TYPE_STRING: {
                // A `str` argument is passed to native code as a char*. ffi needs
                // values[i] to point at the pointer value, so the char* must live
                // until ffi_call. Stash it in the static ffi context rather than a
                // loop-local (whose address would dangle by call time).
                ns_str s = ns_eval_str(vm, v);
                _ffi_ctx.value_refs[i] = (void*)s.data;
                _ffi_ctx.values[i] = &_ffi_ctx.value_refs[i];
            } break;
            case NS_TYPE_ARRAY: {
                void *data = ns_eval_array_raw(vm, v);
                _ffi_ctx.value_refs[i] = data;
                _ffi_ctx.values[i] = &_ffi_ctx.value_refs[i];
            } break;
            default: {
                u64 offset = (u64)ns_type_size(vm, v.t);
                u64 start = (_ffi_ctx.stack_offset + 7) & ~7; // Align to 8 bytes
                u64 end = start + offset; 
                if (end > NS_MAX_FFI_STACK) {
                    return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "ffi stack overflow.");
                }
                i8* dst = (i8*)_ffi_ctx.stack + start;
                const void *src = ns_type_in_stack(v.t) ? (const void*)((i8*)vm->stack + v.o) : (const void*)v.o;
                memcpy(dst, src, offset);
                _ffi_ctx.stack_offset = end;
                _ffi_ctx.values[i] = dst;
            }break;
        }
        _ffi_ctx.type_refs[i] = ns_ffi_map_type(v.t);
        _ffi_ctx.types[i] = &_ffi_ctx.type_refs[i];
    }
    ns_scope_exit(vm);

    ffi_type ret_type = ns_ffi_map_type(fn->fn.ret);
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, call->arg_count, &ret_type, _ffi_ctx.types);
    if (status != FFI_OK) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "failed to prep ffi call.");
    }

    ns_type t = fn->fn.ret;
    i32 size = ns_type_size(vm, t);
    // libffi widens any integer return to at least sizeof(ffi_arg) and writes
    // that many bytes through the result pointer, so the destination slot must be
    // >= sizeof(ffi_arg) even for a narrower type (e.g. i32). Reserving only
    // ns_type_size() would let the call scribble past the slot and corrupt the
    // vm stack's heap block -- fatal after a few native calls. Over-reserve here;
    // only `size` bytes are read back below.
    i32 alloc_size = size < (i32)sizeof(ffi_arg) ? (i32)sizeof(ffi_arg) : size;
    ns_value v = {.t = ns_type_set_stack(fn->fn.ret, true), .o = ns_eval_alloc(vm, alloc_size)};
    // Time the native call so `--profile` can attribute wall-clock cost per
    // foreign symbol. The clock read is skipped entirely when profiling is off.
    f64 ffi_start_ms = ns_profile.enabled ? ns_profile_now_ms() : 0.0;
    ffi_call(&cif, FFI_FN(fn->fn.fn_ptr), &vm->stack[v.o], _ffi_ctx.values);
    if (ns_profile.enabled) ns_profile_record_ffi(fn->name, fn->lib, ffi_start_ms, ns_profile_now_ms() - ffi_start_ms);
    if (ns_type_is_ref(t)) {
        // Native code returns an absolute pointer to the result. Represent it as
        // an absolute-address ref value (stack=false, .o = the pointer) so that
        // member access dereferences the heap struct directly.
        call->ret.t = ns_type_set_stack(t, false);
        call->ret.o = *(u64*)&vm->stack[v.o];
    } else if (ns_type_is(t, NS_TYPE_STRING)) {
        // Native `str` returns use a C string pointer. Copy it into the VM's
        // string table so ns code receives the usual str_list-backed handle.
        const char *s = *(const char **)&vm->stack[v.o];
        call->ret.t = ns_type_str;
        call->ret.o = ns_vm_push_string(vm, s ? ns_str_cstr((char *)s) : ns_str_null);
    } else {
        if (size > 0) ns_eval_copy(vm, call->ret, v, size);
    }
    return ns_return_ok(bool, true);
}

// ---------------------------------------------------------------------------
// Closure -> C function-pointer bridge.
//
// Native APIs (e.g. view.on_frame) store a `void(*)(view*)` and invoke it from
// their event loop. ns closures are interpreted, so we hand the native side a
// libffi closure: a real, callable address whose invocation re-enters the
// interpreter to evaluate the ns closure body. The bridge is allocated for the
// process lifetime (callbacks may fire for as long as the window is open).
typedef struct ns_callback_bridge {
    ns_vm *vm;
    ns_ast_ctx *ctx;
    ns_value closure;
    void *cap_base; // heap snapshot of captured fields (ns_null if non-capturing)
    ffi_cif cif;
    ffi_type *arg_types[1];
    ffi_closure *closure_obj;
} ns_callback_bridge;

static void ns_callback_bridge_handler(ffi_cif *cif, void *ret, void **args, void *user) {
    (void)cif;
    (void)ret;
    ns_callback_bridge *b = (ns_callback_bridge *)user;
    void *arg_ptr = *(void **)args[0];
    ns_eval_invoke_callback(b->vm, b->ctx, b->closure, b->cap_base, arg_ptr);
}

void *ns_callback_bridge_create(ns_vm *vm, ns_ast_ctx *ctx, ns_value closure) {
    ns_callback_bridge *b = (ns_callback_bridge *)ns_malloc(sizeof(ns_callback_bridge));
    b->vm = vm;
    b->ctx = ctx;
    b->closure = closure;
    b->cap_base = ns_null;

    // A capturing closure (NS_TYPE_BLOCK) holds its captures on the eval stack,
    // which is reclaimed when the defining scope exits. The callback may fire
    // much later, so snapshot the capture struct onto the heap for its lifetime.
    ns_symbol *sym = &vm->symbols[ns_type_index(closure.t)];
    if (sym->type == NS_SYMBOL_BLOCK) {
        u64 stride = sym->bc.st.stride;
        if (stride > 0) {
            b->cap_base = ns_malloc(stride);
            memcpy(b->cap_base, &vm->stack[closure.o], stride);
        }
    }

    void *code = ns_null;
    b->closure_obj = ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (!b->closure_obj) return ns_null;

    b->arg_types[0] = &ffi_type_pointer;
    if (ffi_prep_cif(&b->cif, FFI_DEFAULT_ABI, 1, &ffi_type_void, b->arg_types) != FFI_OK) return ns_null;
    if (ffi_prep_closure_loc(b->closure_obj, &b->cif, ns_callback_bridge_handler, b, code) != FFI_OK) return ns_null;
    return code;
}
#else
// XCFramework build: FFI not supported
ns_return_bool ns_vm_call_ffi(ns_vm *vm) {
    (void)vm;
    return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "FFI calls not supported in XCFramework build.");
}

void *ns_callback_bridge_create(ns_vm *vm, ns_ast_ctx *ctx, ns_value closure) {
    (void)vm;
    (void)ctx;
    (void)closure;
    return ns_null;
}
#endif // NS_XCLIB

ns_return_bool ns_vm_call_ref(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->callee;

    if (ns_str_equals(fn->lib, ns_str_cstr("std"))) return ns_vm_call_std(vm);
#ifndef NS_XCLIB
    if (fn->fn.fn_ptr)  return ns_vm_call_ffi(vm);

    ns_lib *lib = ns_lib_find(vm, fn->lib);
    if (!lib) lib = ns_lib_import(vm, fn->lib);
    if (!lib) {
        static char msg[512];
        snprintf(msg, sizeof(msg), "ref lib not found: %.*s", fn->lib.len, fn->lib.data);
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, msg);
    }

    ns_str fn_name = ns_str_slice(fn->name, 0, fn->name.len);
    const char *load_err = lib->lib ? ns_null : dlerror();
    dlerror();
    void *fn_ptr = dlsym(lib->lib, fn_name.data);
    if (!fn_ptr) {
        static char msg[1024];
        const char *err = dlerror();
        if (lib->lib) {
            snprintf(msg, sizeof(msg), "ref fn not found: %.*s.%.*s%s%s",
                     fn->lib.len, fn->lib.data,
                     fn_name.len, fn_name.data,
                     err ? ": " : "", err ? err : "");
        } else {
            snprintf(msg, sizeof(msg), "ref lib not loaded: %.*s (%.*s)%s%s; ref fn not found: %.*s.%.*s%s%s",
                     fn->lib.len, fn->lib.data,
                     lib->path.len, lib->path.data,
                     load_err ? ": " : "", load_err ? load_err : "",
                     fn->lib.len, fn->lib.data,
                     fn_name.len, fn_name.data,
                     err ? ": " : "", err ? err : "");
        }
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, msg);
    }

    fn->fn.fn_ptr = fn_ptr;
    return ns_vm_call_ffi(vm);
#else
    // XCFramework build: no FFI or dynamic library support
    (void)call;
    (void)fn;
    return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "External library calls not supported in XCFramework build.");
#endif // NS_XCLIB
}
