#include "ns_shader.h"

// ns fn -> shader source transcoder. The emitter walks the fn-body AST directly
// (like ns_ast_print.c) because shader source needs structured control flow;
// type facts come from the VM symbol table populated by ns_vm_parse. Only the
// shader subset of ns is accepted: scalars, simd vectors (float2/3/4 and mat4), user
// structs, arithmetic/logic, if/for/loop and calls to other user fns. Anything
// else produces a source-located error instead of silently emitting bad code.

#define NS_SHADER_MAX_DEPTH 64

static char ns_shader_err[512];

typedef struct ns_shader_local {
    ns_str name;
    ns_type t;
} ns_shader_local;

typedef struct ns_shader_emit {
    ns_vm *vm;
    ns_ast_ctx *ctx;
    ns_shader_target target;

    ns_str out; // final shader source
    ns_str pre; // hoisted lines (struct-literal temps) for the current statement
    i32 indent;
    i32 tmp_id;

    i32 *structs;  // user struct symbol indices, dependency order
    i32 *fns;      // helper fn symbol indices, callees first
    i32 *fn_visit; // fn DFS stack for recursion detection

    ns_shader_entry_desc *entries;

    i32 *vs_inputs; // struct symbol indices used as vertex input
    i32 *stage_ios; // struct symbol indices used as stage io (vs out == fs in)

    ns_shader_local *locals;
    ns_bool uses_shadow_map;
    ns_bool uses_texture_map;
    ns_bool uses_scene_uniforms;
    ns_bool uses_global_id;
    ns_bool uses_write_texture;
} ns_shader_emit;

#define ns_shader_try(x)                                                                                                                             \
    do {                                                                                                                                             \
        ns_return_void _r = (x);                                                                                                                     \
        if (ns_return_is_error(_r)) return _r;                                                                                                       \
    } while (0)

#define ns_shader_loc(e, n) ns_ast_state_loc((e)->ctx, (n)->state)

static ns_return_void ns_shader_emit_expr(ns_shader_emit *e, i32 i, ns_str *dst);
static ns_return_void ns_shader_emit_stmt(ns_shader_emit *e, i32 i);
static ns_return_void ns_shader_collect_stmt(ns_shader_emit *e, i32 i, i32 depth);

// ---------------------------------------------------------------------------
// small string helpers
// ---------------------------------------------------------------------------
static void ns_shader_cstr(ns_str *dst, const char *s) { ns_str_append_len(dst, (const i8 *)s, (i32)strlen(s)); }
static void ns_shader_str(ns_str *dst, ns_str s) { ns_str_append_len(dst, s.data, s.len); }
static void ns_shader_i32(ns_str *dst, i32 n) { ns_str_append_i32(dst, n); }
static void ns_shader_pad(ns_str *dst, i32 indent) {
    for (i32 i = 0; i < indent; ++i) ns_shader_cstr(dst, "    ");
}

static ns_str ns_shader_literal_body(ns_token_t t) {
    i32 suffix_len = 0;
    switch (t.suffix) {
    case NS_NUM_SUFFIX_U8:
    case NS_NUM_SUFFIX_U16:
    case NS_NUM_SUFFIX_U64:
    case NS_NUM_SUFFIX_BF16: suffix_len = 2; break;
    case NS_NUM_SUFFIX_I8:
    case NS_NUM_SUFFIX_I16:
    case NS_NUM_SUFFIX_U32:
    case NS_NUM_SUFFIX_I64:
    case NS_NUM_SUFFIX_F64:
    case NS_NUM_SUFFIX_F16: suffix_len = 1; break;
    default: break;
    }
    if (suffix_len <= 0 || t.val.len < suffix_len) return t.val;
    return ns_str_range(t.val.data, t.val.len - suffix_len);
}

static const char *ns_shader_half_type(ns_shader_target target) {
    return target == NS_SHADER_GLSL_VULKAN ? "float16_t" : "half";
}

