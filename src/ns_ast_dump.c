#include "ns_ast.h"

#define NS_STR_CASE(type) case type: return ns_str_cstr(#type);

ns_str ns_ast_type_to_string(NS_AST_TYPE type) {
    switch (type) {
        ns_str_case(NS_AST_PROGRAM)
        ns_str_case(NS_AST_ARG_DEF)
        ns_str_case(NS_AST_FN_DEF)
        ns_str_case(NS_AST_VAR_DEF)
        ns_str_case(NS_AST_STRUCT_DEF)
        ns_str_case(NS_AST_OPS_FN_DEF)
        ns_str_case(NS_AST_TYPE_DEF)
        ns_str_case(NS_AST_STRUCT_FIELD_DEF)
        ns_str_case(NS_AST_EXPR)
        ns_str_case(NS_AST_BINARY_EXPR)
        ns_str_case(NS_AST_UNARY_EXPR)
        ns_str_case(NS_AST_PRIMARY_EXPR)
        ns_str_case(NS_AST_CALL_EXPR)
        ns_str_case(NS_AST_DESIG_EXPR)
        ns_str_case(NS_AST_FIELD_DEF)
        ns_str_case(NS_AST_MEMBER_EXPR)
        ns_str_case(NS_AST_GEN_EXPR)
        ns_str_case(NS_AST_IF_STMT)
        ns_str_case(NS_AST_FOR_STMT)
        ns_str_case(NS_AST_LOOP_STMT)
        ns_str_case(NS_AST_RETURN_STMT)
        ns_str_case(NS_AST_JUMP_STMT)
        ns_str_case(NS_AST_COMPOUND_STMT)
        ns_str_case(NS_AST_IMPORT_STMT)
        ns_str_case(NS_AST_CAST_EXPR)
    default:
        ns_error("ast", "unknown type %d\n", type);
        return ns_str_cstr("NS_AST_UNKNOWN");
    }
}

