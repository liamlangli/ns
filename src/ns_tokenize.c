#include "ns_tokenize.h"
#include "ns.h"


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
        case NS_TOKEN_OPERATOR:
            return "NS_TOKEN_OPERATOR";
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

static ns_token_t ns_token_as = {NS_TOKEN_AS, {"as", 2}};
static ns_token_t ns_token_async = {NS_TOKEN_ASYNC, {"async", 5}};
static ns_token_t ns_token_await = {NS_TOKEN_AWAIT, {"await", 5}};
static ns_token_t ns_token_break = {NS_TOKEN_BREAK, {"break", 5}};
static ns_token_t ns_token_const = {NS_TOKEN_CONST, {"const", 5}};
static ns_token_t ns_token_continue = {NS_TOKEN_CONTINUE, {"continue", 8}};
static ns_token_t ns_token_default = {NS_TOKEN_DEFAULT, {"default", 7}};
static ns_token_t ns_token_do = {NS_TOKEN_DO, {"do", 2}};
static ns_token_t ns_token_else = {NS_TOKEN_ELSE, {"else", 4}};
static ns_token_t ns_token_fn = {NS_TOKEN_FN, {"fn", 2}};
static ns_token_t ns_token_for = {NS_TOKEN_FOR, {"for", 3}};
static ns_token_t ns_token_if = {NS_TOKEN_IF, {"if", 2}};
static ns_token_t ns_token_in = {NS_TOKEN_IN, {"in", 2}};
static ns_token_t ns_token_i32 = {NS_TOKEN_TYPE, {"i32", 3}};
static ns_token_t ns_token_i64 = {NS_TOKEN_TYPE, {"i64", 3}};
static ns_token_t ns_token_i8 = {NS_TOKEN_TYPE, {"i8", 2}};
static ns_token_t ns_token_i16 = {NS_TOKEN_TYPE, {"i16", 3}};
static ns_token_t ns_token_f32 = {NS_TOKEN_TYPE, {"f32", 3}};
static ns_token_t ns_token_f64 = {NS_TOKEN_TYPE, {"f64", 3}};
static ns_token_t ns_token_let = {NS_TOKEN_LET, {"let", 3}};
static ns_token_t ns_token_nil = {NS_TOKEN_NIL, {"nil", 3}};
static ns_token_t ns_token_return = {NS_TOKEN_RETURN, {"return", 6}};
static ns_token_t ns_token_ref = {NS_TOKEN_REF, {"ref", 3}};
static ns_token_t ns_token_struct = {NS_TOKEN_STRUCT, {"struct", 6}};
static ns_token_t ns_token_str = {NS_TOKEN_TYPE, {"str", 3}};
static ns_token_t ns_token_true = {NS_TOKEN_TRUE, {"true", 4}};
static ns_token_t ns_token_type = {NS_TOKEN_TYPE, {"type", 4}};
static ns_token_t ns_token_to = {NS_TOKEN_TO, {"to", 2}};
static ns_token_t ns_token_u32 = {NS_TOKEN_TYPE, {"u32", 3}};
static ns_token_t ns_token_u64 = {NS_TOKEN_TYPE, {"u64", 3}};
static ns_token_t ns_token_u8 = {NS_TOKEN_TYPE, {"u8", 2}};
static ns_token_t ns_token_u16 = {NS_TOKEN_TYPE, {"u16", 3}};
static ns_token_t ns_token_while = {NS_TOKEN_WHILE, {"while", 5}};
static ns_token_t ns_token_open_brace = {NS_TOKEN_OPEN_BRACE, {"{", 1}};
static ns_token_t ns_token_close_brace = {NS_TOKEN_CLOSE_BRACE, {"}", 1}};
static ns_token_t ns_token_open_paren = {NS_TOKEN_OPEN_PAREN, {"(", 1}};
static ns_token_t ns_token_close_paren = {NS_TOKEN_CLOSE_PAREN, {")", 1}};
static ns_token_t ns_token_open_bracket = {NS_TOKEN_OPEN_BRACKET, {"[", 1}};
static ns_token_t ns_token_close_bracket = {NS_TOKEN_CLOSE_BRACKET, {"]", 1}};
static ns_token_t ns_token_eof = {NS_TOKEN_EOF, {"", 0}};
static ns_token_t ns_token_assign = {NS_TOKEN_ASSIGN, {"=", 1}};
static ns_token_t ns_token_colon = {NS_TOKEN_COLON, {":", 1}};

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
int ns_token_separator(char *s, int i) {
    int c = s[i];
    int to = i;
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        i++;
        c = s[i];
    }
    return to - i;
}

