#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"

void ns_parse_stack_push_index(ns_parse_context_t *ctx, int index) { ctx->stack[++ctx->top] = index; }

void ns_parse_stack_push(ns_parse_context_t *ctx, ns_ast_t n) { ns_parse_stack_push_index(ctx, ns_ast_push(ctx, n)); }

int ns_parse_stack_pop(ns_parse_context_t *ctx) { return ctx->stack[ctx->top--]; }

ns_ast_t ns_parse_stack_top(ns_parse_context_t *ctx) { return ctx->nodes[ctx->stack[ctx->top]]; }

bool ns_parse_primary_expr(ns_parse_context_t *ctx);
bool ns_parse_postfix_expr(ns_parse_context_t *ctx);
bool ns_parse_unary_expr(ns_parse_context_t *ctx);
bool ns_parse_expr_stack(ns_parse_context_t *ctx);

bool ns_parse_stack_top_is_operator(ns_parse_context_t *ctx) {
    if (ctx->top == -1)
        return false; // empty stack
    ns_ast_t n = ns_parse_stack_top(ctx);
    return n.type == NS_AST_BINARY_EXPR;
}

bool ns_parse_stack_top_is_operand(ns_parse_context_t *ctx) {
    if (ctx->top == -1)
        return false; // empty stack
    ns_ast_t n = ns_parse_stack_top(ctx);
    switch (n.type) {
    case NS_AST_PRIMARY_EXPR:
    case NS_AST_CALL_EXPR:
    case NS_AST_MEMBER_EXPR:
    case NS_AST_CAST_EXPR:
    case NS_AST_INDEX_EXPR:
    case NS_AST_UNARY_EXPR:
    case NS_AST_EXPR:
        return true;
    default:
        return false;
    }
}

bool ns_token_is_operator(ns_token_t token) {
    switch (token.type) {
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
        ns_parse_error(ctx, "syntax error: expr stack rewind failed.");
    }

    ctx->top = expr_top;
    ctx->current = root;
    return true;
}

bool ns_parse_call_expr(ns_parse_context_t *ctx) {
    ns_ast_t n = {.type = NS_AST_CALL_EXPR, .call_expr = {.arg_count = 0}};
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
            ns_parse_error(ctx, "syntax error: expected ',' or ')'");
        }
    }

    if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
        ns_parse_error(ctx, "syntax error: expected ')'");
    }

    ns_ast_push(ctx, n);
    return true;
}

bool ns_parse_primary_expr(ns_parse_context_t *ctx) {
    ns_parse_state_t state = ns_save_state(ctx);
    if (ns_parse_next_token(ctx)) {
        switch (ctx->token.type) {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLOAT_LITERAL:
        case NS_TOKEN_STRING_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_ast_push(ctx, n);
            return true;
        } break;

        case NS_TOKEN_IDENTIFIER: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_ast_push(ctx, n);
            return true;
        } break;

        default:
            ns_restore_state(ctx, state);
            return false;
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_postfix_expr(ns_parse_context_t *ctx) {
    ns_parse_state_t primary_state = ns_save_state(ctx);
    if (!ns_parse_primary_expr(ctx)) {
        ns_restore_state(ctx, primary_state);
        return false;
    }

    ns_token_t token = ctx->token;
    int callee = ctx->current;
    ns_parse_state_t state = ns_save_state(ctx);
    // parse postfix '(' [expr]* [,expr]* ')'
    if (ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_token_skip_eol(ctx);
        if (ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
            ns_ast_t n = {.type = NS_AST_CALL_EXPR, .call_expr = {.callee = callee, .arg_count = 0}};
            ns_ast_push(ctx, n);
            return true;
        }

        ns_ast_t n = {.type = NS_AST_CALL_EXPR, .call_expr = {.callee = callee, .arg_count = 0}};
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
                ns_parse_error(ctx, "syntax error: expected ',' or ')'");
            }
        }

        if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_parse_error(ctx, "syntax error: expected ')'");
        }
    }

    //  parse postfix '[' expr ']'
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACKET)) {
        if (ns_parse_expr_stack(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACKET)) {
            ns_ast_t n = {.type = NS_AST_INDEX_EXPR, .index_expr = {.table = callee, .expr = ctx->current}};
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_parse_error(ctx, "syntax error: expected expression after '['");
        }
    }

    // parse postfix '.' identifier
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_DOT)) {
        if (ns_token_require(ctx, NS_TOKEN_IDENTIFIER)) {
            ns_ast_t n = {.type = NS_AST_MEMBER_EXPR, .member_expr = {.left = callee, .right = ctx->token}};
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_parse_error(ctx, "syntax error: expected identifier after '.'");
        }
    }

    // parse postfix 'as' type
    ns_restore_state(ctx, state);
    ns_token_skip_eol(ctx);
    if (ns_token_require(ctx, NS_TOKEN_AS)) {
        ns_token_skip_eol(ctx);
        if (ns_token_require_type(ctx)) {
            ns_ast_t n = {.type = NS_AST_CAST_EXPR, .type_cast = {.expr = callee, .type = ctx->token}};
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_parse_error(ctx, "syntax error: expected type after 'as'");
        }
    }

    // only primary expr
    ns_restore_state(ctx, state);
    return true;
}

