#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

bool ns_parse_unary_expr(ns_parse_context_t *ctx);
bool ns_parse_type_expr(ns_parse_context_t *ctx);

void ns_restore_state(ns_parse_context_t *ctx, int f) { ctx->f = f; }

int ns_save_state(ns_parse_context_t *ctx) { return ctx->f; }

int ns_ast_push(ns_parse_context_t *ctx, ns_ast_t n) {
    ctx->current = ctx->node_count;
    ctx->nodes[ctx->node_count++] = n;
    return ctx->current;
}

bool ns_parse_next_token(ns_parse_context_t *ctx) {
    do {
        ctx->last_f = ctx->f;
        ctx->last_token = ctx->token;
        ctx->f = ns_next_token(&ctx->token, ctx->source, ctx->filename, ctx->f);
    } while (ctx->token.type == NS_TOKEN_COMMENT || ctx->token.type == NS_TOKEN_SPACE);
    return ctx->token.type != NS_TOKEN_EOF;
}

bool ns_token_require(ns_parse_context_t *ctx, NS_TOKEN token) {
    int state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == token) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_token_look_ahead(ns_parse_context_t *ctx, NS_TOKEN token) {
    int state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    bool result = ctx->token.type == token;
    ns_restore_state(ctx, state);
    return result;
}

void ns_token_skip_eol(ns_parse_context_t *ctx) {
    int state;
    do {
        state = ns_save_state(ctx);
        ns_parse_next_token(ctx);
    } while (ctx->token.type == NS_TOKEN_EOL || ctx->token.type == NS_TOKEN_COMMENT);
    ns_restore_state(ctx, state);
}

bool ns_parse_constant_expr(ns_parse_context_t *ctx) { return false; }

