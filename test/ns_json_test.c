#include "ns_test.h"

int main() {
    ns_str s = ns_str_cstr("{\"a\":1,\"b\":2,\"c\":{\"d\":3,\"e\":4},\"f\":[5,6,7],\"g\":\"h\"}");
    {
        i32 j = ns_json_parse(s);
        ns_str r = ns_json_to_string(ns_json_get(j));
        ns_expect(ns_str_equals(s, r), "ns_json_parse & ns_json_to_string.");
    }

    {
        i32 o = ns_json_make_object();
        ns_json_set(o, ns_str_cstr("a"), ns_json_make_number(1));
        ns_json_set(o, ns_str_cstr("b"), ns_json_make_number(2));
        i32 c = ns_json_make_object();
        ns_json_set(c, ns_str_cstr("d"), ns_json_make_number(3));
        ns_json_set(c, ns_str_cstr("e"), ns_json_make_number(4));
        ns_json_set(o, ns_str_cstr("c"), c);
        i32 a = ns_json_make_array();
        ns_json_push(a, ns_json_make_number(5));
        ns_json_push(a, ns_json_make_number(6));
        ns_json_push(a, ns_json_make_number(7));
        ns_json_set(o, ns_str_cstr("f"), a);
        ns_json_set(o, ns_str_cstr("g"), ns_json_make_string(ns_str_cstr("h")));
        ns_str r = ns_json_to_string(ns_json_get(o));
        ns_expect(ns_str_equals(s, r), "ns_json_make.");
    }

    return 0;
}