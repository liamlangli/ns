#include "ns_ssa.h"
#include "ns_os.h"
#include "ns_vm.h"
#include "ns_shader.h"

#ifdef NS_DEBUG
    #define NS_SSA_REF_PATH "lib"
#else
    #define NS_SSA_REF_PATH "ns/ref"
#endif

typedef struct ns_ssa_binding {
    ns_str name;
    i32 value;
    ns_type type;
} ns_ssa_binding;

typedef struct ns_ssa_global_const {
    ns_str name;
    ns_token_t token;
    ns_type type;
} ns_ssa_global_const;

typedef struct ns_ssa_loop_ctx {
    i32 break_block;
    i32 continue_block;
    i32 phi_block; /* header whose phis must be flushed on break */
    ns_str for_iter; /* range or foreach index: continue increments it */
    i32 foreach_subject;
    ns_str foreach_elem;
    i32 foreach_stride;    /* >0: write the loop var back through ARRAY_STORE */
    i32 foreach_field_off; /* >=0: write the loop var back to subject.value */
} ns_ssa_loop_ctx;

static ns_ssa_loop_ctx ns_ssa_loop_ctx_init(i32 break_block, i32 continue_block) {
    return (ns_ssa_loop_ctx){
        .break_block = break_block,
        .continue_block = continue_block,
        .phi_block = continue_block,
        .for_iter = ns_str_null,
        .foreach_subject = -1,
        .foreach_elem = ns_str_null,
        .foreach_stride = 0,
        .foreach_field_off = -1,
    };
}

typedef struct ns_ssa_capture {
    ns_str name;
    i32 offset;
    ns_type type;
} ns_ssa_capture;

typedef struct ns_ssa_builder {
    ns_ast_ctx *ctx;
    ns_vm *vm;
    ns_ssa_module *m;
    ns_ssa_fn *fn;
    i32 block;
    i32 next_value;
    ns_ssa_binding *env;
    ns_ssa_global_const *globals;
    ns_str *import_seen;
    ns_ssa_loop_ctx *loops;
    i32 capture_env;
    ns_ssa_capture *captures;
    ns_str *ref_names;
    ns_str *shader_only;
    ns_return_str fold_error;
} ns_ssa_builder;

#define NS_SSA_INST_INIT(kind, ast_i) ((ns_ssa_inst){ \
    .op = (kind), \
    .dst = -1, \
    .a = -1, \
    .b = -1, \
    .c = -1, \
    .target0 = -1, \
    .target1 = -1, \
    .ast = (ast_i), \
    .type = ns_type_unknown, \
    .name = ns_str_null, \
    .module = ns_str_null, \
    .token = (ns_token_t){0}, \
})

static i32 ns_ssa_lower_expr(ns_ssa_builder *b, i32 i);
static void ns_ssa_lower_stmt(ns_ssa_builder *b, i32 i);
static void ns_ssa_lower_compound(ns_ssa_builder *b, i32 i);
static i32 ns_ssa_emit_i32_const(ns_ssa_builder *b, i32 n, i32 ast);
static i32 ns_ssa_env_get_value(ns_ssa_binding *env, ns_str name, i32 fallback);
static ns_type ns_ssa_value_type(ns_ssa_builder *b, i32 value);
static i32 ns_ssa_wasm32_size(ns_ssa_builder *b, ns_type type);
static i32 ns_ssa_wasm32_align(i32 offset, i32 size);
static i32 ns_ssa_wasm32_field_offset(ns_ssa_builder *b, ns_symbol *st, i32 field);
static i32 ns_ssa_clone_struct(ns_ssa_builder *b, i32 value, i32 ast);
static i32 ns_ssa_global_index(ns_ssa_builder *b, ns_str name);
static void ns_ssa_env_bind(ns_ssa_builder *b, ns_str name, i32 value, ns_type t);
static void ns_ssa_flush_captures(ns_ssa_builder *b, i32 ast);
static ns_str ns_ssa_ensure_block_fn(ns_ssa_builder *b, i32 symbol_index);
static i32 ns_ssa_named_fn_value(ns_ssa_builder *b, ns_symbol *sym, i32 ast);
static i32 ns_ssa_emit_i32_const(ns_ssa_builder *b, i32 n, i32 ast);
static i32 ns_ssa_emit_named_call(ns_ssa_builder *b, const char *name, i32 *args, i32 nargs,
                                 ns_type ret, i32 ast);
static void ns_ssa_emit_store_off(ns_ssa_builder *b, i32 obj, i32 val, i32 off, ns_type t, i32 ast);

static ns_type ns_ssa_type_from_token(ns_token_type t) {
    switch (t) {
    case NS_TOKEN_TYPE_I8: return ns_type_i8;
    case NS_TOKEN_TYPE_I16: return ns_type_i16;
    case NS_TOKEN_TYPE_I32: return ns_type_i32;
    case NS_TOKEN_TYPE_I64: return ns_type_i64;
    case NS_TOKEN_TYPE_U8: return ns_type_u8;
    case NS_TOKEN_TYPE_U16: return ns_type_u16;
    case NS_TOKEN_TYPE_U32: return ns_type_u32;
    case NS_TOKEN_TYPE_U64: return ns_type_u64;
    case NS_TOKEN_TYPE_F32: return ns_type_f32;
    case NS_TOKEN_TYPE_F64: return ns_type_f64;
    case NS_TOKEN_TYPE_BOOL: return ns_type_bool;
    case NS_TOKEN_TYPE_STR: return ns_type_str;
    case NS_TOKEN_TYPE_VOID: return ns_type_void;
    default: return ns_type_unknown;
    }
}

static ns_type ns_ssa_type_from_label(ns_ssa_builder *b, i32 type_label_ast) {
    if (type_label_ast <= 0) return ns_type_unknown;
    ns_ast_t *n = &b->ctx->nodes[type_label_ast];
    if (n->type != NS_AST_TYPE_LABEL) return ns_type_unknown;
    if (n->type_label.is_fn || n->type_label.is_array) return ns_type_unknown;
    ns_type t = ns_ssa_type_from_token(n->type_label.name.type);
    if (!ns_type_is(t, NS_TYPE_UNKNOWN)) return t;
    ns_symbol *s = ns_vm_find_symbol(b->vm, n->type_label.name.val, false);
    if (s && s->type == NS_SYMBOL_ENUM) return s->en.underlying;
    if (s && s->type == NS_SYMBOL_STRUCT) return s->st.st.t;
    if (s && s->type == NS_SYMBOL_UNION) return s->un.t;
    if (s && s->type == NS_SYMBOL_CONTAINER) return s->ct.t;
    if (s && s->type == NS_SYMBOL_FN) return s->fn.fn.t;
    return ns_type_unknown;
}

static ns_ast_t *ns_ssa_unwrap_expr(ns_ssa_builder *b, i32 i) {
    if (i <= 0) return ns_null;
    ns_ast_t *n = &b->ctx->nodes[i];
    while (n->type == NS_AST_EXPR) n = &b->ctx->nodes[n->expr.body];
    return n;
}

static ns_symbol *ns_ssa_enum_from_expr(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = ns_ssa_unwrap_expr(b, i);
    if (!n || n->type != NS_AST_PRIMARY_EXPR || n->primary_expr.token.type != NS_TOKEN_IDENTIFIER) return ns_null;
    ns_symbol *s = ns_vm_find_symbol(b->vm, n->primary_expr.token.val, false);
    return s && s->type == NS_SYMBOL_ENUM ? s : ns_null;
}

static ns_str ns_ssa_enum_literal(ns_ssa_builder *b, ns_symbol *s, u64 value) {
    char buf[32];
    i32 len;
    ns_type t = s->en.underlying;
    if (ns_type_is(t, NS_TYPE_I8) || ns_type_is(t, NS_TYPE_I16) ||
        ns_type_is(t, NS_TYPE_I32) || ns_type_is(t, NS_TYPE_I64)) {
        len = snprintf(buf, sizeof(buf), "%lld", (long long)(i64)value);
    } else {
        len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
    }
    ns_str owned = ns_str_range(ns_malloc((szt)len + 1), len);
    memcpy(owned.data, buf, (szt)len + 1);
    ns_array_push(b->m->owned_strings, owned);
    return owned;
}

ns_str ns_ssa_op_to_string(ns_ssa_op op) {
    switch (op) {
        ns_str_case(NS_SSA_OP_UNKNOWN)
        ns_str_case(NS_SSA_OP_UNDEF)
        ns_str_case(NS_SSA_OP_CONST)
        ns_str_case(NS_SSA_OP_PARAM)
        ns_str_case(NS_SSA_OP_COPY)
        ns_str_case(NS_SSA_OP_PHI)
        ns_str_case(NS_SSA_OP_CAST)
        ns_str_case(NS_SSA_OP_CALL)
        ns_str_case(NS_SSA_OP_ARG)
        ns_str_case(NS_SSA_OP_GLOBAL_GET)
        ns_str_case(NS_SSA_OP_GLOBAL_SET)
        ns_str_case(NS_SSA_OP_ALLOC)
        ns_str_case(NS_SSA_OP_CLONE)
        ns_str_case(NS_SSA_OP_LOAD)
        ns_str_case(NS_SSA_OP_STORE)
        ns_str_case(NS_SSA_OP_ARRAY_NEW)
        ns_str_case(NS_SSA_OP_ARRAY_STORE)
        ns_str_case(NS_SSA_OP_MEMBER)
        ns_str_case(NS_SSA_OP_INDEX)
        ns_str_case(NS_SSA_OP_NEG)
        ns_str_case(NS_SSA_OP_NOT)
        ns_str_case(NS_SSA_OP_ADD)
        ns_str_case(NS_SSA_OP_SUB)
        ns_str_case(NS_SSA_OP_MUL)
        ns_str_case(NS_SSA_OP_DIV)
        ns_str_case(NS_SSA_OP_MOD)
        ns_str_case(NS_SSA_OP_SHL)
        ns_str_case(NS_SSA_OP_SHR)
        ns_str_case(NS_SSA_OP_BAND)
        ns_str_case(NS_SSA_OP_BOR)
        ns_str_case(NS_SSA_OP_BXOR)
        ns_str_case(NS_SSA_OP_AND)
        ns_str_case(NS_SSA_OP_OR)
        ns_str_case(NS_SSA_OP_EQ)
        ns_str_case(NS_SSA_OP_NE)
        ns_str_case(NS_SSA_OP_LT)
        ns_str_case(NS_SSA_OP_LE)
        ns_str_case(NS_SSA_OP_GT)
        ns_str_case(NS_SSA_OP_GE)
        ns_str_case(NS_SSA_OP_ASSERT)
        ns_str_case(NS_SSA_OP_BR)
        ns_str_case(NS_SSA_OP_JMP)
        ns_str_case(NS_SSA_OP_RET)
        ns_str_case(NS_SSA_OP_TRAP)
        ns_str_case(NS_SSA_OP_FNADDR)
    default:
        return ns_str_cstr("NS_SSA_OP_UNKNOWN");
    }
}

static i32 ns_ssa_new_value(ns_ssa_builder *b) {
    i32 v = b->next_value;
    b->next_value++;
    return v;
}

static i32 ns_ssa_new_block(ns_ssa_builder *b, i32 ast) {
    ns_ssa_block block = {0};
    block.id = (i32)ns_array_length(b->fn->blocks);
    block.ast = ast;
    ns_array_push(b->fn->blocks, block);
    return block.id;
}

static void ns_ssa_connect(ns_ssa_builder *b, i32 from, i32 to) {
    ns_array_push(b->fn->blocks[from].succs, to);
    ns_array_push(b->fn->blocks[to].preds, from);
}

i32 ns_ssa_phi_incoming(ns_ssa_inst *phi, i32 pred) {
    if (!phi) return -1;
    for (i32 i = 0, l = (i32)ns_array_length(phi->phi_edges); i < l; ++i) {
        if (phi->phi_edges[i].pred == pred) return phi->phi_edges[i].value;
    }
    if (phi->target0 == pred) return phi->a;
    if (phi->target1 == pred) return phi->b;
    return -1;
}

void ns_ssa_phi_set_incoming(ns_ssa_inst *phi, i32 pred, i32 value) {
    if (!phi) return;
    for (i32 i = 0, l = (i32)ns_array_length(phi->phi_edges); i < l; ++i) {
        if (phi->phi_edges[i].pred == pred) {
            phi->phi_edges[i].value = value;
            if (phi->target0 == pred) phi->a = value;
            if (phi->target1 == pred) phi->b = value;
            return;
        }
    }
    ns_ssa_phi_edge edge = {.pred = pred, .value = value};
    ns_array_push(phi->phi_edges, edge);
    if (phi->target0 < 0 || phi->target0 == pred) {
        phi->target0 = pred;
        phi->a = value;
    } else if (phi->target1 < 0 || phi->target1 == pred) {
        phi->target1 = pred;
        phi->b = value;
    }
}

static i32 ns_ssa_emit_raw(ns_ssa_builder *b, ns_ssa_inst inst) {
    i32 idx = (i32)ns_array_length(b->fn->insts);
    ns_array_push(b->fn->insts, inst);
    ns_array_push(b->fn->blocks[b->block].insts, idx);
    if (inst.op == NS_SSA_OP_BR || inst.op == NS_SSA_OP_JMP || inst.op == NS_SSA_OP_RET || inst.op == NS_SSA_OP_TRAP) {
        b->fn->blocks[b->block].terminated = true;
    }
    return idx;
}

static i32 ns_ssa_emit_value(ns_ssa_builder *b, ns_ssa_op op, i32 a, i32 c, ns_type t, ns_str name, ns_token_t token, i32 ast) {
    i32 dst = ns_ssa_new_value(b);
    ns_ssa_inst inst = NS_SSA_INST_INIT(op, ast);
    inst.dst = dst;
    inst.a = a;
    inst.c = c;
    inst.type = t;
    inst.name = name;
    inst.token = token;
    ns_ssa_emit_raw(b, inst);
    return dst;
}

static void ns_ssa_emit_branch(ns_ssa_builder *b, i32 cond, i32 if_true, i32 if_false, i32 ast) {
    ns_ssa_inst inst = NS_SSA_INST_INIT(NS_SSA_OP_BR, ast);
    inst.a = cond;
    inst.target0 = if_true;
    inst.target1 = if_false;
    inst.type = ns_type_void;
    ns_ssa_emit_raw(b, inst);
    ns_ssa_connect(b, b->block, if_true);
    ns_ssa_connect(b, b->block, if_false);
}

static void ns_ssa_emit_jump(ns_ssa_builder *b, i32 target, i32 ast) {
    ns_ssa_inst inst = NS_SSA_INST_INIT(NS_SSA_OP_JMP, ast);
    inst.target0 = target;
    inst.type = ns_type_void;
    ns_ssa_emit_raw(b, inst);
    ns_ssa_connect(b, b->block, target);
}

static const char *ns_ssa_cstr(ns_ssa_builder *b, ns_str s) {
    if (s.len == 0) return "";
    if (s.data && s.data[s.len] == 0) return s.data;
    ns_str owned = ns_str_range(ns_malloc((szt)s.len + 1), s.len);
    if (s.len > 0) memcpy(owned.data, s.data, (szt)s.len);
    owned.data[s.len] = 0;
    ns_array_push(b->m->owned_strings, owned);
    return owned.data;
}

static ns_str ns_ssa_fn_symbol_name(ns_ssa_builder *b, i32 ast, ns_str fallback) {
    for (i32 i = 0, l = (i32)ns_array_length(b->vm->symbols); i < l; ++i) {
        ns_symbol *s = &b->vm->symbols[i];
        if (s->type != NS_SYMBOL_FN || s->fn.ast != ast) continue;
        if (s->fn.ctx && b->ctx && s->fn.ctx != b->ctx) continue;
        return s->name;
    }
    return fallback;
}

static ns_str ns_ssa_to_str_symbol_name(ns_ssa_builder *b, ns_type t) {
    ns_symbol *s = ns_vm_find_fn_overload(b->vm, ns_str_cstr("to_str"), 1, t, true);
    if (s && s->type == NS_SYMBOL_FN && ns_type_is(s->fn.ret, NS_TYPE_STRING) &&
        !s->fn.fn.t.ref && s->fn.fn_type != NS_FN_ASYNC) {
        return s->name;
    }
    return ns_str_null;
}

static ns_str ns_ssa_ops_fn_name(ns_ssa_builder *b, ns_ast_t *n) {
    ns_ast_t *l = &b->ctx->nodes[n->ops_fn_def.left];
    ns_ast_t *r = &b->ctx->nodes[n->ops_fn_def.right];
    ns_type lt = ns_ssa_type_from_label(b, l->arg.type);
    ns_type rt = ns_ssa_type_from_label(b, r->arg.type);
    ns_str ln = ns_vm_get_type_name(b->vm, lt);
    ns_str rn = ns_vm_get_type_name(b->vm, rt);
    ns_str name = ns_ops_override_name(ln, rn, n->ops_fn_def.ops);
    if (name.data) ns_array_push(b->m->owned_strings, name);
    return name.len > 0 ? name : n->ops_fn_def.ops.val;
}

static ns_str ns_ssa_fresh_name(ns_ssa_builder *b, const char *prefix) {
    char buf[40];
    i32 n = snprintf(buf, sizeof(buf), "%s%d", prefix, b->next_value);
    if (n < 0) n = 0;
    ns_str owned = ns_str_range(ns_malloc((szt)n + 1), n);
    memcpy(owned.data, buf, (szt)n + 1);
    ns_array_push(b->m->owned_strings, owned);
    return owned;
}

static i32 ns_ssa_emit_add_i32(ns_ssa_builder *b, i32 v, i32 n, ns_str name, i32 ast) {
    i32 c = ns_ssa_emit_i32_const(b, n, ast);
    i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_ADD, v, -1, ns_type_i32, name, (ns_token_t){0}, ast);
    b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].b = c;
    return dst;
}

static i32 ns_ssa_emit_lt(ns_ssa_builder *b, i32 lhs, i32 rhs, i32 ast) {
    i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_LT, lhs, -1, ns_type_bool, ns_str_null, (ns_token_t){0}, ast);
    b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].b = rhs;
    return dst;
}

static i32 ns_ssa_emit_index(ns_ssa_builder *b, i32 table, i32 idx, i32 stride, ns_type elem, i32 ast) {
    i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_INDEX, table, stride, elem, ns_str_null, (ns_token_t){0}, ast);
    b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].b = idx;
    return dst;
}

static void ns_ssa_foreach_writeback(ns_ssa_builder *b, ns_ssa_loop_ctx *loop, i32 ast) {
    if (loop->foreach_subject < 0 || loop->foreach_elem.len == 0) return;
    i32 elem = ns_ssa_env_get_value(b->env, loop->foreach_elem, -1);
    if (elem < 0) return;
    ns_type et = ns_ssa_value_type(b, elem);
    i32 subject = loop->foreach_subject;
    if (loop->foreach_stride > 0 && loop->for_iter.len > 0) {
        i32 idx = ns_ssa_env_get_value(b->env, loop->for_iter, -1);
        if (idx < 0) return;
        ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_ARRAY_STORE, ast);
        store.a = subject;
        store.b = elem;
        store.target0 = idx;
        store.c = loop->foreach_stride;
        store.type = et;
        ns_ssa_emit_raw(b, store);
    }
    if (loop->foreach_field_off >= 0) {
        ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_STORE, ast);
        store.a = subject;
        store.b = elem;
        store.c = loop->foreach_field_off;
        store.target0 = ns_ssa_wasm32_size(b, et);
        store.type = et;
        ns_ssa_emit_raw(b, store);
    }
}

static void ns_ssa_loop_flush_phis(ns_ssa_builder *b, i32 header) {
    if (header < 0 || header >= (i32)ns_array_length(b->fn->blocks)) return;
    ns_ssa_block *hb = &b->fn->blocks[header];
    for (i32 hi = 0, hl = (i32)ns_array_length(hb->insts); hi < hl; ++hi) {
        ns_ssa_inst *phi = &b->fn->insts[hb->insts[hi]];
        if (phi->op != NS_SSA_OP_PHI) break;
        i32 src = ns_ssa_env_get_value(b->env, phi->name, -1);
        if (src < 0 || src == phi->dst) continue;
        ns_ssa_inst copy = NS_SSA_INST_INIT(NS_SSA_OP_COPY, phi->ast);
        copy.dst = phi->dst;
        copy.a = src;
        copy.type = phi->type;
        copy.name = phi->name;
        ns_ssa_emit_raw(b, copy);
    }
}

static void ns_ssa_foreach_assign_writeback(ns_ssa_builder *b, ns_str name, i32 ast) {
    for (i32 i = (i32)ns_array_length(b->loops) - 1; i >= 0; --i) {
        ns_ssa_loop_ctx *loop = &b->loops[i];
        if (loop->foreach_elem.len > 0 && ns_str_equals(loop->foreach_elem, name)) {
            ns_ssa_foreach_writeback(b, loop, ast);
            return;
        }
    }
}

