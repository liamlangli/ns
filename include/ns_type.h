#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ns_def
#ifndef bool
typedef int bool;
    #define true 1
    #define false 0
#endif

#ifndef nil
    #define nil ns_null
#endif

#define ns_color_bld "\x1b[1m"
#define ns_color_err "\x1b[1;31m"
#define ns_color_log "\x1b[1;32m"
#define ns_color_wrn "\x1b[1;33m"
#define ns_color_nil "\x1b[0m"

#define ns_ptr_size sizeof(void *)

#ifdef NS_DEBUG
    #define ns_error(t, m, ...)     fprintf(stderr, ns_color_bld "[%s:%d] " ns_color_err "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__), assert(false)
    #define ns_warn(t, m, ...)      fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_wrn "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__)
    #define ns_info(t, m, ...)      fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_log "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__)
    #define ns_exit(c, t, m, ...)   fprintf(stderr, ns_color_bld "[%s:%d] " ns_color_err "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__), exit(c)
    #define ne_exit_safe(t, m, ...) fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_log "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__), exit(0)
#else
    #define ns_error(t, m, ...)     fprintf(stderr, ns_color_err "%s: " ns_color_nil m, t, ##__VA_ARGS__), assert(false)
    #define ns_warn(t, m, ...)      fprintf(stdout, ns_color_wrn "%s: " ns_color_nil m, t, ##__VA_ARGS__)
    #define ns_info(t, m, ...)      fprintf(stdout, ns_color_log "%s: " ns_color_nil m, t, ##__VA_ARGS__)
    #define ns_exit(c, t, m, ...)   fprintf(stderr, ns_color_err "%s: " ns_color_nil m, t, ##__VA_ARGS__), exit(c)
    #define ne_exit_safe(t, m, ...) fprintf(stdout, ns_color_log "%s: " ns_color_nil m, t, ##__VA_ARGS__), exit(0)
#endif // NS_DEBUG

#define ns_max(a, b) ((a) > (b) ? (a) : (b))
#define ns_min(a, b) ((a) < (b) ? (a) : (b))
#define ns_clamp(x, b, t) (ns_max((b), ns_min((t), (x))))

#define ns_str_case(type) case type: return ns_str_cstr(#type);

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

u64 ns_align(u64 offset, u64 stride);

// ns_array
typedef struct ns_array_header {
    size_t len;
    size_t cap;
} ns_array_header;

void *_ns_array_grow(void *a, size_t elem_size, size_t add_count, size_t min_cap);

#define ns_array_header(a) ((ns_array_header *)(a) - 1)
#define ns_array_length(a) ((a) ? (ns_array_header(a))->len : 0)
#define ns_array_capacity(a) ((a) ? ns_array_header(a)->cap : 0)
#define ns_array_free(a) ((a) ? free(ns_array_header(a)), (a) = 0 : 0)

#define ns_array_grow(a, n, m) ((a) = _ns_array_grow((a), sizeof *(a), (n), (m)))
#define ns_array_ensure(a, n) ((!(a) || ns_array_header(a)->len + (n) > ns_array_header(a)->cap) ? (ns_array_grow(a, n, 0), 0) : 0)

#define ns_array_set_capacity(a, n) (ns_array_grow(a, 0, n))
#define ns_array_set_length(a, n) (ns_array_ensure(a, (n) - ns_array_length(a)), (a) ? ns_array_header(a)->len = (n) : 0)

#define ns_array_push(a, v) (ns_array_ensure(a, 1), (a)[ns_array_header(a)->len++] = (v))
#define ns_array_pop(a) ((a)[--ns_array_header(a)->len])

#define ns_array_last(a) (&(a)[ns_array_length(a) - 1])
#define ns_array_last_safe(a) ((a) ? ns_array_last(a) : 0)

// ns_str
typedef struct ns_str {
    i8 *data;
    i32 len;
    bool dynamic;
} ns_str;

ns_str ns_str_slice(ns_str s, i32 start, i32 end);

i32 ns_str_to_i32(ns_str s);
f64 ns_str_to_f64(ns_str s);
ns_str ns_str_unescape(ns_str s);
i32 ns_str_append_len(ns_str *a, const i8 *data, i32 len);

