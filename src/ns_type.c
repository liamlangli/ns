#include "ns_type.h"

#include <stdlib.h>
#include <string.h>

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

static char _cast_buffer[64];
f64 ns_str_to_f64(ns_str s) {
    int size = s.len;
    memcpy(_cast_buffer, s.data, size);
    _cast_buffer[size] = '\0';
    return atof(_cast_buffer);
}