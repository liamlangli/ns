#include "ns_vm.h"
#include "ns_fmt.h"

#ifdef NS_DEBUG_HOOK
    #define ns_vm_inject_hook(vm, ctx, i) if (vm->step_hook) vm->step_hook(vm, ctx, i)
#else
    #define ns_vm_inject_hook(vm, ctx, i)
#endif

u64 ns_eval_alloc(ns_vm *vm, i32 stride);
ns_return_value ns_eval_copy(ns_vm *vm, ns_value dst, ns_value src, i32 size);

ns_return_void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_void ns_eval_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_void ns_eval_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

ns_return_value ns_eval_binary_override(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i);
ns_return_value ns_eval_binary_ops_number(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i);
ns_return_value ns_eval_binary_number_upgrade(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i);

ns_return_value ns_eval_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i);

ns_return_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_str_fmt_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_member_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

ns_return_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

u64 ns_eval_alloc(ns_vm *vm, i32 stride) {
    u64 offset = ns_array_length(vm->stack);
    u64 align = ns_min(sizeof(void*), (u64)stride);
    if (align > 0) offset = (offset + (align - 1)) & ~(align - 1);
    ns_array_set_length(vm->stack, offset + stride);
    return offset; // leading 4 bytes for type size
}

ns_return_value ns_eval_copy(ns_vm *vm, ns_value dst, ns_value src, i32 size) {
    u64 offset = dst.o;
    if (ns_type_in_stack(dst.t)) {
        switch (src.t.store)
        {
        case NS_STORE_CONST: {
            switch (src.t.type)
            {
            case NS_TYPE_I8: *(i8*)&vm->stack[offset] = src.i8; break;
            case NS_TYPE_I16: *(i16*)&vm->stack[offset] = src.i16; break;
            case NS_TYPE_I32: *(i32*)&vm->stack[offset] = src.i32; break;
            case NS_TYPE_I64: *(i64*)&vm->stack[offset] = src.i64; break;
            case NS_TYPE_U8: *(u8*)&vm->stack[offset] = src.u8; break;
            case NS_TYPE_U16: *(u16*)&vm->stack[offset] = src.u16; break;
            case NS_TYPE_U32: *(u32*)&vm->stack[offset] = src.u32; break;
            case NS_TYPE_U64: *(u64*)&vm->stack[offset] = src.u64; break;
            case NS_TYPE_F32: *(f32*)&vm->stack[offset] = src.f32; break;
            case NS_TYPE_F64: *(f64*)&vm->stack[offset] = src.f64; break;
            case NS_TYPE_BOOL: *(ns_bool*)&vm->stack[offset] = src.b; break;
            case NS_TYPE_STRING: *(u64*)&vm->stack[offset] = src.o; break;
            default: return ns_return_error(value, ns_code_loc_nil, NS_ERR_EVAL, "invalid const type.");
            }
        } break;
        case NS_STORE_STACK: memcpy(&vm->stack[offset], &vm->stack[src.o], size); break;
        case NS_STORE_HEAP: memcpy(&vm->stack[offset], (void*)src.o, size); break;
        default: return ns_return_error(value, ns_code_loc_nil, NS_ERR_EVAL, "invalid store type.");
        }
    } else if (ns_type_in_heap(dst.t)) {
        switch (src.t.store)
        {
        case NS_STORE_CONST: {
            switch (src.t.type)
            {
            case NS_TYPE_I8: *(i8*)dst.o = src.i8; break;
            case NS_TYPE_I16: *(i16*)dst.o = src.i16; break;
            case NS_TYPE_I32: *(i32*)dst.o = src.i32; break;
            case NS_TYPE_I64: *(i64*)dst.o = src.i64; break;
            case NS_TYPE_U8: *(u8*)dst.o = src.u8; break;
            case NS_TYPE_U16: *(u16*)dst.o = src.u16; break;
            case NS_TYPE_U32: *(u32*)dst.o = src.u32; break;
            case NS_TYPE_U64: *(u64*)dst.o = src.u64; break;
            case NS_TYPE_F32: *(f32*)dst.o = src.f32; break;
            case NS_TYPE_F64: *(f64*)dst.o = src.f64; break;
            case NS_TYPE_BOOL: *(ns_bool*)dst.o = src.b; break;
            case NS_TYPE_STRING: *(u64*)dst.o = src.o; break;
            default: return ns_return_error(value, ns_code_loc_nil, NS_ERR_EVAL, "invalid const type.");
            }
        } break;
        case NS_STORE_STACK: memcpy((void*)dst.o, &vm->stack[src.o], size); break;
        case NS_STORE_HEAP: memcpy((void*)dst.o, (void*)src.o, size); break;
        default: return ns_return_error(value, ns_code_loc_nil, NS_ERR_EVAL, "invalid store type.");
        }
    } else {
        return ns_return_error(value, ns_code_loc_nil, NS_ERR_EVAL, "invalid store type.");
    }
    return ns_return_ok(value, dst);
}

ns_value ns_eval_find_value(ns_vm *vm, ns_str name) {
    ns_symbol *s = ns_vm_find_symbol(vm, name);
    if (!s) return ns_nil;
    switch (s->type)
    {
    case NS_SYMBOL_VALUE: return s->val;
    case NS_SYMBOL_FN: return s->fn.fn;
    case NS_SYMBOL_STRUCT: return s->st.st;
    default: break;
    }
    return ns_nil;
}

