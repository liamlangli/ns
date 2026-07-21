#include "ns_ssa.h"
#include "ns_os.h"
#include "ns_vm.h"

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
} ns_ssa_loop_ctx;

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
    .token = (ns_token_t){0}, \
})

static i32 ns_ssa_lower_expr(ns_ssa_builder *b, i32 i);
static void ns_ssa_lower_stmt(ns_ssa_builder *b, i32 i);
static void ns_ssa_lower_compound(ns_ssa_builder *b, i32 i);

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

static void ns_ssa_emit_ret(ns_ssa_builder *b, i32 value, i32 ast) {
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

static ns_bool ns_ssa_const_from_value(ns_ssa_builder *b, ns_value value, ns_token_t *token, ns_type *type) {
    if (ns_type_is(value.t, NS_TYPE_ENUM)) value = ns_eval_enum_underlying_value(b->vm, value);
    char text[96];
    i32 len = 0;
    token->type = NS_TOKEN_INT_LITERAL;
    switch (value.t.type) {
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

// Preserve the compiler's historical treatment of a top-level `let` whose
// initializer is a single integer/bool literal. New constant expressions
// should use `lit`; this compatibility path intentionally does not grow.
static ns_bool ns_ssa_legacy_let_const(ns_ast_ctx *ctx, i32 expr, ns_token_t *token, ns_type *type) {
    if (expr <= 0) return false;
    ns_ast_t *n = &ctx->nodes[expr];
    if (n->type == NS_AST_EXPR) return ns_ssa_legacy_let_const(ctx, n->expr.body, token, type);
    if (n->type != NS_AST_PRIMARY_EXPR) return false;
    ns_token_t t = n->primary_expr.token;
    if (t.type == NS_TOKEN_INT_LITERAL) {
        *token = t;
        *type = ns_type_i32;
        return true;
    }
    if (t.type == NS_TOKEN_TRUE || t.type == NS_TOKEN_FALSE) {
        *token = (ns_token_t){.type = NS_TOKEN_INT_LITERAL,
                              .val = t.type == NS_TOKEN_TRUE ? ns_str_cstr("1") : ns_str_cstr("0")};
        *type = ns_type_bool;
        return true;
    }
    return false;
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

#ifdef NS_DEBUG
    ns_str ref_path = ns_str_cstr(NS_SSA_REF_PATH);
#else
    ns_str home = ns_path_home();
    ns_str ref_path = ns_path_join(home, ns_str_cstr(NS_SSA_REF_PATH));
#endif

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
        } else if (!ns_ssa_legacy_let_const(ctx, n->var_def.expr, &token, &type)) {
            continue;
        }

        ns_ssa_global_const global = {.name = n->var_def.name.val, .token = token, .type = type};
        ns_array_push(b->globals, global);
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
    if (token.type == NS_TOKEN_IDENTIFIER) {
        i32 idx = ns_ssa_env_find(b->env, token.val);
        if (idx >= 0) return b->env[idx].value;
        return ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, -1, ns_type_unknown, token.val, token, i);
    }
    return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_unknown, token.val, token, i);
}

