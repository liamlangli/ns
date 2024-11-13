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
    struct ns_json *child;
    i32 type;
    ns_str key;
    union {
        ns_str str;
        f64 n;
    };
} ns_json;

f64 ns_json_get_number(ns_json *json);
ns_str ns_json_get_string(ns_json *json);

ns_json* ns_json_make_null();
ns_json* ns_json_make_bool(bool b);
ns_json* ns_json_make_number(f64 n);
ns_json* ns_json_make_string(ns_str s);
ns_json* ns_json_make_array();
ns_json* ns_json_make_object();

bool ns_json_push(ns_json *json, ns_json *child); // for array
bool ns_json_set(ns_json *json, ns_str key, ns_json *child); // for object

ns_str ns_json_to_string(ns_json *json);
ns_json *ns_json_parse(ns_str s);
bool ns_json_print(ns_json *json);
