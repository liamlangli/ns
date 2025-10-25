#include "ns_ast.h"
#include "ns_token.h"

ns_bool ns_parse_module_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_MODULE) &&
        ns_parse_identifier(ctx)) {
        ns_ast_t n = (ns_ast_t){.type = NS_AST_MODULE_STMT, .state = state, .module_stmt = { .name = ctx->token } };
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

ns_bool ns_parse_use_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_USE) && ns_parse_identifier(ctx)) {
        ns_ast_t n = (ns_ast_t){.type = NS_AST_USE_STMT, .state = state, .use_stmt = { .lib = ctx->token } };
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

// type name = type_name
// type name = (args) to type_name
ns_return_bool ns_parse_typedef_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_TYPE)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    ns_ast_t n = {.type = NS_AST_TYPE_DEF, .state = state, .type_def = {.name = ctx->token}};

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    n.type_def.name = ctx->token;
    ns_token_skip_eol(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_return_bool ret = ns_parse_type_label(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        n.type_def.type = ctx->current;
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

// assert expr
ns_return_bool ns_parse_assert_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (!ns_token_require(ctx, NS_TOKEN_ASSERT)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t n = {.type = NS_AST_ASSERT_STMT, .state = state};
    ns_return_bool ret = ns_parse_expr(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        n.assert_stmt.expr = ctx->current;
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }
    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_jump_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    // continue
    ns_ast_t n = {.type = NS_AST_JUMP_STMT, .state = state, .jump_stmt.label = ctx->token, .jump_stmt.expr = 0};
    if (ns_token_require(ctx, NS_TOKEN_CONTINUE) && ns_token_require(ctx, NS_TOKEN_EOL)) {
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }
    ns_restore_state(ctx, state);

    // break
    if (ns_token_require(ctx, NS_TOKEN_BREAK)) {
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }
    ns_restore_state(ctx, state);

    // return
    if (ns_token_require(ctx, NS_TOKEN_RETURN)) {
        ns_ast_state end_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_EOL)) {
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        }
        ns_restore_state(ctx, end_state);

        ret = ns_parse_expr(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            n.jump_stmt.expr = ctx->current;
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected expression after 'return'");
        }
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_if_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_IF)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    // if expression statement [else statement]
    ret = ns_parse_expr(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t n = {.type = NS_AST_IF_STMT, .state = state, .if_stmt.condition = ctx->current};
    ret = ns_parse_compound_stmt(ctx, true);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    n.if_stmt.body = ctx->current;
    n.if_stmt.else_body = 0;

    ns_ast_state else_state = ns_save_state(ctx);
    // try parse else statement
    if (ns_token_require(ctx, NS_TOKEN_ELSE)) {
        ns_ast_state recursive_state = ns_save_state(ctx);

        ret = ns_parse_if_stmt(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            n.if_stmt.else_body = ctx->current;
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        }
        ns_restore_state(ctx, recursive_state);

        ret = ns_parse_compound_stmt(ctx, true);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            n.if_stmt.else_body = ctx->current;
        }
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, else_state);
    ns_ast_push(ctx, n);
    return ns_return_ok(bool, true);
}

ns_return_bool ns_parse_for_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    if (!ns_token_require(ctx, NS_TOKEN_FOR)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    
    ret = ns_parse_gen_expr(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        ns_ast_t n = {.type = NS_AST_FOR_STMT, .state = state, .for_stmt.generator = ctx->current};

        ret = ns_parse_compound_stmt(ctx, true);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            n.for_stmt.body = ctx->current;
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        }
    }
    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