static i32 ns_ssa_lower_struct_ctor(ns_ssa_builder *b, ns_ast_t *n, ns_symbol *st, i32 ast) {
    i32 object = ns_ssa_emit_value(b, NS_SSA_OP_ALLOC, -1, ns_ssa_wasm32_size(b, st->st.st.t),
                                   st->st.st.t, st->name, (ns_token_t){0}, ast);
    i32 next = n->next;
    i32 nfields = (i32)ns_array_length(st->st.fields);
    for (i32 fi = 0; fi < n->call_expr.arg_count && fi < nfields && next > 0; ++fi) {
        i32 val = ns_ssa_lower_expr(b, next);
        val = ns_ssa_clone_struct(b, val, next);
        ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_STORE, next);
        store.a = object;
        store.b = val;
        store.c = ns_ssa_wasm32_field_offset(b, st, fi);
        store.target0 = ns_ssa_wasm32_size(b, st->st.fields[fi].t);
        store.type = st->st.fields[fi].t;
        ns_ssa_emit_raw(b, store);
        next = b->ctx->nodes[next].next;
    }
    return object;
}

static i32 ns_ssa_emit_i32_const(ns_ssa_builder *b, i32 n, i32 ast) {
    char text[24];
    i32 len = snprintf(text, sizeof(text), "%d", n);
    ns_str owned = ns_str_range(ns_malloc((szt)len + 1), len);
    memcpy(owned.data, text, (szt)len + 1);
    ns_array_push(b->m->owned_strings, owned);
    return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_i32, owned,
                             (ns_token_t){.type = NS_TOKEN_INT_LITERAL, .val = owned}, ast);
}

static i32 ns_ssa_emit_named_call(ns_ssa_builder *b, const char *name, i32 *args, i32 nargs,
                                  ns_type ret, i32 ast) {
    for (i32 i = 0; i < nargs; ++i) {
        ns_ssa_inst arg = NS_SSA_INST_INIT(NS_SSA_OP_ARG, ast);
        arg.a = args[i];
        ns_ssa_emit_raw(b, arg);
    }
    return ns_ssa_emit_value(b, NS_SSA_OP_CALL, -1, nargs, ret, ns_str_cstr((i8 *)name),
                             (ns_token_t){0}, ast);
}

static i32 ns_ssa_map_flags(ns_ssa_builder *b, ns_type t) {
    i32 flags = ns_type_is(t, NS_TYPE_SET) ? 1 : 0;
    i32 idx = ns_type_index(t);
    if (idx >= 0 && idx < (i32)ns_array_length(b->vm->symbols)) {
        ns_type key = b->vm->symbols[idx].ct.key;
        ns_type val = b->vm->symbols[idx].ct.val;
        if (ns_type_is(key, NS_TYPE_STRING)) flags |= 2;
        if (ns_type_is(val, NS_TYPE_STRING)) flags |= 4;
    }
    return flags;
}

static void ns_ssa_emit_ret(ns_ssa_builder *b, i32 value, i32 ast) {
    ns_ssa_flush_captures(b, ast);
    ns_ssa_inst inst = NS_SSA_INST_INIT(NS_SSA_OP_RET, ast);
    inst.a = value;
    inst.type = ns_type_void;
    ns_ssa_emit_raw(b, inst);
}

static i32 ns_ssa_env_find(ns_ssa_binding *env, ns_str name) {
    for (i32 i = (i32)ns_array_length(env) - 1; i >= 0; --i) {
        if (ns_str_equals(env[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static ns_ssa_binding *ns_ssa_env_clone(ns_ssa_binding *env) {
    ns_ssa_binding *ret = NULL;
    for (i32 i = 0, l = (i32)ns_array_length(env); i < l; ++i) {
        ns_array_push(ret, env[i]);
    }
    return ret;
}

static i32 ns_ssa_env_get_value(ns_ssa_binding *env, ns_str name, i32 fallback) {
    i32 idx = ns_ssa_env_find(env, name);
    if (idx < 0) return fallback;
    return env[idx].value;
}

static void ns_ssa_env_bind(ns_ssa_builder *b, ns_str name, i32 value, ns_type t) {
    i32 idx = ns_ssa_env_find(b->env, name);
    ns_ssa_binding binding = {.name = name, .value = value, .type = t};
    if (idx >= 0) {
        b->env[idx] = binding;
    } else {
        ns_array_push(b->env, binding);
    }
}

static ns_bool ns_ssa_name_listed(ns_str *names, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(names); i < l; ++i) {
        if (ns_str_equals(names[i], name)) return true;
    }
    return false;
}

static void ns_ssa_add_ref_name(ns_ssa_builder *b, ns_str name) {
    if (name.len <= 0 || ns_ssa_name_listed(b->ref_names, name)) return;
    ns_array_push(b->ref_names, name);
}

static ns_bool ns_ssa_needs_box(ns_ssa_builder *b, ns_str name) {
    return ns_ssa_name_listed(b->ref_names, name);
}

static ns_bool ns_ssa_heap_payload(ns_type t) {
    t = ns_type_set_ref(t, false);
    return ns_type_is(t, NS_TYPE_STRUCT) || ns_type_is_array(t) ||
           ns_type_is(t, NS_TYPE_STRING) || ns_type_is(t, NS_TYPE_FN) ||
           ns_type_is(t, NS_TYPE_BLOCK) || ns_type_is(t, NS_TYPE_TASK) ||
           ns_type_is(t, NS_TYPE_DICT) || ns_type_is(t, NS_TYPE_SET) ||
           ns_type_is(t, NS_TYPE_UNION);
}

static i32 ns_ssa_type_tag(ns_type t) {
    t = ns_type_set_ref(t, false);
    if (ns_type_is(t, NS_TYPE_STRUCT) || ns_type_is(t, NS_TYPE_UNION) ||
        ns_type_is(t, NS_TYPE_FN) || ns_type_is(t, NS_TYPE_BLOCK) ||
        ns_type_is(t, NS_TYPE_TASK) || ns_type_is(t, NS_TYPE_DICT) ||
        ns_type_is(t, NS_TYPE_SET)) {
        return 1000 + (i32)ns_type_index(t);
    }
    return (i32)t.type;
}

static void ns_ssa_collect_ref_in(ns_ssa_builder *b, i32 i);

static void ns_ssa_collect_ref_chain(ns_ssa_builder *b, i32 i, i32 count) {
    for (i32 n = 0; n < count && i > 0; ++n) {
        ns_ssa_collect_ref_in(b, i);
        i = b->ctx->nodes[i].next;
    }
}

static void ns_ssa_collect_ref_in(ns_ssa_builder *b, i32 i) {
    if (i <= 0) return;
    ns_ast_t *n = &b->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR:
        ns_ssa_collect_ref_in(b, n->expr.body);
        break;
    case NS_AST_UNARY_EXPR:
        if (n->unary_expr.op.type == NS_TOKEN_REF) {
            ns_ast_t *inner = ns_ssa_unwrap_expr(b, n->unary_expr.expr);
            if (inner && inner->type == NS_AST_PRIMARY_EXPR &&
                inner->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
                ns_ssa_add_ref_name(b, inner->primary_expr.token.val);
            }
        }
        ns_ssa_collect_ref_in(b, n->unary_expr.expr);
        break;
    case NS_AST_BINARY_EXPR:
        ns_ssa_collect_ref_in(b, n->binary_expr.left);
        ns_ssa_collect_ref_in(b, n->binary_expr.right);
        break;
    case NS_AST_CALL_EXPR:
        ns_ssa_collect_ref_in(b, n->call_expr.callee);
        ns_ssa_collect_ref_chain(b, n->next, n->call_expr.arg_count);
        break;
    case NS_AST_CAST_EXPR:
        ns_ssa_collect_ref_in(b, n->cast_expr.expr);
        break;
    case NS_AST_MEMBER_EXPR:
        ns_ssa_collect_ref_in(b, n->member_expr.left);
        ns_ssa_collect_ref_in(b, n->member_expr.right);
        break;
    case NS_AST_INDEX_EXPR:
        ns_ssa_collect_ref_in(b, n->index_expr.table);
        ns_ssa_collect_ref_in(b, n->index_expr.expr);
        break;
    case NS_AST_ARRAY_EXPR:
        ns_ssa_collect_ref_in(b, n->array_expr.count_expr);
        ns_ssa_collect_ref_chain(b, n->next, n->array_expr.elem_count);
        break;
    case NS_AST_DESIG_EXPR:
        ns_ssa_collect_ref_chain(b, n->next, n->desig_expr.count);
        break;
    case NS_AST_BLOCK_EXPR:
        ns_ssa_collect_ref_in(b, n->block_expr.body);
        break;
    case NS_AST_STR_FMT_EXPR:
        ns_ssa_collect_ref_chain(b, n->next, n->str_fmt.expr_count);
        break;
    case NS_AST_VAR_DEF:
        ns_ssa_collect_ref_in(b, n->var_def.expr);
        break;
    case NS_AST_FN_DEF:
        ns_ssa_collect_ref_in(b, n->fn_def.body);
        break;
    case NS_AST_OP_FN_DEF:
        ns_ssa_collect_ref_in(b, n->ops_fn_def.body);
        break;
    case NS_AST_ASSERT_STMT:
        ns_ssa_collect_ref_in(b, n->assert_stmt.expr);
        break;
    case NS_AST_JUMP_STMT:
        ns_ssa_collect_ref_in(b, n->jump_stmt.expr);
        break;
    case NS_AST_IF_STMT:
        ns_ssa_collect_ref_in(b, n->if_stmt.condition);
        ns_ssa_collect_ref_in(b, n->if_stmt.body);
        ns_ssa_collect_ref_in(b, n->if_stmt.else_body);
        break;
    case NS_AST_LOOP_STMT:
        ns_ssa_collect_ref_in(b, n->loop_stmt.condition);
        ns_ssa_collect_ref_in(b, n->loop_stmt.body);
        break;
    case NS_AST_FOR_STMT:
        ns_ssa_collect_ref_in(b, n->for_stmt.generator);
        ns_ssa_collect_ref_in(b, n->for_stmt.body);
        break;
    case NS_AST_GEN_EXPR:
        ns_ssa_collect_ref_in(b, n->gen_expr.from);
        ns_ssa_collect_ref_in(b, n->gen_expr.to);
        break;
    case NS_AST_COMPOUND_STMT:
        ns_ssa_collect_ref_chain(b, n->next, n->compound_stmt.count);
        break;
    case NS_AST_LABELED_STMT:
        ns_ssa_collect_ref_in(b, n->label_stmt.condition);
        ns_ssa_collect_ref_in(b, n->label_stmt.stmt);
        break;
    default:
        break;
    }
}

static i32 ns_ssa_make_ref(ns_ssa_builder *b, i32 value, ns_type type, i32 ast) {
    if (value < 0 || ns_type_is_ref(type) || ns_ssa_heap_payload(type)) return value;
    i32 size = ns_ssa_wasm32_size(b, type);
    if (size < 1) size = 8;
    ns_type box_t = ns_type_set_ref(type, true);
    i32 box = ns_ssa_emit_value(b, NS_SSA_OP_ALLOC, -1, size, box_t, ns_str_null,
                                (ns_token_t){0}, ast);
    ns_ssa_emit_store_off(b, box, value, 0, type, ast);
    return box;
}

static i32 ns_ssa_load_ref(ns_ssa_builder *b, i32 box, ns_type type, i32 ast) {
    if (box < 0) return box;
    ns_type inner = ns_type_set_ref(type, false);
    if (ns_ssa_heap_payload(inner)) return box;
    i32 v = ns_ssa_emit_value(b, NS_SSA_OP_LOAD, box, 0, inner, ns_str_null,
                              (ns_token_t){0}, ast);
    b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].target0 =
        ns_ssa_wasm32_size(b, inner);
    return v;
}

static i32 ns_ssa_union_wrap(ns_ssa_builder *b, i32 value, ns_type union_t, i32 ast) {
    ns_type st = ns_ssa_value_type(b, value);
    if (ns_type_is(st, NS_TYPE_UNION) || !ns_type_is(union_t, NS_TYPE_UNION)) return value;
    i32 tag = ns_ssa_emit_i32_const(b, ns_ssa_type_tag(st), ast);
    i32 args[2] = {tag, value};
    return ns_ssa_emit_named_call(b, "ns_rt_union_new", args, 2, union_t, ast);
}

static i32 ns_ssa_union_unwrap(ns_ssa_builder *b, i32 value, ns_type dst, i32 ast) {
    ns_type st = ns_ssa_value_type(b, value);
    if (!ns_type_is(st, NS_TYPE_UNION)) return value;
    i32 want = ns_ssa_emit_i32_const(b, ns_ssa_type_tag(dst), ast);
    i32 args[2] = {value, want};
    return ns_ssa_emit_named_call(b, "ns_rt_union_as", args, 2, dst, ast);
}

static i32 ns_ssa_maybe_box_name(ns_ssa_builder *b, ns_str name, i32 value, ns_type type, i32 ast) {
    if (!ns_ssa_needs_box(b, name) || ns_type_is_ref(type)) {
        ns_ssa_env_bind(b, name, value, type);
        return value;
    }
    i32 boxed = ns_ssa_make_ref(b, value, type, ast);
    ns_ssa_env_bind(b, name, boxed, ns_type_set_ref(type, true));
    return boxed;
}

static ns_bool ns_ssa_fn_exists(ns_ssa_builder *b, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(b->m->fns); i < l; ++i) {
        if (ns_str_equals(b->m->fns[i].name, name)) return true;
    }
    return false;
}

static ns_str ns_ssa_owned_fmt(ns_ssa_builder *b, const char *fmt, i32 n) {
    char buf[80];
    i32 len = snprintf(buf, sizeof(buf), fmt, n);
    if (len < 0) len = 0;
    ns_str owned = ns_str_range(ns_malloc((szt)len + 1), len);
    memcpy(owned.data, buf, (szt)len + 1);
    ns_array_push(b->m->owned_strings, owned);
    return owned;
}

static ns_str ns_ssa_owned_prefix(ns_ssa_builder *b, const char *prefix, ns_str rest) {
    char buf[160];
    i32 len = snprintf(buf, sizeof(buf), "%s%.*s", prefix, rest.len, rest.data);
    if (len < 0) len = 0;
    ns_str owned = ns_str_range(ns_malloc((szt)len + 1), len);
    memcpy(owned.data, buf, (szt)len + 1);
    ns_array_push(b->m->owned_strings, owned);
    return owned;
}

static void ns_ssa_emit_store_off(ns_ssa_builder *b, i32 obj, i32 val, i32 off, ns_type t, i32 ast) {
    ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_STORE, ast);
    store.a = obj;
    store.b = val;
    store.c = off;
    store.target0 = ns_type_is(t, NS_TYPE_I64) || ns_type_is(t, NS_TYPE_U64) ||
                    ns_type_is(t, NS_TYPE_F64) ? 8 : ns_ssa_wasm32_size(b, t);
    store.type = t;
    ns_ssa_emit_raw(b, store);
}

static void ns_ssa_flush_captures(ns_ssa_builder *b, i32 ast) {
    if (b->capture_env < 0) return;
    for (i32 i = 0, l = (i32)ns_array_length(b->captures); i < l; ++i) {
        ns_ssa_capture *c = &b->captures[i];
        i32 val = ns_ssa_env_get_value(b->env, c->name, -1);
        if (val < 0) continue;
        ns_ssa_emit_store_off(b, b->capture_env, val, c->offset, c->type, ast);
    }
}

static i32 ns_ssa_block_payload(ns_ssa_builder *b, ns_symbol *sym) {
    if (!sym || sym->type != NS_SYMBOL_BLOCK) return 0;
    i32 off = 0;
    for (i32 i = 0, l = (i32)ns_array_length(sym->bc.st.fields); i < l; ++i) {
        i32 sz = ns_ssa_wasm32_size(b, sym->bc.st.fields[i].t);
        off = ns_ssa_wasm32_align(off, sz) + sz;
    }
    return off;
}

static i32 ns_ssa_block_field_off(ns_ssa_builder *b, ns_symbol *sym, i32 field) {
    i32 off = 8;
    for (i32 i = 0; i < field; ++i) {
        i32 sz = ns_ssa_wasm32_size(b, sym->bc.st.fields[i].t);
        off = ns_ssa_wasm32_align(off, sz) + sz;
    }
    i32 sz = ns_ssa_wasm32_size(b, sym->bc.st.fields[field].t);
    return ns_ssa_wasm32_align(off, sz);
}

static ns_type ns_ssa_callable_ret(ns_ssa_builder *b, ns_type t) {
    if (!ns_type_is(t, NS_TYPE_FN) && !ns_type_is(t, NS_TYPE_BLOCK)) return ns_type_unknown;
    i32 idx = ns_type_index(t);
    if (idx < 0 || idx >= (i32)ns_array_length(b->vm->symbols)) return ns_type_unknown;
    ns_symbol *s = &b->vm->symbols[idx];
    if (s->type != NS_SYMBOL_FN && s->type != NS_SYMBOL_BLOCK) return ns_type_unknown;
    ns_fn_symbol *fn = ns_symbol_get_fn(s);
    return fn ? ns_enum_underlying_type(b->vm, fn->ret) : ns_type_unknown;
}

static i32 ns_ssa_emit_callable(ns_ssa_builder *b, ns_str code, ns_symbol *cap_sym, ns_type t, i32 ast) {
    i32 extra = ns_ssa_block_payload(b, cap_sym);
    i32 obj = ns_ssa_emit_value(b, NS_SSA_OP_ALLOC, -1, 8 + extra, t, ns_str_null, (ns_token_t){0}, ast);
    i32 addr = ns_ssa_emit_value(b, NS_SSA_OP_FNADDR, -1, -1, ns_type_i64, code, (ns_token_t){0}, ast);
    ns_ssa_emit_store_off(b, obj, addr, 0, ns_type_i64, ast);
    if (cap_sym && cap_sym->type == NS_SYMBOL_BLOCK) {
        for (i32 i = 0, l = (i32)ns_array_length(cap_sym->bc.st.fields); i < l; ++i) {
            ns_struct_field *f = &cap_sym->bc.st.fields[i];
            i32 val = ns_ssa_env_get_value(b->env, f->name, -1);
            if (val < 0) {
                i32 g = ns_ssa_global_index(b, f->name);
                if (g >= 0) {
                    val = ns_ssa_emit_value(b, NS_SSA_OP_GLOBAL_GET, -1, g, b->m->globals[g].type,
                                            f->name, (ns_token_t){0}, ast);
                }
            }
            if (val < 0) continue;
            ns_ssa_emit_store_off(b, obj, val, ns_ssa_block_field_off(b, cap_sym, i), f->t, ast);
        }
    }
    return obj;
}

static ns_bool ns_ssa_str_seen(ns_str *items, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(items); i < l; ++i) {
        if (ns_str_equals(items[i], name)) return true;
    }
    return false;
}

static ns_bool ns_ssa_global_const_seen(ns_ssa_builder *b, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(b->globals); i < l; ++i) {
        if (ns_str_equals(b->globals[i].name, name)) return true;
    }
    return false;
}

static i32 ns_ssa_global_index(ns_ssa_builder *b, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(b->m->globals); i < l; ++i) {
        if (ns_str_equals(b->m->globals[i].name, name)) return i;
    }
    return -1;
}

static ns_type ns_ssa_value_type(ns_ssa_builder *b, i32 value) {
    if (value < 0) return ns_type_unknown;
    for (i32 i = (i32)ns_array_length(b->fn->insts) - 1; i >= 0; --i) {
        if (b->fn->insts[i].dst == value) return b->fn->insts[i].type;
    }
    return ns_type_unknown;
}

static i32 ns_ssa_wasm32_size(ns_ssa_builder *b, ns_type type);

static i32 ns_ssa_wasm32_align(i32 offset, i32 size) {
    i32 alignment = ns_min(4, size);
    return alignment > 1 ? (offset + alignment - 1) & ~(alignment - 1) : offset;
}

static i32 ns_ssa_wasm32_field_offset(ns_ssa_builder *b, ns_symbol *st, i32 field) {
    i32 offset = 0;
    for (i32 i = 0; i <= field; ++i) {
        i32 size = ns_ssa_wasm32_size(b, st->st.fields[i].t);
        offset = ns_ssa_wasm32_align(offset, size);
        if (i == field) return offset;
        offset += size;
    }
    return -1;
}

static ns_bool ns_ssa_native_struct(ns_type t) {
    return ns_type_is_ref(t) && ns_type_is(t, NS_TYPE_STRUCT);
}

static i32 ns_ssa_field_off(ns_ssa_builder *b, ns_symbol *st, i32 field, ns_bool native) {
    if (native && field >= 0 && field < (i32)ns_array_length(st->st.fields)) {
        return (i32)st->st.fields[field].o;
    }
    return ns_ssa_wasm32_field_offset(b, st, field);
}

