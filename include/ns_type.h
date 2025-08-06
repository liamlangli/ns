#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

#if NS_DARWIN
    #define ns_lib_ext ns_str_cstr(".dylib")
    #define ns_export __attribute__((visibility("default")))
#elif NS_WIN
    #define ns_lib_ext ns_str_cstr(".dll")
    #define ns_export __declspec(dllexport)
#else
    #define ns_lib_ext ns_str_cstr(".so")
    #define ns_export __attribute__((visibility("default")))
#endif

// ns_def
#ifndef ns_bool
    typedef int ns_bool;
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
#define ns_color_cmt "\x1b[1;37m"
#define ns_color_ign "\x1b[0;90m"
#define ns_color_nil "\x1b[0m"

#define ns_ptr_size sizeof(void *)

#ifdef NS_DEBUG
    #define ns_error(t, m, ...)     fprintf(stderr, ns_color_bld "[%s:%d] " ns_color_err "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__), assert(false)
    #define ns_warn(t, m, ...)      fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_wrn "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__)
    #define ns_info(t, m, ...)      fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_log "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__)
    #define ns_exit(c, t, m, ...)   fprintf(stderr, ns_color_bld "[%s:%d] " ns_color_err "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__), exit(c)
    #define ns_exit_safe(t, m, ...) fprintf(stdout, ns_color_bld "[%s:%d] " ns_color_log "%s: " ns_color_nil m, __FILE__, __LINE__, t, ##__VA_ARGS__), exit(0)
#else
    #define ns_error(t, m, ...)     fprintf(stderr, ns_color_err "%s: " ns_color_nil m, t, ##__VA_ARGS__), assert(false)
    #define ns_warn(t, m, ...)      fprintf(stdout, ns_color_wrn "%s: " ns_color_nil m, t, ##__VA_ARGS__)
    #define ns_info(t, m, ...)      fprintf(stdout, ns_color_log "%s: " ns_color_nil m, t, ##__VA_ARGS__)
    #define ns_exit(c, t, m, ...)   fprintf(stderr, ns_color_err "%s: " ns_color_nil m, t, ##__VA_ARGS__), exit(c)
    #define ns_exit_safe(t, m, ...) fprintf(stdout, ns_color_log "%s: " ns_color_nil m, t, ##__VA_ARGS__), exit(0)
#endif // NS_DEBUG

#define ns_max(a, b) ((a) > (b) ? (a) : (b))
#define ns_min(a, b) ((a) < (b) ? (a) : (b))
#define ns_clamp(x, b, t) (ns_max((b), ns_min((t), (x))))
#define ns_unused(x) (void)(x)

#define ns_str_case(type) case type: return ns_str_cstr(#type);

// ns_type
typedef char i8;
typedef short i16;
typedef int i32;
typedef long i64;
typedef size_t szt;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

typedef float f32;
typedef double f64;

typedef const char * const_str;

u64 ns_align(u64 offset, u64 stride);

// memory
#ifdef NS_DEBUG
    void *_ns_malloc(szt size, const_str file, i32 line);
    void *_ns_realloc(void *ptr, szt size, const_str file, i32 line);
    void _ns_free(void *ptr, const_str file, i32 line);
    void ns_mem_status(void);

    #define ns_malloc(size) _ns_malloc(size, __FILE__, __LINE__)
    #define ns_realloc(ptr, size) _ns_realloc(ptr, size, __FILE__, __LINE__)
    #define ns_free(ptr) _ns_free(ptr, __FILE__, __LINE__)    
#else
    #define ns_malloc(size) malloc(size)
    #define ns_realloc(ptr, size) realloc(ptr, size)
    #define ns_free(ptr) free(ptr)
    #define ns_mem_status() (void)0
#endif

// ns_str
typedef struct ns_str {
    i8 *data;
    i32 len;
    ns_bool dynamic;
} ns_str;

#define ns_str_null ((ns_str){0, 0, 0})
#define ns_str_range(s, n) ((ns_str){(s), (n), 1})
#define ns_str_cstr(s) ((ns_str){(i8*)(s), strlen(s), 0})
#define ns_str_free(s) if ((s).dynamic) ns_free((void *)(s).data)

#define ns_str_equals(a, b) ((a).len == (b).len && strncmp((a).data, (b).data, (a).len) == 0)
#define ns_str_equals_STR(s, S) ((!(s).data) ? 0 : (strncmp((s).data, (S), strlen(S)) == 0))
#define ns_str_starts_with(a, b) ((a).len >= (b).len && strncmp((a).data, (b).data, (b).len) == 0)
#define ns_str_printf(s) (printf("%.*s", (s).len, (s).data))
#define ns_str_append(a, b) (ns_str_append_len((a), (b).data, (b).len))

