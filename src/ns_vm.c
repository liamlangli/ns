#include "ns_vm.h"
#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"

#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int ns_add_value(ns_vm_t *vm, ns_value v) {
    int i = ns_array_length(vm->values);
    v.index = i;
    ns_array_push(vm->values, v);
    return i;
}

void ns_vm_parse_fn_expr(ns_vm_t *vm, int i, ns_fn_t *fn) {
    ns_ast_t n = vm->ast[i];
    switch (n.type)
    {
    case NS_AST_VAR_DEF: {
        ns_str name = n.var_def.name.val;
        ns_hash_map_set(fn->locals, name, -(fn->localc++));
    } break;
    case NS_AST_BINARY_EXPR:
        ns_vm_parse_fn_expr(vm, n.binary_expr.left, fn);
        ns_vm_parse_fn_expr(vm, n.binary_expr.right, fn);
        break;
    case NS_AST_CALL_EXPR:
        ns_vm_parse_fn_expr(vm, n.call_expr.callee, fn);
        for (int i = 0; i < n.call_expr.argc; i++) {
            ns_vm_parse_fn_expr(vm, n.call_expr.args[i], fn);
        }
        break;
    case NS_AST_PRIMARY_EXPR:
        if (n.primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
            ns_str name = n.primary_expr.token.val;
            if (ns_hash_map_has(fn->args, name)) {
                vm->ast[i].primary_expr.slot = ns_hash_map_get(fn->args, name);
                break;
            }
            if (ns_hash_map_has(fn->locals, name)) {
                vm->ast[i].primary_expr.slot = -ns_hash_map_get(fn->locals, name);
                break;
            }
            if (ns_hash_map_has(vm->global_values, name)) {
                vm->ast[i].primary_expr.slot = 0;
                break;
            }
            fprintf(stderr, "eval error: unknown variable %*.s\n", name.len, name.data);
            assert(false);
        }
        break;
    case NS_AST_IF_STMT:
        ns_vm_parse_fn_expr(vm, n.if_stmt.condition, fn);
        ns_vm_parse_fn_expr(vm, n.if_stmt.body, fn);
        ns_vm_parse_fn_expr(vm, n.if_stmt.else_body, fn);
        break;
    case NS_AST_ITER_STMT:
        ns_vm_parse_fn_expr(vm, n.iter_stmt.condition, fn);
        ns_vm_parse_fn_expr(vm, n.iter_stmt.generator, fn);
        ns_vm_parse_fn_expr(vm, n.iter_stmt.body, fn);
        break;
    default:
        break;
    }
}

ns_fn_t* ns_vm_parse_fn(ns_vm_t *vm, ns_ast_t n) {
    ns_fn_t *fn = (ns_fn_t *)malloc(sizeof(ns_fn_t));
    fn->name = n.fn_def.name.val;
    fn->args = NULL;
    fn->locals = NULL;
    for (int i = 0; i < ns_array_length(n.fn_def.params); i++) {
        ns_str name = vm->ast[n.fn_def.params[i]].param.name.val;
        ns_hash_map_set(fn->args, name, i);
    }
    // TODO collect local variables
    ns_vm_parse_fn_expr(vm, n.fn_def.body, fn);
    return fn;
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
        if (vm->stack_depth > 0) {
            ns_str key = n.primary_expr.token.val;
            ns_call_scope *scope = &vm->call_stack[vm->call_stack_top];
            ns_fn_t *fn = vm->fns[scope->fn_index].value;

            // TODO handle ref value
        
            if (ns_hash_map_has(fn->locals, key)) {
                return scope->locals[ns_hash_map_get(fn->locals, key)];
            }

            if (ns_hash_map_has(fn->args, key)) {
                return scope->args[ns_hash_map_get(fn->args, key)];
            }

            if (ns_hash_map_has(vm->global_values, key)) {
                return vm->values[vm->global_values[ns_hash_map_get(vm->global_values, key)].value];
            }

            fprintf(stderr, "eval error: unknown variable %*.s\n", key.len, key.data);
            assert(false);
        }
        break;
    default:
        fprintf(stderr, "eval error: unknown literal type\n");
        assert(false);
        break;
    }
    return NS_NIL;
}

void ns_vm_parse_ast(ns_vm_t *vm, ns_parse_context_t *ctx) {
    // stage 1 save global variable & function name in to global context
    int num_sections = ns_array_length(ctx->sections);
    for (int i = 0; i < num_sections; ++i) {
        int s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_FN_DEF: {
            ns_str key = n.fn_def.name.val;
            ns_hash_map_set(vm->fns, key, NULL);
        } break;
        case NS_AST_VAR_DEF: {
            ns_str key = n.var_def.name.val;
            ns_hash_map_set(vm->global_values, key, -1);
        } break;
        default:
            break;
        }
    }

    // stage 2 parse function body
    for (int i = 0; i < num_sections; ++i) {
        int s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_FN_DEF: {
            ns_str key = n.fn_def.name.val;
            ns_fn_t *f = ns_vm_parse_fn(vm, n);
            ns_hash_map_set(vm->fns, key, f);
        } break;
        default:
            break;
        }
    }
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

    vm->values = NULL;
    vm->call_stack_top = -1;
    vm->fns = NULL;
    vm->structs = NULL;

    return vm;
}