bool ns_type_restriction(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // : type
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_COLON) {
        if (ns_parse_type_expr(ctx)) {
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false; // allow empty type declare
}

bool ns_parse_type_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // type
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_TYPE) {
        return true;
    }

    // identifier
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_identifier(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_primary_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_parse_next_token(ctx);

    // literal
    bool is_literal = false;
    switch (ctx->token.type) {
    case NS_TOKEN_INT_LITERAL:
    case NS_TOKEN_FLOAT_LITERAL:
    case NS_TOKEN_STRING_LITERAL:
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
    case NS_TOKEN_NIL:
        is_literal = true;
        break;
    default:
        break;
    }

    if (is_literal) {
        ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token, .slot = -1}};
        ns_ast_push(ctx, n);
        return true;
    }

    // identifier
    ns_restore_state(ctx, state);
    if (ns_parse_identifier(ctx)) {
        return true;
    }

    // ( expression )
    ns_restore_state(ctx, state);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
        if (ns_parse_expr_stack(ctx)) {
            ns_parse_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_generator_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    if (ns_token_require(ctx, NS_TOKEN_LET) && ns_parse_identifier(ctx)) {
        ns_ast_t n = {.type = NS_AST_GENERATOR_EXPR, .generator.label = ctx->token};
        if (!ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
            ns_restore_state(ctx, state);
            return false;
        }

        // identifier in b
        int from_state = ns_save_state(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_IN) {
            n.generator.token = ctx->token;
            if (ns_parse_expr_stack(ctx)) {
                n.generator.from = ctx->current;
                ns_ast_push(ctx, n);
                return true;
            }
        }
        ns_restore_state(ctx, from_state);

        // identifier a to b
        if (ns_primary_expr(ctx)) {
            n.generator.from = ctx->current;
            if (ns_token_require(ctx, NS_TOKEN_TO)) {
                n.generator.token = ctx->token;
                if (ns_primary_expr(ctx)) {
                    n.generator.to = ctx->current;
                    ns_ast_push(ctx, n);
                    return true;
                }
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parameter(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    ns_ast_t n = {.type = NS_AST_PARAM_DEF};
    if (ns_token_require(ctx, NS_TOKEN_REF)) {
        n.param.is_ref = true;
    }

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    n.param.name = ctx->token;

    if (ns_type_restriction(ctx)) {
        n.param.type = ctx->token;
    }

    ctx->current = ns_ast_push(ctx, n);
    return true;
}

bool ns_parse_ops_overridable(ns_token_t token) {
    switch (token.type) {
    case NS_TOKEN_ADDITIVE_OPERATOR:
    case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
    case NS_TOKEN_SHIFT_OPERATOR:
    case NS_TOKEN_RELATIONAL_OPERATOR:
    case NS_TOKEN_EQUALITY_OPERATOR:
    case NS_TOKEN_ARITHMETIC_OPERATOR:
    case NS_TOKEN_BITWISE_OPERATOR:
        return true;
    default:
        return false;
    }
}

bool ns_parse_ops_fn_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // [async] fn identifier ( [type_declare identifier] ) [type_declare] { stmt }

    bool is_async = false;
    if (ns_token_require(ctx, NS_TOKEN_ASYNC)) {
        is_async = true;
    }

    if (!ns_token_require(ctx, NS_TOKEN_FN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_token_require(ctx, NS_TOKEN_OPS)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_parse_next_token(ctx);
    if (!ns_parse_ops_overridable(ctx->token)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_token_t ops = ctx->token;

    if (!(ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN) && ns_token_require(ctx, NS_TOKEN_OPEN_PAREN))) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_ast_t fn = {.type = NS_AST_OPS_FN_DEF, .ops_fn_def = {.ops = ops, .is_async = is_async}};
    // parse parameters
    ns_token_skip_eol(ctx);
    if (!ns_parameter(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    fn.ops_fn_def.left = ctx->current;

    if (!ns_token_require(ctx, NS_TOKEN_COMMA)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_token_skip_eol(ctx);
    if (!ns_parameter(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    fn.ops_fn_def.right = ctx->current;

    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    // optional
    if (ns_type_restriction(ctx)) {
        fn.fn_def.return_type = ctx->token;
    }

    ns_token_skip_eol(ctx);
    if (ns_parse_compound_stmt(ctx)) {
        fn.fn_def.body = ctx->current;
        ctx->current = ns_ast_push(ctx, fn);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_fn_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // [async] fn identifier ( [type_declare identifier] ) [type_declare] { stmt }

    bool is_async = false;
    if (ns_token_require(ctx, NS_TOKEN_ASYNC)) {
        is_async = true;
    }

    if (!ns_token_require(ctx, NS_TOKEN_FN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_token_t name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_ast_t fn = {.type = NS_AST_FN_DEF, .fn_def = {.name = name, .param_count = 0, .is_async = is_async}};
    // parse parameters
    ns_token_skip_eol(ctx);
    while (ns_parameter(ctx)) {
        fn.fn_def.params[fn.fn_def.param_count++] = ctx->current;
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            continue;
        } else {
            break;
        }
    }

    if (fn.fn_def.param_count == 0)
        ns_parse_next_token(ctx);
    if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
        ns_restore_state(ctx, state);
        return false;
    }

    // optional
    if (ns_type_restriction(ctx)) {
        fn.fn_def.return_type = ctx->token;
    }

    if (ns_parse_compound_stmt(ctx)) {
        fn.fn_def.body = ctx->current;
        ctx->current = ns_ast_push(ctx, fn);
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_struct_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_STRUCT)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_token_t name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_ast_t n = {.type = NS_AST_STRUCT_DEF, .struct_def = {.name = name, .count = 0}};
    ns_ast_t *last = &n;
    ns_token_skip_eol(ctx);
    while (ns_parameter(ctx)) {
        ns_token_skip_eol(ctx);
        last->next = ctx->current;
        last = &ctx->nodes[ctx->current];
        n.struct_def.count++;
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA) {
            ns_token_skip_eol(ctx);
            continue;
        } else {
            break;
        }
    }

    if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
        ns_ast_push(ctx, n);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_var_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // identifier [type_declare] = expression
    if (ns_token_require(ctx, NS_TOKEN_LET) && ns_parse_identifier(ctx)) {
        ns_ast_t n = {.type = NS_AST_VAR_DEF, .var_def = {.name = ctx->token}};
        if (ns_type_restriction(ctx)) {
            n.var_def.type = ctx->token;
        }
        int assign_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
            if (ns_parse_expr_stack(ctx)) {
                n.var_def.expr = ctx->current;
                ns_ast_push(ctx, n);
                return true;
            }
        }
        ns_restore_state(ctx, assign_state);
        ns_ast_push(ctx, n);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_type_define(ns_parse_context_t *ctx) {
    // int state = ns_save_state(ctx);

    // type identifier = type
    // if (ns_token_require(ctx, NS_TOKEN_TYPE) && ns_parse_identifier(ctx)) {
    //     ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_TYPE_DEF);
    //     n->type_def.name = ctx->token;
    //     if (ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
    //         if (ns_type_expr(ctx)) {
    //             return true;
    //         }
    //     }
    // }

    // ns_restore_state(ctx, state);
    return false;
}

static ns_parse_context_t _parse_ctx = {0};
ns_parse_context_t *ns_parse(const char *source, const char *filename) {
    ns_parse_context_t *ctx = &_parse_ctx;
    ctx->source = source;
    ctx->filename = filename;
    ctx->f = 0;
    ctx->last_f = 0;
    ctx->top = -1;

    ctx->node_count = 0;
    ctx->section_count = 0;

    ctx->current = -1;

    bool loop = false;
    do {
        ns_token_skip_eol(ctx);
        loop = ns_parse_external_define(ctx);
        if (loop)
            ctx->sections[ctx->section_count++] = ctx->current;
    } while (loop);

    return ctx;
}
