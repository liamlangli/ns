#include "ns_type.h"
#include "ns_parse.h"
#include "ns_tokenize.h"

#include <stdlib.h>

bool ns_stmt(ns_parse_context_t *ctx);
bool ns_expr(ns_parse_context_t *ctx);

void ns_restore_state(ns_parse_context_t *ctx, int f) {
    ctx->f = f;
    ctx->token->type = NS_TOKEN_UNKNOWN;
}

int ns_save_state(ns_parse_context_t *ctx) {
    return ctx->f;
}

bool ns_next_token(ns_parse_context_t *ctx) {
    ctx->f = ns_tokenize(ctx->token, ctx->source, ctx->filename, ctx->f);
    return ctx->token->type != NS_TOKEN_EOF;
}

bool ns_labeled_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
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
    ns_restore_state(ctx, state);

    // default : statement
    if (ctx->token->type == NS_TOKEN_DEFAULT) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_COLON) {
            ns_next_token(ctx);
            if (ns_stmt(ctx)) {
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_selection_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // if ( expression ) statement
    if (ctx->token->type == NS_TOKEN_IF) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token->type == NS_TOKEN_CLOSE_PAREN) {
                    ns_next_token(ctx);
                    if (ns_stmt(ctx)) {
                        return true;
                    }
                }
            }
        }
    }
    ns_restore_state(ctx, state);

    // if ( expression ) statement else statement
    if (ctx->token->type == NS_TOKEN_IF) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token->type == NS_TOKEN_CLOSE_PAREN) {
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
    ns_restore_state(ctx, state);

    // switch ( expression ) statement
    if (ctx->token->type == NS_TOKEN_SWITCH) {
        ns_next_token(ctx);
        if (ctx->token->type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token->type == NS_TOKEN_CLOSE_PAREN) {
                    ns_next_token(ctx);
                    if (ns_stmt(ctx)) {
                        return true;
                    }
                }
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}



bool ns_multiplicative_expr(ns_parse_context_t *ctx) {
}

bool ns_additive_expr(ns_parse_context_t *ctx) {

}

bool ns_shift_expr(ns_parse_context_t *ctx) {

}

bool ns_relational_expr(ns_parse_context_t *ctx) {

}

bool ns_equality_expr(ns_parse_context_t *ctx) {

}

bool ns_and_expr(ns_parse_context_t *ctx) {

}

bool ns_exclusive_or_expr(ns_parse_context_t *ctx) {

}

bool ns_inclusive_or_expr(ns_parse_context_t *ctx) {

}

bool ns_logic_and_expr(ns_parse_context_t *ctx) {

}

bool ns_logic_or_expr(ns_parse_context_t *ctx) {
    // logic-and-expression
    int state = ns_save_state(ctx);
    return false;
}

bool ns_logical_expr(ns_parse_context_t *ctx) {

    return false;
}

bool ns_conditional_expr(ns_parse_context_t *ctx) {

}

bool ns_unary_operator(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_next_token(ctx);
    switch (ctx->token->type) {
        case NS_TOKEN_ARITHMETIC_OPERATOR:
        case NS_TOKEN_BITWISE_OPERATOR:
        case NS_TOKEN_BOOL_OPERATOR:
            return true;
        default:
            ns_restore_state(ctx, state);
            return false;
    }
}

bool ns_identifier(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_next_token(ctx);
    if (ctx->token->type == NS_TOKEN_IDENTIFIER) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_primary_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_next_token(ctx);

    // literal
    switch (ctx->token->type) {
        case NS_TOKEN_INT_LITERAL:
        case NS_TOKEN_FLOAT_LITERAL:
        case NS_TOKEN_STRING_LITERAL:
        case NS_TOKEN_TRUE:
        case NS_TOKEN_FALSE:
        case NS_TOKEN_NIL:
            return true;
        default:
        break;
    }

    // identifier
    ns_restore_state(ctx, state);
    if (ns_identifier(ctx)) {
        return true;
    }

    // ( expression )
    ns_restore_state(ctx, state);
    if (ctx->token->type == NS_TOKEN_OPEN_PAREN) {
        ns_next_token(ctx);
        if (ns_expr(ctx)) {
            if (ctx->token->type == NS_TOKEN_CLOSE_PAREN) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_postfix_expr(ns_parse_context_t *ctx) {
    
}

bool ns_unary_expr(ns_parse_context_t *ctx) {
    // postfix-expression
    int state = ns_save_state(ctx);
    if (ns_postfix_expr(ctx)) {
        return true;
    }

    // unary-operator unary-expression
    ns_restore_state(ctx, state);
    if (ns_unary_operator(ctx)) {
        if (ns_unary_expr(ctx)) {
            return true;
        }
    }

    // await unary-expression
    ns_restore_state(ctx, state);
    if (ctx->token->type == NS_TOKEN_AWAIT) {
        ns_next_token(ctx);
        if (ns_unary_expr(ctx)) {
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_assign_expr(ns_parse_context_t *ctx) {
    // conditional-expression
    int state = ns_save_state(ctx);
    if (ns_expr(ctx)) {
        return true;
    }
    ns_restore_state(ctx, state);

    // unary-expression assignment-operator assignment-expression
    if (ns_expr(ctx)) {
        if (ctx->token->type == NS_TOKEN_ASSIGN_OPERATOR) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_expr(ns_parse_context_t *ctx) {
    // assignment-expression
    int state = ns_save_state(ctx);
    if (ns_expr(ctx)) {
        if (ctx->token->type == NS_TOKEN_ASSIGN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                return true;
            }
        }
    }
    // assignment-expression , expression
    ns_restore_state(ctx, state);

    if (ns_expr(ctx)) {
        if (ctx->token->type == NS_TOKEN_COMMA) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                return true;
            }
        }
    }

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