bool ns_parse_unary_expr(ns_parse_context_t *ctx) {
    ns_parse_state_t state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_BIT_INVERT_OPERATOR ||
        (ctx->token.type == NS_TOKEN_ADDITIVE_OPERATOR && ns_str_equals_STR(ctx->token.val, "-"))) {

        ns_ast_t n = {.type = NS_AST_UNARY_EXPR, .unary_expr = {.op = ctx->token}};
        if (ns_parse_postfix_expr(ctx)) {
            n.unary_expr.expr = ctx->current;
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_parse_error(ctx, "syntax error: expected expression after unary operator");
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

// stack based expression parser
bool ns_parse_expr_stack(ns_parse_context_t *ctx) {
    int expr_top = ++ctx->top;
    ns_parse_state_t state;
    do {
        state = ns_save_state(ctx);
        if (!ns_parse_next_token(ctx)) {
            if (ctx->token.type == NS_TOKEN_EOF) {
                goto rewind;
            }
            return false;
        }

        switch (ctx->token.type) {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLOAT_LITERAL:
        case NS_TOKEN_STRING_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_parse_stack_push(ctx, n);
        } break;

        case NS_TOKEN_IDENTIFIER: {
            if (ns_parse_stack_top_is_operand(ctx) && ctx->top > expr_top) {
                goto rewind;
            }

            ns_restore_state(ctx, state);
            if (!ns_parse_postfix_expr(ctx)) {
                ns_parse_error(ctx, "syntax error: expected postfix expression");
            }
            ns_parse_stack_push_index(ctx, ctx->current);
        } break;

        case NS_TOKEN_ADDITIVE_OPERATOR:
        case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
        case NS_TOKEN_BITWISE_OPERATOR:
        case NS_TOKEN_SHIFT_OPERATOR:
        case NS_TOKEN_RELATIONAL_OPERATOR:
        case NS_TOKEN_EQUALITY_OPERATOR:
        case NS_TOKEN_BOOL_OPERATOR:
        case NS_TOKEN_LOGICAL_OPERATOR: {

            // first token is operator
            if (ctx->top == expr_top) {
                ns_restore_state(ctx, state);
                if (ns_parse_unary_expr(ctx)) {
                    ns_parse_stack_push_index(ctx, ctx->current);
                    break;
                } else {
                    ns_parse_error(ctx, "syntax error: expected unary expression");
                }
            }

            // try rewind
            if (ns_parse_stack_top_is_operand(ctx)) {
                ns_ast_t n = {.type = NS_AST_BINARY_EXPR, .binary_expr = {.op = ctx->token}};
                ns_parse_stack_push(ctx, n);
            } else {
                ns_parse_error(ctx, "syntax error: multi operator");
            }
        } break;
        case NS_TOKEN_OPEN_PAREN: {
            if (ns_parse_stack_top_is_operator(ctx) || ctx->top == expr_top) {
                // parse inner expr
                if (ns_parse_expr_stack(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
                    ns_ast_t expr = {.type = NS_AST_EXPR, .expr = {.body = ctx->current}};
                    ns_parse_stack_push(ctx, expr);
                    break;
                } else {
                    ns_parse_error(ctx, "syntax error: expected inner expression after '('");
                }
            } else {
                int callee = ns_parse_stack_pop(ctx);
                if (!ns_parse_call_expr(ctx)) {
                    ns_parse_error(ctx, "syntax error: expected call expression after '('");
                }
                ns_ast_t *call = &ctx->nodes[ctx->current];
                call->call_expr.callee = callee;
                ns_parse_stack_push_index(ctx, ctx->current);
                break;
            }

        } break;

        case NS_TOKEN_OPEN_BRACE: {
            // check if leading token is identifier
            if (ns_parse_stack_top_is_operand(ctx)) {
                int last = ns_parse_stack_pop(ctx);
                ns_ast_t type = ctx->nodes[last];
                ns_restore_state(ctx, state);
                if (!ns_parse_designated_stmt(ctx)) {
                    ns_parse_stack_push_index(ctx, last);
                    goto rewind;
                }
                ns_ast_t *n = &ctx->nodes[ctx->current];
                n->designated_stmt.name = type.primary_expr.token;

                ns_parse_stack_push_index(ctx, ctx->current);
            }
        } break;

        case NS_TOKEN_CLOSE_BRACE:
        case NS_TOKEN_CLOSE_PAREN:
        case NS_TOKEN_RETURN:
        case NS_TOKEN_LET: {
            goto rewind;
        } break;

        case NS_TOKEN_COMMA:
        case NS_TOKEN_EOL: {
            ns_parse_next_token(ctx);
            if (ns_token_is_operator(ctx->token) || ctx->token.type == NS_TOKEN_CLOSE_PAREN ||
                ctx->token.type == NS_TOKEN_OPEN_PAREN) {
                ns_restore_state(ctx, state);
                break;
            }
            goto rewind;
        } break;

        case NS_TOKEN_ASSIGN:
        case NS_TOKEN_ASSIGN_OPERATOR: {
            int left = ns_parse_stack_pop(ctx);
            if (ns_parse_expr_stack(ctx)) {
                ns_ast_t n = {.type = NS_AST_BINARY_EXPR, .binary_expr = {.op = ctx->token}};
                n.binary_expr.left = left;
                n.binary_expr.right = ctx->current;
                ns_parse_stack_push(ctx, n);
            } else {
                ns_parse_error(ctx, "syntax error: expected expression after assign operator");
            }
        } break;
        default:
            ns_parse_error(ctx, "syntax error: unexpected token type in expr stack parser.");
            break;
        }

    } while (ctx->top >= expr_top);
rewind:
    ns_restore_state(ctx, state);
    if (ctx->top > expr_top && ns_parse_expr_rewind(ctx, expr_top)) {
        goto success;
    } else {
        if (ctx->top == expr_top) {
            ns_ast_push(ctx, (ns_ast_t){.type = NS_AST_COMPOUND_STMT, .compound_stmt.count = 0});
            goto success; // empty compound stmt
        }
        ns_parse_error(ctx, "syntax error: invalid expression before EOL");
    }
failed:
    return false;

success:
    ctx->top = expr_top - 1;
    return true;
}