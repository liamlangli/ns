#pragma once

#include "ns_type.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "ns_tokenize.h"
#include "ns_parse.h"
#include "ns_vm.h"

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

#ifdef __cplusplus
} // extern "C"
#endif
