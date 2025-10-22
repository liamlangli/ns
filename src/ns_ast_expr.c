#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"

static ns_return_bool ns_parse_trailing_block_arg(ns_ast_ctx *ctx, i32 call_index);

i32 ns_ast_push_expr(ns_ast_ctx *ctx, ns_ast_state state, i32 i) {
    ns_ast_t expr = {.type = NS_AST_EXPR, .state = state, .expr = {.body = i, .atomic = 1}};
    return ns_ast_push(ctx, expr);
}

ns_bool ns_parse_stack_push_operand(ns_ast_ctx *ctx, i32 i) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    scope->pre = i;
    ns_array_push(ctx->stack, i);
    return true;
}

ns_bool ns_parse_stack_push_operator(ns_ast_ctx *ctx, i32 i) {
    ns_ast_t n = ctx->nodes[i];
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    scope->pre = i;

    i32 op_len = ns_array_length(ctx->op_stack);
    if (op_len <= scope->op_top) { // empty
        ns_array_push(ctx->op_stack, i);
    } else {
        i32 top = op_len - 1;
        while (top >= scope->op_top && n.binary_expr.op.type <= ctx->nodes[ctx->op_stack[top]].binary_expr.op.type) {
            ns_array_push(ctx->stack, ns_array_pop(ctx->op_stack));
            top--;
        }
        ns_array_push(ctx->op_stack, i);
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

ns_bool ns_parse_is_operand(ns_ast_t n) {
    switch (n.type) {
    case NS_AST_PRIMARY_EXPR:
    case NS_AST_CALL_EXPR:
    case NS_AST_MEMBER_EXPR:
    case NS_AST_CAST_EXPR:
    case NS_AST_INDEX_EXPR:
    case NS_AST_UNARY_EXPR:
    case NS_AST_DESIG_EXPR:
    case NS_AST_BLOCK_EXPR:
    case NS_AST_EXPR:
        return true;
    default:
        return false;
    }
}

ns_bool ns_parse_stack_empty(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    return scope->stack_top == (i32)ns_array_length(ctx->stack);
}

ns_bool ns_parse_stack_leading_operand(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    if (scope->pre == 0) {
        return false;
    }
    return ns_parse_is_operand(ctx->nodes[scope->pre]);
}

ns_bool ns_parse_stack_leading_operator(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    if (scope->pre == 0) {
        return false;
    }
    return !ns_parse_is_operand(ctx->nodes[scope->pre]);
}

ns_bool ns_token_is_operator(ns_token_t token) {
    switch (token.type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_BITWISE_OP:
    case NS_TOKEN_SHIFT_OP:
    case NS_TOKEN_REL_OP:
    case NS_TOKEN_EQ_OP:
    case NS_TOKEN_LOGIC_OP:
    case NS_TOKEN_REF:
        return true;
    default:
        return false;
    }
}

// Shunting Yard Algorithm
ns_return_bool ns_parse_expr_rewind(ns_ast_ctx *ctx) {
    ns_ast_expr_scope *scope = ns_array_last(ctx->scopes);
    
    i32 op_len = ns_array_length(ctx->op_stack);
    for (i32 i = scope->op_top; i < op_len; ++i) {
        i32 op = ns_array_pop(ctx->op_stack);
        ns_array_push(ctx->stack, op);
    }

    i32 len = ns_array_length(ctx->stack);
    if (len == 1) {
        ctx->current = ns_array_pop(ctx->stack);
        return ns_return_ok(bool, true);
    }

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

    if (ctx->expr_stack == NULL || ns_array_length(ctx->expr_stack) == 0) {
        fprintf(stderr, "DEBUG ns_parse_expr_rewind: stack_len=%zu expr_stack_len=%zu scope_top=%d op_top=%d\n",
                ns_array_length(ctx->stack), ctx->expr_stack ? ns_array_length(ctx->expr_stack) : 0,
                scope->stack_top, scope->op_top);
        ns_ast_state state = ns_save_state(ctx);
        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "unexpected expr.");
    }

    ctx->current = ns_array_pop(ctx->expr_stack);
    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_call_expr(ns_ast_ctx *ctx, int callee) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_CALL_EXPR, .state = state, .call_expr = { .callee = callee, .arg_count = 0}};
    if (ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    i32 next = 0;
    do {
        ret = ns_parse_expr(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        next = next == 0 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
        n.call_expr.arg_count++;

        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA) {
            continue;
        } else if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
            break;
        } else {
            return ns_return_error(bool, ns_ast_code_loc(ctx), NS_ERR_SYNTAX, "expected ',' or ')'");
        }
    } while (ret.r);

    if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
        return ns_return_error(bool, ns_ast_code_loc(ctx), NS_ERR_SYNTAX, "expected ')'");
    }

    ns_ast_push(ctx, n);
    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_str_format(ns_ast_ctx *ctx) {
    ns_ast_state state = (ns_ast_state){.f = ctx->last_f, .l = ctx->token.line, .o = ctx->last_f - ctx->token.line_start};

    ns_str fmt = ctx->token.val;
    ns_ast_t n = {.type = NS_AST_STR_FMT_EXPR, .state = state, .str_fmt = {.expr_count = 0, .fmt = fmt}};
    ns_ast_state end_state = (ns_ast_state){.f = state.f + fmt.len + 2, .l = state.l, .o = state.o + fmt.len + 2};
    i32 source_len = ctx->source.len;

    // parse format string
    ns_ast_t *expr = &n;
    i32 i = 0;
    while (i < fmt.len) {
        if (fmt.data[i] == '{' && (i == 0 || (i > 0 && fmt.data[i - 1] != '\\'))) {
            i32 start = ++i;
            while (i < fmt.len && fmt.data[i] != '}') {
                i++;
            }

            if (i == fmt.len) {
                ns_ast_state expr_state = (ns_ast_state){.f = state.f + start, .l = state.l, .o = state.o + start};
                return ns_return_error(bool, ns_ast_state_loc(ctx, expr_state), NS_ERR_SYNTAX, "missing '}'.");
            }
            i32 len = i - start;
            ns_ast_state expr_state = (ns_ast_state){.f = state.f + start + 1, .l = state.l, .o = state.o + start + 1};
            ns_restore_state(ctx, expr_state);

            // temporary set end of source to parse expression
            ctx->source.len = expr_state.f + len;
            ns_return_bool ret = ns_parse_expr(ctx);

            if (ns_return_is_error(ret)) return ret;
            if (ret.r) {
                n.str_fmt.expr_count++;
                ns_ast_push_expr(ctx, expr_state, ctx->current);
                expr->next = ctx->current;
                expr = &ctx->nodes[ctx->current];
            } else {
                return ns_return_error(bool, ns_ast_state_loc(ctx, expr_state), NS_ERR_SYNTAX, "expected expression.");
            }
        } else {
            i++;
        }
    }
    ctx->source.len = source_len;

    if (n.str_fmt.expr_count == 0) {
        n = (ns_ast_t){.type = NS_AST_PRIMARY_EXPR, .state = state, .primary_expr = {.token = ctx->token}};
        return ns_return_ok(bool, true);
    }

    ns_ast_push(ctx, n);
    ns_restore_state(ctx, end_state);
    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_array_expr(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    // [type](expr)
    ns_ast_state state = ns_save_state(ctx);
    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACKET)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ret = ns_parse_type_label(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected type after '['");
    }

    ns_ast_t n = {.type = NS_AST_ARRAY_EXPR, .state = state, .array_expr = {.type = ctx->current}};
    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_BRACKET)) {
        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected ']' after type");
    }

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected '(' after ']'");
    }

    ret = ns_parse_expr(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected expression after '('");
    }

    n.array_expr.count_expr = ctx->current;
    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected ')' after expression");
    }

    ns_ast_push(ctx, n);
    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_primary_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_parse_next_token(ctx)) {
        switch (ctx->token.type) {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLT_LITERAL:
        case NS_TOKEN_STR_LITERAL: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .state = state, .primary_expr = {.token = ctx->token}};
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } break;
        case NS_TOKEN_STR_FORMAT: {
            return ns_parse_str_format(ctx);
        } break;
        case NS_TOKEN_IDENTIFIER: {
            ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .state = state, .primary_expr = {.token = ctx->token}};
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } break;

        default:
            ns_restore_state(ctx, state);
            return ns_return_ok(bool, false);
        }
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_cast_expr(ns_ast_ctx *ctx, i32 operand) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_AS)) {
        if (ns_token_require_type(ctx)) {
            ns_ast_t n = {.type = NS_AST_CAST_EXPR, .state = state, .cast_expr = {.expr = operand, .type = ctx->token}};
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected type after 'as'");
        }
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_designated_field(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_FIELD_DEF, .state = state, .field_def = {.name = ctx->token}};
    if (ns_parse_identifier(ctx)) {
        if (ns_token_require(ctx, NS_TOKEN_COLON)) {
            ret = ns_parse_expr(ctx);
            if (ns_return_is_error(ret)) return ret;
            if (ret.r) {
                n.field_def.expr = ctx->current;
                ns_ast_push(ctx, n);
                return ns_return_ok(bool, true);
            } else {
                return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected expression after ':'");
            }
        }
    }
    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

// struct { a: 1, b: 2 }
ns_return_bool ns_parse_designated_expr(ns_ast_ctx *ctx, i32 st) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    ns_token_skip_eol(ctx);
    ns_ast_t n = {.type = NS_AST_DESIG_EXPR, .state = state, .desig_expr = {.name = ctx->nodes[st].primary_expr.token}};
    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    

    i32 next = 0;
    ns_token_skip_eol(ctx);
    do {
        ret = ns_parse_designated_field(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        next = next == 0 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
        n.desig_expr.count++;

        ns_token_skip_eol(ctx);
        ns_ast_state next_state = ns_save_state(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA) {
            ns_token_skip_eol(ctx);
            continue;
        } else {
            ns_restore_state(ctx, next_state);
            break;
        }
    } while (1);

    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_push(ctx, n);
    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_member_expr(ns_ast_ctx *ctx, i32 operand) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_DOT)) {
        if (ns_token_require(ctx, NS_TOKEN_IDENTIFIER)) {
            ns_ast_t token = {.type = NS_AST_PRIMARY_EXPR, .state = state, .primary_expr = {.token = ctx->token}};
            i32 t = ns_ast_push(ctx, token);
            
            ns_ast_state member_state = ns_save_state(ctx);
            ns_ast_t n = {.type = NS_AST_MEMBER_EXPR, .state = member_state, .next = t, .member_expr = {.left = operand}};

            ret = ns_parse_member_expr(ctx, t);
            if (ns_return_is_error(ret)) return ret;
            if (ret.r) { // recursive member expr
                n.next = ctx->current;
            } else {
                ns_restore_state(ctx, member_state);
            }
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected identifier after '.'");
        }
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

/*
 * parse postfix expression
 * operand: 
 */
ns_return_bool ns_parse_postfix_expr(ns_ast_ctx *ctx, i32 operand) {
    ns_return_bool ret;
    ns_ast_state primary_state = ns_save_state(ctx);

    if (operand == 0) {
        ret = ns_parse_primary_expr(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) {
            ns_restore_state(ctx, primary_state);
            return ns_return_ok(bool, false);
        }
        operand = ctx->current;
    }
    ns_ast_state state = ns_save_state(ctx);

    // parse postfix '(' [expr]* [,expr]* ')'
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ret = ns_parse_call_expr(ctx, operand);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            ns_return_bool trailing = ns_parse_trailing_block_arg(ctx, ctx->current);
            if (ns_return_is_error(trailing)) return trailing;
            return ns_return_ok(bool, true);
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected call expression after '('");
        }
    }

    // parse postfix { [a: expr]*, [b: expr]* }
    ns_restore_state(ctx, state);
    ret = ns_parse_designated_expr(ctx, operand);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        return ns_return_ok(bool, true);
    }

    //  parse postfix '[' expr ']'
    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACKET)) {
        ns_return_bool ret = ns_parse_expr(ctx);
        if (ns_return_is_error(ret)) return ret;

        if (ret.r && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACKET)) {
            ns_ast_t n = {.type = NS_AST_INDEX_EXPR, .state = state, .index_expr = {.table = operand, .expr = ctx->current}};
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected expression after '['");
        }
    }

    // parse postfix '.' identifier
    ns_restore_state(ctx, state);
    ret = ns_parse_member_expr(ctx, operand);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_unary_expr(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_BIT_INVERT_OP ||
        (ctx->token.type == NS_TOKEN_ADD_OP && ns_str_equals_STR(ctx->token.val, "-"))) {
        ns_ast_t n = {.type = NS_AST_UNARY_EXPR, .state = state, .unary_expr = {.op = ctx->token}};

        ns_ast_state operand_state = ns_save_state(ctx);
        ret = ns_parse_postfix_expr(ctx, 0);
        if (ns_return_is_error(ret)) return ret;

        if (ret.r) {
            n.unary_expr.expr = ctx->current;
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } else {
            ns_restore_state(ctx, operand_state);
            ret = ns_parse_primary_expr(ctx);
            if (ns_return_is_error(ret)) return ret;

            if (ret.r) {
                n.unary_expr.expr = ctx->current;
                ns_ast_push(ctx, n);
                return ns_return_ok(bool, true);
            } else {
                ns_code_loc loc = ns_ast_state_loc(ctx, state);
                return ns_return_error(bool, loc, NS_ERR_SYNTAX, "expected expression after unary operator");
            }
        }
    } else if (ctx->token.type == NS_TOKEN_REF) {
        ns_ast_t n = {.type = NS_AST_UNARY_EXPR, .state = state, .unary_expr = {.op = ctx->token}};

        ns_ast_state operand_state = ns_save_state(ctx);
        ret = ns_parse_postfix_expr(ctx, 0);
        if (ns_return_is_error(ret)) return ret;

        if (ret.r) {
            n.unary_expr.expr = ctx->current;
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } else {
            ns_restore_state(ctx, operand_state);
            ret = ns_parse_primary_expr(ctx);
            if (ns_return_is_error(ret)) return ret;

            if (ret.r) {
                n.unary_expr.expr = ctx->current;
                ns_ast_push(ctx, n);
                return ns_return_ok(bool, true);
            } else {
                ns_code_loc loc = ns_ast_state_loc(ctx, state);
                return ns_return_error(bool, loc, NS_ERR_SYNTAX, "expected expression after 'ref'");
            }
        }
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

// { [(args) [-> ret]] in body }
ns_return_bool ns_parse_block_expr(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t n = {.type = NS_AST_BLOCK_EXPR, .state = state, .block_expr = {.arg_count = 0}};
    ns_token_skip_eol(ctx);
    i32 next = 0;
    do {
        ret = ns_parse_arg(ctx, false);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        next = next == 0 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
        n.block_expr.arg_count++;
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            continue;
        } else {
            break;
        }
    } while(1);

    

    // [-> ret_type]
    if (ctx->token.type == NS_TOKEN_RETURN_TYPE) {
        ns_return_bool ret = ns_parse_type_label(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            n.block_expr.ret = ctx->current;
        }
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
    }
    // require in
    if (ctx->token.type != NS_TOKEN_IN) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    /* consume 'in' */
    ns_parse_next_token(ctx);
    ns_token_skip_eol(ctx);
    ns_token_skip_eol(ctx);

    ret = ns_parse_compound_stmt(ctx, false);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    n.block_expr.body = ctx->current;
    ctx->current = ns_ast_push(ctx, n);
    return ns_return_ok(bool, true);
}

