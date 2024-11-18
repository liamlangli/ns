#include "ns_token.h"
#include "ns_type.h"

ns_str ns_token_type_to_string(NS_TOKEN type) {
    switch (type) {
        ns_str_case(NS_TOKEN_AS);
        ns_str_case(NS_TOKEN_INT_LITERAL);
        ns_str_case(NS_TOKEN_FLT_LITERAL);
        ns_str_case(NS_TOKEN_STR_LITERAL);
        ns_str_case(NS_TOKEN_STR_FORMAT);
        ns_str_case(NS_TOKEN_CONST);
        ns_str_case(NS_TOKEN_COMMENT);
        ns_str_case(NS_TOKEN_LET);
        ns_str_case(NS_TOKEN_MATCH);
        ns_str_case(NS_TOKEN_MODULE);
        ns_str_case(NS_TOKEN_FN);
        ns_str_case(NS_TOKEN_IN);
        ns_str_case(NS_TOKEN_SPACE);
        ns_str_case(NS_TOKEN_STRUCT);
        ns_str_case(NS_TOKEN_IDENTIFIER);
        ns_str_case(NS_TOKEN_ASYNC);
        ns_str_case(NS_TOKEN_AWAIT);
        ns_str_case(NS_TOKEN_TYPE_I8);
        ns_str_case(NS_TOKEN_TYPE_I16);
        ns_str_case(NS_TOKEN_TYPE_I32);
        ns_str_case(NS_TOKEN_TYPE_I64);
        ns_str_case(NS_TOKEN_TYPE_U8);
        ns_str_case(NS_TOKEN_TYPE_U16);
        ns_str_case(NS_TOKEN_TYPE_U32);
        ns_str_case(NS_TOKEN_TYPE_U64);
        ns_str_case(NS_TOKEN_TYPE_F32);
        ns_str_case(NS_TOKEN_TYPE_F64);
        ns_str_case(NS_TOKEN_TYPE_BOOL);
        ns_str_case(NS_TOKEN_TYPE_STR);
        ns_str_case(NS_TOKEN_ASSIGN);
        ns_str_case(NS_TOKEN_COLON);
        ns_str_case(NS_TOKEN_ASSIGN_OP);
        ns_str_case(NS_TOKEN_ADD_OP);
        ns_str_case(NS_TOKEN_BITWISE_OP);
        ns_str_case(NS_TOKEN_CMP_OP);
        ns_str_case(NS_TOKEN_OPEN_BRACE);
        ns_str_case(NS_TOKEN_CLOSE_BRACE);
        ns_str_case(NS_TOKEN_OPEN_PAREN);
        ns_str_case(NS_TOKEN_CLOSE_PAREN);
        ns_str_case(NS_TOKEN_OPEN_BRACKET);
        ns_str_case(NS_TOKEN_CLOSE_BRACKET);
        ns_str_case(NS_TOKEN_EOL);
        ns_str_case(NS_TOKEN_EOF);
        ns_str_case(NS_TOKEN_RETURN);
        ns_str_case(NS_TOKEN_IF);
        ns_str_case(NS_TOKEN_ELSE);
        ns_str_case(NS_TOKEN_EQ_OP);
        ns_str_case(NS_TOKEN_COMMA);
        ns_str_case(NS_TOKEN_FOR);
        ns_str_case(NS_TOKEN_TO);
        ns_str_case(NS_TOKEN_OPS);
        ns_str_case(NS_TOKEN_IMPORT);
        ns_str_case(NS_TOKEN_DOT);
        ns_str_case(NS_TOKEN_MUL_OP);
    default:
        ns_error("token", "unknown token %d\n", type);
    }
}

i32 ns_token_float_literal(ns_token_t *t, char *s, i32 i) {
    i32 j = i;
    while (s[j] >= '0' && s[j] <= '9') {
        j++;
    }
    if (s[j] == '.') {
        j++;
        while (s[j] >= '0' && s[j] <= '9') {
            j++;
        }
    }
    t->type = NS_TOKEN_FLT_LITERAL;
    i32 len = j - i;
    t->val = ns_str_range(s + i, len);
    return j;
}

i32 ns_token_int_literal(ns_token_t *t, char *s, i32 i) {
    i32 j = i;
    while (s[j] >= '0' && s[j] <= '9') {
        j++;
    }
    t->type = NS_TOKEN_INT_LITERAL;
    i32 len = j - i;
    t->val = ns_str_range(s + i, len);
    return j;
}

// > 0 mean there is a {separator}+
// = 0 mean there is no {separator}
i32 ns_token_separator(char *s, i32 i) {
    i32 c = s[i];
    i32 to = i, sep;
    do {
        sep = c == ' ' || c == '\t' || c == '\v' || c == ';';
        to++;
        c = s[to];
    } while (sep);
    return to - i - 1;
}

