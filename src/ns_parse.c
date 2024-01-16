#include "ns_type.h"
#include "ns_parse.h"
#include "ns_tokenize.h"

#include <stdlib.h>
#include "stb_ds.h"

bool ns_stmt(ns_parse_context_t *ctx);
bool ns_expr(ns_parse_context_t *ctx);
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
bool ns_selection_stmt(ns_parse_context_t *ctx);
bool ns_labeled_stmt(ns_parse_context_t *ctx);

void ns_restore_state(ns_parse_context_t *ctx, int f) {
    ctx->f = f;
    ctx->token.type = NS_TOKEN_UNKNOWN;
}

int ns_save_state(ns_parse_context_t *ctx) {
    return ctx->f;
}

bool ns_next_token(ns_parse_context_t *ctx) {
    ctx->f = ns_tokenize(&ctx->token, ctx->source, ctx->filename, ctx->f);
    return ctx->token.type != NS_TOKEN_EOF;
}

bool ns_labeled_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // case constant-expression : statement
    if (ctx->token.type == NS_TOKEN_CASE) {
        ns_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_INT_LITERAL) {
            ns_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_COLON) {
                ns_next_token(ctx);
                if (ns_stmt(ctx)) {
                    return true;
                }
            }
        }
    }
    ns_restore_state(ctx, state);

    // default : statement
    if (ctx->token.type == NS_TOKEN_DEFAULT) {
        ns_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_COLON) {
            ns_next_token(ctx);
            if (ns_stmt(ctx)) {
                return true;
            }
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_type_restriction(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    // : type
    if (ctx->token.type == NS_TOKEN_COLON) {
        ns_next_token(ctx);
        if (ns_type_expr(ctx)) {
            return true;
        }
    }

    ns_restore_state(ctx, state);
    return true; // allow empty type declare
}

bool ns_token_require(ns_parse_context_t *ctx, NS_TOKEN token) {
    int state = ns_save_state(ctx);
    ns_next_token(ctx);
    if (ctx->token.type == token) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_token_require_optional(ns_parse_context_t *ctx, NS_TOKEN token) {
    int state = ns_save_state(ctx);
    ns_next_token(ctx);
    if (ctx->token.type == token) {
        return true;
    }
    ns_restore_state(ctx, state);
    return true;
}

bool ns_selection_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // if ( expression ) statement
    if (ctx->token.type == NS_TOKEN_IF) {
        ns_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
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
    if (ctx->token.type == NS_TOKEN_IF) {
        ns_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
                    ns_next_token(ctx);
                    if (ns_stmt(ctx)) {
                        if (ctx->token.type == NS_TOKEN_ELSE) {
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
    if (ctx->token.type == NS_TOKEN_SWITCH) {
        ns_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
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

bool ns_type_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // type
    if (ctx->token.type == NS_TOKEN_TYPE) {
        ns_next_token(ctx);
        return true;
    }
    ns_restore_state(ctx, state);

    // identifier
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
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
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                if (ctx->token.type == NS_TOKEN_COLON) {
                    ns_next_token(ctx);
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
    ns_next_token(ctx);
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
    ns_next_token(ctx);
    if (ctx->token.type == NS_TOKEN_IDENTIFIER) {
        return true;
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_primary_expr(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_next_token(ctx);

    // literal
    switch (ctx->token.type) {
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
    if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
        ns_next_token(ctx);
        if (ns_expr(ctx)) {
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
    if (ns_postfix_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
            ns_next_token(ctx);
            if (ctx->token.type == NS_TOKEN_CLOSE_PAREN) {
                return true;
            }
        }
    }

    // post_fix_expr ( )
    if (ns_postfix_expr(ctx)) {
        ns_next_token(ctx);
        if (ctx->token.type == NS_TOKEN_OPEN_PAREN) {
            if (ns_assign_expr(ctx)) {
                ns_next_token(ctx);
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
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
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
            ns_next_token(ctx);
            if (ns_identifier(ctx)) {
                return true;
            }
        }
    }

    // post_fix_expr as type
    ns_restore_state(ctx, state);
    if (ns_postfix_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_AS) {
            ns_next_token(ctx);
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
        if (ctx->token.type == NS_TOKEN_ASSIGN_OPERATOR) {
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
        if (ctx->token.type == NS_TOKEN_ASSIGN) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                return true;
            }
        }
    }
    // assignment-expression , expression
    ns_restore_state(ctx, state);

    if (ns_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_COMMA) {
            ns_next_token(ctx);
            if (ns_expr(ctx)) {
                return true;
            }
        }
    }

    return false;
}

bool ns_iteration_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    
    // while expr { stmt }
    if (ctx->token.type == NS_TOKEN_WHILE) {
        ns_next_token(ctx);
        if (ns_expr(ctx)) {
            if (ns_stmt(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    // do { stmt } while expr
    if (ctx->token.type == NS_TOKEN_DO) {
        ns_next_token(ctx);
        if (ns_stmt(ctx)) {
            if (ctx->token.type == NS_TOKEN_WHILE) {
                ns_next_token(ctx);
                if (ns_expr(ctx)) {
                    return true;
                }
            }
        }
    }

    ns_restore_state(ctx, state);
    // for identifier int to in { stmt }
    if (ctx->token.type == NS_TOKEN_FOR) {
        ns_next_token(ctx);
        if (ns_identifier(ctx)) {
            if (ctx->token.type == NS_TOKEN_INT_LITERAL) {
                ns_next_token(ctx);
                if (ctx->token.type == NS_TOKEN_TO) {
                    ns_next_token(ctx);
                    if (ctx->token.type == NS_TOKEN_IN) {
                        ns_next_token(ctx);
                        if (ns_stmt(ctx)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    ns_restore_state(ctx, state);
    // for identifier [type_declare] in identifier { stmt }
    if (ctx->token.type == NS_TOKEN_FOR) {
        ns_next_token(ctx);
        if (ns_identifier(ctx)) {
            if (ns_type_restriction(ctx)) {
                if (ctx->token.type == NS_TOKEN_IN) {
                    ns_next_token(ctx);
                    if (ns_identifier(ctx)) {
                        if (ns_stmt(ctx)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_parameter(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    ns_token_require_optional(ctx, NS_TOKEN_REF);
    if (ns_identifier(ctx)) {
        if (ns_type_restriction(ctx)) {
            return true;
        }
    }
    ns_restore_state(ctx, state);
    return false;
}

bool ns_parameters(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    if (ns_parameter(ctx)) {
        if (ctx->token.type == NS_TOKEN_COMMA) {
            ns_next_token(ctx);
            if (ns_parameters(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return true; // allow empty parameters
}

bool ns_fn_declaration(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // [async] fn identifier ( [type_declare identifier] ) [type_declare] { stmt }
    ns_token_require_optional(ctx, NS_TOKEN_ASYNC);

    if (!ns_token_require_optional(ctx, NS_TOKEN_FN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }
    ns_token_t fn_name = ctx->token;

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_PAREN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_parameters(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_PAREN)) {
        ns_restore_state(ctx, state);
        return false;
    }

    // optional
    ns_type_restriction(ctx);

    if (!ns_token_require(ctx, NS_TOKEN_OPEN_BRACE)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_stmt(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }

    if (!ns_token_require(ctx, NS_TOKEN_CLOSE_BRACE)) {
        ns_restore_state(ctx, state);
        return false;
    }

    return true;
}

bool ns_struct_declaration(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // struct identifier { stmt }
    if (ctx->token.type == NS_TOKEN_STRUCT) {
        ns_next_token(ctx);
        if (ns_identifier(ctx)) {
            if (ns_parameters(ctx)) {
                return true;
            }
        }
    }

    ns_restore_state(ctx, state);
    return false;
}

bool ns_variable_declaration(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // identifier [type_declare] = expression
    if (!ns_token_require(ctx, NS_TOKEN_LET)) {
        ns_restore_state(ctx, state);
        return false;
    }
    
    if (!ns_identifier(ctx)) {
        ns_restore_state(ctx, state);
        return false;
    }

    ns_token_t identifier = ctx->token;


    return true;
}

bool ns_declaration_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);
    // fn_declaration
    if (ns_fn_declaration(ctx)) {
        return true;
    }

    // struct declaration

    // variable declaration
    return false;
}

bool ns_stmt(ns_parse_context_t *ctx) {
    int state = ns_save_state(ctx);

    ns_ast_t stmt;
    arrpush(ctx->program, stmt);

    // expression statement
    if (ns_expr(ctx)) {
        if (ctx->token.type == NS_TOKEN_SEMICOLON) {
            ns_next_token(ctx);
            return true;
        }
    }

    ns_restore_state(ctx, state);
    // selection statement
    if (ns_selection_stmt(ctx)) {
        arrpush(ctx->program, stmt);
        return true;
    }

    ns_restore_state(ctx, state);
    // iteration statement
    if (ns_iteration_stmt(ctx)) {
        return true;
    }

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