static ns_bool ns_shader_index_in(i32 *arr, i32 v) {
    for (i32 i = 0, l = (i32)ns_array_length(arr); i < l; ++i) {
        if (arr[i] == v) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// target / stage naming
// ---------------------------------------------------------------------------
ns_shader_target ns_shader_target_from_str(ns_str s) {
    if (ns_str_equals(s, ns_str_cstr("msl")) || ns_str_equals(s, ns_str_cstr("metal"))) return NS_SHADER_MSL;
    if (ns_str_equals(s, ns_str_cstr("glsl")) || ns_str_equals(s, ns_str_cstr("vulkan")) || ns_str_equals(s, ns_str_cstr("spirv"))) return NS_SHADER_GLSL_VULKAN;
    if (ns_str_equals(s, ns_str_cstr("hlsl")) || ns_str_equals(s, ns_str_cstr("dx12")) || ns_str_equals(s, ns_str_cstr("dxil"))) return NS_SHADER_HLSL;
    return NS_SHADER_TARGET_UNKNOWN;
}

ns_str ns_shader_target_name(ns_shader_target t) {
    switch (t) {
    case NS_SHADER_MSL: return ns_str_cstr("msl");
    case NS_SHADER_GLSL_VULKAN: return ns_str_cstr("glsl");
    case NS_SHADER_HLSL: return ns_str_cstr("hlsl");
    default: return ns_str_cstr("unknown");
    }
}

ns_str ns_shader_entry_name(ns_shader_target t, ns_str fn_name) {
    // GLSL entry points are always the generated `void main()` wrapper.
    if (t == NS_SHADER_GLSL_VULKAN) return ns_str_cstr("main");
    return fn_name;
}

// ---------------------------------------------------------------------------
// symbol helpers
// ---------------------------------------------------------------------------
static ns_bool ns_shader_is_main_tu(ns_symbol *s) { return s->lib.len == 0 || ns_str_equals(s->lib, ns_str_cstr("main")); }

// float2/3/4 -> component count, mat4 -> 16, otherwise 0.
static i32 ns_shader_simd_dim(ns_str name) {
    if (ns_str_equals(name, ns_str_cstr("float2"))) return 2;
    if (ns_str_equals(name, ns_str_cstr("float3"))) return 3;
    if (ns_str_equals(name, ns_str_cstr("float4"))) return 4;
    if (ns_str_equals(name, ns_str_cstr("mat4"))) return 16;
    return 0;
}

static ns_bool ns_shader_is_simd(ns_symbol *s) {
    return s->type == NS_SYMBOL_STRUCT && ns_str_equals(s->lib, ns_str_cstr("simd")) && ns_shader_simd_dim(s->name) != 0;
}

static const char *ns_shader_simd_name(ns_shader_target target, i32 dim) {
    switch (dim) {
    case 2: return target == NS_SHADER_GLSL_VULKAN ? "vec2" : "float2";
    case 3: return target == NS_SHADER_GLSL_VULKAN ? "vec3" : "float3";
    case 4: return target == NS_SHADER_GLSL_VULKAN ? "vec4" : "float4";
    case 16: return target == NS_SHADER_GLSL_VULKAN ? "mat4" : "float4x4";
    default: return "float";
    }
}

// Global symbol lookup that skips the eval symbol stack (a runtime transpile
// happens inside an active call, whose locals must not shadow type names).
// Prefers a main-TU match so user symbols win over same-named lib symbols.
static ns_symbol *ns_shader_find_global(ns_vm *vm, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(vm->symbols); i < l; ++i) {
        if (ns_str_equals(vm->symbols[i].name, name) && ns_shader_is_main_tu(&vm->symbols[i])) return &vm->symbols[i];
    }
    for (i32 i = 0, l = (i32)ns_array_length(vm->symbols); i < l; ++i) {
        if (ns_str_equals(vm->symbols[i].name, name)) return &vm->symbols[i];
    }
    return ns_null;
}

// Resolve a struct name (or a type alias of one, e.g. quatf) to its struct
// symbol index in vm->symbols, or -1.
static i32 ns_shader_struct_index(ns_vm *vm, ns_str name) {
    ns_symbol *s = ns_shader_find_global(vm, name);
    if (!s) return -1;
    if (s->type == NS_SYMBOL_TYPE && ns_type_is(s->val.t, NS_TYPE_STRUCT)) return (i32)ns_type_index(s->val.t);
    if (s->type == NS_SYMBOL_STRUCT) return (i32)(s - vm->symbols);
    return -1;
}

// The stage-io position field: `position: float4`.
static ns_bool ns_shader_is_position_field(ns_vm *vm, ns_struct_field *f) {
    if (!ns_str_equals(f->name, ns_str_cstr("position"))) return false;
    if (!ns_type_is(f->t, NS_TYPE_STRUCT)) return false;
    ns_symbol *s = &vm->symbols[ns_type_index(f->t)];
    return ns_shader_is_simd(s) && ns_shader_simd_dim(s->name) == 4;
}

// ---------------------------------------------------------------------------
// type naming
// ---------------------------------------------------------------------------
static ns_return_void ns_shader_type_name(ns_shader_emit *e, ns_type t, ns_str *dst, ns_code_loc loc) {
    if (ns_type_is_ref(t)) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: ref types are not supported in shader fns.");
    if (ns_type_is_array(t)) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: array types are not supported in shader fns.");
    switch (t.type) {
    case NS_TYPE_F32:
    case NS_TYPE_F64: ns_shader_cstr(dst, "float"); break; // f64 demoted: shaders are f32
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32: ns_shader_cstr(dst, "int"); break;
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: ns_shader_cstr(dst, "uint"); break;
    case NS_TYPE_BOOL: ns_shader_cstr(dst, "bool"); break;
    case NS_TYPE_VOID: ns_shader_cstr(dst, "void"); break;
    case NS_TYPE_STRUCT: {
        ns_symbol *s = &e->vm->symbols[ns_type_index(t)];
        if (ns_shader_is_simd(s)) {
            ns_shader_cstr(dst, ns_shader_simd_name(e->target, ns_shader_simd_dim(s->name)));
        } else {
            ns_shader_str(dst, s->name);
        }
    } break;
    default:
        return ns_return_error(void, loc, NS_ERR_EVAL, "shader: unsupported type in shader fn (str, containers, fn and 64-bit ints are not allowed).");
    }
    return ns_return_ok_void;
}

static ns_return_void ns_shader_zero_value(ns_shader_emit *e, ns_type t, ns_str *dst, ns_code_loc loc) {
    switch (t.type) {
    case NS_TYPE_F32:
    case NS_TYPE_F64: ns_shader_cstr(dst, "0.0"); break;
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32:
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: ns_shader_cstr(dst, "0"); break;
    case NS_TYPE_BOOL: ns_shader_cstr(dst, "false"); break;
    case NS_TYPE_STRUCT: {
        ns_symbol *s = &e->vm->symbols[ns_type_index(t)];
        i32 dim = ns_shader_is_simd(s) ? ns_shader_simd_dim(s->name) : 0;
        if (dim < 2 || dim > 4) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: struct field must be explicitly initialized.");
        ns_shader_cstr(dst, ns_shader_simd_name(e->target, dim));
        ns_shader_cstr(dst, "(");
        for (i32 c = 0; c < dim; ++c) ns_shader_cstr(dst, c == 0 ? "0.0" : ", 0.0");
        ns_shader_cstr(dst, ")");
    } break;
    default: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: cannot zero-initialize this type.");
    }
    return ns_return_ok_void;
}

// ---------------------------------------------------------------------------
// builtin math fns
// ---------------------------------------------------------------------------
typedef struct ns_shader_builtin {
    const char *name;
    const char *msl;
    const char *glsl;
    const char *hlsl;
    ns_bool scalar_ret; // dot/length/distance return f32; others follow arg0
} ns_shader_builtin;

static const ns_shader_builtin ns_shader_builtins[] = {
    {"sqrt", "sqrt", "sqrt", "sqrt", false},       {"sin", "sin", "sin", "sin", false},
    {"cos", "cos", "cos", "cos", false},           {"tan", "tan", "tan", "tan", false},
    {"abs", "abs", "abs", "abs", false},           {"floor", "floor", "floor", "floor", false},
    {"ceil", "ceil", "ceil", "ceil", false},       {"pow", "pow", "pow", "pow", false},
    {"min", "min", "min", "min", false},           {"max", "max", "max", "max", false},
    {"clamp", "clamp", "clamp", "clamp", false},   {"normalize", "normalize", "normalize", "normalize", false},
    {"cross", "cross", "cross", "cross", false},   {"dot", "dot", "dot", "dot", true},
    {"length", "length", "length", "length", true}, {"distance", "distance", "distance", "distance", true},
    {"lerp", "mix", "mix", "lerp", false},         {"mix", "mix", "mix", "lerp", false},
    {"fract", "fract", "fract", "frac", false},
    {"shader_sample_shadow", "ns_shadow_compare", "ns_shadow_compare", "ns_shadow_compare", true},
    {"shader_sample_texture", "ns_texture_sample", "ns_texture_sample", "ns_texture_sample", false},
    {"shader_transform_position", "ns_transform_position", "ns_transform_position", "ns_transform_position", false},
    {"shader_transform_normal", "ns_transform_normal", "ns_transform_normal", "ns_transform_normal", false},
    {"shader_shadow_clip_position", "ns_shadow_clip_position", "ns_shadow_clip_position", "ns_shadow_clip_position", false},
    {"shader_scene_selected", "ns_scene_selected", "ns_scene_selected", "ns_scene_selected", true},
    {"shader_scene_textured", "ns_scene_textured", "ns_scene_textured", "ns_scene_textured", true},
    {"shader_scene_receives_shadow", "ns_scene_receives_shadow", "ns_scene_receives_shadow", "ns_scene_receives_shadow", true},
    {"shader_global_id_x", "ns_global_id_x", "ns_global_id_x", "ns_global_id_x", true},
    {"shader_global_id_y", "ns_global_id_y", "ns_global_id_y", "ns_global_id_y", true},
    {"shader_global_id_z", "ns_global_id_z", "ns_global_id_z", "ns_global_id_z", true},
    {"shader_write_texture", "ns_write_texture", "ns_write_texture", "ns_write_texture", false},
};

static const ns_shader_builtin *ns_shader_find_builtin(ns_str name) {
    for (szt i = 0; i < sizeof(ns_shader_builtins) / sizeof(ns_shader_builtins[0]); ++i) {
        if (ns_str_equals(name, ns_str_cstr(ns_shader_builtins[i].name))) return &ns_shader_builtins[i];
    }
    return ns_null;
}

static const char *ns_shader_builtin_name(const ns_shader_builtin *b, ns_shader_target t) {
    switch (t) {
    case NS_SHADER_MSL: return b->msl;
    case NS_SHADER_GLSL_VULKAN: return b->glsl;
    default: return b->hlsl;
    }
}

// ---------------------------------------------------------------------------
// local type inference (shader subset only; unknown on anything else)
// ---------------------------------------------------------------------------
static ns_type ns_shader_local_type(ns_shader_emit *e, ns_str name) {
    for (i32 i = (i32)ns_array_length(e->locals) - 1; i >= 0; --i) {
        if (ns_str_equals(e->locals[i].name, name)) return e->locals[i].t;
    }
    return ns_type_unknown;
}

static ns_type ns_shader_infer(ns_shader_emit *e, i32 i) {
    ns_ast_t *n = &e->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_shader_infer(e, n->expr.body);
    case NS_AST_PRIMARY_EXPR: {
        switch (n->primary_expr.token.type) {
        case NS_TOKEN_INT_LITERAL: return ns_type_i32;
        case NS_TOKEN_FLT_LITERAL: return ns_type_f32;
        case NS_TOKEN_TRUE:
        case NS_TOKEN_FALSE: return ns_type_bool;
        case NS_TOKEN_IDENTIFIER: return ns_shader_local_type(e, n->primary_expr.token.val);
        default: return ns_type_unknown;
        }
    }
    case NS_AST_MEMBER_EXPR: {
        ns_type lt = ns_shader_infer(e, n->member_expr.left);
        if (!ns_type_is(lt, NS_TYPE_STRUCT)) return ns_type_unknown;
        ns_ast_t *r = &e->ctx->nodes[n->member_expr.right];
        if (r->type != NS_AST_PRIMARY_EXPR) return ns_type_unknown;
        ns_symbol *s = &e->vm->symbols[ns_type_index(lt)];
        for (i32 f = 0, l = (i32)ns_array_length(s->st.fields); f < l; ++f) {
            if (ns_str_equals(s->st.fields[f].name, r->primary_expr.token.val)) return s->st.fields[f].t;
        }
        return ns_type_unknown;
    }
    case NS_AST_CALL_EXPR: {
        ns_ast_t *callee = &e->ctx->nodes[n->call_expr.callee];
        if (callee->type != NS_AST_PRIMARY_EXPR) return ns_type_unknown;
        ns_str name = callee->primary_expr.token.val;
        const ns_shader_builtin *b = ns_shader_find_builtin(name);
        if (b) {
            if (ns_str_starts_with(name, ns_str_cstr("shader_"))) {
                ns_symbol *decl = ns_shader_find_global(e->vm, name);
                if (decl && decl->type == NS_SYMBOL_FN) return decl->fn.ret;
            }
            if (b->scalar_ret) return ns_type_f32;
            return n->call_expr.arg_count > 0 ? ns_shader_infer(e, n->next) : ns_type_unknown;
        }
        ns_symbol *s = ns_shader_find_global(e->vm, name);
        if (s && s->type == NS_SYMBOL_FN) return s->fn.ret;
        return ns_type_unknown;
    }
    case NS_AST_DESIG_EXPR: {
        i32 st = ns_shader_struct_index(e->vm, n->desig_expr.name.val);
        if (st < 0) return ns_type_unknown;
        return e->vm->symbols[st].st.st.t;
    }
    case NS_AST_BINARY_EXPR: {
        switch (n->binary_expr.op.type) {
        case NS_TOKEN_LOGIC_OP:
        case NS_TOKEN_EQ_OP:
        case NS_TOKEN_REL_OP: return ns_type_bool;
        default: break;
        }
        ns_type lt = ns_shader_infer(e, n->binary_expr.left);
        ns_type rt = ns_shader_infer(e, n->binary_expr.right);
        if (ns_type_is(lt, NS_TYPE_STRUCT)) return lt;
        if (ns_type_is(rt, NS_TYPE_STRUCT)) return rt;
        if (ns_type_is_float(lt) || ns_type_is_float(rt)) return ns_type_f32;
        if (!ns_type_is_unknown(lt)) return lt;
        return rt;
    }
    case NS_AST_UNARY_EXPR: {
        if (n->unary_expr.op.type == NS_TOKEN_CMP_OP) return ns_type_bool;
        return ns_shader_infer(e, n->unary_expr.expr);
    }
    case NS_AST_CAST_EXPR: {
        ns_return_type rt = ns_vm_parse_type_by_token(e->vm, n->cast_expr.type, ns_shader_loc(e, n));
        return ns_return_is_error(rt) ? ns_type_unknown : rt.r;
    }
    default: return ns_type_unknown;
    }
}

// ---------------------------------------------------------------------------
// dependency collection
// ---------------------------------------------------------------------------
static ns_return_void ns_shader_collect_struct(ns_shader_emit *e, i32 st_index, ns_code_loc loc, i32 depth) {
    if (depth > NS_SHADER_MAX_DEPTH) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: struct nesting too deep.");
    ns_symbol *s = &e->vm->symbols[st_index];
    if (s->type != NS_SYMBOL_STRUCT) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: expected a struct type.");
    if (ns_shader_is_simd(s)) return ns_return_ok_void; // maps to a native vector type
    if (ns_shader_index_in(e->structs, st_index)) return ns_return_ok_void;
    for (i32 f = 0, l = (i32)ns_array_length(s->st.fields); f < l; ++f) {
        ns_type t = s->st.fields[f].t;
        if (ns_type_is(t, NS_TYPE_STRUCT)) {
            ns_shader_try(ns_shader_collect_struct(e, (i32)ns_type_index(t), loc, depth + 1));
        }
    }
    ns_array_push(e->structs, st_index); // post-order: dependencies first
    return ns_return_ok_void;
}

static ns_return_void ns_shader_collect_type(ns_shader_emit *e, ns_type t, ns_code_loc loc) {
    if (ns_type_is(t, NS_TYPE_STRUCT)) return ns_shader_collect_struct(e, (i32)ns_type_index(t), loc, 0);
    return ns_return_ok_void;
}

static ns_return_void ns_shader_collect_fn(ns_shader_emit *e, i32 fn_index, ns_bool is_entry, i32 depth);

