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

typedef struct ns_value_pair {
    ns_str key;
    int value;
} ns_value_pair;

typedef struct ns_vm_t {
    ns_ast_t *ast;
    ns_value *values;
    int stack_depth;
    ns_value_pair *global_values;
} ns_vm_t;

ns_value ns_new_i32(ns_vm_t *vm, i32 i);
ns_value ns_new_f64(ns_vm_t *vm, f64 f);
ns_value ns_new_bool(ns_vm_t *vm, bool b);

ns_vm_t *ns_create_vm();
ns_value ns_call(ns_vm_t *vm, ns_value fn, ns_value *args, int argc);
ns_value ns_eval(ns_vm_t *vm, const char* source, const char *filename);