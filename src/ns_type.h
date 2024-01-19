#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "stb_ds.h"

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

#define ns_str_equals_STR(s, S) (strncmp((s).data, (S), (s).len) == 0)
#define ns_str_printf(s) (printf("%.*s", (s).len, (s).data))

int ns_str_to_int(ns_str s);
f64 ns_str_to_f64(ns_str s);

#define ns_array_push arrpush
#define ns_array_pop arrpop
#define ns_array_len arrlen

#define ns_hash_map_get hmgeti
#define ns_hash_map_set hmput
