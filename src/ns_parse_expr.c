#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"
#include <stdbool.h>

void ns_parse_stack_push_index(ns_parse_context_t *ctx, int index) {
    ctx->stack[++ctx->top] = index;
}

void ns_parse_stack_push(ns_parse_context_t *ctx, ns_ast_t n) {
    ns_parse_stack_push_index(ctx, ns_ast_push(ctx, n));
}

int ns_parse_stack_pop(ns_parse_context_t *ctx) {
    return ctx->stack[ctx->top--];
}

ns_ast_t ns_parse_stack_top(ns_parse_context_t *ctx) {
    return ctx->nodes[ctx->stack[ctx->top]];
}

bool ns_parse_stack_top_is_operator(ns_parse_context_t *ctx) {
    if (ctx->top == -1) return false; // empty stack
    ns_ast_t n = ns_parse_stack_top(ctx);
    return n.type == NS_AST_BINARY_EXPR;
}

bool ns_parse_stack_top_is_operand(ns_parse_context_t *ctx) {
    if (ctx->top == -1) return false; // empty stack
    ns_ast_t n = ns_parse_stack_top(ctx);
    return n.type == NS_AST_PRIMARY_EXPR || n.type == NS_AST_CALL_EXPR || n.type == NS_AST_MEMBER_EXPR || n.type == NS_AST_CAST_EXPR;
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
        ns_parse_dump_error(ctx, "Rewind failed.");
    }

    ctx->top = expr_top;
    ctx->current = root;
    return true;
}

bool ns_parse_call_expr(ns_parse_context_t *ctx) {
    ns_ast_t n = {.type = NS_AST_CALL_EXPR, .call_expr = { .arg_count = 0 }};
    if (ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {

        ns_ast_push(ctx, n);
        return true;
    }

    ns_ast_t *last = &n;
    while (ns_parse_expr_stack(ctx)) {
        last->next = ctx->current;
        last = &ctx->nodes[ctx->current];
        n.call_expr.arg_count++;

        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA) {
            continue;
        } else if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
            break;
        } else {
            ns_parse_dump_error(ctx, "syntax error: expected ',' or ')'");
        }
    }

    if (ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        ns_parse_dump_error(ctx, "syntax error: expected ')'");
    }

    ns_ast_push(ctx, n);
    return true;
}

// stack based expression parser
bool ns_parse_expr_stack(ns_parse_context_t *ctx) {
    int expr_top = ctx->top;
    int state;
    do {
        state = ns_save_state(ctx);
        if (!ns_parse_next_token(ctx)) {
            return false;
        }

        switch (ctx->token.type)
        {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLOAT_LITERAL:
        case NS_TOKEN_STRING_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_parse_stack_push(ctx, n);
        } break;

        case NS_TOKEN_IDENTIFIER: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_parse_stack_push(ctx, n);
        } break;
        
        case NS_TOKEN_ADDITIVE_OPERATOR:
        case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
        case NS_TOKEN_BITWISE_OPERATOR:
        case NS_TOKEN_SHIFT_OPERATOR:
        case NS_TOKEN_RELATIONAL_OPERATOR:
        case NS_TOKEN_EQUALITY_OPERATOR:
        case NS_TOKEN_BOOL_OPERATOR:
        case NS_TOKEN_LOGICAL_OPERATOR: {
            // try rewind
            if (ns_parse_stack_top_is_operand(ctx)) {
                ns_ast_t n = {.type = NS_AST_BINARY_EXPR, .binary_expr = {
                    .op = ctx->token
                }};
                ns_parse_stack_push(ctx, n);
            } else {
                ns_parse_dump_error(ctx, "syntax error: multi operator");
            }
        } break;
        case NS_TOKEN_OPEN_PAREN: {
            if (ns_parse_stack_top_is_operator(ctx)) {
                // parse inner expr
                if (ns_parse_expr_stack(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
                    ns_ast_t expr = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
                } else {
                    ns_parse_dump_error(ctx, "syntax error: expected inner expression after '('");
                }
            } else {
                int callee = ns_parse_stack_pop(ctx);
                if (!ns_parse_call_expr(ctx)) {
                    ns_parse_dump_error(ctx, "syntax error: expected call expression after '('");
                }
                ns_ast_t *call = &ctx->nodes[ctx->current];
                call->call_expr.callee = callee;
                ns_parse_stack_push_index(ctx, ctx->current);
                break;
            }

        } break;

        case NS_TOKEN_DOT: {
            // push to stack
            if (!ns_token_require(ctx, NS_TOKEN_IDENTIFIER)) {
                ns_parse_dump_error(ctx, "syntax error: expected identifier after '.'");
            }

            // rewind member access
            ns_ast_t n = {.type = NS_AST_MEMBER_EXPR, .member_expr = {
                .left = ns_parse_stack_pop(ctx),
                .right = ctx->token}
            };

            ns_parse_stack_push(ctx, n);
        } break;

        case NS_TOKEN_AS: {
            // push to stack
            if (!ns_token_require(ctx, NS_TOKEN_TYPE)) {
                ns_parse_dump_error(ctx, "syntax error: expected type after 'as'");
            }

            // rewind type cast
            ns_ast_t n = {.type = NS_AST_CAST_EXPR, .type_cast = {
                .expr = ns_parse_stack_pop(ctx),
                .type = ctx->token}
            };
            ns_parse_stack_push(ctx, n);
        } break;

        case NS_TOKEN_OPEN_BRACE: {
            // check if leading token is identifier
            if (ns_parse_stack_top_is_operand(ctx)) {
                ns_ast_t type = ctx->nodes[ns_parse_stack_pop(ctx)];
                ns_restore_state(ctx, state); // pop
                if (!ns_parse_designated_stmt(ctx)) {
                    ns_parse_dump_error(ctx, "syntax error: expected designated stmt");
                }
                ns_ast_t* n = &ctx->nodes[ctx->current];
                n->designated_stmt.name = type.primary_expr.token;

                ns_parse_stack_push_index(ctx, ctx->current);
            }
        } break;

        case NS_TOKEN_CLOSE_BRACE:
        case NS_TOKEN_CLOSE_PAREN:
        case NS_TOKEN_LET:
        {
            // push to stack
            if (ctx->top > expr_top && ns_parse_expr_rewind(ctx, expr_top)) {
                ns_restore_state(ctx, state);
                return true;
            } else {
                ns_parse_dump_error(ctx, "syntax error: invalid expression before '{'");
            }
        } break;

        case NS_TOKEN_COMMA:
        case NS_TOKEN_EOL: {
            ns_parse_next_token(ctx);
            if (ns_token_is_operator(ctx->token) || 
                ctx->token.type == NS_TOKEN_CLOSE_PAREN ||
                ctx->token.type == NS_TOKEN_OPEN_PAREN) {
                ns_restore_state(ctx, state);
                continue;
            } else {
                ns_restore_state(ctx, state);
                if (ctx->top > expr_top && ns_parse_expr_rewind(ctx, expr_top)) {
                    return true;
                } else {
                    if (ctx->top == expr_top) {
                        ns_ast_push(ctx, (ns_ast_t){ .type= NS_AST_COMPOUND_STMT, .compound_stmt.count = 0 });
                        return true; // empty compound stmt
                    }
                    ns_parse_dump_error(ctx, "syntax error: invalid expression before EOL");
                }
            }
        } break;
        default:
            ns_parse_dump_error(ctx, "fatal error: unimplemented token type in expr stack parser.");
            break;
        }

    } while (ctx->top >= expr_top);
    return false;
}