ns_scope* ns_enter_scope(ns_vm *vm) {
    ns_scope scope = (ns_scope){.stack_top = ns_array_length(vm->stack), .symbol_top = ns_array_length(vm->symbol_stack)};
    ns_array_push(vm->scope_stack, scope);
    return ns_array_last(vm->scope_stack);
}

ns_scope* ns_exit_scope(ns_vm *vm) {
    ns_scope *scope = ns_array_last(vm->scope_stack);
    ns_array_set_length(vm->stack, scope->stack_top);
    ns_array_set_length(vm->symbol_stack, scope->symbol_top);
    ns_array_pop(vm->scope_stack);
    return scope;
}

ns_return_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_l = ns_eval_expr(vm, ctx, n->binary_expr.left);
    if (ns_return_is_error(ret_l)) return ns_return_change_type(value, ret_l);
    ns_value l = ret_l.r;

    ns_return_value ret_r = ns_eval_expr(vm, ctx, n->binary_expr.right);
    if (ns_return_is_error(ret_r)) return ns_return_change_type(value, ret_r);
    ns_value r = ret_r.r;

    if (!ns_type_equals(l.t, r.t) && ns_type_is(l.t, NS_TYPE_INFER))
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid assign expr.");

    i32 s_l = ns_type_size(vm, l.t);
    i32 s_r = ns_type_size(vm, r.t);
    if (s_l != s_r)
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "type size mismatched.");

    if (ns_type_is_const(l.t))
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "can't assign to const value.");

    ns_eval_copy(vm, l, r, s_l);
    return ns_return_ok(value, l);
}

ns_return_value ns_eval_binary_override(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t op = n->binary_expr.op;
    ns_str l_name = ns_vm_get_type_name(vm, l.t);
    ns_str r_name = ns_vm_get_type_name(vm, r.t);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
    if (!fn) return ns_return_ok(value, ns_nil);

    i32 ret_size = ns_type_size(vm, fn->fn.ret);
    if (ret_size < 0) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid override fn.");

    u64 ret_offset = ns_eval_alloc(vm, ret_size);
    ns_value ret_val = (ns_value){.t = ns_type_set_store(fn->fn.ret, NS_STORE_STACK), .o = ret_offset};
    ns_call call = (ns_call){.fn = fn, .ret = ret_val, .ret_set = false, .scope_top = ns_array_length(vm->scope_stack), .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = 2};
    ns_enter_scope(vm);

    ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[0].name, .val = l};
    ns_array_push(vm->symbol_stack, l_arg);
    ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[1].name, .val = r};
    ns_array_push(vm->symbol_stack, r_arg);

    ns_array_push(vm->call_stack, call);
    ns_ast_t *fn_ast = &ctx->nodes[fn->fn.ast];
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn_ast->ops_fn_def.body);
    if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
    call = ns_array_pop(vm->call_stack);
    ns_exit_scope(vm);

    return ns_return_ok(value, call.ret);
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
ns_bool ns_eval_bool(ns_vm *vm, ns_value n) { return ns_eval_number_i32(vm, n) != 0; }
ns_str ns_eval_str(ns_vm *vm, ns_value n) {
    if (ns_type_is_const(n.t)) return vm->str_list[n.o];
    if (ns_type_in_stack(n.t)) return vm->str_list[*(u64*)&vm->stack[n.o]];
    return *(ns_str*)n.o;
}

void *ns_eval_array_raw(ns_vm *vm, ns_value n) {
    if (ns_type_is_const(n.t)) return (void*)n.o;
    if (ns_type_in_stack(n.t)) return (void*)&vm->stack[n.o];
    return (void*)n.o;
}

#define ns_eval_number_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) {\
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST) };\
    switch (l.t.type) {\
        case NS_TYPE_I8:  ret.i8 = ns_eval_number_i8(vm, l) op ns_eval_number_i8(vm, r); break;\
        case NS_TYPE_I16: ret.i16 = ns_eval_number_i16(vm, l) op ns_eval_number_i16(vm, r); break;\
        case NS_TYPE_I32: ret.i32 = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        case NS_TYPE_I64: ret.i64 = ns_eval_number_i64(vm, l) op ns_eval_number_i64(vm, r); break;\
        case NS_TYPE_U8:  ret.u8 = ns_eval_number_u8(vm, l) op ns_eval_number_u8(vm, r); break;\
        case NS_TYPE_U16: ret.u16 = ns_eval_number_u16(vm, l) op ns_eval_number_u16(vm, r); break;\
        case NS_TYPE_U32: ret.u32 = ns_eval_number_u32(vm, l) op ns_eval_number_u32(vm, r); break;\
        case NS_TYPE_U64: ret.u64 = ns_eval_number_u64(vm, l) op ns_eval_number_u64(vm, r); break;\
        case NS_TYPE_F32: ret.f32 = ns_eval_number_f32(vm, l) op ns_eval_number_f32(vm, r); break;\
        case NS_TYPE_F64: ret.f64 = ns_eval_number_f64(vm, l) op ns_eval_number_f64(vm, r); break;\
        default: break;\
    }\
    return ret;\
}