static i32 ns_ssa_field_sz(ns_ssa_builder *b, ns_symbol *st, i32 field, ns_bool native) {
    if (native && field >= 0 && field < (i32)ns_array_length(st->st.fields)) {
        return st->st.fields[field].s;
    }
    return ns_ssa_wasm32_size(b, st->st.fields[field].t);
}

static i32 ns_ssa_wasm32_size(ns_ssa_builder *b, ns_type type) {
    type = ns_enum_underlying_type(b->vm, type);
    if (ns_type_is_ref(type) || ns_type_is(type, NS_TYPE_ANY)) return 8;
    if (ns_type_is_array(type) ||
        ns_type_is(type, NS_TYPE_STRING) || ns_type_is(type, NS_TYPE_FN) ||
        ns_type_is(type, NS_TYPE_BLOCK) || ns_type_is(type, NS_TYPE_UNION) ||
        ns_type_is(type, NS_TYPE_DICT) || ns_type_is(type, NS_TYPE_SET) ||
        ns_type_is(type, NS_TYPE_TASK)) return 4;
    if (ns_type_is(type, NS_TYPE_I8) || ns_type_is(type, NS_TYPE_U8)) return 1;
    if (ns_type_is(type, NS_TYPE_I16) || ns_type_is(type, NS_TYPE_U16)) return 2;
    if (ns_type_is(type, NS_TYPE_I64) || ns_type_is(type, NS_TYPE_U64) ||
        ns_type_is(type, NS_TYPE_F64)) return 8;
    if (ns_type_is(type, NS_TYPE_STRUCT)) {
        ns_symbol *st = &b->vm->symbols[ns_type_index(type)];
        i32 size = 0;
        for (i32 i = 0, l = (i32)ns_array_length(st->st.fields); i < l; ++i) {
            i32 field_size = ns_ssa_wasm32_size(b, st->st.fields[i].t);
            size = ns_ssa_wasm32_align(size, field_size) + field_size;
        }
        return size;
    }
    return 4;
}

// Plain structs are values in Nano Script. The Wasm ABI represents them by a
// linear-memory address, so copy their bytes whenever they cross an ordinary
// value boundary. Reference-backed fields remain shared, as they do natively.
static i32 ns_ssa_clone_struct(ns_ssa_builder *b, i32 value, i32 ast) {
    ns_type type = ns_ssa_value_type(b, value);
    if (!ns_type_is(type, NS_TYPE_STRUCT) || ns_type_is_ref(type) || ns_type_is_array(type)) return value;
    return ns_ssa_emit_value(b, NS_SSA_OP_CLONE, value, ns_ssa_wasm32_size(b, type),
                             type, ns_str_null, (ns_token_t){0}, ast);
}

static ns_bool ns_ssa_const_from_value(ns_ssa_builder *b, ns_value value, ns_token_t *token, ns_type *type) {
    if (ns_type_is(value.t, NS_TYPE_ENUM)) value = ns_eval_enum_underlying_value(b->vm, value);
    char text[96];
    i32 len = 0;
    token->type = NS_TOKEN_INT_LITERAL;
    switch (value.t.type) {
    case NS_TYPE_TYPE: {
        i32 id = ns_type_in_stack(value.t) ? *(i32 *)&b->vm->stack[value.o] : value.i32;
        len = snprintf(text, sizeof(text), "%d", id);
    } break;
    case NS_TYPE_I8: len = snprintf(text, sizeof(text), "%d", (i32)ns_eval_number_i8(b->vm, value)); break;
    case NS_TYPE_I16: len = snprintf(text, sizeof(text), "%d", (i32)ns_eval_number_i16(b->vm, value)); break;
    case NS_TYPE_I32: len = snprintf(text, sizeof(text), "%d", ns_eval_number_i32(b->vm, value)); break;
    case NS_TYPE_I64: len = snprintf(text, sizeof(text), "%lld", (long long)ns_eval_number_i64(b->vm, value)); break;
    case NS_TYPE_U8: len = snprintf(text, sizeof(text), "%u", (u32)ns_eval_number_u8(b->vm, value)); break;
    case NS_TYPE_U16: len = snprintf(text, sizeof(text), "%u", (u32)ns_eval_number_u16(b->vm, value)); break;
    case NS_TYPE_U32: len = snprintf(text, sizeof(text), "%u", ns_eval_number_u32(b->vm, value)); break;
    case NS_TYPE_U64: len = snprintf(text, sizeof(text), "%llu", (unsigned long long)ns_eval_number_u64(b->vm, value)); break;
    case NS_TYPE_F32:
        token->type = NS_TOKEN_FLT_LITERAL;
        len = snprintf(text, sizeof(text), "%.9g", (double)ns_eval_number_f32(b->vm, value));
        break;
    case NS_TYPE_F64:
        token->type = NS_TOKEN_FLT_LITERAL;
        token->suffix = NS_NUM_SUFFIX_F64;
        len = snprintf(text, sizeof(text), "%.17g", ns_eval_number_f64(b->vm, value));
        break;
    case NS_TYPE_BOOL: len = snprintf(text, sizeof(text), "%d", ns_eval_bool(b->vm, value) ? 1 : 0); break;
    default:
        return false; // strings are interpreter literals; native string constants are not implemented yet
    }
    i8 *copy = ns_malloc((szt)len + 1);
    memcpy(copy, text, (szt)len + 1);
    token->val = ns_str_range(copy, len);
    ns_array_push(b->m->owned_strings, token->val);
    *type = value.t;
    return true;
}

static void ns_ssa_seed_global_consts(ns_ssa_builder *b) {
    for (i32 i = 0, l = (i32)ns_array_length(b->globals); i < l; ++i) {
        ns_ssa_global_const *g = &b->globals[i];
        i32 value = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, g->type, g->token.val, g->token, -1);
        ns_ssa_env_bind(b, g->name, value, g->type);
    }
}

static void ns_ssa_collect_module_consts(ns_ssa_builder *b, ns_ast_ctx *ctx);

static void ns_ssa_import_module_consts(ns_ssa_builder *b, ns_str lib_name) {
    if (ns_ssa_str_seen(b->import_seen, lib_name)) return;
    ns_array_push(b->import_seen, lib_name);

    ns_str ref_path = b->vm->ref_path;
    if (ref_path.data == ns_null) {
#ifdef NS_DEBUG
        ref_path = ns_str_cstr(NS_SSA_REF_PATH);
#else
        ns_str home = ns_path_home();
        ref_path = ns_path_join(home, ns_str_cstr(NS_SSA_REF_PATH));
#endif
    }

    ns_str path = ns_path_join(ref_path, ns_str_concat(lib_name, ns_str_cstr(".ns")));
    ns_str source = ns_os_read_file(path);
    if (source.data == ns_null || source.len == 0) return;

    ns_ast_ctx lib_ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&lib_ctx, source, path);
    if (ns_return_is_error(parsed)) return;
    ns_str previous_lib = b->vm->lib;
    b->vm->lib = lib_name;
    ns_ssa_collect_module_consts(b, &lib_ctx);
    b->vm->lib = previous_lib;
}

static void ns_ssa_collect_module_consts(ns_ssa_builder *b, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        ns_ast_t *n = &ctx->nodes[ctx->sections[i]];
        if (n->type == NS_AST_USE_STMT) ns_ssa_import_module_consts(b, n->use_stmt.lib.val);
    }

    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        ns_ast_t *n = &ctx->nodes[ctx->sections[i]];
        if (n->type != NS_AST_VAR_DEF) continue;
        if (ns_ssa_global_const_seen(b, n->var_def.name.val)) continue;

        ns_token_t token = {0};
        ns_type type = ns_type_unknown;
        if (n->var_def.is_lit) {
            ns_symbol *symbol = ns_vm_find_symbol(b->vm, n->var_def.name.val, false);
            if (!symbol || symbol->type != NS_SYMBOL_VALUE || !symbol->is_lit) continue;
            if (!ns_ssa_const_from_value(b, symbol->val, &token, &type)) continue;
        } else {
            continue;
        }

        ns_ssa_global_const global = {.name = n->var_def.name.val, .token = token, .type = type};
        ns_array_push(b->globals, global);
    }
}

static void ns_ssa_collect_module_globals(ns_ssa_builder *b) {
    for (i32 i = b->ctx->section_begin; i < b->ctx->section_end; ++i) {
        ns_ast_t *n = &b->ctx->nodes[b->ctx->sections[i]];
        if (n->type != NS_AST_VAR_DEF || n->var_def.is_lit ||
            ns_ssa_global_const_seen(b, n->var_def.name.val)) continue;
        ns_symbol *symbol = ns_vm_find_symbol(b->vm, n->var_def.name.val, false);
        ns_type type = symbol && symbol->type == NS_SYMBOL_VALUE ? symbol->val.t : ns_type_unknown;
        if (ns_type_is(type, NS_TYPE_UNKNOWN) && n->var_def.expr > 0) {
            ns_return_type parsed = ns_vm_parse_expr(b->vm, b->ctx, n->var_def.expr, ns_type_infer);
            if (!ns_return_is_error(parsed)) type = parsed.r;
        }
        if (ns_type_is(type, NS_TYPE_ENUM)) type = ns_enum_underlying_type(b->vm, type);
        ns_array_push(b->m->globals, ((ns_ssa_global){.name = n->var_def.name.val, .type = type}));
    }
}

static ns_ssa_op ns_ssa_binary_op(ns_token_t op) {
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        return ns_str_equals_STR(op.val, "+") ? NS_SSA_OP_ADD : NS_SSA_OP_SUB;
    case NS_TOKEN_MUL_OP:
        if (ns_str_equals_STR(op.val, "*")) return NS_SSA_OP_MUL;
        if (ns_str_equals_STR(op.val, "/")) return NS_SSA_OP_DIV;
        return NS_SSA_OP_MOD;
    case NS_TOKEN_LOGIC_OP:
        return ns_str_equals_STR(op.val, "&&") ? NS_SSA_OP_AND : NS_SSA_OP_OR;
    case NS_TOKEN_SHIFT_OP:
        return ns_str_equals_STR(op.val, "<<") ? NS_SSA_OP_SHL : NS_SSA_OP_SHR;
    case NS_TOKEN_REL_OP: {
        if (ns_str_equals_STR(op.val, "<")) return NS_SSA_OP_LT;
        if (ns_str_equals_STR(op.val, "<=")) return NS_SSA_OP_LE;
        if (ns_str_equals_STR(op.val, ">")) return NS_SSA_OP_GT;
        return NS_SSA_OP_GE;
    }
    case NS_TOKEN_EQ_OP:
        return ns_str_equals_STR(op.val, "==") ? NS_SSA_OP_EQ : NS_SSA_OP_NE;
    case NS_TOKEN_BITWISE_OP: {
        if (ns_str_equals_STR(op.val, "&")) return NS_SSA_OP_BAND;
        if (ns_str_equals_STR(op.val, "|")) return NS_SSA_OP_BOR;
        return NS_SSA_OP_BXOR;
    }
    default:
        return NS_SSA_OP_UNKNOWN;
    }
}

static i32 ns_ssa_lower_primary(ns_ssa_builder *b, ns_ast_t *n, i32 i) {
    ns_token_t token = n->primary_expr.token;
    if (token.type == NS_TOKEN_TYPE ||
        (token.type >= NS_TOKEN_TYPE_I8 && token.type <= NS_TOKEN_TYPE_VOID)) {
        ns_type represented = ns_vm_parse_generic_type(token);
        char literal[16];
        i32 len = snprintf(literal, sizeof(literal), "%d", (i32)represented.type);
        ns_str owned = ns_str_range(ns_malloc((szt)len + 1), len);
        memcpy(owned.data, literal, (szt)len);
        owned.data[len] = '\0';
        ns_array_push(b->m->owned_strings, owned);
        ns_token_t id_token = {.type = NS_TOKEN_INT_LITERAL, .val = owned};
        return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_type, owned, id_token, i);
    }
    if (token.type == NS_TOKEN_IDENTIFIER) {
        i32 idx = ns_ssa_env_find(b->env, token.val);
        if (idx >= 0) {
            i32 v = b->env[idx].value;
            if (ns_type_is_ref(b->env[idx].type)) {
                return ns_ssa_load_ref(b, v, b->env[idx].type, i);
            }
            return v;
        }
        i32 global = ns_ssa_global_index(b, token.val);
        if (global >= 0) {
            i32 v = ns_ssa_emit_value(b, NS_SSA_OP_GLOBAL_GET, -1, global,
                                      b->m->globals[global].type, token.val, token, i);
            if (ns_ssa_needs_box(b, token.val) || ns_type_is_ref(b->m->globals[global].type)) {
                return ns_ssa_load_ref(b, v, b->m->globals[global].type, i);
            }
            return v;
        }
        for (i32 si = 0, sl = (i32)ns_array_length(b->m->shaders); si < sl; ++si) {
            if (!ns_str_equals(b->m->shaders[si].name, token.val)) continue;
            char literal[16];
            i32 len = snprintf(literal, sizeof(literal), "%u", b->m->shaders[si].id);
            ns_str owned = ns_str_range(ns_malloc((szt)len + 1), len);
            memcpy(owned.data, literal, (szt)len);
            owned.data[len] = '\0';
            ns_array_push(b->m->owned_strings, owned);
            ns_token_t id_token = {.type = NS_TOKEN_INT_LITERAL, .val = owned};
            return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_i32, owned, id_token, i);
        }
        ns_symbol *fn_sym = ns_vm_find_symbol(b->vm, token.val, false);
        if (fn_sym && fn_sym->type == NS_SYMBOL_FN && !fn_sym->fn.fn.t.ref) {
            return ns_ssa_named_fn_value(b, fn_sym, i);
        }
        return ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, -1, ns_type_unknown, token.val, token, i);
    }
    ns_type type = n->primary_expr.t;
    if (token.type == NS_TOKEN_STR_LITERAL || token.type == NS_TOKEN_STR_FORMAT) type = ns_type_str;
    if (token.type == NS_TOKEN_TRUE || token.type == NS_TOKEN_FALSE) type = ns_type_bool;
    return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, type, token.val, token, i);
}

static ns_bool ns_ssa_shader_name(ns_str name) {
    return ns_str_starts_with(name, ns_str_cstr("vs_")) ||
           ns_str_starts_with(name, ns_str_cstr("fs_")) ||
           ns_str_starts_with(name, ns_str_cstr("ps_")) ||
           ns_str_starts_with(name, ns_str_cstr("cs_"));
}

static ns_bool ns_ssa_shader_stage_intrinsic(ns_str name) {
    return ns_str_equals_STR(name, "ddx") ||
           ns_str_equals_STR(name, "ddy") ||
           ns_str_equals_STR(name, "shader_buffer_i32") ||
           ns_str_equals_STR(name, "shader_buffer_store_i32") ||
           ns_str_equals_STR(name, "shader_sample_mask") ||
           ns_str_equals_STR(name, "shader_sample_texture") ||
           ns_str_equals_STR(name, "shader_sample_texture_nearest") ||
           ns_str_equals_STR(name, "shader_sample_shadow") ||
           ns_str_equals_STR(name, "shader_global_id_x") ||
           ns_str_equals_STR(name, "shader_global_id_y") ||
           ns_str_equals_STR(name, "shader_global_id_z") ||
           ns_str_equals_STR(name, "shader_vertex_id") ||
           ns_str_equals_STR(name, "shader_root_f32") ||
           ns_str_equals_STR(name, "shader_read_texture") ||
           ns_str_equals_STR(name, "shader_write_texture") ||
           ns_str_equals_STR(name, "shader_write_texture_secondary") ||
           ns_str_equals_STR(name, "shader_transform_position") ||
           ns_str_equals_STR(name, "shader_transform_normal") ||
           ns_str_equals_STR(name, "shader_shadow_clip_position") ||
           ns_str_equals_STR(name, "shader_scene_selected") ||
           ns_str_equals_STR(name, "shader_scene_textured") ||
           ns_str_equals_STR(name, "shader_scene_receives_shadow");
}

static ns_bool ns_ssa_calls_shader_stage(ns_ssa_builder *b, i32 i);

static ns_bool ns_ssa_calls_shader_stage_chain(ns_ssa_builder *b, i32 i, i32 count) {
    for (i32 n = 0; n < count && i > 0; ++n) {
        if (ns_ssa_calls_shader_stage(b, i)) return true;
        i = b->ctx->nodes[i].next;
    }
    return false;
}

static ns_bool ns_ssa_calls_shader_stage(ns_ssa_builder *b, i32 i) {
    if (i <= 0) return false;
    ns_ast_t *n = &b->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR:
        return ns_ssa_calls_shader_stage(b, n->expr.body);
    case NS_AST_UNARY_EXPR:
        return ns_ssa_calls_shader_stage(b, n->unary_expr.expr);
    case NS_AST_BINARY_EXPR:
        return ns_ssa_calls_shader_stage(b, n->binary_expr.left) ||
               ns_ssa_calls_shader_stage(b, n->binary_expr.right);
    case NS_AST_CALL_EXPR: {
        ns_ast_t *callee = ns_ssa_unwrap_expr(b, n->call_expr.callee);
        if (callee && callee->type == NS_AST_PRIMARY_EXPR &&
            callee->primary_expr.token.type == NS_TOKEN_IDENTIFIER &&
            ns_ssa_shader_stage_intrinsic(callee->primary_expr.token.val)) {
            return true;
        }
        if (ns_ssa_calls_shader_stage(b, n->call_expr.callee)) return true;
        return ns_ssa_calls_shader_stage_chain(b, n->next, n->call_expr.arg_count);
    }
    case NS_AST_CAST_EXPR:
        return ns_ssa_calls_shader_stage(b, n->cast_expr.expr);
    case NS_AST_MEMBER_EXPR:
        return ns_ssa_calls_shader_stage(b, n->member_expr.left) ||
               ns_ssa_calls_shader_stage(b, n->member_expr.right);
    case NS_AST_INDEX_EXPR:
        return ns_ssa_calls_shader_stage(b, n->index_expr.table) ||
               ns_ssa_calls_shader_stage(b, n->index_expr.expr);
    case NS_AST_ARRAY_EXPR:
        return ns_ssa_calls_shader_stage(b, n->array_expr.count_expr) ||
               ns_ssa_calls_shader_stage_chain(b, n->next, n->array_expr.elem_count);
    case NS_AST_DESIG_EXPR:
        return ns_ssa_calls_shader_stage_chain(b, n->next, n->desig_expr.count);
    case NS_AST_BLOCK_EXPR:
        return ns_ssa_calls_shader_stage(b, n->block_expr.body);
    case NS_AST_STR_FMT_EXPR:
        return ns_ssa_calls_shader_stage_chain(b, n->next, n->str_fmt.expr_count);
    case NS_AST_VAR_DEF:
        return ns_ssa_calls_shader_stage(b, n->var_def.expr);
    case NS_AST_FN_DEF:
        return ns_ssa_calls_shader_stage(b, n->fn_def.body);
    case NS_AST_OP_FN_DEF:
        return ns_ssa_calls_shader_stage(b, n->ops_fn_def.body);
    case NS_AST_ASSERT_STMT:
        return ns_ssa_calls_shader_stage(b, n->assert_stmt.expr);
    case NS_AST_JUMP_STMT:
        return ns_ssa_calls_shader_stage(b, n->jump_stmt.expr);
    case NS_AST_IF_STMT:
        return ns_ssa_calls_shader_stage(b, n->if_stmt.condition) ||
               ns_ssa_calls_shader_stage(b, n->if_stmt.body) ||
               ns_ssa_calls_shader_stage(b, n->if_stmt.else_body);
    case NS_AST_LOOP_STMT:
        return ns_ssa_calls_shader_stage(b, n->loop_stmt.condition) ||
               ns_ssa_calls_shader_stage(b, n->loop_stmt.body);
    case NS_AST_FOR_STMT:
        return ns_ssa_calls_shader_stage(b, n->for_stmt.generator) ||
               ns_ssa_calls_shader_stage(b, n->for_stmt.body);
    case NS_AST_GEN_EXPR:
        return ns_ssa_calls_shader_stage(b, n->gen_expr.from) ||
               ns_ssa_calls_shader_stage(b, n->gen_expr.to);
    case NS_AST_COMPOUND_STMT:
        return ns_ssa_calls_shader_stage_chain(b, n->next, n->compound_stmt.count);
    case NS_AST_LABELED_STMT:
        return ns_ssa_calls_shader_stage(b, n->label_stmt.condition) ||
               ns_ssa_calls_shader_stage(b, n->label_stmt.stmt);
    default:
        return false;
    }
}

static void ns_ssa_collect_callees(ns_ssa_builder *b, i32 i, ns_str **names);

