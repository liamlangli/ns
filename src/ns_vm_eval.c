#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_shader.h"

/**
 * value transfer semantics
 * 1. dst can not be const. no const ref
 * if dst is value semantics, copy value
 * if dst is reference semantics, return src
 */

#define ns_eval_stmt_case(fn) \
    { \
        ns_return_void ret = fn(vm, ctx, next); \
        if (ns_return_is_error(ret)) return ret; \
    } break

ns_return_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_index_expr_with_create(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_bool create);
ns_return_void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

typedef struct ns_dict_table {
    i32 cap;
    i32 len;
    i32 key_size;
    i32 val_size;
    i32 key_off;
    i32 val_off;
    i32 stride;
    ns_type key_type;
    ns_type val_type;
    u8 data[];
} ns_dict_table;

static void *ns_eval_handle_ptr(ns_vm *vm, ns_value v) {
    return ns_type_in_stack(v.t) ? (void *)(*(u64 *)&vm->stack[v.o]) : (void *)v.o;
}

static ns_dict_table *ns_eval_dict_table(ns_vm *vm, ns_value v) {
    return (ns_dict_table *)ns_eval_handle_ptr(vm, v);
}

static void *ns_eval_array_data(ns_vm *vm, ns_value v) {
    return ns_eval_handle_ptr(vm, v);
}

ns_bool ns_eval_number_assign_compatible(ns_vm *vm, ns_type dst, ns_type src) {
    if (!ns_type_is_number(dst) || !ns_type_is_number(src)) return false;
    if (ns_type_is_float(src) && !ns_type_is_float(dst)) return false;

    i32 dst_size = ns_type_size(vm, dst);
    i32 src_size = ns_type_size(vm, src);
    if (dst_size < src_size) return false;
    return true;
}

static i64 ns_eval_to_i64(ns_vm *vm, ns_value v) {
    switch (v.t.type) {
    case NS_TYPE_I8: return ns_eval_number_i8(vm, v);
    case NS_TYPE_I16: return ns_eval_number_i16(vm, v);
    case NS_TYPE_I32: return ns_eval_number_i32(vm, v);
    case NS_TYPE_I64: return ns_eval_number_i64(vm, v);
    case NS_TYPE_U8: return (i64)ns_eval_number_u8(vm, v);
    case NS_TYPE_U16: return (i64)ns_eval_number_u16(vm, v);
    case NS_TYPE_U32: return (i64)ns_eval_number_u32(vm, v);
    case NS_TYPE_U64: return (i64)ns_eval_number_u64(vm, v);
    case NS_TYPE_F32: return (i64)ns_eval_number_f32(vm, v);
    case NS_TYPE_F64: return (i64)ns_eval_number_f64(vm, v);
    case NS_TYPE_BOOL: return ns_eval_bool(vm, v) ? 1 : 0;
    default: return 0;
    }
}

static u64 ns_eval_to_u64(ns_vm *vm, ns_value v) {
    switch (v.t.type) {
    case NS_TYPE_I8: return (u64)ns_eval_number_i8(vm, v);
    case NS_TYPE_I16: return (u64)ns_eval_number_i16(vm, v);
    case NS_TYPE_I32: return (u64)ns_eval_number_i32(vm, v);
    case NS_TYPE_I64: return (u64)ns_eval_number_i64(vm, v);
    case NS_TYPE_U8: return (u64)ns_eval_number_u8(vm, v);
    case NS_TYPE_U16: return (u64)ns_eval_number_u16(vm, v);
    case NS_TYPE_U32: return (u64)ns_eval_number_u32(vm, v);
    case NS_TYPE_U64: return (u64)ns_eval_number_u64(vm, v);
    case NS_TYPE_F32: return (u64)ns_eval_number_f32(vm, v);
    case NS_TYPE_F64: return (u64)ns_eval_number_f64(vm, v);
    case NS_TYPE_BOOL: return ns_eval_bool(vm, v) ? 1 : 0;
    default: return 0;
    }
}

static f64 ns_eval_to_f64(ns_vm *vm, ns_value v) {
    switch (v.t.type) {
    case NS_TYPE_I8: return (f64)ns_eval_number_i8(vm, v);
    case NS_TYPE_I16: return (f64)ns_eval_number_i16(vm, v);
    case NS_TYPE_I32: return (f64)ns_eval_number_i32(vm, v);
    case NS_TYPE_I64: return (f64)ns_eval_number_i64(vm, v);
    case NS_TYPE_U8: return (f64)ns_eval_number_u8(vm, v);
    case NS_TYPE_U16: return (f64)ns_eval_number_u16(vm, v);
    case NS_TYPE_U32: return (f64)ns_eval_number_u32(vm, v);
    case NS_TYPE_U64: return (f64)ns_eval_number_u64(vm, v);
    case NS_TYPE_F32: return (f64)ns_eval_number_f32(vm, v);
    case NS_TYPE_F64: return ns_eval_number_f64(vm, v);
    case NS_TYPE_BOOL: return ns_eval_bool(vm, v) ? 1.0 : 0.0;
    default: return 0.0;
    }
}

static u64 ns_hash_bytes(const u8 *data, i32 len) {
    u64 h = 1469598103934665603ULL;
    for (i32 i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static u64 ns_eval_hash_value(ns_vm *vm, ns_value v) {
    if (ns_type_is(v.t, NS_TYPE_STRING)) {
        ns_str s = ns_eval_str(vm, v);
        return ns_hash_bytes((u8 *)s.data, s.len);
    }
    if (ns_type_is_number(v.t)) {
        u64 n = ns_eval_to_u64(vm, v);
        n ^= n >> 33;
        n *= 0xff51afd7ed558ccdULL;
        n ^= n >> 33;
        n *= 0xc4ceb9fe1a85ec53ULL;
        n ^= n >> 33;
        return n;
    }
    return 0;
}

static ns_bool ns_eval_value_equals(ns_vm *vm, ns_value a, ns_value b) {
    if (ns_type_is(a.t, NS_TYPE_STRING) && ns_type_is(b.t, NS_TYPE_STRING)) {
        return ns_str_equals(ns_eval_str(vm, a), ns_eval_str(vm, b));
    }
    if (ns_type_is_number(a.t) && ns_type_is_number(b.t)) {
        if (ns_type_is_float(a.t) || ns_type_is_float(b.t)) return ns_eval_to_f64(vm, a) == ns_eval_to_f64(vm, b);
        return ns_eval_to_u64(vm, a) == ns_eval_to_u64(vm, b);
    }
    return false;
}

static ns_dict_table *ns_eval_dict_alloc(ns_vm *vm, ns_type t, i32 cap) {
    ns_symbol *ct = &vm->symbols[ns_type_index(t)];
    ns_type key_type = ct->ct.key;
    ns_type val_type = ct->ct.val;
    i32 key_size = ns_type_size(vm, key_type);
    i32 val_size = ns_type_is(t, NS_TYPE_SET) ? 0 : ns_type_size(vm, val_type);
    if (ns_type_is(key_type, NS_TYPE_STRING)) key_size = (i32)sizeof(u64);
    if (ns_type_is(val_type, NS_TYPE_STRING) && !ns_type_is(t, NS_TYPE_SET)) val_size = (i32)sizeof(u64);

    i32 key_off = (i32)ns_align(1, key_size);
    i32 val_off = ns_type_is(t, NS_TYPE_SET) ? 0 : (i32)ns_align(key_off + key_size, val_size);
    i32 stride = ns_type_is(t, NS_TYPE_SET) ? key_off + key_size : val_off + val_size;
    cap = ns_max(1, cap);
    ns_dict_table *table = ns_malloc(sizeof(ns_dict_table) + (szt)cap * (szt)stride);
    memset(table, 0, sizeof(ns_dict_table) + (szt)cap * (szt)stride);
    table->cap = cap;
    table->key_size = key_size;
    table->val_size = val_size;
    table->key_off = key_off;
    table->val_off = val_off;
    table->stride = stride;
    table->key_type = key_type;
    table->val_type = val_type;
    return table;
}

static u8 *ns_eval_dict_slot(ns_dict_table *table, i32 index) {
    return table->data + (szt)index * (szt)table->stride;
}

static ns_value ns_eval_slot_value(ns_dict_table *table, u8 *ptr, ns_type t) {
    ns_unused(table);
    ns_value v = {.t = ns_type_set_stack(ns_type_set_mut(t, true), false), .o = (u64)ptr};
    if (ns_type_is(t, NS_TYPE_STRING)) v.o = *(u64 *)ptr;
    return v;
}

static ns_value ns_eval_dict_value(ns_dict_table *table, u8 *ptr, ns_bool write_target) {
    ns_value v = {.t = ns_type_set_stack(ns_type_set_mut(table->val_type, true), false), .o = (u64)ptr};
    if (!write_target && ns_type_is(table->val_type, NS_TYPE_STRING)) {
        v.t = ns_type_set_stack(table->val_type, false);
        v.o = *(u64 *)ptr;
    }
    return v;
}

static ns_return_value ns_eval_dict_find(ns_vm *vm, ns_value table_v, ns_value key, ns_bool create, ns_code_loc loc) {
    if (!ns_type_is_number(key.t) && !ns_type_is(key.t, NS_TYPE_STRING)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "dict key type not hashable.");
    }

    ns_dict_table *table = ns_eval_dict_table(vm, table_v);
    u64 h = ns_eval_hash_value(vm, key);
    i32 start = (i32)(h % (u64)table->cap);
    for (i32 step = 0; step < table->cap; ++step) {
        i32 index = (start + step) % table->cap;
        u8 *slot = ns_eval_dict_slot(table, index);
        if (slot[0] == 0) {
            if (!create) break;
            if (table->len >= table->cap) {
                return ns_return_error(value, loc, NS_ERR_EVAL, "dict capacity exceeded.");
            }
            slot[0] = 1;
            table->len++;
            ns_value dst_key = {.t = ns_type_set_stack(ns_type_set_mut(table->key_type, true), false), .o = (u64)(slot + table->key_off)};
            ns_eval_copy(vm, dst_key, key, table->key_size);
            return ns_return_ok(value, ns_eval_dict_value(table, slot + table->val_off, create));
        }

        ns_value slot_key = ns_eval_slot_value(table, slot + table->key_off, table->key_type);
        if (ns_eval_value_equals(vm, slot_key, key)) {
            return ns_return_ok(value, ns_eval_dict_value(table, slot + table->val_off, create));
        }
    }
    return ns_return_error(value, loc, NS_ERR_EVAL, "dict key not found.");
}

ns_return_value ns_eval_cast_number(ns_vm *vm, ns_value v, ns_type dst, ns_code_loc loc) {
    if (ns_type_equals(v.t, dst)) return ns_return_ok(value, v);
    if (!ns_type_is_number(v.t) || !ns_type_is_number(dst)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "numeric cast type mismatch.");
    }

    ns_value out = {.t = ns_type_set_mut(dst, false)};
    if (ns_type_is_float(dst)) {
        if (ns_type_is(dst, NS_TYPE_F32)) {
            out.f32 = (f32)ns_eval_to_f64(vm, v);
        } else {
            out.f64 = ns_eval_to_f64(vm, v);
        }
        return ns_return_ok(value, out);
    }

    if (ns_type_unsigned(dst)) {
        u64 n = ns_eval_to_u64(vm, v);
        switch (dst.type) {
        case NS_TYPE_U8: out.u8 = (u8)n; break;
        case NS_TYPE_U16: out.u16 = (u16)n; break;
        case NS_TYPE_U32: out.u32 = (u32)n; break;
        case NS_TYPE_U64: out.u64 = (u64)n; break;
        default: return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented numeric cast type.");
        }
    } else {
        i64 n = ns_eval_to_i64(vm, v);
        switch (dst.type) {
        case NS_TYPE_I8: out.i8 = (i8)n; break;
        case NS_TYPE_I16: out.i16 = (i16)n; break;
        case NS_TYPE_I32: out.i32 = (i32)n; break;
        case NS_TYPE_I64: out.i64 = (i64)n; break;
        case NS_TYPE_BOOL: out.b = n != 0; break;
        default: return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented numeric cast type.");
        }
    }
    return ns_return_ok(value, out);
}

