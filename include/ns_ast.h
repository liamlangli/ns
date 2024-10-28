#pragma once

#include "ns_token.h"
#include "ns_type.h"

#include <assert.h>
#define ns_ast_error(c, t, m, ...) ns_error(t, "\n[%s:%d:%d]: " m "\n", c->filename.data, c->token.line, c->f - c->token.line_start, ##__VA_ARGS__ );

typedef enum {
    NS_AST_UNKNOWN = 0,
    NS_AST_PROGRAM,

    NS_AST_TYPE_LABEL,

    NS_AST_ARG_DEF,
    NS_AST_FN_DEF,
    NS_AST_OPS_FN_DEF,
    NS_AST_VAR_DEF,
    NS_AST_STRUCT_DEF,
    NS_AST_STRUCT_FIELD_DEF,
    NS_AST_TYPE_DEF,
    NS_AST_FIELD_DEF,

    NS_AST_EXPR,
    NS_AST_PRIMARY_EXPR,
    NS_AST_BINARY_EXPR,
    NS_AST_MEMBER_EXPR,
    NS_AST_UNARY_EXPR,
    NS_AST_CALL_EXPR,
    NS_AST_INDEX_EXPR,
    NS_AST_CAST_EXPR,
    NS_AST_GEN_EXPR,
    NS_AST_DESIG_EXPR,

    NS_AST_IF_STMT,
    NS_AST_IMPORT_STMT,
    NS_AST_FOR_STMT,
    NS_AST_LOOP_STMT,
    NS_AST_RETURN_STMT,
    NS_AST_JUMP_STMT,
    NS_AST_LABELED_STMT,
    NS_AST_COMPOUND_STMT
} NS_AST_TYPE;

typedef struct ns_ast_t ns_ast_t;

typedef struct ns_ast_state {
    i32 f, l, o; // buffer offset, line, line offset
} ns_ast_state;

typedef struct ns_ast_type_label {
    bool is_ref;
    ns_token_t name;
} ns_ast_type_label;

typedef struct ns_ast_arg {
    ns_token_t name;
    i32 type;
} ns_ast_arg;

typedef struct ns_ast_fn_def {
    bool is_ref;
    bool is_async;
    bool is_kernel;
    ns_token_t name;
    i32 ret;
    i32 body;
    i32 arg_count;
} ns_ast_fn_def;

typedef struct ns_ast_ops_fn_def {
    bool is_ref;
    bool is_async;
    ns_token_t ops;
    i32 ret;
    i32 body;
    i32 left;
    i32 right;
} ns_ast_ops_fn_def;

typedef struct ns_ast_var_def {
    ns_token_t name;
    i32 expr;
    i32 type;
} ns_ast_var_def;

typedef struct ns_ast_struct_def {
    ns_token_t name;
    i32 count;
} ns_ast_struct_def;

typedef struct ns_ast_type_def {
    ns_token_t name;
    ns_token_t type;
} ns_ast_type_def;

typedef struct ns_ast_binary_expr {
    i32 left;
    i32 right;
    ns_token_t op;
} ns_ast_binary_expr;

typedef struct ns_ast_cast_expr {
    i32 expr;
    ns_token_t type;
} ns_ast_cast_expr;

typedef struct ns_ast_primary_expr {
    ns_token_t token;
    i32 expr;
} ns_ast_primary_expr;

typedef struct ns_ast_unary_expr {
    ns_token_t op;
    i32 expr;
} ns_ast_unary_expr;

typedef struct ns_ast_expr {
    i32 body;
} ns_ast_expr;

typedef struct ns_ast_member_expr {
    i32 left;
    ns_token_t right;
} ns_ast_member_expr;

typedef struct ns_ast_call_expr {
    i32 callee;
    i32 arg_count;
    i32 arg;
} ns_ast_call_expr;

typedef struct ns_ast_index_expr {
    i32 table;
    i32 expr;
} ns_ast_index_expr;

typedef struct ns_ast_gen_expr {
    ns_token_t name;
    i32 from;
    i32 to;
    bool range;
} ns_ast_gen_expr;

typedef struct ns_ast_import_stmt {
    ns_token_t lib;
} ns_ast_import_stmt;

typedef struct ns_ast_if_stmt {
    i32 condition;
    i32 body;
    i32 else_body;
} ns_ast_if_stmt;

typedef struct ns_ast_for_stmt {
    i32 generator;
    i32 body;
} ns_ast_for_stmt;

typedef struct ns_ast_loop_stmt {
    i32 condition;
    i32 body;
    bool do_first;
} ns_ast_loop_stmt;

typedef struct ns_ast_jump_stmt {
    ns_token_t label;
    i32 expr;
} ns_ast_jump_stmt;