void ns_ast_dump(ns_ast_ctx *ctx, i32 i) {
    ns_ast_t n = ctx->nodes[i];
    ns_str type = ns_ast_type_to_string(n.type);
    printf("%4d [type: %-20.*s next: %4d] ", i, type.len, type.data, n.next);
    switch (n.type) {
    case NS_AST_FN_DEF: {
        printf(ns_color_log "fn " ns_color_nil);
        ns_str_printf(n.fn_def.name.val);
        printf(" (");
        ns_ast_t *arg = &n;
        for (i32 i = 0; i < n.fn_def.arg_count; i++) {
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
            printf(" { [%d] }", n.fn_def.body);
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
            printf(" { [%d] }", n.ops_fn_def.body);
        }
    } break;
    case NS_AST_STRUCT_DEF: {
        printf(ns_color_log "struct " ns_color_nil);
        ns_str_printf(n.struct_def.name.val);
        printf(" { ");
        i32 count = n.struct_def.count;
        ns_ast_t field = n;
        for (i32 i = 0; i < count; i++) {
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
        printf(ns_color_log "let " ns_color_nil);
        ns_str_printf(n.var_def.name.val);
        if (n.var_def.type.type != NS_TOKEN_INVALID) {
            printf(":");
            ns_str_printf(n.var_def.type.val);
        }
        if (n.var_def.expr != -1) {
            printf(" = [%d]", n.var_def.expr);
        }
        break;
    case NS_AST_EXPR:
        printf("[%d]", n.expr.body);
        break;
    case NS_AST_CAST_EXPR:
        printf("[%d] as ", n.cast_expr.expr);
        ns_str_printf(n.cast_expr.type.val);
        break;
    case NS_AST_PRIMARY_EXPR: {
        if (n.primary_expr.token.type == NS_TOKEN_UNKNOWN) {
            printf("[%d]", n.primary_expr.expr);
        } else {
            ns_str_printf(n.primary_expr.token.val);
        }
    } break;
    case NS_AST_BINARY_EXPR:
        printf("[%d] ", n.binary_expr.left);
        ns_str_printf(n.binary_expr.op.val);
        printf(" [%d]", n.binary_expr.right);
        break;
    case NS_AST_UNARY_EXPR:
        ns_str_printf(n.unary_expr.op.val);
        printf(" [%d]", n.unary_expr.expr);
        break;
    case NS_AST_FIELD_DEF: {
        ns_str_printf(n.field_def.name.val);
        printf(" = [%d]", n.field_def.expr);
    } break;
    case NS_AST_MEMBER_EXPR: {
        printf("[%d].", n.member_expr.left);
        ns_str_printf(n.member_expr.right.val);
    } break;
    case NS_AST_CALL_EXPR: {
        printf("[%d]", n.call_expr.callee);
        printf("(");
        i32 next = n.call_expr.arg;
        for (i32 i = 0; i < n.call_expr.arg_count; i++) {
            printf("[%d]", next);
            next = ctx->nodes[next].next;
            if (i != n.call_expr.arg_count - 1) {
                printf(", ");
            }
        }
        printf(")");
    } break;
    case NS_AST_JUMP_STMT:
        ns_str_printf(n.jump_stmt.label.val);
        if (n.jump_stmt.expr != -1) {
            printf(" [%d]", n.jump_stmt.expr);
        }
        break;
    case NS_AST_COMPOUND_STMT: {
        printf("{ ");
        if (n.compound_stmt.count == 0) {
            printf("}");
            break;
        }
        ns_ast_t *stmt = &n;
        for (i32 i = 0; i < n.compound_stmt.count; i++) {
            printf("[%d]", stmt->next);
            stmt = &ctx->nodes[stmt->next];
            if (i != n.compound_stmt.count - 1) {
                printf(", ");
            }
        }
        printf(" }");
    } break;
    case NS_AST_IF_STMT: {
        printf(ns_color_log "if" ns_color_nil " ([%d]) [%d]", n.if_stmt.condition, n.if_stmt.body);
        if (n.if_stmt.else_body != -1) {
            printf(" " ns_color_log "else" ns_color_nil " [%d]", n.if_stmt.else_body);
        }
    } break;
    case NS_AST_DESIG_EXPR: {
        ns_str_printf(n.desig_expr.name.val);
        printf(" { ");
        i32 count = n.desig_expr.count;
        ns_ast_t *field = &n;
        for (i32 i = 0; i < count; i++) {
            field = &ctx->nodes[field->next];
            ns_str_printf(field->field_def.name.val);
            printf(": [%d]", field->field_def.expr);
            if (i != count - 1) {
                printf(", ");
            }
        }
        printf(" }");
    } break;
    case NS_AST_GEN_EXPR: {
        ns_str_printf(n.gen_expr.name.val);
        if (n.gen_expr.range) {
            printf(" " ns_color_log "in" ns_color_nil " [%d] "ns_color_log "to" ns_color_nil " [%d]", n.gen_expr.from, n.gen_expr.to);
        } else {
            printf(" " ns_color_log "in" ns_color_nil " [%d]", n.gen_expr.from);
        }
    } break;
    case NS_AST_FOR_STMT: {
        printf(ns_color_log "for " ns_color_nil);
        ns_ast_t gen = ctx->nodes[n.for_stmt.generator];
        ns_str_printf(gen.gen_expr.name.val);
        if (gen.gen_expr.range) {
            printf(" " ns_color_log "in" ns_color_nil " [%d] " ns_color_log "to" ns_color_nil " [%d]", gen.gen_expr.from, gen.gen_expr.to);
        } else {
            printf(" " ns_color_log "in" ns_color_nil " [%d]", gen.gen_expr.from);
        }
        printf(" { [%d] }", n.for_stmt.body);
    } break;
    case NS_AST_LOOP_STMT: {
        if (n.loop_stmt.do_first) {
            printf(ns_color_log "do" ns_color_nil " { [%d] } loop [%d]", n.loop_stmt.body, n.loop_stmt.condition);
        } else {
            printf(ns_color_log "loop" ns_color_nil " [%d] { [%d] }", n.loop_stmt.condition, n.loop_stmt.body);
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
    for (i32 i = 0, l = ns_array_length(ctx->nodes); i < l; i++) {
        ns_ast_dump(ctx, i);
    }

    ns_info("ast", "section count %d\n", ctx->section_end - ctx->section_begin);
    for (i32 i = ctx->section_begin; i < ctx->section_end; i++) {
        ns_ast_dump(ctx, ctx->sections[i]);
    }
}