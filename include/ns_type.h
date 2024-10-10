#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef bool
    #define bool int
    #define true 1
    #define false 0
#endif

#define ns_error(msg) \
    fprintf(stderr, "%s\n", msg);\

#ifndef macro_max
    #define macro_max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef macro_min
    #define macro_min(a, b) ((a) < (b) ? (a) : (b))
#endif

// types
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
