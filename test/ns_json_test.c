#include "ns_json.h"

int main() {
    ns_str s = ns_str_cstr("{\"a\":1,\"b\":2,\"c\":{\"d\":3,\"e\":4},\"f\":[5,6,7],\"g\":\"h\"}");
    i32 j = ns_json_parse(s);
    ns_str r = ns_json_to_string(ns_json_get(j));
    ns_str_printf(r);
    printf("\n");
    if (ns_str_equals(s, r)) {
        ns_info("ns_json_test", "ns_json_to_string: Passed\n");
    } else {
        ns_error("ns_json_test", "ns_json_to_string: Failed\n");
    }
    return 0;
}