static ns_return_void ns_shader_collect_expr(ns_shader_emit *e, i32 i, i32 depth) {
    if (i == 0) return ns_return_ok_void;
    if (depth > NS_SHADER_MAX_DEPTH) return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: expression nesting too deep.");
    ns_ast_t *n = &e->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_shader_collect_expr(e, n->expr.body, depth + 1);
    case NS_AST_BINARY_EXPR:
        ns_shader_try(ns_shader_collect_expr(e, n->binary_expr.left, depth + 1));
        return ns_shader_collect_expr(e, n->binary_expr.right, depth + 1);
    case NS_AST_UNARY_EXPR: return ns_shader_collect_expr(e, n->unary_expr.expr, depth + 1);
    case NS_AST_MEMBER_EXPR: return ns_shader_collect_expr(e, n->member_expr.left, depth + 1);
    case NS_AST_CAST_EXPR: return ns_shader_collect_expr(e, n->cast_expr.expr, depth + 1);
    case NS_AST_CALL_EXPR: {
        ns_ast_t *callee = &e->ctx->nodes[n->call_expr.callee];
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_sample_shadow"))) {
            e->uses_shadow_map = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_sample_texture"))) {
            e->uses_texture_map = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_starts_with(callee->primary_expr.token.val, ns_str_cstr("shader_global_id_"))) {
            e->uses_global_id = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_write_texture"))) {
            e->uses_write_texture = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            (ns_str_starts_with(callee->primary_expr.token.val, ns_str_cstr("shader_transform_")) ||
             ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_shadow_clip_position")) ||
             ns_str_starts_with(callee->primary_expr.token.val, ns_str_cstr("shader_scene_")))) {
            e->uses_scene_uniforms = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR && !ns_shader_find_builtin(callee->primary_expr.token.val)) {
            ns_symbol *s = ns_shader_find_global(e->vm, callee->primary_expr.token.val);
            if (s && s->type == NS_SYMBOL_FN) {
                ns_shader_try(ns_shader_collect_fn(e, (i32)(s - e->vm->symbols), false, depth + 1));
            }
            // unresolved callees fail later in ns_shader_emit_expr with a location
        }
        i32 next = n->next;
        for (i32 a = 0; a < n->call_expr.arg_count; ++a) {
            ns_shader_try(ns_shader_collect_expr(e, next, depth + 1));
            next = e->ctx->nodes[next].next;
        }
        return ns_return_ok_void;
    }
    case NS_AST_DESIG_EXPR: {
        i32 st = ns_shader_struct_index(e->vm, n->desig_expr.name.val);
        if (st >= 0) {
            ns_shader_try(ns_shader_collect_struct(e, st, ns_shader_loc(e, n), 0));
        }
        i32 fi = n->next;
        for (i32 f = 0; f < n->desig_expr.count; ++f) {
            ns_ast_t *field = &e->ctx->nodes[fi];
            ns_shader_try(ns_shader_collect_expr(e, field->field_def.expr, depth + 1));
            fi = field->next;
        }
        return ns_return_ok_void;
    }
    default: return ns_return_ok_void; // unsupported nodes are rejected at emission
    }
}

static ns_return_void ns_shader_collect_stmt(ns_shader_emit *e, i32 i, i32 depth) {
    if (i == 0) return ns_return_ok_void;
    if (depth > NS_SHADER_MAX_DEPTH) return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: statement nesting too deep.");
    ns_ast_t *n = &e->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_COMPOUND_STMT: {
        ns_ast_t *stmt = n;
        for (i32 s = 0; s < n->compound_stmt.count; ++s) {
            ns_shader_try(ns_shader_collect_stmt(e, stmt->next, depth + 1));
            stmt = &e->ctx->nodes[stmt->next];
        }
        return ns_return_ok_void;
    }
    case NS_AST_VAR_DEF: {
        if (n->var_def.type != 0) {
            ns_return_type rt = ns_vm_parse_type(e->vm, e->ctx, &e->ctx->nodes[n->var_def.type]);
            if (!ns_return_is_error(rt)) {
                ns_shader_try(ns_shader_collect_type(e, rt.r, ns_shader_loc(e, n)));
            }
        }
        return ns_shader_collect_expr(e, n->var_def.expr, depth + 1);
    }
    case NS_AST_JUMP_STMT: return ns_shader_collect_expr(e, n->jump_stmt.expr, depth + 1);
    case NS_AST_IF_STMT:
        ns_shader_try(ns_shader_collect_expr(e, n->if_stmt.condition, depth + 1));
        ns_shader_try(ns_shader_collect_stmt(e, n->if_stmt.body, depth + 1));
        return ns_shader_collect_stmt(e, n->if_stmt.else_body, depth + 1);
    case NS_AST_FOR_STMT: {
        ns_ast_t *gen = &e->ctx->nodes[n->for_stmt.generator];
        ns_shader_try(ns_shader_collect_expr(e, gen->gen_expr.from, depth + 1));
        ns_shader_try(ns_shader_collect_expr(e, gen->gen_expr.to, depth + 1));
        return ns_shader_collect_stmt(e, n->for_stmt.body, depth + 1);
    }
    case NS_AST_LOOP_STMT:
        ns_shader_try(ns_shader_collect_expr(e, n->loop_stmt.condition, depth + 1));
        return ns_shader_collect_stmt(e, n->loop_stmt.body, depth + 1);
    default: return ns_shader_collect_expr(e, i, depth + 1); // expression statement
    }
}

static ns_return_void ns_shader_collect_fn(ns_shader_emit *e, i32 fn_index, ns_bool is_entry, i32 depth) {
    if (depth > NS_SHADER_MAX_DEPTH) return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: call nesting too deep.");
    ns_symbol *s = &e->vm->symbols[fn_index];
    if (!is_entry) {
        for (i32 k = 0, l = (i32)ns_array_length(e->entries); k < l; ++k) {
            if (e->entries[k].fn_index == fn_index)
                return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: a shader entry fn cannot be called from another shader fn.");
        }
        if (ns_shader_index_in(e->fns, fn_index)) return ns_return_ok_void;
    }
    if (ns_shader_index_in(e->fn_visit, fn_index)) {
        return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: recursive fn calls are not supported in shaders.");
    }

    if (s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || s->fn.body == 0) {
        snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: `%.*s` is not a transpilable fn (native ref fns have no body).", s->name.len, s->name.data);
        return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, ns_shader_err);
    }
    if (!ns_shader_is_main_tu(s)) {
        snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: fn `%.*s` must be defined in the current translation unit.", s->name.len, s->name.data);
        return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, ns_shader_err);
    }

    ns_ast_t *fn_node = &e->ctx->nodes[s->fn.ast];
    ns_code_loc loc = ns_shader_loc(e, fn_node);
    ns_shader_try(ns_shader_collect_type(e, s->fn.ret, loc));
    for (i32 a = 0, l = (i32)ns_array_length(s->fn.args); a < l; ++a) {
        ns_shader_try(ns_shader_collect_type(e, s->fn.args[a].val.t, loc));
    }

    ns_array_push(e->fn_visit, fn_index);
    ns_return_void body = ns_shader_collect_stmt(e, s->fn.body, depth + 1);
    ns_array_set_length(e->fn_visit, ns_array_length(e->fn_visit) - 1);
    if (ns_return_is_error(body)) return body;

    if (!is_entry) ns_array_push(e->fns, fn_index); // post-order: callees first
    return ns_return_ok_void;
}

// ---------------------------------------------------------------------------
// expression emission
// ---------------------------------------------------------------------------
static ns_return_void ns_shader_emit_desig(ns_shader_emit *e, ns_ast_t *n, ns_str *dst) {
    ns_code_loc loc = ns_shader_loc(e, n);
    i32 st_index = ns_shader_struct_index(e->vm, n->desig_expr.name.val);
    if (st_index < 0) {
        snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: unknown struct `%.*s`.", n->desig_expr.name.val.len, n->desig_expr.name.val.data);
        return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
    }
    ns_symbol *s = &e->vm->symbols[st_index];
    i32 dim = ns_shader_is_simd(s) ? ns_shader_simd_dim(s->name) : 0;
    if (dim == 16) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: mat4 designated init is not supported yet.");

    // per struct field (declaration order): the matching field expr node, or 0
    i32 field_count = (i32)ns_array_length(s->st.fields);
    i32 provided[64];
    if (field_count > 64) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: too many struct fields.");
    for (i32 f = 0; f < field_count; ++f) provided[f] = 0;
    i32 fi = n->next;
    for (i32 f = 0; f < n->desig_expr.count; ++f) {
        ns_ast_t *field = &e->ctx->nodes[fi];
        ns_bool found = false;
        for (i32 k = 0; k < field_count; ++k) {
            if (ns_str_equals(s->st.fields[k].name, field->field_def.name.val)) {
                provided[k] = field->field_def.expr;
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: `%.*s` has no field `%.*s`.", s->name.len, s->name.data, field->field_def.name.val.len,
                     field->field_def.name.val.data);
            return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
        }
        fi = field->next;
    }

    if (dim >= 2 && dim <= 4) {
        // simd vector: native constructor, fields reordered to declaration order,
        // missing components zero-filled.
        ns_shader_cstr(dst, ns_shader_simd_name(e->target, dim));
        ns_shader_cstr(dst, "(");
        for (i32 f = 0; f < field_count; ++f) {
            if (f > 0) ns_shader_cstr(dst, ", ");
            if (provided[f]) {
                ns_shader_try(ns_shader_emit_expr(e, provided[f], dst));
            } else {
                ns_shader_cstr(dst, "0.0");
            }
        }
        ns_shader_cstr(dst, ")");
        return ns_return_ok_void;
    }

    // user struct: hoist a temp (HLSL has no struct constructors), assign fields.
    i32 tmp = e->tmp_id++;
    ns_str decl = {.data = ns_null, .len = 0, .dynamic = true};
    ns_shader_pad(&decl, e->indent);
    ns_shader_str(&decl, s->name);
    ns_shader_cstr(&decl, " ns_t");
    ns_shader_i32(&decl, tmp);
    ns_shader_cstr(&decl, ";\n");
    for (i32 f = 0; f < field_count; ++f) {
        ns_shader_pad(&decl, e->indent);
        ns_shader_cstr(&decl, "ns_t");
        ns_shader_i32(&decl, tmp);
        ns_shader_cstr(&decl, ".");
        ns_shader_str(&decl, s->st.fields[f].name);
        ns_shader_cstr(&decl, " = ");
        if (provided[f]) {
            ns_return_void r = ns_shader_emit_expr(e, provided[f], &decl);
            if (ns_return_is_error(r)) {
                ns_array_free(decl.data);
                return r;
            }
        } else {
            ns_return_void r = ns_shader_zero_value(e, s->st.fields[f].t, &decl, loc);
            if (ns_return_is_error(r)) {
                ns_array_free(decl.data);
                return r;
            }
        }
        ns_shader_cstr(&decl, ";\n");
    }
    ns_shader_str(&e->pre, decl);
    ns_array_free(decl.data);

    ns_shader_cstr(dst, "ns_t");
    ns_shader_i32(dst, tmp);
    return ns_return_ok_void;
}