static void ns_ssa_collect_callees_chain(ns_ssa_builder *b, i32 i, i32 count, ns_str **names) {
    for (i32 n = 0; n < count && i > 0; ++n) {
        ns_ssa_collect_callees(b, i, names);
        i = b->ctx->nodes[i].next;
    }
}

static void ns_ssa_collect_callees(ns_ssa_builder *b, i32 i, ns_str **names) {
    if (i <= 0) return;
    ns_ast_t *n = &b->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR:
        ns_ssa_collect_callees(b, n->expr.body, names);
        break;
    case NS_AST_UNARY_EXPR:
        ns_ssa_collect_callees(b, n->unary_expr.expr, names);
        break;
    case NS_AST_BINARY_EXPR:
        ns_ssa_collect_callees(b, n->binary_expr.left, names);
        ns_ssa_collect_callees(b, n->binary_expr.right, names);
        break;
    case NS_AST_CALL_EXPR: {
        ns_ast_t *callee = ns_ssa_unwrap_expr(b, n->call_expr.callee);
        if (callee && callee->type == NS_AST_PRIMARY_EXPR &&
            callee->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
            ns_str name = callee->primary_expr.token.val;
            if (!ns_ssa_name_listed(*names, name)) ns_array_push(*names, name);
        }
        ns_ssa_collect_callees(b, n->call_expr.callee, names);
        ns_ssa_collect_callees_chain(b, n->next, n->call_expr.arg_count, names);
        break;
    }
    case NS_AST_CAST_EXPR:
        ns_ssa_collect_callees(b, n->cast_expr.expr, names);
        break;
    case NS_AST_MEMBER_EXPR:
        ns_ssa_collect_callees(b, n->member_expr.left, names);
        ns_ssa_collect_callees(b, n->member_expr.right, names);
        break;
    case NS_AST_INDEX_EXPR:
        ns_ssa_collect_callees(b, n->index_expr.table, names);
        ns_ssa_collect_callees(b, n->index_expr.expr, names);
        break;
    case NS_AST_ARRAY_EXPR:
        ns_ssa_collect_callees(b, n->array_expr.count_expr, names);
        ns_ssa_collect_callees_chain(b, n->next, n->array_expr.elem_count, names);
        break;
    case NS_AST_DESIG_EXPR:
        ns_ssa_collect_callees_chain(b, n->next, n->desig_expr.count, names);
        break;
    case NS_AST_BLOCK_EXPR:
        ns_ssa_collect_callees(b, n->block_expr.body, names);
        break;
    case NS_AST_STR_FMT_EXPR:
        ns_ssa_collect_callees_chain(b, n->next, n->str_fmt.expr_count, names);
        break;
    case NS_AST_VAR_DEF:
        ns_ssa_collect_callees(b, n->var_def.expr, names);
        break;
    case NS_AST_FN_DEF:
        ns_ssa_collect_callees(b, n->fn_def.body, names);
        break;
    case NS_AST_OP_FN_DEF:
        ns_ssa_collect_callees(b, n->ops_fn_def.body, names);
        break;
    case NS_AST_ASSERT_STMT:
        ns_ssa_collect_callees(b, n->assert_stmt.expr, names);
        break;
    case NS_AST_JUMP_STMT:
        ns_ssa_collect_callees(b, n->jump_stmt.expr, names);
        break;
    case NS_AST_IF_STMT:
        ns_ssa_collect_callees(b, n->if_stmt.condition, names);
        ns_ssa_collect_callees(b, n->if_stmt.body, names);
        ns_ssa_collect_callees(b, n->if_stmt.else_body, names);
        break;
    case NS_AST_LOOP_STMT:
        ns_ssa_collect_callees(b, n->loop_stmt.condition, names);
        ns_ssa_collect_callees(b, n->loop_stmt.body, names);
        break;
    case NS_AST_FOR_STMT:
        ns_ssa_collect_callees(b, n->for_stmt.generator, names);
        ns_ssa_collect_callees(b, n->for_stmt.body, names);
        break;
    case NS_AST_GEN_EXPR:
        ns_ssa_collect_callees(b, n->gen_expr.from, names);
        ns_ssa_collect_callees(b, n->gen_expr.to, names);
        break;
    case NS_AST_COMPOUND_STMT:
        ns_ssa_collect_callees_chain(b, n->next, n->compound_stmt.count, names);
        break;
    case NS_AST_LABELED_STMT:
        ns_ssa_collect_callees(b, n->label_stmt.condition, names);
        ns_ssa_collect_callees(b, n->label_stmt.stmt, names);
        break;
    default:
        break;
    }
}

static void ns_ssa_collect_shader_only_fns(ns_ssa_builder *b) {
    typedef struct { ns_str name; i32 body; } ns_ssa_fn_seed;
    ns_ssa_fn_seed *fns = ns_null;
    for (i32 i = b->ctx->section_begin; i < b->ctx->section_end; ++i) {
        ns_ast_t *n = &b->ctx->nodes[b->ctx->sections[i]];
        if (n->type != NS_AST_FN_DEF) continue;
        ns_str name = n->fn_def.name.val;
        ns_ssa_fn_seed seed = {.name = name, .body = n->fn_def.body};
        ns_array_push(fns, seed);
        if (ns_ssa_shader_name(name) || ns_ssa_calls_shader_stage(b, n->fn_def.body)) {
            if (!ns_ssa_name_listed(b->shader_only, name)) ns_array_push(b->shader_only, name);
        }
    }

    ns_bool changed = true;
    while (changed) {
        changed = false;
        for (i32 fi = 0, fl = (i32)ns_array_length(fns); fi < fl; ++fi) {
            if (ns_ssa_name_listed(b->shader_only, fns[fi].name)) continue;
            ns_str *callees = ns_null;
            ns_ssa_collect_callees(b, fns[fi].body, &callees);
            for (i32 ci = 0, cl = (i32)ns_array_length(callees); ci < cl; ++ci) {
                if (!ns_ssa_name_listed(b->shader_only, callees[ci])) continue;
                ns_array_push(b->shader_only, fns[fi].name);
                changed = true;
                break;
            }
            ns_array_free(callees);
        }
    }
    ns_array_free(fns);
}

static ns_return_bool ns_ssa_collect_shaders(ns_ssa_builder *b) {
    for (i32 i = 0, l = (i32)ns_array_length(b->vm->symbols); i < l; ++i) {
        ns_symbol *symbol = &b->vm->symbols[i];
        if (symbol->type != NS_SYMBOL_FN || !ns_ssa_shader_name(symbol->name) ||
            (!ns_str_is_empty(symbol->lib) && !ns_str_equals(symbol->lib, ns_str_cstr("main")))) continue;
        ns_shader_stage stage = ns_shader_stage_infer(b->vm, b->ctx, i);
        if (stage == NS_SHADER_STAGE_AUTO) continue;
        ns_return_str source = ns_shader_transpile(b->vm, b->ctx, i, NS_SHADER_WGSL, stage);
        if (ns_return_is_error(source)) return ns_return_change_type(bool, source);
        ns_ssa_shader shader = {.id = (u32)ns_array_length(b->m->shaders) + 1,
                                .stage = stage, .name = symbol->name, .wgsl = source.r};
        if (stage == NS_SHADER_STAGE_VERTEX && ns_array_length(symbol->fn.args) > 0 &&
            ns_type_is(symbol->fn.args[0].val.t, NS_TYPE_STRUCT)) {
            ns_symbol *input = &b->vm->symbols[ns_type_index(symbol->fn.args[0].val.t)];
            shader.vertex_stride = ns_ssa_wasm32_size(b, symbol->fn.args[0].val.t);
            for (i32 f = 0, fl = (i32)ns_array_length(input->st.fields); f < fl; ++f) {
                ns_array_push(shader.vertex_offsets, ns_ssa_wasm32_field_offset(b, input, f));
                ns_array_push(shader.vertex_sizes, ns_ssa_wasm32_size(b, input->st.fields[f].t));
            }
        }
        ns_array_push(b->m->shaders, shader);
    }
    return ns_return_ok(bool, true);
}

static void ns_ssa_record_import(ns_ssa_builder *b, ns_symbol *symbol) {
    if (!symbol || symbol->type != NS_SYMBOL_FN || ns_str_is_empty(symbol->lib)) return;
    for (i32 i = 0, l = (i32)ns_array_length(b->m->imports); i < l; ++i) {
        ns_ssa_import *import = &b->m->imports[i];
        if (ns_str_equals(import->module, symbol->lib) && ns_str_equals(import->name, symbol->name)) return;
    }
    ns_ssa_import import = {.module = symbol->lib, .name = symbol->name,
                            .ret = ns_enum_underlying_type(b->vm, symbol->fn.ret)};
    for (i32 i = 0, l = (i32)ns_array_length(symbol->fn.args); i < l; ++i) {
        ns_array_push(import.params, ns_enum_underlying_type(b->vm, symbol->fn.args[i].val.t));
    }
    ns_array_push(b->m->imports, import);
}

static ns_shader_target ns_ssa_native_shader_target(void) {
#if defined(__APPLE__)
    return NS_SHADER_MSL;
#else
    return NS_SHADER_HLSL;
#endif
}

static ns_str ns_ssa_unquote(ns_str s) {
    if (s.len >= 2 && s.data[0] == '"' && s.data[s.len - 1] == '"') {
        return (ns_str){.data = s.data + 1, .len = s.len - 2};
    }
    return s;
}

static ns_ast_t *ns_ssa_call_arg(ns_ssa_builder *b, ns_ast_t *n, i32 arg_index) {
    i32 next = n->next;
    for (i32 ai = 0; ai < arg_index && next > 0; ++ai) {
        next = b->ctx->nodes[next].next;
    }
    return ns_ssa_unwrap_expr(b, next);
}

static ns_str ns_ssa_call_arg_token(ns_ssa_builder *b, ns_ast_t *n, i32 arg_index) {
    ns_ast_t *arg = ns_ssa_call_arg(b, n, arg_index);
    if (!arg || arg->type != NS_AST_PRIMARY_EXPR) return ns_str_null;
    return ns_ssa_unquote(arg->primary_expr.token.val);
}

static i32 ns_ssa_emit_str_const(ns_ssa_builder *b, ns_str text, i32 ast) {
    ns_str owned = ns_str_range(ns_malloc((szt)text.len + 1), text.len);
    if (text.len > 0 && text.data) memcpy(owned.data, text.data, (szt)text.len);
    owned.data[text.len] = 0;
    ns_array_push(b->m->owned_strings, owned);
    return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_str, owned,
                             (ns_token_t){.type = NS_TOKEN_STR_LITERAL, .val = owned}, ast);
}

// Fold shader_transpile / shader_transpile_stage / shader_entry when the first
// argument is a known fn. Native builds have no VM, so these must become
// string constants. The host's shader language is used (MSL on Apple).
static i32 ns_ssa_fold_shader_call(ns_ssa_builder *b, ns_ast_t *n, i32 ast,
                                   ns_str callee_name, ns_str callee_module) {
    if (!ns_str_equals(callee_module, ns_str_cstr("shader"))) return -1;
    ns_bool is_transpile = ns_str_equals_STR(callee_name, "shader_transpile");
    ns_bool is_transpile_stage = ns_str_equals_STR(callee_name, "shader_transpile_stage");
    ns_bool is_entry = ns_str_equals_STR(callee_name, "shader_entry");
    if (!is_transpile && !is_transpile_stage && !is_entry) return -1;

    ns_str fn_name = ns_ssa_call_arg_token(b, n, 0);
    if (fn_name.len == 0) return -1;
    ns_symbol *fn_sym = ns_vm_find_symbol(b->vm, fn_name, false);
    if (!fn_sym || fn_sym->type != NS_SYMBOL_FN) return -1;
    i32 fn_index = (i32)(fn_sym - b->vm->symbols);
    ns_shader_target target = ns_ssa_native_shader_target();

    if (is_entry) {
        return ns_ssa_emit_str_const(b, ns_shader_entry_name(target, fn_sym->name), ast);
    }

    ns_shader_stage stage = NS_SHADER_STAGE_AUTO;
    if (is_transpile_stage) {
        ns_str stage_s = ns_ssa_call_arg_token(b, n, 2);
        if (ns_str_equals(stage_s, ns_str_cstr("vertex"))) stage = NS_SHADER_STAGE_VERTEX;
        else if (ns_str_equals(stage_s, ns_str_cstr("fragment"))) stage = NS_SHADER_STAGE_FRAGMENT;
        else if (ns_str_equals(stage_s, ns_str_cstr("compute"))) stage = NS_SHADER_STAGE_COMPUTE;
        else return -1;
    }

    ns_ast_ctx *fn_ctx = fn_sym->fn.ctx ? fn_sym->fn.ctx : b->ctx;
    ns_return_str src = ns_shader_transpile(b->vm, fn_ctx, fn_index, target, stage);
    if (ns_return_is_error(src)) {
        b->fold_error = src;
        return -1;
    }
    i32 value = ns_ssa_emit_str_const(b, src.r, ast);
    ns_array_free(src.r.data);
    return value;
}

static i32 ns_ssa_lower_assign(ns_ssa_builder *b, ns_ast_t *n, i32 i) {
    i32 rhs = ns_ssa_lower_expr(b, n->binary_expr.right);
    i32 left = n->binary_expr.left;
    ns_ast_t *ln = &b->ctx->nodes[left];
    if (ln->type == NS_AST_EXPR) {
        ln = &b->ctx->nodes[ln->expr.body];
    }

    if (n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP && n->binary_expr.op.val.len > 1 &&
        ln->type == NS_AST_PRIMARY_EXPR && ln->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        ns_token_t arith = n->binary_expr.op;
        arith.val.len -= 1;
        ns_ssa_op op = NS_SSA_OP_UNKNOWN;
        if (ns_str_equals_STR(arith.val, "+")) op = NS_SSA_OP_ADD;
        else if (ns_str_equals_STR(arith.val, "-")) op = NS_SSA_OP_SUB;
        else if (ns_str_equals_STR(arith.val, "*")) op = NS_SSA_OP_MUL;
        else if (ns_str_equals_STR(arith.val, "/")) op = NS_SSA_OP_DIV;
        else if (ns_str_equals_STR(arith.val, "%")) op = NS_SSA_OP_MOD;
        else if (ns_str_equals_STR(arith.val, "&")) op = NS_SSA_OP_BAND;
        else if (ns_str_equals_STR(arith.val, "|")) op = NS_SSA_OP_BOR;
        else if (ns_str_equals_STR(arith.val, "^")) op = NS_SSA_OP_BXOR;
        else if (ns_str_equals_STR(arith.val, "<<")) op = NS_SSA_OP_SHL;
        else if (ns_str_equals_STR(arith.val, ">>")) op = NS_SSA_OP_SHR;
        i32 cur = -1;
        i32 local = ns_ssa_env_find(b->env, ln->primary_expr.token.val);
        i32 global = ns_ssa_global_index(b, ln->primary_expr.token.val);
        if (local >= 0) {
            cur = b->env[local].value;
            if (ns_type_is_ref(b->env[local].type)) {
                cur = ns_ssa_load_ref(b, cur, b->env[local].type, i);
            }
        } else if (global >= 0) {
            cur = ns_ssa_emit_value(b, NS_SSA_OP_GLOBAL_GET, -1, global, b->m->globals[global].type,
                                    ln->primary_expr.token.val, ln->primary_expr.token, i);
            if (ns_ssa_needs_box(b, ln->primary_expr.token.val) ||
                ns_type_is_ref(b->m->globals[global].type)) {
                cur = ns_ssa_load_ref(b, cur, b->m->globals[global].type, i);
            }
        }
        if (cur >= 0 && op != NS_SSA_OP_UNKNOWN) {
            ns_type ct = ns_ssa_value_type(b, cur);
            i32 next = ns_ssa_emit_value(b, op, cur, -1, ct, ns_str_null, arith, i);
            b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].b = rhs;
            rhs = next;
        }
    }

    if (ln->type == NS_AST_PRIMARY_EXPR && ln->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        rhs = ns_ssa_clone_struct(b, rhs, i);
        i32 local = ns_ssa_env_find(b->env, ln->primary_expr.token.val);
        i32 global = ns_ssa_global_index(b, ln->primary_expr.token.val);
        if (local >= 0 && ns_type_is(b->env[local].type, NS_TYPE_UNION)) {
            rhs = ns_ssa_union_wrap(b, rhs, b->env[local].type, i);
        } else if (local < 0 && global >= 0 && ns_type_is(b->m->globals[global].type, NS_TYPE_UNION)) {
            rhs = ns_ssa_union_wrap(b, rhs, b->m->globals[global].type, i);
        }
        if (local >= 0 && ns_type_is_ref(b->env[local].type)) {
            ns_type inner = ns_type_set_ref(b->env[local].type, false);
            if (ns_ssa_heap_payload(inner) && ns_type_is(inner, NS_TYPE_STRUCT)) {
                ns_ssa_inst copy = NS_SSA_INST_INIT(NS_SSA_OP_STORE, i);
                copy.a = b->env[local].value;
                copy.b = rhs;
                copy.c = 0;
                copy.target0 = ns_ssa_wasm32_size(b, inner);
                copy.type = inner;
                ns_ssa_emit_raw(b, copy);
            } else {
                ns_ssa_emit_store_off(b, b->env[local].value, rhs, 0, inner, i);
            }
            return rhs;
        }
        if (local < 0 && global >= 0) {
            if (ns_ssa_needs_box(b, ln->primary_expr.token.val) ||
                ns_type_is_ref(b->m->globals[global].type)) {
                i32 box = ns_ssa_emit_value(b, NS_SSA_OP_GLOBAL_GET, -1, global,
                                            b->m->globals[global].type,
                                            ln->primary_expr.token.val, ln->primary_expr.token, i);
                ns_type inner = ns_type_set_ref(b->m->globals[global].type, false);
                ns_ssa_emit_store_off(b, box, rhs, 0, inner, i);
                return rhs;
            }
            ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_GLOBAL_SET, i);
            store.a = rhs;
            store.c = global;
            store.type = b->m->globals[global].type;
            store.name = ln->primary_expr.token.val;
            ns_ssa_emit_raw(b, store);
            return rhs;
        }
        ns_type rhs_type = ns_ssa_value_type(b, rhs);
        i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_COPY, rhs, -1, rhs_type, ln->primary_expr.token.val, ln->primary_expr.token, i);
        ns_ssa_env_bind(b, ln->primary_expr.token.val, dst, rhs_type);
        ns_ssa_foreach_assign_writeback(b, ln->primary_expr.token.val, i);
        if (b->capture_env >= 0) {
            for (i32 ci = 0, cl = (i32)ns_array_length(b->captures); ci < cl; ++ci) {
                if (ns_str_equals(b->captures[ci].name, ln->primary_expr.token.val)) {
                    ns_ssa_emit_store_off(b, b->capture_env, dst, b->captures[ci].offset,
                                          b->captures[ci].type, i);
                    break;
                }
            }
        }
        return dst;
    }

    if (ln->type == NS_AST_INDEX_EXPR) {
        i32 table = ns_ssa_lower_expr(b, ln->index_expr.table);
        i32 index = ns_ssa_lower_expr(b, ln->index_expr.expr);
        ns_type table_type = ns_ssa_value_type(b, table);
        if (ns_type_is_array(table_type)) {
            ns_type element = table_type;
            element.array = false;
            ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_ARRAY_STORE, i);
            store.a = table;
            store.b = rhs;
            store.target0 = index;
            store.c = ns_ssa_wasm32_size(b, element);
            store.type = element;
            ns_ssa_emit_raw(b, store);
            return rhs;
        }
        if (ns_type_is(table_type, NS_TYPE_DICT)) {
            i32 args[3] = {table, index, rhs};
            ns_ssa_emit_named_call(b, "ns_rt_map_set", args, 3, ns_type_void, i);
            return rhs;
        }
    }

    if (ln->type == NS_AST_MEMBER_EXPR) {
        i32 object = ns_ssa_lower_expr(b, ln->member_expr.left);
        ns_type object_type = ns_ssa_value_type(b, object);
        ns_ast_t *member = ns_ssa_unwrap_expr(b, ln->member_expr.right);
        if (ns_type_is(object_type, NS_TYPE_STRUCT) && member && member->type == NS_AST_PRIMARY_EXPR) {
            ns_symbol *st = &b->vm->symbols[ns_type_index(object_type)];
            i32 field = ns_struct_field_index(st, member->primary_expr.token.val);
            if (field >= 0) {
                ns_bool native = ns_ssa_native_struct(object_type);
                ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_STORE, i);
                store.a = object;
                store.b = rhs;
                store.c = ns_ssa_field_off(b, st, field, native);
                store.target0 = ns_ssa_field_sz(b, st, field, native);
                store.type = st->st.fields[field].t;
                ns_ssa_emit_raw(b, store);
                return rhs;
            }
        }
    }

    ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
    trap.a = rhs;
    ns_ssa_emit_raw(b, trap);
    return rhs;
}