static i32 ns_ssa_lower_assign(ns_ssa_builder *b, ns_ast_t *n, i32 i) {
    i32 rhs = ns_ssa_lower_expr(b, n->binary_expr.right);
    i32 left = n->binary_expr.left;
    ns_ast_t *ln = &b->ctx->nodes[left];
    if (ln->type == NS_AST_EXPR) {
        ln = &b->ctx->nodes[ln->expr.body];
    }

    if (ln->type == NS_AST_PRIMARY_EXPR && ln->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_COPY, rhs, -1, ns_type_unknown, ln->primary_expr.token.val, ln->primary_expr.token, i);
        ns_ssa_env_bind(b, ln->primary_expr.token.val, dst, ns_type_unknown);
        return dst;
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
        i32 rhs = ns_ssa_lower_expr(b, n->binary_expr.right);
        ns_ssa_op op = ns_ssa_binary_op(n->binary_expr.op);
        if (op == NS_SSA_OP_UNKNOWN) {
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            trap.a = lhs;
            trap.b = rhs;
            ns_ssa_emit_raw(b, trap);
            return -1;
        }
        i32 dst = ns_ssa_emit_value(b, op, lhs, -1, ns_type_unknown, ns_str_null, n->binary_expr.op, i);
        ns_ssa_inst *inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        inst->b = rhs;
        return dst;
    }
    case NS_AST_UNARY_EXPR: {
        i32 v = ns_ssa_lower_expr(b, n->unary_expr.expr);
        ns_ssa_op op = NS_SSA_OP_UNKNOWN;
        if (ns_str_equals_STR(n->unary_expr.op.val, "-")) op = NS_SSA_OP_NEG;
        if (ns_str_equals_STR(n->unary_expr.op.val, "!")) op = NS_SSA_OP_NOT;
        if (op == NS_SSA_OP_UNKNOWN) {
            ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
            trap.a = v;
            ns_ssa_emit_raw(b, trap);
            return -1;
        }
        return ns_ssa_emit_value(b, op, v, -1, ns_type_unknown, ns_str_null, n->unary_expr.op, i);
    }
    case NS_AST_CALL_EXPR: {
        ns_type result_type = ns_type_unknown;
        ns_ast_t *callee_node = ns_ssa_unwrap_expr(b, n->call_expr.callee);
        if (callee_node && callee_node->type == NS_AST_PRIMARY_EXPR &&
            callee_node->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
            ns_symbol *symbol = ns_vm_find_symbol(b->vm, callee_node->primary_expr.token.val, false);
            if (symbol && symbol->type == NS_SYMBOL_FN) {
                result_type = ns_enum_underlying_type(b->vm, symbol->fn.ret);
            }
        }
        i32 callee = ns_ssa_lower_expr(b, n->call_expr.callee);
        i32 next = n->next;
        for (i32 ai = 0; ai < n->call_expr.arg_count && next > 0; ++ai) {
            i32 av = ns_ssa_lower_expr(b, next);
            ns_ssa_inst arg = NS_SSA_INST_INIT(NS_SSA_OP_ARG, next);
            arg.a = av;
            ns_ssa_emit_raw(b, arg);
            next = b->ctx->nodes[next].next;
        }
        return ns_ssa_emit_value(b, NS_SSA_OP_CALL, callee, n->call_expr.arg_count,
                                 result_type, ns_str_null, (ns_token_t){0}, i);
    }
    case NS_AST_CAST_EXPR: {
        i32 src = ns_ssa_lower_expr(b, n->cast_expr.expr);
        ns_return_type parsed = ns_vm_parse_type_by_token(b->vm, n->cast_expr.type,
                                                          ns_ast_state_loc(b->ctx, n->state));
        ns_type dst = ns_return_is_error(parsed) ? ns_type_unknown : parsed.r;
        dst = ns_enum_underlying_type(b->vm, dst);
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
        i32 field = ns_ssa_lower_expr(b, n->member_expr.right);
        i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_MEMBER, obj, -1, ns_type_unknown, ns_str_null, (ns_token_t){0}, i);
        ns_ssa_inst *inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        inst->b = field;
        return dst;
    }
    case NS_AST_INDEX_EXPR: {
        i32 table = ns_ssa_lower_expr(b, n->index_expr.table);
        i32 idx = ns_ssa_lower_expr(b, n->index_expr.expr);
        i32 dst = ns_ssa_emit_value(b, NS_SSA_OP_INDEX, table, -1, ns_type_unknown, ns_str_null, (ns_token_t){0}, i);
        ns_ssa_inst *inst = &b->fn->insts[ns_array_last(b->fn->blocks[b->block].insts)[0]];
        inst->b = idx;
        return dst;
    }
    case NS_AST_STR_FMT_EXPR:
        return ns_ssa_emit_value(b, NS_SSA_OP_CONST, -1, -1, ns_type_str, n->str_fmt.fmt, (ns_token_t){0}, i);
    default: {
        ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
        ns_ssa_emit_raw(b, trap);
        return -1;
    }
    }
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
        inst->b = v_else;
        inst->target0 = then_block;
        inst->target1 = else_block;
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
    ns_ssa_binding *pre_env = ns_ssa_env_clone(b->env);
    i32 nbind = (i32)ns_array_length(b->env);
    i32 *phi_inst = ns_null;
    ns_array_set_length(phi_inst, nbind > 0 ? nbind : 0);
    for (i32 k = 0; k < nbind; ++k) {
        ns_ssa_binding *bd = &b->env[k];
        i32 phi = ns_ssa_emit_value(b, NS_SSA_OP_PHI, pre_env[k].value, -1, bd->type, bd->name, (ns_token_t){0}, ast);
        i32 idx = ns_array_last(b->fn->blocks[header].insts)[0];
        b->fn->insts[idx].b = pre_env[k].value; // placeholder until the back edge is known
        b->fn->insts[idx].target0 = preheader;
        b->fn->insts[idx].target1 = body_block;
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
        phi->b = ns_ssa_env_get_value(b->env, phi->name, phi->dst);
        phi->target1 = body_exit;
    }
}

static void ns_ssa_lower_loop(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = &b->ctx->nodes[i];
    i32 preheader = b->block;
    i32 cond_block = ns_ssa_new_block(b, n->loop_stmt.condition);
    i32 body_block = ns_ssa_new_block(b, n->loop_stmt.body);
    i32 exit_block = ns_ssa_new_block(b, i);

    ns_ssa_emit_jump(b, cond_block, i);
    b->block = cond_block;
    b->fn->blocks[cond_block].terminated = false;

    i32 *phi_inst = ns_ssa_loop_header_phis(b, cond_block, preheader, body_block, i);
    ns_ssa_binding *header_env = ns_ssa_env_clone(b->env);

    if (n->loop_stmt.condition > 0) {
        i32 cond = ns_ssa_lower_expr(b, n->loop_stmt.condition);
        ns_ssa_emit_branch(b, cond, body_block, exit_block, i);
    } else {
        ns_ssa_emit_jump(b, body_block, i);
    }

    ns_ssa_loop_ctx loop = {.break_block = exit_block, .continue_block = cond_block};
    ns_array_push(b->loops, loop);

    b->block = body_block;
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
    b->env = header_env; // values live after the loop are the header phis
}

