#include "ns_parse.h"

const char *ns_ast_type_str(NS_AST_TYPE type) {
    switch (type) {
    case NS_AST_PROGRAM:
        return "AST_PROGRAM";
    case NS_AST_PARAM_DEF:
        return "AST_PARAM_DEF";
    case NS_AST_FN_DEF:
        return "AST_FN_DEF";
    case NS_AST_VAR_DEF:
        return "AST_VAR_DEF";
    case NS_AST_STRUCT_DEF:
        return "AST_STRUCT_DEF";
    case NS_AST_BINARY_EXPR:
        return "AST_BINARY_EXPR";
    case NS_AST_PRIMARY_EXPR:
        return "AST_PRIMARY_EXPR";
    case NS_AST_CALL_EXPR:
        return "AST_CALL_EXPR";
    case NS_AST_DESIGNATED_EXPR:
        return "AST_DESIGNATED_EXPR";
    case NS_AST_MEMBER_EXPR:
        return "AST_MEMBER_EXPR";
    case NS_AST_IF_STMT:
        return "AST_IF_STMT";
    case NS_AST_FOR_STMT:
        return "AST_FOR_STMT";
    case NS_AST_WHILE_STMT:
        return "AST_WHILE_STMT";
    case NS_AST_RETURN_STMT:
        return "AST_RETURN_STMT";
    case NS_AST_JUMP_STMT:
        return "AST_JUMP_STMT";
    case NS_AST_COMPOUND_STMT:
        return "AST_COMPOUND_STMT";
    case NS_AST_GENERATOR_EXPR:
        return "AST_GENERATOR_EXPR";
    case NS_AST_STRUCT_FIELD_DEF:
        return "AST_STRUCT_FIELD_DEF";
    case NS_AST_DESIGNATED_STMT:
        return "AST_DESIGNATED_STMT";
    case NS_AST_OPS_FN_DEF:
        return "AST_OPS_FN_DEF";
    case NS_AST_TYPE_DEF:
        return "AST_TYPE_DEF";
    default:
        return "AST_UNKNOWN";
    }
}

