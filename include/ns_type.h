#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ns_def
#ifndef bool
    #define bool int
    #define true 1
    #define false 0
#endif

#define ns_color_bld "\x1b[1m"
#define ns_color_err "\x1b[1;31m"
#define ns_color_log "\x1b[1;32m"
#define ns_color_wrn "\x1b[1;33m"
#define ns_color_nil "\x1b[0m"

#ifdef NS_DEBUG
    #define ns_error(t, m, ...)     fprintf(stderr, ns_color_bld "[%s:%d] " ns_color_err "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__);assert(false)
    #define ns_exit(c, t, m, ...)   fprintf(stderr, ns_color_bld "[%s:%d] " ns_color_err "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__);exit(c)
    #define ns_warn(t, m, ...)      fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_wrn "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__)
    #define ns_info(t, m, ...)      fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_log "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__)
#else
    #define ns_error(t, m, ...)     fprintf(stderr, ns_color_err "%s: " ns_color_nil m, t, ##__VA_ARGS__);assert(false)
    #define ns_exit(c, t, m, ...)   fprintf(stderr, ns_color_err "%s: " ns_color_nil m, t, ##__VA_ARGS__);exit(c)
    #define ns_warn(t, m, ...)      fprintf(stdout, ns_color_wrn "%s: " ns_color_nil m, t, ##__VA_ARGS__)
    #define ns_info(t, m, ...)      fprintf(stdout, ns_color_log "%s: " ns_color_nil m, t, ##__VA_ARGS__)
#endif // NS_DEBUG

#define ns_max(a, b) ((a) > (b) ? (a) : (b))
#define ns_min(a, b) ((a) < (b) ? (a) : (b))
#define ns_clamp(x, b, t) (ns_max((b), ns_min((t), (x))))

// ns_type
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

// ns_array
typedef struct ns_array_header {
    size_t len;
    size_t cap;
} ns_array_header;

void *_ns_array_grow(void *a, size_t elem_size, size_t add_count, size_t min_cap);

#define ns_array_header(a) ((ns_array_header *)(a) - 1)
#define ns_array_length(a) ((a) ? (ns_array_header(a))->len : 0)
#define ns_array_capacity(a) ((a) ? ns_array_header(a)->cap : 0)

#define ns_array_grow(a, n, m) ((a) = _ns_array_grow((a), sizeof *(a), (n), (m)))
#define ns_array_ensure(a, n) ((!(a) || ns_array_header(a)->len + (n) > ns_array_header(a)->cap) ? (ns_array_grow(a, n, 0), 0) : 0)

#define ns_array_set_capacity(a, n) (ns_array_grow(a, 0, n))
#define ns_array_set_length(a, n) (ns_array_ensure(a, (n) - ns_array_length(a)), (a) ? ns_array_header(a)->len = (n) : 0)

#define ns_array_push(a, v) (ns_array_ensure(a, 1), (a)[ns_array_header(a)->len++] = (v))
#define ns_array_pop(a) ((a)[--ns_array_header(a)->len])

// ns_str
typedef struct ns_str {
    char *data;
    int len;
    bool dynamic;
} ns_str;

ns_str ns_str_slice(ns_str s, int start, int end);

#define ns_str_null ((ns_str){0, 0, 0})
#define ns_str_range(s, n) ((ns_str){(s), (n), 1})
#define ns_str_cstr(s) ((ns_str){(s), strlen(s), 0})
#define ns_str_free(s) if ((s).dynamic) free((void *)(s).data)

#define ns_str_equals(a, b) ((a).len == (b).len && strncmp((a).data, (b).data, (a).len) == 0)
#define ns_str_equals_STR(s, S) (strncmp((s).data, (S), strlen(S)) == 0)
#define ns_str_printf(s) (printf("%.*s", (s).len, (s).data))

int ns_str_to_i32(ns_str s);
f64 ns_str_to_f64(ns_str s);
ns_str ns_str_unescape(ns_str s);

