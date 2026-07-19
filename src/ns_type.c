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

ns_str ns_mem_str(u64 i) {
    if (i < 1024) {
        ns_str s = ns_str_from_i32((i32)i);
        ns_str_append(&s, ns_str_cstr(ns_color_ign "B" ns_color_nil));
        return s;
    } else if (i < 1024 * 1024) {
        ns_str s = ns_str_from_i32((i32)i >> 10);
        ns_str_append(&s, ns_str_cstr(ns_color_ign "KB" ns_color_nil));
        return s;
    } else if (i < 1024 * 1024 * 1024) {
        ns_str s = ns_str_from_i32((i32)i >> 20);
        ns_str_append(&s, ns_str_cstr(ns_color_ign "MB" ns_color_nil));
        return s;
    }
    ns_str s = ns_str_from_i32((i32)i >> 30);
    ns_str_append(&s, ns_str_cstr(ns_color_ign "GB" ns_color_nil));
    return s;
}

void ns_mem_status(void) {
    ns_str alloc = ns_mem_str(_ns_heap.alloc);
    ns_str realloc = ns_mem_str(_ns_heap.realloc);
    ns_info("ns_mem_status", "alloc[%lu|%.*s], realloc[%lu|%.*s], free[%lu]\n", _ns_heap.alloc_op, alloc.len, alloc.data, _ns_heap.realloc_op, realloc.len, realloc.data, _ns_heap.free_op);
}
#endif

// ns_type
u64 ns_align(u64 offset, u64 stride) {
    u64 align = ns_min(sizeof(void *), stride);
    if (align > 0) offset = (offset + (align - 1)) & ~(align - 1);
    return offset;
}

static void ns_return_print_source_line(FILE *file, i32 line, i32 target, i32 col) {
    ns_bool selected = line == target - 1 || line == target || line == target + 1;

    if (selected) fprintf(stderr, "  %5d | ", line);
    for (;;) {
        int ch = fgetc(file);
        if (ch == EOF || ch == '\n') {
            if (selected) {
                fprintf(stderr, "\n");
                if (line == target && col >= 0) {
                    fprintf(stderr, "        | ");
                    for (i32 i = 0; i < col; ++i) fputc(' ', stderr);
                    fprintf(stderr, "^\n");
                }
            }
            return;
        }

        if (selected) {
            fputc(ch, stderr);
        }
    }
}

static void ns_return_print_source_context(ns_code_loc loc) {
    if (loc.f.len == 0 || loc.f.data == ns_null || loc.l <= 0) return;

    char *path = (char *)ns_malloc((szt)loc.f.len + 1);
    if (!path) return;
    memcpy(path, loc.f.data, (szt)loc.f.len);
    path[loc.f.len] = '\0';

    FILE *file = fopen(path, "r");
    ns_free(path);
    if (!file) return;

    i32 end = loc.l + 1;
    for (i32 line = 1; line <= end; ++line) {
        if (feof(file)) break;
        ns_return_print_source_line(file, line, loc.l, loc.o);
    }

    fclose(file);
}

static void ns_return_exit_or_recover(void) {
    if (getenv("NS_REPL_RECOVER") == NULL) {
        exit(1);
    }
}

void ns_return_print_error(ns_return_state s, ns_return e) {
    ns_str state = ns_return_state_str(s);
    fprintf(stderr, ns_color_err "%.*s: " ns_color_nil "%.*s\n", state.len, state.data, e.msg.len, e.msg.data);
    if (e.loc.f.len > 0 && e.loc.f.data != ns_null) {
        fprintf(stderr, "  --> %.*s:%d:%d\n", e.loc.f.len, e.loc.f.data, e.loc.l, e.loc.o);
        ns_return_print_source_context(e.loc);
    }
    ns_return_exit_or_recover();
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
        // Capture the old size before realloc frees the old buffer; reading the
        // header through `a` afterwards would be a use-after-free.
        szt old_size = ns_array_capacity(a) * elem_size + sizeof(ns_array_header_t);
        b = _ns_realloc(ns_array_header(a), new_size, file, line);
        _ns_heap.alloc_op++;
        _ns_heap.alloc += new_size - old_size;
    } else {
        b = _ns_malloc(new_size, file, line);
        _ns_heap.alloc_op++;
        _ns_heap.alloc += new_size;
    }