i32 ns_identifier_follow(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (c >= '0' && c <= '9');
}

#define ns_range_token(token, length) \
    sep = ns_token_separator(s, i + length);                 \
    if (sep == 0 && ns_identifier_follow(s[i + length]))   \
        goto identifier;                                \
    t->type = token;                                  \
    t->val = ns_str_range(s + f, length);                    \
    to = i + length + sep;

i32 ns_next_token(ns_token_t *t, ns_str src, ns_str filename, i32 f) {
    i32 i = f;
    i32 to = f + 1;
    i32 l, sep;
    char *s = src.data;
    char lead = s[i]; // TODO parse utf8 characters
    switch (lead) {
    case '0' ... '9': {
        if (s[i + 1] == 'x') {
            // parse hex literal
            i += 2;
            while ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')) {
                i++;
            }
            t->type = NS_TOKEN_INT_LITERAL;
            t->val = ns_str_range(s + f, i - f);
            to = i;
        } else if (s[i + 1] == 'b') {
            // parse binary literal
            i += 2;
            while (s[i] == '0' || s[i] == '1') {
                i++;
            }
            t->type = NS_TOKEN_INT_LITERAL;
            t->val = ns_str_range(s + f, i - f);
            to = i;
        } else if (s[i + 1] == 'o') {
            // parse octal literal
            i += 2;
            while (s[i] >= '0' && s[i] <= '7') {
                i++;
            }
            t->type = NS_TOKEN_INT_LITERAL;
            t->val = ns_str_range(s + f, i - f);
            to = i;
        } else if (s[i + 1] == '.') {
            // parse float literal
            i = ns_token_float_literal(t, s, i);
            to = i;
        } else {
            while (s[i] >= '0' && s[i] <= '9') {
                i++;
            }
            if (s[i] == '.') {
                // parse float literal
                i = ns_token_float_literal(t, s, f);
                to = i;
            } else {
                t->type = NS_TOKEN_INT_LITERAL;
                t->val = ns_str_range(s + f, i - f);
                to = i;
            }
        }
    } break;
    case '.': {
        if (s[i + 1] >= '0' && s[i + 1] <= '9') {
            // parse float literal
            i = ns_token_float_literal(t, s, i);
            to = i;
        } else {
            t->type = NS_TOKEN_DOT;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;

    case 'a': // as async await
    {
        if (strncmp(s + f, "as", 2) == 0) {
            ns_range_token(NS_TOKEN_AS, 2)
        } else if (strncmp(s + f, "any", 3) == 0) {
            ns_range_token(NS_TOKEN_TYPE_ANY, 3)
        } else if (strncmp(s + f, "async", 5) == 0) {
            ns_range_token(NS_TOKEN_ASYNC, 5)
        } else if (strncmp(s + f, "await", 5) == 0) {
            ns_range_token(NS_TOKEN_AWAIT, 5)
        } else {
            goto identifier;
        }
    } break;
    case 'b': // break
    {
        if (strncmp(s + f, "break", 5) == 0) {
            ns_range_token(NS_TOKEN_BREAK, 5)
        } else if (strncmp(s + f, "bool", 4) == 0) {
            ns_range_token(NS_TOKEN_TYPE_BOOL, 4)
        } else {
            goto identifier;
        }
    } break;
    case 'c': // const, continue
    {
        if (strncmp(s + f, "const", 5) == 0) {
            ns_range_token(NS_TOKEN_CONST, 5)
        } else if (strncmp(s + f, "continue", 8) == 0) {
            ns_range_token(NS_TOKEN_CONTINUE, 8)
        } else {
            goto identifier;
        }
    } break;
    case 'd': // do
    {
        if (strncmp(s + f, "do", 2) == 0) {
            ns_range_token(NS_TOKEN_DO, 2)
        } else {
            goto identifier;
        }
    } break;

    case 'e': // else, enum
    {
        if (strncmp(s + f, "else", 4) == 0) {
            ns_range_token(NS_TOKEN_ELSE, 4)
        } else {
            goto identifier;
        }
    } break;

    case 'f': // fn, for, f32, f64
    {
        if (strncmp(s + f, "fn", 2) == 0) {
            ns_range_token(NS_TOKEN_FN, 2)
        } else if (strncmp(s + f, "for", 3) == 0) {
            ns_range_token(NS_TOKEN_FOR, 3)
        } else if (strncmp(s + f, "f32", 3) == 0 || strncmp(s + f, "f64", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = strncmp(s + f, "f32", 3) == 0 ? NS_TOKEN_TYPE_F32 : NS_TOKEN_TYPE_F64;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'i': // if, in, i32, i64, i8, i16
    {
        if (strncmp(s + f, "if", 2) == 0) {
            ns_range_token(NS_TOKEN_IF, 2)
        } else if (strncmp(s + f, "in", 2) == 0) {
            ns_range_token(NS_TOKEN_IN, 2)
        } else if (strncmp(s + f, "i32", 3) == 0 || strncmp(s + f, "i16", 3) == 0 || strncmp(s + f, "i64", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = strncmp(s + f, "i32", 3) == 0   ? NS_TOKEN_TYPE_I32
                      : strncmp(s + f, "i64", 3) == 0 ? NS_TOKEN_TYPE_I64
                                                      : NS_TOKEN_TYPE_I16;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "i8", 2) == 0) {
            ns_range_token(NS_TOKEN_TYPE_I8, 2)
        } else if (strncmp(s + f, "import", 6) == 0) {
            ns_range_token(NS_TOKEN_IMPORT, 6)
        } else {
            goto identifier;
        }
    } break;

    case 'k': {
        if (strncmp(s + f, "kernel", 6) == 0) {
            ns_range_token(NS_TOKEN_KERNEL, 6)
        } else {
            goto identifier;
        }
    } break;

    case 'l': {
        if (strncmp(s + f, "let", 3) == 0) {
            ns_range_token(NS_TOKEN_LET, 3)
        } else if (strncmp(s + f, "loop", 4) == 0) {
            ns_range_token(NS_TOKEN_LOOP, 4)
        } else {
            goto identifier;
        }
    } break;

    case 'm': // match, mod
    {
        if (strncmp(s + f, "match", 5) == 0) {
            ns_range_token(NS_TOKEN_MATCH, 5)
        } else if (strncmp(s + f, "mod", 3) == 0) {
            ns_range_token(NS_TOKEN_MODULE, 3)
        } else {
            goto identifier;
        }
    } break;

    case 'n': // nil
    {
        if (strncmp(s + f, "nil", 3) == 0) {
            ns_range_token(NS_TOKEN_NIL, 3)
        } else {
            goto identifier;
        }
    } break;

    case 'o': // ops
    {
        if (strncmp(s + f, "ops", 3) == 0) {
            ns_range_token(NS_TOKEN_OPS, 3)
        } else {
            goto identifier;
        }
    } break;

    case 'r': // return, ref
    {
        if (strncmp(s + f, "return", 6) == 0) {
            ns_range_token(NS_TOKEN_RETURN, 6)
        } else if (strncmp(s + f, "ref", 3) == 0) {
            ns_range_token(NS_TOKEN_REF, 3)
        } else {
            goto identifier;
        }
    } break;

    case 's': // struct, str
    {
        if (strncmp(s + f, "struct", 6) == 0) {
            ns_range_token(NS_TOKEN_STRUCT, 6)
        } else if (strncmp(s + f, "str", 3) == 0) {
            ns_range_token(NS_TOKEN_TYPE_STR, 3)
        } else {
            goto identifier;
        }
    } break;

    case 't': // type true to
    {
        if (strncmp(s + f, "type", 4) == 0) {
            ns_range_token(NS_TOKEN_TYPE, 4)
        } else if (strncmp(s + f, "true", 4) == 0) {
            ns_range_token(NS_TOKEN_TRUE, 4)
        } else if (strncmp(s + f, "to", 2) == 0) {
            ns_range_token(NS_TOKEN_TO, 2)
        } else {
            goto identifier;
        }
    } break;

    case 'u': // u32, u64, u8, u16
    {
        if (strncmp(s + f, "u32", 3) == 0 || strncmp(s + f, "u64", 3) == 0 || strncmp(s + f, "u16", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = strncmp(s + f, "u32", 3) == 0   ? NS_TOKEN_TYPE_U32
                      : strncmp(s + f, "u64", 3) == 0 ? NS_TOKEN_TYPE_U64
                                                      : NS_TOKEN_TYPE_U16;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "u8", 2) == 0) {
            ns_range_token(NS_TOKEN_TYPE_U8, 2)
        } else {
            goto identifier;
        }
    } break;

    case '(': {
        t->type = NS_TOKEN_OPEN_PAREN;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case ')': {
        t->type = NS_TOKEN_CLOSE_PAREN;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case '{': {
        t->type = NS_TOKEN_OPEN_BRACE;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case '}': {
        t->type = NS_TOKEN_CLOSE_BRACE;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case '[': {
        t->type = NS_TOKEN_OPEN_BRACKET;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case ']': {
        t->type = NS_TOKEN_CLOSE_BRACKET;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case 39: // '
    case 34: // "
    case 96: // `
    {
        // parse string LITERAL
        char quote = lead;
        i++;
        while (s[i] != quote && s[i] != '\0') {
            i++;
        }
        t->type = lead == 96 ? NS_TOKEN_STR_FORMAT : NS_TOKEN_STR_LITERAL;
        t->val = ns_str_range(s + f + 1, i - f - 1);
        to = i + 1;
    } break;
    case '!': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_EQ_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_CMP_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '&':
    case '|': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_BITWISE_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else if (s[i + 1] == lead) {
            t->type = NS_TOKEN_LOGIC_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_BITWISE_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '^': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_BITWISE_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '=': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_EQ_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_ASSIGN;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '+': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_ADD_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '-': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else if (s[i + 1] >= '0' && s[i + 1] <= '9') {
            i++;
            while (s[i] >= '0' && s[i] <= '9') {
                i++;
            }
            if (s[i] == '.') {
                // parse float literal
                i = ns_token_float_literal(t, s, f + 1);
                t->val.data--;
                t->val.len++;
                to = i;
            } else {
                t->type = NS_TOKEN_INT_LITERAL;
                t->val = ns_str_range(s + f, i - f);
                to = i;
            }
        } else {
            t->type = NS_TOKEN_ADD_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
        break;
    }
    case '*': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_MUL_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '/': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else if (s[i + 1] == '/') {
            i += 2;
            while (s[i] != '\n' && strncmp(s + i, "\r\n", 2) != 0 && i < src.len) {
                i++;
            }
            t->type = NS_TOKEN_COMMENT;
            t->val = ns_str_range(s + f, i - f);
            to = i;
        } else {
            t->type = NS_TOKEN_MUL_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '%': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_MUL_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '>': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_CMP_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else if (s[i + 1] == '>') {
            if (s[i + 2] == '=') {
                t->type = NS_TOKEN_ASSIGN_OP;
                t->val = ns_str_range(s + f, 3);
                to = i + 3;
            } else {
                t->type = NS_TOKEN_SHIFT_OP;
                t->val = ns_str_range(s + f, 2);
                to = i + 2;
            }
        } else {
            t->type = NS_TOKEN_CMP_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '<': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_CMP_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else if (s[i + 1] == '<') {
            if (s[i + 2] == '=') {
                t->type = NS_TOKEN_ASSIGN_OP;
                t->val = ns_str_range(s + f, 3);
                to = i + 3;
            } else {
                t->type = NS_TOKEN_SHIFT_OP;
                t->val = ns_str_range(s + f, 2);
                to = i + 2;
            }
        } else {
            t->type = NS_TOKEN_CMP_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '?': {
        t->type = NS_TOKEN_QUESTION_MARK;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case ':': {
        t->type = NS_TOKEN_COLON;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case ',': {
        t->type = NS_TOKEN_COMMA;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
    } break;
    case ' ':
    case '\t':
    case '\v': {
        l = i;
        sep = ns_token_separator(s, i);
        if (sep == 0) {
            goto identifier;
        }
        t->type = NS_TOKEN_SPACE;
        t->val = ns_str_range(s + l, sep);
        to = i + sep;
    } break;
    case ';':
    case '\r':
    case '\n': {
        if (s[i] == '\r' && s[i + 1] == '\n') {
            i++;
        }
        t->type = NS_TOKEN_EOL;
        t->val = ns_str_range(s + f, 1);
        to = i + 1;
        t->line++;
        t->line_start = i;
    } break;

    case '\0': {
        t->type = NS_TOKEN_EOF;
    } break;
    identifier:
    default: {
        char lead = s[i];
        if (!((lead >= 'a' && lead <= 'z') || (lead >= 'A' && lead <= 'Z'))) {
            ns_error("unexpected character", "[%s:%d:%d]%c\n", filename.data, t->line, i - t->line_start,
                    lead);
        }
        i++;

        while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_') {
            i++;
        }

        t->type = NS_TOKEN_IDENTIFIER;
        t->val = ns_str_range(s + f, i - f);
        to = i;
    } break;
    }
    return to;
}

ns_export void ns_token(ns_str source, ns_str filename) {
    ns_info("ns", "tokenizing %.*s\n", ns_max(0, filename.len), filename.data);
    i32 len = strlen(source.data);
    i32 i = 0;
    ns_token_t t = {0};
    t.line = 1;
    do {
        i = ns_next_token(&t, source, filename, i);
        ns_str type = ns_token_type_to_string(t.type);
        if (t.type == NS_TOKEN_SPACE) {
            continue;
        } else if (t.type == NS_TOKEN_EOF) {
            break;
        } else if (t.type == NS_TOKEN_EOL) {
            printf("[%s:%d:%-4d] %-20.*s\n", filename.data, t.line, i - t.line_start, type.len, type.data);
        } else {
            printf("[%s:%d:%-4d] %-20.*s %.*s\n", filename.data, t.line, i - t.line_start, type.len, type.data, ns_max(0, t.val.len), t.val.data);
        }
        t.type = NS_TOKEN_UNKNOWN;
    } while (i < len);
}