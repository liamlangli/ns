#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef char i8;
typedef short i16;
typedef int i32;
typedef long i64;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

typedef float f32;
typedef double f64;

typedef enum { NS_TYPE_NIL = -1, NS_TYPE_INT, NS_TYPE_FLOAT, NS_TYPE_FUNCTION, NS_TYPE_STRUCT } ns_type;

typedef enum {
    NS_TOKEN_INT_LITERIAL,
    NS_TOKEN_FLOAT_LITERIAL,
    NS_TOKEN_STRING_LITERIAL,
    NS_TOKEN_CONST,
    NS_TOKEN_LET,
    NS_TOKEN_FN,
    NS_TOKEN_IN,
    NS_TOKEN_SPACE,
    NS_TOKEN_STRUCT,
    NS_TOKEN_IDENTIFIER,
    NS_TOKEN_ASYNC,
    NS_TOKEN_AWAIT,
    NS_TOKEN_TYPE,
    NS_TOKEN_ASSIGN,
    NS_TOKEN_COLON,
    NS_TOKEN_SEMICOLON,
    NS_TOKEN_OPEATOR,
    NS_TOKEN_BITWISE_OPEATOR,
    NS_TOKEN_BOOL_OPEATOR,
    NS_TOKEN_OPEN_BRACE,
    NS_TOKEN_CLOSE_BRACE,
    NS_TOKEN_OPEN_PAREN,
    NS_TOKEN_CLOSE_PAREN,
    NS_TOKEN_OPEN_BRACKET,
    NS_TOKEN_CLOSE_BRACKET,
    NS_TOKEN_EOF
} ns_token;

const char *ns_token_to_string(ns_token token) {
    switch (token) {
    case NS_TOKEN_INT_LITERIAL:
        return "NS_TOKEN_INT_LITERIAL";
    case NS_TOKEN_FLOAT_LITERIAL:
        return "NS_TOKEN_FLOAT_LITERIAL";
    case NS_TOKEN_STRING_LITERIAL:
        return "NS_TOKEN_STRING_LITERIAL";
    case NS_TOKEN_CONST:
        return "NS_TOKEN_CONST";
    case NS_TOKEN_LET:
        return "NS_TOKEN_LET";
    case NS_TOKEN_FN:
        return "NS_TOKEN_FN";
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
    case NS_TOKEN_OPEATOR:
        return "NS_TOKEN_OPEATOR";
    case NS_TOKEN_BITWISE_OPEATOR:
        return "NS_TOKEN_BITWISE_OPEATOR";
    case NS_TOKEN_BOOL_OPEATOR:
        return "NS_TOKEN_BOOL_OPEATOR";
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
        return "NS_TOKEN_UNKNOWN";
    }
}

#define macro_max(a, b) ((a) > (b) ? (a) : (b))
#define macro_min(a, b) ((a) < (b) ? (a) : (b))
#define macro_clamp(x, b, t) (macro_max((b), macro_min((t), (x))))

#ifdef __cplusplus
}
#endif