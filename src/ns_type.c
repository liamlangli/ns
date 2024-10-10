#include "ns_type.h"

#include <stdlib.h>
#include <string.h>

void *_ns_array_grow(void *a, size_t elem_size, size_t add_count, size_t min_cap) {
    ns_array_header h = {0};
    void *b;
    size_t min_len = ns_array_length(a) + add_count;
    (void) sizeof(h);

    // compute new capacity
    if (min_len > min_cap) min_cap = min_len;
    if (min_cap < ns_array_capacity(a)) return a;
    if (min_cap < 2 * ns_array_capacity(a)) min_cap = 2 * ns_array_capacity(a);
    else if (min_cap < 4) min_cap = 4;

    b = realloc((a) ? ns_array_header(a) : 0, elem_size * min_cap + sizeof(ns_array_header));
    b = (char *)b + sizeof(ns_array_header);
    if (NULL == a) {
        ns_array_header(b)->len = 0;
    }
    ns_array_header(b)->cap = min_cap;
    return b;
}

int ns_str_to_i32(ns_str s) {
    int size = s.len;
    int i = 0;
    int r = 0;
    while (i < size) {
        r = r * 10 + (s.data[i] - '0');
        i++;
    }
    return r;
}

static char _ns_str_buff[256];
f64 ns_str_to_f64(ns_str s) {
    int size = s.len;
    memcpy(_ns_str_buff, s.data, size);
    _ns_str_buff[size] = '\0';
    return atof(_ns_str_buff);
}

ns_str ns_str_unescape(ns_str s) {
    int size = s.len;
    char *data = (char *)malloc(size);
    int i = 0;
    int j = 0;
    while (i < size) {
        if (s.data[i] == '\\') {
            i++;
            switch (s.data[i]) {
            case 'n': data[j] = '\n'; break;
            case 't': data[j] = '\t'; break;
            case 'r': data[j] = '\r'; break;
            case '0': data[j] = '\0'; break;
            case '\\': data[j] = '\\'; break;
            default: data[j] = s.data[i]; break;
            }
        } else {
            data[j] = s.data[i];
        }
        i++;
        j++;
    }
    data[j] = '\0';
    ns_str ret = {.data = data, .len = j};
    return ret;
}