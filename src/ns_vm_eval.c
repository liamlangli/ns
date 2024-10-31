#include "ns_vm.h"

#include <math.h>

u64 ns_eval_alloc(ns_vm *vm, i32 stride);
ns_value ns_eval_alloc_value(ns_vm *vm, ns_value n);

void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
void ns_eval_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
void ns_eval_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

void ns_eval_enter_scope(ns_vm *vm, ns_call *call);
void ns_eval_exit_scope(ns_vm *vm, ns_call *call);

ns_value ns_eval_binary_override(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_token_t op);
ns_value ns_eval_binary_ops_number(ns_vm *vm, ns_value l, ns_value r, ns_token_t op);
ns_value ns_eval_binary_number_upgrade(ns_vm *vm, ns_value l, ns_value r, ns_token_t op);

ns_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n);
ns_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

u64 ns_eval_alloc(ns_vm *vm, i32 stride) {
    u64 offset = ns_array_length(vm->stack);
    i32 align = ns_min(ns_align, stride);
    if (align > 0) offset = (offset + (align - 1)) & ~(align - 1);
    ns_array_set_length(vm->stack, offset + stride);
    return offset; // leading 4 bytes for type size
}

ns_value ns_eval_alloc_value(ns_vm *vm, ns_value n) {
    i32 s = ns_type_size(vm, n.t);
    i32 offset = ns_eval_alloc(vm, s);
    i8 *dst = &vm->stack[offset];
    i8 *src = ns_eval_value_ptr(vm, n);
    memcpy(dst, dst, s);
    return (ns_value){.t = ns_type_set_store(n.t, NS_STORE_HEAP), .o = offset};
}

ns_value* ns_eval_find_value(ns_vm *vm, ns_str name) {
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call* call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        for (i32 i = 0, l = ns_array_length(call->fn->fn.args); i < l; ++i) {
            if (ns_str_equals(call->fn->fn.args[i].name, name)) {
                return &call->args[i];
            }
        }

        ns_scope* scope = &call->scopes[ns_array_length(call->scopes) - 1];
        for (i32 i = 0, l = ns_array_length(scope->vars); i < l; ++i) {
            if (ns_str_equals(scope->vars[i].name, name)) {
                return &scope->vars[i].val.val;
            }
        }
    }

    ns_symbol *r = ns_vm_find_symbol(vm, name);
    if (!r) return ns_null;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *r = &vm->symbols[i];
        if (ns_str_equals(r->name, name)) {
            switch (r->type)
            {
            case ns_symbol_value: return &r->val.val;
            case ns_symbol_fn: return &r->fn.fn;
            default:
                break;
            }
        }
    }

    return ns_null;
}

void ns_eval_enter_scope(ns_vm *vm, ns_call *call) {
    ns_scope scope = (ns_scope){.vars = ns_null, .stack_top = ns_array_length(vm->stack)};
    ns_array_push(call->scopes, scope);
}

void ns_eval_exit_scope(ns_vm *vm, ns_call *call) {
    ns_scope scope = ns_array_pop(call->scopes);
    ns_array_set_length(vm->stack, scope.stack_top);
}

ns_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value l = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.left]);
    ns_value r = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.right]);

    if (l.t != r.t && ns_type_is(l.t, NS_TYPE_INFER)) {
        ns_vm_error(ctx->filename, n.state, "eval error", "invalid assign expr.");
    }

    i32 s_l = ns_type_size(vm, l.t);
    i32 s_r = ns_type_size(vm, r.t);
    if (s_l != s_r) ns_vm_error(ctx->filename, n.state, "eval error", "type size mismatched.");

    if (ns_type_is_ref(r.t)) {
        return r;
    } else {

    }
}

