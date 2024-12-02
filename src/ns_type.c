#include "ns_type.h"

u64 ns_align(u64 offset, u64 stride) {
    u64 align = ns_min(sizeof(void *), stride);
    if (align > 0) offset = (offset + (align - 1)) & ~(align - 1);
    return offset;
}

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
    case NS_TYPE_FN: return ns_str_cstr("fn");
    case NS_TYPE_STRUCT: return ns_str_cstr("struct");
    default: return ns_str_null;
    }
}

ns_bool ns_type_is_number(ns_type t) {
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
    case NS_TYPE_BOOL:
        return true;
    default:
        return false;
    }
}

ns_type ns_type_encode(ns_value_type t, u64 i,ns_bool is_ref, ns_store s) {
    return (ns_type){.type = t, .index = i, .ref = is_ref, .store = s};
}

ns_number_type ns_vm_number_type(ns_type t) {
    if (ns_type_is_float(t)) return NS_NUMBER_FLT;
    if (ns_type_signed(t)) return NS_NUMBER_I;
    return NS_NUMBER_U;
}

#ifdef NS_DEBUG
typedef struct ns_allocator {
    u64 alloc_op, free_op;
    u64 alloc, free;
} ns_allocator;
static ns_allocator _ns_allocator = {0};
#endif

void *_ns_array_grow(void *a, size_t elem_size, size_t add_count, size_t min_cap) {
    void *b;
    size_t min_len = ns_array_length(a) + add_count;

    // compute new capacity
    if (min_len > min_cap) min_cap = min_len;
    if (min_cap < ns_array_capacity(a)) return a;
    if (min_cap < 2 * ns_array_capacity(a)) min_cap = 2 * ns_array_capacity(a);
    else if (min_cap < 8) min_cap = 8;

    b = malloc(elem_size * min_cap + sizeof(ns_array_header));
#ifdef NS_DEBUG
    _ns_allocator.alloc_op++;
    _ns_allocator.alloc += elem_size * min_cap + sizeof(ns_array_header);
#endif

    if (a) {
#ifdef NS_DEBUG
        _ns_allocator.free_op++;
        _ns_allocator.free += ns_array_capacity(a) * elem_size + sizeof(ns_array_header);
#endif
        memcpy(b, ns_array_header(a), elem_size * ns_array_length(a) + sizeof(ns_array_header));
        free(ns_array_header(a));
    }

    b = (char *)b + sizeof(ns_array_header);
    if (ns_null == a) {
        ns_array_header(b)->len = 0;
    }
    ns_array_header(b)->cap = min_cap;
    return b;
}

void ns_array_status() {
#ifdef NS_DEBUG
    ns_info("ns_array", "alloc_op %llu, free_op %llu, alloc %llu, free %llu\n", _ns_allocator.alloc_op, _ns_allocator.free_op, _ns_allocator.alloc, _ns_allocator.free);
#endif
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

i32 ns_str_index_of(ns_str s, ns_str sub) {
    i32 i = 0;
    i32 j = 0;
    while (i < s.len) {
        if (s.data[i] == sub.data[j]) {
            j++;
            if (j == sub.len) {
                return i - j + 1;
            }
        } else {
            j = 0;
        }
        i++;
    }
    return -1;
}

ns_str ns_str_slice(ns_str s, i32 start, i32 end) {
    char *buffer = (char *)malloc(end - start + 1);
    memcpy(buffer, s.data + start, end - start);
    buffer[end - start] = '\0';
    ns_str data = ns_str_range(buffer, end - start);
    data.dynamic = true;
    return data;
}

ns_str ns_str_concat(ns_str a, ns_str b) {
    char *buffer = (char *)malloc(a.len + b.len + 1);
    memcpy(buffer, a.data, a.len);
    memcpy(buffer + a.len, b.data, b.len);
    buffer[a.len + b.len] = '\0';
    ns_str data = ns_str_range(buffer, a.len + b.len);
    data.dynamic = true;
    return data;
}

ns_str ns_read_file(ns_str path) {
    FILE *file = fopen(path.data, "rb");
    if (!file) {
        return ns_str_null;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = (char *)malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    ns_str data = ns_str_range(buffer, size);
    data.dynamic = true;
    return data;
}

ns_str ns_return_state_str(ns_return_state s) {
    switch (s) {
    case NS_OK: return ns_str_cstr("ok");
    case NS_ERR: return ns_str_cstr("error");
    case NS_ERR_SYNTAX: return ns_str_cstr("syntax error");
    case NS_ERR_EVAL: return ns_str_cstr("eval error");
    case NS_ERR_TYPE: return ns_str_cstr("type error");
    case NS_ERR_RUNTIME: return ns_str_cstr("runtime error");
    case NS_ERR_IMPORT: return ns_str_cstr("import error");
    case NS_ERR_FILE: return ns_str_cstr("file error");
    case NS_ERR_MEMORY: return ns_str_cstr("memory error");
    case NS_ERR_INTERNAL: return ns_str_cstr("internal error");
    case NS_ERR_UNKNOWN: return ns_str_cstr("unknown error");
    default: return ns_str_cstr("unknown error");
    }
}