#define ns_eval_number_cmp_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, NS_STORE_CONST) };\
    switch (l.t.type) {\
        case NS_TYPE_I8: ret.b = ns_eval_number_i8(vm, l) op ns_eval_number_i8(vm, r); break;\
        case NS_TYPE_I16: ret.b = ns_eval_number_i16(vm, l) op ns_eval_number_i16(vm, r); break;\
        case NS_TYPE_I32: ret.b = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        case NS_TYPE_I64: ret.b = ns_eval_number_i64(vm, l) op ns_eval_number_i64(vm, r); break;\
        case NS_TYPE_U8: ret.b = ns_eval_number_u8(vm, l) op ns_eval_number_u8(vm, r); break;\
        case NS_TYPE_U16: ret.b = ns_eval_number_u16(vm, l) op ns_eval_number_u16(vm, r); break;\
        case NS_TYPE_U32: ret.b = ns_eval_number_u32(vm, l) op ns_eval_number_u32(vm, r); break;\
        case NS_TYPE_U64: ret.b = ns_eval_number_u64(vm, l) op ns_eval_number_u64(vm, r); break;\
        case NS_TYPE_F32: ret.b = ns_eval_number_f32(vm, l) op ns_eval_number_f32(vm, r); break;\
        case NS_TYPE_F64: ret.b = ns_eval_number_f64(vm, l) op ns_eval_number_f64(vm, r); break;\
        default: break;\
    }\
    return ret;\
}

#define ns_eval_number_shift_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_set_store(l.t, NS_STORE_CONST) };\
    switch (l.t.type) {\
        case NS_TYPE_I8:  ret.i8 = ns_eval_number_i8(vm, l) op ns_eval_number_i8(vm, r); break;\
        case NS_TYPE_I16: ret.i16 = ns_eval_number_i16(vm, l) op ns_eval_number_i16(vm, r); break;\
        case NS_TYPE_I32: ret.i32 = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        case NS_TYPE_I64: ret.i64 = ns_eval_number_i64(vm, l) op ns_eval_number_i64(vm, r); break;\
        case NS_TYPE_U8:  ret.u8 = ns_eval_number_u8(vm, l) op ns_eval_number_u8(vm, r); break;\
        case NS_TYPE_U16: ret.u16 = ns_eval_number_u16(vm, l) op ns_eval_number_u16(vm, r); break;\
        case NS_TYPE_U32: ret.u32 = ns_eval_number_u32(vm, l) op ns_eval_number_u32(vm, r); break;\
        case NS_TYPE_U64: ret.u64 = ns_eval_number_u64(vm, l) op ns_eval_number_u64(vm, r); break;\
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
    switch (l.t.type) {
        case NS_TYPE_I8:  ret.i8 = ns_eval_number_i8(vm, l) % ns_eval_number_i8(vm, r); break;
        case NS_TYPE_I16: ret.i16 = ns_eval_number_i16(vm, l) % ns_eval_number_i16(vm, r); break;
        case NS_TYPE_I32: ret.i32 = ns_eval_number_i32(vm, l) % ns_eval_number_i32(vm, r); break;
        case NS_TYPE_I64: ret.i64 = ns_eval_number_i64(vm, l) % ns_eval_number_i64(vm, r); break;
        case NS_TYPE_U8:  ret.u8 = ns_eval_number_u8(vm, l) % ns_eval_number_u8(vm, r); break;
        case NS_TYPE_U16: ret.u16 = ns_eval_number_u16(vm, l) % ns_eval_number_u16(vm, r); break;
        case NS_TYPE_U32: ret.u32 = ns_eval_number_u32(vm, l) % ns_eval_number_u32(vm, r); break;
        case NS_TYPE_U64: ret.u64 = ns_eval_number_u64(vm, l) % ns_eval_number_u64(vm, r); break;
        case NS_TYPE_F32: ret.f32 = fmod(ns_eval_number_f32(vm, l), ns_eval_number_f32(vm, r)); break;
        case NS_TYPE_F64: ret.f64 = fmod(ns_eval_number_f64(vm, l), ns_eval_number_f64(vm, r)); break;
        default: break;
    }
    return ret;
}

ns_return_value ns_eval_binary_number_upgrade(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_number_type ln = ns_vm_number_type(l.t);
    ns_number_type rn = ns_vm_number_type(r.t);
    if (((ln & rn) & NS_NUMBER_FLT) == NS_NUMBER_FLT) {
        return ns_eval_binary_ops_number(vm, ctx, l, r, i);
    } else if (((ln & rn) & NS_NUMBER_I) == NS_NUMBER_I) {
        return ns_eval_binary_ops_number(vm, ctx, l, r, i);
    }

    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_callee = ns_eval_expr(vm, ctx, n->call_expr.callee);
    if (ns_return_is_error(ret_callee)) return ret_callee;

    ns_value callee = ret_callee.r;
    if (ns_is_nil(callee)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "nil callee.");
    }

    ns_symbol *fn = &vm->symbols[ns_type_index(callee.t)];
    i32 ret_size = ns_type_size(vm, fn->fn.ret);
    if (ret_size < 0) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid override fn.");

    u64 ret_offset = ns_eval_alloc(vm, ret_size);
    ns_value ret_val = (ns_value){.t = ns_type_set_store(fn->fn.ret, NS_STORE_STACK), .o = ret_offset};
    ns_call call = (ns_call){.fn = fn, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = ns_array_length(fn->fn.args)};

    ns_enter_scope(vm);
    i32 next = n->call_expr.arg;
    for (i32 a_i = 0, l = n->call_expr.arg_count; a_i < l; ++a_i) {
        ns_return_value ret_v = ns_eval_expr(vm, ctx, next);
        if (ns_return_is_error(ret_v)) return ret_v;

        ns_value v = ret_v.r;
        next = ctx->nodes[next].next;
        ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[a_i].name, .val = v, .parsed = true};
        ns_array_push(vm->symbol_stack, arg);
    }

    ns_array_push(vm->call_stack, call);
    if (fn->fn.fn.t.ref) {
        ns_return_bool ret = ns_vm_call_ref(vm);
        if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
    } else {
        ns_ast_t *fn_ast = &ctx->nodes[fn->fn.ast];
        ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn_ast->fn_def.body);
        if (ns_return_is_error(ret)) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "call expr error.");
    }
    call = ns_array_pop(vm->call_stack);
    ns_exit_scope(vm);
    return ns_return_ok(value, call.ret);
}