ns_value ns_eval_binary_override(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_token_t op) {
    ns_str l_name = ns_vm_get_type_name(vm, l.t);
    ns_str r_name = ns_vm_get_type_name(vm, r.t);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
    if (!fn) ns_nil;
    ns_call call = (ns_call){.fn = fn, .args = ns_null, .scopes = ns_null};
    ns_array_set_length(call.args, 2);
    call.args[0] = l;
    call.args[1] = r;
    ns_eval_enter_scope(vm, &call);
    ns_array_push(vm->call_stack, call);
    ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.fn_def.body]);
    ns_eval_exit_scope(vm, &call);
    call = ns_array_pop(vm->call_stack);
    return call.ret;
}

#define ns_eval_value(type) type ns_eval_number_##type(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.##type : *(type*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
ns_eval_value(i8)
ns_eval_value(i16)
ns_eval_value(i32)
ns_eval_value(i64)
ns_eval_value(u8)
ns_eval_value(u16)
ns_eval_value(u32)
ns_eval_value(u64)
ns_eval_value(f32)
ns_eval_value(f64)
bool ns_eval_bool(ns_vm *vm, ns_value n) { return ns_eval_number_i32(vm, n) != 0; }

#define ns_eval_number_op_case(type, op) case ns_type_##type: ret.##type = ns_eval_number_##type(vm, l) op ns_eval_number_##type(vm, r); break;

#define ns_eval_number_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST) };\
    switch (ns_type_enum(l.t)) {                                            \
        ns_eval_number_op_case(i8, op)                                      \
        ns_eval_number_op_case(i16, op)                                     \
        ns_eval_number_op_case(i32, op)                                     \
        ns_eval_number_op_case(i64, op)                                     \
        ns_eval_number_op_case(u8, op)                                      \
        ns_eval_number_op_case(u16, op)                                     \
        ns_eval_number_op_case(u32, op)                                     \
        ns_eval_number_op_case(u64, op)                                     \
        ns_eval_number_op_case(f32, op)                                     \
        ns_eval_number_op_case(f64, op)                                     \
        default: break;                                                     \
    }                                                                       \
    return ret;                                                             \
}

#define ns_eval_number_cmp_op_case(type, op) case ns_type_##type: ret.b = ns_eval_number_##type(vm, l) op ns_eval_number_##type(vm, r); break;

#define ns_eval_number_cmp_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, NS_STORE_CONST) };\
    switch (ns_type_enum(l.t)) {                                            \
        ns_eval_number_cmp_op_case(i8, op)                                      \
        ns_eval_number_cmp_op_case(i16, op)                                     \
        ns_eval_number_cmp_op_case(i32, op)                                     \
        ns_eval_number_cmp_op_case(i64, op)                                     \
        ns_eval_number_cmp_op_case(u8, op)                                      \
        ns_eval_number_cmp_op_case(u16, op)                                     \
        ns_eval_number_cmp_op_case(u32, op)                                     \
        ns_eval_number_cmp_op_case(u64, op)                                     \
        ns_eval_number_cmp_op_case(f32, op)                                     \
        ns_eval_number_cmp_op_case(f64, op)                                     \
        default: break;                                                     \
    }                                                                       \
    return ret;                                                             \
}

#define ns_eval_number_shift_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST) };\
    switch (ns_type_enum(l.t)) {                                            \
        ns_eval_number_op_case(i8, op)                                      \
        ns_eval_number_op_case(i16, op)                                     \
        ns_eval_number_op_case(i32, op)                                     \
        ns_eval_number_op_case(i64, op)                                     \
        ns_eval_number_op_case(u8, op)                                      \
        ns_eval_number_op_case(u16, op)                                     \
        ns_eval_number_op_case(u32, op)                                     \
        ns_eval_number_op_case(u64, op)                                     \
        default: break;                                                     \
    }                                                                       \
    return ret;                                                             \
}