static ns_return_void ns_shader_emit_expr(ns_shader_emit *e, i32 i, ns_str *dst) {
    ns_ast_t *n = &e->ctx->nodes[i];
    ns_code_loc loc = ns_shader_loc(e, n);
    switch (n->type) {
    case NS_AST_EXPR: {
        if (n->expr.atomic) return ns_shader_emit_expr(e, n->expr.body, dst);
        ns_shader_cstr(dst, "(");
        ns_shader_try(ns_shader_emit_expr(e, n->expr.body, dst));
        ns_shader_cstr(dst, ")");
        return ns_return_ok_void;
    }
    case NS_AST_PRIMARY_EXPR: {
        switch (n->primary_expr.token.type) {
        case NS_TOKEN_INT_LITERAL:
            ns_shader_str(dst, ns_shader_literal_body(n->primary_expr.token));
            return ns_return_ok_void;
        case NS_TOKEN_FLT_LITERAL: {
            ns_token_t tok = n->primary_expr.token;
            if (tok.suffix == NS_NUM_SUFFIX_F16 || tok.suffix == NS_NUM_SUFFIX_BF16) {
                if (tok.suffix == NS_NUM_SUFFIX_BF16) ns_warn("shader", "brain-float literal fallback to half.\n");
                ns_shader_cstr(dst, ns_shader_half_type(e->target));
                ns_shader_cstr(dst, "(");
                ns_shader_str(dst, ns_shader_literal_body(tok));
                ns_shader_cstr(dst, ")");
            } else {
                ns_shader_str(dst, ns_shader_literal_body(tok));
            }
            return ns_return_ok_void;
        }
        case NS_TOKEN_IDENTIFIER:
        case NS_TOKEN_TRUE:
        case NS_TOKEN_FALSE:
            ns_shader_str(dst, n->primary_expr.token.val);
            return ns_return_ok_void;
        default: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: string and nil literals are not supported in shader fns.");
        }
    }
    case NS_AST_BINARY_EXPR: {
        ns_type lt = ns_shader_infer(e, n->binary_expr.left);
        ns_type rt = ns_shader_infer(e, n->binary_expr.right);
        if (ns_type_is(lt, NS_TYPE_STRING) || ns_type_is(rt, NS_TYPE_STRING)) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: string operations are not supported in shader fns.");
        }
        // mat4 math needs per-target rewriting (mul() on HLSL); deferred.
        if ((ns_type_is(lt, NS_TYPE_STRUCT) && ns_shader_simd_dim(e->vm->symbols[ns_type_index(lt)].name) == 16) ||
            (ns_type_is(rt, NS_TYPE_STRUCT) && ns_shader_simd_dim(e->vm->symbols[ns_type_index(rt)].name) == 16)) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: mat4 operators are not supported yet.");
        }
        ns_shader_try(ns_shader_emit_expr(e, n->binary_expr.left, dst));
        ns_shader_cstr(dst, " ");
        ns_str op = n->binary_expr.op.val;
        if (ns_str_equals(op, ns_str_cstr("==="))) op = ns_str_cstr("==");
        else if (ns_str_equals(op, ns_str_cstr("!=="))) op = ns_str_cstr("!=");
        ns_shader_str(dst, op);
        ns_shader_cstr(dst, " ");
        return ns_shader_emit_expr(e, n->binary_expr.right, dst);
    }
    case NS_AST_UNARY_EXPR: {
        if (n->unary_expr.op.type == NS_TOKEN_REF) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: ref expressions are not supported in shader fns.");
        }
        ns_shader_str(dst, n->unary_expr.op.val);
        return ns_shader_emit_expr(e, n->unary_expr.expr, dst);
    }
    case NS_AST_MEMBER_EXPR: {
        ns_ast_t *r = &e->ctx->nodes[n->member_expr.right];
        if (r->type != NS_AST_PRIMARY_EXPR) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: unsupported member expression.");
        }
        ns_str field = r->primary_expr.token.val;
        ns_type lt = ns_shader_infer(e, n->member_expr.left);
        // mat4 columns (col0..col3) index the matrix in every target.
        if (ns_type_is(lt, NS_TYPE_STRUCT) && ns_shader_simd_dim(e->vm->symbols[ns_type_index(lt)].name) == 16) {
            if (field.len == 4 && strncmp(field.data, "col", 3) == 0 && field.data[3] >= '0' && field.data[3] <= '3') {
                ns_shader_try(ns_shader_emit_expr(e, n->member_expr.left, dst));
                ns_shader_cstr(dst, "[");
                ns_str_append_len(dst, field.data + 3, 1);
                ns_shader_cstr(dst, "]");
                return ns_return_ok_void;
            }
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: unknown mat4 member.");
        }
        if (ns_str_equals(field, ns_str_cstr("len")) || ns_str_equals(field, ns_str_cstr("size")) || ns_str_equals(field, ns_str_cstr("cap"))) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: container members are not supported in shader fns.");
        }
        ns_shader_try(ns_shader_emit_expr(e, n->member_expr.left, dst));
        ns_shader_cstr(dst, ".");
        ns_shader_str(dst, field);
        return ns_return_ok_void;
    }
    case NS_AST_CALL_EXPR: {
        ns_ast_t *callee = &e->ctx->nodes[n->call_expr.callee];
        if (callee->type != NS_AST_PRIMARY_EXPR) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: only direct fn calls are supported in shader fns.");
        }
        ns_str name = callee->primary_expr.token.val;
        const ns_shader_builtin *b = ns_shader_find_builtin(name);
        if (b) {
            ns_bool transform_position = ns_str_equals(name, ns_str_cstr("shader_transform_position"));
            ns_bool transform_normal = ns_str_equals(name, ns_str_cstr("shader_transform_normal"));
            ns_bool shadow_position = ns_str_equals(name, ns_str_cstr("shader_shadow_clip_position"));
            ns_bool selected = ns_str_equals(name, ns_str_cstr("shader_scene_selected"));
            ns_bool textured = ns_str_equals(name, ns_str_cstr("shader_scene_textured"));
            ns_bool receives_shadow = ns_str_equals(name, ns_str_cstr("shader_scene_receives_shadow"));
            ns_bool sample_texture = ns_str_equals(name, ns_str_cstr("shader_sample_texture"));
            ns_bool global_x = ns_str_equals(name, ns_str_cstr("shader_global_id_x"));
            ns_bool global_y = ns_str_equals(name, ns_str_cstr("shader_global_id_y"));
            ns_bool global_z = ns_str_equals(name, ns_str_cstr("shader_global_id_z"));
            ns_bool write_texture = ns_str_equals(name, ns_str_cstr("shader_write_texture"));
            if (global_x || global_y || global_z) {
                if (n->call_expr.arg_count != 0) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: global id intrinsic takes no arguments.");
                const char component = global_x ? 'x' : global_y ? 'y' : 'z';
                if (e->target == NS_SHADER_GLSL_VULKAN) {
                    ns_shader_cstr(dst, "int(gl_GlobalInvocationID.");
                    ns_str_append_len(dst, (const i8 *)&component, 1);
                    ns_shader_cstr(dst, ")");
                } else {
                    ns_shader_cstr(dst, "int(ns_global_id.");
                    ns_str_append_len(dst, (const i8 *)&component, 1);
                    ns_shader_cstr(dst, ")");
                }
                return ns_return_ok_void;
            }
            if (write_texture) {
                if (n->call_expr.arg_count != 3) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_write_texture expects x, y and float4 color arguments.");
                i32 x = n->next;
                i32 y = e->ctx->nodes[x].next;
                i32 color = e->ctx->nodes[y].next;
                if (e->target == NS_SHADER_MSL) {
                    ns_shader_cstr(dst, "ns_write_texture.write(");
                    ns_shader_try(ns_shader_emit_expr(e, color, dst));
                    ns_shader_cstr(dst, ", uint2(");
                    ns_shader_try(ns_shader_emit_expr(e, x, dst));
                    ns_shader_cstr(dst, ", ");
                    ns_shader_try(ns_shader_emit_expr(e, y, dst));
                    ns_shader_cstr(dst, "))");
                } else if (e->target == NS_SHADER_GLSL_VULKAN) {
                    ns_shader_cstr(dst, "imageStore(ns_write_texture, ivec2(");
                    ns_shader_try(ns_shader_emit_expr(e, x, dst));
                    ns_shader_cstr(dst, ", ");
                    ns_shader_try(ns_shader_emit_expr(e, y, dst));
                    ns_shader_cstr(dst, "), ");
                    ns_shader_try(ns_shader_emit_expr(e, color, dst));
                    ns_shader_cstr(dst, ")");
                } else {
                    ns_shader_cstr(dst, "ns_write_texture[int2(");
                    ns_shader_try(ns_shader_emit_expr(e, x, dst));
                    ns_shader_cstr(dst, ", ");
                    ns_shader_try(ns_shader_emit_expr(e, y, dst));
                    ns_shader_cstr(dst, ")] = ");
                    ns_shader_try(ns_shader_emit_expr(e, color, dst));
                }
                return ns_return_ok_void;
            }
            if (selected || textured || receives_shadow) {
                if (n->call_expr.arg_count != 0) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: scene parameter intrinsic takes no arguments.");
                if (e->target == NS_SHADER_HLSL) {
                    ns_shader_cstr(dst, selected ? "ns_scene_params.x" : textured ? "ns_scene_params.y" : "ns_scene_params.z");
                } else {
                    ns_shader_cstr(dst, selected ? "ns_uniforms.params.x" : textured ? "ns_uniforms.params.y" : "ns_uniforms.params.z");
                }
                return ns_return_ok_void;
            }
            if (transform_position || transform_normal || shadow_position) {
                if (n->call_expr.arg_count != 1) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: transform intrinsic expects one float3 argument.");
                if (e->target == NS_SHADER_HLSL) {
                    if (transform_position) ns_shader_cstr(dst, "mul(ns_view_projection, mul(ns_model, float4(");
                    if (transform_normal) ns_shader_cstr(dst, "normalize((mul(ns_model, float4(");
                    if (shadow_position) ns_shader_cstr(dst, "mul(ns_light_view_projection, mul(ns_model, float4(");
                    ns_shader_try(ns_shader_emit_expr(e, n->next, dst));
                    if (transform_position || shadow_position) ns_shader_cstr(dst, ", 1.0)))");
                    if (transform_normal) ns_shader_cstr(dst, ", 0.0))).xyz)");
                } else {
                    if (transform_position) ns_shader_cstr(dst, "(ns_uniforms.view_projection * (ns_uniforms.model * ");
                    if (transform_normal) ns_shader_cstr(dst, "normalize((ns_uniforms.model * ");
                    if (shadow_position) ns_shader_cstr(dst, "(ns_uniforms.light_view_projection * (ns_uniforms.model * ");
                    ns_shader_cstr(dst, e->target == NS_SHADER_GLSL_VULKAN ? "vec4(" : "float4(");
                    ns_shader_try(ns_shader_emit_expr(e, n->next, dst));
                    if (transform_position || shadow_position) ns_shader_cstr(dst, ", 1.0)))");
                    if (transform_normal) ns_shader_cstr(dst, ", 0.0)).xyz)");
                }
                return ns_return_ok_void;
            }
            if (sample_texture) {
                if (n->call_expr.arg_count != 1) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_sample_texture expects one float2 argument.");
                ns_shader_cstr(dst, e->target == NS_SHADER_MSL ? "ns_texture_sample(ns_texture_map, " : "ns_texture_sample(");
                ns_shader_try(ns_shader_emit_expr(e, n->next, dst));
                ns_shader_cstr(dst, ")");
                return ns_return_ok_void;
            }
            ns_shader_cstr(dst, ns_shader_builtin_name(b, e->target));
            if (ns_str_equals(name, ns_str_cstr("shader_sample_shadow")) && e->target == NS_SHADER_MSL) {
                ns_shader_cstr(dst, "(ns_shadow_map, ");
                if (n->call_expr.arg_count != 1) {
                    return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_sample_shadow expects one float3 argument.");
                }
                ns_shader_try(ns_shader_emit_expr(e, n->next, dst));
                ns_shader_cstr(dst, ")");
                return ns_return_ok_void;
            }
        } else {
            ns_symbol *s = ns_shader_find_global(e->vm, name);
            if (!s || s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || !ns_shader_is_main_tu(s)) {
                snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: cannot call `%.*s` from a shader fn (not a user fn in this file).", name.len, name.data);
                return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
            }
            ns_shader_str(dst, name);
        }
        ns_shader_cstr(dst, "(");
        i32 next = n->next;
        for (i32 a = 0; a < n->call_expr.arg_count; ++a) {
            if (a > 0) ns_shader_cstr(dst, ", ");
            ns_shader_try(ns_shader_emit_expr(e, next, dst));
            next = e->ctx->nodes[next].next;
        }
        ns_shader_cstr(dst, ")");
        return ns_return_ok_void;
    }
    case NS_AST_DESIG_EXPR: return ns_shader_emit_desig(e, n, dst);
    case NS_AST_CAST_EXPR: {
        ns_return_type rt = ns_vm_parse_type_by_token(e->vm, n->cast_expr.type, loc);
        if (ns_return_is_error(rt)) return ns_return_change_type(void, rt);
        ns_type t = rt.r;
        if (!ns_type_is_number(t) && !ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: only numeric casts are supported in shader fns.");
        }
        ns_str tn = {.data = ns_null, .len = 0, .dynamic = true};
        ns_return_void r = ns_shader_type_name(e, t, &tn, loc);
        if (ns_return_is_error(r)) {
            ns_array_free(tn.data);
            return r;
        }
        if (e->target == NS_SHADER_HLSL) {
            ns_shader_cstr(dst, "((");
            ns_shader_str(dst, tn);
            ns_shader_cstr(dst, ")(");
            ns_array_free(tn.data);
            ns_shader_try(ns_shader_emit_expr(e, n->cast_expr.expr, dst));
            ns_shader_cstr(dst, "))");
        } else {
            ns_shader_str(dst, tn);
            ns_array_free(tn.data);
            ns_shader_cstr(dst, "(");
            ns_shader_try(ns_shader_emit_expr(e, n->cast_expr.expr, dst));
            ns_shader_cstr(dst, ")");
        }
        return ns_return_ok_void;
    }
    case NS_AST_STR_FMT_EXPR: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: string formatting is not supported in shader fns.");
    case NS_AST_INDEX_EXPR: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: index expressions are not supported in shader fns yet.");
    case NS_AST_ARRAY_EXPR: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: arrays are not supported in shader fns.");
    case NS_AST_BLOCK_EXPR: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: closures are not supported in shader fns.");
    default: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: unsupported expression in shader fn.");
    }
}

