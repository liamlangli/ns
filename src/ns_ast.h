#pragma one

#ifdef __cplusplus
extern "C" {
#endif

#include "ns_tokenize.h"

typedef struct ns_statement_t {

} ns_statement_t;

typedef struct ns_expression_t {

} ns_expression_t;

typedef struct ns_function_t {} ns_function_t;

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