ns_eval_number_op(_add, +)
ns_eval_number_op(_sub, -)
ns_eval_number_op(_mul, *)
ns_eval_number_op(_div, /)
ns_eval_number_op(_and, &&)
ns_eval_number_op(_or, ||)
ns_eval_number_cmp_op(_eq, ==)
ns_eval_number_cmp_op(_ne, !=)
ns_eval_number_cmp_op(_lt, <)
ns_eval_number_cmp_op(_le, <=)
ns_eval_number_cmp_op(_gt, >)
ns_eval_number_cmp_op(_ge, >=)
ns_eval_number_shift_op(_shl, <<)
ns_eval_number_shift_op(_shr, >>)

ns_value ns_eval_number_mod(ns_vm *vm, ns_value l, ns_value r) {
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST)};
    switch (ns_type_enum(l.t)) {
        ns_eval_number_op_case(i8, %)
        ns_eval_number_op_case(i16, %)
        ns_eval_number_op_case(i32, %)
        ns_eval_number_op_case(i64, %)
        ns_eval_number_op_case(u8, %)
        ns_eval_number_op_case(u16, %)
        ns_eval_number_op_case(u32, %)
        ns_eval_number_op_case(u64, %)
        case ns_type_f32: ret.f32 = fmod(ns_eval_number_f32(vm, l), ns_eval_number_f32(vm, r)); break;
        case ns_type_f64: ret.f64 = fmod(ns_eval_number_f64(vm, l), ns_eval_number_f64(vm, r)); break;
        default: break;
    }
    return ret;
}


ns_value ns_eval_binary_number_upgrade(ns_vm *vm, ns_value l, ns_value r, ns_token_t op) {
    ns_number_type ln = ns_vm_number_type(l.t);
    ns_number_type rn = ns_vm_number_type(r.t);
    if (((ln & rn) & NS_NUMBER_FLT) == NS_NUMBER_FLT) {
        l.t = ns_type_f64;
        r.t = ns_type_f64;
        return ns_eval_binary_ops_number(vm, l, r, op);
    } else if (((ln & rn) & NS_NUMBER_I) == NS_NUMBER_I) {
        l.t = ns_type_i64;
        r.t = ns_type_i64;
        return ns_eval_binary_ops_number(vm, l, r, op);
    } else {
        return ns_nil;
    }
}

ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value callee = ns_eval_expr(vm, ctx, ctx->nodes[n.call_expr.callee]);
    if (ns_is_nil(callee)) {
        ns_error("eval error", "unknown callee\n");
    }

    ns_symbol *fn = &vm->symbols[ns_type_index(callee.t)];
    ns_call call = (ns_call){.fn = fn, .args = ns_null, .scopes = ns_null };
    ns_array_set_length(call.args, ns_array_length(fn->fn.args));

    i32 next = n.call_expr.arg;
    for (i32 i = 0, l = n.call_expr.arg_count; i < l; ++i) {
        ns_ast_t arg = ctx->nodes[next];
        next = arg.next;
        ns_value v = ns_eval_expr(vm, ctx, arg);
        call.args[i] = v;
    }

    ns_eval_enter_scope(vm, &call);
    ns_array_push(vm->call_stack, call);
    if (ns_str_equals_STR(fn->lib, "std")) {
        ns_vm_eval_std(vm);
    } else {
        ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.fn_def.body]);
    }
    ns_eval_exit_scope(vm, &call);
    call = ns_array_pop(vm->call_stack);
    return call.ret;
}

ns_value ns_eval_return_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = ctx->nodes[n.jump_stmt.expr];
    ns_value ret = ns_eval_expr(vm, ctx, expr);
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        if (call->fn->fn.ret != ret.t) {
            // TODO: try type cast, emit error if failed
            ns_error("eval error", "return type mismatch\n");
        }
        call->ret = ret;
    }
    return ret;
}

void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.jump_stmt.label.type) {
    case NS_TOKEN_RETURN: ns_eval_return_stmt(vm, ctx, n); break;
    default: {
        ns_str l = n.jump_stmt.label.val;
        ns_error("eval error", "unknown jump stmt type %.*s\n", l.len, l.data);
    } break;
    }
}

