#include "ns_type.h"
#include "ns_parse.h"
#include "ns_tokenize.h"

#include <stdio.h>
#include <stdlib.h>
#include "stb_ds.h"

bool ns_assign_expr(ns_parse_context_t *ctx);
bool ns_unary_expr(ns_parse_context_t *ctx);
bool ns_postfix_expr(ns_parse_context_t *ctx);
bool ns_primary_expr(ns_parse_context_t *ctx);
bool ns_logical_expr(ns_parse_context_t *ctx);
bool ns_logic_or_expr(ns_parse_context_t *ctx);
bool ns_inclusive_or_expr(ns_parse_context_t *ctx);
bool ns_exclusive_or_expr(ns_parse_context_t *ctx);
bool ns_and_expr(ns_parse_context_t *ctx);
bool ns_equality_expr(ns_parse_context_t *ctx);
bool ns_relational_expr(ns_parse_context_t *ctx);
bool ns_shift_expr(ns_parse_context_t *ctx);
bool ns_additive_expr(ns_parse_context_t *ctx);
bool ns_multiplicative_expr(ns_parse_context_t *ctx);
bool ns_type_expr(ns_parse_context_t *ctx);

bool ns_token_require(ns_parse_context_t *ctx, NS_TOKEN token);
bool ns_fn_define(ns_parse_context_t *ctx);
bool ns_struct_define(ns_parse_context_t *ctx);
bool ns_variable_define(ns_parse_context_t *ctx);
bool ns_parameter(ns_parse_context_t *ctx);

void ns_restore_state(ns_parse_context_t *ctx, int f) {
    ctx->f = f;
    ctx->token.type = NS_TOKEN_UNKNOWN;
}

int ns_save_state(ns_parse_context_t *ctx) {
    return ctx->f;
}

ns_ast_t *ns_ast_emplace(ns_parse_context_t *ctx, NS_AST_TYPE type) {
    arrpush(ctx->nodes, (ns_ast_t){.type = type});
    ns_ast_t *node = ctx->nodes + arrlen(ctx->nodes) - 1;
    ctx->current = node;
    return node;
}

void ns_ast_pop(ns_parse_context_t *ctx) {
    arrpop(ctx->nodes);
    return;
}