ns_return_value ns_eval_return_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_ret = ns_eval_expr(vm, ctx, n->jump_stmt.expr);
    if (ns_return_is_error(ret_ret)) return ret_ret;

    ns_value ret = ret_ret.r;
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        if (!ns_type_equals(call->fn->fn.ret, ret.t)) {
            // TODO: try type cast, emit error if failed
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "return type mismatch.");
        }
        ns_eval_copy(vm, call->ret, ret, ns_type_size(vm, ret.t));
        call->ret_set = true;
    }
    return ns_return_ok(value, ret);
}

ns_return_void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->jump_stmt.label.type) {
    case NS_TOKEN_RETURN: ns_eval_return_stmt(vm, ctx, i); break;
    default:
        return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown jump stmt type.");
    }
    return ns_return_ok_void;
}

ns_return_void ns_eval_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_b = ns_eval_expr(vm, ctx, n->if_stmt.condition);
    if (ns_return_is_error(ret_b)) return ns_return_change_type(void, ret_b);

    ns_value b = ret_b.r;
    i32 body = ns_eval_bool(vm, b) ? n->if_stmt.body : n->if_stmt.else_body;
    ns_enter_scope(vm);
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, body);
    if (ns_return_is_error(ret)) return ret;
    ns_exit_scope(vm);

    return ns_return_ok_void;
}

ns_return_void ns_eval_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *gen = &ctx->nodes[n->for_stmt.generator];

    ns_enter_scope(vm);
    if (gen->gen_expr.range) {
        ns_return_value ret_from = ns_eval_expr(vm, ctx, gen->gen_expr.from);
        if (ns_return_is_error(ret_from)) return ns_return_change_type(void, ret_from);
        ns_value from = ret_from.r;

        ns_return_value ret_to = ns_eval_expr(vm, ctx, gen->gen_expr.to);
        if (ns_return_is_error(ret_to)) return ns_return_change_type(void, ret_to);
        ns_value to = ret_to.r;

        ns_str name = gen->gen_expr.name.val;

        i32 ii = ns_array_length(vm->symbol_stack);
        ns_array_push(vm->symbol_stack, ((ns_symbol){.type = NS_SYMBOL_VALUE, .name = name, .val = { .t = ns_type_i32 }, .parsed = true}));
        i32 from_i = ns_eval_number_i32(vm, from);
        i32 to_i = ns_eval_number_i32(vm, to);
        for (i32 g_i = from_i; g_i < to_i; ++g_i) {
            ns_symbol *index = &vm->symbol_stack[ii];
            index->val.i32 = g_i;
            ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->for_stmt.body);
            if (ns_return_is_error(ret)) return ret;
        }
    } else {
        // TODO, generator expr from generable subject.
    }
    ns_exit_scope(vm);
    return ns_return_ok_void;
}

