#include "ns_fmt.h"

#include <string.h>
#include <math.h>

ns_str ns_fmt_value(ns_value n) {
    switch (n.type) {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32: {
        char* buff = malloc(12);
        sprintf(buff, "%d", (i32)n.i);
        return ns_str_cstr(buff);
    }
    case NS_TYPE_I64: {
        char* buff = malloc(22);
        sprintf(buff, "%ld", n.i);
        return ns_str_cstr(buff);
    }
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: {
        char* buff = malloc(12);
        sprintf(buff, "%u", (u32)n.i);
        return ns_str_cstr(buff);
    }
    case NS_TYPE_U64: {
        char* buff = malloc(22);
        sprintf(buff, "%lu", (u64)n.i);
        return ns_str_cstr(buff);
    }
    case NS_TYPE_F32:
    case NS_TYPE_F64: {
        char* buff = malloc(n.type == NS_TYPE_F32 ? 12 : 22);
        f64 f = fabs(n.f);
        if (f < 1e-3 || f > 1e6) {
            sprintf(buff, "%6.e", n.f);
        } else {
            sprintf(buff, "%6.3f", n.f);
        }
        return ns_str_cstr(buff);
    }
    case NS_TYPE_BOOL:
        return n.i == 0 ? ns_str_cstr("false") : ns_str_cstr("true");
    case NS_TYPE_STRING:
        assert(false); // not implemented
    default:
        return ns_str_cstr("nil");
    }
}

ns_str ns_fmt_eval(ns_vm *vm, ns_str fmt) {
    // parse fmt string in the form of "hello {a} {b} {c}" but not "\{a}"
    // replace {a} with value of a

    char *buff = malloc(fmt.len * 2);
    int i = 0;

    while (i < fmt.len) {
        if (fmt.data[i] == '{' && (i == 0 || (i > 0 && fmt.data[i - 1] != '\\'))) {
            i++;
            int start = i;
            while (i < fmt.len && fmt.data[i] != '}') i++;
            if (i == fmt.len) {
                ns_error("fmt error: missing '}'.");
                return ns_str_null;
            }
            ns_ast_ctx ctx = {0};
            ns_str expr = ns_str_slice(fmt, start, i);
            // add null terminator
            expr.data[expr.len] = '\0'; // dynamic string
            int i = ns_parse_expr_stack(&ctx);
            ns_value n = ns_eval_expr(vm, &ctx, i);
            ns_str value = ns_fmt_value(n);
        }
        i++;
    }
}