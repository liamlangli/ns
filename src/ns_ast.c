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
    return ctx->token.type != NS_TOKEN_EOF && ctx->f <= ctx->source.len;
}

ns_bool ns_token_require(ns_ast_ctx *ctx, ns_token_type t) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    if (ctx->token.type == t) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

ns_bool ns_token_can_be_type(ns_token_type t) {
    switch (t) {
    case NS_TOKEN_TYPE_ANY:
    case NS_TOKEN_TYPE_VOID:
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

ns_bool ns_token_look_ahead(ns_ast_ctx *ctx, ns_token_type t) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    ns_bool result = ctx->token.type == t;
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

// [ref] type_name
// [ref] [type_name]
// (args_type) to type_name
ns_return_bool ns_parse_type_label(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_ast_t n = {.type = NS_AST_TYPE_LABEL, .state = state};

    if (ns_token_require(ctx, NS_TOKEN_REF)) {
        n.type_label.is_ref = true;
    } else {
        ns_restore_state(ctx, state);
    }

    // array type
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

    // fn type
    ns_ast_state func_state = ns_save_state(ctx);
    if (ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        n.type_label.is_fn = true;
        ns_token_skip_eol(ctx);
        i32 next = 0;
        do {
            ns_return_bool ret = ns_parse_type_label(ctx);
            if (ns_return_is_error(ret)) return ret;

            next = next == 0 ? n.next = ctx->current : (ctx->nodes[next].next = ctx->current);
            n.type_label.arg_count++;
            ns_token_skip_eol(ctx);
            ns_parse_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
                continue;
            } else {
                break;
            }
        } while(1);

        if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
            ns_restore_state(ctx, func_state);
            return ns_return_ok(bool, false);
        }

        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_TO || ctx->token.type == NS_TOKEN_RETURN_TYPE) {
            ns_return_bool ret = ns_parse_type_label(ctx);
            if (ns_return_is_error(ret)) return ret;
            if (ret.r) {
                n.type_label.ret = ctx->current;
            }
        }

        n.type_label.is_fn = true;
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    } else {
        ns_restore_state(ctx, func_state);
    }

    // type name
    if (ns_parse_type_name(ctx)) {
        n.type_label.name = ctx->token;
        ns_ast_push(ctx, n);
        return ns_return_ok(bool, true);
    }

    ns_restore_state(ctx, state);
    return ns_return_ok(bool, false);
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