ns_return_value ns_eval_binary_ops_number(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t op = n->binary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP: {
        ns_value n = ns_str_equals_STR(op.val, "+") ? ns_eval_binary_add(vm, l, r) : ns_eval_binary_sub(vm, l, r);
        return ns_return_ok(value, n);
    } break;
    case NS_TOKEN_MUL_OP: {
        if (ns_str_equals_STR(op.val, "*"))
            return ns_return_ok(value, ns_eval_binary_mul(vm, l, r));
        else if (ns_str_equals_STR(op.val, "/"))
            return ns_return_ok(value, ns_eval_binary_div(vm, l, r));
        else
            return ns_return_ok(value, ns_eval_number_mod(vm, l, r));
    } break;
    case NS_TOKEN_SHIFT_OP: {
        if (ns_type_is_float(l.t)) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "shift operator not supported for float type.");
        if (ns_str_equals_STR(op.val, "<<"))
            return ns_return_ok(value, ns_eval_binary_shl(vm, l, r));
        else
            return ns_return_ok(value, ns_eval_binary_shr(vm, l, r));
    } break;
    case NS_TOKEN_LOGIC_OP: {
        ns_value n = ns_str_equals_STR(op.val, "&&") ? ns_eval_binary_and(vm, l, r) : ns_eval_binary_or(vm, l, r);
        return ns_return_ok(value, n);
    } break;
    case NS_TOKEN_CMP_OP: {
        if (ns_str_equals_STR(op.val, "=="))
            return ns_return_ok(value, ns_eval_binary_eq(vm, l, r));
        else if (ns_str_equals_STR(op.val, "!="))
            return ns_return_ok(value, ns_eval_binary_ne(vm, l, r));
        else if (ns_str_equals_STR(op.val, "<"))
            return ns_return_ok(value, ns_eval_binary_lt(vm, l, r));
        else if (ns_str_equals_STR(op.val, "<="))
            return ns_return_ok(value, ns_eval_binary_le(vm, l, r));
        else if (ns_str_equals_STR(op.val, ">"))
            return ns_return_ok(value, ns_eval_binary_gt(vm, l, r));
        else if (ns_str_equals_STR(op.val, ">="))
            return ns_return_ok(value, ns_eval_binary_ge(vm, l, r));
    } break;
        default:
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented binary ops.");
        break;
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_call_ops_fn(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_value l, ns_value r, ns_symbol *fn) {
    ns_vm_inject_hook(vm, ctx, i);
    ns_unused(i);

    i32 ret_size = ns_type_size(vm, fn->fn.ret);
    ns_ast_t *n = &ctx->nodes[i];
    if (ret_size < 0) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid override fn.");

    u64 ret_offset = ns_eval_alloc(vm, ret_size);
    ns_value ret_val = (ns_value){.t = ns_type_set_store(fn->fn.ret, NS_STORE_STACK), .o = ret_offset};
    ns_call call = (ns_call){.fn = fn, .ret = ret_val, .ret_set = false, .scope_top = ns_array_length(vm->scope_stack), .arg_offset = ns_array_length(vm->symbol_stack), 2 };
    ns_enter_scope(vm);

    ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[0].name, .val = l, .parsed = true};
    ns_array_push(vm->symbol_stack, l_arg);
    ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[1].name, .val = r, .parsed = true};
    ns_array_push(vm->symbol_stack, r_arg);

    ns_array_push(vm->call_stack, call);
    ns_ast_t *fn_ast = &ctx->nodes[fn->fn.ast];
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn_ast->ops_fn_def.body);
    if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);

    call = ns_array_pop(vm->call_stack);
    ns_exit_scope(vm);
    return ns_return_ok(value, call.ret);
}

ns_return_value ns_eval_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (ns_type_is_number(l.t)) {
        return ns_eval_binary_ops_number(vm, ctx, l, r, i);
    } else {
        switch (l.t.type)
        {
        case NS_TYPE_STRING:
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented string ops.");
        default:
            break;
        }
        ns_str l_t = ns_vm_get_type_name(vm, l.t);
        ns_str r_t = ns_vm_get_type_name(vm, r.t);
        ns_str fn_name = ns_ops_override_name(l_t, r_t, n->ops_fn_def.ops);
        ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
        if (fn) {
            return ns_eval_call_ops_fn(vm, ctx, i, l, r, fn);
        }
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented binary ops.");
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP || n->binary_expr.op.type == NS_TOKEN_ASSIGN) {
        return ns_eval_assign_expr(vm, ctx, i);
    }

    ns_return_value ret_l = ns_eval_expr(vm, ctx, n->binary_expr.left);
    if (ns_return_is_error(ret_l)) return ns_return_change_type(value, ret_l);
    ns_value l = ret_l.r;

    ns_return_value ret_r = ns_eval_expr(vm, ctx, n->binary_expr.right);
    if (ns_return_is_error(ret_r)) return ns_return_change_type(value, ret_r);
    ns_value r = ret_r.r;

    if (ns_type_equals(l.t, r.t)) {
        return ns_eval_binary_ops(vm, ctx, l, r, i); // same type apply binary operator
    }

    ns_return_value ret_ret = ns_eval_binary_override(vm, ctx, l, r, i);
    if (ns_return_is_error(ret_ret)) return ret_ret;
    ns_value ret = ret_ret.r;

    if (!ns_type_is_unknown(ret.t)) return ns_return_ok(value, ret);

    if (ns_type_is_number(l.t) && ns_type_is_number(r.t)) {
        ret_ret = ns_eval_binary_number_upgrade(vm, ctx, l, r, i);
        if (ns_return_is_error(ret_ret)) return ret_ret;

        ret = ret_ret.r;
        if (!ns_type_is_unknown(ret.t)) return ns_return_ok(value, ret);
    }

    return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "binary expr type mismatch.");
}