// [loop(cond) {}]  [do {body} loop(cond)]
ns_return_bool ns_parse_loop_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_LOOP_STMT, .loop_stmt.condition = 0, .loop_stmt.body = 0, .loop_stmt.do_first = false};

    // do first
    if (ns_token_require(ctx, NS_TOKEN_DO)) {
        n.loop_stmt.do_first = true;
    } else {
        ns_restore_state(ctx, state);
    }

    if (!n.loop_stmt.do_first && ns_token_require(ctx, NS_TOKEN_LOOP)) {
        n.loop_stmt.do_first = false;
        ret = ns_parse_expr(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected expression after 'loop' statement");
        }
    }

    // loop body
    ret = ns_parse_compound_stmt(ctx, true);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    n.loop_stmt.body = ctx->current;
    if (n.loop_stmt.do_first) {
        if (ns_token_require(ctx, NS_TOKEN_LOOP)) {
            n.loop_stmt.do_first = false;
            ret = ns_parse_expr(ctx);
            if (ns_return_is_error(ret)) return ret;
            if (ret.r) {
                n.loop_stmt.condition = ctx->current;
                ns_ast_push(ctx, n);
                return ns_return_ok(bool, true);
            } else {
                return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected expression after 'loop' statement");
            }
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "expected 'loop' after 'do' statement");
        }
    } else {
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_iteration_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    // for expression statement
    ret = ns_parse_for_stmt(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        return ns_return_ok(bool, true);
    }
    ns_restore_state(ctx, state);

    // loop expression statement
    ret = ns_parse_loop_stmt(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

// { statement* }
ns_return_bool ns_parse_compound_stmt(ns_ast_ctx *ctx, ns_bool brace_required) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    ns_token_skip_eol(ctx);
    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        if (brace_required) {
            ns_restore_state(ctx, state);
            return ns_return_ok(bool, false);
        }
    }

    // direct close test
    ns_ast_state close_state = ns_save_state(ctx);
    ns_token_skip_eol(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
        ns_ast_t n = {.type = NS_AST_COMPOUND_STMT, .state = state};
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }
    ns_restore_state(ctx, close_state);

    ns_token_skip_eol(ctx);
    ns_ast_t n = {.type = NS_AST_COMPOUND_STMT, .state = state};
    i32 next = 0;
    do {
        ret = ns_parse_stmt(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        next = next == 0 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
        n.compound_stmt.count++;
        ns_token_skip_eol(ctx);
        if (ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        }
    } while (1);

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_expr_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    ret = ns_parse_expr(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_EOL || ctx->token.type == NS_TOKEN_EOF) {
            return ns_return_ok(bool, true);
        }
    }
    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);;
}

#define ns_parse_check_fn(f) ret = f(ctx); if (ns_return_is_error(ret)) return ret; if (ret.r) return ns_return_ok(bool, true); ns_restore_state(ctx, state);

ns_return_bool ns_parse_stmt(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    // compound statement
    ret = ns_parse_compound_stmt(ctx, true); if (ns_return_is_error(ret)) return ret; if (ret.r) return ns_return_ok(bool, true); ns_restore_state(ctx, state);
    ns_parse_check_fn(ns_parse_var_define);
    ns_parse_check_fn(ns_parse_assert_stmt);
    ns_parse_check_fn(ns_parse_jump_stmt);
    ns_parse_check_fn(ns_parse_if_stmt);
    ns_parse_check_fn(ns_parse_iteration_stmt);
    ns_parse_check_fn(ns_parse_expr_stmt);

    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_global_define(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    if (ns_parse_use_stmt(ctx)) return ns_return_ok(bool, true);
    ns_restore_state(ctx, state);

    if (ns_parse_module_stmt(ctx)) return ns_return_ok(bool, true);
    ns_restore_state(ctx, state);

    ns_parse_check_fn(ns_parse_var_define);
    ns_parse_check_fn(ns_parse_fn_define);
    ns_parse_check_fn(ns_parse_op_fn_define);
    ns_parse_check_fn(ns_parse_typedef_stmt);
    ns_parse_check_fn(ns_parse_struct_def);
    ns_parse_check_fn(ns_parse_stmt);
    ns_parse_check_fn(ns_parse_expr_stmt);

    return ns_return_error(bool, ns_ast_state_loc(ctx, state), NS_ERR_SYNTAX, "invalid syntax.");
}
