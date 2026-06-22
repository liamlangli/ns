#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_os.h"

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

// ---------------------------------------------------------------------------
// Static (position-independent) standard library symbol table.
//
// The lib/* C functions are compiled with -fPIC directly into the ns binary,
// so a `ref fn` can be resolved by a compiled-in lookup instead of a runtime
// dlopen()/dlsym() against a shared object. This removes per-process load cost
// (no shared object mapping, relocation or symbol binding at first call) and
// makes the standard library available even when no .so/.dylib is shipped.
//
// Prototypes are intentionally opaque (void*) — only the function address is
// needed here; the real signatures live in lib/include and are honoured by the
// ffi call that follows. Declaring opaque pointers keeps this table free of the
// lib/* headers (and their platform GUI types) while still linking the symbol.
extern void *io_load_image(const char *path);
extern i32 io_save_image(const char *path, const void *img);

extern void *view_create(const char *title, i32 width, i32 height);
extern void view_close(void *v);
extern void view_capture_require(void *v);
extern void view_on_scroll(void *v, f64 x, f64 y);
extern void view_on_resize(void *v, i32 width, i32 height);
extern void view_on_mouse_move(void *v, f64 x, f64 y);
extern void view_on_mouse_btn(void *v, i32 button, i32 action);
extern void view_on_key_action(void *v, i32 key, i32 action);
extern ns_bool view_is_key_pressed(void *v, i32 key);
extern ns_str view_get_clipboard(void *v);
extern void view_set_clipboard(void *v, ns_str text);

extern ns_bool gpu_request_device(void *v);

extern i32 term_enable_raw(void);
extern i32 term_disable_raw(void);
extern i32 term_read_key(void);
extern i32 term_width(void);
extern i32 term_height(void);
extern i32 term_putc(i32 c);
extern i32 term_flush(void);
extern i32 term_path_reset(void);
extern i32 term_path_push(i32 c);
extern i32 term_open_read(void);
extern i32 term_getc(void);
extern i32 term_close_read(void);
extern i32 term_open_write(void);
extern i32 term_putc_file(i32 c);
extern i32 term_close_write(void);
extern i32 term_env_byte(i32 idx);

typedef struct ns_static_sym {
    const char *name;
    void *fn;
} ns_static_sym;

static const ns_static_sym ns_static_syms[] = {
    {"io_load_image", (void*)io_load_image},
    {"io_save_image", (void*)io_save_image},

    {"view_create", (void*)view_create},
    {"view_close", (void*)view_close},
    {"view_capture_require", (void*)view_capture_require},
    {"view_on_scroll", (void*)view_on_scroll},
    {"view_on_resize", (void*)view_on_resize},
    {"view_on_mouse_move", (void*)view_on_mouse_move},
    {"view_on_mouse_btn", (void*)view_on_mouse_btn},
    {"view_on_key_action", (void*)view_on_key_action},
    {"view_is_key_pressed", (void*)view_is_key_pressed},
    {"view_get_clipboard", (void*)view_get_clipboard},
    {"view_set_clipboard", (void*)view_set_clipboard},

    {"gpu_request_device", (void*)gpu_request_device},

    {"term_enable_raw", (void*)term_enable_raw},
    {"term_disable_raw", (void*)term_disable_raw},
    {"term_read_key", (void*)term_read_key},
    {"term_width", (void*)term_width},
    {"term_height", (void*)term_height},
    {"term_putc", (void*)term_putc},
    {"term_flush", (void*)term_flush},
    {"term_path_reset", (void*)term_path_reset},
    {"term_path_push", (void*)term_path_push},
    {"term_open_read", (void*)term_open_read},
    {"term_getc", (void*)term_getc},
    {"term_close_read", (void*)term_close_read},
    {"term_open_write", (void*)term_open_write},
    {"term_putc_file", (void*)term_putc_file},
    {"term_close_write", (void*)term_close_write},
    {"term_env_byte", (void*)term_env_byte},
};

