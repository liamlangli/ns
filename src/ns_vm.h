#pragma once

#include "ns_parse.h"
#include "ns_type.h"

#include "stb_ds.h"

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
    NS_TYPE_FUNCTION,
    NS_TYPE_STRUCT,
    NS_TYPE_ARRAY,
    NS_TYPE_,
} NS_VALUE_TYPE ;

#define NS_NIL ((ns_value){.type = NS_TYPE_NIL})

typedef union ns_value_union {
    i32 int32;
    f64 float64;
    int ptr;
} ns_value_union;

typedef struct ns_value {
    NS_VALUE_TYPE type;
    ns_value_union u;
} ns_value;

typedef struct ns_vm_t {
    ns_ast_t *ast;
    int stack_depth;
    ns_value *global;
} ns_vm_t;

ns_vm_t *ns_create_vm();
ns_value ns_call(ns_vm_t *vm, ns_value fn, ns_value *args, int argc);
ns_value ns_eval(ns_vm_t *vm, const char* source, const char *filename);