static ns_return_bool ns_parse_trailing_block_arg(ns_ast_ctx *ctx, i32 call_index) {
    ns_ast_t *call = &ctx->nodes[call_index];
    if (call->type != NS_AST_CALL_EXPR) {
        return ns_return_ok(bool, false);
    }

    ctx->current = call_index;

    ns_ast_state start_state = ns_save_state(ctx);
    ns_ast_state token_state = start_state;

    while (1) {
        ns_restore_state(ctx, token_state);
        if (!ns_parse_next_token(ctx)) {
            ns_restore_state(ctx, start_state);
            return ns_return_ok(bool, false);
        }
        if (ctx->token.type == NS_TOKEN_EOL) {
            token_state = ns_save_state(ctx);
            continue;
        }
        break;
    }

    if (ctx->token.type != NS_TOKEN_OPEN_BRACE) {
        ns_restore_state(ctx, start_state);
        return ns_return_ok(bool, false);
    }

    ns_restore_state(ctx, token_state);
    ns_return_bool block_ret = ns_parse_block_expr(ctx);
    if (ns_return_is_error(block_ret)) return block_ret;
    if (!block_ret.r) {
        ns_restore_state(ctx, start_state);
        ctx->current = call_index;
        return ns_return_ok(bool, false);
    }

    i32 block_node = ctx->current;
    i32 block_expr = ns_ast_push_expr(ctx, start_state, block_node);
    call = &ctx->nodes[call_index];

    if (call->next == 0) {
        call->next = block_expr;
    } else {
        i32 arg = call->next;
        while (ctx->nodes[arg].next != 0) {
            arg = ctx->nodes[arg].next;
        }
        ctx->nodes[arg].next = block_expr;
    }
    call->call_expr.arg_count++;
    ctx->current = call_index;

    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_expr(ns_ast_ctx *ctx) {
    i32 stack_top = ns_array_length(ctx->stack);
    i32 op_top = ns_array_length(ctx->op_stack);
    ns_ast_expr_scope scope = (ns_ast_expr_scope){.stack_top = stack_top, .op_top = op_top, .pre = 0};
    ns_array_push(ctx->scopes, scope);

    ns_return_bool ret;
    ns_ast_state state;
    i32 source_len = ctx->source.len;
    do {
        state = ns_save_state(ctx);
        if (!ns_parse_next_token(ctx)) {
            if (ctx->token.type == NS_TOKEN_EOF || ctx->f > source_len) {
                goto rewind;
            }
            return ns_return_ok(bool, false);
        }

        switch (ctx->token.type) {
        case NS_TOKEN_IDENTIFIER:
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLT_LITERAL:
        case NS_TOKEN_STR_FORMAT:
        case NS_TOKEN_STR_LITERAL: {
            ns_restore_state(ctx, state);

            ret = ns_parse_primary_expr(ctx);
            if (ns_return_is_error(ret)) return ret;

            if (ret.r) {
                ns_parse_stack_push_operand(ctx, ctx->current);
            } else {
                ns_code_loc loc = ns_ast_code_loc(ctx);
                return ns_return_error(bool, loc, NS_ERR_SYNTAX, "expected primary expression");
            }
        } break;

        case NS_TOKEN_ASSIGN:
        case NS_TOKEN_REF:
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
            if (ns_parse_stack_leading_operator(ctx) ||
                ns_parse_stack_empty(ctx)) {
                ns_restore_state(ctx, state);

                ns_return_bool ret = ns_parse_unary_expr(ctx);
                if (ns_return_is_error(ret)) return ret;
                if (ret.r) {
                    ns_parse_stack_push_operand(ctx, ctx->current);
                    break;
                } else {
                    ns_code_loc loc = ns_ast_state_loc(ctx, state);
                    return ns_return_error(bool, loc, NS_ERR_SYNTAX, "expected unary expression");
                }
            } else {
                ns_parse_stack_push_operator(ctx, ns_ast_push(ctx, (ns_ast_t){.type = NS_AST_BINARY_EXPR, .state = state, .binary_expr = {.op = ctx->token}}));
            }
        } break;
        case NS_TOKEN_OPEN_PAREN: {
            if (ns_parse_stack_leading_operator(ctx) ||
                ns_parse_stack_empty(ctx)) {

                ns_return_bool ret = ns_parse_expr(ctx);
                if (ns_return_is_error(ret)) return ret;
                if (ret.r &&
                    ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
                    ns_parse_stack_push_operand(ctx, ns_ast_push_expr(ctx, state, ctx->current));
                    break;
                } else {
                    return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected valid expression after '('");
                }
            } else {
                i32 callee = ns_parse_stack_pop(ctx);
                ret = ns_parse_call_expr(ctx, callee);
                if (ns_return_is_error(ret)) return ret;
                if (!ret.r) {
                    return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected call expression after '('");
                }
                ns_parse_stack_push_operand(ctx, ns_ast_push_expr(ctx, state, ctx->current));
                break;
            }
        } break;

        case NS_TOKEN_AS: {
            if (ns_parse_stack_leading_operand(ctx)) {
                i32 operand = ns_parse_stack_pop(ctx);
                ns_restore_state(ctx, state);
                ns_return_bool ret = ns_parse_cast_expr(ctx, operand);
                if (ns_return_is_error(ret)) return ret;

                if (ret.r) {
                    ns_parse_stack_push_operand(ctx, ctx->current);
                    break;
                } else {
                    return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected type after 'as'");
                }
            }
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected operand before 'as'");
        } break;
        case NS_TOKEN_DOT:
        case NS_TOKEN_OPEN_BRACKET:
        case NS_TOKEN_OPEN_BRACE: {
            if (ns_parse_stack_leading_operand(ctx)) {
                i32 operand = ns_parse_stack_pop(ctx);
                ns_restore_state(ctx, state);
                if (operand == 0) {
                    return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected operand before.");
                    break;
                }

                ns_return_bool ret = ns_parse_postfix_expr(ctx, operand);
                if (ns_return_is_error(ret)) return ret;
                if (ret.r) {
                    ns_parse_stack_push_operand(ctx, ctx->current);
                    break;
                } else {
                    ns_parse_stack_push_operand(ctx, operand);
                    goto rewind;
                }
            } else {
                if (ctx->token.type == NS_TOKEN_OPEN_BRACKET) { // parse array define: [type](expr)
                    ns_restore_state(ctx, state);
                    ret = ns_parse_array_expr(ctx);
                    if (ns_return_is_error(ret)) return ret;
                    if (ret.r) {
                        ns_parse_stack_push_operand(ctx, ctx->current);
                        break;
                    } else {
                        return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected array expression");
                    }
                } else if (ctx->token.type == NS_TOKEN_OPEN_BRACE) { // parse block define: { [(args)] [-> ret] in body }
                    ns_restore_state(ctx, state);
                    ret = ns_parse_block_expr(ctx);
                if (ns_return_is_error(ret))
                    return ret;
                if (!ret.r)
                    return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected block expression");
                i32 expr_index = ns_ast_push_expr(ctx, state, ctx->current);
                ns_parse_stack_push_operand(ctx, expr_index);
                
                break;    
            }
        }
    } break;

        default:
            goto rewind;
        }
    } while ((i32)ns_array_length(ctx->stack) > stack_top);

rewind:
    ns_restore_state(ctx, state);
    fprintf(stderr, "DEBUG before rewind stack_len=%zu scope_top=%d\n",
            ns_array_length(ctx->stack), ns_array_last(ctx->scopes)->stack_top);
    ns_return_bool rewind_ret = ns_parse_expr_rewind(ctx);
    if (ns_return_is_error(rewind_ret)) return rewind_ret;
    if (rewind_ret.r) {
        scope = ns_array_pop(ctx->scopes);
        ns_array_set_length(ctx->stack, scope.stack_top);
        ns_array_set_length(ctx->op_stack, scope.op_top);
        ns_array_set_length(ctx->expr_stack, 0);
        return ns_return_ok(bool, true);
    } else {
        return ns_return_error(bool, ns_ast_code_loc(ctx), NS_ERR_SYNTAX, "invalid expression before EOL");
    }
    return ns_return_ok(bool, false);
}
