#include "ns.h"
#include "ns_def.h"

#include <stdio.h>

i32 add(i32 a, i32 b) {
    return a + b;
}

const i8 *script = 
"import std\n"
"let c = add(1, 2)\n"
"print(`Hello, ns! {c}\n`)\n";

i32 main() {
    ns_vm vm = {0};

    ns_fn_def add_fn = {0};
    add_fn.name = ns_str_cstr("add");
    add_fn.lib = ns_str_cstr("");
    add_fn.args = (ns_arg_def[]){
        { .name = ns_str_cstr("a"), .type = ns_type_i32 },
        { .name = ns_str_cstr("b"), .type = ns_type_i32 },
    };
    add_fn.arg_count = 2;
    add_fn.ret = ns_type_i32;
    add_fn.fn = add;
    ns_vm_def_fn(&vm, &add_fn);
    ns_return_value ret =  ns_eval(&vm, ns_str_cstr(script), ns_str_cstr("<eval>"));
    ns_return_assert(ret);
    return 0;
}