// ns_token
typedef enum {
    NS_TOKEN_UNKNOWN = -1,
    NS_TOKEN_INVALID = 0,
    NS_TOKEN_AS = 1,        // as: keyword for type casting
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
    NS_TOKEN_OPS,           // ops: keyword for operator overloading

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

    NS_TOKEN_TYPE_DEF,      // type: keyword for type definition

    NS_TOKEN_TO,
    NS_TOKEN_WHILE,

    NS_TOKEN_INT_LITERAL,
    NS_TOKEN_FLT_LITERAL,
    NS_TOKEN_STR_LITERAL,
    NS_TOKEN_FN,
    NS_TOKEN_SPACE,
    NS_TOKEN_IDENTIFIER,

    NS_TOKEN_COMMA,         // ,
    NS_TOKEN_DOT,           // .
    NS_TOKEN_ASSIGN,        // =
    NS_TOKEN_COLON,         // :
    NS_TOKEN_QUESTION_MARK, // ?

    NS_TOKEN_LOGIC_OP,      // !, &&, ||

    NS_TOKEN_ADD_OP,        // +, -
    NS_TOKEN_MUL_OP,        // *, /, %
    NS_TOKEN_SHIFT_OP,      // <<, >>

    NS_TOKEN_REL_OP,        // >, <, >=, <=
    NS_TOKEN_EQ_OP,         // ==, !=

    NS_TOKEN_BITWISE_OP,    // &, |, ^
    NS_TOKEN_ASSIGN_OP,     // +=, -=, *=, /=, %=, &=, |=, ^=
    NS_TOKEN_BOOL_OP,       // &&, ||
    NS_TOKEN_BIT_INVERT_OP, // ~

    NS_TOKEN_OPEN_BRACE,    // {
    NS_TOKEN_CLOSE_BRACE,   // }
    NS_TOKEN_OPEN_PAREN,    // (
    NS_TOKEN_CLOSE_PAREN,   // )
    NS_TOKEN_OPEN_BRACKET,  // [
    NS_TOKEN_CLOSE_BRACKET, // ]
    NS_TOKEN_EOL,
    NS_TOKEN_EOF
} NS_TOKEN;

typedef struct ns_token_t {
    NS_TOKEN type;
    ns_str val;
    int line, line_start;
} ns_token_t;

// ns_value 
typedef enum { 
    NS_TYPE_UNKNOWN = -1,
    NS_TYPE_NIL = 0,
    NS_TYPE_I8,
    NS_TYPE_I16,
    NS_TYPE_I32,
    NS_TYPE_I64,
    NS_TYPE_U8,
    NS_TYPE_U16,
    NS_TYPE_U32,
    NS_TYPE_U64,
    NS_TYPE_F32,
    NS_TYPE_F64,
    NS_TYPE_BOOL,
    NS_TYPE_STRING,
    NS_TYPE_FN,
    NS_TYPE_STRUCT,
    NS_TYPE_ARRAY,
    NS_TYPE_ALIAS,
} NS_VALUE_TYPE;

typedef struct ns_type {
    NS_VALUE_TYPE type;
    ns_str name;
} ns_type;

#define ns_type_unknown ((ns_type){.type = NS_TYPE_UNKNOWN, .name = ns_str_null})

typedef enum {
    NS_SCOPE_GLOBAL,
    NS_SCOPE_LOCAL,
    NS_SCOPE_ARG,
    NS_SCOPE_FIELD
} NS_VALUE_SCOPE;

typedef struct ns_value {
    NS_VALUE_TYPE type;
    int index;
    union {
        i64 i;
        f64 f;
    };
} ns_value;

#define NS_NIL ((ns_value){.type = NS_TYPE_NIL,})
#define NS_TRUE ((ns_value){.type = NS_TYPE_BOOL, .u.boolean = true})
#define NS_FALSE ((ns_value){.type = NS_TYPE_BOOL, .u.boolean = false})