static i32 ns_ssa_lower_expr(ns_ssa_builder *b, i32 i) {
    if (i <= 0) return -1;
    ns_ast_t *n = &b->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR:
        return ns_ssa_lower_expr(b, n->expr.body);
    case NS_AST_PRIMARY_EXPR:
        return ns_ssa_lower_primary(b, n, i);
    case NS_AST_BINARY_EXPR: {
        if (n->binary_expr.op.type == NS_TOKEN_ASSIGN || n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP) {
            return ns_ssa_lower_assign(b, n, i);
        }
        i32 lhs = ns_ssa_lower_expr(b, n->binary_expr.left);
        ns_ssa_op op = ns_ssa_binary_op(n->binary_expr.op);
        ns_type lhs_type = ns_ssa_value_type(b, lhs);
        if ((op == NS_SSA_OP_AND || op == NS_SSA_OP_OR) && ns_type_is(lhs_type, NS_TYPE_BOOL)) {
            i32 rhs_block = ns_ssa_new_block(b, n->binary_expr.right);
            i32 merge = ns_ssa_new_block(b, i);
            i32 lhs_exit = b->block;
            if (op == NS_SSA_OP_AND) ns_ssa_emit_branch(b, lhs, rhs_block, merge, i);
            else ns_ssa_emit_branch(b, lhs, merge, rhs_block, i);
            b->block = rhs_block;
            b->fn->blocks[rhs_block].terminated = false;
            i32 rhs = ns_ssa_lower_expr(b, n->binary_expr.right);
            i32 rhs_exit = b->block;
            if (!b->fn->blocks[rhs_exit].terminated) ns_ssa_emit_jump(b, merge, i);
            b->block = merge;
            b->fn->blocks[merge].terminated = false;
            i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_PHI, lhs, -1, ns_type_bool,
                                        ns_str_null, n->binary_expr.op, i);
            ns_ssa_inst *phi = &b->fn->insts[ns_array_last(b->fn->blocks[merge].insts)[0]];
            ns_ssa_phi_set_incoming(phi, lhs_exit, lhs);
            ns_ssa_phi_set_incoming(phi, rhs_exit, rhs);
            return dst;
        }
        i32 rhs = ns_ssa_lower_expr(b, n->binary_expr.right);
        ns_type rhs_type = ns_ssa_value_type(b, rhs);
        if (ns_type_is(lhs_type, NS_TYPE_STRUCT) || ns_type_is(rhs_type, NS_TYPE_STRUCT)) {
            ns_str ln = ns_vm_get_type_name(b->vm, lhs_type);
            ns_str rn = ns_vm_get_type_name(b->vm, rhs_type);
            ns_str fname = ns_ops_override_name(ln, rn, n->binary_expr.op);
            if (fname.data) ns_array_push(b->m->owned_strings, fname);
            ns_symbol *fn = fname.len > 0 ? ns_vm_find_symbol(b->vm, fname, true) : ns_null;
            if (fn && fn->type == NS_SYMBOL_FN) {
                i32 a0 = ns_ssa_clone_struct(b, lhs, n->binary_expr.left);
                i32 a1 = ns_ssa_clone_struct(b, rhs, n->binary_expr.right);
                i32 args[2] = {a0, a1};
                return ns_ssa_emit_named_call(b, fname.data, args, 2, fn->fn.ret, i);
            }
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            trap.a = lhs;
            trap.b = rhs;
            ns_ssa_emit_raw(b, trap);
            return -1;
        }
        if (op == NS_SSA_OP_UNKNOWN) {
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            trap.a = lhs;
            trap.b = rhs;
            ns_ssa_emit_raw(b, trap);
            return -1;
        }
        if (lhs_type.type != rhs_type.type &&
            ns_type_is_number(lhs_type) && ns_type_is_number(rhs_type)) {
            ns_type up = ns_vm_number_type_upgrade(lhs_type, rhs_type);
            if (!ns_type_is(up, NS_TYPE_UNKNOWN) && up.type != lhs_type.type) {
                lhs = ns_ssa_emit_value(b, NS_SSA_OP_CAST, lhs, -1, up, ns_str_null,
                                        n->binary_expr.op, i);
                lhs_type = up;
            }
            if (!ns_type_is(up, NS_TYPE_UNKNOWN) && up.type != rhs_type.type) {
                rhs = ns_ssa_emit_value(b, NS_SSA_OP_CAST, rhs, -1, up, ns_str_null,
                                        n->binary_expr.op, i);
                rhs_type = up;
            }
        }
        ns_type result_type = lhs_type;
        if (op == NS_SSA_OP_EQ || op == NS_SSA_OP_NE || op == NS_SSA_OP_LT || op == NS_SSA_OP_LE ||
            op == NS_SSA_OP_GT || op == NS_SSA_OP_GE || op == NS_SSA_OP_AND || op == NS_SSA_OP_OR) {
            result_type = ns_type_bool;
        }
        i32 dst = ns_ssa_emit_value(b, op, lhs, -1, result_type, ns_str_null, n->binary_expr.op, i);
        ns_ssa_inst *inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        inst->b = rhs;
        return dst;
    }
    case NS_AST_UNARY_EXPR: {
        i32 v = ns_ssa_lower_expr(b, n->unary_expr.expr);
        ns_type vt = ns_ssa_value_type(b, v);
        if (ns_str_equals_STR(n->unary_expr.op.val, "~")) {
            ns_str owned = ns_str_range(ns_malloc(3), 2);
            memcpy(owned.data, "-1", 3);
            ns_array_push(b->m->owned_strings, owned);
            i32 ones = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_i64, owned,
                                        (ns_token_t){.type = NS_TOKEN_INT_LITERAL, .val = owned}, i);
            i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_BXOR, v, -1, vt, ns_str_null, n->unary_expr.op, i);
            b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].b = ones;
            return dst;
        }
        if (n->unary_expr.op.type == NS_TOKEN_REF) {
            ns_ast_t *inner = ns_ssa_unwrap_expr(b, n->unary_expr.expr);
            if (inner && inner->type == NS_AST_PRIMARY_EXPR &&
                inner->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
                ns_str name = inner->primary_expr.token.val;
                i32 local = ns_ssa_env_find(b->env, name);
                if (local >= 0) {
                    if (ns_type_is_ref(b->env[local].type)) return b->env[local].value;
                    i32 boxed = ns_ssa_make_ref(b, b->env[local].value, b->env[local].type, i);
                    ns_ssa_env_bind(b, name, boxed, ns_type_set_ref(b->env[local].type, true));
                    return boxed;
                }
                i32 global = ns_ssa_global_index(b, name);
                if (global >= 0) {
                    i32 gv = ns_ssa_emit_value(b, NS_SSA_OP_GLOBAL_GET, -1, global,
                                               b->m->globals[global].type, name,
                                               inner->primary_expr.token, i);
                    if (ns_type_is_ref(b->m->globals[global].type)) return gv;
                    i32 boxed = ns_ssa_make_ref(b, gv, b->m->globals[global].type, i);
                    ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_GLOBAL_SET, i);
                    store.a = boxed;
                    store.c = global;
                    store.type = ns_type_set_ref(b->m->globals[global].type, true);
                    store.name = name;
                    ns_ssa_emit_raw(b, store);
                    b->m->globals[global].type = store.type;
                    return boxed;
                }
            }
            if (ns_type_is_ref(vt)) return v;
            return ns_ssa_make_ref(b, v, vt, i);
        }
        if (n->unary_expr.op.type == NS_TOKEN_AWAIT) {
            ns_type result = (ns_type){.type = NS_TYPE_ANY};
            if (ns_type_is(vt, NS_TYPE_TASK)) {
                i32 tidx = ns_type_index(vt);
                if (tidx >= 0 && tidx < (i32)ns_array_length(b->vm->symbols)) {
                    result = b->vm->symbols[tidx].ct.val;
                }
            }
            i32 args[1] = {v};
            return ns_ssa_emit_named_call(b, "ns_rt_task_await", args, 1, result, i);
        }
        ns_ssa_op op = NS_SSA_OP_UNKNOWN;
        if (ns_str_equals_STR(n->unary_expr.op.val, "-")) op = NS_SSA_OP_NEG;
        if (ns_str_equals_STR(n->unary_expr.op.val, "!")) op = NS_SSA_OP_NOT;
        if (op == NS_SSA_OP_UNKNOWN) {
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            trap.a = v;
            ns_ssa_emit_raw(b, trap);
            return -1;
        }
        if (op == NS_SSA_OP_NOT) vt = ns_type_bool;
        return ns_ssa_emit_value(b, op, v, -1, vt, ns_str_null, n->unary_expr.op, i);
    }
    case NS_AST_CALL_EXPR: {
        ns_ast_t *cb_callee = ns_ssa_unwrap_expr(b, n->call_expr.callee);
        ns_ast_t *cb_arg = n->call_expr.arg_count == 1 ? ns_ssa_unwrap_expr(b, n->next) : ns_null;
        if (cb_callee && cb_callee->type == NS_AST_MEMBER_EXPR &&
            cb_arg && cb_arg->type == NS_AST_BLOCK_EXPR) {
            i32 obj = ns_ssa_lower_expr(b, cb_callee->member_expr.left);
            i32 closure = ns_ssa_lower_expr(b, n->next);
            i32 cb_args[1] = {closure};
            i32 tramp = ns_ssa_emit_named_call(b, "ns_rt_callback", cb_args, 1, ns_type_i64, i);
            ns_type ot = ns_ssa_value_type(b, obj);
            ns_ast_t *member = ns_ssa_unwrap_expr(b, cb_callee->member_expr.right);
            if (ns_type_is(ot, NS_TYPE_STRUCT) && member && member->type == NS_AST_PRIMARY_EXPR) {
                ns_symbol *st = &b->vm->symbols[ns_type_index(ot)];
                i32 field = ns_struct_field_index(st, member->primary_expr.token.val);
                if (field >= 0) {
                    ns_bool native = ns_ssa_native_struct(ot);
                    ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_STORE, i);
                    store.a = obj;
                    store.b = tramp;
                    store.c = ns_ssa_field_off(b, st, field, native);
                    store.target0 = ns_ssa_field_sz(b, st, field, native);
                    store.type = st->st.fields[field].t;
                    ns_ssa_emit_raw(b, store);
                    return tramp;
                }
            }
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            trap.a = tramp;
            ns_ssa_emit_raw(b, trap);
            return tramp;
        }
        ns_ast_t *builtin = ns_ssa_unwrap_expr(b, n->call_expr.callee);
        if (builtin && builtin->type == NS_AST_PRIMARY_EXPR &&
            builtin->primary_expr.token.type == NS_TOKEN_IDENTIFIER &&
            n->call_expr.arg_count == 2) {
            ns_str bname = builtin->primary_expr.token.val;
            ns_bool is_insert = ns_str_equals_STR(bname, "insert");
            ns_bool is_has = ns_str_equals_STR(bname, "has");
            ns_bool is_remove = ns_str_equals_STR(bname, "remove");
            ns_symbol *user = ns_vm_find_symbol(b->vm, bname, false);
            if ((is_insert || is_has || is_remove) && (!user || user->type != NS_SYMBOL_FN)) {
                i32 a0n = n->next;
                i32 a1n = a0n > 0 ? b->ctx->nodes[a0n].next : 0;
                i32 c = ns_ssa_lower_expr(b, a0n);
                i32 k = ns_ssa_lower_expr(b, a1n);
                i32 args[2] = {c, k};
                const char *rt = is_insert ? "ns_rt_map_insert" : is_has ? "ns_rt_map_has" : "ns_rt_map_remove";
                return ns_ssa_emit_named_call(b, rt, args, 2, ns_type_bool, i);
            }
        }
        ns_type result_type = ns_type_unknown;
        ns_str callee_name = ns_str_null;
        ns_str callee_module = ns_str_null;
        ns_ast_t *callee_node = ns_ssa_unwrap_expr(b, n->call_expr.callee);
        ns_symbol *callee_sym = ns_null;
        ns_bool value_call = false;
        if (callee_node && callee_node->type == NS_AST_PRIMARY_EXPR &&
            callee_node->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
            ns_str cname = callee_node->primary_expr.token.val;
            if (ns_ssa_env_find(b->env, cname) >= 0) value_call = true;
            else {
                i32 gi = ns_ssa_global_index(b, cname);
                if (gi >= 0 && (ns_type_is(b->m->globals[gi].type, NS_TYPE_FN) ||
                                ns_type_is(b->m->globals[gi].type, NS_TYPE_BLOCK))) {
                    value_call = true;
                }
            }
        }
        if (!value_call) {
            if (n->call_expr.rt >= 0 && n->call_expr.rt < (i32)ns_array_length(b->vm->symbols)) {
                callee_sym = &b->vm->symbols[n->call_expr.rt];
            } else if (callee_node && callee_node->type == NS_AST_PRIMARY_EXPR &&
                       callee_node->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
                callee_sym = ns_vm_find_symbol(b->vm, callee_node->primary_expr.token.val, false);
            }
        }
        if (callee_sym && callee_sym->type == NS_SYMBOL_STRUCT) {
            return ns_ssa_lower_struct_ctor(b, n, callee_sym, i);
        }
        ns_bool skip_clone = false;
        ns_bool async_call = false;
        if (callee_sym && callee_sym->type == NS_SYMBOL_FN) {
            result_type = ns_enum_underlying_type(b->vm, callee_sym->fn.ret);
            callee_name = callee_sym->name;
            callee_module = callee_sym->lib;
            i32 folded = ns_ssa_fold_shader_call(b, n, i, callee_name, callee_module);
            if (folded >= 0) return folded;
            if (ns_return_is_error(b->fold_error)) return -1;
            // Only `ref fn` crosses the FFI boundary. Regular bodies from
            // simd.ns / gpu.ns wrappers are compiled into this object.
            if (callee_sym->fn.fn.t.ref) {
                ns_ssa_record_import(b, callee_sym);
            } else {
                callee_module = ns_str_null;
            }
            skip_clone = ns_str_equals_STR(callee_sym->name, "next");
            async_call = callee_sym->fn.fn_type == NS_FN_ASYNC && !callee_sym->fn.fn.t.ref;
        }
        i32 callee = -1;
        if (value_call || callee_name.len == 0) {
            callee = ns_ssa_lower_expr(b, n->call_expr.callee);
        }
        if (value_call || callee_name.len == 0) {
            ns_type ct = ns_ssa_value_type(b, callee);
            if (ns_type_is(ct, NS_TYPE_FN) || ns_type_is(ct, NS_TYPE_BLOCK)) {
                value_call = true;
                result_type = ns_ssa_callable_ret(b, ct);
                callee_name = ns_str_null;
            }
        }
        i32 next = n->next;
        i32 *avals = ns_null;
        if (value_call) ns_array_push(avals, callee);
        ns_bool is_dispatch = ns_str_equals_STR(callee_name, "dispatch") &&
                              ns_str_equals(callee_module, ns_str_cstr("task"));
        for (i32 ai = 0; ai < n->call_expr.arg_count && next > 0; ++ai) {
            i32 av = -1;
            if (is_dispatch && ai == 0) {
                ns_ast_t *qn = ns_ssa_unwrap_expr(b, next);
                if (qn && qn->type == NS_AST_PRIMARY_EXPR &&
                    qn->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
                    ns_str qn_name = qn->primary_expr.token.val;
                    i32 qv = -1;
                    if (ns_str_equals_STR(qn_name, "queue_main")) qv = 0;
                    else if (ns_str_equals_STR(qn_name, "queue_worker")) qv = 1;
                    else if (ns_str_equals_STR(qn_name, "queue_idle")) qv = 2;
                    if (qv >= 0) av = ns_ssa_emit_i32_const(b, qv, next);
                }
            }
            if (av < 0) av = ns_ssa_lower_expr(b, next);
            if (!skip_clone) av = ns_ssa_clone_struct(b, av, next);
            if (callee_sym && callee_sym->type == NS_SYMBOL_FN &&
                ai < (i32)ns_array_length(callee_sym->fn.args)) {
                ns_type pt = callee_sym->fn.args[ai].val.t;
                if (ns_type_is(pt, NS_TYPE_UNION)) av = ns_ssa_union_wrap(b, av, pt, next);
                if (ns_type_is_ref(pt) && !ns_type_is_ref(ns_ssa_value_type(b, av))) {
                    av = ns_ssa_make_ref(b, av, ns_ssa_value_type(b, av), next);
                }
            }
            ns_array_push(avals, av);
            next = b->ctx->nodes[next].next;
        }
        i32 nargs = (i32)ns_array_length(avals);
        if (async_call && !value_call) {
            i32 fnaddr = ns_ssa_emit_value(b, NS_SSA_OP_FNADDR, -1, -1, ns_type_i64, callee_name,
                                           (ns_token_t){0}, i);
            i32 zero = ns_ssa_emit_i32_const(b, 0, i);
            i32 nconst = ns_ssa_emit_i32_const(b, nargs, i);
            i32 qconst = ns_ssa_emit_i32_const(b, 1, i);
            i32 spawn_args[8];
            spawn_args[0] = fnaddr;
            spawn_args[1] = zero;
            spawn_args[2] = nconst;
            spawn_args[3] = nargs > 0 ? avals[0] : zero;
            spawn_args[4] = nargs > 1 ? avals[1] : zero;
            spawn_args[5] = nargs > 2 ? avals[2] : zero;
            spawn_args[6] = nargs > 3 ? avals[3] : zero;
            spawn_args[7] = qconst;
            ns_array_free(avals);
            ns_type handle_t = ns_task_type(b->vm, result_type);
            return ns_ssa_emit_named_call(b, "ns_rt_task_spawn", spawn_args, 8, handle_t, i);
        }
        for (i32 ai = 0; ai < nargs; ++ai) {
            ns_ssa_inst arg = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
            arg.a = avals[ai];
            ns_ssa_emit_raw(b, arg);
        }
        ns_array_free(avals);
        i32 value = ns_ssa_emit_value(b, NS_SSA_OP_CALL, callee, nargs,
                                      result_type, callee_name, (ns_token_t){0}, i);
        ns_ssa_inst *call = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        call->module = callee_module;
        return value;
    }
    case NS_AST_CAST_EXPR: {
        i32 src = ns_ssa_lower_expr(b, n->cast_expr.expr);
        ns_return_type parsed = ns_vm_parse_type_by_token(b->vm, n->cast_expr.type,
                                                          ns_ast_state_loc(b->ctx, n->state));
        ns_type dst = ns_return_is_error(parsed) ? ns_type_unknown : parsed.r;
        dst = ns_enum_underlying_type(b->vm, dst);
        ns_type src_t = ns_ssa_value_type(b, src);
        if (ns_type_is(dst, NS_TYPE_UNION)) return ns_ssa_union_wrap(b, src, dst, i);
        if (ns_type_is(src_t, NS_TYPE_UNION) && !ns_type_is(dst, NS_TYPE_UNION)) {
            return ns_ssa_union_unwrap(b, src, dst, i);
        }
        return ns_ssa_emit_value(b, NS_SSA_OP_CAST, src, -1, dst, ns_str_null, n->cast_expr.type, i);
    }
    case NS_AST_MEMBER_EXPR: {
        ns_symbol *en = ns_ssa_enum_from_expr(b, n->member_expr.left);
        ns_ast_t *member = ns_ssa_unwrap_expr(b, n->member_expr.right);
        if (en && member && member->type == NS_AST_PRIMARY_EXPR) {
            i32 mi = ns_enum_member_index(en, member->primary_expr.token.val);
            if (mi >= 0) {
                ns_str literal = ns_ssa_enum_literal(b, en, en->en.members[mi].value);
                ns_token_t token = {.type = NS_TOKEN_INT_LITERAL, .val = literal};
                return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, en->en.underlying,
                                         literal, token, i);
            }
        }
        i32 obj = ns_ssa_lower_expr(b, n->member_expr.left);
        ns_type object_type = ns_ssa_value_type(b, obj);
        if (member && member->type == NS_AST_PRIMARY_EXPR &&
            (ns_type_is_array(object_type) || ns_type_is(object_type, NS_TYPE_STRING) ||
             ns_type_is(object_type, NS_TYPE_DICT) || ns_type_is(object_type, NS_TYPE_SET)) &&
            (ns_str_equals_STR(member->primary_expr.token.val, "len") ||
             ns_str_equals_STR(member->primary_expr.token.val, "size") ||
             ns_str_equals_STR(member->primary_expr.token.val, "cap"))) {
            i32 offset = ns_str_equals_STR(member->primary_expr.token.val, "cap") ? 8 : 4;
            return ns_ssa_emit_value(b, NS_SSA_OP_LOAD, obj, offset, ns_type_i32,
                                     member->primary_expr.token.val, member->primary_expr.token, i);
        }
        if (ns_type_is(object_type, NS_TYPE_STRUCT) && member && member->type == NS_AST_PRIMARY_EXPR) {
            ns_symbol *st = &b->vm->symbols[ns_type_index(object_type)];
            i32 field = ns_struct_field_index(st, member->primary_expr.token.val);
            if (field >= 0) {
                ns_bool native = ns_ssa_native_struct(object_type);
                i32 result = ns_ssa_emit_value(b, NS_SSA_OP_LOAD, obj,
                                               ns_ssa_field_off(b, st, field, native),
                                               st->st.fields[field].t, member->primary_expr.token.val,
                                               member->primary_expr.token, i);
                b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].target0 =
                    ns_ssa_field_sz(b, st, field, native);
                return result;
            }
        }
        i32 field = ns_ssa_lower_expr(b, n->member_expr.right);
        i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_MEMBER, obj, -1, ns_type_unknown, ns_str_null, (ns_token_t){0}, i);
        b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].b = field;
        return dst;
    }
    case NS_AST_INDEX_EXPR: {
        i32 table = ns_ssa_lower_expr(b, n->index_expr.table);
        i32 idx = ns_ssa_lower_expr(b, n->index_expr.expr);
        ns_type table_type = ns_ssa_value_type(b, table);
        ns_type element = ns_type_unknown;
        i32 stride = 0;
        if (ns_type_is_array(table_type)) {
            element = table_type;
            element.array = false;
            stride = ns_ssa_wasm32_size(b, element);
        } else if (ns_type_is(table_type, NS_TYPE_STRING)) {
            element = ns_type_i32;
            stride = 1;
        } else if (ns_type_is(table_type, NS_TYPE_DICT)) {
            i32 args[2] = {table, idx};
            ns_type val = ns_type_unknown;
            i32 tidx = ns_type_index(table_type);
            if (tidx >= 0 && tidx < (i32)ns_array_length(b->vm->symbols)) val = b->vm->symbols[tidx].ct.val;
            return ns_ssa_emit_named_call(b, "ns_rt_map_get", args, 2, val, i);
        }
        i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_INDEX, table, stride, element, ns_str_null, (ns_token_t){0}, i);
        ns_ssa_inst *inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        inst->b = idx;
        return dst;
    }
    case NS_AST_DESIG_EXPR: {
        ns_symbol *st = ns_vm_find_symbol(b->vm, n->desig_expr.name.val, true);
        if (!st || st->type != NS_SYMBOL_STRUCT) break;
        i32 object = ns_ssa_emit_value(b, NS_SSA_OP_ALLOC, -1, ns_ssa_wasm32_size(b, st->st.st.t),
                                       st->st.st.t, n->desig_expr.name.val, n->desig_expr.name, i);
        ns_ast_t *field_node = n;
        for (i32 field_i = 0; field_i < n->desig_expr.count; ++field_i) {
            field_node = &b->ctx->nodes[field_node->next];
            i32 resolved = field_node->field_def.rt.index;
            i32 value = ns_ssa_lower_expr(b, field_node->field_def.expr);
            ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_STORE, field_node->field_def.expr);
            store.a = object;
            store.b = value;
            store.c = ns_ssa_wasm32_field_offset(b, st, resolved);
            store.target0 = ns_ssa_wasm32_size(b, st->st.fields[resolved].t);
            store.type = st->st.fields[resolved].t;
            ns_ssa_emit_raw(b, store);
        }
        return object;
    }
    case NS_AST_ARRAY_EXPR: {
        ns_type array_type = n->array_expr.rt;
        if (ns_type_is(array_type, NS_TYPE_DICT) || ns_type_is(array_type, NS_TYPE_SET)) {
            i32 count = n->array_expr.literal
                ? ns_ssa_emit_i32_const(b, n->array_expr.elem_count, i)
                : ns_ssa_lower_expr(b, n->array_expr.count_expr);
            i32 flags = ns_ssa_emit_i32_const(b, ns_ssa_map_flags(b, array_type), i);
            i32 args[2] = {count, flags};
            return ns_ssa_emit_named_call(b, "ns_rt_map_new", args, 2, array_type, i);
        }
        ns_type element = array_type;
        element.array = false;
        i32 stride = ns_ssa_wasm32_size(b, element);
        i32 count;
        if (n->array_expr.literal) {
            char count_text[24];
            i32 count_len = snprintf(count_text, sizeof(count_text), "%d", n->array_expr.elem_count);
            ns_str owned = ns_str_range(ns_malloc((szt)count_len + 1), count_len);
            memcpy(owned.data, count_text, (szt)count_len + 1);
            ns_array_push(b->m->owned_strings, owned);
            count = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_i32, owned,
                                      (ns_token_t){.type = NS_TOKEN_INT_LITERAL, .val = owned}, i);
        } else {
            count = ns_ssa_lower_expr(b, n->array_expr.count_expr);
        }
        i32 array = ns_ssa_emit_value(b, NS_SSA_OP_ARRAY_NEW, count, stride, array_type,
                                      ns_str_null, (ns_token_t){0}, i);
        if (n->array_expr.literal) {
            i32 next = n->next;
            for (i32 element_i = 0; element_i < n->array_expr.elem_count; ++element_i) {
                i32 value = ns_ssa_lower_expr(b, next);
                char index_text[24];
                i32 index_len = snprintf(index_text, sizeof(index_text), "%d", element_i);
                ns_str owned = ns_str_range(ns_malloc((szt)index_len + 1), index_len);
                memcpy(owned.data, index_text, (szt)index_len + 1);
                ns_array_push(b->m->owned_strings, owned);
                i32 index = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_i32, owned,
                                              (ns_token_t){.type = NS_TOKEN_INT_LITERAL, .val = owned}, next);
                ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_ARRAY_STORE, next);
                store.a = array;
                store.b = value;
                store.target0 = index;
                store.c = stride;
                store.type = element;
                ns_ssa_emit_raw(b, store);
                next = b->ctx->nodes[next].next;
            }
        }
        return array;
    }
    case NS_AST_STR_FMT_EXPR: {
        ns_str fmt = n->str_fmt.fmt;
        i32 acc = -1;
        i32 expr = i;
        i32 remaining = n->str_fmt.expr_count;
        i32 lit_start = 0;
        i32 j = 0;
        while (j <= fmt.len) {
            ns_bool hole = j < fmt.len && fmt.data[j] == '{' &&
                           (j == 0 || fmt.data[j - 1] != '\\');
            if (!hole && j < fmt.len) {
                j++;
                continue;
            }
            if (j > lit_start) {
                ns_str chunk = ns_str_range(ns_malloc((szt)(j - lit_start) + 1), j - lit_start);
                memcpy(chunk.data, fmt.data + lit_start, (szt)(j - lit_start));
                chunk.data[j - lit_start] = 0;
                ns_array_push(b->m->owned_strings, chunk);
                i32 lit = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_str, chunk,
                                            (ns_token_t){.type = NS_TOKEN_STR_LITERAL, .val = chunk}, i);
                if (acc < 0) {
                    acc = lit;
                } else {
                    ns_ssa_inst a0 = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
                    a0.a = acc;
                    ns_ssa_emit_raw(b, a0);
                    ns_ssa_inst a1 = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
                    a1.a = lit;
                    ns_ssa_emit_raw(b, a1);
                    acc = ns_ssa_emit_value(b, NS_SSA_OP_CALL, -1, 2, ns_type_str,
                                            ns_str_cstr("ns_rt_strcat"), (ns_token_t){0}, i);
                }
            }
            if (j >= fmt.len) break;
            j++;
            while (j < fmt.len && fmt.data[j] != '}') j++;
            if (j < fmt.len) j++;
            lit_start = j;
            if (remaining <= 0) continue;
            remaining--;
            expr = b->ctx->nodes[expr].next;
            i32 val = ns_ssa_lower_expr(b, expr);
            ns_type vt = ns_ssa_value_type(b, val);
            i32 piece = val;
            if (!ns_type_is(vt, NS_TYPE_STRING)) {
                ns_str callee = ns_ssa_to_str_symbol_name(b, vt);
                if (callee.len == 0) {
                    const char *helper = ns_type_is_float(vt) ? "ns_rt_ftos" :
                                         ns_type_is(vt, NS_TYPE_BOOL) ? "ns_rt_btos" :
                                         ns_type_is(vt, NS_TYPE_STRUCT) ? NULL :
                                         ns_type_unsigned(vt) ? "ns_rt_utos" : "ns_rt_itos";
                    if (!helper) {
                        ns_str nils = ns_str_cstr("nil");
                        piece = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_str, nils,
                                                  (ns_token_t){.type = NS_TOKEN_STR_LITERAL, .val = nils}, i);
                    } else {
                        ns_ssa_inst a0 = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
                        a0.a = val;
                        ns_ssa_emit_raw(b, a0);
                        piece = ns_ssa_emit_value(b, NS_SSA_OP_CALL, -1, 1, ns_type_str,
                                                  ns_str_cstr((i8 *)helper), (ns_token_t){0}, i);
                    }
                } else {
                    ns_ssa_inst a0 = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
                    a0.a = val;
                    ns_ssa_emit_raw(b, a0);
                    piece = ns_ssa_emit_value(b, NS_SSA_OP_CALL, -1, 1, ns_type_str,
                                              callee, (ns_token_t){0}, i);
                }
            }
            if (acc < 0) {
                acc = piece;
            } else {
                ns_ssa_inst a0 = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
                a0.a = acc;
                ns_ssa_emit_raw(b, a0);
                ns_ssa_inst a1 = NS_SSA_INST_INIT(NS_SSA_OP_ARG, i);
                a1.a = piece;
                ns_ssa_emit_raw(b, a1);
                acc = ns_ssa_emit_value(b, NS_SSA_OP_CALL, -1, 2, ns_type_str,
                                        ns_str_cstr("ns_rt_strcat"), (ns_token_t){0}, i);
            }
        }
        if (acc < 0) {
            return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_str, ns_str_cstr(""),
                                     (ns_token_t){0}, i);
        }
        return acc;
    }
    case NS_AST_BLOCK_EXPR: {
        i32 idx = n->block_expr.rt.index;
        if (idx < 0 || idx >= (i32)ns_array_length(b->vm->symbols)) {
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            ns_ssa_emit_raw(b, trap);
            return -1;
        }
        ns_symbol *sym = &b->vm->symbols[idx];
        ns_str code = ns_ssa_ensure_block_fn(b, idx);
        ns_type t = (sym->type == NS_SYMBOL_BLOCK) ? sym->bc.val.t : sym->fn.fn.t;
        ns_symbol *caps = sym->type == NS_SYMBOL_BLOCK ? sym : ns_null;
        return ns_ssa_emit_callable(b, code, caps, t, i);
    }
    default: {
        ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
        ns_ssa_emit_raw(b, trap);
        return -1;
    }
    }
    ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
    ns_ssa_emit_raw(b, trap);
    return -1;
}

