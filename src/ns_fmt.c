#include "ns_fmt.h"

#include <string.h>
#include <math.h>

#define ns_fmt_print_number(type) \
{ \
    type v = ns_eval_number_##type(vm, n); \
    ns_str pattern = ns_fmt_type_str(ns_type_mask(n.t)); \
    i32 s = snprintf(ns_null, 0, pattern.data, v); \
    char* d = malloc(s + 1); \
    snprintf(d, s + 1, pattern.data, v); \
    return (ns_str){.data=d, .len=s, .dynamic=1}; \
}

ns_str ns_fmt_value(ns_vm *vm, ns_value n) {
    switch (ns_type_mask(n.t)) {
    case NS_TYPE_I8: ns_fmt_print_number(i8)
    case NS_TYPE_U8: ns_fmt_print_number(u8)
    case NS_TYPE_I16: ns_fmt_print_number(i16)
    case NS_TYPE_U16: ns_fmt_print_number(u16)
    case NS_TYPE_I32: ns_fmt_print_number(i32)
    case NS_TYPE_U32: ns_fmt_print_number(u32)
    case NS_TYPE_I64: ns_fmt_print_number(i64)
    case NS_TYPE_U64: ns_fmt_print_number(u64)
    case NS_TYPE_F32: ns_fmt_print_number(f32)
    case NS_TYPE_F64: ns_fmt_print_number(f64)
    case NS_TYPE_BOOL:
        return ns_eval_bool(vm, n) ? ns_str_false : ns_str_true;
    case NS_TYPE_STRING: {
        if (ns_type_is_const(n.t)) {
            return vm->str_list[n.o];
        } else {
            i8* raw = ns_type_in_stack(n.t) ? vm->stack + n.o : (i8*)n.o;
            return ns_str_range(raw, strlen(raw));
        }
    } break;
    default:
        return ns_str_cstr("nil");
    }
}

ns_str ns_fmt_type_str(ns_type t) {
    switch (ns_type_mask(t))
    {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32: return ns_str_cstr("%d");
    case NS_TYPE_I64: return ns_str_cstr("%ld");
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: return ns_str_cstr("%u");
    case NS_TYPE_U64: return ns_str_cstr("%lu");
    case NS_TYPE_F32: return ns_str_cstr("%.2f");
    case NS_TYPE_F64: return ns_str_cstr("%.2lf");
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

            ctx.token.line = 1; // start from 1
            ctx.current = -1;

            ns_parse_expr(&ctx);
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