u64 ns_eval_alloc(ns_vm *vm, i32 stride) {
    u64 offset = ns_array_length(vm->stack);
    if (stride <= 0) return offset; // nothing to reserve (e.g. void/unknown type)
    u64 align = ns_min(sizeof(void*), (u64)stride);
    if (align > 0) offset = (offset + (align - 1)) & ~(align - 1);
    ns_array_set_length(vm->stack, offset + (u64)stride);
    return offset; // leading 4 bytes for type size
}

ns_return_value ns_eval_copy(ns_vm *vm, ns_value dst, ns_value src, i32 size) {
    if (ns_type_is_const(dst.t)) {
        return ns_return_error(value, vm->loc, NS_ERR_EVAL, "cannot assign to const.");
    }

    if (ns_type_is_ref(dst.t)) return ns_return_ok(value, src); // ref semantics, return src

    // A value's .o is interpreted relative to vm->stack when its `stack` flag is
    // set, and as an absolute address otherwise. Honor that flag for the
    // destination so heap-backed locations (e.g. array elements) work too.
    i8 *dptr = ns_type_in_stack(dst.t) ? &vm->stack[dst.o] : (i8 *)dst.o;

    // Arrays, dicts, sets, strings and fns are reference values stored as a single u64 handle
    // (a heap pointer or str_list index). Their value type carries mut=1, so they
    // never reach the const switch below; handle them uniformly here.
    if (ns_type_is_array(src.t) || ns_type_is(src.t, NS_TYPE_STRING) || ns_type_is(src.t, NS_TYPE_FN) ||
        ns_type_is(src.t, NS_TYPE_DICT) || ns_type_is(src.t, NS_TYPE_SET)) {
        u64 handle = ns_type_in_stack(src.t) ? *(u64 *)&vm->stack[src.o] : src.o;
        *(u64 *)dptr = handle;
        return ns_return_ok(value, dst);
    }

    if (ns_type_is_const(src.t)) {
        switch (src.t.type)
        {
        case NS_TYPE_I8: *(i8*)dptr = src.i8; break;
        case NS_TYPE_I16: *(i16*)dptr = src.i16; break;
        case NS_TYPE_I32: *(i32*)dptr = src.i32; break;
        case NS_TYPE_I64: *(i64*)dptr = src.i64; break;
        case NS_TYPE_U8: *(u8*)dptr = src.u8; break;
        case NS_TYPE_U16: *(u16*)dptr = src.u16; break;
        case NS_TYPE_U32: *(u32*)dptr = src.u32; break;
        case NS_TYPE_U64: *(u64*)dptr = src.u64; break;
        case NS_TYPE_F32: *(f32*)dptr = src.f32; break;
        case NS_TYPE_F64: *(f64*)dptr = src.f64; break;
        case NS_TYPE_BOOL: *(ns_bool*)dptr = src.b; break;
        case NS_TYPE_STRING: *(u64*)dptr = src.o; break;
        case NS_TYPE_FN: *(u64*)dptr = src.o; break;
        case NS_TYPE_STRUCT: memcpy(dptr, &vm->stack[src.o], size); break;
        case NS_TYPE_BLOCK: {
            ns_error("eval error", "can't copy block type.");
        } break;
        default: return ns_return_error(value, ns_code_loc_nil, NS_ERR_EVAL, "invalid const type.");
        }
    } else if (ns_type_in_stack(src.t)) {
        memcpy(dptr, &vm->stack[src.o], size);
    } else {
        memcpy(dptr, (void*)src.o, size);
    }
    return ns_return_ok(value, dst);
}

ns_value ns_eval_find_value(ns_vm *vm, ns_str name) {
    ns_symbol *s = ns_vm_find_symbol(vm, name, false);
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

ns_scope* ns_scope_enter(ns_vm *vm) {
    ns_scope scope = (ns_scope){.stack_top = ns_array_length(vm->stack), .symbol_top = ns_array_length(vm->symbol_stack)};
    ns_array_push(vm->scope_stack, scope);
    return ns_array_last(vm->scope_stack);
}

ns_scope* ns_scope_exit(ns_vm *vm) {
    ns_scope *scope = ns_array_last(vm->scope_stack);
    ns_array_set_length(vm->stack, scope->stack_top);
    ns_array_set_length(vm->symbol_stack, scope->symbol_top);
    ns_array_pop(vm->scope_stack);
    return scope;
}

ns_return_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_l;
    if (ctx->nodes[n->binary_expr.left].type == NS_AST_INDEX_EXPR) {
        ns_ast_t *idx = &ctx->nodes[n->binary_expr.left];
        ns_return_value ret_table = ns_eval_expr(vm, ctx, idx->index_expr.table);
        if (ns_return_is_error(ret_table)) return ns_return_change_type(value, ret_table);
        if (ns_type_is(ret_table.r.t, NS_TYPE_DICT)) {
            ret_l = ns_eval_index_expr_with_create(vm, ctx, n->binary_expr.left, true);
        } else {
            ret_l = ns_eval_expr(vm, ctx, n->binary_expr.left);
        }
    } else {
        ret_l = ns_eval_expr(vm, ctx, n->binary_expr.left);
    }
    if (ns_return_is_error(ret_l)) return ns_return_change_type(value, ret_l);
    ns_value l = ret_l.r;

    ns_return_value ret_r = ns_eval_expr(vm, ctx, n->binary_expr.right);
    if (ns_return_is_error(ret_r)) return ns_return_change_type(value, ret_r);
    ns_value r = ret_r.r;

    if (!ns_type_equals(l.t, r.t) && ns_type_is(l.t, NS_TYPE_INFER))
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid assign expr.");

    if (ns_type_is_const(l.t))
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "can't assign to const value.");

    if (!ns_type_equals(l.t, r.t)) {
        if (!ns_eval_number_assign_compatible(vm, l.t, r.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "assign expr type mismatch.");
        }
        ns_return_value cast_ret = ns_eval_cast_number(vm, r, l.t, ns_ast_state_loc(ctx, n->state));
        if (ns_return_is_error(cast_ret)) return cast_ret;
        r = cast_ret.r;
    }

    i32 s_l = ns_type_size(vm, l.t);
    ns_eval_copy(vm, l, r, s_l);
    return ns_return_ok(value, l);
}

