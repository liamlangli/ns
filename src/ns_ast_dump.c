#include "ns_ast.h"

ns_str ns_ast_type_to_string(NS_AST_TYPE type) {
    switch (type) {
    case NS_AST_PROGRAM:
        return ns_str_cstr("NS_AST_PROGRAM");
    case NS_AST_ARG_DEF:
        return ns_str_cstr("NS_AST_ARG_DEF");
    case NS_AST_FN_DEF:
        return ns_str_cstr("NS_AST_FN_DEF");
    case NS_AST_VAR_DEF:
        return ns_str_cstr("NS_AST_VAR_DEF");
    case NS_AST_STRUCT_DEF:
        return ns_str_cstr("NS_AST_STRUCT_DEF");
    case NS_AST_OPS_FN_DEF:
        return ns_str_cstr("NS_AST_OPS_FN_DEF");
    case NS_AST_TYPE_DEF:
        return ns_str_cstr("NS_AST_TYPE_DEF");
    case NS_AST_STRUCT_FIELD_DEF:
        return ns_str_cstr("NS_AST_STRUCT_FIELD_DEF");
    case NS_AST_EXPR:
        return ns_str_cstr("NS_AST_EXPR");
    case NS_AST_BINARY_EXPR:
        return ns_str_cstr("NS_AST_BINARY_EXPR");
    case NS_AST_UNARY_EXPR:
        return ns_str_cstr("NS_AST_UNARY_EXPR");
    case NS_AST_PRIMARY_EXPR:
        return ns_str_cstr("NS_AST_PRIMARY_EXPR");
    case NS_AST_CALL_EXPR:
        return ns_str_cstr("NS_AST_CALL_EXPR");
    case NS_AST_DESIGNATED_EXPR:
        return ns_str_cstr("NS_AST_DESIGNATED_EXPR");
    case NS_AST_MEMBER_EXPR:
        return ns_str_cstr("NS_AST_MEMBER_EXPR");
    case NS_AST_GEN_EXPR:
        return ns_str_cstr("NS_AST_GEN_EXPR");
    case NS_AST_IF_STMT:
        return ns_str_cstr("NS_AST_IF_STMT");
    case NS_AST_FOR_STMT:
        return ns_str_cstr("NS_AST_FOR_STMT");
    case NS_AST_LOOP_STMT:
        return ns_str_cstr("NS_AST_LOOP_STMT");
    case NS_AST_RETURN_STMT:
        return ns_str_cstr("NS_AST_RETURN_STMT");
    case NS_AST_JUMP_STMT:
        return ns_str_cstr("NS_AST_JUMP_STMT");
    case NS_AST_COMPOUND_STMT:
        return ns_str_cstr("NS_AST_COMPOUND_STMT");
    case NS_AST_DESIGNATED_STMT:
        return ns_str_cstr("NS_AST_DESIGNATED_STMT");
    case NS_AST_IMPORT_STMT:
        return ns_str_cstr("NS_AST_IMPORT_STMT");
    case NS_AST_CAST_EXPR:
        return ns_str_cstr("NS_AST_CAST_EXPR");
    default:
        ns_error("ast", "unknown type %d\n", type);
        return ns_str_cstr("NS_AST_UNKNOWN");
    }
}

