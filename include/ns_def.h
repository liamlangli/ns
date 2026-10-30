#include "ns.h"
#include "ns_vm.h"

typedef struct ns_arg_def {
    ns_str name;
    ns_type type;
} ns_arg_def;

typedef struct ns_fn_def {
    ns_str name;
    ns_str lib;
    ns_arg_def *args;
    i32 arg_count;
    ns_type ret;

    void *fn;
} ns_fn_def;

void ns_vm_def_fn(ns_vm *vm, ns_fn_def *fn);