#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define macro_max(a, b) ((a) > (b) ? (a) : (b))
#define macro_min(a, b) ((a) < (b) ? (a) : (b))
#define macro_clamp(x, b, t) (macro_max((b), macro_min((t), (x))))

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
} NS_TOKEN;

typedef struct ns_str {
    char *data;
    int len;
} ns_str;
#define ns_str_range(s, n) ((ns_str){(s), (n)})
#define ns_str_STR(s) ((ns_str){.data = ""})

typedef struct ns_token_t {
    NS_TOKEN type;
    ns_str val;
} ns_token_t;

const char *ns_token_to_string(NS_TOKEN token);

int ns_tokenize(ns_token_t *token, char *src, int from);

#ifdef __cplusplus
}
#endif