// ---------------------------------------------------------------------------
// statement emission
// ---------------------------------------------------------------------------
static void ns_shader_flush_pre(ns_shader_emit *e) {
    if (e->pre.len > 0) {
        ns_shader_str(&e->out, e->pre);
        ns_array_set_length(e->pre.data, 0);
        e->pre.len = 0;
    }
}

static ns_return_void ns_shader_emit_block(ns_shader_emit *e, i32 i) {
    ns_ast_t *n = &e->ctx->nodes[i];
    if (n->type != NS_AST_COMPOUND_STMT) {
        // single-statement bodies still emit a block for uniform formatting
        ns_shader_cstr(&e->out, "{\n");
        e->indent++;
        ns_shader_try(ns_shader_emit_stmt(e, i));
        e->indent--;
        ns_shader_pad(&e->out, e->indent);
        ns_shader_cstr(&e->out, "}");
        return ns_return_ok_void;
    }
    i32 mark = (i32)ns_array_length(e->locals);
    ns_shader_cstr(&e->out, "{\n");
    e->indent++;
    ns_ast_t *stmt = n;
    for (i32 s = 0; s < n->compound_stmt.count; ++s) {
        ns_shader_try(ns_shader_emit_stmt(e, stmt->next));
        stmt = &e->ctx->nodes[stmt->next];
    }
    e->indent--;
    ns_shader_pad(&e->out, e->indent);
    ns_shader_cstr(&e->out, "}");
    ns_array_set_length(e->locals, mark);
    return ns_return_ok_void;
}

static ns_return_void ns_shader_emit_stmt(ns_shader_emit *e, i32 i) {
    ns_ast_t *n = &e->ctx->nodes[i];
    ns_code_loc loc = ns_shader_loc(e, n);
    switch (n->type) {
    case NS_AST_VAR_DEF: {
        if (n->var_def.is_ref) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: ref bindings are not supported in shader fns.");
        ns_type t = ns_type_unknown;
        if (n->var_def.type != 0) {
            ns_return_type rt = ns_vm_parse_type(e->vm, e->ctx, &e->ctx->nodes[n->var_def.type]);
            if (ns_return_is_error(rt)) return ns_return_change_type(void, rt);
            t = rt.r;
        } else if (n->var_def.expr != 0) {
            t = ns_shader_infer(e, n->var_def.expr);
        }
        if (ns_type_is_unknown(t)) {
            snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: cannot infer the type of `%.*s`; add a type label.", n->var_def.name.val.len,
                     n->var_def.name.val.data);
            return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
        }

        ns_str line = {.data = ns_null, .len = 0, .dynamic = true};
        ns_return_void r = ns_shader_type_name(e, t, &line, loc);
        if (!ns_return_is_error(r)) {
            ns_shader_cstr(&line, " ");
            ns_shader_str(&line, n->var_def.name.val);
            if (n->var_def.expr != 0) {
                ns_shader_cstr(&line, " = ");
                r = ns_shader_emit_expr(e, n->var_def.expr, &line);
            }
        }
        if (ns_return_is_error(r)) {
            ns_array_free(line.data);
            return r;
        }
        ns_shader_flush_pre(e);
        ns_shader_pad(&e->out, e->indent);
        ns_shader_str(&e->out, line);
        ns_shader_cstr(&e->out, ";\n");
        ns_array_free(line.data);
        ns_array_push(e->locals, ((ns_shader_local){.name = n->var_def.name.val, .t = t}));
        return ns_return_ok_void;
    }
    case NS_AST_JUMP_STMT: {
        ns_str label = n->jump_stmt.label.val;
        ns_str line = {.data = ns_null, .len = 0, .dynamic = true};
        ns_shader_str(&line, label);
        if (n->jump_stmt.expr != 0) {
            ns_shader_cstr(&line, " ");
            ns_return_void r = ns_shader_emit_expr(e, n->jump_stmt.expr, &line);
            if (ns_return_is_error(r)) {
                ns_array_free(line.data);
                return r;
            }
        }
        ns_shader_flush_pre(e);
        ns_shader_pad(&e->out, e->indent);
        ns_shader_str(&e->out, line);
        ns_shader_cstr(&e->out, ";\n");
        ns_array_free(line.data);
        return ns_return_ok_void;
    }
    case NS_AST_IF_STMT: {
        ns_str cond = {.data = ns_null, .len = 0, .dynamic = true};
        ns_return_void r = ns_shader_emit_expr(e, n->if_stmt.condition, &cond);
        if (ns_return_is_error(r)) {
            ns_array_free(cond.data);
            return r;
        }
        ns_shader_flush_pre(e);
        ns_shader_pad(&e->out, e->indent);
        ns_shader_cstr(&e->out, "if (");
        ns_shader_str(&e->out, cond);
        ns_shader_cstr(&e->out, ") ");
        ns_array_free(cond.data);
        ns_shader_try(ns_shader_emit_block(e, n->if_stmt.body));
        if (n->if_stmt.else_body != 0) {
            ns_shader_cstr(&e->out, " else ");
            ns_shader_try(ns_shader_emit_block(e, n->if_stmt.else_body));
        }
        ns_shader_cstr(&e->out, "\n");
        return ns_return_ok_void;
    }
    case NS_AST_FOR_STMT: {
        ns_ast_t *gen = &e->ctx->nodes[n->for_stmt.generator];
        if (gen->type != NS_AST_GEN_EXPR || !gen->gen_expr.range) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: only `for i in a to b` range loops are supported in shader fns.");
        }
        ns_str from = {.data = ns_null, .len = 0, .dynamic = true};
        ns_str to = {.data = ns_null, .len = 0, .dynamic = true};
        ns_return_void r = ns_shader_emit_expr(e, gen->gen_expr.from, &from);
        if (!ns_return_is_error(r)) r = ns_shader_emit_expr(e, gen->gen_expr.to, &to);
        if (ns_return_is_error(r)) {
            ns_array_free(from.data);
            ns_array_free(to.data);
            return r;
        }
        ns_str name = gen->gen_expr.name.val;
        ns_shader_flush_pre(e);
        ns_shader_pad(&e->out, e->indent);
        ns_shader_cstr(&e->out, "for (int ");
        ns_shader_str(&e->out, name);
        ns_shader_cstr(&e->out, " = ");
        ns_shader_str(&e->out, from);
        ns_shader_cstr(&e->out, "; ");
        ns_shader_str(&e->out, name);
        ns_shader_cstr(&e->out, " < ");
        ns_shader_str(&e->out, to);
        ns_shader_cstr(&e->out, "; ++");
        ns_shader_str(&e->out, name);
        ns_shader_cstr(&e->out, ") ");
        ns_array_free(from.data);
        ns_array_free(to.data);
        i32 mark = (i32)ns_array_length(e->locals);
        ns_array_push(e->locals, ((ns_shader_local){.name = name, .t = ns_type_i32}));
        ns_shader_try(ns_shader_emit_block(e, n->for_stmt.body));
        ns_array_set_length(e->locals, mark);
        ns_shader_cstr(&e->out, "\n");
        return ns_return_ok_void;
    }
    case NS_AST_LOOP_STMT: {
        ns_str cond = {.data = ns_null, .len = 0, .dynamic = true};
        ns_return_void r = ns_shader_emit_expr(e, n->loop_stmt.condition, &cond);
        if (ns_return_is_error(r)) {
            ns_array_free(cond.data);
            return r;
        }
        ns_shader_flush_pre(e);
        ns_shader_pad(&e->out, e->indent);
        if (n->loop_stmt.do_first) {
            ns_shader_cstr(&e->out, "do ");
            ns_return_void rb = ns_shader_emit_block(e, n->loop_stmt.body);
            if (ns_return_is_error(rb)) {
                ns_array_free(cond.data);
                return rb;
            }
            ns_shader_cstr(&e->out, " while (");
            ns_shader_str(&e->out, cond);
            ns_shader_cstr(&e->out, ");\n");
        } else {
            ns_shader_cstr(&e->out, "while (");
            ns_shader_str(&e->out, cond);
            ns_shader_cstr(&e->out, ") ");
            ns_return_void rb = ns_shader_emit_block(e, n->loop_stmt.body);
            if (ns_return_is_error(rb)) {
                ns_array_free(cond.data);
                return rb;
            }
            ns_shader_cstr(&e->out, "\n");
        }
        ns_array_free(cond.data);
        return ns_return_ok_void;
    }
    case NS_AST_COMPOUND_STMT: {
        ns_shader_pad(&e->out, e->indent);
        ns_shader_try(ns_shader_emit_block(e, i));
        ns_shader_cstr(&e->out, "\n");
        return ns_return_ok_void;
    }
    case NS_AST_ASSERT_STMT: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: assert is not supported in shader fns.");
    case NS_AST_FN_DEF:
    case NS_AST_STRUCT_DEF: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: nested definitions are not supported in shader fns.");
    default: { // expression statement (assignment, call, ...)
        ns_str line = {.data = ns_null, .len = 0, .dynamic = true};
        ns_return_void r = ns_shader_emit_expr(e, i, &line);
        if (ns_return_is_error(r)) {
            ns_array_free(line.data);
            return r;
        }
        ns_shader_flush_pre(e);
        ns_shader_pad(&e->out, e->indent);
        ns_shader_str(&e->out, line);
        ns_shader_cstr(&e->out, ";\n");
        ns_array_free(line.data);
        return ns_return_ok_void;
    }
    }
}