void ns_eval_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value b = ns_eval_expr(vm, ctx, ctx->nodes[n.if_stmt.condition]);
    ns_call *call = ns_array_last(vm->call_stack);
    ns_ast_t body = ctx->nodes[ns_eval_bool(vm, b) ? n.if_stmt.body : n.if_stmt.else_body];
    ns_eval_enter_scope(vm, call);
    ns_eval_compound_stmt(vm, ctx, body);
    ns_eval_exit_scope(vm, call);
}

void ns_eval_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t gen = ctx->nodes[n.for_stmt.generator];

    ns_call *call = ns_array_last(vm->call_stack);
    ns_eval_enter_scope(vm, call);

    if (gen.gen_expr.range) {
        ns_value from = ns_eval_expr(vm, ctx, ctx->nodes[gen.gen_expr.from]);
        ns_value to = ns_eval_expr(vm, ctx, ctx->nodes[gen.gen_expr.to]);
        ns_str name = gen.gen_expr.name.val;
        ns_scope *scope = ns_array_last(call->scopes);
        ns_array_push(scope->vars, ((ns_symbol){.type = ns_symbol_value, .name = name, .val = { .type = ns_type_i32 }, .parsed = true}));
        i32 from_i = ns_eval_number_i32(vm, from);
        i32 to_i = ns_eval_number_i32(vm, to);
        for (i32 i = from_i; i < to_i; ++i) {
            ns_symbol *index = &scope->vars[0];
            index->val.val.i32 = i;
            ns_eval_compound_stmt(vm, ctx, ctx->nodes[n.for_stmt.body]);
        }
    } else {
        // TODO, generator expr from generable subject.
    }
    ns_eval_exit_scope(vm, call);
}

ns_value ns_eval_binary_ops_number(ns_vm *vm, ns_value l, ns_value r, ns_token_t op) {
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        return ns_str_equals_STR(op.val, "+") ?  ns_eval_binary_add(vm, l, r) : ns_eval_binary_sub(vm, l, r);
    case NS_TOKEN_MUL_OP: {
        if (ns_str_equals_STR(op.val, "*"))
            return ns_eval_binary_mul(vm, l, r);
        else if (ns_str_equals_STR(op.val, "/"))
            return ns_eval_binary_div(vm, l, r);
        else
            return ns_eval_number_mod(vm, l, r);
    } break;
    case NS_TOKEN_SHIFT_OP: {
        if (ns_type_is_float(l.t)) ns_error("eval error", "shift op not support float\n");
        if (ns_str_equals_STR(op.val, "<<"))
            return ns_eval_binary_shl(vm, l, r);
        else
            return ns_eval_binary_shr(vm, l, r);
    } break;
    case NS_TOKEN_LOGIC_OP:
        return ns_str_equals_STR(op.val, "&&") ? ns_eval_binary_and(vm, l, r) : ns_eval_binary_or(vm, l, r);
    case NS_TOKEN_CMP_OP: {
        if (ns_str_equals_STR(op.val, "=="))
            return ns_eval_binary_eq(vm, l, r);
        else if (ns_str_equals_STR(op.val, "!="))
            return ns_eval_binary_ne(vm, l, r);
        else if (ns_str_equals_STR(op.val, "<"))
            return ns_eval_binary_lt(vm, l, r);
        else if (ns_str_equals_STR(op.val, "<="))
            return ns_eval_binary_le(vm, l, r);
        else if (ns_str_equals_STR(op.val, ">"))
            return ns_eval_binary_gt(vm, l, r);
        else if (ns_str_equals_STR(op.val, ">="))
            return ns_eval_binary_ge(vm, l, r);
    } break;
        default:
        ns_error("eval error", "unimplemented binary ops\n");
        break;
    }
    return ns_nil;
}

