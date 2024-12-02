#include "ns_ast.h"
#include "ns_token.h"

void ns_restore_state(ns_ast_ctx *ctx, ns_ast_state state) {
    ctx->f = state.f;
    ctx->token.line = state.l;
}

ns_ast_state ns_save_state(ns_ast_ctx *ctx) { return (ns_ast_state){.f = ctx->f, .l = ctx->token.line, .o = ctx->f - ctx->token.line_start}; }

i32 ns_ast_push(ns_ast_ctx *ctx, ns_ast_t n) {
    ctx->current = ns_array_length(ctx->nodes);
    ns_array_push(ctx->nodes, n);
    return ctx->current;
}

ns_bool ns_parse_next_token(ns_ast_ctx *ctx) {
    do {
        ctx->last_f = ctx->f;
        ctx->last_token = ctx->token;
        ctx->f = ns_next_token(&ctx->token, ctx->source, ctx->filename, ctx->f);
    } while (ctx->token.type == NS_TOKEN_COMMENT || ctx->token.type == NS_TOKEN_SPACE);
    return ctx->token.type != NS_TOKEN_EOF;
}

ns_bool ns_token_require(ns_ast_ctx *ctx, NS_TOKEN token) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == token) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

ns_bool ns_token_can_be_type(NS_TOKEN t) {
    switch (t) {
    case NS_TOKEN_NIL:
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

ns_bool ns_token_require_type(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ns_token_can_be_type(ctx->token.type)) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

ns_bool ns_token_look_ahead(ns_ast_ctx *ctx, NS_TOKEN token) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
   ns_bool result = ctx->token.type == token;
    ns_restore_state(ctx, state);
    return result;
}

ns_bool ns_token_skip_eol(ns_ast_ctx *ctx) {
    ns_ast_state state;
    do {
        state = ns_save_state(ctx);
        ns_parse_next_token(ctx);
    } while (ctx->token.type == NS_TOKEN_EOL || ctx->token.type == NS_TOKEN_COMMENT);
    ns_restore_state(ctx, state);
    return ctx->token.type == NS_TOKEN_EOF;
}

// [ref] type_name
ns_return_bool ns_parse_type_label(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_TYPE_LABEL, .state = state};

    if (ns_token_require(ctx, NS_TOKEN_REF)) {
        n.type_label.is_ref = true;
    } else {
        ns_restore_state(ctx, state);
    }

    ns_ast_state array_state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_BRACKET) && ns_parse_type_name(ctx)) {
        n.type_label.name = ctx->token;
        if (ns_token_require(ctx, NS_TOKEN_CLOSE_BRACKET)) {
            n.type_label.is_array = true;
            ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        } else {
            return ns_return_error(bool, ns_ast_state_loc(ctx, array_state), NS_ERR_SYNTAX, "expected ']'");
        }
    } else {
        ns_restore_state(ctx, array_state);
    }

    if (ns_parse_type_name(ctx)) {
        n.type_label.name = ctx->token;
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_bool ns_parse_type_name(ns_ast_ctx *ctx) {
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

ns_bool ns_parse_identifier(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

ns_return_bool ns_primary_expr(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);

    // literal
   ns_bool is_literal = false;
    switch (ctx->token.type) {
    case NS_TOKEN_INT_LITERAL:
    case NS_TOKEN_FLT_LITERAL:
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT:
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
    case NS_TOKEN_NIL:
        is_literal = true;
        break;
    default:
        break;
    }

    if (is_literal) {
        ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .state = state, .primary_expr = {.token = ctx->token} };
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    // identifier
    ns_restore_state(ctx, state);
    if (ns_parse_identifier(ctx)) {
        ns_ast_t n = {.type = NS_AST_PRIMARY_EXPR, .state = state,.primary_expr = {.token = ctx->token} };
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    // ( expression )
    ns_restore_state(ctx, state);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
        ret = ns_parse_expr(ctx);
        if (ns_return_is_error(ret)) return ret;

        if (ret.r) {
            ns_parse_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
                return ns_return_ok(bool, true);
            }
        }
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

// [a in b] [a in n to m]
ns_return_bool ns_parse_gen_expr(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t n = {.type = NS_AST_GEN_EXPR, .state = state, .gen_expr.name = ctx->token};
    if (!ns_token_require(ctx, NS_TOKEN_IN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ret = ns_parse_expr(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        n.gen_expr.from = ctx->current;
        if (ns_token_require(ctx, NS_TOKEN_TO)) {

            ret = ns_parse_expr(ctx);
            if (ns_return_is_error(ret)) return ret;
            if (ret.r) {
                n.gen_expr.to = ctx->current;
                n.gen_expr.range = true;
            }
        }
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_arg(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    ns_ast_t n = {.type = NS_AST_ARG_DEF, .state = state};

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    n.arg.name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_COLON)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ret = ns_parse_type_label(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        n.arg.type = ctx->current;
        ctx->current = ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_bool ns_parse_ops_overridable(ns_token_t token) {
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

ns_return_bool ns_parse_ops_fn_define(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    // [async] fn identifier ( [type_declare identifier] ) [type_declare] { stmt }

   ns_bool is_ref = ns_token_require(ctx, NS_TOKEN_REF);
   ns_bool is_async = ns_token_require(ctx, NS_TOKEN_ASYNC);

    if (!ns_token_require(ctx, NS_TOKEN_FN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    if (!ns_token_require(ctx, NS_TOKEN_OPS)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_parse_next_token(ctx);
    if (!ns_parse_ops_overridable(ctx->token)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    ns_token_t ops = ctx->token;

    if (!(ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN) && ns_token_require(ctx, NS_TOKEN_OPEN_PAREN))) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t fn = {.type = NS_AST_OPS_FN_DEF, .state = state, .ops_fn_def = {.ops = ops, .is_async = is_async, .is_ref = is_ref, .ret = 0}};
    // parse parameters
    ns_token_skip_eol(ctx);
    ret = ns_parse_arg(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    fn.ops_fn_def.left = ctx->current;

    if (!ns_token_require(ctx, NS_TOKEN_COMMA)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_token_skip_eol(ctx);

    ret = ns_parse_arg(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    fn.ops_fn_def.right = ctx->current;

    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    // optional
    ns_ast_state type_state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_COLON)) {
        ret = ns_parse_type_label(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            fn.ops_fn_def.ret = ctx->current;
        } else {
            ns_restore_state(ctx, type_state);
        }
    } else {
        ns_restore_state(ctx, type_state);
    }

    // ref type declare, no fn body
    if (is_ref) {
        ctx->current = ns_ast_push(ctx, fn);
        return ns_return_ok(bool, true);
    }

    ns_token_skip_eol(ctx);
    ret = ns_parse_compound_stmt(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        fn.ops_fn_def.body = ctx->current;
        ctx->current = ns_ast_push(ctx, fn);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_fn_define(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    // [async] fn identifier ( [type_declare identifier] ) [type_declare] { stmt }

   ns_bool is_ref = ns_token_require(ctx, NS_TOKEN_REF);
   ns_bool is_kernel = ns_token_require(ctx, NS_TOKEN_KERNEL);
   ns_bool is_async = ns_token_require(ctx, NS_TOKEN_ASYNC);

    if (!ns_token_require(ctx, NS_TOKEN_FN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    ns_token_t name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t fn = {.type = NS_AST_FN_DEF, .state = state, .fn_def = {.name = name, .arg_count = 0, .is_async = is_async, .is_ref = is_ref, .is_kernel = is_kernel, .ret = 0}};
    // parse args
    ns_token_skip_eol(ctx);
    i32 next = 0;
    do {
        ret = ns_parse_arg(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        next = next == 0 ? fn.next = ctx->current : (ctx->nodes[next].next = ctx->current);
        fn.fn_def.arg_count++;
        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            continue;
        } else {
            break;
        }
    } while(1);

    if (fn.fn_def.arg_count == 0)
        ns_parse_next_token(ctx);

    if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    // optional
    ns_ast_state type_state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_COLON)) {
        ret = ns_parse_type_label(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            fn.fn_def.ret = ctx->current;
        } else {
            ns_restore_state(ctx, type_state);
        }
    } else {
        ns_restore_state(ctx, type_state);
    }

    // ref type declare, no fn body
    if (is_ref) {
        ctx->current = ns_ast_push(ctx, fn);
        return ns_return_ok(bool, true);
    }

    ret = ns_parse_compound_stmt(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (ret.r) {
        fn.fn_def.body = ctx->current;
        ctx->current = ns_ast_push(ctx, fn);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_struct_def(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_STRUCT)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    ns_token_t name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_t n = {.type = NS_AST_STRUCT_DEF, .state = state, .struct_def = {.name = name, .count = 0}};
    i32 next = 0;
    ns_token_skip_eol(ctx);
    do {
        ret = ns_parse_arg(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        ns_token_skip_eol(ctx);
        next = next == 0 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);

        n.struct_def.count++;
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA) {
            ns_token_skip_eol(ctx);
            continue;
        } else {
            break;
        }
    } while(1);

    if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_return_bool ns_parse_var_define(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    // identifier [type_declare] = expression
    if (ns_token_require(ctx, NS_TOKEN_LET) && ns_parse_identifier(ctx)) {
        ns_ast_t n = {.type = NS_AST_VAR_DEF, .state = state, .var_def = {.name = ctx->token, .type = 0, .expr = 0}};

        ns_ast_state type_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_COLON)) {
            ret = ns_parse_type_label(ctx);
            if (ns_return_is_error(ret)) return ret;
            if (ret.r) {
                n.var_def.type = ctx->current;
            } else {
                ns_restore_state(ctx, type_state);
            }
        } else {
            ns_restore_state(ctx, type_state);
        }

        ns_ast_state assign_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
            ret = ns_parse_expr(ctx);
            if (ns_return_is_error(ret)) return ret;

            if (ret.r) {
                n.var_def.expr = ctx->current;
                ns_ast_push(ctx, n);
                return ns_return_ok(bool, true);
            }
        }
        ns_restore_state(ctx, assign_state);
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
}

ns_bool ns_parse_type_define(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_TYPE)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_ast_t n = {.type = NS_AST_TYPE_DEF, .state = state, .type_def = {.name = ctx->token}};

    if (!ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_parse_type_name(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }

    n.type_def.type = ctx->token;
    ns_ast_push(ctx, n);
    return true;
}

ns_return_bool ns_ast_parse(ns_ast_ctx *ctx, ns_str source, ns_str filename) {
    ctx->source = source;
    ctx->filename = filename;
    ctx->token.line = 1; // start from 1
    ctx->current = 0;
    ctx->f = 0;

    ctx->stack = NULL;
    ctx->op_stack = NULL;
    ctx->expr_stack = NULL;
    ctx->sections = NULL;
    ctx->nodes = NULL;

    ns_ast_push(ctx, (ns_ast_t){.type = NS_AST_PROGRAM, .next = 0});

    ctx->section_begin = ctx->section_end;
    ns_return_bool loop;
    do {
        if(ns_token_skip_eol(ctx)) break; // EOF
        loop = ns_parse_global_define(ctx);
        if (ns_return_is_error(loop)) return loop;
        if (loop.r) {
            ns_array_push(ctx->sections, ctx->current);
            ctx->section_end++;
        }
    } while (loop.r);

    if (ctx->f < ctx->source.len) {
        return ns_return_error(bool, ns_ast_code_loc(ctx), NS_ERR_SYNTAX, "unexpected parse error");
    }
    return ns_return_ok(bool, true);
}