bool ns_parse_next_token(ns_parse_context_t *ctx) {
    do {
        ctx->last_f = ctx->f;
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

bool ns_parse_constant_expr(ns_parse_context_t *ctx) {
    return false;
}

bool ns_type_restriction(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // : type
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_COLON) {
        if (ns_type_expr(ctx)) {
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return false; // allow empty type declare
}

bool ns_type_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // type
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_TYPE) {
        return true;
    }
    ns_restore_state(ctx, state);

    // identifier
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        ns_parse_next_token(ctx);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_multiplicative_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // unary-expression
    if (ns_unary_expr(ctx)) {
        return true;
    }

    // multiplicative-expression [*|/|%] unary-expression
    ns_restore_state(ctx, state);
    if (ns_multiplicative_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_MULTIPLICATIVE_OPERATOR) {
            ns_parse_next_token(ctx);
            if (ns_unary_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_additive_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // multiplicative-expression
    if (ns_multiplicative_expr(ctx)) {
        return true;
    }

    // additive-expression [+|-] multiplicative-expression
    ns_restore_state(ctx, state);
    if (ns_additive_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_ADDITIVE_OPERATOR) {
            ns_parse_next_token(ctx);
            if (ns_multiplicative_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_shift_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // additive-expression
    if (ns_additive_expr(ctx)) {
        return true;
    }

    // shift-expression [<<|>>] additive-expression
    ns_restore_state(ctx, state);
    if (ns_shift_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_SHIFT_OPERATOR) {
            ns_parse_next_token(ctx);
            if (ns_additive_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_relational_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // shift-expression
    if (ns_shift_expr(ctx)) {
        return true;
    }

    // relational-expression [<|>|<=|>=] shift-expression
    ns_restore_state(ctx, state);
    if (ns_relational_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_BOOL_OPERATOR) {
            ns_parse_next_token(ctx);
            if (ns_shift_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_equality_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // relational-expression
    if (ns_relational_expr(ctx)) {
        return true;
    }

    // equality-expression [==|!=] relational-expression
    ns_restore_state(ctx, state);
    if (ns_equality_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_BOOL_OPERATOR) {
            ns_parse_next_token(ctx);
            if (ns_relational_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_and_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // equality-expression
    if (ns_equality_expr(ctx)) {
        return true;
    }

    // and-expression & equality-expression
    ns_restore_state(ctx, state);
    if (ns_and_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_BITWISE_OPERATOR && ctx->token.val.data[0] == '&') {
            ns_parse_next_token(ctx);
            if (ns_equality_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_exclusive_or_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // and-expression
    if (ns_and_expr(ctx)) {
        return true;
    }

    // exclusive-or-expression ^ and-expression
    ns_restore_state(ctx, state);
    if (ns_exclusive_or_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_BITWISE_OPERATOR && ctx->token.val.data[0] == '^') {
            ns_parse_next_token(ctx);
            if (ns_and_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_inclusive_or_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // exclusive-or-expression
    if (ns_exclusive_or_expr(ctx)) {
        return true;
    }

    // inclusive-or-expression | exclusive-or-expression
    ns_restore_state(ctx, state);
    if (ns_inclusive_or_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_BITWISE_OPERATOR && ctx->token.val.data[0] == '|') {
            ns_parse_next_token(ctx);
            if (ns_exclusive_or_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_logic_and_expr(ns_parse_context_t *ctx) {
    // inclusive-or-expression
    int state = ns_save_state(ctx);
    if (ns_inclusive_or_expr(ctx)) {
        return true;
    }

    // logic-and-expression && inclusive-or-expression
    ns_restore_state(ctx, state);
    if (ns_logic_and_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_LOGICAL_OPERATOR && ctx->token.val.data[0] == '&') {
            ns_parse_next_token(ctx);
            if (ns_inclusive_or_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_logic_or_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // logic-and-expression
    if (ns_logic_and_expr(ctx)) {
        return true;
    }

    // logic-or-expression || logic-and-expression
    ns_restore_state(ctx, state);
    if (ns_logic_or_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_LOGICAL_OPERATOR && ctx->token.val.data[0] == '|') {
            ns_parse_next_token(ctx);
            if (ns_logic_and_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_conditional_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // logical-or-expression
    if (ns_logic_or_expr(ctx)) {
        return true;
    }

    ns_restore_state(ctx, state);
    // logical-or-expression ? expression : conditional-expression
    if (ns_logic_or_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_QUESTION_MARK) {
            ns_parse_next_token(ctx);
            if (ns_parse_expr(ctx)) {
                if (ctx->token.type == NS_TOKEN_COLON) {
                    ns_parse_next_token(ctx);
                    if (ns_conditional_expr(ctx)) {
                        return true;
                    }
                }
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_unary_operator(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_parse_next_token(ctx);
    switch (ctx->token.type) {
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
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_IDENTIFIER);
        n->identifier.token = ctx->token;
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
        ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_LITERAL);
        n->literal.token = ctx->token;
        return true;
    }

    // identifier
    ns_restore_state(ctx, state);
    if (ns_identifier(ctx)) {
        return true;
    }

    // ( expression )
    ns_restore_state(ctx, state);
    ns_parse_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
        if (ns_parse_expr(ctx)) {
            ns_parse_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_postfix_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // post_fix_expr ()
    if (ns_primary_expr(ctx)) {
        return true;
    }

    // post_fix_expr ( )
    if (ns_postfix_expr(ctx)) {
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
            if (ns_assign_expr(ctx)) {
                ns_parse_next_token(ctx);
                if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
                    return true;
                }
            }
        }
    }

    // post_fix_expr [ expression ]
    ns_restore_state(ctx, state);
    if (ns_postfix_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_OPEN_BRACKET) {
            ns_parse_next_token(ctx);
            if (ns_parse_expr(ctx)) {
                if (ctx->token.type == NS_TOKEN_CLOSE_BRACKET) {
                    return true;
                }
            }
        }
    }

    // post_fix_expr . identifier
    ns_restore_state(ctx, state);
    if (ns_postfix_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_DOT) {
            ns_parse_next_token(ctx);
            if (ns_identifier(ctx)) {
                return true;
            }
        }
    }

    // post_fix_expr as type
    ns_restore_state(ctx, state);
    if (ns_postfix_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_AS) {
            ns_parse_next_token(ctx);
            if (ns_type_expr(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
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
    if (ctx->token.type == NS_TOKEN_AWAIT) {
        ns_parse_next_token(ctx);
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
    if (ns_conditional_expr(ctx)) {
        return true;
    }

    // unary-expression assignment-operator assignment-expression
    if (ns_parse_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_ASSIGN_OPERATOR) {
            ns_parse_next_token(ctx);
            if (ns_parse_expr(ctx)) {
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_generator_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // identifier a to b
    // identifier in b
    if (ns_identifier(ctx)) {
        ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_CALL_EXPR);
        n->generator.label = ctx->token;

        int from_state = ns_save_state(ctx);
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_IN) {
            if (ns_parse_expr(ctx)) {
                return true;
            }
        }
        ns_restore_state(ctx, from_state);

        if (ns_parse_expr(ctx)) {
            n->generator.spawn = ctx->current;
            return true;
        }
        ns_ast_pop(ctx);
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_expr(ns_parse_context_t *ctx) {
    // assignment-expression
    int state = ns_save_state(ctx);
    if (ns_assign_expr(ctx)) {
        return true;
    }

    return false;
}

bool ns_parameter(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_PARAM);
    if (ns_token_require(ctx, NS_TOKEN_REF)) {
        n->param.is_ref = true;
    }

    if (!ns_identifier(ctx)) {
        ns_restore_state(ctx, state);
        ns_ast_pop(ctx);
        return false;
    }
    n->param.name = ctx->token;

    if (ns_type_restriction(ctx)) {
        n->param.type = ctx->token;
    }

    ctx->current = n;
    return true;
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

    if (!ns_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_token_t name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_ast_t *fn = ns_ast_emplace(ctx, NS_AST_FN_DEF);
    fn->fn_def.is_async = is_async;
    fn->fn_def.name = name;
    fn->fn_def.param_count = 0;
    // parse parameters
    while (ns_parameter(ctx)) {
        fn->fn_def.params[fn->fn_def.param_count++] = ctx->current;
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            continue;
        } else {
            break;
        }
    }

    if (ctx->token.type != NS_TOKEN_CLOSE_PAREN) {
        ns_restore_state(ctx, state);
        ns_ast_pop(ctx);
        return false;
    }

    // optional
    if(ns_type_restriction(ctx)) {
        fn->fn_def.return_type = ctx->token;
    }

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        ns_ast_pop(ctx);
        return false;
    }

    if (!ns_parse_stmt(ctx)) {
        ns_restore_state(ctx, state);
        ns_ast_pop(ctx);
        return false;
    }
    fn->fn_def.body = ctx->current;

    if (ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
        return true;
    }

    ns_restore_state(ctx, state);
    ns_ast_pop(ctx);
    return false;
}

bool ns_parse_struct_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_STRUCT)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_token_t name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_STRUCT_DEF);
    n->struct_def.name = name;
    while (ns_parameter(ctx)) {
        n->struct_def.fields[n->struct_def.field_count++] = ctx->current;
        ns_parse_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COMMA || ctx->token.type == NS_TOKEN_EOL) {
            return true;
        } else {
            break;
        }
    }

    if (ctx->token.type == NS_TOKEN_CLOSE_BRACE) {
        return true;
    }

    ns_restore_state(ctx, state);
    ns_ast_pop(ctx);
    return false;
}

bool ns_parse_var_define(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // identifier [type_declare] = expression
    if (ns_token_require(ctx, NS_TOKEN_LET) && ns_identifier(ctx)) {
        ns_ast_t *n = ns_ast_emplace(ctx, NS_AST_VAR_DEF);
        n->var_def.name = ctx->token;
        if (ns_type_restriction(ctx)) {
            n->var_def.type = ctx->token;
        }
        int assign_state = ns_save_state(ctx);
        if (ns_token_require(ctx, NS_TOKEN_ASSIGN)) {
            if (ns_parse_expr(ctx)) {
                return true;
            }
        }
        ns_restore_state(ctx, assign_state);
        return true;
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parse_type_define(ns_parse_context_t *ctx) {
    // int state = ns_save_state(ctx);

    // type identifier = type
    // if (ns_token_require(ctx, NS_TOKEN_TYPE) && ns_identifier(ctx)) {
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

ns_parse_context_t* ns_parse(const char *source, const char *filename) {
    ns_parse_context_t* ctx = (ns_parse_context_t*)malloc(sizeof(ns_parse_context_t));
    ctx->source = source;
    ctx->filename = filename;
    ctx->f = 0;
    ctx->last_f = 0;

    while (ns_parse_external_define(ctx)) {
        arrpush(ctx->sections, *ctx->current);
    }

    return ctx;
}

const char * ns_ast_type_str(NS_AST_TYPE type) {
    switch (type) {
        case NS_AST_PROGRAM:
            return "NS_AST_PROGRAM";
        case NS_AST_PARAM:
            return "NS_AST_PARAM";
        case NS_AST_FN_DEF:
            return "NS_AST_FN_DEF";
        case NS_AST_VAR_DEF:
            return "NS_AST_VAR_DEF";
        case NS_AST_STRUCT_DEF:
            return "NS_AST_STRUCT_DEF";
        case NS_AST_BINARY_EXPR:
            return "NS_AST_BINARY_EXPR";
        case NS_AST_LITERAL:
            return "NS_AST_LITERAL";
        case NS_AST_IDENTIFIER:
            return "NS_AST_IDENTIFIER";
        case NS_AST_CALL_EXPR:
            return "NS_AST_CALL_EXPR";
        case NS_AST_IF_STMT:
            return "NS_AST_IF_STMT";
        case NS_AST_ITER_STMT:
            return "NS_AST_ITER_STMT";
        case NS_AST_RETURN_STMT:
            return "NS_AST_RETURN_STMT";
        default:
            return "NS_AST_UNKNOWN";
    }
}

void ns_ast_dump(ns_ast_t *node, int depth) {
    printf("[type:  %s] ", ns_ast_type_str(node->type));
    switch (node->type) {
        case NS_AST_FN_DEF:
            ns_str_printf(node->fn_def.name.val);
            printf(" (");
            for (int i = 0; i < node->fn_def.param_count; i++) {
                ns_str_printf(node->fn_def.params[i]->param.name.val);
                printf(":");
                ns_str_printf(node->fn_def.params[i]->param.type.val);
                if (i != node->fn_def.param_count - 1) {
                    printf(", ");
                }
            }
            printf(")");
            if (node->fn_def.return_type.type != NS_TOKEN_UNKNOWN) {
                printf(" -> ");
                ns_str_printf(node->fn_def.return_type.val);
            }
            break;
        case NS_AST_IDENTIFIER:
            ns_str_printf(node->identifier.token.val);
            break;
        case NS_AST_PARAM:
            if (node->param.is_ref) {
                printf("ref ");
            }
            ns_str_printf(node->param.name.val);
            if (node->param.type.type != NS_TOKEN_UNKNOWN) {
                printf(":");
                ns_str_printf(node->param.type.val);
            }
            break;
        default:
            break;
    }
    printf("\n");
}

void ns_parse_context_dump(ns_parse_context_t *ctx) {
    for (int i = 0; i < arrlen(ctx->nodes); i++) {
        ns_ast_dump(ctx->nodes + i, 0);
    }
}