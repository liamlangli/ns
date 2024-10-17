#include "ns_ast.h"
#include "ns_tokenize.h"

#include <assert.h>

bool ns_parse_unary_expr(ns_ast_ctx *ctx);
bool ns_parse_type_expr(ns_ast_ctx *ctx);

void ns_restore_state(ns_ast_ctx *ctx, ns_ast_state state) {
    ctx->f = state.f;
    ctx->token.line = state.l;
}

ns_ast_state ns_save_state(ns_ast_ctx *ctx) { return (ns_ast_state){.f = ctx->f, .l = ctx->token.line, .o = ctx->f - ctx->token.line_start}; }

int ns_ast_push(ns_ast_ctx *ctx, ns_ast_t n) {
    ctx->current = ns_array_length(ctx->nodes);
    ns_array_push(ctx->nodes, n);
    return ctx->current;
}

bool ns_parse_next_token(ns_ast_ctx *ctx) {
    do {
        ctx->last_f = ctx->f;
        ctx->last_token = ctx->token;
        ctx->f = ns_next_token(&ctx->token, ctx->source, ctx->filename, ctx->f);
    } while (ctx->token.type == NS_TOKEN_COMMENT || ctx->token.type == NS_TOKEN_SPACE);
    return ctx->token.type != NS_TOKEN_EOF;
}

bool ns_token_require(ns_ast_ctx *ctx, NS_TOKEN token) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == token) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_token_can_be_type(NS_TOKEN t) {
    switch (t) {
    case NS_TOKEN_TYPE_I8:
    case NS_TOKEN_TYPE_I16:
    case NS_TOKEN_TYPE_I32:
    case NS_TOKEN_TYPE_I64:
    case NS_TOKEN_TYPE_U8:
    case NS_TOKEN_TYPE_U16:
    case NS_TOKEN_TYPE_U32:
    case NS_TOKEN_TYPE_U64:
    case NS_TOKEN_TYPE_F32:
    case NS_TOKEN_TYPE_F64:
    case NS_TOKEN_TYPE_BOOL:
    case NS_TOKEN_TYPE_STR:
    case NS_TOKEN_IDENTIFIER:
        return true;
    default:
        break;
    }
    return false;
}

bool ns_token_require_type(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ns_token_can_be_type(ctx->token.type)) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_token_look_ahead(ns_ast_ctx *ctx, NS_TOKEN token) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    bool result = ctx->token.type == token;
    ns_restore_state(ctx, state);
    return result;
}

bool ns_token_skip_eol(ns_ast_ctx *ctx) {
    ns_ast_state state;
    do {
        state = ns_save_state(ctx);
        ns_parse_next_token(ctx);
    } while (ctx->token.type == NS_TOKEN_EOL || ctx->token.type == NS_TOKEN_COMMENT);
    ns_restore_state(ctx, state);
    return ctx->token.type == NS_TOKEN_EOF;
}

bool ns_type_restriction(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

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

bool ns_parse_type_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    // type
    ns_parse_next_token(ctx);
    if (ns_token_can_be_type(ctx->token.type)) {
        return true;
    }

    // identifier
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_identifier(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_primary_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);

    // literal
    bool is_literal = false;
    switch (ctx->token.type) {
    case NS_TOKEN_INT_LITERAL:
    case NS_TOKEN_FLT_LITERAL:
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
    case NS_TOKEN_NIL:
        is_literal = true;
        break;
    default:
        break;
    }

    if (is_literal) {
        ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .primary_expr = {.token = ctx->token}};
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

bool ns_parse_generator_expr(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    if (ns_token_require(ctx, NS_TOKEN_LET) && ns_parse_identifier(ctx)) {
        ns_ast_t n = {.type = NS_AST_GENERATOR_EXPR, .generator.label = ctx->token};
        if (!ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
            ns_restore_state(ctx, state);
            return false;
        }

        // identifier in b
        ns_ast_state from_state = ns_save_state(ctx);
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

bool ns_parameter(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    ns_ast_t n = {.type = NS_AST_ARG_DEF};
    if (ns_token_require(ctx, NS_TOKEN_REF)) {
        n.arg.is_ref = true;
    }

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    n.arg.name = ctx->token;

    if (ns_type_restriction(ctx)) {
        n.arg.type = ctx->token;
    }

    ctx->current = ns_ast_push(ctx, n);
    return true;
}

bool ns_parse_ops_overridable(ns_token_t token) {
    switch (token.type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_SHIFT_OP:
    case NS_TOKEN_REL_OP:
    case NS_TOKEN_EQ_OP:
    case NS_TOKEN_BITWISE_OP:
        return true;
    default:
        return false;
    }
}

bool ns_parse_ops_fn_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
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

bool ns_parse_fn_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
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

    ns_ast_t fn = {.type = NS_AST_FN_DEF, .fn_def = {.name = name, .arg_count = 0, .is_async = is_async}};
    // parse args
    ns_token_skip_eol(ctx);
    ns_ast_t *arg = &fn;
    while (ns_parameter(ctx)) {
        fn.fn_def.arg_count++;
        arg->next = ctx->current;
        arg = &ctx->nodes[ctx->current];
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            continue;
        } else {
            break;
        }
    }

    if (fn.fn_def.arg_count == 0)
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

bool ns_parse_struct_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

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
    ns_ast_t *field = &n;
    ns_token_skip_eol(ctx);
    while (ns_parameter(ctx)) {
        ns_token_skip_eol(ctx);
        field->next = ctx->current;
        field = &ctx->nodes[ctx->current];
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

bool ns_parse_var_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    // identifier [type_declare] = expression
    if (ns_token_require(ctx, NS_TOKEN_LET) && ns_parse_identifier(ctx)) {
        ns_ast_t n = {.type = NS_AST_VAR_DEF, .var_def = {.name = ctx->token}};
        if (ns_type_restriction(ctx)) {
            n.var_def.type = ctx->token;
        }
        ns_ast_state assign_state = ns_save_state(ctx);
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

bool ns_parse_type_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_restore_state(ctx, state);
    return false;
}

bool ns_ast_parse(ns_ast_ctx *ctx, ns_str source, ns_str filename) {
    ctx->source = source;
    ctx->filename = filename;
    ctx->top = -1;
    ctx->token.line = 1; // start from 1
    ctx->current = -1;
    ctx->f = 0;

    ctx->section_begin = ctx->section_end;
    bool loop = false;
    do {
        if(ns_token_skip_eol(ctx)) break; // EOF
        loop = ns_parse_global_define(ctx);
        if (loop) {
            ns_array_push(ctx->sections, ctx->current);
            ctx->section_end++;
        }
    } while (loop);
    return true;
}