ns_return_value ns_eval_binary_override(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t op = n->binary_expr.op;
    ns_str l_name = ns_vm_get_type_name(vm, l.t);
    ns_str r_name = ns_vm_get_type_name(vm, r.t);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name, false);
    if (!fn) return ns_return_ok(value, (ns_value){.t = ns_type_unknown});

    i32 ret_size = ns_type_size(vm, fn->fn.ret);
    if (ret_size < 0) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid override fn.");

    u64 ret_offset = ns_eval_alloc(vm, ret_size);
    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->fn.ret, true), .o = ret_offset};
    ns_call call = (ns_call){.callee = fn, .ret = ret_val, .ret_set = false, .scope_top = ns_array_length(vm->scope_stack), .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = 2};
    ns_scope_enter(vm);
    ns_array_push(vm->call_stack, call);

    ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[0].name, .val = l};
    ns_array_push(vm->symbol_stack, l_arg);
    ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[1].name, .val = r};
    ns_array_push(vm->symbol_stack, r_arg);

    ns_ast_t *fn_ast = &ctx->nodes[fn->fn.ast];
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn_ast->ops_fn_def.body);
    if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);

    return ns_return_ok(value, call.ret);
}

#define ns_eval_number_fn(type) \
type ns_eval_number_##type(ns_vm *vm, ns_value n) { return ns_type_is_const(n.t) ? n.type : *(type*)(ns_type_in_stack(n.t) ? &vm->stack[n.o] : (void*)n.o); }

ns_eval_number_fn(i8)
ns_eval_number_fn(u8)
ns_eval_number_fn(i16)
ns_eval_number_fn(i32)
ns_eval_number_fn(i64)
ns_eval_number_fn(u16)
ns_eval_number_fn(u32)
ns_eval_number_fn(u64)
ns_eval_number_fn(f32)
ns_eval_number_fn(f64)

ns_bool ns_eval_bool(ns_vm *vm, ns_value n) { return ns_eval_number_i32(vm, n) != 0; }
ns_str ns_eval_str(ns_vm *vm, ns_value n) {
    // String values always carry a str_list index in `.o` (string literals are
    // mut=1 but still index-based), or, when held in a stack slot, the slot
    // stores that index. There is no raw ns_str* representation.
    if (ns_type_in_stack(n.t)) return vm->str_list[*(u64*)&vm->stack[n.o]];
    return vm->str_list[n.o];
}

void *ns_eval_array_raw(ns_vm *vm, ns_value n) {
    return ns_eval_array_data(vm, n);
}

#define ns_eval_number_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) {\
    ns_value ret = (ns_value){.t = ns_type_set_mut(l.t, false) };\
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
        case NS_TYPE_BOOL: ret.b = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        default: break;\
    }\
    return ret;\
}

#define ns_eval_number_cmp_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, false, true) };\
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
        case NS_TYPE_BOOL: ret.b = ns_eval_number_i32(vm, l) op ns_eval_number_i32(vm, r); break;\
        default: break;\
    }\
    return ret;\
}

#define ns_eval_number_shift_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_set_mut(l.t, false) };\
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

// Integer-only bitwise ops. Result is an immediate constant (mut=false), the
// same representation arithmetic ops use, so it reads back correctly.
#define ns_eval_number_bit_op(fn, op) \
ns_value ns_eval_binary##fn(ns_vm *vm, ns_value l, ns_value r) { \
    ns_value ret = (ns_value){.t = ns_type_set_mut(l.t, false) };\
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
ns_eval_number_bit_op(_band, &)
ns_eval_number_bit_op(_bor, |)
ns_eval_number_bit_op(_bxor, ^)

ns_value ns_eval_number_mod(ns_vm *vm, ns_value l, ns_value r) {
    ns_value ret = (ns_value){.t = ns_type_set_mut(l.t, false)};
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
    case NS_TOKEN_BITWISE_OP: {
        if (ns_type_is_float(l.t)) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "bitwise operator not supported for float type.");
        if (ns_str_equals(op.val, ns_str_cstr("&")))
            return ns_return_ok(value, ns_eval_binary_band(vm, l, r));
        else if (ns_str_equals(op.val, ns_str_cstr("|")))
            return ns_return_ok(value, ns_eval_binary_bor(vm, l, r));
        else if (ns_str_equals(op.val, ns_str_cstr("^")))
            return ns_return_ok(value, ns_eval_binary_bxor(vm, l, r));
    } break;
    case NS_TOKEN_REL_OP: {
        // Use exact matching: prefix matching would let "<=" be caught by "<".
        if (ns_str_equals(op.val, ns_str_cstr("<=")))
            return ns_return_ok(value, ns_eval_binary_le(vm, l, r));
        else if (ns_str_equals(op.val, ns_str_cstr("<")))
            return ns_return_ok(value, ns_eval_binary_lt(vm, l, r));
        else if (ns_str_equals(op.val, ns_str_cstr(">=")))
            return ns_return_ok(value, ns_eval_binary_ge(vm, l, r));
        else if (ns_str_equals(op.val, ns_str_cstr(">")))
            return ns_return_ok(value, ns_eval_binary_gt(vm, l, r));
    } break;
    case NS_TOKEN_EQ_OP: {
        if (ns_str_equals_STR(op.val, "=="))
            return ns_return_ok(value, ns_eval_binary_eq(vm, l, r));
        else if (ns_str_equals_STR(op.val, "!="))
            return ns_return_ok(value, ns_eval_binary_ne(vm, l, r));
        else if (ns_str_equals_STR(op.val, "==="))
            return ns_return_ok(value, ns_eval_binary_eq(vm, l, r));
        else if (ns_str_equals_STR(op.val, "!=="))
            return ns_return_ok(value, ns_eval_binary_ne(vm, l, r));
    } break;
    default:
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented binary ops.");
        break;
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_binary_number_upgrade(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_type t = ns_vm_number_type_upgrade(l.t, r.t);
    if (ns_type_is_unknown(t)) return ns_return_ok(value, (ns_value){.t = ns_type_unknown});

    ns_code_loc loc = ns_ast_state_loc(ctx, ctx->nodes[i].state);
    ns_return_value ret_l = ns_eval_cast_number(vm, l, t, loc);
    if (ns_return_is_error(ret_l)) return ret_l;
    ns_return_value ret_r = ns_eval_cast_number(vm, r, t, loc);
    if (ns_return_is_error(ret_r)) return ret_r;
    return ns_eval_binary_ops_number(vm, ctx, ret_l.r, ret_r.r, i);
}

