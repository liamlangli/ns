#include "ns_json.h"

static ns_json *_ns_json_stack = ns_null;

ns_str ns_json_parse_key(ns_json_ctx *ctx);
i32 ns_json_skip_whitespace(ns_json_ctx *ctx);

bool ns_json_set(i32 j, ns_str key, i32 c);
bool ns_json_push(i32 j, i32 c);

i32 ns_json_parse_object(ns_json_ctx *ctx);
i32 ns_json_parse_array(ns_json_ctx *ctx);
i32 ns_json_parse_value(ns_json_ctx *ctx);
i32 ns_json_parse_string(ns_json_ctx *ctx);
i32 ns_json_parse_true(ns_json_ctx *ctx);
i32 ns_json_parse_false(ns_json_ctx *ctx);
i32 ns_json_parse_null(ns_json_ctx *ctx);
i32 ns_json_parse_number(ns_json_ctx *ctx);

i32 ns_json_make(ns_json_type type) {
    i32 i = ns_array_length(_ns_json_stack);
    ns_array_push(_ns_json_stack, (ns_json){.type = type});
    ns_json *json = ns_array_last(_ns_json_stack);
    json->next_prop = json->next_item = 0;
    return i;
}

ns_json* ns_json_get(i32 i) {
    return &_ns_json_stack[i];
}

i32 ns_json_make_null() {
    return ns_json_make(NS_JSON_NULL);
}

i32 ns_json_make_bool(bool b) {
    return ns_json_make(b ? NS_JSON_TRUE : NS_JSON_FALSE);
}

i32 ns_json_make_number(f64 n) {
    i32 i = ns_json_make(NS_JSON_NUMBER);
    _ns_json_stack[i].n = n;
    return i;
}

i32 ns_json_make_string(ns_str s) {
    i32 i = ns_json_make(NS_JSON_STRING);
    _ns_json_stack[i].str = s;
    return i;
}

i32 ns_json_make_array() {
    return ns_json_make(NS_JSON_ARRAY);
}

i32 ns_json_make_object() {
    return ns_json_make(NS_JSON_OBJECT);
}

f64 ns_json_get_number(i32 i) {
    ns_json *j = &_ns_json_stack[i];
    if (j->type == NS_JSON_NUMBER) {
        return j->n;
    }
    return 0;
}

ns_str ns_json_get_string(i32 i) {
    ns_json *j = &_ns_json_stack[i];
    if (j->type == NS_JSON_STRING) {
        return j->str;
    }
    return ns_str_null;
}

ns_str ns_json_to_string(ns_json *json) {
    if (json) {
        switch (json->type)
        {
        case NS_JSON_INVALID:
            return ns_str_cstr("invalid");
        case NS_JSON_FALSE:
            return ns_str_cstr("false");
        case NS_JSON_TRUE:
            return ns_str_cstr("true");
        case NS_JSON_NULL:
            return ns_str_cstr("null");
        case NS_JSON_NUMBER:
            return ns_str_null;
        case NS_JSON_STRING:
            return ns_str_null;
        case NS_JSON_ARRAY:
            return ns_str_cstr("array");
        case NS_JSON_OBJECT:
            return ns_str_cstr("object");
        case NS_JSON_RAW:
            return ns_str_cstr("raw");
        default:
            break;
        }
    }

    return ns_str_null;
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

bool ns_json_push(i32 j, i32 c) {
    ns_json *json = &_ns_json_stack[j];
    while (json->next_item) {
        json = &_ns_json_stack[json->next_item];
    }
    json->next_item = c;
    return false;
}

bool ns_json_set(i32 j, ns_str key, i32 c) {
    ns_json *json = &_ns_json_stack[j];
    while (json->next_prop) {
        json = &_ns_json_stack[json->next_prop];
    }
    ns_json_get(c)->key = key;
    json->next_prop = c;
    return false;
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


i32 ns_json_parse_object(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    if (s.data[ctx->i] == '{') {
        ctx->i++;
        i32 j = ns_json_make_object();
        i32 count = 0;
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
            i32 v = ns_json_parse_value(ctx);
            if (v == 0) {
                ns_error("ns_json", "invalid object value\n");
            }

            ns_json_set(j, key, v);
            count++;
            ns_json_skip_whitespace_comma(ctx);
        }
        ctx->i++;
        ns_json_get(j)->count = count;
        return j;
    }
    return 0;
}

i32 ns_json_parse_array(ns_json_ctx *ctx) {
    ns_str s = ctx->s;
    if (s.data[ctx->i] == '[') {
        ctx->i++;
        i32 j = ns_json_make_array();
        i32 count = 0;
        while (ctx->i < s.len && s.data[ctx->i] != ']') {
            i32 v = ns_json_parse_value(ctx);
            if (v == 0) {
                ns_error("ns_json", "invalid array value\n");
            }
            ns_json_push(j, v);
            count++;
            ns_json_skip_whitespace_comma(ctx);
        }
        ctx->i++;
        ns_json_get(j)->count = count;
        return j;
    }
    return 0;
}

ns_json *ns_json_parse(ns_str s) {
    ns_array_set_length(_ns_json_stack, 0);
    ns_array_push(_ns_json_stack, (ns_json){.type = NS_JSON_INVALID});
    i32 root = ns_json_parse_value(&(ns_json_ctx){.s = s});
    return ns_json_get(root);
}

#define NS_JSON_PAD 4

bool ns_json_print(ns_json *json) {
    return ns_json_print_node(json, 0, false);
}

#define ns_printf_wrap(fmt, ...) (printf("%*.s"fmt, (wrap ? depth * NS_JSON_PAD : 0), "", ##__VA_ARGS__))

bool ns_json_print_node(ns_json *json, i32 depth, bool wrap) {
    if (!json) return false;
    switch (json->type)
    {
    case NS_JSON_FALSE: ns_printf_wrap("false"); break;
    case NS_JSON_TRUE: ns_printf_wrap("true"); break;
    case NS_JSON_NULL: ns_printf_wrap("null"); break;
    case NS_JSON_NUMBER: ns_printf_wrap("%.2f", json->n); break;
    case NS_JSON_STRING: ns_printf_wrap("\"%.*s\"", json->str.len, json->str.data); break;
    case NS_JSON_ARRAY: {
        ns_printf_wrap("[");
        i32 c = json->next_item;
        i32 count = json->count;
        while (c) {
            ns_json *child = ns_json_get(c);
            ns_json_print_node(child, depth + 1, wrap);
            c = child->next_item;
            if (--count == 0) break; else printf(",");
        }
        ns_printf_wrap("]");
    } break;
    case NS_JSON_OBJECT: {
        ns_printf_wrap("{");
        i32 count = json->count;
        i32 c = json->next_prop;
        while (c) {
            ns_json *child = ns_json_get(c);
            ns_printf_wrap("\"%.*s\":", child->key.len, child->key.data);
            ns_json_print_node(child, depth + 1, false);
            c = child->next_prop;
            if (--count == 0) { break;} else { printf(","); } 
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
