#include "ns_vm.h"
#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

ns_value ns_eval_expr(ns_vm_t *vm, int i);

int ns_add_value(ns_vm_t *vm, ns_value v) {
    v.index = vm->value_count;
    vm->values[vm->value_count++] = v;
    return v.index;
}

int ns_vm_find_fn(ns_vm_t *vm, ns_str name) {
    for (int i = 0, l = vm->fn_count; i < l; i++) {
        if (ns_str_equals(vm->fns[i].name, name)) {
            return i;
        }
    }
    return -1;
}

int ns_vm_find_scope_arg_slot(ns_vm_t *vm, ns_str name) {
    ns_call_scope *scope = &vm->call_stack[vm->call_stack_top];
    ns_fn_t *fn = &vm->fns[scope->fn_index];
    for (int i = 0; i < fn->arg_count; i++) {
        if (ns_str_equals(fn->arg_names[i], name)) {
            return i;
        }
    }
    return -1;
}

int ns_vm_find_scope_local_slot(ns_vm_t *vm, ns_str name) {
    ns_call_scope *scope = &vm->call_stack[vm->call_stack_top];
    ns_fn_t *fn = &vm->fns[scope->fn_index];
    for (int i = 0; i < fn->local_count; i++) {
        if (ns_str_equals(fn->local_names[i], name)) {
            return i;
        }
    }
    return -1;
}

int ns_vm_find_global_value_slot(ns_vm_t *vm, ns_str name) {
    for (int i = 0, l = vm->global_count; i < l; i++) {
        if (ns_str_equals(vm->global_names[i], name)) {
            return i;
        }
    }
    return -1;
}

int ns_vm_find_fn_arg_slot(ns_vm_t *vm, ns_fn_t *fn, ns_str name) {
    for (int i = 0; i < fn->arg_count; i++) {
        if (ns_str_equals(fn->arg_names[i], name)) {
            return i;
        }
    }
    return -1;
}

int ns_vm_find_fn_local_slot(ns_vm_t *vm, ns_fn_t *fn, ns_str name) {
    for (int i = 0; i < fn->local_count; i++) {
        if (ns_str_equals(fn->local_names[i], name)) {
            return i;
        }
    }
    return -1;
}

