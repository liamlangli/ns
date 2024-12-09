#pragma once

#include "ns_ast.h"
#include "ns_type.h"

typedef enum {
    NS_SYMBOL_INVALID,
    NS_SYMBOL_VALUE,
    NS_SYMBOL_FN,
    NS_SYMBOL_STRUCT,
} ns_symbol_type;

typedef struct ns_symbol ns_symbol;

typedef struct ns_fn_symbol {
    ns_type ret;
    ns_symbol *args;
    ns_value fn;
    ns_ast_t ast;
    void *fn_ptr;
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
   ns_bool stack;
} ns_array_symbol;

typedef struct ns_symbol {
    ns_symbol_type type;
    ns_str name;
    ns_str lib;
   ns_bool parsed;
    union {
        ns_value val;
        ns_fn_symbol fn;
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
    i32 stack_top;
    i32 symbol_top;
} ns_scope;

typedef struct ns_call {
    ns_symbol *fn;
    i32 arg_offset, arg_count;
    ns_value ret;
    u32 scope_top;
} ns_call;

typedef struct ns_lib {
    void *lib;
    ns_str name;
    ns_str path;
} ns_lib;

typedef struct ns_vm {
    // parse state
    ns_symbol *symbols;
    i32 symbol_top;

    // eval state
    ns_call *call_stack;
    ns_scope *scope_stack;
    ns_symbol *symbol_stack;

    ns_str *str_list;
    ns_data *data_list;
    ns_lib *libs;
    ns_str lib;
    i8* stack;

    // mode
    ns_bool repl;

    // debug
    void (*step_hook)(struct ns_vm *vm, ns_ast_ctx *ctx, i32 i);
    void *debug_session;
    i32 stack_depth;
} ns_vm;

#define NS_MAX_STACK_DEPTH 255

// ops fn
ns_str ns_ops_name(ns_token_t op);
ns_str ns_ops_override_name(ns_str l, ns_str r, ns_token_t op);

// number type
ns_number_type ns_vm_number_type(ns_type t);

// vm parse stage
i32 ns_vm_push_symbol_global(ns_vm *vm, ns_symbol r);
i32 ns_vm_push_string(ns_vm *vm, ns_str s);
i32 ns_vm_push_data(ns_vm *vm, ns_data d);
i32 ns_type_size(ns_vm *vm, ns_type t);
ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t);
ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s);
ns_return_bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t,ns_bool infer);

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
ns_bool ns_eval_bool(ns_vm *vm, ns_value n);
ns_str ns_eval_str(ns_vm *vm, ns_value n);
void *ns_eval_array_raw(ns_vm *vm, ns_value n);
u64 ns_eval_alloc(ns_vm *vm, i32 stride);
ns_return_value ns_eval_copy(ns_vm *vm, ns_value dst, ns_value src, i32 size);

ns_scope *ns_enter_scope(ns_vm *vm);
ns_scope *ns_exit_scope(ns_vm *vm);

ns_return_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);

// vm eval stage
void ns_vm_symbol_print(ns_vm *vm);
ns_return_bool ns_vm_call_ref(ns_vm *vm);

// vm mod
ns_lib* ns_lib_import(ns_vm *vm, ns_str lib);
ns_lib* ns_lib_find(ns_vm *vm, ns_str lib);

// vm repl
void ns_repl(ns_vm* vm);