ns_value ns_eval_call_ops_fn(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_token_t op) {
    ns_str l_name = ns_vm_get_type_name(vm, l.t);
    ns_str r_name = ns_vm_get_type_name(vm, r.t);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
    if (fn == ns_null) {
        ns_error("eval error", "override fn not found %.*s %.*s %.*s\n", l_name.len, l_name.data, op.val.len, op.val.data, r_name.len, r_name.data);
    }

    ns_call call = (ns_call){.fn = fn, .args = ns_null };
    ns_array_push(call.scopes, ((ns_scope){.vars = ns_null, .stack_top = ns_array_length(vm->stack)}));
    ns_array_set_length(call.args, 2);
    call.args[0] = l;
    call.args[1] = r;
    ns_array_push(vm->call_stack, call);
    ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.fn_def.body]);
    ns_array_pop(vm->call_stack);
    ns_array_set_length(vm->stack, call.scopes[0].stack_top);
    return call.ret;
}

ns_value ns_eval_binary_ops(ns_vm *vm, ns_value l, ns_value r, ns_token_t op) {
    if (ns_type_is_number(l.t)) {
        return ns_eval_binary_ops_number(vm, l, r, op);
    } else {
        switch (ns_type_enum(l.t))
        {
        case NS_TYPE_STRING:
            ns_error("eval error", "unimplemented string ops\n");
        default:
            break;
        }
        ns_error("eval error", "unimplemented binary ops\n");
    }
}

ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    if (n.binary_expr.op.type == NS_TOKEN_ASSIGN_OP || n.binary_expr.op.type == NS_TOKEN_ASSIGN) {
        return ns_eval_assign_expr(vm, ctx, n);
    }

    ns_value l = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.left]);
    ns_value r = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.right]);
    if (ns_type_enum(l.t) == ns_type_enum(r.t)) {
        return ns_eval_binary_ops(vm, l, r, n.binary_expr.op); // same type apply binary operator
    }

    ns_value ret = ns_eval_binary_override(vm, ctx, l, r, n.binary_expr.op);
    if (!ns_type_is_unknown(ret.t)) return ret;

    if (ns_type_is_number(l.t) && ns_type_is_number(r.t)) {
        ret = ns_eval_binary_number_upgrade(vm, l, r, n.binary_expr.op);
        if (!ns_type_is_unknown(ret.t)) return ret;
    }

    ns_vm_error(ctx->filename, n.state, "eval error", "binary expr type mismatch.");
    return ns_nil;
}

ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n) {
    ns_token_t t = n.primary_expr.token;
    switch (t.type) {
    case NS_TOKEN_INT_LITERAL:
        return (ns_value){.t = ns_type_i64, .i32 = ns_str_to_i32(t.val)};
    case NS_TOKEN_FLT_LITERAL:
        return (ns_value){.t = ns_type_f64, .f32 = ns_str_to_f64(t.val)};
    case NS_TOKEN_STR_LITERAL:
        return (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, t.val)};
    case NS_TOKEN_TRUE:
        return ns_true;
    case NS_TOKEN_FALSE:
        return ns_false;
    case NS_TOKEN_IDENTIFIER:
        return *ns_eval_find_value(vm, t.val);
    default: {
        ns_str type = ns_token_type_to_string(t.type);
        ns_error("eval error", "unimplemented primary expr type %.*s\n", type.len, type.data);
    } break;
    }
    return ns_nil;
}

ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.type) {
    case NS_AST_EXPR:
        return ns_eval_expr(vm, ctx, ctx->nodes[n.expr.body]);
    case NS_AST_CALL_EXPR:
        return ns_eval_call_expr(vm, ctx, n);
    case NS_AST_BINARY_EXPR:
        return ns_eval_binary_expr(vm, ctx, n);
    case NS_AST_PRIMARY_EXPR:
        return ns_eval_primary_expr(vm, n);
    case NS_AST_DESIG_EXPR:
        return ns_eval_desig_expr(vm, ctx, n);
    default: {
        ns_str type = ns_ast_type_to_string(n.type);
        ns_error("eval error", "unimplemented expr type %.*s\n", type.len, type.data);
    } break;
    }

    return ns_nil;
}

