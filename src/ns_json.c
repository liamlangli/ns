#include "ns_json.h"

static ns_json *_ns_json_stack = ns_null;
static ns_str _ns_json_str = ns_str_null;

i32 ns_json_parse_value(ns_json_ctx *ctx);

ns_json_ref ns_json_make(ns_json_type type) {
    ns_json_ref i = ns_array_length(_ns_json_stack);
    ns_array_push(_ns_json_stack, (ns_json){.type = type});
    ns_json *json = ns_array_last(_ns_json_stack);
    json->next = json->prop = 0;
    return i;
}

ns_json* ns_json_get(ns_json_ref i) {
    return &_ns_json_stack[i];
}

ns_json_ref ns_json_make_null() {
    return ns_json_make(NS_JSON_NULL);
}

ns_json_ref ns_json_make_bool(ns_bool b) {
    return ns_json_make(b ? NS_JSON_TRUE : NS_JSON_FALSE);
}

ns_json_ref ns_json_make_number(f64 n) {
    i32 i = ns_json_make(NS_JSON_NUMBER);
    _ns_json_stack[i].n = n;
    return i;
}

ns_json_ref ns_json_make_string(ns_str s) {
    i32 i = ns_json_make(NS_JSON_STRING);
    _ns_json_stack[i].str = s;
    return i;
}

ns_json_ref ns_json_make_array() {
    return ns_json_make(NS_JSON_ARRAY);
}

ns_json_ref ns_json_make_object() {
    return ns_json_make(NS_JSON_OBJECT);
}

f64 ns_json_to_number(ns_json_ref i) {
    ns_json *j = &_ns_json_stack[i];
    if (j->type == NS_JSON_NUMBER) {
        return j->n;
    }
    return 0;
}

i32 ns_json_to_i32(ns_json_ref i) {
    return (i32)ns_json_to_number(i);
}

ns_str ns_json_to_string(ns_json_ref i) {
    ns_json *j = &_ns_json_stack[i];
    if (j->type == NS_JSON_STRING) {
        return j->str;
    }
    return ns_str_null;
}

ns_json_ref ns_json_get_prop(ns_json_ref i, ns_str key) {
    ns_json *j = &_ns_json_stack[i];
    ns_json_ref c = j->prop;
    while (c) {
        ns_json *child = ns_json_get(c);
        if (ns_str_equals(child->key, key)) {
            return c;
        }
        c = child->next;
    }
    return 0;
}

ns_json_ref ns_json_get_item(ns_json_ref i, i32 index) {
    ns_json *j = &_ns_json_stack[i];
    ns_json_ref c = j->prop;
    i32 count = 0;
    while (c) {
        if (count == index) {
            return c;
        }
        count++;
        c = ns_json_get(c)->next;
    }
    return 0;
}

ns_bool ns_json_number_is_int(f64 n) {
    return n == (i32)n;
}

ns_json *ns_json_top() {
    return ns_array_length(_ns_json_stack) > 0 ? ns_array_last(_ns_json_stack) : ns_null;
}

i32 ns_json_skip_whitespace(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    while (i < s.len && (s.data[i] == ' ' || s.data[i] == '\t' || s.data[i] == '\n' || s.data[i] == '\r')) {
        i++;
    }
    ctx->i = i;
    return i;
}

i32 ns_json_skip_whitespace_comma(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    while (i < s.len && (s.data[i] == ' ' || s.data[i] == '\t' || s.data[i] == '\n' || s.data[i] == '\r' || s.data[i] == ',')) {
        i++;
    }
    ctx->i = i;
    return i;
}

i32 ns_json_parse_string(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    if (s.data[i] == '"') {
        i++;
        i32 start = i;
        while (i < s.len) {
            if (s.data[i] == '"' && s.data[i - 1] != '\\') {
                break;
            }
            i++;
        }
        ns_str str = ns_str_range(s.data + start, i - start);
        i++;
        ctx->i = i;
        return ns_json_make_string(str);
    }
    return 0;
}

i32 ns_json_parse_true(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    if (strncmp(s.data + i, "true", 4) == 0) {
        i += 4;
        ctx->i = i;
        return ns_json_make_bool(true);
    }
    return 0;
}

i32 ns_json_parse_false(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    if (strncmp(s.data + i, "false", 5) == 0) {
        i += 5;
        ctx->i = i;
        return ns_json_make_bool(false);
    }
    return 0;
}

