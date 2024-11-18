#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_path.h"

#include <dlfcn.h>
#include <stdarg.h>
// #include <ffi.h>

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

    ns_str lib_path = ns_path_join(ns_str_cstr("lib"), ns_str_concat(lib, ns_lib_ext));
    void* lib_ptr = dlopen(lib_path.data, RTLD_LAZY);
    ns_lib _lib = { .name = lib, .path = lib_path, .lib = lib_ptr };
    ns_array_push(vm->libs, _lib);
    return ns_array_last(vm->libs);
}

ns_bool ns_vm_call_ref(ns_vm *vm) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_symbol *fn = call->fn;

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

    void *fn_ptr = dlsym(lib->lib, fn->name.data);
    if (!fn_ptr) {
        ns_error("vm call", "failed to find fn %.*s\n", fn->name.len, fn->name.data);
    }

    fn->fn.fn_ptr = fn_ptr;
do_call:

    // TODO use ffi to call fn

    return true;
}