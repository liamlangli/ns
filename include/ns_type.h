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

#define NS_MAX_PARAMS 16
#define NS_MAX_FIELDS 32
#define NS_MAX_PARSE_STACK 64
#define NS_MAX_CALL_STACK 128
#define NS_MAX_COMPOUND_SECTION_COUNT 128
#define NS_MAX_SECTION_COUNT 256
#define NS_MAX_NODE_COUNT 1024
#define NS_MAX_GLOBAL_VARS 256
#define NS_MAX_FN_COUNT 128
#define NS_MAX_VALUE_COUNT 1024

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

#ifndef macro_max
    #define macro_max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef macro_min
    #define macro_min(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct ns_str {
    const char *data;
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

#define ns_error(msg) \
    fprintf(stderr, "%s\n", msg);\

int ns_str_to_i32(ns_str s);
f64 ns_str_to_f64(ns_str s);