typedef struct ns_ast_labeled_stmt {
    ns_token_t label;
    i32 condition;
    i32 stmt;
} ns_ast_labeled_stmt;

typedef struct ns_ast_compound_stmt {
    i32 count;
} ns_ast_compound_stmt;

typedef struct ns_ast_desig_expr {
    ns_token_t name;
    i32 count;
} ns_ast_desig_expr;

typedef struct ns_ast_struct_field {
    ns_token_t name;
    i32 expr;
} ns_ast_struct_field;

typedef struct ns_ast_t {
    NS_AST_TYPE type;
    i32 next; // -1 mean null ref
    ns_ast_state state;
    union {
        ns_ast_type_label type_label;

        ns_ast_arg arg;
        ns_ast_fn_def fn_def;
        ns_ast_ops_fn_def ops_fn_def;
        ns_ast_var_def var_def;
        ns_ast_struct_def struct_def;
        ns_ast_type_def type_def;
        ns_ast_struct_field field_def;

        ns_ast_expr expr;
        ns_ast_binary_expr binary_expr;
        ns_ast_call_expr call_expr;
        ns_ast_index_expr index_expr;
        ns_ast_cast_expr cast_expr;
        ns_ast_member_expr member_expr;
        ns_ast_primary_expr primary_expr;
        ns_ast_unary_expr unary_expr;
        ns_ast_gen_expr gen_expr;
        ns_ast_desig_expr desig_expr;

        ns_ast_import_stmt import_stmt;
        ns_ast_if_stmt if_stmt;
        ns_ast_loop_stmt loop_stmt;
        ns_ast_for_stmt for_stmt;
        ns_ast_jump_stmt jump_stmt;
        ns_ast_labeled_stmt label_stmt;
        ns_ast_compound_stmt compound_stmt;
    };
} ns_ast_t;

#define ns_ast_nil (ns_ast_t){.type = NS_AST_UNKNOWN, .next = -1}

#define NS_MAX_PARSE_STACK 64
#define NS_MAX_CALL_STACK 64

typedef struct as_parse_context_t {
    i32 f, last_f;
    i32 *sections;
    i32 section_begin, section_end;
    i32 current;

    ns_ast_t *nodes;

    i32 stack[NS_MAX_PARSE_STACK];
    i32 top;

    ns_token_t token, last_token;
    ns_str source;
    ns_str filename;
    ns_str output;
} ns_ast_ctx;

ns_str ns_ast_type_to_string(NS_AST_TYPE type);

// token fn
bool ns_parse_next_token(ns_ast_ctx *ctx); // skip space
bool ns_token_can_be_type(NS_TOKEN t);
bool ns_token_require(ns_ast_ctx *ctx, NS_TOKEN token);
bool ns_token_require_type(ns_ast_ctx *ctx);
bool ns_token_skip_eol(ns_ast_ctx *ctx);

// node fn
void ns_restore_state(ns_ast_ctx *ctx, ns_ast_state state);
ns_ast_state ns_save_state(ns_ast_ctx *ctx);
i32 ns_ast_push(ns_ast_ctx *ctx, ns_ast_t n);

// primary fn
bool ns_parse_identifier(ns_ast_ctx *ctx);

// type fn
bool ns_parse_unary_expr(ns_ast_ctx *ctx);
bool ns_parse_type_name(ns_ast_ctx *ctx);

// external fn
bool ns_parse_fn_define(ns_ast_ctx *ctx);
bool ns_parse_ops_fn_define(ns_ast_ctx *ctx);
bool ns_parse_var_define(ns_ast_ctx *ctx);
bool ns_parse_struct_def(ns_ast_ctx *ctx);
bool ns_parse_type_define(ns_ast_ctx *ctx);

// stmt fn
bool ns_parse_global_define(ns_ast_ctx *ctx);
bool ns_parse_stmt(ns_ast_ctx *ctx);
bool ns_parse_compound_stmt(ns_ast_ctx *ctx);
bool ns_parse_desig_expr(ns_ast_ctx *ctx);

// expr fn
ns_ast_t ns_parse_stack_top(ns_ast_ctx *ctx);
bool ns_parse_gen_expr(ns_ast_ctx *ctx);
bool ns_parse_primary_expr(ns_ast_ctx *ctx);
bool ns_parse_postfix_expr(ns_ast_ctx *ctx);
bool ns_parse_expr_stack(ns_ast_ctx *ctx);

// dump fn
void ns_ast_ctx_dump(ns_ast_ctx *ctx);

// main parse fn
bool ns_ast_parse(ns_ast_ctx *ctx, ns_str source, ns_str filename);