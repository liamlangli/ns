#include "ns_tokenize.h"
#include "ns.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

const char *ns_token_to_string(NS_TOKEN type) {
    switch (type) {
    case NS_TOKEN_INT_LITERAL:
        return "NS_TOKEN_INT_LITERAL";
    case NS_TOKEN_FLOAT_LITERAL:
        return "NS_TOKEN_FLOAT_LITERAL";
    case NS_TOKEN_STRING_LITERAL:
        return "NS_TOKEN_STRING_LITERAL";
    case NS_TOKEN_CONST:
        return "NS_TOKEN_CONST";
    case NS_TOKEN_LET:
        return "NS_TOKEN_LET";
    case NS_TOKEN_FN:
        return "NS_TOKEN_FN";
    case NS_TOKEN_IN:
        return "NS_TOKEN_IN";
    case NS_TOKEN_SPACE:
        return "NS_TOKEN_SPACE";
    case NS_TOKEN_STRUCT:
        return "NS_TOKEN_STRUCT";
    case NS_TOKEN_IDENTIFIER:
        return "NS_TOKEN_IDENTIFIER";
    case NS_TOKEN_ASYNC:
        return "NS_TOKEN_ASYNC";
    case NS_TOKEN_AWAIT:
        return "NS_TOKEN_AWAIT";
    case NS_TOKEN_TYPE:
        return "NS_TOKEN_TYPE";
    case NS_TOKEN_ASSIGN:
        return "NS_TOKEN_ASSIGN";
    case NS_TOKEN_COLON:
        return "NS_TOKEN_COLON";
    case NS_TOKEN_SEMICOLON:
        return "NS_TOKEN_SEMICOLON";
    case NS_TOKEN_ASSIGN_OPERATOR:
        return "NS_TOKEN_ASSIGN_OPERATOR";
    case NS_TOKEN_ARITHMETIC_OPERATOR:
        return "NS_TOKEN_ARITHMETIC_OPERATOR";
    case NS_TOKEN_BITWISE_OPERATOR:
        return "NS_TOKEN_BITWISE_OPERATOR";
    case NS_TOKEN_BOOL_OPERATOR:
        return "NS_TOKEN_BOOL_OPERATOR";
    case NS_TOKEN_OPEN_BRACE:
        return "NS_TOKEN_OPEN_BRACE";
    case NS_TOKEN_CLOSE_BRACE:
        return "NS_TOKEN_CLOSE_BRACE";
    case NS_TOKEN_OPEN_PAREN:
        return "NS_TOKEN_OPEN_PAREN";
    case NS_TOKEN_CLOSE_PAREN:
        return "NS_TOKEN_CLOSE_PAREN";
    case NS_TOKEN_OPEN_BRACKET:
        return "NS_TOKEN_OPEN_BRACKET";
    case NS_TOKEN_CLOSE_BRACKET:
        return "NS_TOKEN_CLOSE_BRACKET";
    case NS_TOKEN_EOF:
        return "NS_TOKEN_EOF";
    default:
        return "Unknown token";
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
    t->type = NS_TOKEN_FLOAT_LITERAL;
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
int ns_token_separator(ns_token_t *t, char *s, int i) {
    int c = s[i];
    int to = i, eol, sep;
    do {
        eol = c == '\n' || strncmp(s + to, "\r\n", 2) == 0;
        if (eol) {
            t->line++;
            t->line_start = to;
        }
        sep = c == ' ' || c == '\t' || c == '\v' || c == ';';
        to++;
        c = s[to];
    } while (eol || sep);
    return to - i - 1;
}

int ns_tokenize(ns_token_t *t, char *s, int f) {
    int i = f;
    int to = f + 1;
    int l, sep;
    char lead = s[i]; // TODO parse utf8 characters
    switch (lead) {
    case '0' ... '9':

    case 'a': // as async await
    {
        if (strncmp(s + f, "as", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0) {
                goto identifier;
            }
            t->type = NS_TOKEN_AS;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "async", 5) == 0) {
            sep = ns_token_separator(t, s, i + 5);
            if (sep == 0) {
                i += sep;
                goto identifier;
            }
            t->type = NS_TOKEN_ASYNC;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else if (strncmp(s + f, "await", 5) == 0) {
            sep = ns_token_separator(t, s, i + 5);
            if (sep == 0) {
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
            sep = ns_token_separator(t, s, i + 5);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_BREAK;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else {
            goto identifier;
        }
    } break;
    case 'c': // const, continue
    {
        if (strncmp(s + f, "const", 5) == 0) {
            sep = ns_token_separator(t, s, i + 5);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_CONST;
            t->val = ns_str_range(s + f, 5);
            to = i + 5 + sep;
        } else if (strncmp(s + f, "continue", 8) == 0) {
            sep = ns_token_separator(t, s, i + 8);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_CONTINUE;
            t->val = ns_str_range(s + f, 8);
            to = i + 8 + sep;
        }
    } break;
    case 'd': // default, do
    {
        if (strncmp(s + f, "default", 7) == 0) {
            sep = ns_token_separator(t, s, i + 7);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_DEFAULT;
            t->val = ns_str_range(s + f, 7);
            to = i + 7 + sep;
        } else if (strncmp(s + f, "do", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
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
            sep = ns_token_separator(t, s, i + 4);
            if (sep == 0)
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
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_FN;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "for", 3) == 0) {
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_FOR;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "f32", 3) == 0 || strncmp(s + f, "f64", 3) == 0) {
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'i': // if, in, i32, i64, i8, i16
    {
        if (strncmp(s + f, "if", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_IF;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "in", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_IN;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else if (strncmp(s + f, "i32", 3) == 0 || strncmp(s + f, "i16", 3) == 0 || strncmp(s + f, "i64", 3) == 0) {
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "i8", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'l': {
        if (strncmp(s + f, "let", 3) == 0) {
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
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
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_NIL;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'r': // return, ref
    {
        if (strncmp(s + f, "return", 6) == 0) {
            sep = ns_token_separator(t, s, i + 6);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_RETURN;
            t->val = ns_str_range(s + f, 6);
            to = i + 6 + sep;
        } else if (strncmp(s + f, "ref", 3) == 0) {
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
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
            sep = ns_token_separator(t, s, i + 6);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_STRUCT;
            t->val = ns_str_range(s + f, 6);
            to = i + 6 + sep;
        } else if (strncmp(s + f, "str", 3) == 0) {
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 't': // type true to
    {
        if (strncmp(s + f, "type", 4) == 0) {
            sep = ns_token_separator(t, s, i + 4);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 4);
            to = i + 4 + sep;
        } else if (strncmp(s + f, "true", 4) == 0) {
            sep = ns_token_separator(t, s, i + 4);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TRUE;
            t->val = ns_str_range(s + f, 4);
            to = i + 4 + sep;
        } else if (strncmp(s + f, "to", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
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
            sep = ns_token_separator(t, s, i + 3);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else if (strncmp(s + f, "u8", 2) == 0) {
            sep = ns_token_separator(t, s, i + 2);
            if (sep == 0)
                goto identifier;
            t->type = NS_TOKEN_TYPE;
            t->val = ns_str_range(s + f, 2);
            to = i + 2 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'w': // while
    {
        if (strncmp(s + f, "while", 5) == 0) {
            sep = ns_token_separator(t, s, i + 5);
            if (sep == 0)
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
        t->type = NS_TOKEN_STRING_LITERAL;
        t->val = ns_str_range(s + f + 1, i - f - 1);
        to = i + 1;
    } break;
    case '=': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_BOOL_OPERATOR;
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
            t->type = NS_TOKEN_ASSIGN_OPERATOR;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_ARITHMETIC_OPERATOR;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '-': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OPERATOR;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_ARITHMETIC_OPERATOR;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
        break;
    }
    case '*': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OPERATOR;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_ARITHMETIC_OPERATOR;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '/': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OPERATOR;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else if (s[i + 1] == '/') {
            while (s[i] != '\n' && strncmp(s + i, "\r\n", 2) != 0) {
                i++;
            }
            t->type = NS_TOKEN_COMMENT;
            t->val = ns_str_range(s + f, i - f);
            to = i;
        } else {
            t->type = NS_TOKEN_ARITHMETIC_OPERATOR;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case '%': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OPERATOR;
            t->val = ns_str_range(s + f, 2);
            to = i + 2;
        } else {
            t->type = NS_TOKEN_ARITHMETIC_OPERATOR;
            t->val = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;
    case ' ':
    case ';':
    case '\t':
    case '\n':
    case '\r':
    case '\v': {
        l = i;
        sep = ns_token_separator(t, s, i);
        if (sep == 0) {
            goto identifier;
        }
        t->type = NS_TOKEN_SPACE;
        t->val = ns_str_range(s + l, sep);
        to = i + sep;
    } break;

    case '\0': {
        t->type = NS_TOKEN_EOF;
    } break;
    identifier:
    default: {
        i += sep;
        char lead = s[i];
        if (!((lead >= 'a' && lead <= 'z') || (lead >= 'A' && lead <= 'Z'))) {
            printf("[line:%d, offset:%d] Unexpected character: %c\n", t->line, i - t->line_start, lead);
            assert(false);
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
