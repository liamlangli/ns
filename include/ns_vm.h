#pragma once

#include "ns_ast.h"
#include "ns_type.h"

typedef enum {
    NS_RECORD_INVALID,
    NS_RECORD_VALUE,
    NS_RECORD_FN,
    NS_RECORD_FN_CALL,
    NS_RECORD_STRUCT,
} NS_RECORD_TYPE;

typedef struct ns_record ns_record;

typedef struct ns_value_record {
    ns_type type;
    NS_VALUE_SCOPE scope;
    ns_value val;
    bool is_const;
    bool is_ref;
} ns_value_record;

typedef struct ns_fn_record {
    ns_type ret;
    ns_record *args;
    ns_value fn;
    i32 ast;
} ns_fn_record;

typedef struct ns_fn_call_record {
    ns_record *fn;
    ns_record *locals;
} ns_fn_call_record;

typedef struct ns_struct_record {
    ns_str name;
    ns_record *fields;
    i32 ast;
} ns_struct_record;

typedef struct ns_record {
    NS_RECORD_TYPE type;
    ns_str name;
    ns_str lib;
    i32 index;
    union {
        ns_fn_record fn;
        ns_fn_call_record call;
        ns_value_record val;
        ns_struct_record st;
    };
} ns_record;

typedef struct ns_fn {
    ns_str name;
    i32 ast;
} ns_fn;

typedef struct ns_struct {
    ns_str name;
    i32 index;
    ns_str *field_names;
} ns_struct;

typedef struct ns_call {
    ns_record *fn;
    ns_record *locals;
    ns_value *args;
    ns_value ret;
} ns_call;

typedef struct ns_vm {
    // parse state
    ns_record *records;
    ns_record *call_records;

    // eval state
    ns_call *call_stack;
    ns_value *globals;
    ns_str *str_list;
    ns_data *data_list;
} ns_vm;

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);

// vm record
int ns_vm_push_record(ns_vm *vm, ns_record r);
int ns_vm_push_string(ns_vm *vm, ns_str s);
int ns_vm_push_data(ns_vm *vm, ns_data d);
ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t);
ns_record* ns_vm_find_record(ns_vm *vm, ns_str s);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

// vm std
void ns_vm_import_std_records(ns_vm *vm);
ns_value ns_vm_eval_std(ns_vm *vm);

void ns_repl(ns_vm* vm);