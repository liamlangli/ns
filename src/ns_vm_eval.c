#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_shader.h"
#include "ns_profile.h"

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
ns_return_value ns_eval_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, i32 i);
static ns_return_value ns_eval_struct_ctor(ns_vm *vm, ns_ast_ctx *ctx, i32 i, i32 st_index);
ns_return_void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

static void ns_eval_profile_scope_end(ns_symbol *sym, i32 depth, f64 start_ms) {
    if (!ns_profile.enabled) return;
    ns_profile_record_scope(sym->name, sym->lib, depth, start_ms, ns_profile_now_ms() - start_ms);
}

static const_str ns_eval_type_mismatch_msg(ns_vm *vm, const_str msg, ns_type expected, ns_type got) {
    ns_str e = ns_vm_get_type_name(vm, expected);
    ns_str g = ns_vm_get_type_name(vm, got);
    szt len = snprintf(ns_null, 0, "%s expected %.*s, got %.*s.", msg, e.len, e.data, g.len, g.data);
    i8 *data = ns_malloc(len + 1);
    snprintf(data, len + 1, "%s expected %.*s, got %.*s.", msg, e.len, e.data, g.len, g.data);
    return data;
}

static const_str ns_eval_type_mismatch_label_msg(ns_vm *vm, const_str msg, const_str expected, ns_type got) {
    ns_str g = ns_vm_get_type_name(vm, got);
    szt len = snprintf(ns_null, 0, "%s expected %s, got %.*s.", msg, expected, g.len, g.data);
    i8 *data = ns_malloc(len + 1);
    snprintf(data, len + 1, "%s expected %s, got %.*s.", msg, expected, g.len, g.data);
    return data;
}

static const_str ns_eval_type_pair_mismatch_msg(ns_vm *vm, const_str msg, const_str left_label, ns_type left, const_str right_label, ns_type right) {
    ns_str l = ns_vm_get_type_name(vm, left);
    ns_str r = ns_vm_get_type_name(vm, right);
    szt len = snprintf(ns_null, 0, "%s %s %.*s, %s %.*s.", msg, left_label, l.len, l.data, right_label, r.len, r.data);
    i8 *data = ns_malloc(len + 1);
    snprintf(data, len + 1, "%s %s %.*s, %s %.*s.", msg, left_label, l.len, l.data, right_label, r.len, r.data);
    return data;
}

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
    if (ns_type_is(dst, NS_TYPE_ENUM)) return false;
    dst = ns_enum_underlying_type(vm, dst);
    src = ns_enum_underlying_type(vm, src);
    if (!ns_type_is_number(dst) || !ns_type_is_number(src)) return false;
    if (ns_type_is_float(src) && !ns_type_is_float(dst)) return false;

    i32 dst_size = ns_type_size(vm, dst);
    i32 src_size = ns_type_size(vm, src);
    if (dst_size < src_size) return false;
    return true;
}

ns_value ns_eval_enum_underlying_value(ns_vm *vm, ns_value v) {
    if (!ns_type_is(v.t, NS_TYPE_ENUM)) return v;
    ns_type underlying = ns_enum_underlying_type(vm, v.t);
    underlying.mut = v.t.mut;
    underlying.stack = v.t.stack;
    underlying.ref = v.t.ref;
    ns_value out = v;
    out.t = underlying;
    return out;
}

static i64 ns_eval_to_i64(ns_vm *vm, ns_value v) {
    v = ns_eval_enum_underlying_value(vm, v);
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
    v = ns_eval_enum_underlying_value(vm, v);
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
    v = ns_eval_enum_underlying_value(vm, v);
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
    v = ns_eval_enum_underlying_value(vm, v);
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
    a = ns_eval_enum_underlying_value(vm, a);
    b = ns_eval_enum_underlying_value(vm, b);
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

static ns_bool ns_eval_dict_key_hashable(ns_vm *vm, ns_value key) {
    return ns_type_is_number(ns_enum_underlying_type(vm, key.t)) || ns_type_is(key.t, NS_TYPE_STRING);
}

// Probe the open-addressed table for `key`. Slot states: 0 empty, 1 occupied,
// 2 tombstone (removed; kept so later entries in the probe chain stay
// reachable). Returns the occupied slot holding the key, or NULL. When
// insert_slot is non-null it receives the first reusable slot (tombstone or
// empty) along the probe path, for insertion.
static u8 *ns_eval_dict_probe(ns_vm *vm, ns_dict_table *table, ns_value key, u8 **insert_slot) {
    if (insert_slot) *insert_slot = ns_null;
    if (!table || table->cap <= 0) return ns_null;
    u64 h = ns_eval_hash_value(vm, key);
    i32 start = (i32)(h % (u64)table->cap);
    u8 *reuse = ns_null;
    for (i32 step = 0; step < table->cap; ++step) {
        i32 index = (start + step) % table->cap;
        u8 *slot = ns_eval_dict_slot(table, index);
        if (slot[0] == 0) {
            if (!reuse) reuse = slot;
            break;
        }
        if (slot[0] == 2) {
            if (!reuse) reuse = slot;
            continue;
        }
        ns_value slot_key = ns_eval_slot_value(table, slot + table->key_off, table->key_type);
        if (ns_eval_value_equals(vm, slot_key, key)) return slot;
    }
    if (insert_slot) *insert_slot = reuse;
    return ns_null;
}

static void ns_eval_dict_slot_insert(ns_vm *vm, ns_dict_table *table, u8 *slot, ns_value key) {
    slot[0] = 1;
    table->len++;
    ns_value dst_key = {.t = ns_type_set_stack(ns_type_set_mut(table->key_type, true), false), .o = (u64)(slot + table->key_off)};
    ns_eval_copy(vm, dst_key, key, table->key_size);
}

static ns_return_value ns_eval_dict_find(ns_vm *vm, ns_value table_v, ns_value key, ns_bool create, ns_code_loc loc) {
    if (!ns_eval_dict_key_hashable(vm, key)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "dict key type not hashable.");
    }

    ns_dict_table *table = ns_eval_dict_table(vm, table_v);
    u8 *insert_slot = ns_null;
    u8 *slot = ns_eval_dict_probe(vm, table, key, &insert_slot);
    if (slot) {
        return ns_return_ok(value, ns_eval_dict_value(table, slot + table->val_off, create));
    }
    if (!create) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "dict key not found.");
    }
    if (!insert_slot || table->len >= table->cap) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "dict capacity exceeded.");
    }
    ns_eval_dict_slot_insert(vm, table, insert_slot, key);
    return ns_return_ok(value, ns_eval_dict_value(table, insert_slot + table->val_off, create));
}

