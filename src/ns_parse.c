#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

void ns_parse_dump_error(ns_parse_context_t *ctx, const char *msg) {
    printf("%s [%s:%d]\n", msg, ctx->filename, ctx->last_token.line);
    assert(false);
}

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