void ns_vm_parse_fn_expr(ns_vm_t *vm, int i, ns_fn_t *fn) {
    if (i == -1) return;
    ns_ast_t n = vm->ast[i];
    switch (n.type)
    {
    case NS_AST_VAR_DEF: {
        ns_str name = n.var_def.name.val;
        int slot = ns_vm_find_fn_local_slot(vm, fn, name);
        if (slot != -1) {
            fprintf(stderr, "eval error: duplicate variable %*.s\n", name.len, name.data);
            assert(false);
        } else {
            slot = fn->local_count++;
            fn->local_names[slot] = name;
        }
    } break;
    case NS_AST_BINARY_EXPR:
        ns_vm_parse_fn_expr(vm, n.binary_expr.left, fn);
        ns_vm_parse_fn_expr(vm, n.binary_expr.right, fn);
        break;
    case NS_AST_CALL_EXPR:
        ns_vm_parse_fn_expr(vm, n.call_expr.callee, fn);
        for (int i = 0; i < n.call_expr.arg_count; i++) {
            ns_vm_parse_fn_expr(vm, n.call_expr.args[i], fn);
        }
        break;
    case NS_AST_PRIMARY_EXPR:
        if (n.primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
            ns_str name = n.primary_expr.token.val;
            int slot = ns_vm_find_fn_arg_slot(vm, fn, name);
            if (slot != -1) {
                vm->ast[i].primary_expr.slot = slot + 1;
                break;
            }

            slot = ns_vm_find_fn_local_slot(vm, fn, name);
            if (slot != -1) {
                vm->ast[i].primary_expr.slot = -1 -slot;
                break;
            }

            slot = ns_vm_find_global_value_slot(vm, name);
            if (slot != -1) {
                vm->ast[i].primary_expr.slot = 0;
                break;
            }
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
    case NS_AST_JUMP_STMT:
        ns_vm_parse_fn_expr(vm, n.jump_stmt.expr, fn);
        break;
    default:
        break;
    }
}

void ns_vm_parse_fn(ns_vm_t *vm, ns_ast_t n) {
    int fn_i = ns_vm_find_fn(vm, n.fn_def.name.val);
    if (fn_i == -1) {
        fprintf(stderr, "eval error: function %*.s not found\n", n.fn_def.name.val.len, n.fn_def.name.val.data);
        assert(false);
    }
    ns_fn_t *fn = &vm->fns[fn_i];

    for (int i = 0; i < n.fn_def.param_count; i++) {
        fn->arg_names[i] = vm->ast[n.fn_def.params[i]].param.name.val;
    }

    fn->arg_count = n.fn_def.param_count;
    ns_vm_parse_fn_expr(vm, n.fn_def.body, fn);
}

ns_value ns_eval_literal(ns_vm_t *vm, ns_ast_t n) {
    switch (n.primary_expr.token.type)
    {
    case NS_TOKEN_INT_LITERAL:
        return (ns_value){.type = NS_TYPE_I64, .u.int64 = ns_str_to_i32(n.primary_expr.token.val)};
    case NS_TOKEN_FLOAT_LITERAL:
        return (ns_value){.type = NS_TYPE_F64, .u.float64 = ns_str_to_f64(n.primary_expr.token.val)};
    case NS_TOKEN_STRING_LITERAL: break;
        // copy & save string
    case NS_TOKEN_TRUE:
        return NS_TRUE;
    case NS_TOKEN_FALSE:
        return NS_FALSE;
    case NS_TOKEN_NIL:
        return NS_NIL;
    case NS_TOKEN_IDENTIFIER:
        if (vm->stack_depth > -1) {
            ns_str key = n.primary_expr.token.val;
            ns_call_scope *scope = &vm->call_stack[vm->call_stack_top];
            ns_fn_t *fn = &vm->fns[scope->fn_index];
            if (n.primary_expr.slot == 0) {
                int slot = ns_vm_find_global_value_slot(vm, key);
                if (slot == -1) {
                    fprintf(stderr, "eval error: unknown variable %*.s\n", key.len, key.data);
                    assert(false);
                } else {
                    return vm->global_values[slot];
                }
            } else if (n.primary_expr.slot > 0) {
                return scope->args[n.primary_expr.slot - 1];
            } else {
                return scope->locals[-n.primary_expr.slot - 1];
            }
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
    for (int i = 0; i < ctx->section_count; ++i) {
        int s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_FN_DEF: {
            ns_fn_t f = {.name = n.fn_def.name.val, .ast_root = n.fn_def.body};
            vm->fns[vm->fn_count++] = f;
        } break;
        case NS_AST_VAR_DEF: {
            ns_str key = n.var_def.name.val;
            int slot = ns_vm_find_global_value_slot(vm, key);
            if (slot != -1) {
                fprintf(stderr, "eval error: duplicate variable %*.s\n", key.len, key.data);
                assert(false);
            } else {
                slot = vm->global_count++;
                vm->global_names[slot] = key;
            }
        } break;
        default:
            break;
        }
    }

    // stage 2 parse function body
    for (int i = 0; i < ctx->section_count; ++i) {
        int s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_FN_DEF: {
            ns_str key = n.fn_def.name.val;
            ns_vm_parse_fn(vm, n);
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

int ns_vm_find_global_value(ns_vm_t *vm, ns_str name) {
    for (int i = 0, l = vm->global_count; i < l; i++) {
        if (ns_str_equals(vm->global_names[i], name)) {
            return i;
        }
    }
    return -1;
}

ns_vm_t *ns_create_vm() {
    ns_vm_t *vm = (ns_vm_t *)malloc(sizeof(ns_vm_t));
    vm->ast = NULL;
    vm->stack_depth = 0;
    vm->global_count = 0;
    vm->fn_count = 0;
    vm->value_count = 0;
    vm->call_stack_top = -1;
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

bool ns_value_conditional_true(ns_value v) {
    switch (v.type)
    {
    case NS_TYPE_BOOL:
        return v.u.boolean;
    case NS_TYPE_I32:
    case NS_TYPE_I64:
    case NS_TYPE_I16:
    case NS_TYPE_I8:
    case NS_TYPE_U32:
    case NS_TYPE_U64:
    case NS_TYPE_U16:
    case NS_TYPE_U8:
        return v.u.int64 != 0;
    case NS_TYPE_F32:
    case NS_TYPE_F64:
        return v.u.float64 != 0;
    default:
        fprintf(stderr, "eval error: unknown value type\n");
        assert(false);
        break;
    }
    return false;
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
        for (int i = 0; i < n.call_expr.arg_count; i++) {
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
        int fn_i = ns_vm_find_fn(vm, name);
        if (fn_i == -1) {
            return ns_call_builtin_fn(vm, name, i);
        } else {
            ns_fn_t *fn = &vm->fns[fn_i];
            ns_call_scope scope = {.argc = n.call_expr.arg_count};
            for (int i = 0; i < n.call_expr.arg_count; i++) {
                scope.args[i] = ns_eval_expr(vm, n.call_expr.args[i]);
            }
            scope.fn_index = fn_i;
            ns_vm_push_call_scope(vm, scope);
            ns_value ret = ns_eval_expr(vm, fn->ast_root);
            vm->call_stack_top--;
            return ret;
        }
    }
    return NS_NIL;
}

ns_value ns_eval_jump_stmt(ns_vm_t *vm, int i) {
    ns_ast_t n = vm->ast[i];
    if (ns_str_equals_STR(n.jump_stmt.label.val, "return")) {
        ns_value ret = ns_eval_expr(vm, n.jump_stmt.expr);
        ns_call_scope *scope = &vm->call_stack[vm->call_stack_top];
        scope->ret = ret;
        return ret;
    }
    return NS_NIL;
}

ns_value ns_eval_expr(ns_vm_t *vm, int i) {
    ns_ast_t n = vm->ast[i];
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
    case NS_AST_IF_STMT: {
        ns_value condition = ns_eval_expr(vm, n.if_stmt.condition);
        if (ns_value_conditional_true(condition)) {
            return ns_eval_expr(vm, n.if_stmt.body);
        } else {
            if (n.if_stmt.else_body != -1) {
                return ns_eval_expr(vm, n.if_stmt.else_body);
            }
        }
    } break;
    case NS_AST_CAST_EXPR:
        break;
    case NS_AST_MEMBER_EXPR:
        break;
    case NS_AST_PRIMARY_EXPR:
        return ns_eval_literal(vm, n);
    case NS_AST_GENERATOR_EXPR:
        break;
    case NS_AST_JUMP_STMT:
        return ns_eval_jump_stmt(vm, i);
    default:
        fprintf(stderr, "eval error: unknown ast type %s\n", ns_ast_type_str(n.type));
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

    int i = 0;
    int l = ctx->section_count;
    ns_value ret = NS_NIL;

    // eval global values
    while (i < l) {
        int s = ctx->sections[i++];
        ns_ast_t n = ctx->nodes[s];
        switch (n.type)
        {
        case NS_AST_VAR_DEF: {
            ns_value v = ns_eval_expr(vm, n.var_def.expr);
            ns_str key = n.var_def.name.val;
            int slot = ns_vm_find_global_value_slot(vm, key);
            if (slot == -1) {
                fprintf(stderr, "eval error: unknown variable %*.s\n", key.len, key.data);
                assert(false);
            } else {
                slot = vm->global_count++;
                vm->global_values[slot] = v;
            }
        } break;
        case NS_AST_CALL_EXPR: {
            ret = ns_call_fn(vm, i);
        } break;

        case NS_AST_FN_DEF:
        case NS_AST_STRUCT_DEF:
        break;

        default:
            fprintf(stderr, "eval error: unknown section type\n");
            assert(false);
            break;
        }
    }

    return ret;
}