// has(c, k) / insert(s, v) / remove(c, k): membership operations over the
// shared dict/set hash table. kind: 0 has, 1 insert, 2 remove.
static ns_return_value ns_eval_container_builtin(ns_vm *vm, ns_ast_ctx *ctx, i32 i, i32 kind) {
    ns_ast_t *n = &ctx->nodes[i];
    i32 a0 = n->next;
    i32 a1 = ctx->nodes[a0].next;
    ns_code_loc loc = ns_ast_state_loc(ctx, n->state);

    ns_return_value ret_c = ns_eval_expr(vm, ctx, a0);
    if (ns_return_is_error(ret_c)) return ret_c;
    ns_return_value ret_k = ns_eval_expr(vm, ctx, a1);
    if (ns_return_is_error(ret_k)) return ret_k;
    ns_value key = ret_k.r;

    if (!ns_eval_dict_key_hashable(vm, key)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "container key type not hashable.");
    }
    ns_dict_table *table = ns_eval_dict_table(vm, ret_c.r);
    u8 *insert_slot = ns_null;
    u8 *slot = ns_eval_dict_probe(vm, table, key, &insert_slot);

    ns_bool result = false;
    if (kind == 0) { // has
        result = slot != ns_null;
    } else if (kind == 1) { // insert
        if (!slot) {
            if (!insert_slot || !table || table->len >= table->cap) {
                return ns_return_error(value, loc, NS_ERR_EVAL, "set capacity exceeded.");
            }
            ns_eval_dict_slot_insert(vm, table, insert_slot, key);
            result = true;
        }
    } else { // remove: tombstone the slot so probe chains stay intact
        if (slot) {
            slot[0] = 2;
            table->len--;
            result = true;
        }
    }
    return ns_return_ok(value, result ? ns_true : ns_false);
}

ns_return_value ns_eval_cast_number(ns_vm *vm, ns_value v, ns_type dst, ns_code_loc loc) {
    // A same-width cast still has to materialize stack/heap-backed scalars.
    // Returning their address unchanged and later retagging it as an immediate
    // enum would turn the storage offset into the enum's numeric value.
    if (ns_type_equals(v.t, dst) && ns_type_is_const(v.t) && !ns_type_in_stack(v.t)) {
        return ns_return_ok(value, v);
    }
    v = ns_eval_enum_underlying_value(vm, v);
    if (!ns_type_is_number(v.t) || !ns_type_is_number(dst)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "numeric cast type mismatch.", dst, v.t));
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

    // Arrays, dicts, sets, strings, fns and tasks are reference values stored as a single u64 handle
    // (a heap pointer or str_list index). Their value type carries mut=1, so they
    // never reach the const switch below; handle them uniformly here.
    if (ns_type_is_array(src.t) || ns_type_is(src.t, NS_TYPE_STRING) || ns_type_is(src.t, NS_TYPE_FN) ||
        ns_type_is(src.t, NS_TYPE_DICT) || ns_type_is(src.t, NS_TYPE_SET) || ns_type_is(src.t, NS_TYPE_TASK)) {
        u64 handle = ns_type_in_stack(src.t) ? *(u64 *)&vm->stack[src.o] : src.o;
        *(u64 *)dptr = handle;
        return ns_return_ok(value, dst);
    }

    if (ns_type_is(src.t, NS_TYPE_ENUM)) src = ns_eval_enum_underlying_value(vm, src);

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

// eval-only ns_vm_find_symbol(vm, s, false) with a per-AST-node inline cache.
// LOCAL entries hold a frame-relative symbol_stack offset and are only trusted
// (and filled) when the innermost fn frame is the top call frame, so closure
// block frames with per-site layouts never alias a same-named outer local.
// GLOBAL entries hold a vm->symbols index, invalidated by vm->symbol_gen.
// Every hit is verified by one name compare; any mismatch falls back to the
// full scan and refills, so semantics (shadowing, lib preference) are unchanged.
ns_symbol* ns_vm_find_symbol_cached(ns_vm *vm, ns_str s, ns_sym_cache *c) {
    i32 l = ns_vm_get_last_call(vm);
    if (l >= 0) {
        ns_call *call = &vm->call_stack[l];
        i32 symbol_top = vm->scope_stack[call->scope_top].symbol_top;
        i32 symbol_count = ns_array_length(vm->symbol_stack);
        ns_bool frame_stable = c && l == (i32)ns_array_length(vm->call_stack) - 1;

        if (frame_stable && c->kind == NS_SYM_CACHE_LOCAL) {
            i32 j = symbol_top + c->index;
            if (j < symbol_count && ns_str_equals(vm->symbol_stack[j].name, s)) {
                return &vm->symbol_stack[j];
            }
        }

        for (i32 j = symbol_count - 1; j >= symbol_top; --j) {
            if (ns_str_equals(vm->symbol_stack[j].name, s)) {
                if (c) {
                    if (frame_stable) *c = (ns_sym_cache){.kind = NS_SYM_CACHE_LOCAL, .index = j - symbol_top};
                    else c->kind = NS_SYM_CACHE_NONE;
                }
                return &vm->symbol_stack[j];
            }
        }
    }

    if (c && c->kind == NS_SYM_CACHE_GLOBAL && c->gen == vm->symbol_gen &&
        c->index < (i32)ns_array_length(vm->symbols) && ns_str_equals(vm->symbols[c->index].name, s)) {
        return &vm->symbols[c->index];
    }

    for (i32 i = 0, n = ns_array_length(vm->symbols); i < n; i++) {
        if (ns_str_equals(vm->symbols[i].name, s) && ns_str_equals(vm->symbols[i].lib, vm->lib)) {
            if (c) *c = (ns_sym_cache){.kind = NS_SYM_CACHE_GLOBAL, .index = i, .gen = vm->symbol_gen};
            return &vm->symbols[i];
        }
    }

    for (i32 i = 0, n = ns_array_length(vm->symbols); i < n; i++) {
        if (ns_str_equals(vm->symbols[i].name, s)) {
            if (c) *c = (ns_sym_cache){.kind = NS_SYM_CACHE_GLOBAL, .index = i, .gen = vm->symbol_gen};
            return &vm->symbols[i];
        }
    }
    return ns_null;
}