// ---------------------------------------------------------------------------
// struct emission with per-target stage annotations
// ---------------------------------------------------------------------------
static const char *ns_shader_hlsl_input_semantic(ns_str name, i32 *texcoord) {
    if (ns_str_equals(name, ns_str_cstr("position"))) return "POSITION";
    if (ns_str_equals(name, ns_str_cstr("normal"))) return "NORMAL";
    if (ns_str_equals(name, ns_str_cstr("tangent"))) return "TANGENT";
    if (ns_str_equals(name, ns_str_cstr("color"))) return "COLOR";
    ns_unused(texcoord);
    return ns_null; // TEXCOORD{n}, appended by the caller
}

static ns_return_void ns_shader_emit_struct(ns_shader_emit *e, i32 st_index) {
    ns_symbol *s = &e->vm->symbols[st_index];
    // A struct imported from a lib module has its ast index in that module's
    // transient parse ctx, so the node cannot be dereferenced here; its fields
    // (names, types) live in vm->symbols and are all the emission needs.
    ns_code_loc loc = ns_shader_is_main_tu(s) ? ns_shader_loc(e, &e->ctx->nodes[s->st.ast]) : ns_code_loc_nil;
    ns_bool is_vs_in = ns_shader_index_in(e->vs_inputs, st_index);
    ns_bool is_io = ns_shader_index_in(e->stage_ios, st_index);

    ns_shader_cstr(&e->out, "struct ");
    ns_shader_str(&e->out, s->name);
    ns_shader_cstr(&e->out, " {\n");
    i32 texcoord = 0;
    for (i32 f = 0, l = (i32)ns_array_length(s->st.fields); f < l; ++f) {
        ns_struct_field *field = &s->st.fields[f];
        ns_shader_cstr(&e->out, "    ");
        ns_shader_try(ns_shader_type_name(e, field->t, &e->out, loc));
        ns_shader_cstr(&e->out, " ");
        ns_shader_str(&e->out, field->name);
        if (e->target == NS_SHADER_MSL) {
            if (is_vs_in) {
                ns_shader_cstr(&e->out, " [[attribute(");
                ns_shader_i32(&e->out, f);
                ns_shader_cstr(&e->out, ")]]");
            } else if (is_io && ns_shader_is_position_field(e->vm, field)) {
                ns_shader_cstr(&e->out, " [[position]]");
            }
        } else if (e->target == NS_SHADER_HLSL) {
            if (is_vs_in) {
                const char *sem = ns_shader_hlsl_input_semantic(field->name, &texcoord);
                ns_shader_cstr(&e->out, " : ");
                if (sem) {
                    ns_shader_cstr(&e->out, sem);
                } else {
                    ns_shader_cstr(&e->out, "TEXCOORD");
                    ns_shader_i32(&e->out, texcoord++);
                }
            } else if (is_io) {
                if (ns_shader_is_position_field(e->vm, field)) {
                    ns_shader_cstr(&e->out, " : SV_Position");
                } else {
                    ns_shader_cstr(&e->out, " : TEXCOORD");
                    ns_shader_i32(&e->out, texcoord++);
                }
            }
        }
        ns_shader_cstr(&e->out, ";\n");
    }
    ns_shader_cstr(&e->out, "};\n\n");
    return ns_return_ok_void;
}

