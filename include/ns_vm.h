#pragma once

#include "ns_ast.h"
#include "ns_type.h"

typedef enum {
    NS_SYMBOL_INVALID,
    NS_SYMBOL_VALUE,
    NS_SYMBOL_FN,
    NS_SYMBOL_FN_CALL,
    NS_SYMBOL_STRUCT,
} NS_SYMBOL_TYPE;

typedef enum {
    NS_VM_MODE_EVAL,
    NS_VM_MODE_REPL
} ns_vm_mode;

typedef struct ns_symbol ns_symbol;

typedef struct ns_value_symbol {
    ns_type type;
    NS_VALUE_SCOPE scope;
    ns_value val;
    bool is_const;
    bool is_ref;
} ns_value_symbol;

typedef struct ns_fn_symbol {
    ns_type ret;
    ns_symbol *args;
    ns_value fn;
    ns_ast_t ast;
} ns_fn_symbol;

typedef struct ns_fn_call_symbol {
    ns_symbol *fn;
    ns_symbol *locals;
} ns_fn_call_symbol;

typedef struct ns_struct_symbol {
    ns_str name;
    ns_symbol *fields;
    ns_ast_t ast;
} ns_struct_symbol;

typedef struct ns_symbol {
    NS_SYMBOL_TYPE type;
    ns_str name;
    ns_str lib;
    i32 index;
    bool parsed;
    union {
        ns_fn_symbol fn;
        ns_fn_call_symbol call;
        ns_value_symbol val;
        ns_struct_symbol st;
    };
} ns_symbol;

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
    ns_symbol *fn;
    ns_symbol *locals;
    ns_value *args;
    ns_value ret;
} ns_call;

typedef struct ns_vm {
    // parse state
    ns_symbol *symbols;
    ns_symbol *call_symbols;

    // eval state
    ns_call *call_stack;
    ns_value *globals;
    ns_str *str_list;
    ns_data *data_list;

    ns_vm_mode mode;
} ns_vm;

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n);
ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);

// vm record
int ns_vm_push_symbol(ns_vm *vm, ns_symbol r);
int ns_vm_push_string(ns_vm *vm, ns_str s);
int ns_vm_push_data(ns_vm *vm, ns_data d);
ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t);
ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

// vm std
void ns_vm_import_std_symbols(ns_vm *vm);
ns_value ns_vm_eval_std(ns_vm *vm);

void ns_repl(ns_vm* vm);