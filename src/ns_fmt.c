#include "ns_fmt.h"

#define ns_fmt_print_number(_t_) \
{ \
    _t_ v = ns_eval_number_##_t_(vm, n); \
    ns_str pattern = ns_fmt_type_str(n.t); \
    i32 s = snprintf(ns_null, 0, pattern.data, v); \
    char* d = ns_malloc(s + 1); \
    snprintf(d, s + 1, pattern.data, v); \
    return (ns_str){.data=d, .len=s, .dynamic=1}; \
}

static u64 ns_fmt_enum_bits(ns_vm *vm, ns_value n) {
    ns_value v = ns_eval_enum_underlying_value(vm, n);
    switch (v.t.type) {
    case NS_TYPE_I8: return (u64)(i64)ns_eval_number_i8(vm, v);
    case NS_TYPE_U8: return ns_eval_number_u8(vm, v);
    case NS_TYPE_I16: return (u64)(i64)ns_eval_number_i16(vm, v);
    case NS_TYPE_U16: return ns_eval_number_u16(vm, v);
    case NS_TYPE_I32: return (u64)(i64)ns_eval_number_i32(vm, v);
    case NS_TYPE_U32: return ns_eval_number_u32(vm, v);
    case NS_TYPE_I64: return (u64)ns_eval_number_i64(vm, v);
    case NS_TYPE_U64: return ns_eval_number_u64(vm, v);
    default: return 0;
    }
}

ns_str ns_fmt_value(ns_vm *vm, ns_value n) {
    if (ns_type_is(n.t, NS_TYPE_ENUM)) {
        ns_symbol *en = &vm->symbols[ns_type_index(n.t)];
        u64 bits = ns_fmt_enum_bits(vm, n);
        for (i32 i = 0, l = (i32)ns_array_length(en->en.members); i < l; ++i) {
            if (en->en.members[i].value == bits) {
                ns_str member = en->en.members[i].name;
                i32 len = en->name.len + 1 + member.len;
                i8 *data = ns_malloc((szt)len + 1);
                snprintf(data, (szt)len + 1, "%.*s.%.*s", en->name.len, en->name.data,
                         member.len, member.data);
                return (ns_str){.data = data, .len = len, .dynamic = true};
            }
        }
        ns_str number = ns_fmt_value(vm, ns_eval_enum_underlying_value(vm, n));
        i32 len = en->name.len + number.len + 2;
        i8 *data = ns_malloc((szt)len + 1);
        snprintf(data, (szt)len + 1, "%.*s(%.*s)", en->name.len, en->name.data,
                 number.len, number.data);
        ns_str_free(number);
        return (ns_str){.data = data, .len = len, .dynamic = true};
    }
    switch (n.t.type) {
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
        return ns_eval_bool(vm, n) ? ns_str_true : ns_str_false;
    case NS_TYPE_STRING: {
        // Return a non-owning view: callers may ns_str_free the result, and the
        // underlying storage lives in vm->str_list and must not be released here.
        // dynamic MUST be 0 -- ns_str_range() would set it to 1, which makes the
        // caller's ns_str_free() release vm->str_list storage and corrupt the
        // interpolated string variable on its next use.
        ns_str s = ns_eval_str(vm, n);
        return (ns_str){.data = s.data, .len = s.len, .dynamic = 0};
    }
    case NS_TYPE_TASK:
        return ns_task_fmt(vm, n);
    default:
        return ns_str_cstr("nil");
    }
}

ns_str ns_fmt_type_str(ns_type t) {
    switch (t.type)
    {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_BOOL: return ns_str_cstr("%d");
    case NS_TYPE_I32: return ns_str_cstr("%d");
    case NS_TYPE_I64: return ns_str_cstr("%ld");
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: return ns_str_cstr("%u");
    case NS_TYPE_U64: return ns_str_cstr("%lu");
    case NS_TYPE_F32: return ns_str_cstr("%.2f");
    case NS_TYPE_F64: return ns_str_cstr("%.2lf");
    default:
        ns_error("fmt error", "unknown type.\n");
        break;
    }
    return ns_str_null;
}

ns_str ns_fmt_eval(ns_vm *vm, ns_str fmt) {
    ns_str ret = (ns_str){.data = ns_null, .len = 0, .dynamic = 1};
    ns_array_set_capacity(ret.data, fmt.len);

    i32 i = 0;
    while (i < fmt.len) {
        if (fmt.data[i] == '{' && (i == 0 || (i > 0 && fmt.data[i - 1] != '\\'))) {
            // Scan for the matching '}'. If there is none, or the braces span
            // multiple lines, the '{' is not an interpolation - emit it
            // literally. This keeps print() tolerant of already-formatted text
            // that legitimately contains brace characters (e.g. printed source
            // code such as ns_shader output, where `{` opens a code block).
            i32 scan = i + 1;
            ns_bool multiline = false;
            while (scan < fmt.len && fmt.data[scan] != '}') {
                if (fmt.data[scan] == '\n') multiline = true;
                scan++;
            }
            if (scan == fmt.len || multiline) {
                ns_array_push(ret.data, fmt.data[i++]);
                continue;
            }
            i32 start = ++i;
            i = scan;
            ns_ast_ctx ctx = {0};
            ns_str expr = ns_str_slice(fmt, start, i++);
            ctx.source = expr;
            ctx.filename = ns_str_cstr("fmt");

            ctx.token.line = 1; // start from 1
            ctx.current = 0;

            ns_parse_expr(&ctx);
            ns_return_value ret_v = ns_eval_expr(vm, &ctx, ctx.current);
            if (ns_return_is_error(ret_v)) {
                ns_error("fmt error", "failed to eval fmt expr.");
                return ns_str_null;
            }
            ns_value v = ret_v.r;
            ns_str s = ns_fmt_value(vm, v);
            ns_str_append(&ret, s);
            ns_str_free(s);
        } else {
            ns_array_push(ret.data, fmt.data[i++]);
        }
    }
    ret.len = ns_array_length(ret.data);
    // String values already carry decoded escape bytes (literals decode at
    // materialization), so emit them verbatim. Return a flat copy: callers
    // free with ns_str_free, which must not see the array backing.
    i8 *flat = (i8 *)ns_malloc((szt)ret.len + 1);
    if (ret.len > 0) memcpy(flat, ret.data, (szt)ret.len);
    flat[ret.len] = '\0';
    ns_str out = (ns_str){.data = flat, .len = ret.len, .dynamic = 1};
    ns_array_free(ret.data);
    return out;
}
