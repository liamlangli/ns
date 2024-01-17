#include "ns_parse.h"
#include "ns_type.h"

#include <assert.h>

// LALR parser for the language
bool ns_parse_expr_lalr(ns_parse_context_t *ctx) {
    int from = ctx->top;
    do {
        if (ns_parse_next_token(ctx)) return false;
        ns_token_t token = ctx->token;
        switch (token.type)
        {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLOAT_LITERAL:
        case NS_TOKEN_STRING_LITERAL: {
            ns_ast_t n = {.type = NS_AST_LITERAL, .literal = {.token = token}};
            ctx->stack[ctx->top++] = ns_ast_push(ctx, n);
        } break;

        case NS_TOKEN_IDENTIFIER:

            break;
        
        case NS_TOKEN_ADDITIVE_OPERATOR:
        case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
        case NS_TOKEN_BITWISE_OPERATOR:
        case NS_TOKEN_SHIFT_OPERATOR:
        case NS_TOKEN_RELATIONAL_OPERATOR:
        case NS_TOKEN_EQUALITY_OPERATOR:
        case NS_TOKEN_LOGICAL_OPERATOR:
            // try rewind
            break;

        case NS_TOKEN_OPEN_PAREN:
            // push to stack
            // ctx->stack[ctx->top++] = ;
            break;

        case NS_TOKEN_CLOSE_PAREN:
            // pop stack
            break;
        
        case NS_TOKEN_DOT: {
            // push to stack
            if (!ns_token_require(ctx, NS_TOKEN_IDENTIFIER)) {
                fprintf(stderr, "Expected identifier after '.'\n");
                assert(false);
            }

            // rewind member access
            ns_ast_t n = {.type = NS_AST_MEMBER_EXPR, .member_expr = {
                .left = ctx->stack[--ctx->top],
                .right = ctx->token}
            };

            ctx->stack[ctx->top++] = ns_ast_push(ctx, n);
        } break;

        case NS_TOKEN_AS: {
            // push to stack
            if (!ns_token_require(ctx, NS_TOKEN_TYPE)) {
                fprintf(stderr, "Expected type after 'as'\n");
                assert(false);
            }

            // rewind type cast
            ns_ast_t n = {.type = NS_AST_CAST_EXPR, .type_cast = {
                .expr = ctx->stack[--ctx->top],
                .type = ctx->token}
            };
            ctx->stack[ctx->top++] = ns_ast_push(ctx, n);
        } break;

        

        default:
            break;
        }

    } while (ctx->top <= from);

    return false;
}