void ns_ast_dump(ns_parse_context_t *ctx, int i) {
    ns_ast_t n = ctx->nodes[i];
    printf("%4d [type: %-21s next: %5d] ", i, ns_ast_type_str(n.type), n.next);
    switch (n.type) {
    case NS_AST_FN_DEF:
        ns_str_printf(n.fn_def.name.val);
        printf(" (");
        for (int i = 0; i < n.fn_def.param_count; i++) {
            ns_ast_t p = ctx->nodes[n.fn_def.params[i]];
            if (p.param.is_ref) {
                printf("ref ");
            }

            ns_str_printf(p.param.name.val);
            printf(":");
            ns_str_printf(p.param.type.val);
            if (i != n.fn_def.param_count - 1) {
                printf(", ");
            }
        }
        printf(")");
        if (n.fn_def.return_type.type != NS_TOKEN_UNKNOWN) {
            printf(" -> ");
            ns_str_printf(n.fn_def.return_type.val);
        }

        if (n.fn_def.body != -1)
            printf(" { node[%d] }", n.fn_def.body);
        else {
            printf(";");
        }
        break;
    case NS_AST_STRUCT_DEF: {
        printf("struct ");
        ns_str_printf(n.struct_def.name.val);
        printf(" { ");
        int count = n.struct_def.count;
        ns_ast_t *last = &n;
        for (int i = 0; i < count; i++) {
            ns_ast_t *field = &ctx->nodes[last->next];
            ns_str_printf(field->param.name.val);
            printf(":");
            ns_str_printf(field->param.type.val);
            if (i != count - 1) {
                printf(", ");
            }
            last = field;
        }
        printf(" }");
    } break;
    case NS_AST_PARAM_DEF:
        if (n.param.is_ref) {
            printf("ref ");
        }
        ns_str_printf(n.param.name.val);
        if (n.param.type.type != NS_TOKEN_UNKNOWN) {
            printf(": ");
            ns_str_printf(n.param.type.val);
        }
        break;
    case NS_AST_VAR_DEF:
        ns_str_printf(n.var_def.name.val);
        if (n.var_def.type.type != NS_TOKEN_UNKNOWN) {
            printf(":");
            ns_str_printf(n.var_def.type.val);
        }
        if (n.var_def.expr != -1) {
            printf(" node[%d]", n.var_def.expr);
        }
        break;
    case NS_AST_OPS_FN_DEF: {
        if (n.ops_fn_def.is_async) {
            printf("async ");
        }
        printf("fn ops ");
        ns_str_printf(n.ops_fn_def.ops.val);
        printf("(");
        ns_ast_t *left = &ctx->nodes[n.ops_fn_def.left];
        ns_str_printf(left->param.name.val);
        printf(":");
        ns_str_printf(left->param.type.val);
        printf(", ");
        ns_ast_t *right = &ctx->nodes[n.ops_fn_def.right];
        ns_str_printf(right->param.name.val);
        printf(":");
        ns_str_printf(right->param.type.val);
        printf(")");
        if (n.ops_fn_def.return_type.type != NS_TOKEN_UNKNOWN) {
            printf(" -> ");
            ns_str_printf(n.ops_fn_def.return_type.val);
        }
        if (n.ops_fn_def.body != -1) {
            printf(" { node[%d] }", n.ops_fn_def.body);
        }
    } break;
    case NS_AST_PRIMARY_EXPR:
        ns_str_printf(n.primary_expr.token.val);
        break;
    case NS_AST_BINARY_EXPR:
        printf("node[%d] ", n.binary_expr.left);
        ns_str_printf(n.binary_expr.op.val);
        printf(" node[%d]", n.binary_expr.right);
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
        printf("node[%d]", n.var_def.expr);
        printf("(");
        ns_ast_t *last = &n;
        for (int i = 0; i < n.call_expr.arg_count; i++) {
            ns_ast_t *arg = &ctx->nodes[last->next];
            printf("node[%d]", last->next);
            if (i != n.call_expr.arg_count - 1) {
                printf(", ");
            }
            last = arg;
        }
        printf(")");
        break;
    case NS_AST_GENERATOR_EXPR: {
        printf("let ");
        ns_str_printf(n.generator.label.val);
        printf(" = ");
        if (n.generator.from != -1) {
            printf("node[%d]", n.generator.from);
        } else {
            ns_str_printf(n.generator.token.val);
        }
        if (n.generator.to != -1) {
            printf(" to node[%d]", n.generator.to);
        }
    } break;
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
        ns_ast_t *last = &n;
        for (int i = 0; i < n.compound_stmt.count; i++) {
            ns_ast_t *stmt = &ctx->nodes[last->next];
            printf("node[%d]", last->next);
            if (i != n.compound_stmt.count - 1) {
                printf(", ");
            }
            last = stmt;
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
        ns_ast_t *last = &n;
        for (int i = 0; i < count; i++) {
            ns_ast_t *field = &ctx->nodes[last->next];
            ns_str_printf(field->designated_expr.name.val);
            printf(": node[%d]", field->designated_expr.expr);
            if (i != count - 1) {
                printf(", ");
            }
            last = field;
        }
        printf(" }");
    } break;
    case NS_AST_FOR_STMT: {
        printf("for node[%d] { node[%d] }", n.for_stmt.generator, n.for_stmt.body);
    } break;
    case NS_AST_WHILE_STMT: {
        printf("while node[%d] { node[%d] }", n.while_stmt.condition, n.while_stmt.body);
    } break;
    default:
        break;
    }
    printf("\n");
}

void ns_parse_context_dump(ns_parse_context_t *ctx) {
    printf("AST:\n");

    for (int i = 0, l = ctx->node_count; i < l; i++) {
        ns_ast_dump(ctx, i);
    }

    printf("Sections:\n");
    for (int i = 0, l = ctx->section_count; i < l; i++) {
        ns_ast_dump(ctx, ctx->sections[i]);
    }
}