ns_return_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];

    // Closure registration into a fn-typed struct field: `v.on_frame({ ... })`.
    // Mirrors the type-check branch in ns_vm_parse_call_expr: evaluate the callee
    // member as an lvalue (the field slot), evaluate the closure, and store it
    // into the field rather than invoking the field's function.
    ns_ast_t *callee_n = &ctx->nodes[n->call_expr.callee];
    ns_ast_t *eval_arg0 = &ctx->nodes[n->next];
    i32 eval_arg0_body = eval_arg0->type == NS_AST_EXPR ? eval_arg0->expr.body : n->next;
    if (callee_n->type == NS_AST_MEMBER_EXPR && n->call_expr.arg_count == 1 &&
        ctx->nodes[eval_arg0_body].type == NS_AST_BLOCK_EXPR) {
        ns_return_value ret_field = ns_eval_expr(vm, ctx, n->call_expr.callee);
        if (ns_return_is_error(ret_field)) return ret_field;
        ns_return_value ret_cb = ns_eval_expr(vm, ctx, n->next);
        if (ns_return_is_error(ret_cb)) return ret_cb;
        // The field is a C function-pointer slot read by native code, so store a
        // trampoline that re-enters the interpreter rather than the raw closure
        // value (which would be invoked as a bogus address).
        void *trampoline = ns_callback_bridge_create(vm, ctx, ret_cb.r);
        if (!trampoline) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "failed to create callback bridge.");
        }
        ns_value dst = ret_field.r;
        i8 *dptr = ns_type_in_stack(dst.t) ? &vm->stack[dst.o] : (i8 *)dst.o;
        *(void **)dptr = trampoline;
        return ns_return_ok(value, ns_nil);
    }

    ns_return_value ret_callee = ns_eval_expr(vm, ctx, n->call_expr.callee);
    if (ns_return_is_error(ret_callee)) return ret_callee;

    ns_value callee = ret_callee.r;
    if (ns_is_nil(callee)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "nil callee.");
    }

    ns_symbol *sym = &vm->symbols[ns_type_index(callee.t)];
    ns_fn_symbol *fn = ns_symbol_get_fn(sym);

    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->ret, true), .o = 0};
    i32 ret_size = ns_type_size(vm, fn->ret);
    if (ret_size > 0) {
        ret_val.o = ns_eval_alloc(vm, ret_size);
    }
    ns_call call = (ns_call){.callee = sym, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = ns_array_length(fn->args)};

    ns_scope_enter(vm);
    i32 next = n->next;
    i32 arg_base = ns_array_length(vm->symbol_stack);
    i32 arg_count = n->call_expr.arg_count;
    // Evaluate every argument before activating any parameter name. Arguments
    // are pushed under a blank name so a later bare-identifier argument that
    // matches a parameter name still resolves to the caller's binding rather
    // than a not-yet-active parameter of this call (e.g. `f(a + 1.0, a)`).
    for (i32 a_i = 0; a_i < arg_count; ++a_i) {
        ns_return_value ret_v = ns_eval_expr(vm, ctx, next);
        if (ns_return_is_error(ret_v)) return ret_v;

        ns_value v = ret_v.r;
        // a numeric arg converts to the numeric param type (the parse pass
        // already accepted the pair in ns_vm_parse_call_expr)
        if (a_i < (i32)ns_array_length(fn->args)) {
            ns_type pt = fn->args[a_i].val.t;
            if (!ns_type_is_ref(pt) && ns_type_is_number(pt) && ns_type_is_number(v.t) && !ns_type_equals(pt, v.t)) {
                ns_return_value cast_ret = ns_eval_cast_number(vm, v, pt, ns_ast_state_loc(ctx, n->state));
                if (ns_return_is_error(cast_ret)) return cast_ret;
                v = cast_ret.r;
            }
        }
        next = ctx->nodes[next].next;
        ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = ns_str_null, .val = v, .parsed = true};
        ns_array_push(vm->symbol_stack, arg);
    }
    for (i32 a_i = 0; a_i < arg_count; ++a_i) {
        vm->symbol_stack[arg_base + a_i].name = fn->args[a_i].name;
    }

    ns_array_push(vm->call_stack, call);
    if (fn->fn.t.ref) {
        // `mod shader` fns are VM-internal intrinsics (like std) but need the ast
        // ctx to walk fn bodies, so they dispatch here where ctx is in scope.
        ns_return_bool ret = ns_str_equals(sym->lib, ns_str_cstr("shader")) ? ns_shader_vm_call(vm, ctx) : ns_vm_call_ref(vm);
        if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
    } else {
        if (sym->type == NS_SYMBOL_BLOCK) { // push block captured field to stack
            szt field_count = ns_array_length(sym->bc.st.fields);
            u64 o = callee.o;
            for (szt f_i = 0; f_i < field_count; ++f_i) {
                ns_struct_field *field = &sym->bc.st.fields[f_i];
                ns_value v = (ns_value){.t = ns_type_set_stack(field->t, true), .o = o + field->o};
                ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = field->name, .val = v, .parsed = true};
                ns_array_push(vm->symbol_stack, arg);
            }
        }

        ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn->body);
        if (ns_return_is_error(ret)) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "call expr error.");
    }
    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);
    return ns_return_ok(value, call.ret);
}

ns_return_value ns_eval_invoke_callback(ns_vm *vm, ns_ast_ctx *ctx, ns_value closure, void *cap_base, void *arg_ptr) {
    ns_symbol *sym = &vm->symbols[ns_type_index(closure.t)];
    ns_fn_symbol *fn = ns_symbol_get_fn(sym);

    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->ret, true), .o = 0};
    i32 ret_size = ns_type_size(vm, fn->ret);
    if (ret_size > 0) ret_val.o = ns_eval_alloc(vm, ret_size);

    ns_call call = (ns_call){.callee = sym, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = ns_array_length(fn->args)};
    ns_scope_enter(vm);

    // The single closure parameter receives the native pointer as an absolute
    // (non-stack) ref value, the same representation native ref returns use.
    if (ns_array_length(fn->args) > 0) {
        ns_type at = fn->args[0].val.t;
        ns_value av = (ns_value){.t = ns_type_set_stack(at, false), .o = (u64)arg_ptr};
        ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->args[0].name, .val = av, .parsed = true};
        ns_array_push(vm->symbol_stack, arg);
    }

    ns_array_push(vm->call_stack, call);
    if (sym->type == NS_SYMBOL_BLOCK && cap_base) { // push captured fields from the heap snapshot
        szt field_count = ns_array_length(sym->bc.st.fields);
        for (szt f_i = 0; f_i < field_count; ++f_i) {
            ns_struct_field *field = &sym->bc.st.fields[f_i];
            i8 *slot = (i8 *)cap_base + field->o;
            // The snapshot is heap memory, so address fields absolutely. A ref
            // field stores the captured pointer in its slot; dereference it so
            // the value carries the pointer (matching native ref representation).
            ns_value v = ns_type_is_ref(field->t)
                ? (ns_value){.t = ns_type_set_stack(field->t, false), .o = *(u64 *)slot}
                : (ns_value){.t = ns_type_set_stack(field->t, false), .o = (u64)slot};
            ns_symbol cap = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = field->name, .val = v, .parsed = true};
            ns_array_push(vm->symbol_stack, cap);
        }
    }

    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn->body);
    if (ns_return_is_error(ret)) {
        ns_array_pop(vm->call_stack);
        ns_scope_exit(vm);
        return ns_return_change_type(value, ret);
    }
    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);
    return ns_return_ok(value, call.ret);
}

ns_return_void ns_eval_assert_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_cond = ns_eval_expr(vm, ctx, n->assert_stmt.expr);
    if (ns_return_is_error(ret_cond)) return ns_return_change_type(void, ret_cond);

    ns_value cond = ret_cond.r;
    if (!ns_eval_bool(vm, cond)) {
        ns_return_void ret = ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_ASSERTION, "assertion failed.");
        ns_return_assert(ret);
    }
    return ns_return_ok_void;
}

ns_return_value ns_eval_return_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_ret = ns_eval_expr(vm, ctx, n->jump_stmt.expr);
    if (ns_return_is_error(ret_ret)) return ret_ret;

    ns_value ret = ret_ret.r;
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        ns_fn_symbol *fn = ns_symbol_get_fn(call->callee);
        // a numeric return converts to the fn's numeric return type (the parse
        // pass already accepted the pair in ns_vm_parse_jump_stmt)
        if (!ns_type_is_ref(fn->ret) && ns_type_is_number(fn->ret) && ns_type_is_number(ret.t) && !ns_type_equals(fn->ret, ret.t)) {
            ns_return_value cast_ret = ns_eval_cast_number(vm, ret, fn->ret, ns_ast_state_loc(ctx, n->state));
            if (ns_return_is_error(cast_ret)) return cast_ret;
            ret = cast_ret.r;
        }
        if (!ns_type_match(vm, fn->ret, ret.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "return type mismatch.");
        }
        ns_eval_copy(vm, call->ret, ret, ns_type_size(vm, ret.t));
        // For a union return type, keep the concrete member's tag on the return
        // slot so the caller's value round-trips (the slot itself is large enough).
        if (ns_type_is(fn->ret, NS_TYPE_UNION)) {
            call->ret.t = ns_type_set_stack(ret.t, true);
        }
        call->ret_set = true;
    }
    return ns_return_ok(value, ret);
}

ns_return_void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->jump_stmt.label.type) {
    case NS_TOKEN_RETURN: ns_eval_return_stmt(vm, ctx, i); break;
    case NS_TOKEN_BREAK:
    case NS_TOKEN_CONTINUE: {
        i32 cl = ns_array_length(vm->call_stack);
        if (cl == 0) return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "break/continue not in loop.");
        ns_call *call = &vm->call_stack[cl - 1];
        if (n->jump_stmt.label.type == NS_TOKEN_BREAK) call->brk_set = true;
        else call->cnt_set = true;
    } break;
    default:
        return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown jump stmt type.");
    }
    return ns_return_ok_void;
}

ns_return_void ns_eval_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_b = ns_eval_expr(vm, ctx, n->if_stmt.condition);
    if (ns_return_is_error(ret_b)) return ns_return_change_type(void, ret_b);

    ns_value b = ret_b.r;
    i32 body = ns_eval_bool(vm, b) ? n->if_stmt.body : n->if_stmt.else_body;
    if (!body) return ns_return_ok_void; // empty else

    // `else if`: the else body may itself be an if-stmt rather than a block.
    ns_ast_t *body_n = &ctx->nodes[body];
    if (body_n->type == NS_AST_IF_STMT) {
        return ns_eval_if_stmt(vm, ctx, body);
    }

    ns_scope_enter(vm);
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, body);
    if (ns_return_is_error(ret)) return ret;
    ns_scope_exit(vm);

    return ns_return_ok_void;
}

// Called by loops after each body iteration. Consumes a pending `continue` so
// the loop proceeds, and reports whether the loop must stop due to a pending
// `break` or `return`. `break` is consumed here; `return` is left set so it
// keeps propagating up to the enclosing function call frame.
static ns_bool ns_loop_should_stop(ns_vm *vm) {
    i32 n = ns_array_length(vm->call_stack);
    if (n == 0) return false;
    ns_call *call = &vm->call_stack[n - 1];
    if (call->cnt_set) call->cnt_set = false;
    if (call->brk_set) { call->brk_set = false; return true; }
    return call->ret_set;
}

ns_return_void ns_eval_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *gen = &ctx->nodes[n->for_stmt.generator];

    ns_scope_enter(vm);
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
            if (ns_loop_should_stop(vm)) break; // break or return
        }
    } else {
        // TODO, generator expr from generable subject.
    }
    ns_scope_exit(vm);
    return ns_return_ok_void;
}

