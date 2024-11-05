#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"

bool ns_parse_stack_push_operand(ns_ast_ctx *ctx, i32 i) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    scope->pre = i;
    ns_array_push(ctx->stack, i);
    return true;
}

bool ns_parse_stack_push_operator(ns_ast_ctx *ctx, i32 i) {
    ns_ast_t n = ctx->nodes[i];
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    scope->pre = i;

    i32 op_len = ns_array_length(ctx->op_stack);
    if (op_len <= scope->op_top) { // empty
        ns_array_push(ctx->op_stack, i);
    } else {
        i32 top = ctx->op_stack[op_len - 1];
        if (n.binary_expr.op.type > ctx->nodes[top].binary_expr.op.type) {
            ns_array_push(ctx->op_stack, i);
        } else {
            ns_array_push(ctx->stack, ns_array_pop(ctx->op_stack));
        }
    }
    return true;
}

i32 ns_parse_stack_pop(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    if (scope->stack_top >= (i32)ns_array_length(ctx->stack)) {
        return -1;
    }
    return ns_array_pop(ctx->stack);
}

bool ns_parse_is_operand(ns_ast_t n) {
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

bool ns_parse_stack_leading_operand(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    if (scope->pre == -1) {
        return false;
    }
    return ns_parse_is_operand(ctx->nodes[scope->pre]);
}

bool ns_parse_stack_leading_operator(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    if (scope->pre == -1) {
        return false;
    }
    return !ns_parse_is_operand(ctx->nodes[scope->pre]);
}

bool ns_token_is_operator(ns_token_t token) {
    switch (token.type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_BITWISE_OP:
    case NS_TOKEN_SHIFT_OP:
    case NS_TOKEN_REL_OP:
    case NS_TOKEN_EQ_OP:
    case NS_TOKEN_LOGIC_OP:
        return true;
    default:
        return false;
    }
}

// Shunting Yard Algorithm
bool ns_parse_expr_rewind(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);

    i32 op_len = ns_array_length(ctx->op_stack);
    for (i32 i = scope->op_top; i < op_len; ++i) {
        i32 op = ns_array_pop(ctx->op_stack);
        ns_array_push(ctx->stack, op);
    }

    i32 len = ns_array_length(ctx->stack);
    for (i32 i = scope->stack_top; i < len; ++i) {
        i32 o = ctx->stack[i];
        if (ns_parse_is_operand(ctx->nodes[o])) {
            ns_array_push(ctx->expr_stack, o);
        } else {
            i32 right = ns_array_pop(ctx->expr_stack);
            i32 left = ns_array_pop(ctx->expr_stack);
            ns_ast_t *n = &ctx->nodes[o];
            n->binary_expr.left = left;
            n->binary_expr.right = right;
            ns_array_push(ctx->expr_stack, o);
        }
    }
    ctx->current = ns_array_pop(ctx->expr_stack);
    return true;
}

bool ns_parse_call_expr(ns_ast_ctx *ctx, int callee) {
    ns_ast_t n = {.type = NS_AST_CALL_EXPR, .call_expr = { .callee = callee, .arg_count = 0}};
    if (ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        ns_ast_push(ctx, n);
        return true;
    }

    i32 next = -1;
    while (ns_parse_expr(ctx)) {
        next = next == -1 ? n.call_expr.arg = ctx->current : (ctx->nodes[next].next = ctx->current);
        n.call_expr.arg_count++;

        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA) {
            continue;
        } else if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
            break;
        } else {
            ns_ast_error(ctx, "syntax error", "expected ',' or ')'");
        }
    }

    if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
        ns_ast_error(ctx, "syntax error", "expected ')'");
    }

    ns_ast_push(ctx, n);
    return true;
}

bool ns_parse_str_format(ns_ast_ctx *ctx) {
    ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
    // parse format string
    ns_ast_push(ctx, n);
    return true;
}

bool ns_parse_primary_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_parse_next_token(ctx)) {
        switch (ctx->token.type) {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLT_LITERAL:
        case NS_TOKEN_STR_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_ast_push(ctx, n);
            return true;
        } break;
        case NS_TOKEN_STR_FORMAT: return ns_parse_str_format(ctx);
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

bool ns_parse_postfix_expr(ns_ast_ctx *ctx) {
    ns_ast_state primary_state = ns_save_state(ctx);
    if (!ns_parse_primary_expr(ctx)) {
        ns_restore_state(ctx, primary_state);
        return false;
    }

    
    i32 callee = ctx->current;
    ns_ast_state state = ns_save_state(ctx);

    // parse postfix 'as' type
    ns_token_skip_eol(ctx);
    if (ns_token_require(ctx, NS_TOKEN_AS)) {
        ns_token_skip_eol(ctx);
        if (ns_token_require_type(ctx)) {
            ns_ast_t n = {.type = NS_AST_CAST_EXPR, .cast_expr = {.expr = callee, .type = ctx->token}};
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_ast_error(ctx, "syntax error", "expected type after 'as'");
        }
    }

    // parse postfix '(' [expr]* [,expr]* ')'
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        if (ns_parse_call_expr(ctx, callee)) {
            return true;
        } else {
            ns_ast_error(ctx, "syntax error", "expected call expression after '('");
        }
    }

    // parse postfix { [a: expr]*, [b: expr]* }
    ns_restore_state(ctx, state);
    if (ns_parse_desig_expr(ctx)) {
        return true;
    }

    //  parse postfix '[' expr ']'
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACKET)) {
        if (ns_parse_expr(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACKET)) {
            ns_ast_t n = {.type = NS_AST_INDEX_EXPR, .index_expr = {.table = callee, .expr = ctx->current}};
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_ast_error(ctx, "syntax error", "expected expression after '['");
        }
    }

    // parse postfix '.' identifier
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_DOT)) {
        ns_ast_state member_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_IDENTIFIER)) {
            ns_ast_t n = {.type = NS_AST_MEMBER_EXPR, .member_expr = {.left = callee}};
            ns_restore_state(ctx, member_state);
            if (ns_parse_postfix_expr(ctx)) { // recursive member expr
                n.next = ctx->current;
            } else {
                n.next = ns_ast_push(ctx, (ns_ast_t){.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}});
            }
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_ast_error(ctx, "syntax error", "expected identifier after '.'");
        }
    }


    // only primary expr
    ns_restore_state(ctx, state);
    return true;
}

