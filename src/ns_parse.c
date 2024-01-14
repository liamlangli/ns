#include "ns.h"
#include "ns_parse.h"
#include "ns_tokenize.h"

#include <stdlib.h>

bool ns_stmt(ns_parse_context_t *ctx);

void ns_parse_context_restore_state(ns_parse_context_t *ctx, int f) {
    ctx->f = f;
    ctx->token->type = NS_TOKEN_UNKNOWN;
}

int ns_parse_context_save_state(ns_parse_context_t *ctx) {
    return ctx->f;
}

bool ns_next_token(ns_parse_context_t *ctx) {
    ctx->f = ns_tokenize(ctx->token, ctx->source, ctx->filename, ctx->f);
    return ctx->token->type != NS_TOKEN_EOF;
}

bool ns_labeled_stmt(ns_parse_context_t *ctx) {
    ns_parse_context_save_state(ctx);
    // case constant-expression : statement
    if (ctx->token->type == NS_TOKEN_CASE) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_INT_LITERAL) {
            ns_next_token(ctx);
            if (ctx->token->type == NS_TOKEN_COLON) {
                ns_next_token(ctx);
                if (ns_stmt(ctx)) {
                    return true;
                }
            }
        }
    }
    ns_parse_context_restore_state(ctx);

    // default : statement
    if (ctx->token->type == NS_TOKEN_DEFAULT) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_COLON) {
            ns_next_token(ctx);
            if (ns_stmt(ctx, source, filename, f)) {
                return true;
            }
        }
    }
    ns_parse_context_restore_state(ctx);
    return false;
}

bool ns_selection_stmt(ns_parse_context_t *ctx) {
    int state = ns_parse_context_save_state(ctx);
    // if ( expression ) statement
    if (ctx->token->type == NS_TOKEN_IF) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_LPAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token->type == NS_TOKEN_RPAREN) {
                    ns_next_token(ctx);
                    if (ns_stmt(ctx)) {
                        return true;
                    }
                }
            }
        }
    }
    ns_parse_context_restore_state(ctx, state);

    // if ( expression ) statement else statement
    if (ctx->token->type == NS_TOKEN_IF) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_LPAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token->type == NS_TOKEN_RPAREN) {
                    ns_next_token(ctx);
                    if (ns_stmt(ctx)) {
                        if (ctx->token->type == NS_TOKEN_ELSE) {
                            ns_next_token(ctx);
                            if (ns_stmt(ctx)) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    ns_parse_context_restore_state(ctx);

    // switch ( expression ) statement
    if (ctx->token->type == NS_TOKEN_SWITCH) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_LPAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token->type == NS_TOKEN_RPAREN) {
                    ns_next_token(ctx);
                    if (ns_stmt(ctx)) {
                        return true;
                    }
                }
            }
        }
    }
    ns_parse_context_restore_state(ctx);
    return false;
}

bool ns_stmt(ns_parse_context_t *ctx) {
    // expression statement
   
    // selection statement
    
    // iteration statement
    
    // declaration statement

    // jump statement
    

    return true;
}

static ns_parse_context_t ctx = {0};
ns_ast_t* ns_parse(const char *source, const char *filename) {
    ns_ast_t *root = (ns_ast_t*)malloc(sizeof(ns_ast_t));

    ctx.source = source;
    ctx.filename = filename;
    ctx.f = 0;
    ctx.last_f = 0;

    while (ns_stmt(&ctx));

    return root;
}
