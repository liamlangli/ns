#pragma once

#include "ns_tokenize.h"
#include "ns_type.h"

#include <assert.h>

typedef enum {
    NS_AST_UNKNOWN = 0,
    NS_AST_PROGRAM,

    NS_AST_PARAM_DEF,
    NS_AST_FN_DEF,
    NS_AST_OPS_FN_DEF,
    NS_AST_VAR_DEF,
    NS_AST_STRUCT_DEF,
    NS_AST_STRUCT_FIELD_DEF,
    NS_AST_TYPE_DEF,

    NS_AST_VAR_ASSIGN,

    NS_AST_EXPR,
    NS_AST_PRIMARY_EXPR,
    NS_AST_BINARY_EXPR,
    NS_AST_MEMBER_EXPR,
    NS_AST_UNARY_EXPR,
    NS_AST_CALL_EXPR,
    NS_AST_INDEX_EXPR,
    NS_AST_CAST_EXPR,
    NS_AST_GENERATOR_EXPR,
    NS_AST_DESIGNATED_EXPR,

    NS_AST_IF_STMT,
    NS_AST_IMPORT_STMT,
    NS_AST_FOR_STMT,
    NS_AST_WHILE_STMT,
    NS_AST_RETURN_STMT,
    NS_AST_JUMP_STMT,
    NS_AST_LABELED_STMT,
    NS_AST_COMPOUND_STMT,
    NS_AST_DESIGNATED_STMT,
} NS_AST_TYPE;

typedef struct ns_ast_t ns_ast_t;

typedef struct ns_parse_state {
    int f, line;
} ns_parse_state_t;

typedef struct ns_ast_param {
    bool is_ref;
    ns_token_t name;
    ns_token_t type;
} ns_ast_param;

typedef struct ns_ast_fn_def {
    bool is_async;
    ns_token_t name;
    ns_token_t return_type;
    int body;
    int params[NS_MAX_PARAMS];
    int param_count;
} ns_ast_fn_def;

typedef struct ns_ast_ops_fn_def {
    bool is_async;
    ns_token_t ops;
    ns_token_t return_type;
    int body;
    int left;
    int right;
} ns_ast_ops_fn_def;

typedef struct ns_ast_var_def {
    int expr;
    ns_token_t name;
    ns_token_t type;
} ns_ast_var_def;

typedef struct ns_ast_var_assign {
    ns_token_t name;
    int property;
    bool computed;
} ns_ast_var_assign;

typedef struct ns_ast_struct_def {
    ns_token_t name;
    int count;
} ns_ast_struct_def;

typedef struct ns_ast_binary_expr {
    int left;
    int right;
    ns_token_t op;
} ns_ast_binary_expr;

typedef struct ns_ast_cast_expr {
    int expr;
    ns_token_t type;
} ns_ast_cast_expr;

typedef struct ns_ast_primary_expr {
    ns_token_t token;
    int expr;
    int slot; // (0) for global, (< 0) for local, (> 0) for param
} ns_ast_primary_expr;

typedef struct ns_ast_unary_expr {
    ns_token_t op;
    int expr;
} ns_ast_unary_expr;

typedef struct ns_ast_expr {
    int body;
} ns_ast_expr;

typedef struct ns_ast_member_expr {
    int left;
    ns_token_t right;
} ns_ast_member_expr;

typedef struct ns_ast_call_expr {
    int callee;
    int arg_count;
} ns_ast_call_expr;

typedef struct ns_ast_index_expr {
    int table;
    int expr;
} ns_ast_index_expr;

typedef struct ns_ast_generator_expr {
    ns_token_t label;
    int from;
    ns_token_t token;
    int to;
} ns_ast_generator_expr;

typedef struct ns_ast_import_stmt {
    ns_token_t lib;
} ns_ast_import_stmt;

typedef struct ns_ast_if_stmt {
    int condition;
    int body;
    int else_body;
} ns_ast_if_stmt;

typedef struct ns_ast_for_stmt {
    int generator;
    int body;
} ns_ast_for_stmt;

typedef struct ns_ast_while_stmt {
    int condition;
    int body;
} ns_ast_while_stmt;

