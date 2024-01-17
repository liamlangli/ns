#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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
} ns_str;
#define ns_str_range(s, n) ((ns_str){(s), (n)})
#define ns_str_STR(s) ((ns_str){.data = ""})
#define ns_str_printf(s) (printf("%.*s", (s).len, (s).data))

typedef union ns_value_union {
    i32 int32;
    f64 float64;
    void *ptr;
} ns_value_union;

#define NS_NIL ((ns_value){.u.ptr = NULL})

typedef struct ns_value {
    ns_value_union u;
    char *name;
} ns_value;