i32 ns_json_parse_null(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    if (strncmp(s.data + i, "null", 4) == 0) {
        i += 4;
        ctx->i = i;
        return ns_json_make_null();
    }
    return 0;
}

i32 ns_json_parse_number(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    i32 start = i;
    while (i < s.len && (s.data[i] == '-' || s.data[i] == '.' || (s.data[i] >= '0' && s.data[i] <= '9'))) {
        i++;
    }
    f64 n = ns_str_to_f64(ns_str_range(s.data + start, i - start));
    ctx->i = i;
    return ns_json_make_number(n);
}

void ns_json_push(ns_json_ref j, ns_json_ref c) {
    ns_json *json = &_ns_json_stack[j];
    if (json->prop == 0) {
        json->prop = c;
        return;
    }

    ns_json *prop = ns_json_get(json->prop);
    while (prop->next) {
        prop = &_ns_json_stack[prop->next];
    }
    prop->next = c;
}

void ns_json_set(ns_json_ref j, ns_str key, ns_json_ref c) {
    ns_json *json = &_ns_json_stack[j];
    if (json->prop == 0) {
        json->prop = c;
        ns_json_get(c)->key = key;
        return;
    }

    ns_json *prop = ns_json_get(json->prop);
    while (prop->next) {
        prop = &_ns_json_stack[prop->next];
    }
    ns_json_get(c)->key = key;
    prop->next = c;
}

ns_str ns_json_parse_key(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    i32 i = ctx->i;
    while (i < s.len && (s.data[i] == ' ' || s.data[i] == '\t' || s.data[i] == '\n' || s.data[i] == '\r')) {
        i++;
    }
    ctx->i = i;

    if (s.data[i] == '"') {
        i++;
        i32 start = i;
        while (i < s.len && s.data[i] != '"') {
            i++;
        }
        ns_str key = ns_str_range(s.data + start, i - start);
        i++;
        ctx->i = i;
        return key;
    }
    return ns_str_null;
}

ns_json_ref ns_json_parse_object(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    if (s.data[ctx->i] == '{') {
        ctx->i++;
        ns_json_ref j = ns_json_make_object();
        while (ctx->i < s.len && s.data[ctx->i] != '}') {
            ns_str key = ns_json_parse_key(ctx);
            if (key.len == 0) {
                ns_error("ns_json", "invalid object key\n");
            }
            // colon
            ns_json_skip_whitespace(ctx);
            if (s.data[ctx->i] != ':') {
                ns_error("ns_json", "expect colon\n");
            }
            ctx->i++;
            ns_json_ref v = ns_json_parse_value(ctx);
            if (v == 0) {
                ns_error("ns_json", "invalid object value\n");
            }

            ns_json_set(j, key, v);
            ns_json_skip_whitespace_comma(ctx);
        }
        ctx->i++;
        return j;
    }
    return 0;
}

ns_json_ref ns_json_parse_array(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    if (s.data[ctx->i] == '[') {
        ctx->i++;
        i32 j = ns_json_make_array();
        while (ctx->i < s.len && s.data[ctx->i] != ']') {
            i32 v = ns_json_parse_value(ctx);
            if (v == 0) {
                ns_error("ns_json", "invalid array value\n");
            }
            ns_json_push(j, v);
            ns_json_skip_whitespace_comma(ctx);
        }
        ctx->i++;
        return j;
    }
    return 0;
}

i32 ns_json_parse_value(ns_json_ctx *ctx) {
    // skip white space
    ns_str s = ctx->s;
    ns_json_skip_whitespace(ctx);
    i8 c = s.data[ctx->i];
    switch (c)
    {
    case '{':
        return ns_json_parse_object(ctx);
    case '[':
        return ns_json_parse_array(ctx);
    case '"':
        return ns_json_parse_string(ctx);
    case 't':
        return ns_json_parse_true(ctx);
    case 'f':
        return ns_json_parse_false(ctx);
    case 'n':
        return ns_json_parse_null(ctx);
    case '-':
    case '.':
    case '0' ... '9':
        return ns_json_parse_number(ctx);
    default:
        ns_error("ns_json", "invalid json value\n");
        break;
    }
}

ns_json_ref ns_json_parse(ns_str s) {
    ns_array_set_length(_ns_json_stack, 0);
    ns_array_push(_ns_json_stack, (ns_json){.type = NS_JSON_INVALID});
    return ns_json_parse_value(&(ns_json_ctx){.s = s});
}

#define NS_JSON_PAD 4
#define NS_JSON_FLT_PRECISION 8