ns_value ns_eval_find_value_cached(ns_vm *vm, ns_str name, ns_sym_cache *c) {
    ns_symbol *s = ns_vm_find_symbol_cached(vm, name, c);
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

ns_value ns_eval_find_value(ns_vm *vm, ns_str name) {
    return ns_eval_find_value_cached(vm, name, ns_null);
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
    (void)ns_array_pop(vm->scope_stack);
    return scope;
}

ns_return_value ns_eval_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_value ret_l;
    if (ctx->nodes[n->binary_expr.left].type == NS_AST_INDEX_EXPR) {
        // Dict targets insert the missing key; array targets need the lvalue
        // (slot address) form — a plain read loads reference-typed elements
        // (strings, nested containers) as immediate handles instead.
        ret_l = ns_eval_index_expr_with_create(vm, ctx, n->binary_expr.left, true);
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
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "assign expr type mismatch.", l.t, r.t));
        }
        ns_return_value cast_ret = ns_eval_cast_number(vm, r, l.t, ns_ast_state_loc(ctx, n->state));
        if (ns_return_is_error(cast_ret)) return cast_ret;
        r = cast_ret.r;
    }

    // Compound assignment (`a += b`): apply the arithmetic to the current
    // value before the store. The node's op briefly swaps to its arithmetic
    // spelling so the shared binary-op dispatch applies, then is restored.
    if (n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP && n->binary_expr.op.val.len > 1) {
        if (ns_type_is(l.t, NS_TYPE_STRING) && !ns_type_in_stack(l.t)) {
            // a string element lvalue carries a slot address, which the
            // string reader cannot distinguish from a handle
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "compound assignment on a string element is unsupported; assign the concatenation instead.");
        }
        ns_token_t aop = n->binary_expr.op;
        ns_token_t arith = aop;
        arith.val = ns_str_range(aop.val.data, aop.val.len - 1);
        switch (aop.val.data[0]) {
        case '+': case '-': arith.type = NS_TOKEN_ADD_OP; break;
        case '*': case '/': case '%': arith.type = NS_TOKEN_MUL_OP; break;
        case '<': case '>': arith.type = NS_TOKEN_SHIFT_OP; break;
        case '&': case '|': case '^': arith.type = NS_TOKEN_BITWISE_OP; break;
        default:
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unsupported compound assignment operator.");
        }
        n->binary_expr.op = arith;
        ns_return_value ret_op = ns_eval_binary_ops(vm, ctx, l, r, i);
        n->binary_expr.op = aop;
        if (ns_return_is_error(ret_op)) return ret_op;
        r = ret_op.r;
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
    f64 profile_start_ms = ns_profile.enabled ? ns_profile_now_ms() : 0.0;
    i32 profile_depth = ns_profile.enabled ? (i32)ns_array_length(vm->call_stack) - 1 : 0;
    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn_ast->ops_fn_def.body);
    ns_eval_profile_scope_end(fn, profile_depth, profile_start_ms);
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

    // Container membership builtins: has(c, k) / insert(s, v) / remove(c, k).
    // A user fn with the same name shadows the builtin (mirrors the parse-side
    // routing in ns_vm_parse_call_expr).
    if (callee_n->type == NS_AST_PRIMARY_EXPR && callee_n->primary_expr.token.type == NS_TOKEN_IDENTIFIER &&
        n->call_expr.arg_count == 2) {
        ns_str bname = callee_n->primary_expr.token.val;
        i32 kind = ns_str_equals_STR(bname, "has")      ? 0
                 : ns_str_equals_STR(bname, "insert")   ? 1
                 : ns_str_equals_STR(bname, "remove")   ? 2
                                                        : -1;
        if (kind >= 0) {
            ns_symbol *user = ns_vm_find_symbol(vm, bname, true);
            if (!user || user->type != NS_SYMBOL_FN) {
                return ns_eval_container_builtin(vm, ctx, i, kind);
            }
        }
    }

    ns_return_value ret_callee = ns_eval_expr(vm, ctx, n->call_expr.callee);
    if (ns_return_is_error(ret_callee)) return ret_callee;

    ns_value callee = ret_callee.r;
    if (ns_is_nil(callee)) {
        // An identifier naming a struct has no value; treat the call as a
        // positional constructor (`point(0, 0)`).
        i32 callee_body = callee_n->type == NS_AST_EXPR ? callee_n->expr.body : n->call_expr.callee;
        ns_ast_t *callee_p = &ctx->nodes[callee_body];
        if (callee_p->type == NS_AST_PRIMARY_EXPR && callee_p->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
            ns_symbol *st = ns_vm_find_symbol(vm, callee_p->primary_expr.token.val, true);
            if (st && st->type == NS_SYMBOL_STRUCT) {
                return ns_eval_struct_ctor(vm, ctx, i, (i32)(st - vm->symbols));
            }
        }
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "nil callee.");
    }

    ns_symbol *sym = &vm->symbols[ns_type_index(callee.t)];
    if (sym->type == NS_SYMBOL_STRUCT) {
        return ns_eval_struct_ctor(vm, ctx, i, (i32)ns_type_index(callee.t));
    }
    ns_fn_symbol *fn = ns_symbol_get_fn(sym);

    // an async fn call runs its body as a concurrent task; the call expression
    // evaluates to the task handle immediately
    if (sym->type == NS_SYMBOL_FN && fn->fn_type == NS_FN_ASYNC && !fn->fn.t.ref) {
        return ns_task_spawn_async_call(vm, ctx, sym, i);
    }

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
            if (!ns_type_is_ref(pt) && !ns_type_is(pt, NS_TYPE_ENUM) &&
                ns_type_is_number(ns_enum_underlying_type(vm, pt)) &&
                ns_type_is_number(ns_enum_underlying_type(vm, v.t)) && !ns_type_equals(pt, v.t)) {
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
        // `mod shader` and `mod task` fns are VM-internal intrinsics (like std)
        // but need the ast ctx to walk fn bodies, so they dispatch here where
        // ctx is in scope.
        vm->loc = ns_ast_state_loc(ctx, n->state);
        ns_return_bool ret = ns_str_equals(sym->lib, ns_str_cstr("shader")) ? ns_shader_vm_call(vm, ctx)
                           : ns_str_equals(sym->lib, ns_str_cstr("task"))   ? ns_task_vm_call(vm, ctx)
                                                                            : ns_vm_call_ref(vm);
        if (ns_return_is_error(ret)) {
            (void)ns_array_pop(vm->call_stack);
            ns_scope_exit(vm);
            return ns_return_change_type(value, ret);
        }
    } else {
        if (sym->type == NS_SYMBOL_BLOCK) { // push block captured field to stack
            szt field_count = ns_array_length(sym->bc.st.fields);
            u64 o = callee.o;
            ns_bool in_stack = ns_type_in_stack(callee.t);
            for (szt f_i = 0; f_i < field_count; ++f_i) {
                ns_struct_field *field = &sym->bc.st.fields[f_i];
                ns_value v;
                if (in_stack) {
                    v = (ns_value){.t = ns_type_set_stack(field->t, true), .o = o + field->o};
                } else {
                    // a heap-snapshotted closure (e.g. one that crossed a task
                    // boundary) addresses its captures absolutely; a ref field's
                    // slot stores the captured pointer itself. mut=1 keeps the
                    // non-stack value a dereferenced location, not an immediate.
                    i8 *slot = (i8 *)o + field->o;
                    v = ns_type_is_ref(field->t)
                        ? (ns_value){.t = ns_type_set_stack(field->t, false), .o = *(u64 *)slot}
                        : (ns_value){.t = ns_type_set_mut(ns_type_set_stack(field->t, false), true), .o = (u64)slot};
                }
                ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = field->name, .val = v, .parsed = true};
                ns_array_push(vm->symbol_stack, arg);
            }
        }

        f64 profile_start_ms = ns_profile.enabled ? ns_profile_now_ms() : 0.0;
        i32 profile_depth = ns_profile.enabled ? (i32)ns_array_length(vm->call_stack) - 1 : 0;
        // The body's node indices belong to the context the fn was parsed
        // from (a lib module's context differs from the caller's).
        ns_return_void ret = ns_eval_compound_stmt(vm, fn->ctx ? fn->ctx : ctx, fn->body);
        ns_eval_profile_scope_end(sym, profile_depth, profile_start_ms);
        // Preserve the original message and source location. Replacing every
        // nested failure with "call expr error" hid actionable diagnostics
        // such as a missing native symbol or an assertion inside the callee.
        if (ns_return_is_error(ret)) {
            (void)ns_array_pop(vm->call_stack);
            ns_scope_exit(vm);
            return ns_return_change_type(value, ret);
        }
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
            // mut=1: a non-stack mut=0 value would read as an immediate.
            ns_value v = ns_type_is_ref(field->t)
                ? (ns_value){.t = ns_type_set_stack(field->t, false), .o = *(u64 *)slot}
                : (ns_value){.t = ns_type_set_mut(ns_type_set_stack(field->t, false), true), .o = (u64)slot};
            ns_symbol cap = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = field->name, .val = v, .parsed = true};
            ns_array_push(vm->symbol_stack, cap);
        }
    }

    f64 profile_start_ms = ns_profile.enabled ? ns_profile_now_ms() : 0.0;
    i32 profile_depth = ns_profile.enabled ? (i32)ns_array_length(vm->call_stack) - 1 : 0;
    ns_return_void ret = ns_eval_compound_stmt(vm, fn->ctx ? fn->ctx : ctx, fn->body);
    ns_eval_profile_scope_end(sym, profile_depth, profile_start_ms);
    if (ns_return_is_error(ret)) {
        (void)ns_array_pop(vm->call_stack);
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
    if (n->jump_stmt.expr <= 0) {
        if (ns_array_length(vm->call_stack) == 0) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "return stmt not in fn.");
        }
        ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        if (call->callee == ns_null) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "return stmt not in fn.");
        }
        ns_fn_symbol *fn = ns_symbol_get_fn(call->callee);
        if (!ns_type_is(fn->ret, NS_TYPE_VOID)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "return stmt requires a value in a non-void fn.");
        }
        call->ret_set = true;
        return ns_return_ok(value, call->ret);
    }

    ns_return_value ret_ret = ns_eval_expr(vm, ctx, n->jump_stmt.expr);
    if (ns_return_is_error(ret_ret)) return ret_ret;

    ns_value ret = ret_ret.r;
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        ns_fn_symbol *fn = ns_symbol_get_fn(call->callee);
        // a numeric return converts to the fn's numeric return type (the parse
        // pass already accepted the pair in ns_vm_parse_jump_stmt)
        if (!ns_type_is_ref(fn->ret) && !ns_type_is(fn->ret, NS_TYPE_ENUM) &&
            ns_type_is_number(ns_enum_underlying_type(vm, fn->ret)) &&
            ns_type_is_number(ns_enum_underlying_type(vm, ret.t)) && !ns_type_equals(fn->ret, ret.t)) {
            ns_return_value cast_ret = ns_eval_cast_number(vm, ret, fn->ret, ns_ast_state_loc(ctx, n->state));
            if (ns_return_is_error(cast_ret)) return cast_ret;
            ret = cast_ret.r;
        }
        if (!ns_type_match(vm, fn->ret, ret.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "return type mismatch.", fn->ret, ret.t));
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
    case NS_TOKEN_RETURN: {
        ns_return_value ret = ns_eval_return_stmt(vm, ctx, i);
        if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
    } break;
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