ns_return_value ns_eval_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t t = n->primary_expr.token;
    ns_value ret;
    switch (t.type) {
    case NS_TOKEN_INT_LITERAL: ret = (ns_value){.t = ns_type_i32, .i32 = ns_str_to_i32(t.val)}; break;
    case NS_TOKEN_FLT_LITERAL: ret = (ns_value){.t = ns_type_f64, .f64 = ns_str_to_f64(t.val)}; break;
    case NS_TOKEN_STR_LITERAL: ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, t.val)}; break;
    case NS_TOKEN_STR_FORMAT: 
    case NS_TOKEN_TRUE: ret = ns_true; break;
    case NS_TOKEN_FALSE: ret = ns_false; break;
    case NS_TOKEN_IDENTIFIER: ret = ns_eval_find_value(vm, t.val); break;
    default: {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented primary expr type.");
    } break;
    }
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_str_fmt_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    i32 count = n->str_fmt.expr_count;
    ns_str fmt = n->str_fmt.fmt;

    ns_str ret = {.data = ns_null, .len = 0, .dynamic = 1};
    ns_array_set_capacity(ret.data, fmt.len);

    i32 j = 0;
    ns_ast_t *expr = n;
    while (j < fmt.len) {
        if (fmt.data[j] == '{' && (j == 0 || (j > 0 && fmt.data[j - 1] != '\\'))) {
            ++j;
            while (j < fmt.len && fmt.data[j] != '}') {
                j++;
            }
            j++;

            if (j == fmt.len) {
                return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "missing '}'.");
            }

            ns_return_value ret_v = ns_eval_expr(vm, ctx, expr->next);
            expr = &ctx->nodes[expr->next];
            count--;

            if (count < 0) {
                return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unmatched fmt expr.");
            }

            if (ns_return_is_error(ret_v)) {
                return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "failed to eval fmt expr.");
            }
            ns_value v = ret_v.r;
            ns_str s = ns_fmt_value(vm, v);
            ns_str_append(&ret, s);
            ns_str_free(s);
        } else {
            ns_array_push(ret.data, fmt.data[j++]);
        }
    }

    ret.len = ns_array_length(ret.data);
    ns_value ret_v = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, ns_str_unescape(ret))};
    return ns_return_ok(value, ret_v);
}

ns_return_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_eval_expr(vm, ctx, n->expr.body);
    case NS_AST_CALL_EXPR: return ns_eval_call_expr(vm, ctx, i);
    case NS_AST_BINARY_EXPR: return ns_eval_binary_expr(vm, ctx, i);
    case NS_AST_STR_FMT_EXPR: return ns_eval_str_fmt_expr(vm, ctx, i);
    case NS_AST_PRIMARY_EXPR: return ns_eval_primary_expr(vm, ctx, i);
    case NS_AST_DESIG_EXPR: return ns_eval_desig_expr(vm, ctx, i);
    case NS_AST_CAST_EXPR: return ns_eval_cast_expr(vm, ctx, i);
    case NS_AST_MEMBER_EXPR: return ns_eval_member_expr(vm, ctx, i);
    case NS_AST_UNARY_EXPR: return ns_eval_unary_expr(vm, ctx, i);
    case NS_AST_ARRAY_EXPR: return ns_eval_array_expr(vm, ctx, i);
    case NS_AST_INDEX_EXPR: return ns_eval_index_expr(vm, ctx, i);
    default:
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented expr type.");
    }

    return ns_return_ok(value, ns_nil);
}

ns_return_void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *expr = n;

    i8 stack_depth = vm->stack_depth;
    if (vm->stack_depth > NS_MAX_STACK_DEPTH) {
        return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "stack overflow.");
    }
    vm->stack_depth++;

    for (i32 e_i = 0, l = n->compound_stmt.count; e_i < l; e_i++) {
        i32 next = expr->next;
        expr = &ctx->nodes[expr->next];
        switch (expr->type) {
        case NS_AST_CALL_EXPR:
        case NS_AST_BINARY_EXPR:
        case NS_AST_PRIMARY_EXPR:
        case NS_AST_MEMBER_EXPR:
        case NS_AST_GEN_EXPR:
        case NS_AST_DESIG_EXPR:
        case NS_AST_UNARY_EXPR: {
            ns_return_value ret = ns_eval_expr(vm, ctx, next);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
        } break;
        case NS_AST_VAR_DEF: {
            ns_return_value ret = ns_eval_local_var_def(vm, ctx, next);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
        } break;
        case NS_AST_JUMP_STMT: { 
            ns_return_void ret = ns_eval_jump_stmt(vm, ctx, next);
            if (ns_return_is_error(ret)) return ret;
        } break;
        case NS_AST_FOR_STMT: { 
            ns_return_void ret = ns_eval_for_stmt(vm, ctx, next);
            if (ns_return_is_error(ret)) return ret;
        } break;
        case NS_AST_IF_STMT: { 
            ns_return_void ret = ns_eval_if_stmt(vm, ctx, next);
            if (ns_return_is_error(ret)) return ret;
        } break;
        default: {
            return ns_return_error(void, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unimplemented stmt type.");
        } break;
        }

        if (ns_array_length(vm->call_stack) > 0) {
            ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
            if (call->ret_set) {
                return ns_return_ok_void;
            }
        }
    }
    vm->stack_depth = stack_depth;
    return ns_return_ok_void;
}