// Resolve a `ref fn` to a statically linked symbol, or NULL when the name is
// not part of the built-in standard library (callers then fall back to dlsym).
void *ns_lib_static_sym(ns_str name) {
    for (u32 i = 0; i < sizeof(ns_static_syms) / sizeof(ns_static_syms[0]); i++) {
        // Exact match required: ns_str_equals_STR() compares only strlen(entry)
        // bytes, so a table name that is a prefix of the queried name (e.g.
        // "term_putc" vs "term_putc_file") would mis-resolve to the shorter
        // symbol. Anchor on the full queried length to forbid that.
        const char *sym = ns_static_syms[i].name;
        if (name.data && name.len == (i32)strlen(sym) &&
            strncmp(name.data, sym, (size_t)name.len) == 0) {
            return ns_static_syms[i].fn;
        }
    }
    return ns_null;
}
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
        call->ret = (ns_value){.t = ns_type_u64, .o = fd};
    } else if (ns_str_equals_STR(call->callee->name, "write")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        ns_value data = vm->symbol_stack[call->arg_offset + 1].val;
        ns_str s = ns_eval_str(vm, data);
        ns_str ss = ns_str_unescape(s);
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        i32 len = fwrite(ss.data, ss.len, 1, f);
        call->ret = (ns_value){.t = ns_type_u64, .o = len};
    } else if (ns_str_equals_STR(call->callee->name, "read")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        ns_value size = vm->symbol_stack[call->arg_offset + 1].val;
        FILE *f = (FILE*)ns_eval_number_u64(vm, fd);
        i32 s = ns_eval_number_i32(vm, size);
        char *buff = ns_malloc(s);
        i32 len = fread(buff, s, 1, f);
        ns_str ret = (ns_str){.data = buff, .len = len};
        call->ret = (ns_value){.t = ns_type_str, .o = (u64)ret.data};
    } else if (ns_str_equals_STR(call->callee->name, "close")) {
        ns_value fd = vm->symbol_stack[call->arg_offset].val;
        fclose((FILE*)fd.o);
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
    ns_return_assert(ret);
    vm->lib = prev;

    if (ns_str_equals(lib_name, ns_str_cstr("std"))) {
        ns_lib _lib = { .name = lib_name, .path = ns_str_null, .lib = ns_null };
        ns_array_push(vm->libs, _lib);
        return ns_array_last(vm->libs);
    } else {
#ifndef NS_XCLIB
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
#else
        // XCFramework build: no dynamic library loading
        ns_lib _lib = { .name = lib_name, .path = ns_str_null, .lib = ns_null };
        ns_array_push(vm->libs, _lib);
        return ns_array_last(vm->libs);
#endif // NS_XCLIB
    }
}

#ifndef NS_XCLIB
ffi_type ns_ffi_map_type(ns_type t) {
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
                memcpy(dst, (i8*)vm->stack + v.o, offset);
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
    ffi_call(&cif, FFI_FN(fn->fn.fn_ptr), &vm->stack[v.o], _ffi_ctx.values);
    if (ns_type_is_ref(t)) {
        // Native code returns an absolute pointer to the result. Represent it as
        // an absolute-address ref value (stack=false, .o = the pointer) so that
        // member access dereferences the heap struct directly.
        call->ret.t = ns_type_set_stack(t, false);
        call->ret.o = *(u64*)&vm->stack[v.o];
    } else {
        if (size > 0) ns_eval_copy(vm, call->ret, v, size);
    }
    return ns_return_ok(bool, true);
}
#else
// XCFramework build: FFI not supported
ns_return_bool ns_vm_call_ffi(ns_vm *vm) {
    (void)vm;
    return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "FFI calls not supported in XCFramework build.");
}
#endif // NS_XCLIB

ns_return_bool ns_vm_call_ref(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->callee;

    if (ns_str_equals(fn->lib, ns_str_cstr("std"))) return ns_vm_call_std(vm);
#ifndef NS_XCLIB
    if (fn->fn.fn_ptr)  return ns_vm_call_ffi(vm);

    // Prefer the statically linked symbol: no dlopen/dlsym round-trip and it
    // works without any shipped shared object. Cache the resolved pointer.
    void *static_ptr = ns_lib_static_sym(fn->name);
    if (static_ptr) {
        fn->fn.fn_ptr = static_ptr;
        return ns_vm_call_ffi(vm);
    }

    ns_lib *lib = ns_lib_find(vm, fn->lib);
    if (!lib) lib = ns_lib_import(vm, fn->lib);
    if (!lib) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "ref lib not found.");

    ns_str fn_name = ns_str_slice(fn->name, 0, fn->name.len);
    void *fn_ptr = dlsym(lib->lib, fn_name.data);
    if (!fn_ptr) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "ref fn not found.");

    fn->fn.fn_ptr = fn_ptr;
    return ns_vm_call_ffi(vm);
#else
    // XCFramework build: no FFI or dynamic library support
    (void)call;
    (void)fn;
    return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, "External library calls not supported in XCFramework build.");
#endif // NS_XCLIB
}
