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

ns_value ns_eval_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_ast_t n);

ns_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n);
ns_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_member_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

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
    ns_value ret = (ns_value){.t = ns_type_set_store(n.t, NS_STORE_STACK)};
    if (ns_type_is_const(n.t) || ns_type_in_heap(n.t)) {
        ret.o = n.o;
    } else {
        memcpy(&vm->stack[offset], &vm->stack[n.o], s);
    }
    return ret;
}

ns_value ns_eval_find_value(ns_vm *vm, ns_str name) {
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call* call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        for (i32 i = 0, l = ns_array_length(call->fn->fn.args); i < l; ++i) {
            if (ns_str_equals(call->fn->fn.args[i].name, name)) {
                return call->args[i];
            }
        }

        for (i32 i = 0, l = ns_array_length(call->scopes); i < l; ++i) {
            ns_scope* scope = &call->scopes[i];
            for (i32 j = 0, k = ns_array_length(scope->vars); j < k; ++j) {
                if (ns_str_equals(scope->vars[j].name, name)) {
                    return scope->vars[j].val;
                }
            }
        }
    }

    ns_symbol *r = ns_vm_find_symbol(vm, name);
    if (!r) return ns_nil;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *r = &vm->symbols[i];
        if (ns_str_equals(r->name, name)) {
            switch (r->type)
            {
            case ns_symbol_value: return r->val;
            case ns_symbol_fn: return r->fn.fn;
            default:
                break;
            }
        }
    }

    return ns_nil;
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
    return r;
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
    ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.ops_fn_def.body]);
    ns_eval_exit_scope(vm, &call);
    call = ns_array_pop(vm->call_stack);
    return call.ret;
}