#else
    if (a) {
        b = ns_realloc(ns_array_header(a), new_size);
    } else {
        b = ns_malloc(new_size);
    }
#endif

    if (!b) return NULL; // handle allocation failure

    b = (char *)b + sizeof(ns_array_header_t);
    if (ns_null == a) {
        ns_array_header(b)->len = 0;
    }
    ns_array_header(b)->cap = min_cap;
    ns_array_header(b)->elem_size = elem_size;
    return b;
}

void *ns_buffer_alloc(szt elem_size, szt len, ns_type elem_type) {
    if (elem_size == 0) elem_size = 1;
    szt payload_size = elem_size * len;
    ns_array_header_t *h = (ns_array_header_t *)ns_malloc(sizeof(ns_array_header_t) + payload_size);
    if (!h) return ns_null;
    h->len = len;
    h->cap = len;
    h->elem_size = elem_size;
    h->type = elem_type;
    void *data = (void *)(h + 1);
    if (payload_size > 0) memset(data, 0, payload_size);
    return data;
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
    case NS_TYPE_UNION: return ns_str_cstr("union");
    case NS_TYPE_ENUM: return ns_str_cstr("enum");
    case NS_TYPE_DICT: return ns_str_cstr("dict");
    case NS_TYPE_SET: return ns_str_cstr("set");
    default: return ns_str_null;
    }
}

i64 ns_str_to_i64(ns_str s) {
    i32 size = s.len;
    i32 i = 0;
    i64 sign = 1;
    i64 r = 0;

    if (i < size && (s.data[i] == '-' || s.data[i] == '+')) {
        if (s.data[i] == '-') sign = -1;
        i++;
    }

    // hex literal: 0x.. / 0X..
    if (i + 1 < size && s.data[i] == '0' && (s.data[i + 1] == 'x' || s.data[i + 1] == 'X')) {
        i += 2;
        while (i < size) {
            i8 c = s.data[i];
            i64 d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            r = r * 16 + d;
            i++;
        }
    } else {
        while (i < size) {
            i8 c = s.data[i];
            if (c < '0' || c > '9') break;
            r = r * 10 + (c - '0');
            i++;
        }
    }

    return sign * r;
}

i32 ns_str_to_i32(ns_str s) {
    return (i32)ns_str_to_i64(s);
}

static i8 _ns_str_buff[128];
f64 ns_str_to_f64(ns_str s) {
    i32 size = s.len;
    memcpy(_ns_str_buff, s.data, size);
    _ns_str_buff[size] = '\0';
    return atof(_ns_str_buff);
}

ns_str ns_str_from_i32(i32 i) {
    ns_str s = {.dynamic = 1};
    szt len = snprintf(_ns_str_buff, sizeof(_ns_str_buff), "%d", i);
    ns_array_set_length(s.data, len);
    strcpy(s.data, _ns_str_buff);
    s.len = len;
    return s;
}

i32 ns_utf8_seq_len(i8 lead) {
    u8 c = (u8)lead;
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 0;
}