void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = n;
    for (i32 i = 0, l = n.compound_stmt.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        switch (expr.type) {
        case NS_AST_CALL_EXPR:
        case NS_AST_BINARY_EXPR:
        case NS_AST_PRIMARY_EXPR:
        case NS_AST_MEMBER_EXPR:
        case NS_AST_GEN_EXPR:
        case NS_AST_DESIG_EXPR:
        case NS_AST_UNARY_EXPR: ns_eval_expr(vm, ctx, expr); break;
        case NS_AST_VAR_DEF: ns_eval_local_var_def(vm, ctx, expr); break;
        case NS_AST_JUMP_STMT: ns_eval_jump_stmt(vm, ctx, expr); break;
        case NS_AST_FOR_STMT: ns_eval_for_stmt(vm, ctx, expr); break;
        case NS_AST_IF_STMT: ns_eval_if_stmt(vm, ctx, expr); break;
        default: {
            ns_str type = ns_ast_type_to_string(expr.type);
            ns_vm_error(ctx->filename, n.state, "eval error", "unimplemented stmt type %.*s", type.len, type.data);
        } break;
        }
    }
}

ns_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_symbol *st = ns_vm_find_symbol(vm, n.desig_expr.name.val);
    if (ns_null == st) ns_vm_error(ctx->filename, n.state, "eval error", "unknown struct %.*s\n", n.desig_expr.name.val.len, n.desig_expr.name.val.data);

    i32 stride = st->st.stride;
    i32 o = ns_eval_alloc(vm, stride);
    i8* data = &vm->stack[o];
    memset(data, 0, stride);

    ns_ast_t expr = n;
    for (i32 i = 0, l = n.desig_expr.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        ns_str name = expr.field_def.name.val;

        ns_symbol *field = ns_null;
        for (i32 j = 0, jl = ns_array_length(st->st.fields); j < jl; ++j) {
            field = &st->st.fields[j];
            if (ns_str_equals(field->name, name)) {
                break;
            }
        }

        if (field == ns_null) {
            ns_vm_error(ctx->filename, expr.state, "eval error", "unknown field %.*s\n", name.len, name.data);
        }

        ns_value val = ns_eval_expr(vm, ctx, ctx->nodes[expr.field_def.expr]);
        if (ns_type_enum(field->val.type) != ns_type_enum(val.t)) { // type mismatch
            ns_str f_type = ns_vm_get_type_name(vm, field->val.type);
            ns_str v_type = ns_vm_get_type_name(vm, val.t);
            ns_vm_error(ctx->filename, expr.state, "eval error", "field type mismatch [%.*s = %.*s]\n", f_type.len, f_type.data, v_type.len, v_type.data);
        }

        ns_type t = field->val.type;
        if (ns_type_is_number(t)) {
            switch (ns_type_enum(t))
            {
            case NS_TYPE_U8: *(u8*)(data + field->val.offset) = ns_eval_number_i8(vm, val); break;
            case NS_TYPE_I8: *(i8*)(data + field->val.offset) = ns_eval_number_i8(vm, val); break;
            case NS_TYPE_U16: *(u16*)(data + field->val.offset) = ns_eval_number_u16(vm, val); break;
            case NS_TYPE_I16: *(i16*)(data + field->val.offset) = ns_eval_number_i16(vm, val); break;
            case NS_TYPE_U32: *(u32*)(data + field->val.offset) = ns_eval_number_u32(vm, val); break;
            case NS_TYPE_I32: *(i32*)(data + field->val.offset) = ns_eval_number_i32(vm, val); break;
            case NS_TYPE_U64: *(u64*)(data + field->val.offset) = ns_eval_number_u64(vm, val); break;
            case NS_TYPE_I64: *(i64*)(data + field->val.offset) = ns_eval_number_i64(vm, val); break;
            case NS_TYPE_F32: *(f32*)(data + field->val.offset) = ns_eval_number_f32(vm, val); break;
            case NS_TYPE_F64: *(f64*)(data + field->val.offset) = ns_eval_number_f64(vm, val); break;
            default:
                break;
            }
        } else if (ns_type_is(t, NS_TYPE_STRUCT)) {
            if (ns_type_in_stack(t)) {
                memcpy(data, vm->stack + val.o, stride);
            } else {
                memcpy(data, (void*)val.o, stride);
            }
        } else if (ns_type_is(t, NS_TYPE_STRING)) {
            ns_error("eval error", "unimplemented string field\n");
        } else {
            ns_error("eval error", "unknown field type\n");
        }
    }

    return (ns_value){.t = ns_type_set_store(st->val.type, NS_STORE_STACK), .o = o};
}

ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_symbol *val = ns_vm_find_symbol(vm, n.var_def.name.val);
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.var_def.expr]);
    if (ns_type_is_const(v.t))
        v = ns_eval_alloc_value(vm, v);
    val->val.val = v;
    return v;
}