int ns_tokenize(ns_token_t *t, char *s, int f) {
    int i = f;
    int to = f + 1;
    int l;
    char lead = s[i]; // TODO parse utf8 characters
    switch (lead) {
    case 'a': // as async await
    {
        if (strncmp(s + f, "as", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;    
            *t = ns_token_as;
            to = i + 2 + sep;
        } else if (strncmp(s + f, "async", 5) == 0) {
            int sep = ns_token_separator(s, i + 5);
            if (sep == 0)
                goto identifier;
            *t = ns_token_async;
            to = i + 5 + sep;
        } else if (strncmp(s + f, "await", 5) == 0) {
            int sep = ns_token_separator(s, i + 5);
            if (sep == 0)
                goto identifier;
            *t = ns_token_await;
            to = i + 5 + sep;
        } else {
            goto identifier;
        }
    } break;
    case 'b': // break
    {
        if (strncmp(s + f, "break", 5) == 0) {
            int sep = ns_token_separator(s, i + 5);
            if (sep == 0)
                goto identifier;
            *t = ns_token_break;
            to = i + 5 + sep;
        } else {
            goto identifier;
        }
    } break;
    case 'c': // const, continue
    {
        if (strncmp(s + f, "const", 5) == 0) {
            int sep = ns_token_separator(s, i + 5);
            if (sep == 0)
                goto identifier;
            *t = ns_token_const;
            to = i + 5 + sep;
        } else if (strncmp(s + f, "continue", 8) == 0) {
            int sep = ns_token_separator(s, i + 8);
            if (sep == 0)
                goto identifier;
            *t = ns_token_continue;
            to = i + 8 + sep;
        }
    } break;
    case 'd': // default, do
    {
        if (strncmp(s + f, "default", 7) == 0) {
            int sep = ns_token_separator(s, i + 7);
            if (sep == 0)
                goto identifier;
            *t = ns_token_default;
            to = i + 7 + sep;
        } else if (strncmp(s + f, "do", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_do;
            to = i + 2 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'e': // else, enum
    {
        if (strncmp(s + f, "else", 4) == 0) {
            int sep = ns_token_separator(s, i + 4);
            if (sep == 0)
                goto identifier;
            *t = ns_token_else;
            to = i + 4 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'f': // fn, for, f32, f64
    {
        if (strncmp(s + f, "fn", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_fn;
            to = i + 2 + sep;
        } else if (strncmp(s + f, "for", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_for;
            to = i + 3 + sep;
        } else if (strncmp(s + f, "f32", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_f32;
            to = i + 3 + sep;
        } else if (strncmp(s + f, "f64", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_f64;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;
    
    case 'i': // if, in, i32, i64, i8, i16
    {
        if (strncmp(s + f, "if", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_if;
            to = i + 2 + sep;
        } else if (strncmp(s + f, "in", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_in;
            to = i + 2 + sep;
        } else if (strncmp(s + f, "i32", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_i32;
            to = i + 3 + sep;
        } else if (strncmp(s + f, "i64", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_i64;
            to = i + 3 + sep;
        } else if (strncmp(s + f, "i8", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_i8;
            to = i + 2 + sep;
        } else if (strncmp(s + f, "i16", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_i16;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;
    
    case 'l':
    {
        if (strncmp(s + f, "let", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_let;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'n': // nil
    {
        if (strncmp(s + f, "nil", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_nil;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'r': // return, ref
    {
        if (strncmp(s + f, "return", 6) == 0) {
            int sep = ns_token_separator(s, i + 6);
            if (sep == 0)
                goto identifier;
            *t = ns_token_return;
            to = i + 6 + sep;
        } else if (strncmp(s + f, "ref", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_ref;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 's': // struct, str
    {
        if (strncmp(s + f, "struct", 6) == 0) {
            int sep = ns_token_separator(s, i + 6);
            if (sep == 0)
                goto identifier;
            *t = ns_token_struct;
            to = i + 6 + sep;
        } else if (strncmp(s + f, "str", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_str;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 't': // type true to
    {
        if (strncmp(s + f, "type", 4) == 0) {
            int sep = ns_token_separator(s, i + 4);
            if (sep == 0)
                goto identifier;
            *t = ns_token_type;
            to = i + 4 + sep;
        } else if (strncmp(s + f, "true", 4) == 0) {
            int sep = ns_token_separator(s, i + 4);
            if (sep == 0)
                goto identifier;
            *t = ns_token_true;
            to = i + 4 + sep;
        } else if (strncmp(s + f, "to", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_to;
            to = i + 2 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'u': // u32, u64, u8, u16
    {
        if (strncmp(s + f, "u32", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_u32;
            to = i + 3 + sep;
        } else if (strncmp(s + f, "u64", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_u64;
            to = i + 3 + sep;
        } else if (strncmp(s + f, "u8", 2) == 0) {
            int sep = ns_token_separator(s, i + 2);
            if (sep == 0)
                goto identifier;
            *t = ns_token_u8;
            to = i + 2 + sep;
        } else if (strncmp(s + f, "u16", 3) == 0) {
            int sep = ns_token_separator(s, i + 3);
            if (sep == 0)
                goto identifier;
            *t = ns_token_u16;
            to = i + 3 + sep;
        } else {
            goto identifier;
        }
    } break;

    case 'w': // while
    {
        if (strncmp(s + f, "while", 5) == 0) {
            int sep = ns_token_separator(s, i + 5);
            if (sep == 0)
                goto identifier;
            *t = ns_token_while;
            to = i + 5 + sep;
        } else {
            goto identifier;
        }
    } break;

    case '(': {
        *t = ns_token_open_paren;
        to = i + 1;
    } break;
    case ')': {
    *t = ns_token_close_paren;
        to = i + 1;
    } break;
    case '{': {
        *t = ns_token_open_brace;        
        to = i + 1;
    } break;
    case '}': {
        *t = ns_token_close_brace;
        to = i + 1;
    } break;
    case '[': {
        *t = ns_token_open_bracket;
        to = i + 1;
    } break;
    case ']': {
        *t = ns_token_close_bracket;
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
        token->type = NS_TOKEN_STRING_LITERAL;
        len = i - from;
        memcpy(us_str, src + from, len + 1);
        us_str[len + 1] = '\0';
        to = i + 1;
    } break;
    // try parse key words
    case 'a': {
        if (src[i + 1] == 's' && src[i + 2] == 'y' && src[i + 3] == 'n' && src[i + 4] == 'c') {
            token->type = NS_TOKEN_ASYNC;
            strcpy(us_str, "async");
            to = i + 5;
        } else if (src[i + 1] == 'w' && src[i + 2] == 'a' && src[i + 3] == 'i' && src[i + 4] == 't') {
            token->type = NS_TOKEN_AWAIT;
            strcpy(us_str, "await");
            to = i + 5;
        } else {
            goto identifier;
        }
    } break;
    case 'c': {
        if (src[i + 1] == 'o' && src[i + 2] == 'n' && src[i + 3] == 's' && src[i + 4] == 't') {
            token->type = NS_TOKEN_CONST;
            strcpy(us_str, "const");
            to = i + 5;
        } else {
            goto identifier;
        }
    } break;
    case 'f': {
        if (src[i + 1] == 'n') {
            token->type = NS_TOKEN_FN;
            strcpy(us_str, "fn");
            to = i + 2;
        } else if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "f32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "f64");
            to = i + 3;
        } else {
            goto identifier;
        }
    } break;
    case 'i':
        if (src[i + 1] == 'n') {
            token->type = NS_TOKEN_IN;
            strcpy(us_str, "in");
            to = i + 2;
        } else if (src[i + 1] == '3' && src[i + 2] == '2') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "i32");
            to = i + 3;
        } else if (src[i + 1] == '6' && src[i + 2] == '4') {
            token->type = NS_TOKEN_TYPE;
            strcpy(us_str, "i64");
            to = i + 3;
        } else {
            goto identifier;
        }
        break;
    case 'l': {
        if (src[i + 1] == 'e' && src[i + 2] == 't') {
            token->type = NS_TOKEN_LET;
            strcpy(us_str, "let");
            to = i + 3;
        } else {
            goto identifier;
        }
    } break;
    case '\t':
    case '\n':
    case '\r': {
        token->type = NS_TOKEN_SPACE;
        while (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r') {
            i++;
        }
        len = i - from;
        memcpy(us_str, src + from, len);
        us_str[len] = '\0';
        to = i;
    } break;
    case '\0': {
        token->type = NS_TOKEN_EOF;
        us_str[0] = '\0';
    } break;
    identifier:
    default: {
        token->type = NS_TOKEN_IDENTIFIER;
        while ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z')) {
            i++;
        }
        int len = macro_max(i - from, 1);
        memcpy(us_str, src + from, len);
        us_str[len] = '\0';
        to = from + len;
    } break;
    }
    return to;
}
