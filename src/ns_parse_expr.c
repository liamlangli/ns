#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"

#include <assert.h>

void ns_parse_stack_push(ns_parse_context_t *ctx, ns_ast_t n) {
    ctx->stack[++ctx->top] = ns_ast_push(ctx, n);
}

bool ns_token_is_operator(ns_token_t token) {
    switch (token.type)
    {
    case NS_TOKEN_ADDITIVE_OPERATOR:
    case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
    case NS_TOKEN_BITWISE_OPERATOR:
    case NS_TOKEN_SHIFT_OPERATOR:
    case NS_TOKEN_RELATIONAL_OPERATOR:
    case NS_TOKEN_EQUALITY_OPERATOR:
    case NS_TOKEN_LOGICAL_OPERATOR:
        return true;
    default:
        return false;
    }
}

bool ns_parse_expr_rewind(ns_parse_context_t *ctx, int expr_top) {
    // rewind
    int top = ctx->top;
    int root = ctx->stack[top];
    while (top > expr_top + 1) {
        int op = ctx->stack[top - 1];
        ns_ast_t *op_n = &ctx->nodes[op];
        op_n->binary_expr.left = ctx->stack[top - 2];
        op_n->binary_expr.right = ctx->stack[top];
        ctx->stack[top - 2] = op;
        top -= 2;
        root = op;
    }

    if (top != expr_top + 1) {
        fprintf(stderr, "Rewind failed\n");
        assert(false);
    }

    ctx->top = expr_top;
    ctx->current = root;
    return true;
}

bool ns_parse_stack_top_is_operator(ns_parse_context_t *ctx) {
    if (ctx->top == -1) return false; // empty stack
    ns_ast_t n = ctx->nodes[ctx->stack[ctx->top]];
    return n.type == NS_AST_BINARY_EXPR;
}

bool ns_parse_stack_top_is_primary_expr(ns_parse_context_t *ctx) {
    if (ctx->top == -1) return false; // empty stack
    ns_ast_t n = ctx->nodes[ctx->stack[ctx->top]];
    return n.type == NS_AST_PRIMARY_EXPR;
}

// stack based expression parser
bool ns_parse_expr_stack(ns_parse_context_t *ctx) {
    int expr_top = ctx->top;
    int state = ns_save_state(ctx);
    do {
        if (!ns_parse_next_token(ctx)) {
            return false;
        }
        ns_token_t token = ctx->token;
        switch (token.type)
        {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLOAT_LITERAL:
        case NS_TOKEN_STRING_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = token}};
            ns_parse_stack_push(ctx, n);
        } break;

        case NS_TOKEN_IDENTIFIER: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = token}};
            ns_parse_stack_push(ctx, n);
        } break;
        
        case NS_TOKEN_ADDITIVE_OPERATOR:
        case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
        case NS_TOKEN_BITWISE_OPERATOR:
        case NS_TOKEN_SHIFT_OPERATOR:
        case NS_TOKEN_RELATIONAL_OPERATOR:
        case NS_TOKEN_EQUALITY_OPERATOR:
        case NS_TOKEN_LOGICAL_OPERATOR: {
            // try rewind
            if (ns_parse_stack_top_is_primary_expr(ctx)) {
                ns_ast_t n = {.type = NS_AST_BINARY_EXPR, .binary_expr = {
                    .op = token
                }};
                ns_parse_stack_push(ctx, n);
            } else {
                fprintf(stderr, "invalid syntax: multi operator\n");
                assert(false);
            }
        } break;
        case NS_TOKEN_OPEN_PAREN: {
            ns_ast_t n = ctx->nodes[ctx->stack[ctx->top]];
            if (n.type == NS_AST_PRIMARY_EXPR) {
                // start call expr
                if (ns_parse_expr_stack(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
                    ns_ast_t expr = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
                } else {
                    
                }
            }

        } break;

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

        case NS_TOKEN_EOL: {
            int eol_state = ns_save_state(ctx);
            ns_parse_next_token(ctx);
            if (ns_token_is_operator(ctx->token) || 
                ctx->token.type == NS_TOKEN_CLOSE_PAREN ||
                ctx->token.type == NS_TOKEN_OPEN_PAREN) {
                ns_restore_state(ctx, eol_state);
                continue;
            } else {
                ns_restore_state(ctx, eol_state);
                if (ctx->top > expr_top && ns_parse_expr_rewind(ctx, expr_top)) {
                    return true;
                } else {
                    return false;
                }
            }
        } break;
        default:
            break;
        }

    } while (ctx->top >= expr_top);
    return false;
}