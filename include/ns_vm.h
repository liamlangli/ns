#pragma once

#include "ns_ast.h"
#include "ns_type.h"

typedef enum {
    NS_RECORD_INVALID,
    NS_RECORD_VALUE,
    NS_RECORD_FN,
    NS_RECORD_STRUCT,
} NS_RECORD_TYPE;

typedef struct ns_record ns_record;

typedef struct ns_value_record {
    ns_type type;
    NS_VALUE_SCOPE scope;
    bool is_const;
    bool is_ref;
} ns_value_record;

typedef struct ns_fn_record {
    ns_type ret;
    ns_record *args;
    ns_record *locals;
    ns_record *globals;
    int ast;
} ns_fn_record;

typedef struct ns_struct_record {
    ns_str name;
    ns_record *fields;
    int ast;
} ns_struct_record;

typedef struct ns_record {
    NS_RECORD_TYPE type;
    ns_str name;
    int index;
    union {
        ns_fn_record fn;
        ns_value_record val;
        ns_struct_record st;
    };
} ns_record;

typedef struct ns_fn {
    ns_str name;
    int ast;
} ns_fn;

typedef struct ns_struct {
    ns_str name;
    int index;
    ns_str *field_names;
} ns_struct;

typedef struct ns_call {
    ns_fn_record fn;
} ns_call;

typedef struct ns_vm {
    ns_call *call_stack;
    ns_record *records;
} ns_vm;

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, int i);
ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);