#define ns_printf_wrap(fmt, ...) (printf("%*.s"fmt, (wrap ? depth * NS_JSON_PAD : 0), "", ##__VA_ARGS__))
#define ns_str_append_char(a, c) (ns_array_push((a).data, c), (a).len++)

ns_bool ns_json_print_node(ns_json *json, i32 depth,ns_bool wrap) {
    if (!json) return false;
    switch (json->type)
    {
    case NS_JSON_FALSE: ns_printf_wrap("false"); break;
    case NS_JSON_TRUE: ns_printf_wrap("true"); break;
    case NS_JSON_NULL: ns_printf_wrap("null"); break;
    case NS_JSON_NUMBER: {
        if (ns_json_number_is_int(json->n)) {
            ns_printf_wrap("%d", (i32)json->n);
        } else {
            ns_printf_wrap("%.2f", json->n);
        }
    } break;
    case NS_JSON_STRING: ns_printf_wrap("\"%.*s\"", json->str.len, json->str.data); break;
    case NS_JSON_ARRAY: {
        ns_printf_wrap("[");
        i32 c = json->prop;
        while (c) {
            ns_json *child = ns_json_get(c);
            ns_json_print_node(child, depth + 1, wrap);
            c = child->next;
            if (c == 0) break; else printf(",");
        }
        ns_printf_wrap("]");
    } break;
    case NS_JSON_OBJECT: {
        ns_printf_wrap("{");
        i32 c = json->prop;
        while (c) {
            ns_json *child = ns_json_get(c);
            ns_printf_wrap("\"%.*s\":", child->key.len, child->key.data);
            ns_json_print_node(child, depth + 1, false);
            c = child->next;
            if (c == 0) break; else printf(",");
        }
        ns_printf_wrap("}");
    } break;
    case NS_JSON_RAW:
        ns_info("ns_json", "raw\n");
        break;
    default:
        ns_error("ns_json", "invalid json type\n");
        break;
    }
    return true;
}

ns_bool ns_json_print(ns_json *json) {
    return ns_json_print_node(json, 0, false);
}

void ns_str_append_i32(ns_str *s, i32 n) {
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
}

void ns_str_append_f64(ns_str *s, f64 n, i32 precision) {
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
}

ns_str ns_json_stringify_append(ns_json *json) {
    switch (json->type)
    {
    case NS_JSON_FALSE: ns_str_append(&_ns_json_str, ns_str_cstr("false")); break;
    case NS_JSON_TRUE: ns_str_append(&_ns_json_str, ns_str_cstr("true")); break;
    case NS_JSON_NULL: ns_str_append(&_ns_json_str, ns_str_cstr("null")); break;
    case NS_JSON_NUMBER: {
        if (ns_json_number_is_int(json->n)) {
            ns_str_append_i32(&_ns_json_str, (i32)json->n);
        } else {
            ns_str_append_f64(&_ns_json_str, json->n, NS_JSON_FLT_PRECISION);
        }
    } break;
    case NS_JSON_STRING: {
        ns_str_append_char(_ns_json_str, '"');
        ns_str_append(&_ns_json_str, json->str);
        ns_str_append_char(_ns_json_str, '"');
    } break;
    case NS_JSON_ARRAY: {
        ns_str_append_char(_ns_json_str, '[');
        i32 c = json->prop;
        while (c) {
            ns_json *child = ns_json_get(c);
            ns_json_stringify_append(child);
            c = child->next;
            if (c == 0) break; else ns_str_append_char(_ns_json_str, ',');
        }
        ns_str_append_char(_ns_json_str, ']');
    } break;
    case NS_JSON_OBJECT: {
        ns_str_append_char(_ns_json_str, '{');
        i32 c = json->prop;
        while (c) {
            ns_json *child = ns_json_get(c);
            ns_str_append_char(_ns_json_str, '"');
            ns_str_append(&_ns_json_str, child->key);
            ns_str_append_char(_ns_json_str, '"');
            ns_str_append_char(_ns_json_str, ':');
            ns_json_stringify_append(child);
            c = child->next;
            if (c == 0) break; else ns_str_append_char(_ns_json_str, ',');
        }
        ns_str_append_char(_ns_json_str, '}');
    } break;
    default:
        break;
    }
    return _ns_json_str;
}

ns_str ns_json_stringify(ns_json *json) {
    ns_array_set_length(_ns_json_str.data, 0);
    _ns_json_str.len = 0;
    return ns_json_stringify_append(json);
}