bool ns_vm_push_call_scope(ns_vm_t *vm, ns_call_scope scope) {
    if (vm->call_stack_top >= NS_MAX_CALL_STACK) {
        fprintf(stderr, "eval error: call stack overflow\n");
        return false;
    }
    vm->call_stack[++vm->call_stack_top] = scope;
    return true;
}

bool ns_value_int_type(ns_value v) {
    return v.type == NS_TYPE_I32 || v.type == NS_TYPE_I64 ||  v.type == NS_TYPE_I16 || v.type == NS_TYPE_I8
        || v.type == NS_TYPE_U32 || v.type == NS_TYPE_U64 || v.type == NS_TYPE_U16 || v.type == NS_TYPE_U8;
}

bool ns_value_float_type(ns_value v) {
    return v.type == NS_TYPE_F32 || v.type == NS_TYPE_F64;
}

ns_value ns_call_scoped_fn(ns_vm_t *vm) {
    ns_call_scope scope = vm->call_stack[vm->call_stack_top];
    ns_ast_t fn = vm->ast[scope.fn_index];
    switch (fn.type)
    {
    case NS_AST_FN_DEF: {
        ns_value v;
        return v;
    } break;
    default:
        fprintf(stderr, "eval error: unknown fn type\n");
        assert(false);
        break;
    }
    return NS_NIL;
}

ns_value ns_eval_binary_op(ns_vm_t *vm, ns_value left, ns_value right, int i) {
    ns_ast_t n = vm->ast[i];
    switch (n.binary_expr.op.type)
    {
    case NS_TOKEN_ADDITIVE_OPERATOR:
        if (ns_str_equals_STR(n.binary_expr.op.val, "+")) {
            if (ns_value_int_type(left) && ns_value_int_type(right)) {
                return (ns_value){.type = NS_TYPE_I64, .u.int64 = left.u.int64 + right.u.int64};
            } else if (ns_value_float_type(left) && ns_value_float_type(right)) {
                return (ns_value){.type = NS_TYPE_F64, .u.float64 = left.u.float64 + right.u.float64};
            } else {
                fprintf(stderr, "eval error: unknown value type\n");
                assert(false);
            }
        }
        /* code */
        break;
    default:
        fprintf(stderr, "eval error: unknown binary op type\n");
        assert(false);
    }
    return NS_NIL;
}

ns_value ns_call_builtin_fn(ns_vm_t *vm, ns_str name, int i) {
    ns_ast_t n = vm->ast[i];
    if (ns_str_equals_STR(name, "print")) {
        for (int i = 0; i < n.call_expr.argc; i++) {
            ns_value v = ns_eval_expr(vm, n.call_expr.args[i]);
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
        return NS_NIL;
    } else {
        fprintf(stderr, "eval error: unknown builtin function %*.s\n", name.len, name.data);
        assert(false);
    }
}

ns_value ns_call_fn(ns_vm_t *vm, int i) {
    ns_ast_t n = vm->ast[i];
    ns_ast_t callee = vm->ast[n.call_expr.callee];
    if (callee.type == NS_AST_PRIMARY_EXPR && callee.primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        ns_str name = callee.primary_expr.token.val;
        if (!ns_hash_map_has(vm->fns, callee.primary_expr.token.val)) {
            return ns_call_builtin_fn(vm, name, i);
        } else {
            ns_fn_t *fn = ns_hash_map_get(vm->fns, name);
            ns_call_scope scope = {0};
            for (int i = 0; i < n.call_expr.argc; i++) {
                scope.args[i] = ns_eval_expr(vm, n.call_expr.args[i]);
            }
            scope.fn_index = fn->index;
            ns_vm_push_call_scope(vm, scope);
            return ns_call_scoped_fn(vm);
        }
    }
    return NS_NIL;
}

ns_value ns_eval_expr(ns_vm_t *vm, int i) {
    ns_ast_t n = vm->ast[i];
    if (n.type == NS_AST_PRIMARY_EXPR) {
        return ns_parse_literal(vm, n);
    }

    switch (n.type)
    {
    case NS_AST_BINARY_EXPR: {
        ns_value left = ns_eval_expr(vm, n.binary_expr.left);
        ns_value right = ns_eval_expr(vm, n.binary_expr.right);
        return ns_eval_binary_op(vm, left, right, i);
    }
    case NS_AST_CALL_EXPR: {
        return ns_call_fn(vm, i);
    }
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
    ns_vm_parse_ast(vm, ctx);

    ns_value ret = NS_NIL;
    int i = 0;
    int l = ns_array_length(ctx->sections);

    // eval global values
    while (i < l) {
        int s = ctx->sections[i++];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_FN_DEF: {
            ns_str key = n.fn_def.name.val;
            ns_fn_t *f = ns_vm_parse_fn(vm, n);
            ns_hash_map_set(vm->fns, key, f);
        } break;
        case NS_AST_VAR_DEF: {
            ns_value v = ns_eval_expr(vm, n.var_def.expr);
            ns_str key = n.var_def.name.val;
            int i = ns_add_value(vm, v);
            ns_hash_map_set(vm->global_values, key, i);
        } break;
        case NS_AST_CALL_EXPR: {
            ret = ns_call_fn(vm, i);
        } break;
        default:
            fprintf(stderr, "eval error: unknown section type\n");
            assert(false);
            break;
        }
    }

    return ret;
}