ns_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    ns_scope *scope = &call->scopes[ns_array_length(call->scopes) - 1];
    if (ns_null == scope) ns_vm_error(ctx->filename, n.state, "vm error", "invalid local var def");
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.var_def.expr]);
    if (ns_type_is(v.t, ns_type_nil)) ns_vm_error(ctx->filename, n.state, "eval error", "nil value can't be assigned.");
    ns_symbol symbol = (ns_symbol){.type = ns_symbol_value, .name = n.var_def.name.val };
    if (ns_type_is_const(v.t)) v = ns_eval_alloc_value(vm, v);
    symbol.val.val = v;
    ns_array_push(scope->vars, symbol);
    return v;
}

ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename) {
    ns_ast_ctx ctx = {0};
    ns_ast_parse(&ctx, source, filename);
    ns_vm_parse(vm, &ctx);

    // eval global value
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol r = vm->symbols[i];
        if (r.type != ns_symbol_value)
            continue;
        ns_value v = r.val.val;
        if (r.val.is_const) {
            ns_symbol *record = &vm->symbols[i];
            record->val.val = v;
        }
    }

    ns_symbol* main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main"));
    if (ns_null != main_fn) {
        ns_call call = (ns_call){.fn = main_fn, .args = ns_null, .scopes = ns_null };
        ns_array_push(call.scopes, ((ns_scope){.vars = ns_null, .stack_top = ns_array_length(vm->stack)}));
        ns_array_set_length(call.args, 0);
        ns_array_push(vm->call_stack, call);
        ns_eval_compound_stmt(vm, &ctx, ctx.nodes[main_fn->fn.ast.fn_def.body]);
        ns_array_pop(vm->call_stack);
        ns_array_set_length(vm->stack, call.scopes[0].stack_top);
        return ns_nil;
    }

    ns_value ret = ns_nil;
    for (i32 i = ctx.section_begin, l = ctx.section_end; i < l; ++i) {
        ns_ast_t n = ctx.nodes[ctx.sections[i]];
        switch (n.type) {
        case NS_AST_EXPR:
        case NS_AST_CALL_EXPR:
            ns_eval_expr(vm, &ctx, n);
            break;
        case NS_AST_VAR_DEF:
            ns_eval_var_def(vm, &ctx, n);
            break;
        case NS_AST_IMPORT_STMT:
        case NS_AST_FN_DEF:
        case NS_AST_OPS_FN_DEF:
        case NS_AST_STRUCT_DEF:
            break; // already parsed
        default: {
            ns_str type = ns_ast_type_to_string(n.type);
            ns_warn("eval error", "unimplemented global ast %.*s\n", type.len, type.data);
        } break;
        }
    }

    return ret;
}