typedef struct ns_ast_loop_stmt {
    int condition;
    int body;
    bool do_first;
} ns_ast_loop_stmt;

typedef struct ns_ast_jump_stmt {
    ns_token_t label;
    int expr;
} ns_ast_jump_stmt;

typedef struct ns_ast_labeled_stmt {
    ns_token_t label;
    int condition;
    int stmt;
} ns_ast_labeled_stmt;

typedef struct ns_ast_compound_stmt {
    int count;
} ns_ast_compound_stmt;

typedef struct ns_ast_designated_stmt {
    ns_token_t name;
    int count;
} ns_ast_designated_stmt;

typedef struct ns_ast_designated_expr {
    ns_token_t name;
    int expr;
} ns_ast_designated_expr;

typedef struct ns_ast_t {
    NS_AST_TYPE type;
    int next; // -1 mean null ref
    union {
        ns_ast_param param;
        ns_ast_fn_def fn_def;
        ns_ast_ops_fn_def ops_fn_def;
        ns_ast_var_def var_def;
        ns_ast_struct_def struct_def;

        ns_ast_expr expr;
        ns_ast_binary_expr binary_expr;
        ns_ast_call_expr call_expr;
        ns_ast_index_expr index_expr;
        ns_ast_cast_expr type_cast;
        ns_ast_member_expr member_expr;
        ns_ast_primary_expr primary_expr;
        ns_ast_unary_expr unary_expr;
        ns_ast_generator_expr generator;
        ns_ast_designated_expr designated_expr;

        ns_ast_import_stmt import_stmt;
        ns_ast_if_stmt if_stmt;
        ns_ast_while_stmt while_stmt;
        ns_ast_for_stmt for_stmt;
        ns_ast_jump_stmt jump_stmt;
        ns_ast_labeled_stmt label_stmt;
        ns_ast_compound_stmt compound_stmt;
        ns_ast_designated_stmt designated_stmt;
    };
} ns_ast_t;

typedef struct as_parse_context_t {
    int f, last_f;
    int sections[NS_MAX_SECTION_COUNT];
    int section_count;
    int current;

    ns_ast_t nodes[NS_MAX_NODE_COUNT];
    int node_count;

    int stack[NS_MAX_PARSE_STACK];
    int top;

    ns_token_t token, last_token;
    const char *source;
    const char *filename;
} ns_parse_context_t;

const char * ns_ast_type_str(NS_AST_TYPE type);
void ns_parse_context_dump(ns_parse_context_t *ctx);

// token func
bool ns_parse_next_token(ns_parse_context_t *ctx); // skip space
bool ns_token_require(ns_parse_context_t *ctx, NS_TOKEN token);
void ns_token_skip_eol(ns_parse_context_t *ctx);

// node func
void ns_restore_state(ns_parse_context_t *ctx, ns_parse_state_t state);
ns_parse_state_t ns_save_state(ns_parse_context_t *ctx);
int ns_ast_push(ns_parse_context_t *ctx, ns_ast_t n);

// primary func
bool ns_parse_identifier(ns_parse_context_t *ctx);

// external func
bool ns_parse_fn_define(ns_parse_context_t *ctx);
bool ns_parse_ops_fn_define(ns_parse_context_t *ctx);
bool ns_parse_var_define(ns_parse_context_t *ctx);
bool ns_parse_struct_define(ns_parse_context_t *ctx);
bool ns_parse_type_define(ns_parse_context_t *ctx);

// stmt func
bool ns_parse_external_define(ns_parse_context_t *ctx);
bool ns_parse_stmt(ns_parse_context_t *ctx);
bool ns_parse_compound_stmt(ns_parse_context_t *ctx);
bool ns_parse_designated_stmt(ns_parse_context_t *ctx);

// expr func
ns_ast_t ns_parse_stack_top(ns_parse_context_t *ctx);
bool ns_parse_generator_expr(ns_parse_context_t *ctx);
bool ns_parse_expr_stack(ns_parse_context_t *ctx);
bool ns_parse_constant_expr(ns_parse_context_t *ctx);

ns_parse_context_t* ns_parse(const char *source, const char *filename);