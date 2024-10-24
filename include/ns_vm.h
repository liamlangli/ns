#pragma once

#include "ns_ast.h"
#include "ns_type.h"

#define ns_vm_error(f, s, t, m, ...) ns_error(t, "\n[%.*s:%d:%d]: " m "\n", f.len, f.data, s.l, s.o, ##__VA_ARGS__)

typedef enum {
    NS_SYMBOL_INVALID,
    NS_SYMBOL_VALUE,
    NS_SYMBOL_FN,
    NS_SYMBOL_FN_CALL,
    NS_SYMBOL_STRUCT,
} NS_SYMBOL_TYPE;

typedef struct ns_symbol ns_symbol;

typedef struct ns_value_symbol {
    ns_type type;
    NS_VALUE_SCOPE scope;
    ns_value val;
    bool is_const;
    bool is_ref;

    // for st field
    i32 offset, size;
} ns_value_symbol;

typedef struct ns_fn_symbol {
    ns_type ret;
    ns_symbol *args;
    ns_value fn;
    ns_ast_t ast;
} ns_fn_symbol;

typedef struct ns_struct_symbol {
    ns_type type;
    ns_str name;
    ns_symbol *fields;
    ns_ast_t ast;
    i32 stride;
} ns_struct_symbol;

typedef struct ns_scope_symbol {
    ns_symbol *vars;
} ns_scope_symbol;

typedef struct ns_array_symbol {
    ns_type element_type;
    bool stack;
} ns_array_symbol;

typedef struct ns_fn_call_symbol {
    ns_symbol *fn;
    ns_symbol *locals;
    ns_scope_symbol *scopes;
} ns_fn_call_symbol;

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

typedef struct ns_scope {
    ns_symbol *vars;
    i32 stack_top;
} ns_scope;

typedef struct ns_call {
    ns_symbol *fn;
    ns_value *args;
    ns_value ret;
    ns_scope *scopes;
} ns_call;

typedef struct ns_vm {
    // parse state
    ns_symbol *symbols;
    i32 parsed_symbol_count;

    ns_symbol *call_symbols;

    // eval state
    ns_call *call_stack;
    ns_value *globals;
    ns_str *str_list;
    ns_data *data_list;
    i8* stack;

    // mode
    bool repl;
} ns_vm;

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n);
ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);

// ops fn
ns_str ns_ops_name(ns_token_t op);
ns_str ns_ops_override_name(ns_str l, ns_str r, ns_token_t op);

// vm record
i32 ns_vm_push_symbol(ns_vm *vm, ns_symbol r);
i32 ns_vm_push_string(ns_vm *vm, ns_str s);
i32 ns_vm_push_data(ns_vm *vm, ns_data d);
i32 ns_type_size(ns_vm *vm, ns_type t);
ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t);
ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

// vm std
void ns_vm_import_std_symbols(ns_vm *vm);
ns_value ns_vm_eval_std(ns_vm *vm);

void ns_repl(ns_vm* vm);