ns_return_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_str st_name = n->desig_expr.name.val;
    ns_symbol *st = ns_vm_find_symbol(vm, st_name);
    
    if (ns_null == st) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown struct.");

    i32 stride = st->st.stride;
    i32 o = ns_eval_alloc(vm, stride);
    i8* data = &vm->stack[o];
    memset(data, 0, stride);

    u64 offset = o + stride;

    ns_ast_t *expr = n;
    for (i32 e_i = 0, l = n->desig_expr.count; e_i < l; e_i++) {
        expr = &ctx->nodes[expr->next];
        ns_str name = expr->field_def.name.val;

        ns_struct_field *field = ns_null;
        for (i32 j = 0, jl = ns_array_length(st->st.fields); j < jl; ++j) {
            field = &st->st.fields[j];
            if (ns_str_equals(field->name, name)) {
                break;
            }
        }

        if (field == ns_null) {
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unknown field.");
        }

        ns_return_value ret_val = ns_eval_expr(vm, ctx, expr->field_def.expr);
        if (ns_return_is_error(ret_val)) return ret_val;
        ns_value val = ret_val.r;

        if (!ns_type_equals(field->t, val.t)) { // type mismatch
            // ns_str f_type = ns_vm_get_type_name(vm, field->t);
            // ns_str v_type = ns_vm_get_type_name(vm, val.t);
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "field type mismatch");
        }

        ns_type t = field->t;
        if (ns_type_is_array(t)) {
            *(u64*)(data + field->o) = val.o;
        } else if (ns_type_is_number(t)) {
            switch (t.type) {
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
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unimplemented string field.");
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unimplemented field type.");
        }
    }

    ns_array_set_length(vm->stack, offset);
    ns_value ret = (ns_value){.t = ns_type_set_store(st->val.t, NS_STORE_STACK), .o = o};
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type t = ns_vm_parse_type(vm, ctx, i, n->cast_expr.type, false).r;

    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->cast_expr.expr);
    if (ns_return_is_error(ret_v)) return ret_v;
    ns_value v = ret_v.r;

    if (ns_type_equals(t, v.t)) return ns_return_ok(value, v);
    if (ns_type_is_number(t) && ns_type_is_number(v.t)) {
        if (ns_type_is_float(t)) {
            if (ns_type_is_float(v.t)) {
                if (ns_type_is(t, NS_TYPE_F32)) {
                    ns_value val = (ns_value){.t = t, .f32 = (f32)ns_eval_number_f64(vm, v)};
                    return ns_return_ok(value, val);
                } else {
                    ns_value val = (ns_value){.t = t, .f64 = ns_eval_number_f64(vm, v)};
                    return ns_return_ok(value, val);
                }
            } else {
                if (ns_type_is(t, NS_TYPE_F32)) {
                    ns_value val = (ns_value){.t = t, .f32 = (f32)ns_eval_number_i64(vm, v)};
                    return ns_return_ok(value, val);
                } else {
                    ns_value val = (ns_value){.t = t, .f64 = (f64)ns_eval_number_i64(vm, v)};
                    return ns_return_ok(value, val);
                }
            }
        } else {
            if (ns_type_is_float(v.t)) {
                ns_value val = (ns_value){.t = t, .i64 = (i64)ns_eval_number_f64(vm, v)};
                return ns_return_ok(value, val);
            } else {
                ns_value val = (ns_value){.t = t, .i64 = ns_eval_number_i64(vm, v)};
                return ns_return_ok(value, val);
            }
        }
    } else {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented cast expr.");
    }

    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_member_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_st = ns_eval_expr(vm, ctx, n->member_expr.left);
    if (ns_return_is_error(ret_st)) return ret_st;
    ns_value st = ret_st.r;

    ns_ast_t *field = &ctx->nodes[n->next];
    if (field->type == NS_AST_MEMBER_EXPR) {
        return ns_eval_member_expr(vm, ctx, n->next);
    }

    // primary expr
    ns_str name = field->primary_expr.token.val;
    ns_symbol *st_type = &vm->symbols[ns_type_index(st.t)];
    if (st_type->type != NS_SYMBOL_STRUCT) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown struct.");
    }

    for (i32 f_i = 0, l = ns_array_length(st_type->st.fields); f_i < l; ++f_i) {
        ns_struct_field *field = &st_type->st.fields[f_i];
        if (ns_str_equals(field->name, name)) {
            if(ns_type_in_heap(st.t)) {
                ns_value val = (ns_value){.t = ns_type_set_store(field->t, NS_STORE_HEAP), .o = st.o + field->o};
                return ns_return_ok(value, val);
            } else {
                ns_value val = (ns_value){.t = ns_type_set_store(field->t, NS_STORE_STACK), .o = st.o + field->o};
                return ns_return_ok(value, val);
            }
        }
    }

    return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown struct field.");
}