#define ns_str_null ((ns_str){0, 0, 0})
#define ns_str_range(s, n) ((ns_str){(s), (n), 1})
#define ns_str_cstr(s) ((ns_str){(s), strlen(s), 0})
#define ns_str_free(s) if ((s).dynamic) free((void *)(s).data)

#define ns_str_equals(a, b) ((a).len == (b).len && strncmp((a).data, (b).data, (a).len) == 0)
#define ns_str_equals_STR(s, S) ((!(s).data) ? 0 : (strncmp((s).data, (S), strlen(S)) == 0))
#define ns_str_printf(s) (printf("%.*s", (s).len, (s).data))
#define ns_str_append(a, b) (ns_str_append_len((a), (b).data, (b).len))

#define ns_str_true ns_str_cstr("true")
#define ns_str_false ns_str_cstr("false")
#define ns_str_nil ns_str_cstr("nil")

// ns_data
typedef struct ns_data {
    void *data;
    size_t len;
} ns_data;

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
    NS_TOKEN_LOOP,
    NS_TOKEN_ELSE,
    NS_TOKEN_FALSE,
    NS_TOKEN_FOR,
    NS_TOKEN_TO,
    NS_TOKEN_IF,
    NS_TOKEN_IMPORT,
    NS_TOKEN_IN,
    NS_TOKEN_LET,
    NS_TOKEN_NIL,
    NS_TOKEN_MATCH,
    NS_TOKEN_RETURN,
    NS_TOKEN_REF,
    NS_TOKEN_STRUCT,
    NS_TOKEN_TRUE,
    NS_TOKEN_TYPE,
    NS_TOKEN_KERNEL,
    NS_TOKEN_OPS,           // ops: keyword for operator overloading

    NS_TOKEN_TYPE_I8,
    NS_TOKEN_TYPE_I16,
    NS_TOKEN_TYPE_I32,
    NS_TOKEN_TYPE_I64,
    NS_TOKEN_TYPE_U8,
    NS_TOKEN_TYPE_U16,
    NS_TOKEN_TYPE_U32,
    NS_TOKEN_TYPE_U64,
    NS_TOKEN_TYPE_F32,
    NS_TOKEN_TYPE_F64,
    NS_TOKEN_TYPE_BOOL,
    NS_TOKEN_TYPE_STR,
    NS_TOKEN_TYPE_ANY,

    NS_TOKEN_INT_LITERAL,
    NS_TOKEN_FLT_LITERAL,
    NS_TOKEN_STR_LITERAL,
    NS_TOKEN_STR_FORMAT,
    NS_TOKEN_FN,
    NS_TOKEN_SPACE,
    NS_TOKEN_IDENTIFIER,

    NS_TOKEN_COMMA,         // ,
    NS_TOKEN_DOT,           // .
    NS_TOKEN_ASSIGN,        // =
    NS_TOKEN_COLON,         // :
    NS_TOKEN_QUESTION_MARK, // ?

    NS_TOKEN_CMP_OP,        // !
    NS_TOKEN_BIT_INVERT_OP, // ~

    NS_TOKEN_ADD_OP,        // +, -
    NS_TOKEN_SHIFT_OP,      // <<, >>
    NS_TOKEN_MUL_OP,        // *, /, %

    NS_TOKEN_REL_OP,        // >, <, >=, <=
    NS_TOKEN_EQ_OP,         // ==, !=

    NS_TOKEN_LOGIC_OP,      // &&, ||

    NS_TOKEN_BITWISE_OP,    // &, |, ^
    NS_TOKEN_ASSIGN_OP,     // +=, -=, *=, /=, %=, &=, |=, ^=

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
    NS_TYPE_UNKNOWN = 0,
    NS_TYPE_NIL,
    NS_TYPE_VOID,
    NS_TYPE_INFER,
    NS_TYPE_I8,
    NS_TYPE_U8,
    NS_TYPE_I16,
    NS_TYPE_U16,
    NS_TYPE_I32,
    NS_TYPE_U32,
    NS_TYPE_I64,
    NS_TYPE_U64,
    NS_TYPE_F32,
    NS_TYPE_F64,
    NS_TYPE_BOOL,
    NS_TYPE_STRING,
    NS_TYPE_FN,
    NS_TYPE_STRUCT,
    NS_TYPE_ARRAY,
    NS_TYPE_ALIAS,
} ns_value_type;

