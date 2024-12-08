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

typedef i32 ns_json_ref;

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

f64 ns_json_to_number(ns_json_ref i);
ns_str ns_json_to_string(ns_json_ref i);
i32 ns_json_to_i32(ns_json_ref i);

ns_json_ref ns_json_get_prop(ns_json_ref i, ns_str key);
ns_json_ref ns_json_get_item(ns_json_ref i, i32 index);
ns_json *ns_json_get(ns_json_ref i);

void ns_json_set(ns_json_ref j, ns_str key, ns_json_ref c);
void ns_json_push(ns_json_ref j, ns_json_ref c);

ns_json_ref ns_json_make_null();
ns_json_ref ns_json_make_bool(ns_bool b);
ns_json_ref ns_json_make_number(f64 n);
ns_json_ref ns_json_make_string(ns_str s);
ns_json_ref ns_json_make_array();
ns_json_ref ns_json_make_object();

ns_bool ns_json_print(ns_json *json);

ns_json_ref ns_json_parse(ns_str s);
ns_str ns_json_stringify(ns_json *json);