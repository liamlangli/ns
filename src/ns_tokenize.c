#include "ns_tokenize.h"
#include "ns_type.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

ns_str ns_token_type_to_string(NS_TOKEN type) {
    switch (type) {
    case NS_TOKEN_INT_LITERAL:
        return ns_str_cstr("NS_TOKEN_INT_LITERAL");
    case NS_TOKEN_FLT_LITERAL:
        return ns_str_cstr("NS_TOKEN_FLT_LITERAL");
    case NS_TOKEN_STR_LITERAL:
        return ns_str_cstr("NS_TOKEN_STR_LITERAL");
    case NS_TOKEN_CONST:
        return ns_str_cstr("NS_TOKEN_CONST");
    case NS_TOKEN_COMMENT:
        return ns_str_cstr("NS_TOKEN_COMMENT");
    case NS_TOKEN_LET:
        return ns_str_cstr("NS_TOKEN_LET");
    case NS_TOKEN_FN:
        return ns_str_cstr("NS_TOKEN_FN");
    case NS_TOKEN_IN:
        return ns_str_cstr("NS_TOKEN_IN");
    case NS_TOKEN_SPACE:
        return ns_str_cstr("NS_TOKEN_SPACE");
    case NS_TOKEN_STRUCT:
        return ns_str_cstr("NS_TOKEN_STRUCT");
    case NS_TOKEN_IDENTIFIER:
        return ns_str_cstr("NS_TOKEN_IDENTIFIER");
    case NS_TOKEN_ASYNC:
        return ns_str_cstr("NS_TOKEN_ASYNC");
    case NS_TOKEN_AWAIT:
        return ns_str_cstr("NS_TOKEN_AWAIT");
    case NS_TOKEN_TYPE_INT8:
        return ns_str_cstr("NS_TOKEN_TYPE_INT8");
    case NS_TOKEN_TYPE_INT16:
        return ns_str_cstr("NS_TOKEN_TYPE_INT16");
    case NS_TOKEN_TYPE_INT32:
        return ns_str_cstr("NS_TOKEN_TYPE_INT32");
    case NS_TOKEN_TYPE_INT64:
        return ns_str_cstr("NS_TOKEN_TYPE_INT64");
    case NS_TOKEN_TYPE_UINT8:
        return ns_str_cstr("NS_TOKEN_TYPE_UINT8");
    case NS_TOKEN_TYPE_UINT16:
        return ns_str_cstr("NS_TOKEN_TYPE_UINT16");
    case NS_TOKEN_TYPE_UINT32:
        return ns_str_cstr("NS_TOKEN_TYPE_UINT32");
    case NS_TOKEN_TYPE_UINT64:
        return ns_str_cstr("NS_TOKEN_TYPE_UINT64");
    case NS_TOKEN_TYPE_F32:
        return ns_str_cstr("NS_TOKEN_TYPE_F32");
    case NS_TOKEN_TYPE_F64:
        return ns_str_cstr("NS_TOKEN_TYPE_F64");
    case NS_TOKEN_TYPE_BOOL:
        return ns_str_cstr("NS_TOKEN_TYPE_BOOL");
    case NS_TOKEN_TYPE_STR:
        return ns_str_cstr("NS_TOKEN_TYPE_STR");
    case NS_TOKEN_ASSIGN:
        return ns_str_cstr("NS_TOKEN_ASSIGN");
    case NS_TOKEN_COLON:
        return ns_str_cstr("NS_TOKEN_COLON");
    case NS_TOKEN_ASSIGN_OP:
        return ns_str_cstr("NS_TOKEN_ASSIGN_OP");
    case NS_TOKEN_ADD_OP:
        return ns_str_cstr("NS_TOKEN_ADD_OP");
    case NS_TOKEN_BITWISE_OP:
        return ns_str_cstr("NS_TOKEN_BITWISE_OP");
    case NS_TOKEN_BOOL_OP:
        return ns_str_cstr("NS_TOKEN_BOOL_OP");
    case NS_TOKEN_OPEN_BRACE:
        return ns_str_cstr("NS_TOKEN_OPEN_BRACE");
    case NS_TOKEN_CLOSE_BRACE:
        return ns_str_cstr("NS_TOKEN_CLOSE_BRACE");
    case NS_TOKEN_OPEN_PAREN:
        return ns_str_cstr("NS_TOKEN_OPEN_PAREN");
    case NS_TOKEN_CLOSE_PAREN:
        return ns_str_cstr("NS_TOKEN_CLOSE_PAREN");
    case NS_TOKEN_OPEN_BRACKET:
        return ns_str_cstr("NS_TOKEN_OPEN_BRACKET");
    case NS_TOKEN_CLOSE_BRACKET:
        return ns_str_cstr("NS_TOKEN_CLOSE_BRACKET");
    case NS_TOKEN_EOL:
        return ns_str_cstr("NS_TOKEN_EOL");
    case NS_TOKEN_EOF:
        return ns_str_cstr("NS_TOKEN_EOF");
    case NS_TOKEN_RETURN:
        return ns_str_cstr("NS_TOKEN_RETURN");
    case NS_TOKEN_IF:
        return ns_str_cstr("NS_TOKEN_IF");
    case NS_TOKEN_ELSE:
        return ns_str_cstr("NS_TOKEN_ELSE");
    case NS_TOKEN_EQ_OP:
        return ns_str_cstr("NS_TOKEN_EQ_OP");
    case NS_TOKEN_COMMA:
        return ns_str_cstr("NS_TOKEN_COMMA");
    case NS_TOKEN_FOR:
        return ns_str_cstr("NS_TOKEN_FOR");
    case NS_TOKEN_TO:
        return ns_str_cstr("NS_TOKEN_TO");
    case NS_TOKEN_OPS:
        return ns_str_cstr("NS_TOKEN_OPS");
    case NS_TOKEN_IMPORT:
        return ns_str_cstr("NS_TOKEN_IMPORT");
    case NS_TOKEN_DOT:
        return ns_str_cstr("NS_TOKEN_DOT");
    case NS_TOKEN_MUL_OP:
        return ns_str_cstr("NS_TOKEN_MUL_OP");
    default:
        return ns_str_cstr("Unknown token");
    }
}