static void ns_ssa_lower_for(ns_ssa_builder *b, i32 i) {
    ns_ast_t *n = &b->ctx->nodes[i];
    ns_ast_t *g = &b->ctx->nodes[n->for_stmt.generator];
    if (!g->gen_expr.range) {
        ns_ssa_lower_compound(b, n->for_stmt.body);
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

    ns_ssa_loop_ctx loop = {.break_block = exit_block, .continue_block = cond_block};
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
        } else {
            value = ns_ssa_emit_value(b, NS_SSA_OP_UNDEF, -1, -1, ns_type_unknown, n->var_def.name.val, n->var_def.name, i);
        }
        ns_ssa_env_bind(b, n->var_def.name.val, value, ns_type_unknown);
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
            ns_ssa_emit_ret(b, ret, i);
        } break;
        case NS_TOKEN_BREAK: {
            if (ns_array_length(b->loops) > 0) {
                ns_ssa_emit_jump(b, ns_array_last(b->loops)->break_block, i);
            } else {
                ns_ssa_inst trap = NS_SSA_INST_INIT(NS_SSA_OP_TRAP, i);
                ns_ssa_emit_raw(b, trap);
            }
        } break;
        case NS_TOKEN_CONTINUE: {
            if (ns_array_length(b->loops) > 0) {
                ns_ssa_emit_jump(b, ns_array_last(b->loops)->continue_block, i);
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

static void ns_ssa_lower_fn(ns_ssa_builder *b, i32 ast, ns_str name, i32 body, i32 arg_head, i32 arg_count) {
    ns_ssa_fn fn = {0};
    fn.name = name;
    fn.ast = ast;
    fn.entry = 0;
    ns_array_push(b->m->fns, fn);
    b->fn = &b->m->fns[ns_array_length(b->m->fns) - 1];
    b->block = ns_ssa_new_block(b, ast);
    b->next_value = 0;

    ns_array_free(b->env);
    b->env = NULL;
    b->loops = NULL;
    ns_ssa_seed_global_consts(b);

    i32 arg = arg_head;
    for (i32 ai = 0; ai < arg_count && arg > 0; ++ai) {
        ns_ast_t *an = &b->ctx->nodes[arg];
        ns_type arg_type = ns_ssa_type_from_label(b, an->arg.type);
        i32 v = ns_ssa_emit_value(b, NS_SSA_OP_PARAM, -1, ai, arg_type, an->arg.name.val, an->arg.name, arg);
        ns_ssa_env_bind(b, an->arg.name.val, v, arg_type);
        arg = an->next;
    }

    ns_ssa_lower_compound(b, body);
    if (!b->fn->blocks[b->block].terminated) {
        ns_ssa_emit_ret(b, -1, ast);
    }

    ns_array_free(b->env);
    ns_array_free(b->loops);
    b->env = NULL;
    b->loops = NULL;
}

ns_return_ptr ns_ssa_build(ns_ast_ctx *ctx) {
    if (ctx == NULL || ns_array_length(ctx->nodes) == 0) {
        return ns_return_error(ptr, ns_code_loc_nil, NS_ERR_SYNTAX, "ast ctx is empty");
    }

    ns_ssa_module *m = ns_malloc(sizeof(ns_ssa_module));
    memset(m, 0, sizeof(*m));

    ns_ssa_builder b = {0};
    b.ctx = ctx;
    b.m = m;

    // The VM semantic pass is shared by interpretation and compilation. It
    // resolves enum values once, enforces nominal typing, and gives SSA the
    // underlying integer representation used by every native backend.
    ns_vm semantic_vm = {0};
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

    ns_bool has_init = false;
    i32 init_ast = 0;
    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s];
        switch (n->type) {
        case NS_AST_FN_DEF:
            ns_ssa_lower_fn(&b, s, n->fn_def.name.val, n->fn_def.body, n->next, n->fn_def.arg_count);
            break;
        case NS_AST_OP_FN_DEF:
            ns_ssa_lower_fn(&b, s, n->ops_fn_def.ops.val, n->ops_fn_def.body, n->ops_fn_def.left, 2);
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
        ns_array_push(m->fns, fn);
        b.fn = &m->fns[ns_array_length(m->fns) - 1];
        b.block = ns_ssa_new_block(&b, init_ast);
        b.next_value = 0;
        b.env = NULL;
        b.loops = NULL;
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

    ns_array_free(b.globals);
    ns_array_free(b.import_seen);

    return ns_return_ok(ptr, m);
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
        ns_array_free(fn->insts);
    }
    ns_array_free(m->fns);
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