// Iterator protocol: invoke the resolved `next(it): bool` with the loop
// subject as its single argument. The subject value is passed as-is (its
// storage view), so field writes inside `next` advance the subject in place.
static ns_return_value ns_eval_call_gen_next(ns_vm *vm, ns_ast_ctx *ctx, i32 fn_i, ns_value subject) {
    ns_symbol *sym = &vm->symbols[fn_i];
    ns_fn_symbol *fn = &sym->fn;

    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->ret, true), .o = 0};
    i32 ret_size = ns_type_size(vm, fn->ret);
    if (ret_size > 0) ret_val.o = ns_eval_alloc(vm, ret_size);

    ns_call call = (ns_call){.callee = sym, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = 1};
    ns_scope_enter(vm);
    ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->args[0].name, .val = subject, .parsed = true};
    ns_array_push(vm->symbol_stack, arg);
    ns_array_push(vm->call_stack, call);
    ns_return_void ret = ns_eval_compound_stmt(vm, fn->ctx ? fn->ctx : ctx, fn->body);
    if (ns_return_is_error(ret)) {
        (void)ns_array_pop(vm->call_stack);
        ns_scope_exit(vm);
        return ns_return_change_type(value, ret);
    }
    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);
    return ns_return_ok(value, call.ret);
}

// Custom formatting hook: resolve `fn to_str(value: T) str` by the value's
// type and invoke it, returning the produced string as a non-owning view into
// the vm's interned string storage (dynamic=0, matching ns_fmt_value's string
// case). Returns a null ns_str (data == NULL) when no matching to_str exists or
// the call fails, so the caller falls back to built-in formatting. Resolution
// is by parameter type, so several user types may each declare their own
// to_str (a plain-name lookup would only ever find the first).
ns_str ns_eval_to_str(ns_vm *vm, ns_value v) {
    i32 fn_i = -1;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        if (s->type != NS_SYMBOL_FN || !ns_str_equals_STR(s->name, "to_str")) continue;
        if (ns_array_length(s->fn.args) != 1) continue;
        ns_type at = s->fn.args[0].val.t;
        if (at.type != v.t.type || ns_type_index(at) != ns_type_index(v.t)) continue;
        if (!ns_type_is(s->fn.ret, NS_TYPE_STRING)) continue;
        if (s->fn.fn.t.ref || s->fn.fn_type == NS_FN_ASYNC || !s->fn.ctx) continue; // script fns only
        fn_i = i;
        break;
    }
    if (fn_i < 0) return ns_str_null;

    ns_symbol *sym = &vm->symbols[fn_i];
    ns_fn_symbol *fn = &sym->fn;

    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->ret, true), .o = 0};
    i32 ret_size = ns_type_size(vm, fn->ret);
    if (ret_size > 0) ret_val.o = ns_eval_alloc(vm, ret_size);

    ns_call call = (ns_call){.callee = sym, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = 1};
    ns_scope_enter(vm);
    ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->args[0].name, .val = v, .parsed = true};
    ns_array_push(vm->symbol_stack, arg);
    ns_array_push(vm->call_stack, call);
    ns_return_void ret = ns_eval_compound_stmt(vm, fn->ctx, fn->body);
    if (ns_return_is_error(ret)) {
        // fall back to built-in formatting; ns_fmt_value has no error channel
        (void)ns_array_pop(vm->call_stack);
        ns_scope_exit(vm);
        return ns_str_null;
    }
    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);

    // The bytes live in vm->str_list and outlive this call; return a non-owning
    // view so the fmt caller's ns_str_free won't release interned storage.
    ns_str s = ns_eval_str(vm, call.ret);
    return (ns_str){.data = s.data, .len = s.len, .dynamic = 0};
}