int ns_token_float_literal(ns_token_t *t, char *s, int i) {
    int j = i;
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
    int len = j - i;
    t->val = ns_str_range(s + i, len);
    return j;
}

int ns_token_int_literal(ns_token_t *t, char *s, int i) {
    int j = i;
    while (s[j] >= '0' && s[j] <= '9') {
        j++;
    }
    t->type = NS_TOKEN_INT_LITERAL;
    int len = j - i;
    t->val = ns_str_range(s + i, len);
    return j;
}

// > 0 mean there is a {separator}+
// = 0 mean there is no {separator}
int ns_token_separator(char *s, int i) {
    int c = s[i];
    int to = i, sep;
    do {
        sep = c == ' ' || c == '\t' || c == '\v' || c == ';';
        to++;
        c = s[to];
    } while (sep);
    return to - i - 1;
}

int ns_identifier_follow(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (c >= '0' && c <= '9');
}

int ns_next_token(ns_token_t *t, ns_str src, ns_str filename, int f) {
    int i = f;
    int to = f + 1;
    int l, sep;
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
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2])) {
                goto identifier;
            }
            t->type = NS_TOKEN_AS;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "async", 5) == 0) {
            sep = ns_token_separator(s, i + 5);
            if (sep == 0 && ns_identifier_follow(s[i + 5])) {
                i += sep;
                goto identifier;
            }
            t->type = NS_TOKEN_ASYNC;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else if (strncmp(s + f, "await", 5) == 0) {
            sep = ns_token_separator(s, i + 5);
            if (sep == 0 && ns_identifier_follow(s[i + 5])) {
                i += sep;
                goto identifier;
            }
            t->type = NS_TOKEN_AWAIT;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else {
            goto identifier;
        }
    } break;
    case 'b': // break
    {
        if (strncmp(s + f, "break", 5) == 0) {
            sep = ns_token_separator(s, i + 5);
            if (sep == 0 && ns_identifier_follow(s[i + 5]))
                goto identifier;
            t->type = NS_TOKEN_BREAK;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else if (strncmp(s + f, "bool", 4) == 0) {
            sep = ns_token_separator(s, i + 4);
            if (sep == 0 && ns_identifier_follow(s[i + 4]))
                goto identifier;
            t->type = NS_TOKEN_TYPE_BOOL;
            t->val = ns_str_range(s + f, 4);
            to = i + 4 + sep;
        } else {
            goto identifier;
        }
    } break;
    case 'c': // const, continue
    {
        if (strncmp(s + f, "const", 5) == 0) {
            sep = ns_token_separator(s, i + 5);
            if (sep == 0 && ns_identifier_follow(s[i + 5]))
                goto identifier;
            t->type = NS_TOKEN_CONST;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else if (strncmp(s + f, "continue", 8) == 0) {
            sep = ns_token_separator(s, i + 8);
            if (sep == 0 && ns_identifier_follow(s[i + 8]))
                goto identifier;
            t->type = NS_TOKEN_CONTINUE;
            t->val = ns_str_range(s + f, 8);
            to = i + 8 + sep;
        } else {
            goto identifier;
        }
    } break;
    case 'd': // do
    {
        if (strncmp(s + f, "do", 2) == 0) {
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_DO;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'e': // else, enum
    {
        if (strncmp(s + f, "else", 4) == 0) {
            sep = ns_token_separator(s, i + 4);
            if (sep == 0 && ns_identifier_follow(s[i + 4]))
                goto identifier;
            t->type = NS_TOKEN_ELSE;
            t->val = ns_str_range(s + f, 4);
            to = i + 4 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'f': // fn, for, f32, f64
    {
        if (strncmp(s + f, "fn", 2) == 0) {
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_FN;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "for", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = NS_TOKEN_FOR;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
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
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_IF;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "in", 2) == 0) {
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_IN;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "i32", 3) == 0 || strncmp(s + f, "i16", 3) == 0 || strncmp(s + f, "i64", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = strncmp(s + f, "i32", 3) == 0   ? NS_TOKEN_TYPE_INT32
                      : strncmp(s + f, "i64", 3) == 0 ? NS_TOKEN_TYPE_INT64
                                                      : NS_TOKEN_TYPE_INT16;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "i8", 2) == 0) {
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_TYPE_INT8;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "import", 6) == 0) {
            sep = ns_token_separator(s, i + 6);
            if (sep == 0 && ns_identifier_follow(s[i + 6]))
                goto identifier;
            t->type = NS_TOKEN_IMPORT;
            t->val = ns_str_range(s + f, 6);
            to = i + 6 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'l': {
        if (strncmp(s + f, "let", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = NS_TOKEN_LET;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'n': // nil
    {
        if (strncmp(s + f, "nil", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = NS_TOKEN_NIL;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'o': // ops
    {
        if (strncmp(s + f, "ops", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = NS_TOKEN_OPS;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'r': // return, ref
    {
        if (strncmp(s + f, "return", 6) == 0) {
            sep = ns_token_separator(s, i + 6);
            if (sep == 0 && ns_identifier_follow(s[i + 6]))
                goto identifier;
            t->type = NS_TOKEN_RETURN;
            t->val = ns_str_range(s + f, 6);
            to = i + 6 + sep;
        } else if (strncmp(s + f, "ref", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = NS_TOKEN_REF;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 's': // struct, str
    {
        if (strncmp(s + f, "struct", 6) == 0) {
            sep = ns_token_separator(s, i + 6);
            if (sep == 0 && ns_identifier_follow(s[i + 6]))
                goto identifier;
            t->type = NS_TOKEN_STRUCT;
            t->val = ns_str_range(s + f, 6);
            to = i + 6 + sep;
        } else if (strncmp(s + f, "str", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3]))
                goto identifier;
            t->type = NS_TOKEN_TYPE_STR;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 't': // type true to
    {
        if (strncmp(s + f, "type", 4) == 0) {
            sep = ns_token_separator(s, i + 4);
            if (sep == 0 && ns_identifier_follow(s[i + 4]))
                goto identifier;
            t->type = NS_TOKEN_TYPE_DEF;
            t->val = ns_str_range(s + f, 4);
            to = i + 4 + sep;
        } else if (strncmp(s + f, "true", 4) == 0) {
            sep = ns_token_separator(s, i + 4);
            if (sep == 0 && ns_identifier_follow(s[i + 4]))
                goto identifier;
            t->type = NS_TOKEN_TRUE;
            t->val = ns_str_range(s + f, 4);
            to = i + 4 + sep;
        } else if (strncmp(s + f, "to", 2) == 0) {
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_TO;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
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
            t->type = strncmp(s + f, "u32", 3) == 0   ? NS_TOKEN_TYPE_UINT32
                      : strncmp(s + f, "u64", 3) == 0 ? NS_TOKEN_TYPE_UINT64
                                                      : NS_TOKEN_TYPE_UINT16;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "u8", 2) == 0) {
            sep = ns_token_separator(s, i + 2);
            if (sep == 0 && ns_identifier_follow(s[i + 2]))
                goto identifier;
            t->type = NS_TOKEN_TYPE_UINT8;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'w': // while
    {
        if (strncmp(s + f, "while", 5) == 0) {
            sep = ns_token_separator(s, i + 5);
            if (sep == 0 && ns_identifier_follow(s[i + 6]))
                goto identifier;
            t->type = NS_TOKEN_WHILE;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
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
        t->type = NS_TOKEN_STR_LITERAL;
        t->val = ns_str_range(s + f + 1, i - f - 1);
        to = i + 1;
    } break;
    case '!': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_EQ_OP;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_BOOL_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '&':
    case '|': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP;
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
            t->type = NS_TOKEN_BOOL_OP;
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
            t->type = NS_TOKEN_BOOL_OP;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '<': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_BOOL_OP;
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
            t->type = NS_TOKEN_BOOL_OP;
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

void ns_tokenize(ns_str source, ns_str filename) {
    ns_info("ns", "tokenizing %.*s\n", ns_max(0, filename.len), filename.data);
    int len = strlen(source.data);
    int i = 0;
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