// ---------------------------------------------------------------------------
// fn emission
// ---------------------------------------------------------------------------
static ns_return_void ns_shader_emit_fn(ns_shader_emit *e, i32 fn_index, ns_shader_stage stage) {
    ns_symbol *s = &e->vm->symbols[fn_index];
    ns_ast_t *fn_node = &e->ctx->nodes[s->fn.ast];
    ns_code_loc loc = ns_shader_loc(e, fn_node);

    ns_array_set_length(e->locals, 0);
    for (i32 a = 0, l = (i32)ns_array_length(s->fn.args); a < l; ++a) {
        ns_array_push(e->locals, ((ns_shader_local){.name = s->fn.args[a].name, .t = s->fn.args[a].val.t}));
    }

    if (e->target == NS_SHADER_MSL) {
        if (stage == NS_SHADER_STAGE_VERTEX) ns_shader_cstr(&e->out, "vertex ");
        if (stage == NS_SHADER_STAGE_FRAGMENT) ns_shader_cstr(&e->out, "fragment ");
        if (stage == NS_SHADER_STAGE_COMPUTE) ns_shader_cstr(&e->out, "kernel ");
    }
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_COMPUTE) {
        ns_shader_cstr(&e->out, "[numthreads(1, 1, 1)]\n");
    }
    ns_shader_try(ns_shader_type_name(e, s->fn.ret, &e->out, loc));
    ns_shader_cstr(&e->out, " ");
    ns_shader_str(&e->out, s->name);
    ns_shader_cstr(&e->out, "(");
    for (i32 a = 0, l = (i32)ns_array_length(s->fn.args); a < l; ++a) {
        if (a > 0) ns_shader_cstr(&e->out, ", ");
        ns_shader_try(ns_shader_type_name(e, s->fn.args[a].val.t, &e->out, loc));
        ns_shader_cstr(&e->out, " ");
        ns_shader_str(&e->out, s->fn.args[a].name);
        if (e->target == NS_SHADER_MSL && stage != NS_SHADER_STAGE_AUTO) ns_shader_cstr(&e->out, " [[stage_in]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_global_id) {
        ns_shader_cstr(&e->out, "uint3 ns_global_id [[thread_position_in_grid]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_write_texture) {
        if (e->uses_global_id) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "texture2d<float, access::write> ns_write_texture [[texture(0)]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_FRAGMENT && e->uses_shadow_map) {
        ns_shader_cstr(&e->out, ", depth2d<float> ns_shadow_map [[texture(0)]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_FRAGMENT && e->uses_texture_map) {
        ns_shader_cstr(&e->out, ", texture2d<float> ns_texture_map [[texture(1)]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_VERTEX && e->uses_scene_uniforms) {
        ns_shader_cstr(&e->out, ", constant ns_scene_uniforms& ns_uniforms [[buffer(1)]]");
    }
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_global_id) {
        ns_shader_cstr(&e->out, "uint3 ns_global_id : SV_DispatchThreadID");
    }
    ns_shader_cstr(&e->out, ")");
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_FRAGMENT) ns_shader_cstr(&e->out, " : SV_Target");
    ns_shader_cstr(&e->out, " ");
    e->indent = 0;
    ns_shader_try(ns_shader_emit_block(e, s->fn.body));
    ns_shader_cstr(&e->out, "\n\n");
    return ns_return_ok_void;
}

// GLSL entry wrapper: flatten struct IO to layout(location=N) globals and call
// the ns fn (emitted verbatim as an ordinary GLSL function) from `void main()`.
static ns_return_void ns_shader_emit_glsl_wrapper(ns_shader_emit *e, ns_shader_entry_desc *entry) {
    ns_symbol *s = &e->vm->symbols[entry->fn_index];
    ns_ast_t *fn_node = &e->ctx->nodes[s->fn.ast];
    ns_code_loc loc = ns_shader_loc(e, fn_node);
    if (entry->stage == NS_SHADER_STAGE_COMPUTE) {
        ns_shader_cstr(&e->out, "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n\nvoid main() {\n    ");
        ns_shader_str(&e->out, s->name);
        ns_shader_cstr(&e->out, "();\n}\n");
        return ns_return_ok_void;
    }
    ns_symbol *in_st = &e->vm->symbols[ns_type_index(s->fn.args[0].val.t)];

    if (entry->stage == NS_SHADER_STAGE_VERTEX) {
        ns_symbol *io_st = &e->vm->symbols[ns_type_index(s->fn.ret)];
        for (i32 f = 0, l = (i32)ns_array_length(in_st->st.fields); f < l; ++f) {
            ns_shader_cstr(&e->out, "layout(location = ");
            ns_shader_i32(&e->out, f);
            ns_shader_cstr(&e->out, ") in ");
            ns_shader_try(ns_shader_type_name(e, in_st->st.fields[f].t, &e->out, loc));
            ns_shader_cstr(&e->out, " ns_in_");
            ns_shader_str(&e->out, in_st->st.fields[f].name);
            ns_shader_cstr(&e->out, ";\n");
        }
        i32 out_loc = 0;
        for (i32 f = 0, l = (i32)ns_array_length(io_st->st.fields); f < l; ++f) {
            if (ns_shader_is_position_field(e->vm, &io_st->st.fields[f])) continue;
            ns_shader_cstr(&e->out, "layout(location = ");
            ns_shader_i32(&e->out, out_loc++);
            ns_shader_cstr(&e->out, ") out ");
            ns_shader_try(ns_shader_type_name(e, io_st->st.fields[f].t, &e->out, loc));
            ns_shader_cstr(&e->out, " ns_out_");
            ns_shader_str(&e->out, io_st->st.fields[f].name);
            ns_shader_cstr(&e->out, ";\n");
        }
        ns_shader_cstr(&e->out, "\nvoid main() {\n    ");
        ns_shader_str(&e->out, in_st->name);
        ns_shader_cstr(&e->out, " ns_in = ");
        ns_shader_str(&e->out, in_st->name);
        ns_shader_cstr(&e->out, "(");
        for (i32 f = 0, l = (i32)ns_array_length(in_st->st.fields); f < l; ++f) {
            if (f > 0) ns_shader_cstr(&e->out, ", ");
            ns_shader_cstr(&e->out, "ns_in_");
            ns_shader_str(&e->out, in_st->st.fields[f].name);
        }
        ns_shader_cstr(&e->out, ");\n    ");
        ns_shader_str(&e->out, io_st->name);
        ns_shader_cstr(&e->out, " ns_ret = ");
        ns_shader_str(&e->out, s->name);
        ns_shader_cstr(&e->out, "(ns_in);\n    gl_Position = ns_ret.position;\n");
        for (i32 f = 0, l = (i32)ns_array_length(io_st->st.fields); f < l; ++f) {
            if (ns_shader_is_position_field(e->vm, &io_st->st.fields[f])) continue;
            ns_shader_cstr(&e->out, "    ns_out_");
            ns_shader_str(&e->out, io_st->st.fields[f].name);
            ns_shader_cstr(&e->out, " = ns_ret.");
            ns_shader_str(&e->out, io_st->st.fields[f].name);
            ns_shader_cstr(&e->out, ";\n");
        }
        ns_shader_cstr(&e->out, "}\n");
    } else {
        // fragment: varyings in, one color out; the stage-io position field (if
        // any) is fed from gl_FragCoord (window coords).
        i32 in_loc = 0;
        for (i32 f = 0, l = (i32)ns_array_length(in_st->st.fields); f < l; ++f) {
            if (ns_shader_is_position_field(e->vm, &in_st->st.fields[f])) continue;
            ns_shader_cstr(&e->out, "layout(location = ");
            ns_shader_i32(&e->out, in_loc++);
            ns_shader_cstr(&e->out, ") in ");
            ns_shader_try(ns_shader_type_name(e, in_st->st.fields[f].t, &e->out, loc));
            ns_shader_cstr(&e->out, " ns_in_");
            ns_shader_str(&e->out, in_st->st.fields[f].name);
            ns_shader_cstr(&e->out, ";\n");
        }
        ns_shader_cstr(&e->out, "layout(location = 0) out vec4 ns_frag_color;\n");
        ns_shader_cstr(&e->out, "\nvoid main() {\n    ");
        ns_shader_str(&e->out, in_st->name);
        ns_shader_cstr(&e->out, " ns_in = ");
        ns_shader_str(&e->out, in_st->name);
        ns_shader_cstr(&e->out, "(");
        for (i32 f = 0, l = (i32)ns_array_length(in_st->st.fields); f < l; ++f) {
            if (f > 0) ns_shader_cstr(&e->out, ", ");
            if (ns_shader_is_position_field(e->vm, &in_st->st.fields[f])) {
                ns_shader_cstr(&e->out, "gl_FragCoord");
            } else {
                ns_shader_cstr(&e->out, "ns_in_");
                ns_shader_str(&e->out, in_st->st.fields[f].name);
            }
        }
        ns_shader_cstr(&e->out, ");\n    ns_frag_color = ");
        ns_shader_str(&e->out, s->name);
        ns_shader_cstr(&e->out, "(ns_in);\n}\n");
    }
    return ns_return_ok_void;
}

// ---------------------------------------------------------------------------
// stage inference and entry validation
// ---------------------------------------------------------------------------
ns_shader_stage ns_shader_stage_infer(ns_vm *vm, ns_ast_ctx *ctx, i32 fn_index) {
    ns_unused(ctx);
    if (fn_index < 0 || fn_index >= (i32)ns_array_length(vm->symbols)) return NS_SHADER_STAGE_AUTO;
    ns_symbol *s = &vm->symbols[fn_index];
    if (s->type != NS_SYMBOL_FN) return NS_SHADER_STAGE_AUTO;
    if (ns_str_starts_with(s->name, ns_str_cstr("vs_")) || ns_str_equals(s->name, ns_str_cstr("vs"))) return NS_SHADER_STAGE_VERTEX;
    if (ns_str_starts_with(s->name, ns_str_cstr("fs_")) || ns_str_starts_with(s->name, ns_str_cstr("ps_")) || ns_str_equals(s->name, ns_str_cstr("fs")))
        return NS_SHADER_STAGE_FRAGMENT;
    if (ns_str_starts_with(s->name, ns_str_cstr("cs_")) || ns_str_equals(s->name, ns_str_cstr("cs"))) return NS_SHADER_STAGE_COMPUTE;

    ns_type ret = s->fn.ret;
    if (!ns_type_is(ret, NS_TYPE_STRUCT)) return NS_SHADER_STAGE_AUTO;
    ns_symbol *rs = &vm->symbols[ns_type_index(ret)];
    if (ns_shader_is_simd(rs)) {
        return ns_shader_simd_dim(rs->name) == 4 ? NS_SHADER_STAGE_FRAGMENT : NS_SHADER_STAGE_AUTO;
    }
    for (i32 f = 0, l = (i32)ns_array_length(rs->st.fields); f < l; ++f) {
        if (ns_shader_is_position_field(vm, &rs->st.fields[f])) return NS_SHADER_STAGE_VERTEX;
    }
    return NS_SHADER_STAGE_AUTO;
}

// Validate an entry signature for its stage and record struct roles.
static ns_return_void ns_shader_classify_entry(ns_shader_emit *e, ns_shader_entry_desc *entry) {
    ns_symbol *s = &e->vm->symbols[entry->fn_index];
    ns_ast_t *fn_node = &e->ctx->nodes[s->fn.ast];
    ns_code_loc loc = ns_shader_loc(e, fn_node);

    if (entry->stage == NS_SHADER_STAGE_COMPUTE) {
        if (ns_array_length(s->fn.args) != 0 || !ns_type_is(s->fn.ret, NS_TYPE_VOID)) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: a compute entry must take no parameters and return void.");
        }
        return ns_return_ok_void;
    }

    if ((i32)ns_array_length(s->fn.args) != 1 || !ns_type_is(s->fn.args[0].val.t, NS_TYPE_STRUCT)) {
        return ns_return_error(void, loc, NS_ERR_EVAL, "shader: an entry fn must take exactly one struct parameter.");
    }
    i32 in_index = (i32)ns_type_index(s->fn.args[0].val.t);
    if (ns_shader_is_simd(&e->vm->symbols[in_index])) {
        return ns_return_error(void, loc, NS_ERR_EVAL, "shader: an entry fn parameter must be a user struct, not a simd type.");
    }

    if (entry->stage == NS_SHADER_STAGE_VERTEX) {
        if (!ns_type_is(s->fn.ret, NS_TYPE_STRUCT) || ns_shader_is_simd(&e->vm->symbols[ns_type_index(s->fn.ret)])) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: a vertex entry must return a user struct (the stage io).");
        }
        i32 io_index = (i32)ns_type_index(s->fn.ret);
        ns_symbol *io = &e->vm->symbols[io_index];
        ns_bool has_position = false;
        for (i32 f = 0, l = (i32)ns_array_length(io->st.fields); f < l; ++f) {
            if (ns_shader_is_position_field(e->vm, &io->st.fields[f])) has_position = true;
        }
        if (!has_position) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: a vertex entry's return struct needs a `position: float4` field.");
        }
        if (!ns_shader_index_in(e->vs_inputs, in_index)) ns_array_push(e->vs_inputs, in_index);
        if (!ns_shader_index_in(e->stage_ios, io_index)) ns_array_push(e->stage_ios, io_index);
    } else {
        ns_bool ret_ok = ns_type_is(s->fn.ret, NS_TYPE_STRUCT) && ns_shader_is_simd(&e->vm->symbols[ns_type_index(s->fn.ret)]) &&
                         ns_shader_simd_dim(e->vm->symbols[ns_type_index(s->fn.ret)].name) == 4;
        if (!ret_ok) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: a fragment entry must return float4.");
        if (!ns_shader_index_in(e->stage_ios, in_index)) ns_array_push(e->stage_ios, in_index);
    }
    return ns_return_ok_void;
}

