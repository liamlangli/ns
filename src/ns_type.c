#include "ns_type.h"

#ifdef NS_DEBUG
typedef struct ns_allocator {
    u64 alloc_op, free_op, realloc_op;
    u64 alloc, realloc;
} ns_allocator;
static ns_allocator _ns_heap = {0};

// memory
void *_ns_malloc(szt size, const_str file, i32 line) {
    ns_unused(file); ns_unused(line);
    void *ptr = malloc(size);
    if (ptr) {
        _ns_heap.alloc_op++;
        _ns_heap.alloc += size;
    }
    return ptr;
}

void *_ns_realloc(void *ptr, szt size, const_str file, i32 line) {
    ns_unused(file); ns_unused(line);
    if (ptr) {
        _ns_heap.realloc_op++;
        _ns_heap.realloc += size;
    }
    void *new_ptr = realloc(ptr, size);
    if (new_ptr) {
        _ns_heap.alloc_op++;
        _ns_heap.alloc += size;
    }
    return new_ptr;
}

void _ns_free(void *ptr, const_str file, i32 line) {
    ns_unused(file); ns_unused(line);
    if (ptr) {
        _ns_heap.free_op++;
    }
    free(ptr);
}

void ns_mem_status(void) {
    ns_info("ns_mem_status[op/fp]", "alloc[%lu/%lu], realloc[%lu/%lu], free[%lu]\n", _ns_heap.alloc_op, _ns_heap.alloc, _ns_heap.realloc_op, _ns_heap.realloc, _ns_heap.free_op);
}
#endif

// ns_type
u64 ns_align(u64 offset, u64 stride) {
    u64 align = ns_min(sizeof(void *), stride);
    if (align > 0) offset = (offset + (align - 1)) & ~(align - 1);
    return offset;
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
void *_ns_array_grow(void *a, szt elem_size, szt add_count, szt min_cap, const_str file, i32 line) 
#else
void *_ns_array_grow(void *a, szt elem_size, szt add_count, szt min_cap)
#endif
{   void *b;
    szt min_len = ns_array_length(a) + add_count;

    // compute new capacity
    if (min_len > min_cap) min_cap = min_len;
    if (min_cap < ns_array_capacity(a)) return a;
    if (min_cap < 2 * ns_array_capacity(a)) min_cap = 2 * ns_array_capacity(a);
    else if (min_cap < 8) min_cap = 8;

    szt new_size = elem_size * min_cap + sizeof(ns_array_header_t);
#ifdef NS_DEBUG
    if (a) {

        b = _ns_realloc(ns_array_header(a), new_size, file, line);
        _ns_heap.alloc_op++;
        _ns_heap.alloc += new_size - (ns_array_capacity(a) * elem_size + sizeof(ns_array_header_t));
    } else {
        b = _ns_malloc(new_size, file, line);
        _ns_heap.alloc_op++;
        _ns_heap.alloc += new_size;
    }
#else
    if (a) {
        b = ns_realloc(ns_array_header(a), new_size);
    else {
        b = ns_malloc(new_size);
    }
#endif

    if (!b) return NULL; // handle allocation failure

    b = (char *)b + sizeof(ns_array_header_t);
    if (ns_null == a) {
        ns_array_header(b)->len = 0;
    }
    ns_array_header(b)->cap = min_cap;
    return b;
}

// ns_str
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

static i8 _ns_str_buff[128];
f64 ns_str_to_f64(ns_str s) {
    i32 size = s.len;
    memcpy(_ns_str_buff, s.data, size);
    _ns_str_buff[size] = '\0';
    return atof(_ns_str_buff);
}

ns_str ns_str_from_i32(i32 i) {
    snprintf(_ns_str_buff, sizeof(_ns_str_buff), "%d", i);
    i32 len = strlen(_ns_str_buff);
    _ns_str_buff[len] = '\0';
    return (ns_str){.data = _ns_str_buff, .len = len, .dynamic = 1};
}

ns_str ns_str_unescape(ns_str s) {
    i32 size = s.len;
    i8 *data = (i8 *)ns_malloc(size);
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
    i8 *d = (i8 *)ns_malloc(end - start + 1);
    memcpy(d, s.data + start, end - start);
    d[end - start] = '\0';
    return ns_str_range(d, end - start);
}

ns_str ns_str_concat(ns_str a, ns_str b) {
    char *buffer = (char *)ns_malloc(a.len + b.len + 1);
    memcpy(buffer, a.data, a.len);
    memcpy(buffer + a.len, b.data, b.len);
    buffer[a.len + b.len] = '\0';
    ns_str data = ns_str_range(buffer, a.len + b.len);
    data.dynamic = true;
    return data;
}

ns_str ns_str_sub_expr(ns_str s) {
    i32 i = 0;
    while (i < s.len && s.data[i] != ' ') {
        i++;
    }
    while (i < s.len && s.data[i] == ' ') {
        i++;
    }
    return ns_str_range(s.data + i, s.len - i);
}

// ns_return
ns_str ns_return_state_str(ns_return_state s) {
    switch (s) {
    case NS_OK: return ns_str_cstr("ok");
    case NS_ERR: return ns_str_cstr("error");
    case NS_ERR_SYNTAX: return ns_str_cstr("syntax error");
    case NS_ERR_EVAL: return ns_str_cstr("eval error");
    case NS_ERR_RUNTIME: return ns_str_cstr("runtime error");
    case NS_ERR_BITCODE: return ns_str_cstr("bitcode error");
    default: return ns_str_cstr("unknown error");
    }
}