ns_return_void ns_eval_loop_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_scope_enter(vm);
    if (n->loop_stmt.do_first) {
        do {
            ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->loop_stmt.body);
            if (ns_return_is_error(ret)) return ret;
            if (ns_loop_should_stop(vm)) break; // break or return

            ns_return_value ret_cond = ns_eval_expr(vm, ctx, n->loop_stmt.condition);
            if (ns_return_is_error(ret_cond)) return ns_return_change_type(void, ret_cond);
            if (!ns_eval_bool(vm, ret_cond.r)) break;
        } while (1);
    } else {
        while (1) {
            ns_return_value ret_cond = ns_eval_expr(vm, ctx, n->loop_stmt.condition);
            if (ns_return_is_error(ret_cond)) return ns_return_change_type(void, ret_cond);
            if (!ns_eval_bool(vm, ret_cond.r)) break;

            ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->loop_stmt.body);
            if (ns_return_is_error(ret)) return ret;
            if (ns_loop_should_stop(vm)) break; // break or return
        }
    }
    ns_scope_exit(vm);
    return ns_return_ok_void;
}

ns_return_value ns_eval_call_ops_fn(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_value l, ns_value r, ns_symbol *fn) {
    ns_unused(i);

    i32 ret_size = ns_type_size(vm, fn->fn.ret);
    ns_ast_t *n = &ctx->nodes[i];
    if (ret_size < 0) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid override fn.");

    u64 ret_offset = ns_eval_alloc(vm, ret_size);
    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->fn.ret, true), .o = ret_offset};
    ns_call call = (ns_call){.callee = fn, .ret = ret_val, .ret_set = false, .scope_top = ns_array_length(vm->scope_stack), .arg_offset = ns_array_length(vm->symbol_stack), 2 };
    ns_scope_enter(vm);

    ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[0].name, .val = l, .parsed = true};
    ns_array_push(vm->symbol_stack, l_arg);
    ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->fn.args[1].name, .val = r, .parsed = true};
    ns_array_push(vm->symbol_stack, r_arg);

    ns_array_push(vm->call_stack, call);
    ns_ast_t *fn_ast = &ctx->nodes[fn->fn.ast];
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn_ast->ops_fn_def.body);
    if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);

    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);
    return ns_return_ok(value, call.ret);
}

ns_return_value ns_eval_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (ns_type_is_number(l.t)) {
        return ns_eval_binary_ops_number(vm, ctx, l, r, i);
    } else {
        switch (l.t.type)
        {
        case NS_TYPE_STRING: {
            ns_token_t op = n->binary_expr.op;
            ns_str ls = ns_eval_str(vm, l);
            ns_str rs = ns_eval_str(vm, r);
            if (op.type == NS_TOKEN_ADD_OP && ns_str_equals(op.val, ns_str_cstr("+"))) {
                ns_str cat = ns_str_concat(ls, rs);
                ns_value v = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, cat)};
                ns_str_free(cat);
                return ns_return_ok(value, v);
            } else if (op.type == NS_TOKEN_EQ_OP) {
                ns_bool eq = ns_str_equals(ls, rs);
                ns_bool ne = ns_str_equals(op.val, ns_str_cstr("!=")) || ns_str_equals(op.val, ns_str_cstr("!=="));
                ns_value v = (ns_value){.t = ns_type_bool, .b = ne ? !eq : eq};
                return ns_return_ok(value, v);
            } else if (op.type == NS_TOKEN_REL_OP) {
                // Lexicographic ordering: compare the shared prefix, then break
                // ties on length (so "ab" < "abc").
                i32 min_len = ls.len < rs.len ? ls.len : rs.len;
                i32 cmp = min_len > 0 ? memcmp(ls.data, rs.data, (szt)min_len) : 0;
                if (cmp == 0) cmp = (ls.len > rs.len) - (ls.len < rs.len);
                ns_bool result;
                if (ns_str_equals(op.val, ns_str_cstr("<"))) result = cmp < 0;
                else if (ns_str_equals(op.val, ns_str_cstr("<="))) result = cmp <= 0;
                else if (ns_str_equals(op.val, ns_str_cstr(">"))) result = cmp > 0;
                else result = cmp >= 0; // ">="
                ns_value v = (ns_value){.t = ns_type_bool, .b = result};
                return ns_return_ok(value, v);
            }
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented string ops.");
        }
        default:
            break;
        }
        ns_str l_t = ns_vm_get_type_name(vm, l.t);
        ns_str r_t = ns_vm_get_type_name(vm, r.t);
        ns_str fn_name = ns_ops_override_name(l_t, r_t, n->ops_fn_def.ops);
        ns_symbol *fn = ns_vm_find_symbol(vm, fn_name, false);
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

static u64 ns_eval_literal_u64(ns_str s) {
    i32 i = 0;
    ns_bool neg = false;
    if (i < s.len && (s.data[i] == '-' || s.data[i] == '+')) {
        neg = s.data[i] == '-';
        i++;
    }

    u64 r = 0;
    if (i + 1 < s.len && s.data[i] == '0' && (s.data[i + 1] == 'x' || s.data[i + 1] == 'X')) {
        i += 2;
        while (i < s.len) {
            i8 c = s.data[i];
            u64 d;
            if (c >= '0' && c <= '9') d = (u64)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (u64)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (u64)(c - 'A' + 10);
            else break;
            r = r * 16u + d;
            i++;
        }
    } else {
        while (i < s.len) {
            i8 c = s.data[i];
            if (c < '0' || c > '9') break;
            r = r * 10u + (u64)(c - '0');
            i++;
        }
    }
    return neg ? (u64)(-(i64)r) : r;
}

ns_return_value ns_eval_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t t = n->primary_expr.token;
    ns_value ret;
    switch (t.type) {
    case NS_TOKEN_INT_LITERAL: {
        // use the type resolved at vm-parse; fall back to the suffix default
        // for exprs evaluated without a parse pass
        ns_type lt = n->primary_expr.t;
        if (!ns_type_is_number(lt)) {
            switch (t.suffix) {
            case NS_NUM_SUFFIX_I8: lt = ns_type_i8; break;
            case NS_NUM_SUFFIX_U8: lt = ns_type_u8; break;
            case NS_NUM_SUFFIX_I16: lt = ns_type_i16; break;
            case NS_NUM_SUFFIX_U16: lt = ns_type_u16; break;
            case NS_NUM_SUFFIX_U32: lt = ns_type_u32; break;
            case NS_NUM_SUFFIX_I64: lt = ns_type_i64; break;
            case NS_NUM_SUFFIX_U64: lt = ns_type_u64; break;
            default: lt = ns_type_i32; break;
            }
        }
        if (ns_type_unsigned(lt)) {
            ns_value uv = (ns_value){.t = ns_type_u64, .u64 = ns_eval_literal_u64(t.val)};
            if (ns_type_is(lt, NS_TYPE_U64)) { ret = uv; break; }
            ns_return_value cast_ret = ns_eval_cast_number(vm, uv, lt, ns_ast_state_loc(ctx, n->state));
            if (ns_return_is_error(cast_ret)) return cast_ret;
            ret = cast_ret.r;
            break;
        }
        ns_value iv = (ns_value){.t = ns_type_i64, .i64 = ns_str_to_i64(t.val)};
        if (ns_type_is(lt, NS_TYPE_I64)) { ret = iv; break; }
        ns_return_value cast_ret = ns_eval_cast_number(vm, iv, lt, ns_ast_state_loc(ctx, n->state));
        if (ns_return_is_error(cast_ret)) return cast_ret;
        ret = cast_ret.r;
    } break;
    case NS_TOKEN_FLT_LITERAL: {
        ns_type lt = n->primary_expr.t;
        if (!ns_type_is_float(lt)) {
            if (t.suffix == NS_NUM_SUFFIX_F16) {
                ns_warn("numeric", "half-float literal fallback to f32 on CPU.\n");
            } else if (t.suffix == NS_NUM_SUFFIX_BF16) {
                ns_warn("numeric", "brain-float literal fallback to f32 on CPU.\n");
            }
            lt = t.suffix == NS_NUM_SUFFIX_F64 ? ns_type_f64 : ns_type_f32;
        }
        f64 fv = ns_str_to_f64(t.val);
        ret = ns_type_is(lt, NS_TYPE_F32) ? (ns_value){.t = ns_type_f32, .f32 = (f32)fv}
                                          : (ns_value){.t = ns_type_f64, .f64 = fv};
    } break;
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

            if (j > fmt.len) {
                return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "missing '}'.");
            }

            ns_return_value ret_v = ns_eval_expr(vm, ctx, expr->next);
            if (ns_return_is_error(ret_v)) return ret_v;

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

