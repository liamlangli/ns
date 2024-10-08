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