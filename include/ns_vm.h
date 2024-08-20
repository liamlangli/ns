#pragma once

#include "ns_parse.h"
#include "ns_type.h"

typedef enum { 
    NS_TYPE_NIL = -1,
    NS_TYPE_INFER = 0,
    NS_TYPE_I8,
    NS_TYPE_I16,
    NS_TYPE_I32,
    NS_TYPE_I64,
    NS_TYPE_U8,
    NS_TYPE_U16,
    NS_TYPE_U32,
    NS_TYPE_U64,
    NS_TYPE_F32,
    NS_TYPE_F64,
    NS_TYPE_BOOL,
    NS_TYPE_STRING,
    NS_TYPE_FN,
    NS_TYPE_STRUCT,
    NS_TYPE_ARRAY,
    NS_TYPE_,
} NS_VALUE_TYPE ;

#define NS_NIL ((ns_value){.type = NS_TYPE_NIL})
#define NS_TRUE ((ns_value){.type = NS_TYPE_BOOL, .u.boolean = true})
#define NS_FALSE ((ns_value){.type = NS_TYPE_BOOL, .u.boolean = false})

typedef union ns_value_union {
    i64 int64;
    f64 float64;
    bool boolean;
    int ptr;
} ns_value_union;

typedef struct ns_value {
    NS_VALUE_TYPE type;
    int index;
    ns_value_union u;
} ns_value;

typedef struct ns_fn_t {
    ns_str name;
    int ast_root;
    int arg_count, local_count;
    ns_str arg_names[NS_MAX_PARAMS];
    ns_value args[NS_MAX_PARAMS];
    ns_str local_names[NS_MAX_PARAMS];
    ns_value locals[NS_MAX_PARAMS];
} ns_fn_t;

typedef struct ns_struct_t {
    ns_str name;
    int index;
    int field_count;
    ns_str field_names[NS_MAX_FIELDS];
} ns_struct_t;

typedef struct ns_call_scope {
    int fn_index;
    int argc;
    bool returned;
    ns_value ret;
    ns_value locals[NS_MAX_PARAMS];
    ns_value args[NS_MAX_PARAMS];
} ns_call_scope;

typedef struct ns_vm_t {
    ns_parse_context_t *parse_ctx;
    ns_ast_t *ast;
    ns_value values[NS_MAX_VALUE_COUNT];
    int value_count;

    ns_value global_values[NS_MAX_GLOBAL_VARS];
    ns_str global_names[NS_MAX_GLOBAL_VARS];
    int global_count;

    int stack_depth;
    ns_fn_t fns[NS_MAX_FN_COUNT];
    int fn_count;

    // func call stack
    ns_call_scope call_stack[NS_MAX_CALL_STACK];
    int call_stack_top;
    ns_value ret;
} ns_vm_t;

ns_value ns_new_i32(ns_vm_t *vm, i32 i);
ns_value ns_new_f64(ns_vm_t *vm, f64 f);
ns_value ns_new_bool(ns_vm_t *vm, bool b);

ns_vm_t *ns_create_vm();
ns_value ns_eval_expr(ns_vm_t *vm, int expr);
ns_value ns_eval(ns_vm_t *vm, const char* source, const char *filename);