i8 ns_eval_number_i8(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.i8 : *(i8*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
i16 ns_eval_number_i16(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.i16 : *(i16*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
i32 ns_eval_number_i32(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.i32 : *(i32*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
i64 ns_eval_number_i64(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.i64 : *(i64*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
u8 ns_eval_number_u8(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.u8 : *(u8*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
u16 ns_eval_number_u16(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.u16 : *(u16*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
u32 ns_eval_number_u32(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.u32 : *(u32*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
u64 ns_eval_number_u64(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.u64 : *(u64*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
f32 ns_eval_number_f32(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.f32 : *(f32*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
f64 ns_eval_number_f64(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.f64 : *(f64*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }
bool ns_eval_bool(ns_vm *vm, ns_value n) { return ns_eval_number_i32(vm, n) != 0; }

#define ns_eval_number_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) {\
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST) };\
    switch (ns_type_mask(l.t)) {\
        case ns_type_i8: ret.i8 = ns_eval_number_i8(vm, l) op ns_eval_number_i8(vm, r); break;\
        case ns_type_i16: ret.i16 = ns_eval_number_i16(vm, l) op ns_eval_number_i16(vm, r); break;\
        case ns_type_i32: ret.i32 = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        case ns_type_i64: ret.i64 = ns_eval_number_i64(vm, l) op ns_eval_number_i64(vm, r); break;\
        case ns_type_u8: ret.u8 = ns_eval_number_u8(vm, l) op ns_eval_number_u8(vm, r); break;\
        case ns_type_u16: ret.u16 = ns_eval_number_u16(vm, l) op ns_eval_number_u16(vm, r); break;\
        case ns_type_u32: ret.u32 = ns_eval_number_u32(vm, l) op ns_eval_number_u32(vm, r); break;\
        case ns_type_u64: ret.u64 = ns_eval_number_u64(vm, l) op ns_eval_number_u64(vm, r); break;\
        case ns_type_f32: ret.f32 = ns_eval_number_f32(vm, l) op ns_eval_number_f32(vm, r); break;\
        case ns_type_f64: ret.f64 = ns_eval_number_f64(vm, l) op ns_eval_number_f64(vm, r); break;\
        default: break;\
    }\
    return ret;\
}

#define ns_eval_number_cmp_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, NS_STORE_CONST) };\
    switch (ns_type_mask(l.t)) {\
        case ns_type_i8: ret.b = ns_eval_number_i8(vm, l) op ns_eval_number_i8(vm, r); break;\
        case ns_type_i16: ret.b = ns_eval_number_i16(vm, l) op ns_eval_number_i16(vm, r); break;\
        case ns_type_i32: ret.b = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        case ns_type_i64: ret.b = ns_eval_number_i64(vm, l) op ns_eval_number_i64(vm, r); break;\
        case ns_type_u8: ret.b = ns_eval_number_u8(vm, l) op ns_eval_number_u8(vm, r); break;\
        case ns_type_u16: ret.b = ns_eval_number_u16(vm, l) op ns_eval_number_u16(vm, r); break;\
        case ns_type_u32: ret.b = ns_eval_number_u32(vm, l) op ns_eval_number_u32(vm, r); break;\
        case ns_type_u64: ret.b = ns_eval_number_u64(vm, l) op ns_eval_number_u64(vm, r); break;\
        case ns_type_f32: ret.b = ns_eval_number_f32(vm, l) op ns_eval_number_f32(vm, r); break;\
        case ns_type_f64: ret.b = ns_eval_number_f64(vm, l) op ns_eval_number_f64(vm, r); break;\
        default: break;\
    }\
    return ret;\
}

#define ns_eval_number_shift_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST) };\
    switch (ns_type_mask(l.t)) {\
        case ns_type_i8: ret.i8 = ns_eval_number_i8(vm, l) op ns_eval_number_i8(vm, r); break;\
        case ns_type_i16: ret.i16 = ns_eval_number_i16(vm, l) op ns_eval_number_i16(vm, r); break;\
        case ns_type_i32: ret.i32 = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        case ns_type_i64: ret.i64 = ns_eval_number_i64(vm, l) op ns_eval_number_i64(vm, r); break;\
        case ns_type_u8: ret.u8 = ns_eval_number_u8(vm, l) op ns_eval_number_u8(vm, r); break;\
        case ns_type_u16: ret.u16 = ns_eval_number_u16(vm, l) op ns_eval_number_u16(vm, r); break;\
        case ns_type_u32: ret.u32 = ns_eval_number_u32(vm, l) op ns_eval_number_u32(vm, r); break;\
        case ns_type_u64: ret.u64 = ns_eval_number_u64(vm, l) op ns_eval_number_u64(vm, r); break;\
        default: break;\
    }\
    return ret;\
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
    switch (ns_type_mask(l.t)) {
        case ns_type_i8:  ret.i8 = ns_eval_number_i8(vm, l) % ns_eval_number_i8(vm, r); break;
        case ns_type_i16: ret.i16 = ns_eval_number_i16(vm, l) % ns_eval_number_i16(vm, r); break;
        case ns_type_i32: ret.i32 = ns_eval_number_i32(vm, l) % ns_eval_number_i32(vm, r); break;
        case ns_type_i64: ret.i64 = ns_eval_number_i64(vm, l) % ns_eval_number_i64(vm, r); break;
        case ns_type_u8:  ret.u8 = ns_eval_number_u8(vm, l) % ns_eval_number_u8(vm, r); break;
        case ns_type_u16: ret.u16 = ns_eval_number_u16(vm, l) % ns_eval_number_u16(vm, r); break;
        case ns_type_u32: ret.u32 = ns_eval_number_u32(vm, l) % ns_eval_number_u32(vm, r); break;
        case ns_type_u64: ret.u64 = ns_eval_number_u64(vm, l) % ns_eval_number_u64(vm, r); break;
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
        return ns_eval_binary_ops_number(vm, l, r, op);
    } else if (((ln & rn) & NS_NUMBER_I) == NS_NUMBER_I) {
        return ns_eval_binary_ops_number(vm, l, r, op);
    } else {
        return ns_nil;
    }
}

ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value callee = ns_eval_expr(vm, ctx, ctx->nodes[n.call_expr.callee]);
    if (ns_is_nil(callee)) {
        ns_error("eval error", "unknown callee.");
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
        if (ns_type_mask(call->fn->fn.ret) != ns_type_mask(ret.t)) {
            // TODO: try type cast, emit error if failed
            ns_vm_error(ctx->filename, n.state, "eval error", "return type mismatch.");
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
        ns_error("eval error", "unknown jump stmt type %.*s.", l.len, l.data);
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
        ns_array_push(scope->vars, ((ns_symbol){.type = ns_symbol_value, .name = name, .val = { .t = ns_type_i32 }, .parsed = true}));
        i32 from_i = ns_eval_number_i32(vm, from);
        i32 to_i = ns_eval_number_i32(vm, to);
        for (i32 i = from_i; i < to_i; ++i) {
            ns_symbol *index = &scope->vars[0];
            index->val.i32 = i;
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
        if (ns_type_is_float(l.t)) ns_error("eval error", "shift op not support float.");
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
        ns_error("eval error", "unimplemented binary ops.");
        break;
    }
    return ns_nil;
}

ns_value ns_eval_call_ops_fn(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_symbol *fn) {
    ns_call call = (ns_call){.fn = fn, .args = ns_null };
    ns_array_set_length(call.args, 2);
    call.args[0] = l;
    call.args[1] = r;
    ns_eval_enter_scope(vm, &call);
    ns_array_push(vm->call_stack, call);
    ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.ops_fn_def.body]);
    ns_eval_exit_scope(vm, &call);
    call = ns_array_pop(vm->call_stack);
    ns_array_set_length(vm->stack, call.scopes[0].stack_top);
    return call.ret;
}

ns_value ns_eval_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_ast_t n) {
    if (ns_type_is_number(l.t)) {
        return ns_eval_binary_ops_number(vm, l, r, n.binary_expr.op);
    } else {
        switch (ns_type_mask(l.t))
        {
        case NS_TYPE_STRING:
            ns_vm_error(ctx->filename, n.state, "eval error", "unimplemented string ops.");
        default:
            break;
        }
        ns_str l_t = ns_vm_get_type_name(vm, l.t);
        ns_str r_t = ns_vm_get_type_name(vm, r.t);
        ns_str op = ns_ops_name(n.binary_expr.op);
        ns_str fn_name = ns_ops_override_name(l_t, r_t, n.ops_fn_def.ops);
        ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
        if (fn) {
            return ns_eval_call_ops_fn(vm, ctx, l, r, fn);
        }
        ns_error("eval error", "unimplemented binary ops %.*s %.*s %.*s.", l_t.len, l_t.data, op.len, op.data, r_t.len, r_t.data);
    }
}

ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    if (n.binary_expr.op.type == NS_TOKEN_ASSIGN_OP || n.binary_expr.op.type == NS_TOKEN_ASSIGN) {
        return ns_eval_assign_expr(vm, ctx, n);
    }

    ns_value l = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.left]);
    ns_value r = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.right]);
    if (ns_type_mask(l.t) == ns_type_mask(r.t)) {
        return ns_eval_binary_ops(vm, ctx, l, r, n); // same type apply binary operator
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
        return (ns_value){.t = ns_type_i32, .i32 = ns_str_to_i32(t.val)};
    case NS_TOKEN_FLT_LITERAL:
        return (ns_value){.t = ns_type_f64, .f64 = ns_str_to_f64(t.val)};
    case NS_TOKEN_STR_LITERAL:
        return (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, t.val)};
    case NS_TOKEN_TRUE:
        return ns_true;
    case NS_TOKEN_FALSE:
        return ns_false;
    case NS_TOKEN_IDENTIFIER:
        return ns_eval_find_value(vm, t.val);
    default: {
        ns_str type = ns_token_type_to_string(t.type);
        ns_error("eval error", "unimplemented primary expr type %.*s.", type.len, type.data);
    } break;
    }
    return ns_nil;
}

ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.type) {
    case NS_AST_EXPR: return ns_eval_expr(vm, ctx, ctx->nodes[n.expr.body]);
    case NS_AST_CALL_EXPR: return ns_eval_call_expr(vm, ctx, n);
    case NS_AST_BINARY_EXPR: return ns_eval_binary_expr(vm, ctx, n);
    case NS_AST_PRIMARY_EXPR: return ns_eval_primary_expr(vm, n);
    case NS_AST_DESIG_EXPR: return ns_eval_desig_expr(vm, ctx, n);
    case NS_AST_CAST_EXPR: return ns_eval_cast_expr(vm, ctx, n);
    case NS_AST_MEMBER_EXPR: return ns_eval_member_expr(vm, ctx, n);
    case NS_AST_UNARY_EXPR: return ns_eval_unary_expr(vm, ctx, n);
    default: {
        ns_str type = ns_ast_type_to_string(n.type);
        ns_vm_error(ctx->filename, n.state, "eval error", "unimplemented expr type %.*s.", type.len, type.data);
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
    if (ns_null == st) ns_vm_error(ctx->filename, n.state, "eval error", "unknown struct %.*s.", n.desig_expr.name.val.len, n.desig_expr.name.val.data);

    i32 stride = st->st.stride;
    i32 o = ns_eval_alloc(vm, stride);
    i8* data = &vm->stack[o];
    memset(data, 0, stride);

    ns_ast_t expr = n;
    for (i32 i = 0, l = n.desig_expr.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        ns_str name = expr.field_def.name.val;

        ns_struct_field *field = ns_null;
        for (i32 j = 0, jl = ns_array_length(st->st.fields); j < jl; ++j) {
            field = &st->st.fields[j];
            if (ns_str_equals(field->name, name)) {
                break;
            }
        }

        if (field == ns_null) {
            ns_vm_error(ctx->filename, expr.state, "eval error", "unknown field %.*s.", name.len, name.data);
        }

        ns_value val = ns_eval_expr(vm, ctx, ctx->nodes[expr.field_def.expr]);
        if (ns_type_mask(field->t) != ns_type_mask(val.t)) { // type mismatch
            ns_str f_type = ns_vm_get_type_name(vm, field->t);
            ns_str v_type = ns_vm_get_type_name(vm, val.t);
            ns_vm_error(ctx->filename, expr.state, "eval error", "field type mismatch [%.*s = %.*s].", f_type.len, f_type.data, v_type.len, v_type.data);
        }

        ns_type t = field->t;
        if (ns_type_is_number(t)) {
            switch (ns_type_mask(t))
            {
            case NS_TYPE_U8:   *(u8*)(data + field->o) = ns_eval_number_i8(vm, val); break;
            case NS_TYPE_I8:   *(i8*)(data + field->o) = ns_eval_number_i8(vm, val); break;
            case NS_TYPE_U16: *(u16*)(data + field->o) = ns_eval_number_u16(vm, val); break;
            case NS_TYPE_I16: *(i16*)(data + field->o) = ns_eval_number_i16(vm, val); break;
            case NS_TYPE_U32: *(u32*)(data + field->o) = ns_eval_number_u32(vm, val); break;
            case NS_TYPE_I32: *(i32*)(data + field->o) = ns_eval_number_i32(vm, val); break;
            case NS_TYPE_U64: *(u64*)(data + field->o) = ns_eval_number_u64(vm, val); break;
            case NS_TYPE_I64: *(i64*)(data + field->o) = ns_eval_number_i64(vm, val); break;
            case NS_TYPE_F32: *(f32*)(data + field->o) = ns_eval_number_f32(vm, val); break;
            case NS_TYPE_F64: *(f64*)(data + field->o) = ns_eval_number_f64(vm, val); break;
            default:
                break;
            }
        } else if (ns_type_is(t, NS_TYPE_STRUCT)) {
            if (ns_type_in_heap(val.t)) {
                memcpy(data, (void*)val.o, stride);
            } else {
                memcpy(data, vm->stack + val.o, stride);
            }
        } else if (ns_type_is(t, NS_TYPE_STRING)) {
            ns_error("eval error", "unimplemented string field.");
        } else {
            ns_error("eval error", "unknown field type.");
        }
    }

    return (ns_value){.t = ns_type_set_store(st->val.t, NS_STORE_STACK), .o = o};
}

ns_value ns_eval_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_type t = ns_vm_parse_type(vm, n.cast_expr.type, false);
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.cast_expr.expr]);
    if (ns_type_mask(t) == ns_type_mask(v.t)) return v;
    if (ns_type_is_number(t) && ns_type_is_number(v.t)) {
        if (ns_type_is_float(t)) {
            if (ns_type_is_float(v.t)) {
                if (ns_type_is(t, NS_TYPE_F32)) {
                    return (ns_value){.t = t, .f32 = (f32)ns_eval_number_f64(vm, v)};
                } else {
                    return (ns_value){.t = t, .f64 = ns_eval_number_f64(vm, v)};
                }
            } else {
                if (ns_type_is(t, NS_TYPE_F32)) {
                    return (ns_value){.t = t, .f32 = (f32)ns_eval_number_i64(vm, v)};
                } else {
                    return (ns_value){.t = t, .f64 = (f64)ns_eval_number_i64(vm, v)};
                }
            }
        } else {
            if (ns_type_is_float(v.t)) {
                return (ns_value){.t = t, .i64 = (i64)ns_eval_number_f64(vm, v)};
            } else {
                return (ns_value){.t = t, .i64 = ns_eval_number_i64(vm, v)};
            }
        }
    } else {
        ns_vm_error(ctx->filename, n.state, "eval error", "unimplemented cast expr");
    }
}

ns_value ns_eval_member_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value st = ns_eval_expr(vm, ctx, ctx->nodes[n.member_expr.left]);

    ns_ast_t field = ctx->nodes[n.next];

    ns_str name = field.primary_expr.token.val;
    ns_symbol *st_type = &vm->symbols[ns_type_index(st.t)];
    if (st_type->type != ns_symbol_struct) {
        ns_type_print(st.t);
        ns_vm_error(ctx->filename, n.state, "eval error", "unknown struct %.*s.", st_type->name.len, st_type->name.data);
    }
    for (i32 i = 0, l = ns_array_length(st_type->st.fields); i < l; ++i) {
        ns_struct_field *field = &st_type->st.fields[i];
        if (ns_str_equals(field->name, name)) {
            if(ns_type_in_heap(st.t)) {
                return (ns_value){.t = ns_type_set_store(field->t, NS_STORE_HEAP), .o = st.o + field->o};
            } else {
                return (ns_value){.t = ns_type_set_store(field->t, NS_STORE_STACK), .o = st.o + field->o};
            }
        }
    }

    ns_vm_error(ctx->filename, n.state, "eval error", "unknown struct field %.*s.", name.len, name.data);
    return ns_nil;
}

ns_value ns_eval_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = ctx->nodes[n.unary_expr.expr];
    ns_value v = ns_eval_expr(vm, ctx, expr);
    switch (n.unary_expr.op.type) {
    case NS_TOKEN_ADD_OP:
        if (!ns_type_is_number(v.t)) {
            ns_vm_error(ctx->filename, n.state, "eval error", "unary expr type mismatch\n");
        }
        if (ns_str_equals_STR(n.unary_expr.op.val, "-")) {
            switch (ns_type_enum(v.t)) {
            case NS_TYPE_I8: v.i8 = -ns_eval_number_i8(vm, v); break;
            case NS_TYPE_U8: v.u8 = -ns_eval_number_u8(vm, v); break;
            case NS_TYPE_I16: v.i16 = -ns_eval_number_i16(vm, v); break;
            case NS_TYPE_U16: v.u16 = -ns_eval_number_u16(vm, v); break;
            case NS_TYPE_I32: v.i32 = -ns_eval_number_i32(vm, v); break;
            case NS_TYPE_U32: v.u32 = -ns_eval_number_u32(vm, v); break;
            case NS_TYPE_I64: v.i64 = -ns_eval_number_i64(vm, v); break;
            case NS_TYPE_U64: v.u64 = -ns_eval_number_u64(vm, v); break;
            case NS_TYPE_F32: v.f32 = -ns_eval_number_f32(vm, v); break;
            case NS_TYPE_F64: v.f64 = -ns_eval_number_f64(vm, v); break;
            case NS_TYPE_BOOL: v.b = !v.b; break;
            default: break;
            }
            v.t = ns_type_set_store(v.t, NS_STORE_CONST);
            return v;
        } else {
            ns_vm_error(ctx->filename, n.state, "eval error", "unary expr type mismatch\n");
        }
        return v;
    case NS_TOKEN_BIT_INVERT_OP:
        if (!ns_type_is(v.t, ns_type_bool)) {
            ns_vm_error(ctx->filename, n.state, "eval error", "unary expr type mismatch\n");
        }
        if (ns_str_equals_STR(n.unary_expr.op.val, "!")) {
            ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, NS_STORE_CONST)};
            switch (ns_type_enum(v.t)) {
            case NS_TYPE_I8: ret.b = 0 != ns_eval_number_i8(vm, v); break;
            case NS_TYPE_U8: ret.b = 0 != ns_eval_number_u8(vm, v); break;
            case NS_TYPE_I16: ret.b = 0 != ns_eval_number_i16(vm, v); break;
            case NS_TYPE_U16: ret.b = 0 != ns_eval_number_u16(vm, v); break;
            case NS_TYPE_I32: ret.b = 0 != ns_eval_number_i32(vm, v); break;
            case NS_TYPE_U32: ret.b = 0 != ns_eval_number_u32(vm, v); break;
            case NS_TYPE_I64: ret.b = 0 != ns_eval_number_i64(vm, v); break;
            case NS_TYPE_U64: ret.b = 0 != ns_eval_number_u64(vm, v); break;
            case NS_TYPE_F32: ret.b = 0 != ns_eval_number_f32(vm, v); break;
            case NS_TYPE_F64: ret.b = 0 != ns_eval_number_f64(vm, v); break;
            case NS_TYPE_BOOL: ret.b = !v.b; break;
            default: break;
            }
            return ret;
        } else {
            ns_vm_error(ctx->filename, n.state, "eval error", "unary expr type mismatch\n");
        }
    default:
        ns_vm_error(ctx->filename, n.state, "syntax error", "unknown unary ops %.*s\n", n.unary_expr.op.val.len, n.unary_expr.op.val.data);
    }
    return ns_nil;
}

ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_symbol *val = ns_vm_find_symbol(vm, n.var_def.name.val);
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.var_def.expr]);
    if (ns_type_is_const(v.t))
        v = ns_eval_alloc_value(vm, v);
    val->val = v;
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
    symbol.val = v;
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
        ns_value v = r.val;
        if (ns_type_is_const(r.val.t)) {
            ns_symbol *record = &vm->symbols[i];
            record->val = v;
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
            ns_warn("eval error", "unimplemented global ast %.*s.", type.len, type.data);
        } break;
        }
    }

    return ret;
}
