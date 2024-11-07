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
} ns_symbol_type;

typedef struct ns_symbol ns_symbol;

typedef struct ns_fn_symbol {
    ns_type ret;
    ns_symbol *args;
    ns_value fn;
    ns_ast_t ast;
} ns_fn_symbol;

typedef struct ns_struct_field {
    ns_str name;
    ns_type t;
    u64 o, s;
} ns_struct_field;

typedef struct ns_struct_symbol {
    ns_value st;
    ns_str name;
    ns_struct_field *fields;
    ns_ast_t ast;
    u64 stride;
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
    ns_symbol_type type;
    ns_str name;
    ns_str lib;
    bool parsed;
    union {
        ns_value val;
        ns_fn_symbol fn;
        ns_fn_call_symbol call;
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
    u32 scope_top;
} ns_call;

typedef struct ns_vm {
    // parse state
    ns_symbol *symbols;
    i32 parsed_symbol_count;
    ns_symbol *call_symbols;

    // eval state
    ns_call *call_stack;
    ns_scope *scope_stack;
    ns_value *globals;
    ns_str *str_list;
    ns_data *data_list;
    i8* stack;

    // mode
    bool repl;
} ns_vm;


// ops fn
ns_str ns_ops_name(ns_token_t op);
ns_str ns_ops_override_name(ns_str l, ns_str r, ns_token_t op);

// number type
ns_number_type ns_vm_number_type(ns_type t);

// vm parse stage
i32 ns_vm_push_symbol(ns_vm *vm, ns_symbol r);
i32 ns_vm_push_string(ns_vm *vm, ns_str s);
i32 ns_vm_push_data(ns_vm *vm, ns_data d);
u64 ns_type_size(ns_vm *vm, ns_type t);
ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t);
ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t, bool infer);

// eval fn
#define ns_eval_value_def(type) type ns_eval_number_##type(ns_vm *vm, ns_value n);
ns_eval_value_def(i8)
ns_eval_value_def(i16)
ns_eval_value_def(i32)
ns_eval_value_def(i64)
ns_eval_value_def(u8)
ns_eval_value_def(u16)
ns_eval_value_def(u32)
ns_eval_value_def(u64)
ns_eval_value_def(f32)
ns_eval_value_def(f64)
bool ns_eval_bool(ns_vm *vm, ns_value n);

ns_scope *ns_eval_enter_scope(ns_vm *vm);
ns_scope *ns_eval_exit_scope(ns_vm *vm, ns_call *call);

ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n);
ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);

// vm std
void ns_vm_import_std_symbols(ns_vm *vm);
ns_value ns_vm_eval_std(ns_vm *vm);

void ns_repl(ns_vm* vm);