ns_return_void ns_eval_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *gen = &ctx->nodes[n->for_stmt.generator];

    ns_scope_enter(vm);
    if (gen->gen_expr.range) {
        ns_return_value ret_from = ns_eval_expr(vm, ctx, gen->gen_expr.from);
        if (ns_return_is_error(ret_from)) {
            ns_scope_exit(vm);
            return ns_return_change_type(void, ret_from);
        }
        ns_value from = ret_from.r;

        ns_return_value ret_to = ns_eval_expr(vm, ctx, gen->gen_expr.to);
        if (ns_return_is_error(ret_to)) {
            ns_scope_exit(vm);
            return ns_return_change_type(void, ret_to);
        }
        ns_value to = ret_to.r;

        ns_str name = gen->gen_expr.name.val;

        i32 ii = ns_array_length(vm->symbol_stack);
        ns_array_push(vm->symbol_stack, ((ns_symbol){.type = NS_SYMBOL_VALUE, .name = name, .val = { .t = ns_type_i32 }, .parsed = true}));
        i32 from_i = ns_eval_number_i32(vm, from);
        i32 to_i = ns_eval_number_i32(vm, to);
        for (i32 g_i = from_i; g_i < to_i; ++g_i) {
            ns_symbol *index = &vm->symbol_stack[ii];
            index->val.i32 = g_i;
            ns_scope_enter(vm);
            ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->for_stmt.body);
            ns_scope_exit(vm);
            if (ns_return_is_error(ret)) {
                ns_scope_exit(vm);
                return ret;
            }
            if (ns_loop_should_stop(vm)) break; // break or return
        }
    } else {
        ns_return_value ret_sub = ns_eval_expr(vm, ctx, gen->gen_expr.from);
        if (ns_return_is_error(ret_sub)) {
            ns_scope_exit(vm);
            return ns_return_change_type(void, ret_sub);
        }
        ns_value subject = ret_sub.r;
        ns_str name = gen->gen_expr.name.val;
        i32 ii = ns_array_length(vm->symbol_stack);

        if (ns_type_is_array(subject.t)) {
            ns_type elem_t = subject.t;
            elem_t.array = false;
            elem_t.stack = false;
            elem_t.mut = true;
            i32 stride = ns_type_size(vm, elem_t);
            ns_array_push(vm->symbol_stack, ((ns_symbol){.type = NS_SYMBOL_VALUE, .name = name, .val = { .t = elem_t }, .parsed = true}));
            for (i32 e_i = 0;; ++e_i) {
                // re-read base and length each round: the body may grow the
                // array and relocate its heap storage
                void *base = ns_eval_array_data(vm, subject);
                i32 len = base ? (i32)ns_buffer_len(base) : 0;
                if (e_i >= len) break;
                u8 *data = (u8 *)base + (u64)stride * (u64)e_i;
                // mirror ns_eval_index_expr: reference-typed elements keep a
                // u64 handle in the slot, so a read loads the handle
                ns_value ev = (ns_type_is_array(elem_t) || ns_type_is(elem_t, NS_TYPE_STRING) ||
                               ns_type_is(elem_t, NS_TYPE_DICT) || ns_type_is(elem_t, NS_TYPE_SET) ||
                               ns_type_is(elem_t, NS_TYPE_FN) || ns_type_is(elem_t, NS_TYPE_TASK))
                                  ? (ns_value){.t = elem_t, .o = *(u64 *)data}
                                  : (ns_value){.t = elem_t, .o = (u64)data};
                vm->symbol_stack[ii].val = ev;
                ns_scope_enter(vm);
                ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->for_stmt.body);
                ns_scope_exit(vm);
                if (ns_return_is_error(ret)) {
                    ns_scope_exit(vm);
                    return ret;
                }
                if (ns_loop_should_stop(vm)) break; // break or return
            }
        } else if (ns_type_is(subject.t, NS_TYPE_STRING)) {
            ns_array_push(vm->symbol_stack, ((ns_symbol){.type = NS_SYMBOL_VALUE, .name = name, .val = { .t = ns_type_i32 }, .parsed = true}));
            for (i32 c_i = 0;; ++c_i) {
                // re-read per round: the body may push strings and relocate
                // the interned string storage backing the subject
                ns_str s = ns_eval_str(vm, subject);
                if (c_i >= s.len) break;
                // each byte as an i32 code, like s[i]
                vm->symbol_stack[ii].val = (ns_value){.t = ns_type_i32, .i32 = (i32)(u8)s.data[c_i]};
                ns_scope_enter(vm);
                ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->for_stmt.body);
                ns_scope_exit(vm);
                if (ns_return_is_error(ret)) {
                    ns_scope_exit(vm);
                    return ret;
                }
                if (ns_loop_should_stop(vm)) break; // break or return
            }
        } else if (ns_type_is(subject.t, NS_TYPE_DICT) || ns_type_is(subject.t, NS_TYPE_SET)) {
            // dict/set subject: bind each key (a set's element, a dict's key;
            // a dict body reads the value with d[k]). Walk the open-addressed
            // slots. The table is fixed-capacity so its pointer stays stable
            // across body mutations; only occupied slots (state 1) are visited.
            ns_type key_t = vm->symbols[ns_type_index(subject.t)].ct.key;
            ns_array_push(vm->symbol_stack, ((ns_symbol){.type = NS_SYMBOL_VALUE, .name = name, .val = { .t = key_t }, .parsed = true}));
            ns_dict_table *table = ns_eval_dict_table(vm, subject);
            i32 cap = table ? table->cap : 0;
            for (i32 s_i = 0; s_i < cap; ++s_i) {
                u8 *slot = ns_eval_dict_slot(table, s_i);
                if (slot[0] != 1) continue; // skip empty (0) and tombstone (2)
                vm->symbol_stack[ii].val = ns_eval_slot_value(table, slot + table->key_off, table->key_type);
                ns_scope_enter(vm);
                ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->for_stmt.body);
                ns_scope_exit(vm);
                if (ns_return_is_error(ret)) {
                    ns_scope_exit(vm);
                    return ret;
                }
                if (ns_loop_should_stop(vm)) break; // break or return
            }
        } else {
            // struct subject: `next(it): bool` advances, `value` carries the
            // element; both were resolved by ns_vm_parse_gen_expr
            ns_symbol *st = &vm->symbols[ns_type_index(subject.t)];
            ns_struct_field *field = &st->st.fields[gen->gen_expr.rt.value_field];
            ns_type vt = field->t;
            vt.stack = ns_type_in_stack(subject.t);
            vt.mut = true;
            u64 field_o = field->o;
            ns_array_push(vm->symbol_stack, ((ns_symbol){.type = NS_SYMBOL_VALUE, .name = name, .val = { .t = vt }, .parsed = true}));
            while (1) {
                // scope the call so its bool slot is reclaimed every round
                ns_scope_enter(vm);
                ns_return_value ret_next = ns_eval_call_gen_next(vm, ctx, gen->gen_expr.rt.next_fn, subject);
                if (ns_return_is_error(ret_next)) {
                    ns_scope_exit(vm);
                    ns_scope_exit(vm);
                    return ns_return_change_type(void, ret_next);
                }
                ns_bool go = ns_eval_bool(vm, ret_next.r);
                ns_scope_exit(vm);
                if (!go) break;
                // bind the loop var to the subject's value field, matching
                // member-expr addressing (stack vs absolute)
                vm->symbol_stack[ii].val = (ns_value){.t = vt, .o = subject.o + field_o};
                ns_scope_enter(vm);
                ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->for_stmt.body);
                ns_scope_exit(vm);
                if (ns_return_is_error(ret)) {
                    ns_scope_exit(vm);
                    return ret;
                }
                if (ns_loop_should_stop(vm)) break; // break or return
            }
        }
    }
    ns_scope_exit(vm);
    return ns_return_ok_void;
}