// A phi records where each of its two inputs flows in from so a native backend
// can materialize it with copies on the incoming edges. The source blocks are
// stashed in the otherwise-unused target0/target1 fields (a phi is never a
// terminator): target0 is the predecessor for input a, target1 for input b.
static void ns_ssa_merge_env(ns_ssa_builder *b, ns_ssa_binding *base, ns_ssa_binding *then_env, ns_bool then_reach, ns_ssa_binding *else_env, ns_bool else_reach, i32 ast, i32 then_block, i32 else_block) {
    b->env = ns_ssa_env_clone(base);
    if (!then_reach && !else_reach) return;
    if (then_reach && !else_reach) {
        ns_array_free(b->env);
        b->env = ns_ssa_env_clone(then_env);
        return;
    }
    if (!then_reach && else_reach) {
        ns_array_free(b->env);
        b->env = ns_ssa_env_clone(else_env);
        return;
    }

    for (i32 i = 0, l = (i32)ns_array_length(b->env); i < l; ++i) {
        ns_ssa_binding *binding = &b->env[i];
        i32 v_then = ns_ssa_env_get_value(then_env, binding->name, binding->value);
        i32 v_else = ns_ssa_env_get_value(else_env, binding->name, binding->value);
        if (v_then == v_else) {
            binding->value = v_then;
            continue;
        }

        i32 phi = ns_ssa_emit_value(b, NS_SSA_OP_PHI, v_then, -1, binding->type, binding->name, (ns_token_t){0}, ast);
        ns_ssa_inst *inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        ns_ssa_phi_set_incoming(inst, then_block, v_then);
        ns_ssa_phi_set_incoming(inst, else_block, v_else);
        binding->value = phi;
    }
}

static void ns_ssa_lower_if(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = &b->ctx->nodes[i];
    i32 cond = ns_ssa_lower_expr(b, n->if_stmt.condition);
    i32 entry_block = b->block;
    i32 then_block = ns_ssa_new_block(b, n->if_stmt.body);
    i32 else_block = n->if_stmt.else_body ? ns_ssa_new_block(b, n->if_stmt.else_body) : -1;
    i32 merge_block = ns_ssa_new_block(b, i);
    if (else_block < 0) else_block = merge_block;

    ns_ssa_binding *base = ns_ssa_env_clone(b->env);
    ns_ssa_emit_branch(b, cond, then_block, else_block, i);

    b->block = then_block;
    b->env = ns_ssa_env_clone(base);
    ns_ssa_lower_compound(b, n->if_stmt.body);
    ns_bool then_reach = !b->fn->blocks[b->block].terminated;
    // The block that actually reaches the merge may be a nested block created
    // while lowering the then/else body, not the region's first block.
    i32 then_exit = b->block;
    if (then_reach) ns_ssa_emit_jump(b, merge_block, i);
    ns_ssa_binding *then_env = ns_ssa_env_clone(b->env);

    ns_ssa_binding *else_env = NULL;
    ns_bool else_reach = true;
    // With no else body the false edge flows straight from the condition block.
    i32 else_exit = entry_block;
    if (n->if_stmt.else_body) {
        b->block = else_block;
        b->env = ns_ssa_env_clone(base);
        ns_ssa_lower_compound(b, n->if_stmt.else_body);
        else_reach = !b->fn->blocks[b->block].terminated;
        else_exit = b->block;
        if (else_reach) ns_ssa_emit_jump(b, merge_block, i);
        else_env = ns_ssa_env_clone(b->env);
    } else {
        else_env = ns_ssa_env_clone(base);
    }

    b->block = merge_block;
    b->fn->blocks[merge_block].terminated = false;
    ns_array_free(b->env);
    ns_ssa_merge_env(b, base, then_env, then_reach, else_env, else_reach, i, then_exit, else_exit);

    ns_array_free(base);
    ns_array_free(then_env);
    ns_array_free(else_env);
}

// Seed the loop header with a phi for every live binding: input a is the value
// coming from the preheader, input b the value produced on the back edge. Body
// code reads the phi values, so a variable reassigned in the loop is observed
// with its updated value on the next iteration instead of its pre-loop value.
static i32 *ns_ssa_loop_header_phis(ns_ssa_builder *b, i32 header, i32 preheader, i32 body_block, i32 ast) {
    (void)body_block;
    ns_ssa_binding *pre_env = ns_ssa_env_clone(b->env);
    i32 nbind = (i32)ns_array_length(b->env);
    i32 *phi_inst = ns_null;
    ns_array_set_length(phi_inst, nbind > 0 ? nbind : 0);
    for (i32 k = 0; k < nbind; ++k) {
        ns_ssa_binding *bd = &b->env[k];
        i32 phi = ns_ssa_emit_value(b, NS_SSA_OP_PHI, pre_env[k].value, -1, bd->type, bd->name, (ns_token_t){0}, ast);
        i32 idx = ns_array_last(b->fn->blocks[header].insts)[0];
        ns_ssa_phi_set_incoming(&b->fn->insts[idx], preheader, pre_env[k].value);
        phi_inst[k] = idx;
        bd->value = phi;
    }
    ns_array_free(pre_env);
    return phi_inst;
}

// After the body is lowered, wire each header phi's back-edge input (b) to the
// binding's current value and record the block the back edge leaves from.
static void ns_ssa_loop_close_phis(ns_ssa_builder *b, i32 *phi_inst, i32 body_exit) {
    for (i32 k = 0, l = (i32)ns_array_length(phi_inst); k < l; ++k) {
        ns_ssa_inst *phi = &b->fn->insts[phi_inst[k]];
        i32 value = ns_ssa_env_get_value(b->env, phi->name, phi->dst);
        ns_ssa_phi_set_incoming(phi, body_exit, value);
    }
}

static void ns_ssa_lower_loop(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = &b->ctx->nodes[i];
    i32 preheader = b->block;
    i32 cond_block = ns_ssa_new_block(b, n->loop_stmt.condition);
    i32 body_block = ns_ssa_new_block(b, n->loop_stmt.body);
    i32 exit_block = ns_ssa_new_block(b, i);
    ns_bool do_first = n->loop_stmt.do_first;

    if (do_first) ns_ssa_emit_jump(b, body_block, i);
    else ns_ssa_emit_jump(b, cond_block, i);

    i32 phi_block = do_first ? body_block : cond_block;
    i32 back_hint = do_first ? cond_block : body_block;
    b->block = phi_block;
    b->fn->blocks[phi_block].terminated = false;
    i32 *phi_inst = ns_ssa_loop_header_phis(b, phi_block, preheader, back_hint, i);
    ns_ssa_binding *header_env = ns_ssa_env_clone(b->env);

    ns_ssa_loop_ctx loop = ns_ssa_loop_ctx_init(exit_block, cond_block);
    loop.phi_block = phi_block;
    ns_array_push(b->loops, loop);

    if (!do_first) {
        if (n->loop_stmt.condition > 0) {
            i32 cond = ns_ssa_lower_expr(b, n->loop_stmt.condition);
            ns_ssa_emit_branch(b, cond, body_block, exit_block, i);
        } else {
            ns_ssa_emit_jump(b, body_block, i);
        }
        b->block = body_block;
        b->fn->blocks[body_block].terminated = false;
        ns_array_free(b->env);
        b->env = ns_ssa_env_clone(header_env);
        ns_ssa_lower_compound(b, n->loop_stmt.body);
        if (!b->fn->blocks[b->block].terminated) {
            ns_ssa_loop_close_phis(b, phi_inst, b->block);
            ns_ssa_emit_jump(b, cond_block, i);
        }
        (void)ns_array_pop(b->loops);
        ns_array_free(phi_inst);
        b->block = exit_block;
        b->fn->blocks[b->block].terminated = false;
        ns_array_free(b->env);
        b->env = header_env;
        return;
    }

    ns_ssa_lower_compound(b, n->loop_stmt.body);
    ns_ssa_binding *body_env = ns_ssa_env_clone(b->env);
    if (!b->fn->blocks[b->block].terminated) ns_ssa_emit_jump(b, cond_block, i);

    b->block = cond_block;
    b->fn->blocks[cond_block].terminated = false;
    ns_array_free(b->env);
    b->env = ns_ssa_env_clone(body_env);
    if (n->loop_stmt.condition > 0) {
        i32 cond = ns_ssa_lower_expr(b, n->loop_stmt.condition);
        ns_ssa_emit_branch(b, cond, body_block, exit_block, i);
    } else {
        ns_ssa_emit_jump(b, body_block, i);
    }
    ns_array_free(b->env);
    b->env = body_env;
    ns_ssa_loop_close_phis(b, phi_inst, cond_block);

    (void)ns_array_pop(b->loops);
    ns_array_free(phi_inst);
    ns_array_free(header_env);
    b->block = exit_block;
    b->fn->blocks[b->block].terminated = false;
}

static void ns_ssa_foreach_step(ns_ssa_builder *b, ns_ssa_loop_ctx *loop, i32 ast) {
    if (loop->for_iter.len == 0) return;
    i32 iter = ns_ssa_env_get_value(b->env, loop->for_iter, -1);
    if (iter < 0) return;
    i32 next = ns_ssa_emit_add_i32(b, iter, 1, loop->for_iter, ast);
    ns_ssa_env_bind(b, loop->for_iter, next, ns_type_i32);
}

