#pragma one

#ifdef __cplusplus
extern "C" {
#endif

#include "ns_tokenize.h"

typedef struct ns_statement_t {
    ns_token_t *token;
    struct ns_statement_t *next;
    struct ns_statement_t *prev;
    struct ns_statement_t *child;
} ns_statement_t;

typedef struct ns_expression_t {
    ns_token_t *token;
    struct ns_expression_t *left;
    struct ns_expression_t *right;
} ns_expression_t;

typedef struct ns_function_t {
    ns_token_t *token;
    struct ns_expression_t *args;
    struct ns_statement_t *body;
} ns_function_t;

typedef struct ns_ast_t {
    union {
        ns_statement_t statement;
        ns_expression_t expression;
        ns_function_t function;
    };
    ns_token_t *token;
} ns_ast_t;

ns_ast_t* ns_ast(ns_token_t *token, int from);

#ifdef __cplusplus
} // extern "C"
#endif