ns_return_void ns_eval_loop_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->loop_stmt.do_first) {
        do {
            ns_scope_enter(vm);
            ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->loop_stmt.body);
            ns_scope_exit(vm);
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

            ns_scope_enter(vm);
            ns_return_void ret = ns_eval_compound_stmt(vm, ctx, n->loop_stmt.body);
            ns_scope_exit(vm);
            if (ns_return_is_error(ret)) return ret;
            if (ns_loop_should_stop(vm)) break; // break or return
        }
    }
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

    // && and || short-circuit on bool operands: the right side must not be
    // evaluated when the left side already decides, so guards like
    // `i < n && arr[i] == x` are safe. Non-bool operands (operator
    // overrides) keep the eager path below.
    if (n->binary_expr.op.type == NS_TOKEN_LOGIC_OP && ns_type_is(l.t, NS_TYPE_BOOL)) {
        ns_bool lb = ns_eval_bool(vm, l);
        ns_bool is_and = n->binary_expr.op.val.data[0] == '&';
        if (is_and != lb) return ns_return_ok(value, lb ? ns_true : ns_false);
        ns_return_value ret_sc = ns_eval_expr(vm, ctx, n->binary_expr.right);
        if (ns_return_is_error(ret_sc)) return ns_return_change_type(value, ret_sc);
        return ns_return_ok(value, ns_eval_bool(vm, ret_sc.r) ? ns_true : ns_false);
    }

    ns_return_value ret_r = ns_eval_expr(vm, ctx, n->binary_expr.right);
    if (ns_return_is_error(ret_r)) return ns_return_change_type(value, ret_r);
    ns_value r = ret_r.r;

    l = ns_eval_enum_underlying_value(vm, l);
    r = ns_eval_enum_underlying_value(vm, r);

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

    return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_pair_mismatch_msg(vm, "binary expr type mismatch.", "left", l.t, "right", r.t));
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
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT: {
        // A format string without interpolation is a plain literal. Escape
        // sequences decode once, when the literal becomes a runtime value;
        // consumers (print/write/FFI) receive the real bytes.
        if (t.val.len > 0 && memchr(t.val.data, '\\', (size_t)t.val.len)) {
            ns_str u = ns_str_unescape(t.val);
            ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, u)};
            ns_str_free(u);
        } else {
            ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, t.val)};
        }
    } break;
    case NS_TOKEN_TRUE: ret = ns_true; break;
    case NS_TOKEN_FALSE: ret = ns_false; break;
    case NS_TOKEN_IDENTIFIER: ret = ns_eval_find_value_cached(vm, t.val, &n->primary_expr.rt.cache); break;
    default: {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented primary expr type.");
    } break;
    }
    return ns_return_ok(value, ret);
}

// Append a literal segment with its escape sequences decoded. Only literal
// text is decoded — interpolated values pass through verbatim.
static void ns_eval_append_unescaped(ns_str *ret, ns_str seg) {
    if (seg.len == 0) return;
    if (memchr(seg.data, '\\', (size_t)seg.len) == NULL) {
        ns_str_append(ret, seg);
        return;
    }
    ns_str u = ns_str_unescape(seg);
    ns_str_append(ret, u);
    ns_str_free(u);
}

ns_return_value ns_eval_str_fmt_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    i32 count = n->str_fmt.expr_count;
    ns_str fmt = n->str_fmt.fmt;

    ns_str ret = {.data = ns_null, .len = 0, .dynamic = 1};
    ns_array_set_capacity(ret.data, fmt.len);

    i32 j = 0;
    i32 lit_start = 0;
    ns_ast_t *expr = n;
    while (j < fmt.len) {
        if (fmt.data[j] == '{' && (j == 0 || (j > 0 && fmt.data[j - 1] != '\\'))) {
            ns_eval_append_unescaped(&ret, ns_str_slice(fmt, lit_start, j));
            ++j;
            while (j < fmt.len && fmt.data[j] != '}') {
                j++;
            }
            j++;

            if (j > fmt.len) {
                return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "missing '}'.");
            }
            lit_start = j;

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
            j++;
        }
    }
    ns_eval_append_unescaped(&ret, ns_str_slice(fmt, lit_start, j));

    ret.len = ns_array_length(ret.data);
    ns_value ret_v = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, ret)};
    ns_array_free(ret.data);
    return ns_return_ok(value, ret_v);
}

// Store `val` into `field` of the struct at stack offset `o`, with the same
// type tolerance designated exprs use (numeric fields accept any numeric
// value; a union field stores the concrete member). Shared by designated
// exprs and positional constructors. The base pointer is re-derived from the
// offset because evaluating a field expression may reallocate vm->stack.
static ns_return_value ns_eval_store_struct_field(ns_vm *vm, i32 o, ns_struct_field *field, ns_value val, ns_code_loc loc) {
    ns_bool number_ok = !ns_type_is(field->t, NS_TYPE_ENUM) &&
                        ns_type_is_number(ns_enum_underlying_type(vm, field->t)) &&
                        ns_type_is_number(ns_enum_underlying_type(vm, val.t));
    if (!number_ok && !ns_type_equals(field->t, val.t) && !ns_type_match(vm, field->t, val.t)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "field type mismatch.", field->t, val.t));
    }

    // For a union field, store the concrete member that was actually provided.
    ns_type t = ns_type_is(field->t, NS_TYPE_UNION) ? val.t : field->t;
    if (ns_type_is(t, NS_TYPE_ENUM)) {
        t = ns_enum_underlying_type(vm, t);
        val = ns_eval_enum_underlying_value(vm, val);
    }
    i8 *dst = &vm->stack[o] + field->o;
    if (ns_type_is_array(t)) {
        *(u64*)dst = (u64)ns_eval_array_raw(vm, val);
    } else if (ns_type_is_number(t)) {
        switch (t.type) {
        case NS_TYPE_U8:   *(u8*)dst = ns_eval_number_i8(vm, val); break;
        case NS_TYPE_I8:   *(i8*)dst = ns_eval_number_i8(vm, val); break;
        case NS_TYPE_U16: *(u16*)dst = ns_eval_number_u16(vm, val); break;
        case NS_TYPE_I16: *(i16*)dst = ns_eval_number_i16(vm, val); break;
        case NS_TYPE_U32: *(u32*)dst = ns_eval_number_u32(vm, val); break;
        case NS_TYPE_I32: *(i32*)dst = ns_eval_number_i32(vm, val); break;
        case NS_TYPE_U64: *(u64*)dst = ns_eval_number_u64(vm, val); break;
        case NS_TYPE_I64: *(i64*)dst = ns_eval_number_i64(vm, val); break;
        case NS_TYPE_F32: *(f32*)dst = ns_eval_number_f32(vm, val); break;
        case NS_TYPE_F64: *(f64*)dst = ns_eval_number_f64(vm, val); break;
        case NS_TYPE_BOOL: *(ns_bool*)dst = ns_eval_bool(vm, val); break;
        default:
            break;
        }
    } else if (ns_type_is(t, NS_TYPE_STRUCT)) {
        // copy the nested struct into its own field slot, sized by the field
        i32 size = ns_type_size(vm, t);
        if (ns_type_in_stack(val.t)) {
            memcpy(dst, &vm->stack[val.o], (size_t)size);
        } else {
            memcpy(dst, (void*)val.o, (size_t)size);
        }
    } else if (ns_type_is(t, NS_TYPE_STRING)) {
        // A string value is a str_list handle: immediate in .o, or stored
        // in a stack slot — the same representation assignment stores.
        *(u64*)dst = ns_type_in_stack(val.t) ? *(u64 *)&vm->stack[val.o] : val.o;
    } else {
        return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented field type.");
    }
    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_str st_name = n->desig_expr.name.val;
    ns_symbol *st = ns_vm_find_symbol_cached(vm, st_name, &n->desig_expr.rt.cache);

    if (ns_null == st) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown struct.");

    i32 st_index = (i32)(st - vm->symbols);
    i32 stride = st->st.stride;
    i32 o = ns_eval_alloc(vm, stride);
    memset(&vm->stack[o], 0, stride);

    u64 offset = o + stride;

    ns_ast_t *expr = n;
    for (i32 e_i = 0, l = n->desig_expr.count; e_i < l; e_i++) {
        expr = &ctx->nodes[expr->next];
        ns_str name = expr->field_def.name.val;

        // re-resolve each iteration: evaluating a field expression may grow
        // vm->symbols and move the struct symbol
        st = &vm->symbols[st_index];
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

        ns_return_value ret_store = ns_eval_store_struct_field(vm, o, &vm->symbols[st_index].st.fields[field - st->st.fields], ret_val.r, ns_ast_state_loc(ctx, expr->state));
        if (ns_return_is_error(ret_store)) return ret_store;
    }

    ns_array_set_length(vm->stack, offset);
    st = &vm->symbols[st_index];
    ns_value ret = (ns_value){.t = ns_type_set_stack(st->st.st.t, true), .o = o};
    return ns_return_ok(value, ret);
}