#define ns_str_true ns_str_cstr("true")
#define ns_str_false ns_str_cstr("false")
#define ns_str_nil ns_str_cstr("nil")
#define ns_str_is_empty(s) ((s).len == 0)
#define ns_str_clear(s) ((s) ? ((s)->len = 0, ns_array_set_length((s)->data, 0)) : (void)0)

ns_str ns_str_slice(ns_str s, i32 start, i32 end);
ns_str ns_str_concat(ns_str a, ns_str b);

i32 ns_str_to_i32(ns_str s);
f64 ns_str_to_f64(ns_str s);
ns_str ns_str_from_i32(i32 i);
ns_str ns_str_unescape(ns_str s);
i32 ns_str_append_len(ns_str *a, const i8 *data, i32 len);
i32 ns_str_append_i32(ns_str *a, i32 i);
i32 ns_str_append_f64(ns_str *s, f64 n, i32 precision);
i32 ns_str_index_of(ns_str s, ns_str sub);
ns_str ns_str_sub_expr(ns_str s); // get inplace sub expr from first space to end of line

#define ns_str_case(type) case type: return ns_str_cstr(#type);

// ns_data
typedef struct ns_data {
    void *data;
    size_t len;
} ns_data;

#define ns_data_null ((ns_data){.data = NULL, .len = 0})

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
    NS_TOKEN_MODULE,
    NS_TOKEN_RETURN,
    NS_TOKEN_REF,
    NS_TOKEN_STRUCT,
    NS_TOKEN_TRUE,
    NS_TOKEN_TYPE,
    NS_TOKEN_VERTEX,
    NS_TOKEN_FRAGMENT,
    NS_TOKEN_COMPUTE,
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
    NS_TOKEN_TYPE_STR,     // str: keyword for string
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

    NS_TOKEN_RETURN_TYPE,   // ->

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
    NS_TYPE_BLOCK,
    NS_TYPE_STRUCT,
    NS_TYPE_ARRAY,
    NS_TYPE_ALIAS,
} ns_value_type;

typedef enum {
    NS_FN_GENERIC = 0,
    NS_FN_ASYNC,
    NS_FN_KERNEL,
    NS_FN_VERTEX,
    NS_FN_FRAGMENT,
    NS_FN_COMPUTE
} ns_fn_type;

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
    NS_STORE_CONST = 0,
    NS_STORE_STACK = 1,
    NS_STORE_HEAP = 2,
} ns_store;

typedef struct ns_type {
    ns_value_type type: 8;
    ns_store store: 4;
    ns_bool ref: 2;
    ns_bool array: 2;
    u32 index;
} ns_type;

#define ns_type_is_ref(t) (t.ref != 0)
#define ns_type_is_array(t) (t.array != 0)
#define ns_type_is_mut(t) (NS_STORE_CONST == t.store)
#define ns_type_in_stack(t) (NS_STORE_STACK == t.store)
#define ns_type_in_heap(t) (NS_STORE_HEAP == t.store)
#define ns_type_index(t) (t.index)
#define ns_type_set_store(t, s) ((ns_type){.ref = t.ref, .store = s, .type = t.type, .array = t.array, .index = t.index})
#define ns_type_set_ref(t, r) ((ns_type){.ref = r, .store = t.store, .type = t.type, .array = t.array, .index = t.index})
#define ns_type_unknown (ns_type){.type = NS_TYPE_UNKNOWN}

#define ns_type_equals(a, b) (a.type == b.type && a.index == b.index)
ns_type ns_type_encode(ns_value_type t, u64 i, ns_bool is_ref, ns_store s);

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

#define ns_type_is(t, tt) (t.type == tt)
#define ns_type_is_float(t) (ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64))
#define ns_type_signed(t) (ns_type_is(t, NS_TYPE_I8) || ns_type_is(t, NS_TYPE_I16) || ns_type_is(t, NS_TYPE_I32) || ns_type_is(t, NS_TYPE_I64))
#define ns_type_unsigned(t) (ns_type_is(t, NS_TYPE_U8) || ns_type_is(t, NS_TYPE_U16) || ns_type_is(t, NS_TYPE_U32) || ns_type_is(t, NS_TYPE_U64))
#define ns_type_is_unknown(t) (ns_type_is(t, NS_TYPE_UNKNOWN))

ns_bool ns_type_is_number(ns_type t);