// ---------------------------------------------------------------------------
// program transpile
// ---------------------------------------------------------------------------
ns_return_str ns_shader_transpile_program(ns_vm *vm, ns_ast_ctx *ctx, ns_shader_entry_desc *entries, i32 count, ns_shader_target target) {
    if (target == NS_SHADER_TARGET_UNKNOWN) {
        return ns_return_error(str, ns_code_loc_nil, NS_ERR_EVAL, "shader: unknown target, expected msl | glsl | hlsl.");
    }
    if (count <= 0) return ns_return_error(str, ns_code_loc_nil, NS_ERR_EVAL, "shader: no entry fns to transpile.");
    if (target == NS_SHADER_GLSL_VULKAN && count > 1) {
        return ns_return_error(str, ns_code_loc_nil, NS_ERR_EVAL, "shader: glsl emits one source per stage; transpile entries one at a time.");
    }

    ns_shader_emit e = {0};
    e.vm = vm;
    e.ctx = ctx;
    e.target = target;
    e.out = (ns_str){.data = ns_null, .len = 0, .dynamic = true};
    e.pre = (ns_str){.data = ns_null, .len = 0, .dynamic = true};

#define ns_shader_fail(r)                                                                                                                            \
    do {                                                                                                                                             \
        ns_array_free(e.out.data);                                                                                                                   \
        ns_array_free(e.pre.data);                                                                                                                   \
        ns_array_free(e.structs);                                                                                                                    \
        ns_array_free(e.fns);                                                                                                                        \
        ns_array_free(e.fn_visit);                                                                                                                   \
        ns_array_free(e.entries);                                                                                                                    \
        ns_array_free(e.vs_inputs);                                                                                                                  \
        ns_array_free(e.stage_ios);                                                                                                                  \
        ns_array_free(e.locals);                                                                                                                     \
        return ns_return_change_type(str, r);                                                                                                        \
    } while (0)

    // resolve stages, validate entries, classify struct roles
    for (i32 i = 0; i < count; ++i) {
        ns_shader_entry_desc entry = entries[i];
        if (entry.fn_index < 0 || entry.fn_index >= (i32)ns_array_length(vm->symbols)) {
            ns_return_void r = ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: invalid fn.");
            ns_shader_fail(r);
        }
        ns_symbol *s = &vm->symbols[entry.fn_index];
        if (s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || s->fn.body == 0 || !ns_shader_is_main_tu(s)) {
            snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: `%.*s` is not a transpilable fn (must be a non-ref fn defined in this file).",
                     s->name.len, s->name.data);
            ns_return_void r = ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, ns_shader_err);
            ns_shader_fail(r);
        }
        if (entry.stage == NS_SHADER_STAGE_AUTO) entry.stage = ns_shader_stage_infer(vm, ctx, entry.fn_index);
        if (entry.stage == NS_SHADER_STAGE_AUTO) {
            snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: cannot infer the stage of `%.*s`; name it vs_*/fs_* or pass the stage explicitly.",
                     s->name.len, s->name.data);
            ns_return_void r = ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, ns_shader_err);
            ns_shader_fail(r);
        }
        ns_array_push(e.entries, entry);
        ns_return_void rc = ns_shader_classify_entry(&e, ns_array_last(e.entries));
        if (ns_return_is_error(rc)) ns_shader_fail(rc);
    }

    // a struct cannot be both a vertex input and the stage io (annotations clash)
    for (i32 i = 0, l = (i32)ns_array_length(e.vs_inputs); i < l; ++i) {
        if (ns_shader_index_in(e.stage_ios, e.vs_inputs[i])) {
            ns_return_void r =
                ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: the same struct cannot be both a vertex input and the stage io.");
            ns_shader_fail(r);
        }
    }

    // collect struct/helper-fn dependencies
    for (i32 i = 0, l = (i32)ns_array_length(e.entries); i < l; ++i) {
        ns_return_void r = ns_shader_collect_fn(&e, e.entries[i].fn_index, true, 0);
        if (ns_return_is_error(r)) ns_shader_fail(r);
    }

    // header
    ns_shader_cstr(&e.out, "// generated by ns_shader (");
    ns_shader_str(&e.out, ns_shader_target_name(target));
    ns_shader_cstr(&e.out, ")\n");
    if (target == NS_SHADER_MSL) ns_shader_cstr(&e.out, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    if (target == NS_SHADER_GLSL_VULKAN) ns_shader_cstr(&e.out, "#version 450\n\n");
    if (target == NS_SHADER_HLSL) ns_shader_cstr(&e.out, "\n");
    if (e.uses_write_texture && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out, "layout(set = 0, binding = 0, rgba8) uniform writeonly image2D ns_write_texture;\n\n");
    }
    if (e.uses_write_texture && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out, "RWTexture2D<float4> ns_write_texture : register(u0);\n\n");
    }
    if (e.uses_scene_uniforms && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out,
            "struct ns_scene_uniforms {\n"
            "    float4x4 model;\n"
            "    float4x4 view_projection;\n"
            "    float4x4 light_view_projection;\n"
            "    float4 params;\n"
            "};\n\n");
    }
    if (e.uses_scene_uniforms && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out,
            "layout(set = 0, binding = 2, std140) uniform ns_scene_uniform_block {\n"
            "    mat4 model; mat4 view_projection; mat4 light_view_projection; vec4 params;\n"
            "} ns_uniforms;\n\n");
    }
    if (e.uses_scene_uniforms && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out,
            "cbuffer ns_uniforms : register(b0) {\n"
            "    column_major float4x4 ns_model;\n"
            "    column_major float4x4 ns_view_projection;\n"
            "    column_major float4x4 ns_light_view_projection;\n"
            "    float4 ns_scene_params;\n"
            "};\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out,
            "inline float ns_shadow_compare(depth2d<float> map, float3 coord) {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) return 1.0;\n"
            "    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear, compare_func::less_equal);\n"
            "    float2 texel = 1.0 / float2(map.get_width(), map.get_height());\n"
            "    float lit = 0.0;\n"
            "    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
            "        lit += map.sample_compare(s, coord.xy + float2(x, y) * texel, coord.z - 0.005);\n"
            "    return lit / 9.0;\n"
            "}\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out,
            "layout(set = 0, binding = 0) uniform sampler2DShadow ns_shadow_map;\n"
            "float ns_shadow_compare(vec3 coord) {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) return 1.0;\n"
            "    vec2 texel = 1.0 / vec2(textureSize(ns_shadow_map, 0)); float lit = 0.0;\n"
            "    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
            "        lit += texture(ns_shadow_map, vec3(coord.xy + vec2(x, y) * texel, coord.z - 0.005));\n"
            "    return lit / 9.0;\n"
            "}\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out,
            "Texture2D<float> ns_shadow_map : register(t0);\n"
            "SamplerComparisonState ns_shadow_sampler : register(s0);\n"
            "float ns_shadow_compare(float3 coord) {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) return 1.0;\n"
            "    uint w, h; ns_shadow_map.GetDimensions(w, h); float lit = 0.0;\n"
            "    float2 texel = 1.0 / float2(w, h);\n"
            "    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)\n"
            "        lit += ns_shadow_map.SampleCmpLevelZero(ns_shadow_sampler, coord.xy + float2(x, y) * texel, coord.z - 0.005);\n"
            "    return lit / 9.0;\n"
            "}\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out,
            "inline float4 ns_texture_sample(texture2d<float> map, float2 coord) {\n"
            "    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);\n"
            "    return map.sample(s, coord);\n"
            "}\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out,
            "layout(set = 0, binding = 1) uniform sampler2D ns_texture_map;\n"
            "vec4 ns_texture_sample(vec2 coord) { return texture(ns_texture_map, coord); }\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out,
            "Texture2D<float4> ns_texture_map : register(t1);\n"
            "float4 ns_texture_sample(float2 coord) {\n"
            "    uint w, h; ns_texture_map.GetDimensions(w, h);\n"
            "    return ns_texture_map.Load(int3(int2(saturate(coord) * float2(w - 1, h - 1)), 0));\n"
            "}\n\n");
    }

    // structs (dependency order), helper fns (callees first), entries
    for (i32 i = 0, l = (i32)ns_array_length(e.structs); i < l; ++i) {
        ns_return_void r = ns_shader_emit_struct(&e, e.structs[i]);
        if (ns_return_is_error(r)) ns_shader_fail(r);
    }
    for (i32 i = 0, l = (i32)ns_array_length(e.fns); i < l; ++i) {
        ns_return_void r = ns_shader_emit_fn(&e, e.fns[i], NS_SHADER_STAGE_AUTO);
        if (ns_return_is_error(r)) ns_shader_fail(r);
    }
    for (i32 i = 0, l = (i32)ns_array_length(e.entries); i < l; ++i) {
        // GLSL entries are plain fns called from the generated main() wrapper
        ns_shader_stage adorn = target == NS_SHADER_GLSL_VULKAN ? NS_SHADER_STAGE_AUTO : e.entries[i].stage;
        ns_return_void r = ns_shader_emit_fn(&e, e.entries[i].fn_index, adorn);
        if (ns_return_is_error(r)) ns_shader_fail(r);
        if (target == NS_SHADER_GLSL_VULKAN) {
            r = ns_shader_emit_glsl_wrapper(&e, &e.entries[i]);
            if (ns_return_is_error(r)) ns_shader_fail(r);
        }
    }
#undef ns_shader_fail

    ns_str out = e.out;
    ns_array_free(e.pre.data);
    ns_array_free(e.structs);
    ns_array_free(e.fns);
    ns_array_free(e.fn_visit);
    ns_array_free(e.entries);
    ns_array_free(e.vs_inputs);
    ns_array_free(e.stage_ios);
    ns_array_free(e.locals);
    return ns_return_ok(str, out);
}

ns_return_str ns_shader_transpile(ns_vm *vm, ns_ast_ctx *ctx, i32 fn_index, ns_shader_target target, ns_shader_stage stage) {
    ns_shader_entry_desc entry = {.fn_index = fn_index, .stage = stage};
    return ns_shader_transpile_program(vm, ctx, &entry, 1, target);
}

// ---------------------------------------------------------------------------
// `mod shader` intrinsic dispatch (mirrors ns_vm_call_std)
// ---------------------------------------------------------------------------
ns_return_bool ns_shader_vm_call(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_str name = call->callee->name;

    ns_bool is_transpile = ns_str_equals(name, ns_str_cstr("shader_transpile"));
    ns_bool is_transpile_stage = ns_str_equals(name, ns_str_cstr("shader_transpile_stage"));
    ns_bool is_entry = ns_str_equals(name, ns_str_cstr("shader_entry"));
    if (!is_transpile && !is_transpile_stage && !is_entry) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "unknown shader fn.");
    }

    ns_value fv = vm->symbol_stack[call->arg_offset].val;
    if (!ns_type_is(fv.t, NS_TYPE_FN)) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: the first argument must be a fn.");
    }
    i32 fn_index = (i32)ns_type_index(fv.t);
    ns_str target_s = ns_eval_str(vm, vm->symbol_stack[call->arg_offset + 1].val);
    ns_shader_target target = ns_shader_target_from_str(target_s);

    if (is_entry) {
        ns_str fn_name = vm->symbols[fn_index].name;
        call->ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, ns_shader_entry_name(target, fn_name))};
        return ns_return_ok(bool, true);
    }

    ns_shader_stage stage = NS_SHADER_STAGE_AUTO;
    if (is_transpile_stage) {
        ns_str stage_s = ns_eval_str(vm, vm->symbol_stack[call->arg_offset + 2].val);
        if (ns_str_equals(stage_s, ns_str_cstr("vertex"))) stage = NS_SHADER_STAGE_VERTEX;
        else if (ns_str_equals(stage_s, ns_str_cstr("fragment"))) stage = NS_SHADER_STAGE_FRAGMENT;
        else if (ns_str_equals(stage_s, ns_str_cstr("compute"))) stage = NS_SHADER_STAGE_COMPUTE;
        else return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: unknown stage, expected vertex | fragment | compute.");
    }

    ns_return_str src = ns_shader_transpile(vm, ctx, fn_index, target, stage);
    if (ns_return_is_error(src)) return ns_return_change_type(bool, src);
    call->ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, src.r)};
    ns_array_free(src.r.data);
    return ns_return_ok(bool, true);
}