// Positional struct construction: `point(0, 0)` fills the fields in
// declaration order, one argument per field.
static ns_return_value ns_eval_struct_ctor(ns_vm *vm, ns_ast_ctx *ctx, i32 i, i32 st_index) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *st = &vm->symbols[st_index];
    i32 field_count = (i32)ns_array_length(st->st.fields);
    if (n->call_expr.arg_count != field_count) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "struct constructor argument count mismatch.");
    }

    i32 stride = st->st.stride;
    i32 o = ns_eval_alloc(vm, stride);
    memset(&vm->stack[o], 0, stride);
    u64 offset = o + stride;

    i32 next = n->next;
    for (i32 a_i = 0; a_i < field_count; ++a_i) {
        ns_ast_t *arg = &ctx->nodes[next];
        i32 arg_next = arg->next;
        ns_code_loc loc = ns_ast_state_loc(ctx, arg->state);

        ns_return_value ret_val = ns_eval_expr(vm, ctx, next);
        if (ns_return_is_error(ret_val)) return ret_val;

        // re-resolve: evaluating an argument may grow vm->symbols
        ns_return_value ret_store = ns_eval_store_struct_field(vm, o, &vm->symbols[st_index].st.fields[a_i], ret_val.r, loc);
        if (ns_return_is_error(ret_store)) return ret_store;
        next = arg_next;
    }

    ns_array_set_length(vm->stack, offset);
    st = &vm->symbols[st_index];
    ns_value ret = (ns_value){.t = ns_type_set_stack(st->st.st.t, true), .o = o};
    return ns_return_ok(value, ret);
}

static ns_return_value ns_eval_cast_enum(ns_vm *vm, ns_value v, ns_type enum_t, ns_code_loc loc) {
    ns_type underlying = ns_enum_underlying_type(vm, enum_t);
    v = ns_eval_enum_underlying_value(vm, v);
    if (!ns_type_is_number(v.t) || ns_type_is_float(v.t) || ns_type_is(v.t, NS_TYPE_BOOL)) {
        return ns_return_error(value, loc, NS_ERR_EVAL, "enum cast requires an integer value.");
    }

    i32 bits = ns_type_size(vm, underlying) * 8;
    ns_bool fits = true;
    if (ns_type_unsigned(underlying)) {
        if (ns_type_signed(v.t) && ns_eval_to_i64(vm, v) < 0) fits = false;
        u64 max = bits == 64 ? UINT64_MAX : ((1ULL << bits) - 1);
        if (ns_eval_to_u64(vm, v) > max) fits = false;
    } else {
        i64 min = bits == 64 ? INT64_MIN : -(1LL << (bits - 1));
        i64 max = bits == 64 ? INT64_MAX : ((1LL << (bits - 1)) - 1);
        if (ns_type_unsigned(v.t)) {
            if (ns_eval_to_u64(vm, v) > (u64)max) fits = false;
        } else {
            i64 n = ns_eval_to_i64(vm, v);
            if (n < min || n > max) fits = false;
        }
    }
    if (!fits) return ns_return_error(value, loc, NS_ERR_EVAL, "integer value is outside enum range.");

    ns_return_value cast = ns_eval_cast_number(vm, v, underlying, loc);
    if (ns_return_is_error(cast)) return cast;
    cast.r.t = ns_type_set_stack(ns_type_set_mut(enum_t, false), false);
    return cast;
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
    if (ns_type_is(t, NS_TYPE_ENUM)) {
        return ns_eval_cast_enum(vm, v, t, ns_ast_state_loc(ctx, n->state));
    }
    if (ns_type_is_number(t) && ns_type_is_number(ns_enum_underlying_type(vm, v.t))) {
        return ns_eval_cast_number(vm, v, t, ns_ast_state_loc(ctx, n->state));
    } else {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unimplemented cast expr.");
    }

    return ns_return_ok(value, ns_nil);
}

