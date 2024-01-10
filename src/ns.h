#pragma once

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef false
    #define false 0
#endif

#ifndef true
    #define true 1
#endif

#ifndef macro_max
    #define macro_max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef macro_min
    #define macro_min(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "ns_tokenize.h"
#include "ns_ast.h"
#include "ns_vm.h"

typedef union ns_value_union {
    i32 int32;
    f64 float64;
    void *ptr;
} ns_value_union;

#define NS_NIL ((ns_value){.type = -1})

typedef struct ns_value {
    ns_value_union u;
    ns_type type;
    char *name;
} ns_value;

#ifdef __cplusplus
} // extern "C"
#endif
