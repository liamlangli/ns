#pragma once

#include "ns_type.h"

typedef enum {
    NS_JSON_INVALID = 0,
    NS_JSON_FALSE,
    NS_JSON_TRUE,
    NS_JSON_NULL,
    NS_JSON_NUMBER,
    NS_JSON_STRING,
    NS_JSON_ARRAY,
    NS_JSON_OBJECT,
    NS_JSON_RAW,
} ns_json_type;

typedef struct ns_json {
    i32 next;
    i32 prop; // both array and object
    ns_json_type type;
    ns_str key;
    union {
        ns_str str;
        f64 n;
    };
} ns_json;

typedef struct ns_json_ctx {
    ns_str s;
    i32 i;
} ns_json_ctx;

#define ns_json_null ((ns_json){.type = NS_JSON_NULL})
#define ns_json_false ((ns_json){.type = NS_JSON_FALSE})
#define ns_json_true ((ns_json){.type = NS_JSON_TRUE})
#define ns_json_is_null(j) ((j).type == NS_JSON_NULL)

f64 ns_json_get_number(i32 i);
ns_str ns_json_get_string(i32 i);
i32 ns_json_get_prop(i32 i, ns_str key);
ns_json *ns_json_get(i32 i);

void ns_json_set(i32 j, ns_str key, i32 c);
void ns_json_push(i32 j, i32 c);

i32 ns_json_make_null();
i32 ns_json_make_bool(bool b);
i32 ns_json_make_number(f64 n);
i32 ns_json_make_string(ns_str s);
i32 ns_json_make_array();
i32 ns_json_make_object();

bool ns_json_print(ns_json *json);

i32 ns_json_parse(ns_str s);
ns_str ns_json_to_string(ns_json *json);