i32 ns_utf8_decode(ns_str s, i32 i, u32 *cp) {
    // low bits kept from the lead byte, and the smallest codepoint each
    // sequence length is allowed to encode (rejects overlong forms)
    static const u8 lead_mask[5] = {0, 0x7f, 0x1f, 0x0f, 0x07};
    static const u32 min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (i < 0 || i >= s.len) return 0;
    i32 n = ns_utf8_seq_len(s.data[i]);
    if (n == 0 || i + n > s.len) return 0;
    u32 c = (u8)s.data[i] & lead_mask[n];
    for (i32 k = 1; k < n; k++) {
        u8 b = (u8)s.data[i + k];
        if ((b & 0xc0) != 0x80) return 0;
        c = (c << 6) | (b & 0x3f);
    }
    if (c < min_cp[n] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    if (cp) *cp = c;
    return n;
}

i32 ns_utf8_encode(u32 cp, i8 *buf) {
    if (cp < 0x80) {
        buf[0] = (i8)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (i8)(0xc0 | (cp >> 6));
        buf[1] = (i8)(0x80 | (cp & 0x3f));
        return 2;
    } else if (cp < 0x10000) {
        if (cp >= 0xd800 && cp <= 0xdfff) return 0; // surrogates never appear in utf8
        buf[0] = (i8)(0xe0 | (cp >> 12));
        buf[1] = (i8)(0x80 | ((cp >> 6) & 0x3f));
        buf[2] = (i8)(0x80 | (cp & 0x3f));
        return 3;
    } else if (cp <= 0x10ffff) {
        buf[0] = (i8)(0xf0 | (cp >> 18));
        buf[1] = (i8)(0x80 | ((cp >> 12) & 0x3f));
        buf[2] = (i8)(0x80 | ((cp >> 6) & 0x3f));
        buf[3] = (i8)(0x80 | (cp & 0x3f));
        return 4;
    }
    return 0;
}

i32 ns_str_utf8_len(ns_str s) {
    i32 i = 0, n = 0;
    while (i < s.len) {
        i32 l = ns_utf8_decode(s, i, ns_null);
        i += l ? l : 1;
        n++;
    }
    return n;
}

ns_bool ns_str_utf8_valid(ns_str s) {
    i32 i = 0;
    while (i < s.len) {
        i32 l = ns_utf8_decode(s, i, ns_null);
        if (l == 0) return false;
        i += l;
    }
    return true;
}

static i32 ns_hex_digit(i8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ns_str ns_str_unescape(ns_str s) {
    i32 size = s.len;
    i8 *data = (i8 *)ns_malloc(size + 1); // +1 for the trailing '\0'
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
            case 'u': {
                // \u{XXXX}: hex codepoint written out as utf8 bytes. The
                // spelling is always at least as long as the encoded bytes,
                // so the size+1 output buffer still fits. A malformed escape
                // falls through and keeps the 'u' verbatim like any other
                // unknown escape.
                i32 k = i + 1;
                u32 cp = 0;
                ns_bool ok = k < size && s.data[k] == '{';
                if (ok) {
                    k++;
                    i32 digits = 0;
                    while (k < size && s.data[k] != '}') {
                        i32 d = ns_hex_digit(s.data[k]);
                        if (d < 0 || cp > 0x10ffff) { ok = false; break; }
                        cp = cp * 16 + d;
                        digits++;
                        k++;
                    }
                    ok = ok && digits > 0 && k < size && s.data[k] == '}';
                }
                i32 n = ok ? ns_utf8_encode(cp, data + j) : 0;
                if (n > 0) {
                    j += n - 1; // the loop tail adds the final +1
                    i = k;      // consume through the closing '}'
                } else {
                    data[j] = s.data[i];
                }
            } break;
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

i32 ns_str_append_i32(ns_str *s, i32 n) {
    // append i32 to string
    ns_bool neg = 0;

    if (n < 0) {
        neg = 1;
        n = -n;
    }

    i32 size = 0;
    i32 m = n;
    while (m) {
        m /= 10;
        size++;
    }

    if (neg) {
        ns_array_push(s->data, '-');
    }

    if (n == 0) {
        ns_array_push(s->data, '0');
        s->len++;
    } else {
        i32 j = size;
        i32 k = s->len;
        while (n) {
            i32 d = n % 10;
            n /= 10;
            ns_array_push(s->data, d + '0');
        }

        i32 l = k + j;
        while (k < l) {
            i8 t = s->data[k];
            s->data[k] = s->data[l - 1];
            s->data[l - 1] = t;
            k++;
            l--;
        }
        s->len += size + neg;
    }

    return s->len;
}

i32 ns_str_append_f64(ns_str *s, f64 n, i32 precision) {
    // append f64 to string
    ns_str_append_i32(s, (i32)n);
    ns_array_push(s->data, '.');

    f64 f = n - (i32)n;
    i32 i = 0;
    while (i < precision) {
        f *= 10;
        i32 d = (i32)f;
        ns_array_push(s->data, d + '0');
        f -= d;
        i++;
    }
    s->len += precision + 1;
    return s->len;
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
    case NS_ERR_ASSERTION: return ns_str_cstr("assertion error");
    default: return ns_str_cstr("unknown error");
    }
}