static void ns_ssa_lower_foreach(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = &b->ctx->nodes[i];
    ns_ast_t *g = &b->ctx->nodes[n->for_stmt.generator];
    i32 subject = ns_ssa_lower_expr(b, g->gen_expr.from);
    ns_type subject_type = ns_ssa_value_type(b, subject);
    ns_ast_t *from_n = ns_ssa_unwrap_expr(b, g->gen_expr.from);
    if (from_n && from_n->type == NS_AST_PRIMARY_EXPR &&
        from_n->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        i32 cur = ns_ssa_env_get_value(b->env, from_n->primary_expr.token.val, subject);
        if (cur >= 0) subject = cur;
    }

    ns_bool is_array = ns_type_is_array(subject_type);
    ns_bool is_string = !is_array && ns_type_is(subject_type, NS_TYPE_STRING);
    ns_bool is_map = !is_array && (ns_type_is(subject_type, NS_TYPE_DICT) || ns_type_is(subject_type, NS_TYPE_SET));
    ns_bool is_struct = !is_array && ns_type_is(subject_type, NS_TYPE_STRUCT);

    ns_str idx_name = ns_str_null;
    if (is_array || is_string || is_map) {
        idx_name = ns_ssa_fresh_name(b, "$fi");
        i32 zero = ns_ssa_emit_i32_const(b, 0, i);
        ns_ssa_env_bind(b, idx_name, zero, ns_type_i32);
    }

    i32 preheader = b->block;
    i32 cond_block = ns_ssa_new_block(b, g->gen_expr.from);
    i32 body_block = ns_ssa_new_block(b, n->for_stmt.body);
    i32 probe_block = is_map ? ns_ssa_new_block(b, i) : -1;
    i32 dead_block = is_map ? ns_ssa_new_block(b, i) : -1;
    i32 exit_block = ns_ssa_new_block(b, i);
    ns_ssa_emit_jump(b, cond_block, i);

    b->block = cond_block;
    b->fn->blocks[cond_block].terminated = false;
    i32 *phi_inst = ns_ssa_loop_header_phis(b, cond_block, preheader, body_block, i);
    ns_ssa_binding *header_env = ns_ssa_env_clone(b->env);
    if (from_n && from_n->type == NS_AST_PRIMARY_EXPR &&
        from_n->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        i32 cur = ns_ssa_env_get_value(b->env, from_n->primary_expr.token.val, subject);
        if (cur >= 0) subject = cur;
    }

    ns_ssa_loop_ctx loop = ns_ssa_loop_ctx_init(exit_block, cond_block);
    loop.for_iter = idx_name;
    loop.foreach_subject = subject;
    loop.foreach_elem = g->gen_expr.name.val;

    if (is_struct) {
        ns_symbol *st = &b->vm->symbols[ns_type_index(subject_type)];
        i32 field = g->gen_expr.rt.value_field;
        if (field < 0) field = ns_struct_field_index(st, ns_str_cstr("value"));
        ns_str next_name = ns_str_cstr("next");
        ns_symbol *next_fn = ns_vm_find_fn_overload(b->vm, next_name, 1, subject_type, true);
        if (next_fn && next_fn->type == NS_SYMBOL_FN) next_name = next_fn->name;
        i32 args[1] = {subject};
        i32 ok = ns_ssa_emit_named_call(b, ns_ssa_cstr(b, next_name), args, 1, ns_type_bool, i);
        ns_ssa_emit_branch(b, ok, body_block, exit_block, i);
        if (field >= 0) loop.foreach_field_off = ns_ssa_wasm32_field_offset(b, st, field);
    } else if (is_map) {
        i32 idx = ns_ssa_env_get_value(b->env, idx_name, -1);
        i32 cap = ns_ssa_emit_value(b, NS_SSA_OP_LOAD, subject, 8, ns_type_i32,
                                    ns_str_null, (ns_token_t){0}, i);
        i32 cond = ns_ssa_emit_lt(b, idx, cap, i);
        ns_ssa_emit_branch(b, cond, probe_block, exit_block, i);
        b->block = probe_block;
        b->fn->blocks[probe_block].terminated = false;
        i32 live_args[2] = {subject, idx};
        i32 live = ns_ssa_emit_named_call(b, "ns_rt_map_slot_live", live_args, 2, ns_type_bool, i);
        ns_ssa_emit_branch(b, live, body_block, dead_block, i);
        b->block = dead_block;
        b->fn->blocks[dead_block].terminated = false;
        ns_array_free(b->env);
        b->env = ns_ssa_env_clone(header_env);
        ns_ssa_foreach_step(b, &loop, i);
        ns_ssa_loop_close_phis(b, phi_inst, b->block);
        ns_ssa_emit_jump(b, cond_block, i);
    } else {
        i32 idx = ns_ssa_env_get_value(b->env, idx_name, -1);
        i32 len = ns_ssa_emit_value(b, NS_SSA_OP_LOAD, subject, 4, ns_type_i32,
                                    ns_str_null, (ns_token_t){0}, i);
        i32 cond = ns_ssa_emit_lt(b, idx, len, i);
        ns_ssa_emit_branch(b, cond, body_block, exit_block, i);
        if (is_array) {
            ns_type elem = subject_type;
            elem.array = false;
            loop.foreach_stride = ns_ssa_wasm32_size(b, elem);
        }
    }

    ns_array_push(b->loops, loop);

    b->block = body_block;
    ns_array_free(b->env);
    b->env = ns_ssa_env_clone(header_env);
    if (from_n && from_n->type == NS_AST_PRIMARY_EXPR &&
        from_n->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        i32 cur = ns_ssa_env_get_value(b->env, from_n->primary_expr.token.val, subject);
        if (cur >= 0) subject = cur;
    }

    ns_type elem_t = ns_type_unknown;
    i32 elem = -1;
    if (is_struct) {
        ns_symbol *st = &b->vm->symbols[ns_type_index(subject_type)];
        i32 field = g->gen_expr.rt.value_field;
        if (field < 0) field = ns_struct_field_index(st, ns_str_cstr("value"));
        if (field >= 0) {
            elem_t = st->st.fields[field].t;
            elem = ns_ssa_emit_value(b, NS_SSA_OP_LOAD, subject,
                                     ns_ssa_wasm32_field_offset(b, st, field),
                                     elem_t, g->gen_expr.name.val, g->gen_expr.name, i);
            b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].target0 =
                ns_ssa_wasm32_size(b, elem_t);
        }
    } else if (is_map) {
        i32 tidx = ns_type_index(subject_type);
        if (tidx >= 0 && tidx < (i32)ns_array_length(b->vm->symbols)) {
            elem_t = b->vm->symbols[tidx].ct.key;
        }
        i32 idx = ns_ssa_env_get_value(b->env, idx_name, -1);
        i32 key_args[2] = {subject, idx};
        elem = ns_ssa_emit_named_call(b, "ns_rt_map_slot_key", key_args, 2, elem_t, i);
    } else if (is_string) {
        elem_t = ns_type_i32;
        i32 idx = ns_ssa_env_get_value(b->env, idx_name, -1);
        elem = ns_ssa_emit_index(b, subject, idx, 1, elem_t, i);
    } else {
        elem_t = subject_type;
        elem_t.array = false;
        i32 stride = ns_ssa_wasm32_size(b, elem_t);
        i32 idx = ns_ssa_env_get_value(b->env, idx_name, -1);
        elem = ns_ssa_emit_index(b, subject, idx, stride, elem_t, i);
    }
    if (elem >= 0) ns_ssa_env_bind(b, g->gen_expr.name.val, elem, elem_t);

    ns_ssa_lower_compound(b, n->for_stmt.body);
    if (!b->fn->blocks[b->block].terminated) {
        ns_ssa_foreach_writeback(b, ns_array_last(b->loops), i);
        ns_ssa_foreach_step(b, ns_array_last(b->loops), i);
        ns_ssa_loop_close_phis(b, phi_inst, b->block);
        ns_ssa_emit_jump(b, cond_block, i);
    }

    (void)ns_array_pop(b->loops);
    ns_array_free(phi_inst);
    b->block = exit_block;
    b->fn->blocks[b->block].terminated = false;
    ns_array_free(b->env);
    b->env = header_env;
}

static void ns_ssa_lower_for(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = &b->ctx->nodes[i];
    ns_ast_t *g = &b->ctx->nodes[n->for_stmt.generator];
    if (!g->gen_expr.range) {
        ns_ssa_lower_foreach(b, i);
        return;
    }

    i32 from = ns_ssa_lower_expr(b, g->gen_expr.from);
    ns_ssa_env_bind(b, g->gen_expr.name.val, from, ns_type_i32);
    i32 preheader = b->block;

    i32 cond_block = ns_ssa_new_block(b, g->gen_expr.to);
    i32 body_block = ns_ssa_new_block(b, n->for_stmt.body);
    i32 exit_block = ns_ssa_new_block(b, i);
    ns_ssa_emit_jump(b, cond_block, i);

    b->block = cond_block;
    b->fn->blocks[cond_block].terminated = false;
    i32 *phi_inst = ns_ssa_loop_header_phis(b, cond_block, preheader, body_block, i);
    ns_ssa_binding *header_env = ns_ssa_env_clone(b->env);

    i32 iter = ns_ssa_env_get_value(b->env, g->gen_expr.name.val, from);
    i32 to = ns_ssa_lower_expr(b, g->gen_expr.to);
    i32 cond = ns_ssa_emit_value(b, NS_SSA_OP_LT, iter, -1, ns_type_bool, ns_str_null, (ns_token_t){0}, i);
    ns_ssa_inst *cond_inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
    cond_inst->b = to;
    ns_ssa_emit_branch(b, cond, body_block, exit_block, i);

    ns_ssa_loop_ctx loop = ns_ssa_loop_ctx_init(exit_block, cond_block);
    loop.for_iter = g->gen_expr.name.val;
    ns_array_push(b->loops, loop);

    b->block = body_block;
    ns_array_free(b->env);
    b->env = ns_ssa_env_clone(header_env);
    ns_ssa_lower_compound(b, n->for_stmt.body);
    if (!b->fn->blocks[b->block].terminated) {
        // The increment is the tail of the back edge: iter = iter + 1.
        iter = ns_ssa_env_get_value(b->env, g->gen_expr.name.val, iter);
        i32 one = ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_i32, ns_str_cstr("1"), (ns_token_t){0}, i);
        i32 next = ns_ssa_emit_value(b, NS_SSA_OP_ADD, iter, -1, ns_type_i32, g->gen_expr.name.val, (ns_token_t){0}, i);
        ns_ssa_inst *next_inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        next_inst->b = one;
        ns_ssa_env_bind(b, g->gen_expr.name.val, next, ns_type_i32);
        ns_ssa_loop_close_phis(b, phi_inst, b->block);
        ns_ssa_emit_jump(b, cond_block, i);
    }

    (void)ns_array_pop(b->loops);
    ns_array_free(phi_inst);
    b->block = exit_block;
    b->fn->blocks[b->block].terminated = false;
    ns_array_free(b->env);
    b->env = header_env;
}

static void ns_ssa_lower_stmt(ns_ssa_builder *b, i32 i) {
    if (i <= 0) return;
    ns_ast_t *n = &b->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_VAR_DEF: {
        i32 value = -1;
        if (n->var_def.expr > 0) {
            value = ns_ssa_lower_expr(b, n->var_def.expr);
            value = ns_ssa_clone_struct(b, value, n->var_def.expr);
        } else {
            value = ns_ssa_emit_value(b, NS_SSA_OP_UNDEF, -1, -1, ns_type_unknown, n->var_def.name.val, n->var_def.name, i);
        }
        i32 global = ns_ssa_global_index(b, n->var_def.name.val);
        ns_type value_type = ns_ssa_value_type(b, value);
        ns_type dest_t = n->var_def.rt;
        if (ns_type_is(dest_t, NS_TYPE_UNION)) {
            value = ns_ssa_union_wrap(b, value, dest_t, i);
            value_type = dest_t;
        }
        if (ns_str_equals(b->fn->name, ns_str_cstr("__module_init")) && global >= 0) {
            if (ns_ssa_needs_box(b, n->var_def.name.val) && !ns_type_is_ref(value_type)) {
                value = ns_ssa_make_ref(b, value, value_type, i);
                value_type = ns_type_set_ref(value_type, true);
                b->m->globals[global].type = value_type;
            }
            ns_ssa_inst store = NS_SSA_INST_INIT(NS_SSA_OP_GLOBAL_SET, i);
            store.a = value;
            store.c = global;
            store.type = b->m->globals[global].type;
            store.name = n->var_def.name.val;
            ns_ssa_emit_raw(b, store);
        } else {
            ns_ssa_maybe_box_name(b, n->var_def.name.val, value, value_type, i);
        }
    } break;
    case NS_AST_ASSERT_STMT: {
        i32 cond = ns_ssa_lower_expr(b, n->assert_stmt.expr);
        ns_ssa_inst inst = NS_SSA_INST_INIT(NS_SSA_OP_ASSERT, i);
        inst.a = cond;
        ns_ssa_emit_raw(b, inst);
    } break;
    case NS_AST_JUMP_STMT: {
        switch (n->jump_stmt.label.type) {
        case NS_TOKEN_RETURN: {
            i32 ret = n->jump_stmt.expr > 0 ? ns_ssa_lower_expr(b, n->jump_stmt.expr) : -1;
            ret = ns_ssa_clone_struct(b, ret, n->jump_stmt.expr);
            if (ret >= 0 && ns_type_is(b->fn->ret, NS_TYPE_UNION)) {
                ret = ns_ssa_union_wrap(b, ret, b->fn->ret, i);
            }
            ns_ssa_emit_ret(b, ret, i);
        } break;
        case NS_TOKEN_BREAK: {
            if (ns_array_length(b->loops) > 0) {
                ns_ssa_loop_ctx *loop = ns_array_last(b->loops);
                ns_ssa_foreach_writeback(b, loop, i);
                ns_ssa_loop_flush_phis(b, loop->phi_block);
                ns_ssa_emit_jump(b, loop->break_block, i);
            } else {
                ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
                ns_ssa_emit_raw(b, trap);
            }
        } break;
        case NS_TOKEN_CONTINUE: {
            if (ns_array_length(b->loops) > 0) {
                ns_ssa_loop_ctx *loop = ns_array_last(b->loops);
                ns_ssa_foreach_writeback(b, loop, i);
                ns_ssa_foreach_step(b, loop, i);
                ns_ssa_block *hb = &b->fn->blocks[loop->continue_block];
                for (i32 hi = 0, hl = (i32)ns_array_length(hb->insts); hi < hl; ++hi) {
                    ns_ssa_inst *phi = &b->fn->insts[hb->insts[hi]];
                    if (phi->op != NS_SSA_OP_PHI) break;
                    i32 src = ns_ssa_env_get_value(b->env, phi->name, -1);
                    if (src >= 0) ns_ssa_phi_set_incoming(phi, b->block, src);
                }
                ns_ssa_emit_jump(b, loop->continue_block, i);
            } else {
                ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
                ns_ssa_emit_raw(b, trap);
            }
        } break;
        default: {
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            ns_ssa_emit_raw(b, trap);
        } break;
        }
    } break;
    case NS_AST_USE_STMT:
    case NS_AST_MODULE_STMT:
    case NS_AST_TYPE_DEF:
    case NS_AST_STRUCT_DEF:
    case NS_AST_ENUM_DEF:
    case NS_AST_FN_DEF:
    case NS_AST_OP_FN_DEF:
        break;
    case NS_AST_IF_STMT:
        ns_ssa_lower_if(b, i);
        break;
    case NS_AST_FOR_STMT:
        ns_ssa_lower_for(b, i);
        break;
    case NS_AST_LOOP_STMT:
        ns_ssa_lower_loop(b, i);
        break;
    case NS_AST_COMPOUND_STMT:
        ns_ssa_lower_compound(b, i);
        break;
    default:
        ns_ssa_lower_expr(b, i);
        break;
    }
}

static void ns_ssa_lower_compound(ns_ssa_builder *b, i32 i) {
    if (i <= 0) return;
    ns_ast_t *n = &b->ctx->nodes[i];
    if (n->type != NS_AST_COMPOUND_STMT) {
        ns_ssa_lower_stmt(b, i);
        return;
    }

    i32 stmt = n->next;
    for (i32 c = 0; c < n->compound_stmt.count && stmt > 0; ++c) {
        if (b->fn->blocks[b->block].terminated) break;
        ns_ssa_lower_stmt(b, stmt);
        stmt = b->ctx->nodes[stmt].next;
    }
}

static ns_str ns_ssa_ensure_block_fn(ns_ssa_builder *b, i32 symbol_index) {
    ns_str name = ns_ssa_owned_fmt(b, "$b%d", symbol_index);
    if (ns_ssa_fn_exists(b, name)) return name;
    ns_symbol *sym = &b->vm->symbols[symbol_index];
    ns_fn_symbol *fn = ns_symbol_get_fn(sym);
    if (!fn) return name;

    i32 saved_fn = (i32)(b->fn ? (b->fn - b->m->fns) : -1);
    i32 saved_block = b->block;
    i32 saved_next = b->next_value;
    ns_ssa_binding *saved_env = b->env;
    ns_ssa_loop_ctx *saved_loops = b->loops;
    i32 saved_cap_env = b->capture_env;
    ns_ssa_capture *saved_caps = b->captures;

    ns_ssa_fn sfn = {0};
    sfn.name = name;
    sfn.ast = fn->body;
    sfn.entry = 0;
    sfn.ret = ns_enum_underlying_type(b->vm, fn->ret);
    if (ns_type_is(sfn.ret, NS_TYPE_INFER) || ns_type_is(sfn.ret, NS_TYPE_UNKNOWN)) sfn.ret = ns_type_void;
    ns_array_push(b->m->fns, sfn);
    b->fn = &b->m->fns[ns_array_length(b->m->fns) - 1];
    b->block = ns_ssa_new_block(b, fn->body);
    b->next_value = 0;
    b->env = NULL;
    b->loops = NULL;
    b->captures = NULL;
    b->capture_env = -1;

    ns_str env_name = ns_str_cstr("$env");
    ns_array_push(b->fn->params, ns_type_i64);
    i32 env = ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, 0, ns_type_i64, env_name, (ns_token_t){0}, fn->body);
    ns_ssa_env_bind(b, env_name, env, ns_type_i64);
    b->capture_env = env;

    i32 nargs = (i32)ns_array_length(fn->args);
    for (i32 ai = 0; ai < nargs; ++ai) {
        ns_type at = ns_enum_underlying_type(b->vm, fn->args[ai].val.t);
        ns_array_push(b->fn->params, at);
        i32 v = ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, ai + 1, at, fn->args[ai].name,
                                  (ns_token_t){0}, fn->body);
        ns_ssa_env_bind(b, fn->args[ai].name, v, at);
    }

    if (sym->type == NS_SYMBOL_BLOCK) {
        for (i32 fi = 0, fl = (i32)ns_array_length(sym->bc.st.fields); fi < fl; ++fi) {
            ns_struct_field *f = &sym->bc.st.fields[fi];
            i32 off = ns_ssa_block_field_off(b, sym, fi);
            ns_type cap_t = f->t;
            if (ns_ssa_needs_box(b, f->name)) cap_t = ns_type_set_ref(f->t, true);
            i32 v = ns_ssa_emit_value(b, NS_SSA_OP_LOAD, env, off, cap_t, f->name,
                                      (ns_token_t){0}, fn->body);
            b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]].target0 =
                ns_ssa_wasm32_size(b, cap_t);
            ns_ssa_env_bind(b, f->name, v, cap_t);
            ns_ssa_capture cap = {.name = f->name, .offset = off, .type = cap_t};
            ns_array_push(b->captures, cap);
        }
    }

    ns_ssa_seed_global_consts(b);
    ns_ssa_lower_compound(b, fn->body);
    if (!b->fn->blocks[b->block].terminated) ns_ssa_emit_ret(b, -1, fn->body);

    ns_array_free(b->env);
    ns_array_free(b->loops);
    ns_array_free(b->captures);

    b->env = saved_env;
    b->loops = saved_loops;
    b->captures = saved_caps;
    b->capture_env = saved_cap_env;
    b->next_value = saved_next;
    b->block = saved_block;
    b->fn = saved_fn >= 0 ? &b->m->fns[saved_fn] : ns_null;
    return name;
}

