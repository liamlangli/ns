#include "ns_parse.h"
#include "ns_tokenize.h"
#include <stdbool.h>

bool ns_parse_jump_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // continue
    if (ns_token_require(ctx, NS_TOKEN_CONTINUE) && ns_token_require(ctx, NS_TOKEN_EOL)) {
        ns_ast_t n = {.type = NS_AST_JUMP_STMT, .jump_stmt.label = ctx->token};
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);

    // break
    if (ns_token_require(ctx, NS_TOKEN_BREAK)) {
        ns_ast_t n = {.type = NS_AST_JUMP_STMT, .jump_stmt.label = ctx->token};
        ns_ast_push(ctx, n);
        return true;
    }
    ns_restore_state(ctx, state);

    // return
    if (ns_token_require(ctx, NS_TOKEN_RETURN)) {
        ns_ast_t n = {.type = NS_AST_JUMP_STMT, .jump_stmt.label = ctx->token};
        int end_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_EOL)) {
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, end_state);

        if (ns_parse_expr(ctx)) {
            n.jump_stmt.expr = ctx->current;
        }
        ns_ast_push(ctx, n);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_if_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // if expression statement [else statement]
    if (ns_token_require(ctx, NS_TOKEN_IF) && ns_parse_expr(ctx)) {
        ns_ast_t n = {.type = NS_AST_IF_STMT, .if_stmt.condition = ctx->current};
        if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE) && ns_parse_stmt(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
            int else_state = ns_save_state(ctx);
            n.if_stmt.body = ctx->current;

            // try parse else statement
            if (ns_token_require(ctx, NS_TOKEN_ELSE)) {
                if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE) && ns_parse_stmt(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
                    n.if_stmt.else_body = ctx->current;
                    ns_ast_push(ctx, n);
                    return true;
                }
            }
            ns_restore_state(ctx, else_state);

            ns_ast_push(ctx, n);
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_iteration_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // for expression statement
    if (ns_token_require(ctx, NS_TOKEN_FOR) && ns_parse_generator_expr(ctx)) {
        ns_ast_t n = {.type = NS_AST_ITER_STMT, .iter_stmt.generator = ctx->current};
        // with body
        int body_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE) && ns_parse_stmt(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
            n.iter_stmt.body = ctx->current;
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, body_state);

        // without body
        if (ns_token_require(ctx, NS_TOKEN_EOL)) {
            ns_ast_push(ctx, n);
            return true;
        }
    }
    ns_restore_state(ctx, state);

    // while expression statement
    if (ns_token_require(ctx, NS_TOKEN_WHILE) && ns_parse_expr(ctx)) {
        ns_ast_t n = {.type = NS_AST_ITER_STMT, .iter_stmt.condition = ctx->current};

        // with body
        int body_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE) && ns_parse_stmt(ctx) && ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
            n.iter_stmt.body = ctx->current;
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, body_state);

        // without body
        if (ns_token_require(ctx, NS_TOKEN_EOL)) {
            ns_ast_push(ctx, n);
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_compound_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        while (ns_parse_var_define(ctx) || ns_parse_stmt(ctx)) {
            if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
                ns_parse_next_token(ctx);
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_expr_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_parse_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_EOL) {
            ns_parse_next_token(ctx);
            return true;
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // define statement
    if (ns_parse_compound_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // jump statement
    if(ns_parse_jump_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // if statement
    if (ns_parse_if_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // iteration statement
    if (ns_iteration_stmt(ctx)) {
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

bool ns_parse_external_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_parse_var_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_fn_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_struct_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_parse_type_define(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    return false;
}