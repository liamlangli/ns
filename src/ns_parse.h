#pragma once

#include "ns_tokenize.h"

typedef enum {
    NS_AST_UNKNOWN = 0,
    NS_AST_PROGRAM,
    NS_AST_LITERAL,
    NS_AST_IDENTIFIER,
    NS_AST_PARAM,

    NS_AST_FN_DEF,
    NS_AST_VAR_DEF,
    NS_AST_STRUCT_DEF,
    NS_AST_TYPE_DEF,

    NS_AST_VAR_ASSIGN,

    NS_AST_BINARY_EXPR,
    NS_AST_MEMBER_EXPR,
    NS_AST_CALL_EXPR,
    NS_AST_CAST_EXPR,

    NS_AST_GENERATOR_EXPR,

    NS_AST_IF_STMT,
    NS_AST_ITER_STMT,
    NS_AST_RETURN_STMT,
    NS_AST_JUMP_STMT,
    NS_AST_LABELED_STMT
} NS_AST_TYPE;

#define MAX_PARAMS 16
#define MAX_FIELDS 32
#define MAX_PARSE_STACK 64

typedef struct ns_ast_t ns_ast_t;

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
    int params[MAX_PARAMS];
    int param_count;
} ns_ast_fn_def;

typedef struct ns_ast_var_def {
    int declarations;
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
    int fields[MAX_FIELDS];
    int field_count;
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

typedef struct ns_ast_literal {
    ns_token_t token;
} ns_ast_literal;

typedef struct ns_ast_identifier {
    ns_token_t token;
} ns_ast_identifier;

typedef struct ns_ast_member_expr {
    int left;
    ns_token_t right;
} ns_ast_member_expr;

typedef struct ns_ast_call_expr {
    int callee;
    int args[MAX_PARAMS];
    int arg_count;
} ns_ast_call_expr;

typedef struct ns_ast_generator_expr {
    int spawn;
    ns_token_t label;
    int from;
    int to;
} ns_ast_generator_expr;

typedef struct ns_ast_if_stmt {
    int condition;
    int body;
    int else_body;
} ns_ast_if_stmt;

typedef struct ns_ast_iter_stmt {
    int condition;
    int generator;
    int body;
} ns_ast_iter_stmt;

typedef struct ns_ast_jump_stmt {
    ns_token_t label;
    int expr;
} ns_ast_jump_stmt;

typedef struct ns_ast_labeled_stmt {
    ns_token_t label;
    int condition;
    int stmt;
} ns_ast_labeled_stmt;

typedef struct ns_ast_t {
    NS_AST_TYPE type;
    union {
        ns_ast_param param;
        ns_ast_literal literal;
        ns_ast_identifier identifier;
        ns_ast_fn_def fn_def;
        ns_ast_var_def var_def;
        ns_ast_struct_def struct_def;

        ns_ast_binary_expr binary_expr;
        ns_ast_call_expr call_expr;
        ns_ast_cast_expr type_cast;
        ns_ast_member_expr member_expr;
        ns_ast_generator_expr generator;

        ns_ast_if_stmt if_stmt;
        ns_ast_iter_stmt iter_stmt;
        ns_ast_jump_stmt jump_stmt;
        ns_ast_labeled_stmt label_stmt;
    };
} ns_ast_t;

typedef struct as_parse_context_t {
    int f, last_f;
    int* sections;
    int current;

    ns_ast_t *nodes;

    int stack[MAX_PARSE_STACK];
    int top;

    ns_token_t token;
    const char *source;
    const char *filename;
} ns_parse_context_t;

ns_parse_context_t* ns_parse(const char *source, const char *filename);
void ns_parse_context_dump(ns_parse_context_t *ctx);

// token func
bool ns_parse_next_token(ns_parse_context_t *ctx); // skip space
bool ns_token_require(ns_parse_context_t *ctx, NS_TOKEN token);

// node func
void ns_restore_state(ns_parse_context_t *ctx, int f);
int ns_save_state(ns_parse_context_t *ctx);
int ns_ast_push(ns_parse_context_t *ctx, ns_ast_t n);

// external func
bool ns_parse_fn_define(ns_parse_context_t *ctx);
bool ns_parse_var_define(ns_parse_context_t *ctx);
bool ns_parse_struct_define(ns_parse_context_t *ctx);
bool ns_parse_type_define(ns_parse_context_t *ctx);

// stmt func
bool ns_parse_external_define(ns_parse_context_t *ctx);
bool ns_parse_stmt(ns_parse_context_t *ctx);

// expr func
bool ns_parse_generator_expr(ns_parse_context_t *ctx);
bool ns_parse_expr(ns_parse_context_t *ctx);
bool ns_parse_constant_expr(ns_parse_context_t *ctx);