static i32 ns_ssa_named_fn_value(ns_ssa_builder *b, ns_symbol *sym, i32 ast) {
    ns_str wrap = ns_ssa_owned_prefix(b, "$v", sym->name);
    if (!ns_ssa_fn_exists(b, wrap)) {
        i32 saved_fn = (i32)(b->fn ? (b->fn - b->m->fns) : -1);
        i32 saved_block = b->block;
        i32 saved_next = b->next_value;
        ns_ssa_binding *saved_env = b->env;
        ns_ssa_loop_ctx *saved_loops = b->loops;
        i32 saved_cap_env = b->capture_env;
        ns_ssa_capture *saved_caps = b->captures;

        ns_ssa_fn sfn = {0};
        sfn.name = wrap;
        sfn.ast = ast;
        sfn.ret = ns_enum_underlying_type(b->vm, sym->fn.ret);
        if (ns_type_is(sfn.ret, NS_TYPE_INFER) || ns_type_is(sfn.ret, NS_TYPE_UNKNOWN)) sfn.ret = ns_type_void;
        ns_array_push(b->m->fns, sfn);
        b->fn = &b->m->fns[ns_array_length(b->m->fns) - 1];
        b->block = ns_ssa_new_block(b, ast);
        b->next_value = 0;
        b->env = NULL;
        b->loops = NULL;
        b->captures = NULL;
        b->capture_env = -1;

        ns_array_push(b->fn->params, ns_type_i64);
        (void)ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, 0, ns_type_i64, ns_str_cstr("$env"),
                                (ns_token_t){0}, ast);
        i32 nargs = (i32)ns_array_length(sym->fn.args);
        i32 *avals = ns_null;
        for (i32 ai = 0; ai < nargs; ++ai) {
            ns_type at = ns_enum_underlying_type(b->vm, sym->fn.args[ai].val.t);
            ns_array_push(b->fn->params, at);
            i32 v = ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, ai + 1, at, sym->fn.args[ai].name,
                                      (ns_token_t){0}, ast);
            ns_array_push(avals, v);
        }
        for (i32 ai = 0; ai < nargs; ++ai) {
            ns_ssa_inst arg = NS_SSA_INST_INIT(NS_SSA_OP_ARG, ast);
            arg.a = avals[ai];
            ns_ssa_emit_raw(b, arg);
        }
        i32 ret = ns_ssa_emit_value(b, NS_SSA_OP_CALL, -1, nargs, sfn.ret, sym->name,
                                    (ns_token_t){0}, ast);
        ns_ssa_emit_ret(b, ret, ast);
        ns_array_free(avals);
        ns_array_free(b->env);
        ns_array_free(b->loops);

        b->env = saved_env;
        b->loops = saved_loops;
        b->captures = saved_caps;
        b->capture_env = saved_cap_env;
        b->next_value = saved_next;
        b->block = saved_block;
        b->fn = saved_fn >= 0 ? &b->m->fns[saved_fn] : ns_null;
    }
    return ns_ssa_emit_callable(b, wrap, ns_null, sym->fn.fn.t, ast);
}

static void ns_ssa_lower_fn(ns_ssa_builder *b, i32 ast, ns_str name, i32 body, i32 arg_head, i32 arg_count) {
    ns_ssa_fn fn = {0};
    fn.name = name;
    fn.ast = ast;
    fn.entry = 0;
    ns_symbol *fn_symbol = ns_vm_find_symbol(b->vm, name, false);
    fn.ret = fn_symbol && fn_symbol->type == NS_SYMBOL_FN
        ? ns_enum_underlying_type(b->vm, fn_symbol->fn.ret) : ns_type_void;
    ns_ast_t *definition = &b->ctx->nodes[ast];
    i32 ret_ast = definition->type == NS_AST_FN_DEF ? definition->fn_def.ret :
                  definition->type == NS_AST_OP_FN_DEF ? definition->ops_fn_def.ret : 0;
    ns_type declared_ret = ns_ssa_type_from_label(b, ret_ast);
    if (!ns_type_is(declared_ret, NS_TYPE_UNKNOWN)) fn.ret = declared_ret;
    if (ns_type_is(fn.ret, NS_TYPE_INFER) || ns_type_is(fn.ret, NS_TYPE_UNKNOWN)) fn.ret = ns_type_void;
    ns_array_push(b->m->fns, fn);
    b->fn = &b->m->fns[ns_array_length(b->m->fns) - 1];
    b->block = ns_ssa_new_block(b, ast);
    b->next_value = 0;

    ns_array_free(b->env);
    b->env = NULL;
    b->loops = NULL;
    b->captures = NULL;
    b->capture_env = -1;
    i32 arg = arg_head;
    for (i32 ai = 0; ai < arg_count && arg > 0; ++ai) {
        ns_ast_t *an = &b->ctx->nodes[arg];
        ns_type arg_type = fn_symbol && fn_symbol->type == NS_SYMBOL_FN &&
                           ai < (i32)ns_array_length(fn_symbol->fn.args)
            ? ns_enum_underlying_type(b->vm, fn_symbol->fn.args[ai].val.t)
            : ns_ssa_type_from_label(b, an->arg.type);
        if (ns_type_is(arg_type, NS_TYPE_UNKNOWN) && an->arg.type > 0) {
            ns_ast_t *tl = &b->ctx->nodes[an->arg.type];
            if (tl->type == NS_AST_TYPE_LABEL) {
                ns_symbol *st = ns_vm_find_symbol(b->vm, tl->type_label.name.val, false);
                if (st && st->type == NS_SYMBOL_STRUCT) arg_type = st->st.st.t;
            }
        }
        ns_array_push(b->fn->params, arg_type);
        i32 v = ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, ai, arg_type, an->arg.name.val, an->arg.name, arg);
        ns_ssa_maybe_box_name(b, an->arg.name.val, v, arg_type, arg);
        if (definition->type == NS_AST_OP_FN_DEF && ai == 0) arg = definition->ops_fn_def.right;
        else arg = an->next;
    }
    // Parameters must occupy Wasm locals 0..n-1. Seed module literals only
    // after parameter values have been allocated.
    ns_ssa_seed_global_consts(b);

    ns_ssa_lower_compound(b, body);
    if (!b->fn->blocks[b->block].terminated) {
        ns_ssa_emit_ret(b, -1, ast);
    }

    ns_array_free(b->env);
    ns_array_free(b->loops);
    b->env = NULL;
    b->loops = NULL;
}

static ns_bool ns_ssa_embed_module(ns_str lib) {
    return ns_str_equals(lib, ns_str_cstr("simd")) ||
           ns_str_equals(lib, ns_str_cstr("std")) ||
           ns_str_equals(lib, ns_str_cstr("task"));
}

static ns_bool ns_ssa_skip_imported_fn(ns_str name) {
    // These wrappers call shader_transpile with `any` fn values, which the
    // native backend cannot fold. Call sites should use shader_transpile_stage
    // plus gpu_shader_*_create instead.
    return ns_str_equals_STR(name, "gpu_shader_graphics") ||
           ns_str_equals_STR(name, "gpu_shader_compute");
}

static void ns_ssa_lower_imported_fns(ns_ssa_builder *b) {
    ns_ast_ctx *saved = b->ctx;
    i32 nsymbols = (i32)ns_array_length(b->vm->symbols);
    for (i32 i = 0; i < nsymbols; ++i) {
        ns_symbol *sym = &b->vm->symbols[i];
        if (sym->type != NS_SYMBOL_FN) continue;
        if (sym->fn.fn.t.ref) continue;
        if (sym->fn.body <= 0 || sym->fn.ast <= 0) continue;
        if (!ns_ssa_embed_module(sym->lib)) continue;
        if (ns_ssa_shader_name(sym->name)) continue;
        if (ns_ssa_skip_imported_fn(sym->name)) continue;
        if (ns_ssa_fn_exists(b, sym->name)) continue;

        ns_ast_ctx *fn_ctx = sym->fn.ctx ? sym->fn.ctx : saved;
        if (!fn_ctx || sym->fn.ast >= (i32)ns_array_length(fn_ctx->nodes)) continue;
        ns_ast_t *n = &fn_ctx->nodes[sym->fn.ast];
        b->ctx = fn_ctx;
        ns_ssa_collect_ref_in(b, sym->fn.body);
        if (n->type == NS_AST_FN_DEF) {
            ns_ssa_lower_fn(b, sym->fn.ast, sym->name, n->fn_def.body, n->next, n->fn_def.arg_count);
        } else if (n->type == NS_AST_OP_FN_DEF) {
            ns_ssa_lower_fn(b, sym->fn.ast, sym->name, n->ops_fn_def.body, n->ops_fn_def.left, 2);
        }
        if (ns_return_is_error(b->fold_error)) break;
    }
    b->ctx = saved;
}

ns_return_ptr ns_ssa_build_with_runtime_paths(ns_ast_ctx *ctx, ns_str ref_path,
                                              ns_str lib_path, ns_str lib_fallback_path) {
    if (ctx == NULL || ns_array_length(ctx->nodes) == 0) {
        return ns_return_error(ptr, ns_code_loc_nil, NS_ERR_SYNTAX, "ast ctx is empty");
    }

    ns_ssa_module *m = ns_malloc(sizeof(ns_ssa_module));
    memset(m, 0, sizeof(*m));
    m->ctx = ctx;

    ns_ssa_builder b = {0};
    b.ctx = ctx;
    b.m = m;
    b.capture_env = -1;

    // The VM semantic pass is shared by interpretation and compilation. It
    // resolves enum values once, enforces nominal typing, and gives SSA the
    // underlying integer representation used by every native backend.
    ns_vm semantic_vm = {0};
    ns_vm_set_ref_path(&semantic_vm, ref_path);
    ns_vm_set_lib_path(&semantic_vm, lib_path);
    ns_vm_set_lib_fallback_path(&semantic_vm, lib_fallback_path);
    ns_return_bool semantic = ns_vm_parse(&semantic_vm, ctx);
    if (ns_return_is_error(semantic)) {
        ns_ssa_module_free(m);
        return ns_return_change_type(ptr, semantic);
    }
    b.vm = &semantic_vm;

    // Evaluate only declared literals. Their semantic checker excludes calls
    // and mutable data, so this has no runtime side effects and turns constant
    // expressions into one canonical value for all native backends.
    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s];
        if (n->type != NS_AST_VAR_DEF || !n->var_def.is_lit) continue;
        ns_return_value evaluated = ns_eval_var_def(&semantic_vm, ctx, s);
        if (ns_return_is_error(evaluated)) {
            ns_ssa_module_free(m);
            return ns_return_change_type(ptr, evaluated);
        }
    }

    ns_ssa_collect_module_consts(&b, ctx);
    ns_ssa_collect_module_globals(&b);
    ns_ssa_collect_shader_only_fns(&b);
    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        ns_ssa_collect_ref_in(&b, ctx->sections[i]);
    }
    ns_return_bool shaders = ns_ssa_collect_shaders(&b);
    if (ns_return_is_error(shaders)) {
        ns_ssa_module_free(m);
        return ns_return_change_type(ptr, shaders);
    }

    ns_bool has_init = false;
    i32 init_ast = 0;
    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s];
        switch (n->type) {
        case NS_AST_FN_DEF:
            if (ns_ssa_name_listed(b.shader_only, n->fn_def.name.val)) break;
            ns_ssa_lower_fn(&b, s, ns_ssa_fn_symbol_name(&b, s, n->fn_def.name.val),
                            n->fn_def.body, n->next, n->fn_def.arg_count);
            break;
        case NS_AST_OP_FN_DEF:
            ns_ssa_lower_fn(&b, s, ns_ssa_ops_fn_name(&b, n), n->ops_fn_def.body, n->ops_fn_def.left, 2);
            break;
        default:
            if (!has_init) {
                init_ast = s;
                has_init = true;
            }
            break;
        }
    }

    if (has_init) {
        ns_ssa_fn fn = {0};
        fn.name = ns_str_cstr("__module_init");
        fn.ast = init_ast;
        fn.entry = 0;
        fn.ret = ns_type_void;
        ns_array_push(m->fns, fn);
        b.fn = &m->fns[ns_array_length(m->fns) - 1];
        b.block = ns_ssa_new_block(&b, init_ast);
        b.next_value = 0;
        b.env = NULL;
        b.loops = NULL;
        b.captures = NULL;
        b.capture_env = -1;
        ns_ssa_seed_global_consts(&b);

        for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
            i32 s = ctx->sections[i];
            ns_ast_t *n = &ctx->nodes[s];
            if (n->type == NS_AST_FN_DEF || n->type == NS_AST_OP_FN_DEF ||
                (n->type == NS_AST_VAR_DEF && n->var_def.is_lit)) continue;
            if (b.fn->blocks[b.block].terminated) break;
            ns_ssa_lower_stmt(&b, s);
        }

        if (!b.fn->blocks[b.block].terminated) {
            ns_ssa_emit_ret(&b, -1, init_ast);
        }
        ns_array_free(b.env);
        ns_array_free(b.loops);
    }

    if (ns_return_is_error(b.fold_error)) {
        ns_ssa_module_free(m);
        return ns_return_change_type(ptr, b.fold_error);
    }

    ns_ssa_lower_imported_fns(&b);
    if (ns_return_is_error(b.fold_error)) {
        ns_ssa_module_free(m);
        return ns_return_change_type(ptr, b.fold_error);
    }

    ns_array_free(b.globals);
    ns_array_free(b.import_seen);
    ns_array_free(b.ref_names);
    ns_array_free(b.shader_only);

    return ns_return_ok(ptr, m);
}

ns_return_ptr ns_ssa_build(ns_ast_ctx *ctx) {
    return ns_ssa_build_with_runtime_paths(ctx, ns_str_null, ns_str_null, ns_str_null);
}

void ns_ssa_module_free(ns_ssa_module *m) {
    if (!m) return;
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_ssa_fn *fn = &m->fns[i];
        for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; ++bi) {
            ns_array_free(fn->blocks[bi].insts);
            ns_array_free(fn->blocks[bi].preds);
            ns_array_free(fn->blocks[bi].succs);
        }
        ns_array_free(fn->blocks);
        for (i32 ii = 0, il = (i32)ns_array_length(fn->insts); ii < il; ++ii) {
            ns_array_free(fn->insts[ii].phi_edges);
        }
        ns_array_free(fn->insts);
        ns_array_free(fn->params);
    }
    ns_array_free(m->fns);
    for (i32 i = 0, l = (i32)ns_array_length(m->imports); i < l; ++i) {
        ns_array_free(m->imports[i].params);
    }
    ns_array_free(m->imports);
    ns_array_free(m->globals);
    for (i32 i = 0, l = (i32)ns_array_length(m->shaders); i < l; ++i) {
        ns_array_free(m->shaders[i].wgsl.data);
        ns_array_free(m->shaders[i].vertex_offsets);
        ns_array_free(m->shaders[i].vertex_sizes);
    }
    ns_array_free(m->shaders);
    for (i32 i = 0, l = (i32)ns_array_length(m->owned_strings); i < l; ++i) {
        ns_free(m->owned_strings[i].data);
    }
    ns_array_free(m->owned_strings);
    ns_free(m);
}

static void ns_ssa_print_value(i32 v) {
    if (v >= 0) {
        printf(ns_color_wrn "n%d" ns_color_nil, v);
    } else {
        printf(ns_color_log "void" ns_color_nil);
    }
}

static ns_str ns_ssa_type_to_str(ns_type t) {
    switch (t.type) {
    case NS_TYPE_I8: return ns_str_cstr("i8");
    case NS_TYPE_I16: return ns_str_cstr("i16");
    case NS_TYPE_I32: return ns_str_cstr("i32");
    case NS_TYPE_I64: return ns_str_cstr("i64");
    case NS_TYPE_U8: return ns_str_cstr("u8");
    case NS_TYPE_U16: return ns_str_cstr("u16");
    case NS_TYPE_U32: return ns_str_cstr("u32");
    case NS_TYPE_U64: return ns_str_cstr("u64");
    case NS_TYPE_F32: return ns_str_cstr("f32");
    case NS_TYPE_F64: return ns_str_cstr("f64");
    case NS_TYPE_BOOL: return ns_str_cstr("bool");
    case NS_TYPE_TYPE: return ns_str_cstr("type");
    case NS_TYPE_STRING: return ns_str_cstr("str");
    case NS_TYPE_VOID: return ns_str_cstr("void");
    default: return ns_str_cstr("unknown");
    }
}

static ns_bool ns_ssa_is_binary_op(ns_ssa_op op) {
    switch (op) {
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB:
    case NS_SSA_OP_MUL:
    case NS_SSA_OP_DIV:
    case NS_SSA_OP_MOD:
    case NS_SSA_OP_SHL:
    case NS_SSA_OP_SHR:
    case NS_SSA_OP_BAND:
    case NS_SSA_OP_BOR:
    case NS_SSA_OP_BXOR:
    case NS_SSA_OP_AND:
    case NS_SSA_OP_OR:
    case NS_SSA_OP_EQ:
    case NS_SSA_OP_NE:
    case NS_SSA_OP_LT:
    case NS_SSA_OP_LE:
    case NS_SSA_OP_GT:
    case NS_SSA_OP_GE:
        return true;
    default:
        return false;
    }
}

static void ns_ssa_print_inst_detail(ns_ssa_inst *inst) {
    switch (inst->op) {
    case NS_SSA_OP_PARAM:
        if (inst->c >= 0) {
            printf(ns_color_ign "name=" ns_color_nil "%.*s " ns_color_ign "index=" ns_color_nil "%d", inst->name.len, inst->name.data, inst->c);
        } else {
            printf(ns_color_ign "symbol=" ns_color_nil "%.*s", inst->name.len, inst->name.data);
        }
        break;
    case NS_SSA_OP_CONST:
        printf(ns_color_ign "literal=" ns_color_nil "%.*s", inst->name.len, inst->name.data);
        break;
    case NS_SSA_OP_COPY:
        printf(ns_color_ign "src=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        if (inst->name.len > 0) {
            printf(" " ns_color_ign "name=" ns_color_nil "%.*s", inst->name.len, inst->name.data);
        }
        break;
    case NS_SSA_OP_PHI:
        printf(ns_color_ign "incoming0=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        printf(" " ns_color_ign "incoming1=" ns_color_nil);
        ns_ssa_print_value(inst->b);
        if (inst->name.len > 0) {
            printf(" " ns_color_ign "name=" ns_color_nil "%.*s", inst->name.len, inst->name.data);
        }
        break;
    case NS_SSA_OP_CAST:
        printf(ns_color_ign "value=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        break;
    case NS_SSA_OP_CALL:
        printf(ns_color_ign "callee=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        printf(" " ns_color_ign "arg_count=" ns_color_nil "%d", inst->c);
        break;
    case NS_SSA_OP_ARG:
        printf(ns_color_ign "value=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        break;
    case NS_SSA_OP_MEMBER:
        printf(ns_color_ign "object=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        printf(" " ns_color_ign "field=" ns_color_nil);
        ns_ssa_print_value(inst->b);
        break;
    case NS_SSA_OP_INDEX:
        printf(ns_color_ign "table=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        printf(" " ns_color_ign "index=" ns_color_nil);
        ns_ssa_print_value(inst->b);
        break;
    case NS_SSA_OP_NEG:
    case NS_SSA_OP_NOT:
    case NS_SSA_OP_ASSERT:
    case NS_SSA_OP_RET:
        printf(ns_color_ign "value=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        break;
    case NS_SSA_OP_BR:
        printf(ns_color_ign "cond=" ns_color_nil);
        ns_ssa_print_value(inst->a);
        printf(" " ns_color_ign "then=" ns_color_nil ns_color_cmt "b%d" ns_color_nil, inst->target0);
        printf(" " ns_color_ign "else=" ns_color_nil ns_color_cmt "b%d" ns_color_nil, inst->target1);
        break;
    case NS_SSA_OP_JMP:
        printf(ns_color_ign "target=" ns_color_nil ns_color_cmt "b%d" ns_color_nil, inst->target0);
        break;
    case NS_SSA_OP_TRAP:
        if (inst->a >= 0 || inst->b >= 0) {
            printf(ns_color_ign "a=" ns_color_nil);
            ns_ssa_print_value(inst->a);
            printf(" " ns_color_ign "b=" ns_color_nil);
            ns_ssa_print_value(inst->b);
        }
        break;
    default:
        if (ns_ssa_is_binary_op(inst->op)) {
            printf(ns_color_ign "lhs=" ns_color_nil);
            ns_ssa_print_value(inst->a);
            printf(" " ns_color_ign "rhs=" ns_color_nil);
            ns_ssa_print_value(inst->b);
        } else if (inst->a >= 0 || inst->b >= 0 || inst->c >= 0) {
            printf(ns_color_ign "a=" ns_color_nil "%d " ns_color_ign "b=" ns_color_nil "%d " ns_color_ign "c=" ns_color_nil "%d", inst->a, inst->b, inst->c);
        }
        break;
    }
}

void ns_ssa_print(ns_ssa_module *m) {
    if (!m) return;
    for (i32 fi = 0, fl = (i32)ns_array_length(m->fns); fi < fl; ++fi) {
        ns_ssa_fn *fn = &m->fns[fi];
        printf(ns_color_log "fn " ns_color_nil "%.*s " ns_color_ign "(ast=%d)" ns_color_nil "\n", fn->name.len, fn->name.data, fn->ast);
        for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; ++bi) {
            ns_ssa_block *bb = &fn->blocks[bi];
            printf("  " ns_color_cmt "b%d:" ns_color_nil "\n", bb->id);
            for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
                ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
                ns_str op = ns_ssa_op_to_string(inst->op);
                printf("    ");
                if (inst->dst >= 0) printf(ns_color_wrn "n%d" ns_color_nil " = ", inst->dst);
                printf(ns_color_log "%.*s" ns_color_nil " ", op.len, op.data);
                ns_ssa_print_inst_detail(inst);
                if (inst->op == NS_SSA_OP_PARAM) {
                    ns_str type_name = ns_ssa_type_to_str(inst->type);
                    printf(" " ns_color_ign "type=" ns_color_nil "%.*s", type_name.len, type_name.data);
                }
                printf(" " ns_color_ign "(ast=%d)" ns_color_nil "\n", inst->ast);
            }
        }
    }
}
