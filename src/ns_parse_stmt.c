#include "ns_parse.h"
#include "ns_tokenize.h"

#include <stdbool.h>
#include <assert.h>

bool ns_parse_jump_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
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
        int end_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_EOL)) {
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, end_state);

        if (ns_parse_expr_stack(ctx)) {
            n.jump_stmt.expr = ctx->current;
        } else {
            fprintf(stderr, "Expected expression after 'return'\n");
            assert(false);
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
    if (ns_token_require(ctx, NS_TOKEN_IF) &&
        ns_parse_expr_stack(ctx)) {
        ns_ast_t n = {.type = NS_AST_IF_STMT, .if_stmt.condition = ctx->current};
        if (ns_parse_compound_stmt(ctx)) {
            n.if_stmt.body = ctx->current;
            n.if_stmt.else_body = -1;

            int else_state = ns_save_state(ctx);
            // try parse else statement
            if (ns_token_require(ctx, NS_TOKEN_ELSE)) {
                int recursive_state = ns_save_state(ctx);
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

bool ns_parse_for_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_FOR) && ns_parse_generator_expr(ctx)) {
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

bool ns_parse_while_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_WHILE) && ns_parse_expr_stack(ctx)) {
        ns_ast_t n = {.type = NS_AST_WHILE_STMT, .while_stmt.condition = ctx->current};
        if (ns_parse_compound_stmt(ctx)) {
            n.while_stmt.body = ctx->current;
            ns_ast_push(ctx, n);
            return true;
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_iteration_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // for expression statement
    if (ns_parse_for_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // while expression statement
    if (ns_parse_while_stmt(ctx)) {
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_compound_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {

        // direct close test
        int close_state = ns_save_state(ctx);
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
            ns_ast_t n = {.type = NS_AST_COMPOUND_STMT, .compound_stmt.section = -1 };
            ns_ast_push(ctx, n);
            return true;
        }
        ns_restore_state(ctx, close_state);

        ns_token_skip_eol(ctx);
        int section = ctx->compound_count;
        ns_ast_compound_sections *sections = &ctx->compound_sections[ctx->compound_count++];
        sections->section_count = 0;
        ns_ast_t n = {.type = NS_AST_COMPOUND_STMT, .compound_stmt.section = section };
        while (ns_parse_var_define(ctx) ||
               ns_parse_stmt(ctx)) {
            sections->sections[sections->section_count++] = ctx->current;
            ns_token_skip_eol(ctx);
            ns_parse_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
                ns_ast_push(ctx, n);
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_expr_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    if (ns_parse_expr_stack(ctx)) {
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_EOL) {
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

    if (ns_parse_expr_stmt(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}