ns_return_value ns_eval_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->unary_expr.expr);
    if (ns_return_is_error(ret_v)) return ret_v;
    ns_value v = ret_v.r;

    ns_token_t op = n->unary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        if (!ns_type_is_number(v.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
        if (ns_str_equals_STR(op.val, "-")) {
            switch (v.t.type) {
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
            return ns_return_ok(value, v);
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
        return ns_return_ok(value, v);
    case NS_TOKEN_BIT_INVERT_OP:
        if (!ns_type_is(v.t, NS_TYPE_BOOL)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
        if (ns_str_equals_STR(op.val, "!")) {
            ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, NS_STORE_CONST)};
            switch (v.t.type) {
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
            return ns_return_ok(value, ret);
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
    default:
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown unary ops.");
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *t = &ctx->nodes[n->array_expr.type];

    ns_type type = ns_vm_parse_type(vm, ctx, i, t->type_label.name, false).r;
    ns_return_value ret_count = ns_eval_expr(vm, ctx, n->array_expr.count);
    if (ns_return_is_error(ret_count)) return ret_count;

    ns_value count = ret_count.r;
    i32 element_count = ns_eval_number_i32(vm, count);
    i32 size = ns_type_size(vm, type) * element_count;
    i8* data = NULL;
    ns_array_set_capacity(data, size);
    memset(data, 0, size);
    ns_value v = (ns_value){.t = {.type = type.type, .array = true, .ref = type.ref, .index = type.index, .store = NS_STORE_HEAP }, .o = (u64)data};
    return ns_return_ok(value, v);
}

ns_return_value ns_eval_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_table = ns_eval_expr(vm, ctx, n->index_expr.table);
    if (ns_return_is_error(ret_table)) return ret_table;
    ns_value table = ret_table.r;

    ns_return_value ret_index = ns_eval_expr(vm, ctx, n->index_expr.expr);
    if (ns_return_is_error(ret_index)) return ret_index;
    ns_value index = ret_index.r;

    if (!ns_type_is_number(index.t)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "index expr type mismatch.");
    }
    if (!ns_type_is_array(table.t)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "index expr type mismatch.");
    }
    ns_type element_type = table.t;
    element_type.array = false;

    u64 offset = ns_type_size(vm, table.t) * (u64)ns_eval_number_i32(vm, index);
    u8* data = NULL;
    if (ns_type_in_stack(table.t)) {
        data = (u8*)(*(u64*)(vm->stack + table.o)) + offset;
    } else {
        data = (u8*)table.o + offset;
    }
    ns_value val = (ns_value){.t = ns_type_set_store(element_type, NS_STORE_HEAP), .o = (u64)data};
    return ns_return_ok(value, val);
}

ns_return_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *val = ns_vm_find_symbol(vm, n->var_def.name.val);

    // eval & store value
    i32 size = n->var_def.type_size;
    if (size < 0 && val->val.t.type != NS_TYPE_INFER) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown type size.");
    }

    u64 offset = ns_eval_alloc(vm, size);
    ns_value ret = (ns_value){.o = offset};
    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->var_def.expr);
    if (ns_return_is_error(ret_v)) return ret_v;

    ns_value v = ret_v.r;
    ret.t = ns_type_set_store(v.t, NS_STORE_STACK);
    ns_eval_copy(vm, ret, v, size);
    ns_array_set_length(vm->stack, offset + size);
    val->val = ret;
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_vm_inject_hook(vm, ctx, i);

    ns_ast_t *n = &ctx->nodes[i];
    if (vm->step_hook) vm->step_hook(vm, ctx, i);

    // eval & store value
    i32 size = n->var_def.type_size;
    if (size < 0) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown type size.");
    }

    u64 offset = ns_eval_alloc(vm, size);
    ns_value ret = (ns_value){.o = offset};
    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->var_def.expr);
    if (ns_return_is_error(ret_v)) return ret_v;

    ns_value v = ret_v.r;
    ret.t = ns_type_set_store(v.t, NS_STORE_STACK);
    ns_eval_copy(vm, ret, v, size);
    ns_array_set_length(vm->stack, offset + size);

    ns_symbol symbol = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = n->var_def.name.val, .val = ret, .parsed = true};
    ns_array_push(vm->symbol_stack, symbol);
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_ast(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_return_bool ret_parse = ns_vm_parse(vm, ctx);
    if (ns_return_is_error(ret_parse)) return ns_return_change_type(value, ret_parse);

    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol r = vm->symbols[i];
        if (r.type != NS_SYMBOL_VALUE)
            continue;
        ns_value v = r.val;
        if (ns_type_is_const(r.val.t)) {
            ns_symbol *record = &vm->symbols[i];
            record->val = v;
        }
    }

    ns_value ret = ns_nil;
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        switch (n->type) {
        case NS_AST_EXPR:
        case NS_AST_CALL_EXPR: {
            ns_return_value ret = ns_eval_expr(vm, ctx, s_i);
            if (ns_return_is_error(ret)) return ret;
        } break;
        case NS_AST_VAR_DEF: {
            ns_return_value ret = ns_eval_var_def(vm, ctx, s_i);
            if (ns_return_is_error(ret)) return ret;
        } break;
        case NS_AST_IMPORT_STMT:
        case NS_AST_MODULE_STMT:
        case NS_AST_FN_DEF:
        case NS_AST_OPS_FN_DEF:
        case NS_AST_STRUCT_DEF:
            break; // already parsed
        default: {
            ns_code_loc loc = (ns_code_loc){.f = ctx->filename, .l = n->state.l, .o = n->state.o};
            return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented global ast.");
        } break;
        }
    }

    ns_symbol* main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main"));
    if (ns_null != main_fn) {
        ns_call call = (ns_call){.fn = main_fn, .scope_top = ns_array_length(vm->scope_stack), .ret_set = false };

        ns_enter_scope(vm);
        ns_array_push(vm->call_stack, call);
        ns_ast_t *fn = &ctx->nodes[main_fn->fn.ast];
        ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn->fn_def.body);
        if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);

        ns_array_pop(vm->call_stack);
        ns_exit_scope(vm);

        return ns_return_ok(value, ns_nil);
    }

    return ns_return_ok(value, ret);
}

ns_return_value ns_eval(ns_vm *vm, ns_str source, ns_str filename) {
    ns_ast_ctx ctx = {0};
    ns_return_bool ret_ast = ns_ast_parse(&ctx, source, filename);
    if (ns_return_is_error(ret_ast)) return ns_return_change_type(value, ret_ast);

    return ns_eval_ast(vm, &ctx);
}