typedef struct ns_value {
    ns_type t;  // type
    union {
        u64 o;
        f64 f64;
        u64 u64;
        i64 i64;
        ns_bool b;
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

// ns_array
typedef struct ns_array_header_t {
    size_t len;
    size_t cap;
    ns_type type;
} ns_array_header_t;

#ifdef NS_DEBUG
void *_ns_array_grow(void *a, szt elem_size, szt add_count, szt min_cap, const_str file, i32 line);
#else
void *_ns_array_grow(void *a, szt elem_size, szt add_count, szt min_cap);
#endif

#define ns_array_header(a) ((ns_array_header_t *)(a) - 1)
#define ns_array_length(a) ((a) ? (ns_array_header(a))->len : 0)
#define ns_array_capacity(a) ((a) ? ns_array_header(a)->cap : 0)
#define ns_array_type(a) ((a) ? ns_array_header(a)->type : ns_type_unknown)
#define ns_array_free(a) ((a) ? ns_free(ns_array_header(a)), (a) = 0 : 0)

#define ns_array_grow(a, n, m) ((a) = _ns_array_grow((a), sizeof *(a), (n), (m), __FILE__, __LINE__))
#define ns_array_ensure(a, n) ((!(a) || ns_array_header(a)->len + (n) > ns_array_header(a)->cap) ? (ns_array_grow(a, n, 0), 0) : 0)

#define ns_array_set_capacity(a, n) (ns_array_grow(a, 0, n))
#define ns_array_set_length(a, n) (ns_array_ensure(a, (n) - ns_array_length(a)), (a) ? ns_array_header(a)->len = (n) : 0)
#define ns_array_set_type(a, t) ((a) ? ns_array_header(a)->type = (t) : 0)

#define ns_array_push(a, v) (ns_array_ensure(a, 1), (a)[ns_array_header(a)->len++] = (v))
#define ns_array_pop(a) ((a)[--ns_array_header(a)->len])

#define ns_array_last(a) (&(a)[ns_array_length(a) - 1])
#define ns_array_last_safe(a) ((a) ? ns_array_last(a) : 0)

#define ns_array_insert(a, i, v) (ns_array_ensure(a, 1), memmove(&(a)[(i) + 1], &(a)[i], (ns_array_length(a) - (i)) * sizeof *(a)), (a)[i] = (v), ns_array_header(a)->len++)
#define ns_array_splice(a, i) (memmove(&(a)[i], &(a)[(i) + 1], (ns_array_length(a) - (i) - 1) * sizeof *(a)), ns_array_header(a)->len--)
#define ns_array_append(a, b) (ns_array_ensure(a, ns_array_length(b)), memcpy(&(a)[ns_array_length(a)], (b), ns_array_length(b) * sizeof *(b)), ns_array_header(a)->len += ns_array_length(b))

// ns return
typedef enum {
    NS_OK = 0,
    NS_ERR,
    NS_ERR_SYNTAX,
    NS_ERR_EVAL,
    NS_ERR_RUNTIME,
    NS_ERR_BITCODE,
    NS_ERR_UNKNOWN,
} ns_return_state;

typedef struct ns_code_loc {
    ns_str f;
    i32 l, o;
} ns_code_loc;
#define ns_code_loc_nil ((ns_code_loc){ns_str_cstr(""), 0, 0})

typedef struct ns_return {
    ns_str msg;
    ns_code_loc loc;
} ns_return;

#define ns_return_define(t, v) typedef struct ns_return_##t { ns_return_state s; union { v r; ns_return e; }; } ns_return_##t
ns_return_define(type, ns_type);
ns_return_define(value, ns_value);
ns_return_define(bool, ns_bool);
ns_return_define(u64, u64);
ns_return_define(ptr, void *);

#define ns_return_ok(t, v) ((ns_return_##t){.r = (v)})
#ifdef NS_DEBUG
    #define ns_return_error(t, l, err, m) assert(false), ((ns_return_##t){.s = err, .e = {.msg = ns_str_cstr(m), .loc = l}})
#else
    #define ns_return_error(t, l, err, m) ((ns_return_##t){.s = err, .e = {.msg = ns_str_cstr(m), .loc = l}})
#endif
#define ns_return_is_error(r) ((r).s != NS_OK)
#define ns_return_change_type(t, err) ((ns_return_##t){.s = err.s, .e = err.e})

ns_str ns_return_state_str(ns_return_state s);
#define ns_return_assert(r) if (ns_return_is_error(r)) ns_error(ns_return_state_str(r.s).data, "%.*s:%d:%d:\n    %.*s\n", r.e.loc.f.len, r.e.loc.f.data, r.e.loc.l, r.e.loc.o, r.e.msg.len, r.e.msg.data)

typedef struct ns_return_void { ns_return_state s; ns_return e; } ns_return_void;
#define ns_return_ok_void ((ns_return_void){.s = NS_OK})
