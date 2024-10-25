#include "ns_fmt.h"

#include <string.h>
#include <math.h>

#define ns_fmt_pattern_i32 "%d"
#define ns_fmt_pattern_i64 "%ld"
#define ns_fmt_pattern_u32 "%u"
#define ns_fmt_pattern_u64 "%lu"
#define ns_fmt_pattern_f32 "%.2f"
#define ns_fmt_pattern_f64 "%.2lf"

ns_str ns_fmt_value(ns_vm *vm, ns_value n) {
    switch (n.type.type) {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32: {
        i32 s = snprintf(ns_null, 0, ns_fmt_pattern_i32, (i32)n.i);
        char* d = malloc(s + 1);
        snprintf(d, s + 1, ns_fmt_pattern_i32, (i32)n.i);
        return (ns_str){.data=d, .len=s, .dynamic=1};
    }
    case NS_TYPE_I64: {
        i32 s = snprintf(ns_null, 0, ns_fmt_pattern_i64, n.i);
        char* d = malloc(s + 1);
        snprintf(d, s + 1, ns_fmt_pattern_i64, n.i);
        return (ns_str){.data=d, .len=s, .dynamic=1};
    }
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: {
        i32 s = snprintf(ns_null, 0, ns_fmt_pattern_u32, (u32)n.i);
        char* d = malloc(s + 1);
        snprintf(d, s + 1, ns_fmt_pattern_u32, (u32)n.i);
        return (ns_str){.data=d, .len=s, .dynamic=1};
    }
    case NS_TYPE_U64: {
        i32 s = snprintf(ns_null, 0, ns_fmt_pattern_u64, (u64)n.i);
        char* d = malloc(s + 1);
        snprintf(d, s + 1, ns_fmt_pattern_u64, (u64)n.i);
        return (ns_str){.data=d, .len=s, .dynamic=1};
    }
    case NS_TYPE_F32: {
        i32 s = snprintf(ns_null, 0, ns_fmt_pattern_f32, (f32)n.i);
        char* d = malloc(s + 1);
        snprintf(d, s + 1, ns_fmt_pattern_f32, (f32)n.i);
        return (ns_str){.data=d, .len=s, .dynamic=1};
    }
    case NS_TYPE_F64: {
        i32 s = snprintf(ns_null, 0, ns_fmt_pattern_f64, n.f);
        char* d = malloc(s + 1);
        snprintf(d, s + 1, ns_fmt_pattern_f64, n.f);
        return (ns_str){.data=d, .len=s, .dynamic=1};
    }
    case NS_TYPE_BOOL:
        return n.i == 0 ? ns_str_false : ns_str_true;
    case NS_TYPE_STRING: {
        ns_str s = vm->str_list[n.i];
        char* d = malloc(s.len + 1);
        memcpy(d, s.data, s.len);
        d[s.len] = '\0';
        return (ns_str){.data = d, .len = s.len, .dynamic = 1};
    } break;
    default:
        return ns_str_cstr("nil");
    }
}

ns_str ns_fmt_type_str(ns_type t) {
    switch (t.type)
    {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32:
        return ns_str_cstr("%d");
    case NS_TYPE_I64:
        return ns_str_cstr("%ld");
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32:
        return ns_str_cstr("%u");
    case NS_TYPE_U64:
        return ns_str_cstr("%lu");
    case NS_TYPE_F32:
        return ns_str_cstr("%.2f");
    case NS_TYPE_F64:
        return ns_str_cstr("%.2lf");
    default:
        ns_error("fmt error", "");
        break;
    }
    return ns_str_null;
}

ns_str ns_fmt_eval(ns_vm *vm, ns_str fmt) {
    ns_str ret = {.data = ns_null, .len = 0, .dynamic = 1};
    ns_array_set_capacity(ret.data, fmt.len);

    i32 i = 0;
    while (i < fmt.len) {
        if (fmt.data[i] == '{' && (i == 0 || (i > 0 && fmt.data[i - 1] != '\\'))) {
            i32 start = ++i;
            while (i < fmt.len && fmt.data[i] != '}') {
                i++;
            }
            if (i == fmt.len) {
                ns_error("fmt error", "missing '}'.");
                return ns_str_null;
            }
            ns_ast_ctx ctx = {0};
            ns_str expr = ns_str_slice(fmt, start, i++);
            ctx.source = expr;
            ctx.filename = ns_str_cstr("fmt");

            ctx.top = -1;
            ctx.token.line = 1; // start from 1
            ctx.current = -1;

            ns_parse_expr_stack(&ctx);
            ns_value v = ns_eval_expr(vm, &ctx, ctx.nodes[ctx.current]);
            ns_str s = ns_fmt_value(vm, v);
            ns_str_append(&ret, s);
            ns_str_free(s);
        } else {
            ns_array_push(ret.data, fmt.data[i++]);
        }
    }
    ret.len = ns_array_length(ret.data);
    return ns_str_unescape(ret);
}