ns_return_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_str st_name = n->desig_expr.name.val;
    ns_symbol *st = ns_vm_find_symbol(vm, st_name, false);
    
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

        // numeric fields accept any numeric value; the switch below converts it
        ns_bool number_ok = ns_type_is_number(field->t) && ns_type_is_number(val.t);
        if (!number_ok && !ns_type_equals(field->t, val.t) && !ns_type_match(vm, field->t, val.t)) { // type mismatch
            // ns_str f_type = ns_vm_get_type_name(vm, field->t);
            // ns_str v_type = ns_vm_get_type_name(vm, val.t);
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "field type mismatch");
        }

        // For a union field, store the concrete member that was actually provided.
        ns_type t = ns_type_is(field->t, NS_TYPE_UNION) ? val.t : field->t;
        if (ns_type_is_array(t)) {
            *(u64*)(data + field->o) = (u64)ns_eval_array_raw(vm, val);
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
            if (ns_type_in_stack(val.t)) {
                memcpy(data, vm->stack + val.o, stride);
            } else {
                memcpy(data, (void*)val.o, stride);
            }
        } else if (ns_type_is(t, NS_TYPE_STRING)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unimplemented string field.");
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unimplemented field type.");
        }
    }

    ns_array_set_length(vm->stack, offset);
    ns_value ret = (ns_value){.t = ns_type_set_stack(st->st.st.t, true), .o = o};
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_type(vm, ctx, n);
    if (ns_return_is_error(ret_t)) return ns_return_change_type(value, ret_t);
    ns_type t = ret_t.r;

    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->cast_expr.expr);
    if (ns_return_is_error(ret_v)) return ret_v;
    ns_value v = ret_v.r;

    // Narrow a value that carries a union tag (e.g. read from a union-typed
    // struct field) to a member type by reinterpreting its stored bytes.
    if (ns_type_is(v.t, NS_TYPE_UNION) && !ns_type_is(t, NS_TYPE_UNION)) {
        ns_value out = v;
        out.t = ns_type_set_stack(t, ns_type_in_stack(v.t));
        return ns_return_ok(value, out);
    }

    if (ns_type_equals(t, v.t)) return ns_return_ok(value, v);
    // Union widening: the value already carries its concrete member tag, so
    // casting it to the union type is an identity at runtime.
    if (ns_type_is(t, NS_TYPE_UNION)) return ns_return_ok(value, v);
    if (ns_type_is_number(t) && ns_type_is_number(v.t)) {
        return ns_eval_cast_number(vm, v, t, ns_ast_state_loc(ctx, n->state));
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

    ns_ast_t *field = &ctx->nodes[n->member_expr.right];
    if (field->type == NS_AST_MEMBER_EXPR) {
        return ns_eval_member_expr(vm, ctx, n->member_expr.right);
    }

    // primary expr
    ns_str name = field->primary_expr.token.val;

    if (ns_type_is_array(st.t)) {
        void *data = ns_eval_array_data(vm, st);
        i32 n = 0;
        if (ns_str_equals_STR(name, "len") || ns_str_equals_STR(name, "size")) {
            n = (i32)ns_buffer_len(data);
        } else if (ns_str_equals_STR(name, "cap")) {
            n = (i32)ns_buffer_cap(data);
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, field->state), NS_ERR_EVAL, "unknown array member.");
        }
        return ns_return_ok(value, ((ns_value){.t = ns_type_i32, .i32 = n}));
    }

    if (ns_type_is(st.t, NS_TYPE_DICT) || ns_type_is(st.t, NS_TYPE_SET)) {
        ns_dict_table *table = ns_eval_dict_table(vm, st);
        i32 n = 0;
        if (ns_str_equals_STR(name, "len") || ns_str_equals_STR(name, "size")) {
            n = table ? table->len : 0;
        } else if (ns_str_equals_STR(name, "cap")) {
            n = table ? table->cap : 0;
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, field->state), NS_ERR_EVAL, "unknown container member.");
        }
        return ns_return_ok(value, ((ns_value){.t = ns_type_i32, .i32 = n}));
    }

    // string .len/.size/.cap yield byte-count metadata.
    if (ns_type_is(st.t, NS_TYPE_STRING) && !ns_type_is_array(st.t) &&
        (ns_str_equals_STR(name, "len") || ns_str_equals_STR(name, "size") || ns_str_equals_STR(name, "cap"))) {
        ns_str s = ns_eval_str(vm, st);
        i32 n = ns_str_equals_STR(name, "cap") && s.data ? (i32)ns_buffer_cap(s.data) : s.len;
        ns_value lv = {.t = ns_type_i32, .i32 = n};
        return ns_return_ok(value, lv);
    }

    ns_symbol *st_type = &vm->symbols[ns_type_index(st.t)];
    if (st_type->type != NS_SYMBOL_STRUCT) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown struct.");
    }

    for (i32 f_i = 0, l = ns_array_length(st_type->st.fields); f_i < l; ++f_i) {
        ns_struct_field *field = &st_type->st.fields[f_i];
        if (ns_str_equals(field->name, name)) {
            // The field inherits the struct's addressing (stack vs absolute)
            // and mutability, so fields of a mutable struct - including
            // heap-backed array elements - are themselves assignable.
            ns_type ft = field->t;
            ft.stack = ns_type_in_stack(st.t);
            // A field reached through a ref is mutable: a ref is a mutable
            // pointer to its target, so `v.field = x` is allowed for `v: ref T`.
            ft.mut = st.t.mut || ns_type_is_ref(st.t);
            ns_value val = (ns_value){.t = ft, .o = st.o + field->o};
            return ns_return_ok(value, val);
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
            v.t.mut = false;
            return ns_return_ok(value, v);
        } else {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
        return ns_return_ok(value, v);
    case NS_TOKEN_CMP_OP: {      // logical not: !bool -> bool
        if (!ns_type_is(v.t, NS_TYPE_BOOL)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "logical not requires a bool operand.");
        }
        // Read through the stack: a bool variable's value lives in the stack
        // slot, not the immediate `v.b` union field.
        ns_value ret = (ns_value){.t = ns_type_encode(NS_TYPE_BOOL, 0, false, false, true)};
        ret.b = !ns_eval_bool(vm, v);
        return ns_return_ok(value, ret);
    }
    case NS_TOKEN_BIT_INVERT_OP: { // bitwise not: ~int -> int (same type)
        if (!ns_type_is_number(v.t) || ns_type_is_float(v.t) || ns_type_is(v.t, NS_TYPE_BOOL)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "bitwise not requires an integer operand.");
        }
        ns_value ret = (ns_value){.t = ns_type_set_mut(v.t, false)};
        switch (v.t.type) {
        case NS_TYPE_I8:  ret.i8  = ~ns_eval_number_i8(vm, v); break;
        case NS_TYPE_U8:  ret.u8  = ~ns_eval_number_u8(vm, v); break;
        case NS_TYPE_I16: ret.i16 = ~ns_eval_number_i16(vm, v); break;
        case NS_TYPE_U16: ret.u16 = ~ns_eval_number_u16(vm, v); break;
        case NS_TYPE_I32: ret.i32 = ~ns_eval_number_i32(vm, v); break;
        case NS_TYPE_U32: ret.u32 = ~ns_eval_number_u32(vm, v); break;
        case NS_TYPE_I64: ret.i64 = ~ns_eval_number_i64(vm, v); break;
        case NS_TYPE_U64: ret.u64 = ~ns_eval_number_u64(vm, v); break;
        default: break;
        }
        return ns_return_ok(value, ret);
    }
    case NS_TOKEN_REF:
        if (ns_type_is_ref(v.t)) return ns_return_ok(value, v);
        if (ns_type_is_const(v.t)) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "cannot take reference of const value.");
        u64 offset = ns_eval_alloc(vm, sizeof(u64));
        ns_value dst = {.o = offset};
        dst.t = ns_type_encode(v.t.type, v.t.index, true, true, true);
        ns_eval_copy(vm, dst, v, ns_type_size(vm, v.t));
        return ns_return_ok(value, dst);
    default:
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown unary ops.");
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->array_expr.literal) {
        ns_type arr_type = n->array_expr.rt;
        if (!ns_type_is_array(arr_type)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "array literal type was not resolved.");
        }

        ns_type element_type = arr_type;
        element_type.array = false;
        i32 element_count = n->array_expr.elem_count;
        i32 element_size = ns_type_size(vm, element_type);
        if (element_size <= 0) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid array element type.");
        }

        i8 *data = (i8 *)ns_buffer_alloc((szt)element_size, (szt)element_count, element_type);
        if (!data) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "array allocation failed.");
        }

        i32 next = n->next;
        for (i32 e_i = 0; e_i < element_count; ++e_i) {
            ns_ast_t elem = ctx->nodes[next];
            ns_return_value ret_elem = ns_eval_expr(vm, ctx, next);
            if (ns_return_is_error(ret_elem)) return ret_elem;
            ns_value elem_v = ret_elem.r;

            if (!ns_type_equals(element_type, elem_v.t)) {
                if (!ns_eval_number_assign_compatible(vm, element_type, elem_v.t) && !ns_type_match(vm, element_type, elem_v.t)) {
                    return ns_return_error(value, ns_ast_state_loc(ctx, elem.state), NS_ERR_EVAL, "array literal element type mismatch.");
                }
                if (ns_type_is_number(element_type) && ns_type_is_number(elem_v.t)) {
                    ns_return_value cast_ret = ns_eval_cast_number(vm, elem_v, element_type, ns_ast_state_loc(ctx, elem.state));
                    if (ns_return_is_error(cast_ret)) return cast_ret;
                    elem_v = cast_ret.r;
                }
            }

            ns_value dst = {.t = element_type, .o = (u64)(data + (szt)e_i * (szt)element_size)};
            dst.t.stack = false;
            dst.t.mut = true;
            ns_return_value copy_ret = ns_eval_copy(vm, dst, elem_v, element_size);
            if (ns_return_is_error(copy_ret)) return copy_ret;
            next = elem.next;
        }

        ns_value v = (ns_value){.t = {.type = arr_type.type, .array = true, .mut = true, .ref = arr_type.ref, .index = arr_type.index, .stack = false}, .o = (u64)data};
        return ns_return_ok(value, v);
    }

    ns_ast_t *t = &ctx->nodes[n->array_expr.type];

    ns_return_type ret_t = ns_vm_parse_type(vm, ctx, t);
    if (ns_return_is_error(ret_t)) return ns_return_change_type(value, ret_t);
    ns_type type = ret_t.r;

    ns_return_value ret_count = ns_eval_expr(vm, ctx, n->array_expr.count_expr);
    if (ns_return_is_error(ret_count)) return ret_count;

    ns_value count = ret_count.r;
    i32 element_count = ns_eval_number_i32(vm, count);
    if (ns_type_is(type, NS_TYPE_DICT) || ns_type_is(type, NS_TYPE_SET)) {
        ns_dict_table *table = ns_eval_dict_alloc(vm, type, element_count);
        ns_value v = (ns_value){.t = ns_type_set_stack(type, false), .o = (u64)table};
        return ns_return_ok(value, v);
    }

    type.array = false;
    if (element_count < 0) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "array size must be non-negative.");
    }
    i32 element_size = ns_type_size(vm, type);
    if (element_size <= 0) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "invalid array element type.");
    }
    i8* data = (i8 *)ns_buffer_alloc((szt)element_size, (szt)element_count, type);
    if (!data) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "array allocation failed.");
    }
    ns_value v = (ns_value){.t = {.type = type.type, .array = true, .mut = true, .ref = type.ref, .index = type.index, .stack = false }, .o = (u64)data};
    return ns_return_ok(value, v);
}

