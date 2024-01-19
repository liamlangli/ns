#include "ns_vm.h"
#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"

#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

ns_value ns_vm_get_scoped_value(ns_vm_t *vm, ns_str key) {
    size_t i = ns_hash_map_get(vm->global_values, key);
    if (i != 0) {
        return vm->values[i];
    }
    return NS_NIL;
}

ns_value ns_parse_literal(ns_vm_t *vm, ns_ast_t n) {
    switch (n.primary_expr.token.type)
    {
    case NS_TOKEN_INT_LITERAL:
        return ns_new_i32(vm, ns_str_to_int(n.primary_expr.token.val));
    case NS_TOKEN_FLOAT_LITERAL:
        return ns_new_f64(vm, ns_str_to_f64(n.primary_expr.token.val));
    case NS_TOKEN_STRING_LITERAL: break;
        // copy & save string
    case NS_TOKEN_TRUE:
        return ns_new_bool(vm, true);
    case NS_TOKEN_FALSE:
        return ns_new_bool(vm, false);
    case NS_TOKEN_NIL:
        return NS_NIL;
    case NS_TOKEN_IDENTIFIER:
        break;
    default:
        fprintf(stderr, "eval error: unknown literal type\n");
        assert(false);
        break;
    }
    return NS_NIL;
}

int ns_add_value(ns_vm_t *vm, ns_value v) {
    int i = ns_array_len(vm->values);
    v.index = i;
    ns_array_push(vm->values, v);
    return i;
}

ns_value ns_new_bool(ns_vm_t *vm, bool value) {
    ns_value v = { .type = NS_TYPE_BOOL, .u.boolean = value };
    int i = ns_add_value(vm, v);
    return vm->values[i];
}

ns_value ns_new_i32(ns_vm_t *vm, i32 value) {
    ns_value v = { .type = NS_TYPE_I64, .u.int64 = value };
    int i = ns_add_value(vm, v);
    return vm->values[i];
}

ns_value ns_new_f64(ns_vm_t *vm, f64 value) {
    ns_value v = { .type = NS_TYPE_F64, .u.float64 = value };
    int i = ns_add_value(vm, v);
    return vm->values[i];
}

ns_vm_t *ns_create_vm() {
    ns_vm_t *vm = (ns_vm_t *)malloc(sizeof(ns_vm_t));
    vm->ast = NULL;
    vm->stack_depth = 0;
    vm->global_values = NULL;
    return vm;
}

ns_value ns_call(ns_vm_t *vm, ns_value fn, ns_value *args, int argc) {
    return NS_NIL;
}

ns_value ns_eval_expr(ns_vm_t *vm, int i) {
    ns_ast_t n = vm->ast[i];
    if (n.type == NS_AST_PRIMARY_EXPR) {
        return ns_parse_literal(vm, n);
    }

    switch (n.type)
    {
    case NS_AST_BINARY_EXPR:
        break;
    case NS_AST_CALL_EXPR:
        break;
    case NS_AST_CAST_EXPR:
        break;
    case NS_AST_MEMBER_EXPR:
        break;
    case NS_AST_PRIMARY_EXPR:
        break;
    case NS_AST_GENERATOR_EXPR:
        break;
    default:
        fprintf(stderr, "eval error: unknown ast type\n");
        assert(false);
        break;
    }
    return NS_NIL;
}

ns_value ns_eval(ns_vm_t *vm, const char* source, const char *filename) {
    ns_parse_context_t *ctx = ns_parse(source, filename);

    if (ctx == NULL) {
        fprintf(stderr, "eval error: ast parse failed\n");
        return NS_NIL;
    }

    vm->ast = ctx->nodes;

    int i = 0;
    int l = ns_array_len(ctx->sections);

    // eval global values
    while (i < l) {
        int s = ctx->sections[i++];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_FN_DEF: {
            ns_value fn = {.type = NS_TYPE_FN, .u.ptr = s};
            int i = ns_add_value(vm, fn);
            ns_str key = n.fn_def.name.val;
            ns_hash_map_set(vm->global_values, key, i);
        } break;
        case NS_AST_VAR_DEF: {
            ns_value v = ns_eval_expr(vm, n.var_def.expr);
            ns_str key = n.var_def.name.val;
            int i = ns_add_value(vm, v);
            ns_hash_map_set(vm->global_values, key, i);
        } break;
        case NS_AST_CALL_EXPR: {
            ns_ast_t callee = ctx->nodes[n.call_expr.callee];
            // global function call
            if (callee.type == NS_AST_PRIMARY_EXPR && callee.primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
                ns_str fn_name = callee.primary_expr.token.val;
                if (ns_str_equals_STR(fn_name, "print")) {
                    ns_value v = ns_eval_expr(vm, n.call_expr.args[0]);
                    switch (v.type)
                    {
                    case NS_TYPE_I64:
                        printf("%ld\n", v.u.int64);
                        break;
                    case NS_TYPE_I32:
                        printf("%d\n", (int)v.u.int64);
                        break;
                    case NS_TYPE_F64:
                        printf("%f\n", v.u.float64);
                        break;
                    default:
                        fprintf(stderr, "eval error: unknown value type\n");
                        assert(false);
                        break;
                    }
                }
            }
        } break;
        default:
            fprintf(stderr, "eval error: unknown section type\n");
            assert(false);
            break;
        }
    }

    return NS_NIL;
}