ns_return_bool ns_parse_arg(ns_ast_ctx *ctx, ns_bool type_required) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);

    ns_ast_t n = {.type = NS_AST_ARG_DEF, .state = state, .arg.type = 0};

    if (!ns_parse_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }
    n.arg.name = ctx->token;

    if (ns_token_require(ctx, NS_TOKEN_COLON) && !type_required) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    ns_ast_state type_state = ns_save_state(ctx);
    ret = ns_parse_type_label(ctx);
    if (ns_return_is_error(ret)) return ret;
    if (!ret.r && type_required) {
        ns_restore_state(ctx, state);
        return ns_return_ok(bool, false);
    }

    if (ret.r)
        n.arg.type = ctx->current;
    else
        ns_restore_state(ctx, type_state);

    // parse default value
    if (ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
        ret = ns_parse_expr(ctx);
        if (ns_return_is_error(ret)) return ret;
        if (ret.r) {
            n.arg.val = ctx->current;
            ctx->current = ns_ast_push(ctx, n);
            return ns_return_ok(bool, true);
        }
    } else {
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

ns_fn_type ns_parse_fn_type(ns_ast_ctx *ctx) {
    ns_ast_state state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    switch (ctx->token.type) {
    case NS_TOKEN_ASYNC: return NS_FN_ASYNC;
    case NS_TOKEN_KERNEL: return NS_FN_KERNEL;
    case NS_TOKEN_VERTEX: return NS_FN_VERTEX;
    case NS_TOKEN_FRAGMENT: return NS_FN_FRAGMENT;
    case NS_TOKEN_COMPUTE: return NS_FN_COMPUTE;
    default:
        ns_restore_state(ctx, state);
        return NS_FN_GENERIC;
    }
}   

ns_return_bool ns_parse_ops_fn_define(ns_ast_ctx *ctx) {
    ns_return_bool ret;
    ns_ast_state state = ns_save_state(ctx);
    // [async] fn identifier ( [type_declare identifier] ) [type_declare] { stmt }
    ns_bool is_ref = ns_token_require(ctx, NS_TOKEN_REF);
    ns_fn_type fn_type = ns_parse_fn_type(ctx);

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

    ns_ast_t fn = {.type = NS_AST_OPS_FN_DEF, .state = state, .ops_fn_def = {.ops = ops, .type = fn_type, .is_ref = is_ref, .ret = 0, .body = 0, .arg_required = 2}};
    // parse parameters
    ns_token_skip_eol(ctx);
    ret = ns_parse_arg(ctx, true);
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

    ret = ns_parse_arg(ctx, true);
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
    ret = ns_parse_compound_stmt(ctx, true);
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
    ns_fn_type fn_type = ns_parse_fn_type(ctx);

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

    ns_ast_t fn = {.type = NS_AST_FN_DEF, .state = state, .fn_def = {.name = name, .arg_count = 0, .type = fn_type, .is_ref = is_ref, .ret = 0, .body = 0}};
    // parse args
    ns_token_skip_eol(ctx);
    i32 val_assigned = 0;
    i32 next = 0;
    do {
        ret = ns_parse_arg(ctx, true);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) break;

        next = next == 0 ? fn.next = ctx->current : (ctx->nodes[next].next = ctx->current);
        fn.fn_def.arg_count++;
        ns_ast_t *arg = &ctx->nodes[ctx->current];
        if (val_assigned > 0 && arg->arg.val == 0) {
            return ns_return_error(bool, ns_ast_state_loc(ctx, arg->state), NS_ERR_SYNTAX, "subscript arg value required.");
        }
        if (arg->arg.val != 0) val_assigned++;

        ns_token_skip_eol(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            continue;
        } else {
            break;
        }
    } while(1);
    if (val_assigned > 0) fn.fn_def.arg_required = fn.fn_def.arg_count - val_assigned;

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

    ret = ns_parse_compound_stmt(ctx, true);
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
        ret = ns_parse_arg(ctx, true);
        if (ns_return_is_error(ret)) return ret;
        if (!ret.r) return ns_return_error(bool, ns_ast_code_loc(ctx), NS_ERR_SYNTAX, "struct field definition expected.");

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

i32 ns_ast_struct_field_index(ns_ast_ctx *ctx, i32 st, ns_str name) {
    ns_ast_t *n = &ctx->nodes[st];
    i32 count = n->struct_def.count;
    for (i32 i = 0; i < count; i++) {
        n = &ctx->nodes[n->next];
        if (ns_str_equals(n->arg.name.val, name)) {
            return i;
        }
    }
    return -1;
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

ns_return_bool ns_ast_parse(ns_ast_ctx *ctx, ns_str source, ns_str filename) {
    ctx->source = source;
    ctx->filename = filename;
    ctx->token.line = 1; // start from 1
    ctx->current = 0;
    ctx->f = 0;

    if (ns_array_length(ctx->nodes) == 0) {
        ns_ast_push(ctx, (ns_ast_t){.type = NS_AST_PROGRAM, .next = 0});
    }

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
        if (ctx->token.type == NS_TOKEN_EOF) break;
    } while (loop.r);

    if (ctx->f < ctx->source.len) {
        return ns_return_error(bool, ns_ast_code_loc(ctx), NS_ERR_SYNTAX, "unexpected parse error");
    }
    return ns_return_ok(bool, true);
}