ns_return_value ns_eval_index_expr_with_create(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_bool create) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_table = ns_eval_expr(vm, ctx, n->index_expr.table);
    if (ns_return_is_error(ret_table)) return ret_table;
    ns_value table = ret_table.r;

    ns_return_value ret_index = ns_eval_expr(vm, ctx, n->index_expr.expr);
    if (ns_return_is_error(ret_index)) return ret_index;
    ns_value index = ret_index.r;

    if (ns_type_is(table.t, NS_TYPE_DICT)) {
        ns_symbol *dict = &vm->symbols[ns_type_index(table.t)];
        if (!ns_type_match(vm, dict->ct.key, index.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "dict key type mismatch.");
        }
        return ns_eval_dict_find(vm, table, index, create, ns_ast_state_loc(ctx, n->state));
    }

    if (!ns_type_is_number(index.t)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "index expr type mismatch.");
    }

    // string indexing: s[i] yields the byte at position i as an i32 char code.
    if (ns_type_is(table.t, NS_TYPE_STRING) && !ns_type_is_array(table.t)) {
        ns_str s = ns_eval_str(vm, table);
        i32 idx = ns_eval_number_i32(vm, index);
        i32 c = (idx >= 0 && idx < s.len) ? (i32)(u8)s.data[idx] : 0;
        ns_value cv = {.t = ns_type_i32, .i32 = c};
        return ns_return_ok(value, cv);
    }

    if (!ns_type_is_array(table.t)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "index expr type mismatch.");
    }
    ns_type element_type = table.t;
    element_type.array = false;

    i32 idx = ns_eval_number_i32(vm, index);
    void *base = ns_eval_array_data(vm, table);
    i32 len = (i32)ns_buffer_len(base);
    if (idx < 0 || idx >= len) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "array index out of bounds.");
    }
    u64 offset = (u64)ns_type_size(vm, element_type) * (u64)idx;
    u8* data = (u8*)base + offset;
    // The element lives in the array's heap storage: address it absolutely
    // (stack=false) and keep it mutable so it can be assigned to.
    element_type.stack = false;
    element_type.mut = true;
    ns_value val = (ns_value){.t = element_type, .o = (u64)data};
    return ns_return_ok(value, val);
}

ns_return_value ns_eval_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    return ns_eval_index_expr_with_create(vm, ctx, i, false);
}

ns_return_value ns_eval_block_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *sym = &vm->symbols[n->block_expr.rt.index];
    if (sym->type == NS_SYMBOL_FN) return ns_return_ok(value, sym->fn.fn);

    u64 offset = ns_eval_alloc(vm, (i32)(sym->bc.st.stride));
    ns_value ret = (ns_value){.t = ns_type_set_stack(sym->bc.val.t, true), .o = offset};

    i32 field_count = ns_array_length(sym->bc.st.fields);
    for (i32 f_i = 0; f_i < field_count; ++f_i) {
        ns_struct_field *field = &sym->bc.st.fields[f_i];
        ns_value src = ns_eval_find_value(vm, field->name);
        if (ns_is_nil(src)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown field.");
        }
        ns_value dst = (ns_value){.t = ns_type_set_stack(field->t, true), .o = offset + field->o};
        if (ns_type_is_ref(field->t)) {
            // ns_eval_copy treats a ref destination as pass-by-reference and
            // skips the write, which would leave the captured slot uninitialized.
            // A captured ref must snapshot the pointer itself so the closure can
            // dereference it later (e.g. after the defining scope exits).
            u64 ptr = ns_type_in_stack(src.t) ? *(u64 *)&vm->stack[src.o] : src.o;
            *(u64 *)&vm->stack[dst.o] = ptr;
        } else {
            ns_eval_copy(vm, dst, src, (i32)field->s);
        }
    }

    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *val = ns_vm_find_symbol(vm, n->var_def.name.val, false);

    // eval & store value
    i32 size = n->var_def.type_size;
    if (size < 0 && val->val.t.type != NS_TYPE_INFER) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown type size.");
    }

    // Evaluate the initializer first so an inferred type (and thus its size)
    // is known before we reserve the global's stack slot.
    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->var_def.expr);
    if (ns_return_is_error(ret_v)) return ret_v;

    ns_value v = ret_v.r;
    ns_type dst_t = val->val.t;
    // A union- or fn-typed slot keeps the concrete assigned type tag at runtime
    // so the stored value round-trips (values are self-describing in the
    // interpreter). See the matching comment in ns_eval_local_var_def for why
    // fn destinations need ns_type_match rather than exact type equality.
    ns_type store_t = dst_t;
    if (ns_type_is(dst_t, NS_TYPE_INFER)) {
        dst_t = v.t;
        store_t = v.t;
    } else if (ns_type_is(dst_t, NS_TYPE_UNION) || ns_type_is(dst_t, NS_TYPE_FN)) {
        if (!ns_type_match(vm, dst_t, v.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "var def type mismatch.");
        }
        store_t = v.t;
    } else if (!ns_type_equals(dst_t, v.t)) {
        if (!ns_eval_number_assign_compatible(vm, dst_t, v.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "var def type mismatch.");
        }
        ns_return_value cast_ret = ns_eval_cast_number(vm, v, dst_t, ns_ast_state_loc(ctx, n->state));
        if (ns_return_is_error(cast_ret)) return cast_ret;
        v = cast_ret.r;
        store_t = dst_t;
    }

    // Reference values (arrays, dicts, sets, strings, fns) are stored as an 8-byte handle;
    // ns_type_size would otherwise report the element/underlying size.
    if (ns_type_is_array(dst_t) || ns_type_is(dst_t, NS_TYPE_STRING) || ns_type_is(dst_t, NS_TYPE_FN) ||
        ns_type_is(dst_t, NS_TYPE_DICT) || ns_type_is(dst_t, NS_TYPE_SET))
        size = (i32)sizeof(void *);
    else if (size <= 0)
        size = ns_type_size(vm, dst_t);
    // Copy only the concrete member's bytes; the union slot may be larger.
    i32 copy_size = ns_type_is(dst_t, NS_TYPE_UNION) ? ns_type_size(vm, v.t) : size;
    ns_value ret = (ns_value){.o = ns_eval_alloc(vm, size), .t = ns_type_set_stack(store_t, true)};
    ns_eval_copy(vm, ret, v, copy_size);
    ns_array_set_length(vm->stack, ret.o + size);
    val->val = ret;
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_module_globals(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        if (n->type != NS_AST_VAR_DEF) continue;

        ns_symbol *val = ns_vm_find_symbol(vm, n->var_def.name.val, false);
        if (val && val->type == NS_SYMBOL_VALUE && ns_type_in_stack(val->val.t)) {
            continue;
        }

        ns_return_value ret = ns_eval_var_def(vm, ctx, s_i);
        if (ns_return_is_error(ret)) return ret;
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    vm->loc = ns_ast_state_loc(ctx, n->state);

    // eval & store value
    i32 size = n->var_def.type_size;
    if (size < 0) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown type size.");
    }

    ns_value ret = (ns_value){.o = ns_eval_alloc(vm, size)};
    ns_return_value ret_v = ns_eval_expr(vm, ctx, n->var_def.expr);
    if (ns_return_is_error(ret_v)) return ret_v;
    if (n->var_def.is_ref && ns_type_is_ref(ret_v.r.t)) {
        return ns_return_ok(value, ret_v.r);
    }

    ns_return_type ret_type = ns_vm_parse_type(vm, ctx, &ctx->nodes[n->var_def.type]);
    if (ns_return_is_error(ret_type)) return ns_return_change_type(value, ret_type);
    ns_type dst_t = ret_type.r;
    ns_value src = ret_v.r;

    // A union- or fn-typed slot keeps the concrete assigned type tag at runtime
    // so the stored value round-trips (values are self-describing in the
    // interpreter). A no-capture closure's own NS_TYPE_FN symbol index never
    // equals a declared `type X = (...) -> ...` alias's index even when the
    // signatures match structurally, so fn destinations need ns_type_match
    // (the same structural check ns_vm_parse_local_var_def already applies)
    // rather than exact type equality.
    ns_type store_t = dst_t;
    if (ns_type_is(dst_t, NS_TYPE_INFER)) {
        dst_t = src.t;
        store_t = src.t;
    } else if (ns_type_is(dst_t, NS_TYPE_UNION) || ns_type_is(dst_t, NS_TYPE_FN)) {
        if (!ns_type_match(vm, dst_t, src.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "local var def type mismatch.");
        }
        store_t = src.t;
    } else if (!ns_type_equals(dst_t, src.t)) {
        if (!ns_eval_number_assign_compatible(vm, dst_t, src.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "local var def type mismatch.");
        }
        ns_return_value cast_ret = ns_eval_cast_number(vm, src, dst_t, ns_ast_state_loc(ctx, n->state));
        if (ns_return_is_error(cast_ret)) return cast_ret;
        src = cast_ret.r;
        store_t = dst_t;
    }

    // A capturing block value points at its capture struct on the stack; bind
    // it directly (as a call-arg closure would be). Copying `size` bytes here
    // would truncate the captures to the declared fn type's 8-byte handle and
    // the trailing stack shrink below would free the struct.
    if (ns_type_is(store_t, NS_TYPE_BLOCK)) {
        ns_symbol symbol = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = n->var_def.name.val, .val = src, .parsed = true};
        ns_array_push(vm->symbol_stack, symbol);
        return ns_return_ok(value, src);
    }

    // A ref-typed binding aliases its referent: keep the ref value itself (an
    // absolute heap pointer from a native call, or a stack-relative reference)
    // rather than copying struct bytes into a fresh slot - ns_eval_copy is a
    // no-op for refs, so copying here would silently drop the pointer.
    if (ns_type_is_ref(store_t)) {
        ns_symbol symbol = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = n->var_def.name.val, .val = src, .parsed = true};
        ns_array_push(vm->symbol_stack, symbol);
        return ns_return_ok(value, src);
    }

    // Copy only the concrete member's bytes; the union slot may be larger.
    i32 copy_size = ns_type_is(dst_t, NS_TYPE_UNION) ? ns_type_size(vm, src.t) : size;
    ret.t = ns_type_set_stack(store_t, true);
    ns_eval_copy(vm, ret, src, copy_size);
    ns_array_set_length(vm->stack, ret.o + size);
    ns_symbol symbol = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = n->var_def.name.val, .val = ret, .parsed = true};
    ns_array_push(vm->symbol_stack, symbol);
    return ns_return_ok(value, ret);
}

