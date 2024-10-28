#include "ns_type.h"

#include <stdlib.h>
#include <string.h>

ns_str ns_type_name(ns_type t) {
    switch (t.type) {
    case NS_TYPE_UNKNOWN: return ns_str_null;
    case NS_TYPE_I8: return ns_str_cstr("i8");
    case NS_TYPE_I16: return ns_str_cstr("i16");
    case NS_TYPE_I32: return ns_str_cstr("i32");
    case NS_TYPE_I64: return ns_str_cstr("i64");
    case NS_TYPE_U8: return ns_str_cstr("u8");
    case NS_TYPE_U16: return ns_str_cstr("u16");
    case NS_TYPE_U32: return ns_str_cstr("u32");
    case NS_TYPE_U64: return ns_str_cstr("u64");
    case NS_TYPE_F32: return ns_str_cstr("f32");
    case NS_TYPE_F64: return ns_str_cstr("f64");
    case NS_TYPE_BOOL: return ns_str_cstr("bool");
    case NS_TYPE_STRING: return ns_str_cstr("str");
    default: return ns_str_null;
    }
}

bool ns_type_is_number(ns_type t) {
    switch (t.type) {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32:
    case NS_TYPE_I64:
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32:
    case NS_TYPE_U64:
    case NS_TYPE_F32:
    case NS_TYPE_F64:
        return true;
    default:
        return false;
    }
}

u32 ns_type_encode(NS_VALUE_TYPE t, i32 i, bool is_ref, bool in_heap) {
    u32 r = t;
    r |= (is_ref) ? NS_REF_MASK : 0;
    r |= (in_heap) ? NS_HEAP_MASK : 0;
    r |= ((i & 0xffffff) << 6);
    return r;
}

ns_number_type ns_vm_number_type(ns_type t) {
    if (ns_type_is_float(t)) return NS_NUMBER_FLT;
    if (ns_type_signed(t)) return NS_NUMBER_I;
    return NS_NUMBER_U;
}

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
    if (ns_null == a) {
        ns_array_header(b)->len = 0;
    }
    ns_array_header(b)->cap = min_cap;
    return b;
}

i32 ns_str_to_i32(ns_str s) {
    i32 size = s.len;
    i32 i = 0;
    i32 r = 0;
    while (i < size) {
        r = r * 10 + (s.data[i] - '0');
        i++;
    }
    return r;
}

static i8 _ns_str_buff[256];
f64 ns_str_to_f64(ns_str s) {
    i32 size = s.len;
    memcpy(_ns_str_buff, s.data, size);
    _ns_str_buff[size] = '\0';
    return atof(_ns_str_buff);
}

ns_str ns_str_unescape(ns_str s) {
    i32 size = s.len;
    i8 *data = (i8 *)malloc(size);
    i32 i = 0;
    i32 j = 0;
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
    ns_str ret = {.data = data, .len = j, .dynamic = 1};
    return ret;
}

i32 ns_str_append_len(ns_str *a, const i8 *data, i32 len) {
    for (i32 i = 0; i < len; i++) {
        ns_array_push(a->data, data[i]);
    }
    a->len += len;
    return a->len;
}