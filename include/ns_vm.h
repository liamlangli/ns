#pragma once

#include "ns_ast.h"
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
    NS_TYPE_ALIAS,
} NS_VALUE_TYPE;

typedef enum {
    NS_SCOPE_GLOBAL,
    NS_SCOPE_LOCAL,
    NS_SCOPE_PARAM
} NS_VALUE_SCOPE;

typedef enum {
    NS_RECORD_INVALID,
    NS_RECORD_VALUE,
    NS_RECORD_FN,
    NS_RECORD_STRUCT,
} NS_RECORD_TYPE;

#define NS_NIL ((ns_value){.type = NS_TYPE_NIL,})
#define NS_TRUE ((ns_value){.type = NS_TYPE_BOOL, .u.boolean = true})
#define NS_FALSE ((ns_value){.type = NS_TYPE_BOOL, .u.boolean = false})

typedef struct ns_value_record {
    NS_VALUE_TYPE type;
    NS_VALUE_SCOPE scope;
} ns_value_record;

typedef struct ns_fn_record {
    int arg_count, local_count, global_count;
    ns_value_record args[NS_MAX_PARAMS];
    ns_value_record locals[NS_MAX_PARAMS];
    ns_value_record globals[NS_MAX_PARAMS];
    int body;
} ns_fn_record;

typedef struct ns_struct_record {
    int field_count;
    ns_value_record fields[NS_MAX_FIELDS];
} ns_struct_record;

typedef struct ns_record {
    NS_RECORD_TYPE type;
    ns_str name;
    int index;
    union {
        ns_value_record val;
        ns_fn_record fn;
        ns_struct_record st;
    };
} ns_record;

typedef struct ns_value {
    NS_VALUE_TYPE type;
    int index;
    union {
        i64 i;
        f64 f;
    };
} ns_value;

typedef struct ns_fn {
    ns_str name;
    int ast_root;
    int arg_count, local_count;
    ns_str arg_names[NS_MAX_PARAMS];
    ns_value args[NS_MAX_PARAMS];
    ns_str local_names[NS_MAX_PARAMS];
    ns_value locals[NS_MAX_PARAMS];
} ns_fn;

typedef struct ns_struct {
    ns_str name;
    int index;
    int field_count;
    ns_str field_names[NS_MAX_FIELDS];
} ns_struct;

typedef struct ns_call {
    ns_fn_record fn;
} ns_call;

typedef struct ns_vm {
    ns_call *call_stack;
    int record_count;
    ns_record records[NS_MAX_RECORD_COUNT];
} ns_vm;

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, int i);
ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);