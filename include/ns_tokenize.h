#pragma once

#include "ns_type.h"

#define macro_max(a, b) ((a) > (b) ? (a) : (b))
#define macro_min(a, b) ((a) < (b) ? (a) : (b))
#define macro_clamp(x, b, t) (macro_max((b), macro_min((t), (x))))

typedef enum {
    NS_TOKEN_UNKNOWN = -1,
    NS_TOKEN_INVALID = 0,
    NS_TOKEN_AS = 1,
    NS_TOKEN_ASYNC,
    NS_TOKEN_AWAIT,
    NS_TOKEN_BREAK,

    NS_TOKEN_CONST,
    NS_TOKEN_CONTINUE,
    NS_TOKEN_COMMENT,
    NS_TOKEN_DO,
    NS_TOKEN_ELSE,
    NS_TOKEN_FALSE,
    NS_TOKEN_FOR,
    NS_TOKEN_IF,
    NS_TOKEN_IMPORT,
    NS_TOKEN_IN,
    NS_TOKEN_LET,
    NS_TOKEN_LOOP,
    NS_TOKEN_NIL,
    NS_TOKEN_MATCH,
    NS_TOKEN_RETURN,
    NS_TOKEN_REF,
    NS_TOKEN_STRUCT,
    NS_TOKEN_TRUE,

    NS_TOKEN_TYPE_INT8,
    NS_TOKEN_TYPE_INT16,
    NS_TOKEN_TYPE_INT32,
    NS_TOKEN_TYPE_INT64,
    NS_TOKEN_TYPE_UINT8,
    NS_TOKEN_TYPE_UINT16,
    NS_TOKEN_TYPE_UINT32,
    NS_TOKEN_TYPE_UINT64,
    NS_TOKEN_TYPE_F32,
    NS_TOKEN_TYPE_F64,
    NS_TOKEN_TYPE_BOOL,
    NS_TOKEN_TYPE_STR,

    NS_TOKEN_TYPE_DEF,

    NS_TOKEN_TO,
    NS_TOKEN_WHILE,

    NS_TOKEN_INT_LITERAL,
    NS_TOKEN_FLT_LITERAL,
    NS_TOKEN_STR_LITERAL,
    NS_TOKEN_FN,
    NS_TOKEN_SPACE,
    NS_TOKEN_IDENTIFIER,

    NS_TOKEN_COMMA,
    NS_TOKEN_DOT,
    NS_TOKEN_ASSIGN,
    NS_TOKEN_COLON,
    NS_TOKEN_QUESTION_MARK,

    NS_TOKEN_LOGIC_OP,

    NS_TOKEN_ADD_OP,
    NS_TOKEN_MUL_OP,
    NS_TOKEN_SHIFT_OP,

    NS_TOKEN_REL_OP, // relational
    NS_TOKEN_EQ_OP,

    NS_TOKEN_BITWISE_OP,
    NS_TOKEN_ASSIGN_OP,
    NS_TOKEN_BOOL_OP,
    NS_TOKEN_BIT_INVERT_OP,

    NS_TOKEN_OPEN_BRACE,
    NS_TOKEN_CLOSE_BRACE,
    NS_TOKEN_OPEN_PAREN,
    NS_TOKEN_OPS, // keyword for operator overloading
    NS_TOKEN_CLOSE_PAREN,
    NS_TOKEN_OPEN_BRACKET,
    NS_TOKEN_CLOSE_BRACKET,
    NS_TOKEN_EOL,
    NS_TOKEN_EOF
} NS_TOKEN;

typedef struct ns_token_t {
    NS_TOKEN type;
    ns_str val;
    int line, line_start;
} ns_token_t;

const char *ns_token_to_string(NS_TOKEN token);

int ns_next_token(ns_token_t *token, ns_str src, ns_str filename, int from);
void ns_tokenize(ns_str source, ns_str filename);