ns_return_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
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
    case NS_AST_BLOCK_EXPR: return ns_eval_block_expr(vm, ctx, i);
    default:
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented expr type.");
    }

    return ns_return_ok(value, ns_nil);
}

ns_return_void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
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
        case NS_AST_EXPR:
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
        case NS_AST_JUMP_STMT: ns_eval_stmt_case(ns_eval_jump_stmt);
        case NS_AST_FOR_STMT: ns_eval_stmt_case(ns_eval_for_stmt);
        case NS_AST_IF_STMT: ns_eval_stmt_case(ns_eval_if_stmt);
        case NS_AST_LOOP_STMT: ns_eval_stmt_case(ns_eval_loop_stmt);
        case NS_AST_ASSERT_STMT: ns_eval_stmt_case(ns_eval_assert_stmt);
        default: {
            return ns_return_error(void, ns_ast_state_loc(ctx, expr->state), NS_ERR_EVAL, "unimplemented stmt type.");
        } break;
        }

        if (ns_array_length(vm->call_stack) > 0) {
            ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
            // return, break and continue all abort the rest of this block. The
            // enclosing loop clears brk/cnt; ret propagates to the call.
            if (call->ret_set || call->brk_set || call->cnt_set) {
                vm->stack_depth = stack_depth; // restore on early exit, else depth leaks
                return ns_return_ok_void;
            }
        }
    }
    vm->stack_depth = stack_depth;
    return ns_return_ok_void;
}

ns_return_value ns_eval_ast(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_return_bool ret_parse = ns_vm_parse(vm, ctx);
    if (ns_return_is_error(ret_parse)) return ns_return_change_type(value, ret_parse);
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals_STR(vm->lib, "main");
    if (!main_mod) {
        ns_warn("ns_eval", "try to eval non-main module %.*s.", vm->lib.len, vm->lib.data);
        return ns_return_ok(value, ns_nil);
    }

    ns_value main_ret = ns_nil;
    ns_symbol* main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main"), false);
    if (!main_fn) {
        ns_call call = (ns_call){.callee = main_fn, .scope_top = ns_array_length(vm->scope_stack), .ret_set = false };
        ns_array_push(vm->call_stack, call);

        ns_scope_enter(vm);
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
                ns_return_value ret = ns_eval_local_var_def(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_ASSERT_STMT: {
                ns_return_void ret = ns_eval_assert_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_IF_STMT: {
                ns_return_void ret = ns_eval_if_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_FOR_STMT: {
                ns_return_void ret = ns_eval_for_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_LOOP_STMT: {
                ns_return_void ret = ns_eval_loop_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_JUMP_STMT: {
                ns_return_void ret = ns_eval_jump_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_USE_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OP_FN_DEF:
            case NS_AST_STRUCT_DEF:
            case NS_AST_TYPE_DEF:
                break; // already parsed, no need to re-evaluate
            default: {
                ns_code_loc loc = (ns_code_loc){.f = ctx->filename, .l = n->state.l, .o = n->state.o};
                return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented global ast.");
            } break;
            }
        }
        ns_scope_exit(vm);
        ns_array_pop(vm->call_stack);
        main_ret = (ns_value){.t = ns_type_i32, .i32 = 0};
    } else {
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
            case NS_AST_ASSERT_STMT: {
                ns_return_void ret = ns_eval_assert_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_IF_STMT: {
                ns_return_void ret = ns_eval_if_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_FOR_STMT: {
                ns_return_void ret = ns_eval_for_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_LOOP_STMT: {
                ns_return_void ret = ns_eval_loop_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_JUMP_STMT: {
                ns_return_void ret = ns_eval_jump_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);
            } break;
            case NS_AST_TYPE_DEF:
            case NS_AST_USE_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OP_FN_DEF:
            case NS_AST_STRUCT_DEF:
                break; // already parsed
            default: {
                ns_code_loc loc = (ns_code_loc){.f = ctx->filename, .l = n->state.l, .o = n->state.o};
                return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented global ast.");
            } break;
            }
        }

        ns_type ret_type = main_fn->fn.ret;
        i32 ret_size = ns_type_size(vm, ret_type);
        u64 ret_offset = ns_eval_alloc(vm, ret_size);
        ns_value ret_val = (ns_value){.t = ns_type_set_stack(ret_type, true), .o = ret_offset};
        ns_call call = (ns_call){.callee = main_fn, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false };

        ns_scope_enter(vm);
        ns_array_push(vm->call_stack, call);
        ns_ast_t *fn = &ctx->nodes[main_fn->fn.ast];
        ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn->fn_def.body);
        if (ns_return_is_error(ret)) return ns_return_change_type(value, ret);

        call = ns_array_pop(vm->call_stack);
        ns_scope_exit(vm);
        main_ret = call.ret;
    }

    return ns_return_ok(value, main_ret);
}

ns_return_value ns_eval_with_map(ns_vm *vm, ns_str source, ns_str filename, ns_line_loc *line_map) {
    // Check for empty input
    if (source.data == ns_null || source.len == 0) {
        return ns_return_error(value, ns_code_loc_nil, NS_ERR_SYNTAX, "empty source input");
    }

    ns_ast_ctx ctx = {0};
    ctx.line_map = line_map;
    ns_return_bool ret_ast = ns_ast_parse(&ctx, source, filename);
    if (ns_return_is_error(ret_ast)) return ns_return_change_type(value, ret_ast);
    return ns_eval_ast(vm, &ctx);
}

ns_return_value ns_eval(ns_vm *vm, ns_str source, ns_str filename) {
    return ns_eval_with_map(vm, source, filename, ns_null);
}
