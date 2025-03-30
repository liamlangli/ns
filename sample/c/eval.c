#include "ns.h"
#include "ns_def.h"

#include <stdio.h>

i32 add(i32 a, i32 b) {
    return a + b;
}

i32 main(i32 argc, i8 *argv[]) {
    ns_unused(argc);
    ns_unused(argv);

    ns_fn_def add_fn = {0};
    add_fn.name = ns_str_cstr("add");
    add_fn.lib = ns_str_cstr("");

    ns_vm vm = {0};
    ns_eval(&vm, ns_str_cstr("import std\nprint(\"Hello, ns!\n\")"), ns_str_cstr("<eval>"));

    return 0;
}