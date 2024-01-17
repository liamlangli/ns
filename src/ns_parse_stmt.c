#include "ns_parse.h"
#include "ns_tokenize.h"

bool ns_parse_define_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // fn_define
    if (ns_parse_fn_define(ctx)) {
        return true;
    }

    // struct define

    // variable define
    return false;
}

bool ns_parse_jump_stmt(ns_parse_context_t *ctx) {

    // | continue ;
    // | break ;
    // | return {<expression>}? ;
    int state = ns_save_state(ctx);
    ns_ast_t *node = ns_ast_emplace(ctx, NS_AST_JUMP_STMT);
    ctx->current = node;

    if (ns_token_require(ctx, NS_TOKEN_CONTINUE)) {
        node->jump_stmt.label = ctx->token;
        return true;
    }
    ns_restore_state(ctx, state);

    if (ns_token_require(ctx, NS_TOKEN_BREAK)) {
        ns_ast_t *node = ns_ast_emplace(ctx, NS_AST_JUMP_STMT);
        node->jump_stmt.label = ctx->token;
        return true;
    }

    ns_restore_state(ctx, state);
    if (ns_token_require(ctx, NS_TOKEN_RETURN)) {
        ns_ast_t *node = ns_ast_emplace(ctx, NS_AST_JUMP_STMT);
        node->jump_stmt.label = ctx->token;
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_EOL) {
            return true;
        }

        if (ns_parse_expr(ctx)) {
            node->jump_stmt.expr = ctx->current;
        }
        return true;
    }

    ctx->current = NULL;
    ns_ast_pop(ctx);
    ns_restore_state(ctx, state);
    return false;
}

bool ns_labeled_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // case constant-expression : statement
    if (ns_token_require(ctx, NS_TOKEN_CASE)) {
        
    }
    ns_restore_state(ctx, state);

    // default : statement
    if (ctx->token.type == NS_TOKEN_DEFAULT) {
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COLON) {
            ns_parse_next_token(ctx);
            if (ns_parse_stmt(ctx)) {
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // define statement
    if (ns_parse_define_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // jump statement
    if(ns_parse_jump_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // expression statement
    // if (ns_expr(ctx)) {
    //     if (ctx->token.type == NS_TOKEN_SEMICOLON) {
    //         ns_parse_next_token(ctx);
    //         return true;
    //     }
    // }

    // selection statement
    // ns_restore_state(ctx, state);
    // if (ns_selection_stmt(ctx)) {
    //     return true;
    // }

    // iteration statement
    // ns_restore_state(ctx, state);
    // if (ns_iteration_stmt(ctx)) {
    //     return true;
    // }


    return false;
}
