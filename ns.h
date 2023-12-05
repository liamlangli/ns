#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int64_t i64;
typedef int32_t i32;
typedef uint64_t u64;
typedef uint32_t u32;
typedef unsigned char u8;
typedef char i8;
typedef float f32;
typedef double f64;
typedef const char *str;

#ifdef __cplusplus
extern "C" {
#endif

enum {
  /* reference count type is negative */
  NS_TAG_FIRST = -7, /* first negative tag */
  NS_TAG_BIG_DECIMAL = -6,
  NS_TAG_BIG_INT = -5,
  NS_TAG_BIG_FLOAT = -4,
  NS_TAG_SYMBOL = -3,
  NS_TAG_STRING = -2,
  NS_TAG_OBJECT = -1,

  NS_TAG_INT = 0,
  NS_TAG_BOOL = 1,
  NS_TAG_NULL = 2,
  NS_TAG_CATCH_OFFSET = 3,
  NS_TAG_EXCEPTION = 6,
  NS_TAG_FLOAT64 = 7,
};

typedef union ns_value_union {
  i32 int32;
  f64 float64;
  void *ptr;
} ns_value_union;

typedef struct ns_value {
  ns_value_union u;
  i64 tag;
} ns_value;
#define ns_const_value ns_value;

#define NS_VALUE_GET_TAG(v) ((i32)(v).tag)

typedef struct ns_runtime_t ns_runtime_t;
typedef struct ns_context_t ns_context_t;

typedef struct ns_string {
  str data;
  u32 length;
  int null_end;
} ns_string;

#define ns_string_str(s)                                                       \
  ((ns_string){                                                                \
      .data = s, .length = s ? (unsigned int)strlen(s) : 0, .null_end = 1})
#define ns_string_STR(s)                                                       \
  ((ns_string){.data = ("" s ""),                                              \
               .length = (unsigned int)(sizeof("" s "") - 1),                  \
               .null_end = 1})
#define ns_string_range(s, e)                                                  \
  ((ns_string){.data = (s), length = (unsigned int)((e) - (s))})

ns_runtime_t *ns_make_runtime();
ns_context_t *ns_make_context(ns_runtime_t *rt);

#define NS_EVAL_FLAG_GLOBAL (0 << 0)        /* global code */
#define NS_EVAL_FLAG_VALIDATE_ONLY (0 << 0) /* validate only */

ns_value ns_eval(ns_context_t *ctx, ns_string source, ns_string filename,
                 int flag);

#define NS_NULL ((ns_value){.tag = NS_TAG_NULL})
int ns_is_null(ns_value value);

#ifdef __cplusplus
} // extern "C"
#endif