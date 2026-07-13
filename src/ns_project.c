#include "ns_project.h"

ns_str ns_project_safe_name(ns_str name) {
    ns_str safe = ns_str_null;
    ns_bool dash = false;
    for (i32 i = 0; i < name.len; i++) {
        i8 c = name.data[i];
        ns_bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        ns_bool digit = c >= '0' && c <= '9';
        if (alpha || digit) {
            if (c >= 'A' && c <= 'Z') c = (i8)(c - 'A' + 'a');
            ns_str_append_len(&safe, &c, 1);
            dash = false;
        } else if (!dash && safe.len > 0) {
            ns_str_append_len(&safe, "-", 1);
            dash = true;
        }
    }
    while (safe.len > 0 && safe.data[safe.len - 1] == '-') {
        safe.len--;
        ns_array_header(safe.data)->len = (size_t)safe.len;
    }
    if (safe.len == 0) ns_str_append_len(&safe, "project", 7);
    if (safe.data[0] >= '0' && safe.data[0] <= '9') {
        ns_str named = ns_str_null;
        ns_str_append_len(&named, "project-", 8);
        ns_str_append(&named, safe);
        ns_array_free(safe.data);
        safe = named;
    }
    ns_array_push(safe.data, '\0');
    safe.dynamic = false; // ns_array-owned process-lifetime storage
    return safe;
}