// f and i, f and u, i and u
typedef enum {
    NS_NUMBER_FLT = 1,
    NS_NUMBER_I = 2,
    NS_NUMBER_U = 4,
    NS_NUMBER_FLT_AND_I = 3,
    NS_NUMBER_FLT_AND_U = 5,
    NS_NUMBER_I_AND_U = 6,
} ns_number_type;

typedef enum {
    NS_STORE_CONST = (u8)0,
    NS_STORE_STACK = (u8)1,
    NS_STORE_HEAP = (u8)2,
} ns_store;

typedef struct ns_type {
    u8 ref;
    u8 store;
    u16 type;
    u32 index;
} ns_type;

#define ns_type_is_ref(t) (t.ref != (u8)0)
#define ns_type_is_const(t) (NS_STORE_CONST == t.store)
#define ns_type_in_stack(t) (NS_STORE_STACK == t.store)
#define ns_type_in_heap(t) (NS_STORE_HEAP == t.store)
#define ns_type_index(t) (t.index)
#define ns_type_set_store(t, s) ((ns_type){.ref = t.ref, .store = s, .type = t.type, .index = t.index})
#define ns_type_equals(a, b) (a.type == b.type && a.index == b.index)

#define ns_type_unknown (ns_type){.type = NS_TYPE_UNKNOWN}

ns_type ns_type_encode(ns_value_type t, u64 i, bool is_ref, ns_store s);

#define ns_type_infer   (ns_type){.type = NS_TYPE_INFER}
#define ns_type_void    (ns_type){.type = NS_TYPE_VOID}
#define ns_type_nil     (ns_type){.type = NS_TYPE_NIL}
#define ns_type_bool    (ns_type){.type = NS_TYPE_BOOL}
#define ns_type_str     (ns_type){.type = NS_TYPE_STRING}
#define ns_type_array   (ns_type){.type = NS_TYPE_ARRAY}

#define ns_type_i8  (ns_type){.type = NS_TYPE_I8}
#define ns_type_i16 (ns_type){.type = NS_TYPE_I16}
#define ns_type_i32 (ns_type){.type = NS_TYPE_I32}
#define ns_type_i64 (ns_type){.type = NS_TYPE_I64}
#define ns_type_u8  (ns_type){.type = NS_TYPE_U8}
#define ns_type_u16 (ns_type){.type = NS_TYPE_U16}
#define ns_type_u32 (ns_type){.type = NS_TYPE_U32}
#define ns_type_u64 (ns_type){.type = NS_TYPE_U64}
#define ns_type_f32 (ns_type){.type = NS_TYPE_F32}
#define ns_type_f64 (ns_type){.type = NS_TYPE_F64}

#define ns_type_fn     NS_TYPE_FN
#define ns_type_struct NS_TYPE_STRUCT

#define ns_type_is(t, tt) (t.type == tt)
#define ns_type_is_float(t) (ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64))
#define ns_type_signed(t) (ns_type_is(t, NS_TYPE_I8) || ns_type_is(t, NS_TYPE_I16) || ns_type_is(t, NS_TYPE_I32) || ns_type_is(t, NS_TYPE_I64))
#define ns_type_unsigned(t) (ns_type_is(t, NS_TYPE_U8) || ns_type_is(t, NS_TYPE_U16) || ns_type_is(t, NS_TYPE_U32) || ns_type_is(t, NS_TYPE_U64))
#define ns_type_is_unknown(t) (ns_type_is(t, NS_TYPE_UNKNOWN))

bool ns_type_is_number(ns_type t);

typedef struct ns_value {
    ns_type t;  // type
    union {
        u64 o;
        f64 f64;
        u64 u64;
        i64 i64;
        bool b;
        i8 i8;
        u8 u8;
        i16 i16;
        u16 u16;
        i32 i32;
        u32 u32;
        f32 f32;
    };
} ns_value;

#define ns_null NULL
#define ns_nil          ((ns_value){.t = ns_type_nil, .o = 0})
#define ns_is_nil(v)    ns_type_is(v.t, NS_TYPE_NIL)
#define ns_true         ((ns_value){.t = ns_type_bool, .b = true})
#define ns_false        ((ns_value){.t = ns_type_bool, .b = false})