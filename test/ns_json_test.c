#include "ns_json.h"

int main() {
    ns_str s = ns_str_cstr("{\"a\": 1, \"b\": 2, \"c\": {\"d\": 3, \"e\": 4}, \"f\": [5, 6, 7], \"g\": \"h\"}");
    i32 j = ns_json_parse(s);
    ns_json_print(ns_json_get(j));
    return 0;
}