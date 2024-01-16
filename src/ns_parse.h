#pragma one

#ifdef __cplusplus
extern "C" {
#endif

#include "ns_tokenize.h"

typedef enum {
    NS_AST_PROGRAM,
    NS_AST_FUNCTION_DEF,
    NS_AST_VARIABLE_DEF,
    NS_AST_BINARY_EXPR,
    NS_AST_LITERAL,
    NS_AST_IDENTIFIER,
    NS_AST_CALL_EXPR,
    NS_AST_IF_STMT,
    NS_AST_ITER_STMT,
    NS_AST_RETURN_STMT,
} NS_AST_TYPE;

typedef struct ns_ast_t ns_ast_t;

typedef struct ns_ast_program {
    ns_ast_t **body;
    int body_len;
} ns_ast_program;

typedef struct ns_ast_fn_def {
    ns_ast_t *name;
    ns_ast_t *body;
    ns_ast_t **params;
    int param_count;
} ns_ast_fn_def;

typedef struct ns_ast_var_def {
    ns_ast_t *name;
    ns_ast_t *declarations;
    ns_token_t *type;
} ns_ast_var_def;

typedef struct ns_ast_struct_def {
    ns_ast_t *name;
    ns_ast_t **fields;
    int field_count;
} ns_ast_struct_def;

typedef struct ns_ast_binary_expr {
    ns_ast_t *left;
    ns_ast_t *right;
    ns_token_t *op;
} ns_ast_binary_expr;

typedef struct ns_ast_literal {
    ns_token_t *token;
} ns_ast_literal;

typedef struct ns_ast_identifier {
    ns_token_t *token;
} ns_ast_identifier;

typedef struct ns_ast_call_expr {
    ns_ast_t *callee;
    ns_ast_t **args;
    int arg_count;
} ns_ast_call_expr;

typedef struct ns_ast_if_stmt {
    ns_ast_t *condition;
    ns_ast_t *body;
    ns_ast_t *else_body;
} ns_ast_if_stmt;

typedef struct ns_ast_iter_stmt {
    ns_ast_t *init;
    ns_ast_t *condition;
    ns_ast_t *update;
    ns_ast_t *body;
} ns_ast_iter_stmt;

typedef struct ns_ast_return_stmt {
    ns_ast_t *value;
} ns_ast_return_stmt;

typedef struct ns_ast_t {
    NS_AST_TYPE type;
    ns_token_t *token;
    union {
        ns_ast_program program;
        ns_ast_fn_def fn_def;
        ns_ast_var_def var_def;
        ns_ast_struct_def struct_def;
        ns_ast_binary_expr binary_expr;
        ns_ast_literal literal;
        ns_ast_identifier identifier;
        ns_ast_call_expr call_expr;
        ns_ast_if_stmt if_stmt;
        ns_ast_iter_stmt iter_stmt;
        ns_ast_return_stmt return_stmt;
    };
} ns_ast_t;

typedef struct as_parse_context_t {
    int f, last_f;
    ns_ast_t *program;
    ns_token_t *token;
    const char *source;
    const char *filename;
} ns_parse_context_t;

ns_ast_t* ns_parse(const char *source, const char *filename);

#ifdef __cplusplus
} // extern "C"
#endif