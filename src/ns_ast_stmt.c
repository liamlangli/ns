#include "ns_ast.h"
#include "ns_token.h"

bool ns_parse_import_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_IMPORT) && ns_parse_identifier(ctx)) {
        ns_ast_t n = (ns_ast_t){.type = NS_AST_IMPORT_STMT, .import_stmt = { .lib = ctx->token } };
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_jump_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    // continue
    ns_ast_t n = {.type = NS_AST_JUMP_STMT, .jump_stmt.label = ctx->token, .jump_stmt.expr = -1};
    if (ns_token_require(ctx, NS_TOKEN_CONTINUE) && ns_token_require(ctx, NS_TOKEN_EOL)) {
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);

    // break
    if (ns_token_require(ctx, NS_TOKEN_BREAK)) {
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);

    // return
    if (ns_token_require(ctx, NS_TOKEN_RETURN)) {
        ns_ast_state end_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_EOL)) {
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, end_state);

        if (ns_parse_expr(ctx)) {
            n.jump_stmt.expr = ctx->current;
        } else {
            ns_ast_error(ctx, "syntax error", "expected expression after 'return'");
        }
        ns_ast_push(ctx, n);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_if_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    // if expression statement [else statement]
    if (ns_token_require(ctx, NS_TOKEN_IF) && ns_parse_expr(ctx)) {
        ns_ast_t n = {.type = NS_AST_IF_STMT, .if_stmt.condition = ctx->current};
        if (ns_parse_compound_stmt(ctx)) {
            n.if_stmt.body = ctx->current;
            n.if_stmt.else_body = -1;

            ns_ast_state else_state = ns_save_state(ctx);
            // try parse else statement
            if (ns_token_require(ctx, NS_TOKEN_ELSE)) {
                ns_ast_state recursive_state = ns_save_state(ctx);
                if (ns_parse_if_stmt(ctx)) {
                    n.if_stmt.else_body = ctx->current;
                    ns_ast_push(ctx, n);
                    return true;
                }
                ns_restore_state(ctx, recursive_state);

                if (ns_parse_compound_stmt(ctx)) {
                    n.if_stmt.else_body = ctx->current;
                }
                ns_ast_push(ctx, n);
                return true;
            }

            ns_restore_state(ctx, else_state);
            ns_ast_push(ctx, n);
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_for_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_FOR) && ns_parse_gen_expr(ctx)) {
        ns_ast_t n = {.type = NS_AST_FOR_STMT, .for_stmt.generator = ctx->current};
        if (ns_parse_compound_stmt(ctx)) {
            n.for_stmt.body = ctx->current;
            ns_ast_push(ctx, n);
            return true;
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

// [loop(cond) {}]  [do {body} loop(cond)]
bool ns_parse_loop_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_LOOP_STMT, .loop_stmt.condition = -1, .loop_stmt.body = -1, .loop_stmt.do_first = false};

    // do first
    if (ns_token_require(ctx, NS_TOKEN_DO)) {
        n.loop_stmt.do_first = true;
    } else {
        ns_restore_state(ctx, state);
    }

    if (!n.loop_stmt.do_first && ns_token_require(ctx, NS_TOKEN_LOOP)) {
        n.loop_stmt.do_first = false;
        if (!ns_parse_expr(ctx)) {
            ns_ast_error(ctx, "syntax error", "expected expression after 'loop' statement");
        }
    }

    // loop body
    if (ns_parse_compound_stmt(ctx)) {
        n.loop_stmt.body = ctx->current;
        if (n.loop_stmt.do_first) {
            if (ns_token_require(ctx, NS_TOKEN_LOOP)) {
                n.loop_stmt.do_first = false;
                if (ns_parse_expr(ctx)) {
                    n.loop_stmt.condition = ctx->current;
                    ns_ast_push(ctx, n);
                    return true;
                } else {
                    ns_ast_error(ctx, "syntax error", "expected expression after 'loop' statement");
                }
            } else {
                ns_ast_error(ctx, "syntax error", "expected 'loop' after 'do' statement");
            }
        } else {
            ns_ast_push(ctx, n);
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_iteration_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    // for expression statement
    if (ns_parse_for_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // loop expression statement
    if (ns_parse_loop_stmt(ctx)) {
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_compound_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {

        // direct close test
        ns_ast_state close_state = ns_save_state(ctx);
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
            ns_ast_t n = {.type = NS_AST_COMPOUND_STMT};
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, close_state);

        ns_token_skip_eol(ctx);
        ns_ast_t n = {.type = NS_AST_COMPOUND_STMT};
        i32 next = -1;
        while (ns_parse_var_define(ctx) || ns_parse_stmt(ctx)) {
            next = next == -1 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
            n.compound_stmt.count++;
            ns_token_skip_eol(ctx);
            if (ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
                ns_ast_push(ctx, n);
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_designated_field(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_FIELD_DEF, .field_def = {.name = ctx->token}};
    if (ns_parse_identifier(ctx)) {
        if (ns_token_require(ctx, NS_TOKEN_COLON)) {
            if (ns_parse_expr(ctx)) {
                n.field_def.expr = ctx->current;
                ns_ast_push(ctx, n);
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

// struct { a: 1, b: 2 }
bool ns_parse_desig_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    ns_token_skip_eol(ctx);
    ns_ast_t n = {.type = NS_AST_DESIG_EXPR, .desig_expr = {.name = ctx->token}};
    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return false;
    }

    i32 next = -1;
    ns_token_skip_eol(ctx);
    while (ns_parse_designated_field(ctx)) {
        next = next == -1 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
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
    }

    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_ast_push(ctx, n);
    return true;
}

bool ns_parse_expr_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    if (ns_parse_expr(ctx)) {
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_EOL || ctx->token.type == NS_TOKEN_EOF) {
            return true;
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_stmt(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    // define statement
    if (ns_parse_compound_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // jump statement
    if (ns_parse_jump_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // if statement
    if (ns_parse_if_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // iteration statement
    if (ns_parse_iteration_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // expression statement
    if (ns_parse_expr_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    return false;
}

bool ns_parse_global_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    if (ns_parse_import_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_var_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_ops_fn_define(ctx) || ns_parse_fn_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_struct_def(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_type_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_expr_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}