ns_return_value ns_eval_member_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];

    ns_ast_t *left = &ctx->nodes[n->member_expr.left];
    if (left->type == NS_AST_PRIMARY_EXPR && left->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        ns_symbol *candidate = ns_vm_find_symbol(vm, left->primary_expr.token.val, false);
        if (candidate && candidate->type == NS_SYMBOL_ENUM) {
            ns_ast_t *member = &ctx->nodes[n->member_expr.right];
            if (member->type != NS_AST_PRIMARY_EXPR) {
                return ns_return_error(value, ns_ast_state_loc(ctx, member->state), NS_ERR_EVAL,
                                       "invalid enum member expression.");
            }
            i32 member_i = ns_enum_member_index(candidate, member->primary_expr.token.val);
            if (member_i < 0) {
                return ns_return_error(value, ns_ast_state_loc(ctx, member->state), NS_ERR_EVAL,
                                       "unknown enum member.");
            }
            ns_value value = {.t = ns_type_set_stack(candidate->en.t, false),
                              .u64 = candidate->en.members[member_i].value};
            return ns_return_ok(value, value);
        }
    }

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
    if (n->unary_expr.op.type == NS_TOKEN_ADD_OP || n->unary_expr.op.type == NS_TOKEN_BIT_INVERT_OP) {
        v = ns_eval_enum_underlying_value(vm, v);
    }

    ns_token_t op = n->unary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        if (!ns_type_is_number(v.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_label_msg(vm, "unary expr type mismatch.", "number", v.t));
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
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_label_msg(vm, "unary expr type mismatch.", "'-'", v.t));
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
    case NS_TOKEN_AWAIT: // suspend until the task operand completes, yield its result
        return ns_task_await(vm, v, ns_ast_state_loc(ctx, n->state));
    case NS_TOKEN_REF:
        if (ns_type_is_ref(v.t)) return ns_return_ok(value, v);
        if (ns_type_is_const(v.t)) return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "cannot take reference of const value.");
        // A reference to a stack value must retain the referent's offset.  The
        // old implementation allocated only a pointer-sized temporary and then
        // copied the complete value into it.  Structs larger than a pointer
        // overflowed that allocation, and the temporary could be reclaimed or
        // overwritten before an FFI call used it.
        v.t = ns_type_encode(v.t.type, v.t.index, true, true, true);
        return ns_return_ok(value, v);
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
                    return ns_return_error(value, ns_ast_state_loc(ctx, elem.state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "array literal element type mismatch.", element_type, elem_v.t));
                }
                if (!ns_type_is(element_type, NS_TYPE_ENUM) &&
                    ns_type_is_number(ns_enum_underlying_type(vm, element_type)) &&
                    ns_type_is_number(ns_enum_underlying_type(vm, elem_v.t))) {
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
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "dict key type mismatch.", dict->ct.key, index.t));
        }
        return ns_eval_dict_find(vm, table, index, create, ns_ast_state_loc(ctx, n->state));
    }

    if (!ns_type_is_number(index.t)) {
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_label_msg(vm, "index expr type mismatch.", "number", index.t));
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
        return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_label_msg(vm, "index expr type mismatch.", "array, dict, or string", table.t));
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
    // Reference-typed elements (strings, nested arrays, dicts, sets, fns,
    // tasks) keep a u64 handle in the slot. A read loads the handle so the
    // value carries the immediate representation every consumer of non-stack
    // reference values expects; only an assignment target keeps the slot
    // address for the store.
    if (!create && (ns_type_is_array(element_type) || ns_type_is(element_type, NS_TYPE_STRING) ||
                    ns_type_is(element_type, NS_TYPE_DICT) || ns_type_is(element_type, NS_TYPE_SET) ||
                    ns_type_is(element_type, NS_TYPE_FN) || ns_type_is(element_type, NS_TYPE_TASK))) {
        ns_value val = (ns_value){.t = element_type, .o = *(u64 *)data};
        return ns_return_ok(value, val);
    }
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
    } else if (ns_type_is(dst_t, NS_TYPE_UNION) || ns_type_is(dst_t, NS_TYPE_FN) || ns_type_is(dst_t, NS_TYPE_TASK)) {
        ns_bool ok = ns_type_match(vm, dst_t, v.t);
        // a task handle is self-describing, so a task[R] slot also accepts a
        // generic `task` value (e.g. one produced without a parse pass)
        if (!ok && ns_type_is(dst_t, NS_TYPE_TASK)) ok = ns_type_match(vm, v.t, dst_t);
        if (!ok) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "var def type mismatch.", dst_t, v.t));
        }
        store_t = v.t;
    } else if (!ns_type_equals(dst_t, v.t)) {
        if (!ns_eval_number_assign_compatible(vm, dst_t, v.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "var def type mismatch.", dst_t, v.t));
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
    } else if (ns_type_is(dst_t, NS_TYPE_UNION) || ns_type_is(dst_t, NS_TYPE_FN) || ns_type_is(dst_t, NS_TYPE_TASK)) {
        ns_bool ok = ns_type_match(vm, dst_t, src.t);
        // a task handle is self-describing, so a task[R] slot also accepts a
        // generic `task` value (e.g. one produced without a parse pass)
        if (!ok && ns_type_is(dst_t, NS_TYPE_TASK)) ok = ns_type_match(vm, src.t, dst_t);
        if (!ok) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "local var def type mismatch.", dst_t, src.t));
        }
        store_t = src.t;
    } else if (!ns_type_equals(dst_t, src.t)) {
        if (!ns_eval_number_assign_compatible(vm, dst_t, src.t)) {
            return ns_return_error(value, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_eval_type_mismatch_msg(vm, "local var def type mismatch.", dst_t, src.t));
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
        // task safepoint: offer the vm lock to other tasks once per time slice
        // and unwind when the current task has been cancelled
        if (vm->task_rt && !ns_task_step(vm)) {
            vm->stack_depth = stack_depth;
            return ns_return_ok_void;
        }
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

static ns_return_value ns_eval_ast_impl(ns_vm *vm, ns_ast_ctx *ctx);

// Top-level evaluation entry. Re-acquires the vm lock when a previous eval in
// this vm started the task runtime (REPL), and on exit cancels + joins tasks
// still running: tasks never outlive the ast they interpret.
ns_return_value ns_eval_ast(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_task_eval_enter(vm);
    ns_return_value ret = ns_eval_ast_impl(vm, ctx);
    ns_task_eval_exit(vm);
    return ret;
}

static ns_return_value ns_eval_ast_impl(ns_vm *vm, ns_ast_ctx *ctx) {
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
            case NS_AST_CALL_EXPR:
            case NS_AST_BINARY_EXPR: { // top-level assignment statements
                ns_return_value ret = ns_eval_expr(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_VAR_DEF: {
                // Top-level lets are module globals even without a main fn,
                // so fns can read and assign them like in any other script.
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
            case NS_AST_USE_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OP_FN_DEF:
            case NS_AST_STRUCT_DEF:
            case NS_AST_TYPE_DEF:
            case NS_AST_ENUM_DEF:
                break; // already parsed, no need to re-evaluate
            default: {
                ns_code_loc loc = (ns_code_loc){.f = ctx->filename, .l = n->state.l, .o = n->state.o};
                return ns_return_error(value, loc, NS_ERR_EVAL, "unimplemented global ast.");
            } break;
            }
        }
        ns_scope_exit(vm);
        (void)ns_array_pop(vm->call_stack);
        main_ret = (ns_value){.t = ns_type_i32, .i32 = 0};
    } else {
        for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
            i32 s_i = ctx->sections[i];
            ns_ast_t *n = &ctx->nodes[s_i];
            switch (n->type) {
            case NS_AST_EXPR:
            case NS_AST_CALL_EXPR:
            case NS_AST_BINARY_EXPR: { // top-level assignment statements
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
            case NS_AST_ENUM_DEF:
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
        f64 profile_start_ms = ns_profile.enabled ? ns_profile_now_ms() : 0.0;
        i32 profile_depth = ns_profile.enabled ? (i32)ns_array_length(vm->call_stack) - 1 : 0;
        ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn->fn_def.body);
        ns_eval_profile_scope_end(main_fn, profile_depth, profile_start_ms);
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