void ns_ast_dump(ns_ast_ctx *ctx, int i) {
    ns_ast_t n = ctx->nodes[i];
    ns_str type = ns_ast_type_to_string(n.type);
    printf("%4d [type: %-23.*s next: %4d] ", i, type.len, type.data, n.next);
    switch (n.type) {
    case NS_AST_FN_DEF: {
        printf(ns_color_log "fn " ns_color_nil);
        ns_str_printf(n.fn_def.name.val);
        printf(" (");
        ns_ast_t *arg = &n;
        for (int i = 0; i < n.fn_def.arg_count; i++) {
            arg = &ctx->nodes[arg->next];
            if (arg->arg.is_ref) {
                printf("ref ");
            }

            ns_str_printf(arg->arg.name.val);
            printf(": ");
            ns_str_printf(arg->arg.type.val);
            if (i != n.fn_def.arg_count - 1) {
                printf(", ");
            }
        }
        printf(")");
        if (n.fn_def.return_type.type != NS_TOKEN_UNKNOWN) {
            printf(": ");
            ns_str_printf(n.fn_def.return_type.val);
        }

        if (n.fn_def.body != -1)
            printf(" { node[%d] }", n.fn_def.body);
        else {
            printf(";");
        }
    } break;
    case NS_AST_OPS_FN_DEF: {
        if (n.ops_fn_def.is_async) {
            printf("async ");
        }
        printf(ns_color_log "fn" ns_color_nil " ops (" ns_color_wrn);
        ns_str_printf(n.ops_fn_def.ops.val);
        printf(ns_color_nil ")(");
        ns_ast_t *left = &ctx->nodes[n.ops_fn_def.left];
        ns_str_printf(left->arg.name.val);
        printf(": ");
        ns_str_printf(left->arg.type.val);
        printf(", ");
        ns_ast_t *right = &ctx->nodes[n.ops_fn_def.right];
        ns_str_printf(right->arg.name.val);
        printf(": ");
        ns_str_printf(right->arg.type.val);
        printf(")");
        if (n.ops_fn_def.return_type.type != NS_TOKEN_UNKNOWN) {
            printf(": ");
            ns_str_printf(n.ops_fn_def.return_type.val);
        }
        if (n.ops_fn_def.body != -1) {
            printf(" { node[%d] }", n.ops_fn_def.body);
        }
    } break;
    case NS_AST_STRUCT_DEF: {
        printf(ns_color_log "struct " ns_color_nil);
        ns_str_printf(n.struct_def.name.val);
        printf(" { ");
        int count = n.struct_def.count;
        ns_ast_t field = n;
        for (int i = 0; i < count; i++) {
            field = ctx->nodes[field.next];
            ns_str_printf(field.arg.name.val);
            printf(": ");
            ns_str_printf(field.arg.type.val);
            if (i != count - 1) {
                printf(", ");
            }
        }
        printf(" }");
    } break;
    case NS_AST_ARG_DEF:
        if (n.arg.is_ref) {
            printf("ref ");
        }
        ns_str_printf(n.arg.name.val);
        if (n.arg.type.type != NS_TOKEN_UNKNOWN) {
            printf(": ");
            ns_str_printf(n.arg.type.val);
        }
        break;
    case NS_AST_VAR_DEF:
        ns_str_printf(n.var_def.name.val);
        if (n.var_def.type.type != NS_TOKEN_INVALID) {
            printf(":");
            ns_str_printf(n.var_def.type.val);
        }
        if (n.var_def.expr != -1) {
            printf(" = node[%d]", n.var_def.expr);
        }
        break;
    case NS_AST_EXPR:
        printf("node[%d]", n.expr.body);
        break;
    case NS_AST_CAST_EXPR:
        printf("node[%d] as ", n.cast_expr.expr);
        ns_str_printf(n.cast_expr.type.val);
        break;
    case NS_AST_PRIMARY_EXPR: {
        if (n.primary_expr.token.type == NS_TOKEN_UNKNOWN) {
            printf("node[%d]", n.primary_expr.expr);
        } else {
            ns_str_printf(n.primary_expr.token.val);
        }
    } break;
    case NS_AST_BINARY_EXPR:
        printf("node[%d] ", n.binary_expr.left);
        ns_str_printf(n.binary_expr.op.val);
        printf(" node[%d]", n.binary_expr.right);
        break;
    case NS_AST_UNARY_EXPR:
        ns_str_printf(n.unary_expr.op.val);
        printf(" node[%d]", n.unary_expr.expr);
        break;
    case NS_AST_DESIGNATED_EXPR: {
        ns_str_printf(n.designated_expr.name.val);
        printf(" = node[%d]", n.designated_expr.expr);
    } break;
    case NS_AST_MEMBER_EXPR: {
        printf("node[%d].", n.member_expr.left);
        ns_str_printf(n.member_expr.right.val);
    } break;
    case NS_AST_CALL_EXPR:
        printf("node[%d]", n.call_expr.callee);
        printf("(");
        ns_ast_t *arg = &n;
        for (int i = 0; i < n.call_expr.arg_count; i++) {
            printf("node[%d]", arg->next);
            arg = &ctx->nodes[arg->next];
            if (i != n.call_expr.arg_count - 1) {
                printf(", ");
            }
        }
        printf(")");
        break;
    case NS_AST_JUMP_STMT:
        ns_str_printf(n.jump_stmt.label.val);
        if (n.jump_stmt.expr != -1) {
            printf(" node[%d]", n.jump_stmt.expr);
        }
        break;
    case NS_AST_COMPOUND_STMT: {
        printf("{ ");
        if (n.compound_stmt.count == 0) {
            printf("}");
            break;
        }
        ns_ast_t *stmt = &n;
        for (int i = 0; i < n.compound_stmt.count; i++) {
            printf("node[%d]", stmt->next);
            stmt = &ctx->nodes[stmt->next];
            if (i != n.compound_stmt.count - 1) {
                printf(", ");
            }
        }
        printf(" }");
    } break;
    case NS_AST_IF_STMT: {
        printf("if (node[%d]) node[%d]", n.if_stmt.condition, n.if_stmt.body);
        if (n.if_stmt.else_body != -1) {
            printf(" else node[%d]", n.if_stmt.else_body);
        }
    } break;
    case NS_AST_DESIGNATED_STMT: {
        ns_str_printf(n.designated_stmt.name.val);
        printf(" { ");
        int count = n.designated_stmt.count;
        ns_ast_t *field = &n;
        for (int i = 0; i < count; i++) {
            field = &ctx->nodes[field->next];
            ns_str_printf(field->designated_expr.name.val);
            printf(": node[%d]", field->designated_expr.expr);
            if (i != count - 1) {
                printf(", ");
            }
        }
        printf(" }");
    } break;
    case NS_AST_GEN_EXPR: {
        ns_str_printf(n.gen_expr.name.val);
        if (n.gen_expr.range) {
            printf(" in node[%d] to node[%d]", n.gen_expr.from, n.gen_expr.to);
        } else {
            printf(" in node[%d]", n.gen_expr.from);
        }
    } break;
    case NS_AST_FOR_STMT: {
        printf(ns_color_log "for " ns_color_nil);
        ns_ast_t gen = ctx->nodes[n.for_stmt.generator];
        ns_str_printf(gen.gen_expr.name.val);
        if (gen.gen_expr.range) {
            printf(" in node[%d] to node[%d]", gen.gen_expr.from, gen.gen_expr.to);
        } else {
            printf(" in node[%d]", gen.gen_expr.from);
        }
        printf(" { node[%d] }", n.for_stmt.body);
    } break;
    case NS_AST_LOOP_STMT: {
        if (n.loop_stmt.do_first) {
            printf(ns_color_log "do" ns_color_nil " { node[%d] } loop node[%d]", n.loop_stmt.body, n.loop_stmt.condition);
        } else {
            printf(ns_color_log "loop" ns_color_nil " node[%d] { node[%d] }", n.loop_stmt.condition, n.loop_stmt.body);
        }
    } break;
    case NS_AST_IMPORT_STMT: {
        printf(ns_color_log "import " ns_color_nil);
        ns_str_printf(n.import_stmt.lib.val);
    } break;
    default:
        break;
    }
    printf("\n");
}

void ns_ast_ctx_dump(ns_ast_ctx *ctx) {
    ns_info("ast", "node count %zu\n", ns_array_length(ctx->nodes));
    for (int i = 0, l = ns_array_length(ctx->nodes); i < l; i++) {
        ns_ast_dump(ctx, i);
    }

    ns_info("ast", "section count %d\n", ctx->section_end - ctx->section_begin);
    for (int i = ctx->section_begin; i < ctx->section_end; i++) {
        ns_ast_dump(ctx, ctx->sections[i]);
    }
}