bool ns_parse_unary_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_BIT_INVERT_OP ||
        (ctx->token.type == NS_TOKEN_ADD_OP && ns_str_equals_STR(ctx->token.val, "-"))) {

        ns_ast_t n = {.type = NS_AST_UNARY_EXPR, .unary_expr = {.op = ctx->token}};
        if (ns_parse_postfix_expr(ctx)) {
            n.unary_expr.expr = ctx->current;
            ns_ast_push(ctx, n);
            return true;
        } else {
            ns_ast_error(ctx, "syntax error", "expected expression after unary operator");
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_expr(ns_ast_ctx *ctx) {
    i32 stack_top = ns_array_length(ctx->stack);
    i32 op_top = ns_array_length(ctx->op_stack);
    ns_ast_expr_scope scope = (ns_ast_expr_scope){.stack_top = stack_top, .op_top = op_top, .pre = -1};
    ns_array_push(ctx->scopes, scope);

    ns_ast_state state;
    do {
        state = ns_save_state(ctx);
        if (!ns_parse_next_token(ctx)) {
            if (ctx->token.type == NS_TOKEN_EOF) {
                goto rewind;
            }
            return false;
        }

        switch (ctx->token.type) {
        case NS_TOKEN_IDENTIFIER:
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLT_LITERAL:
        case NS_TOKEN_STR_FORMAT:
        case NS_TOKEN_STR_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
            ns_parse_stack_push_operand(ctx, ns_ast_push(ctx, n));
        } break;

        case NS_TOKEN_ASSIGN:
        case NS_TOKEN_ASSIGN_OP:
        case NS_TOKEN_ADD_OP:
        case NS_TOKEN_MUL_OP:
        case NS_TOKEN_BITWISE_OP:
        case NS_TOKEN_SHIFT_OP:
        case NS_TOKEN_REL_OP:
        case NS_TOKEN_EQ_OP:
        case NS_TOKEN_CMP_OP:
        case NS_TOKEN_LOGIC_OP: {
            // first token is operator
            if (ns_parse_stack_leading_operator(ctx)) {
                ns_restore_state(ctx, state);
                if (ns_parse_unary_expr(ctx)) {
                    ns_parse_stack_push_operand(ctx, ctx->current);
                    break;
                } else {
                    ns_ast_error(ctx, "syntax error", "unexpected token after operator");
                }
            } else {
                ns_parse_stack_push_operator(ctx, ns_ast_push(ctx, (ns_ast_t){.type = NS_AST_BINARY_EXPR, .binary_expr = {.op = ctx->token}}));
            }
        } break;
        case NS_TOKEN_OPEN_PAREN: {
            if (ns_parse_stack_leading_operator(ctx)) {
                // parse inner expr
                if (ns_parse_expr(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
                    ns_ast_t expr = {.type = NS_AST_EXPR, .expr = {.body = ctx->current}};
                    ns_parse_stack_push_operand(ctx, ns_ast_push(ctx, expr));
                    break;
                } else {
                    ns_ast_error(ctx, "syntax error", "expected valid expression after '('");
                }
            } else {
                i32 callee = ns_parse_stack_pop(ctx);
                if (callee == -1) {
                    ns_ast_error(ctx, "syntax error", "expected operand before '('");
                    break;
                }
                if (!ns_parse_call_expr(ctx, callee)) {
                    ns_ast_error(ctx, "syntax error", "invalid call expression.");
                }
                ns_parse_stack_push_operand(ctx, ctx->current);
                break;
            }
        } break;

        case NS_TOKEN_AS:
        case NS_TOKEN_DOT:
        case NS_TOKEN_OPEN_BRACKET:
        case NS_TOKEN_OPEN_BRACE: {
            if (ns_parse_stack_leading_operand(ctx)) {
                i32 left = ns_parse_stack_pop(ctx);
                if (left == -1) {
                    ns_token_t t = ctx->token;
                    ns_ast_error(ctx, "syntax error", "expected operand before %.*s", t.val.len, t.val.data);
                    break;
                }
                if (ns_parse_postfix_expr(ctx)) {
                    ns_parse_stack_push_operand(ctx, ctx->current);
                    break;
                } else {
                    ns_ast_error(ctx, "syntax error", "expected identifier after '.'");
                }
            }
        } break;

        default:
            goto rewind; break;
        }
    } while ((i32)ns_array_length(ctx->stack) > stack_top);

rewind:
    ns_restore_state(ctx, state);
    if (ns_parse_expr_rewind(ctx)) {
        scope = ns_array_pop(ctx->scopes);
        ns_array_set_length(ctx->stack, scope.stack_top);
        ns_array_set_length(ctx->op_stack, scope.op_top);
        ns_array_set_length(ctx->expr_stack, 0);
        return true;
    } else {
        ns_ast_error(ctx, "syntax error", "invalid expression before EOL");
    }
    return false;
}