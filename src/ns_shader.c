#include "ns_shader.h"

// ns fn -> shader source transcoder. The emitter walks the fn-body AST directly
// (like ns_ast_print.c) because shader source needs structured control flow;
// type facts come from the VM symbol table populated by ns_vm_parse. Only the
// shader subset of ns is accepted: scalars, simd vectors (float2/3/4 and mat4), user
// structs, arithmetic/logic, if/for/loop and calls to other user fns. Anything
// else produces a source-located error instead of silently emitting bad code.
//
// SIMD: float2/3/4 map to native vectors. Binary operators stay operators in
// the AST (`a + b`, `v * s`) and are emitted as native SIMD ops. Calls whose
// names are in ns_shader_builtins (dot, cross, normalize, length, mix, min,
// max, clamp, abs, ddx, ddy, ...) become the target's matching intrinsic rather than
// the host-side simd.ns body. Swizzles such as `.xyz` pass through as-is.

#define NS_SHADER_MAX_DEPTH 64
// Local arrays live in a shader's register/stack budget, so the length stays
// small enough that every backend can hold one.
#define NS_SHADER_MAX_ARRAY_LEN 256
#define NS_SHADER_STORAGE_BINDING_BASE 3
#define NS_SHADER_WGSL_STORAGE_BINDING_BASE 7
// The root words the CPU shader host holds for `shader_host_root`.
#define NS_SHADER_ROOT_WORDS 16
// The root block a generated shader declares, in float4s. Metal takes the root
// as a pointer and reads whatever the program uploaded, so a Metal shader has
// never been bounded by a declared length. Every other backend declares the
// block as a fixed-size uniform array, and an index past its end is clamped to
// the last element rather than reported - a program whose root outgrew the
// declaration then reads a neighbouring word and draws with it. The declared
// block therefore covers the largest root any pass uploads: 256 float4s is 4 KB,
// well inside the 64 KB uniform binding every device guarantees, and the runtime
// pads a root allocation out to the same size so the binding stays valid.
#define NS_SHADER_ROOT_BLOCK_VEC4S 256

static char ns_shader_err[512];

typedef struct ns_shader_local {
    ns_str name;
    ns_type t;
    // Length of a fixed-capacity local array, 0 for every other binding.
    i32 array_len;
} ns_shader_local;

// Resources a fn reaches through the stage intrinsics. GLSL and WGSL declare
// them at module scope, so any fn can name them; MSL binds them as entry-point
// parameters, and HLSL binds the invocation ids as entry semantics. A helper
// fn therefore receives what it (or anything it calls) uses as extra
// parameters, and its call sites pass them along.
enum ns_shader_use {
    NS_SHADER_USE_GLOBAL_ID = 1 << 0,
    NS_SHADER_USE_VERTEX_ID = 1 << 1,
    NS_SHADER_USE_WRITE_TEXTURE = 1 << 2,
    NS_SHADER_USE_READ_TEXTURE = 1 << 3,
    NS_SHADER_USE_ROOT = 1 << 4,
    NS_SHADER_USE_SHADOW_MAP = 1 << 5,
    NS_SHADER_USE_TEXTURE_MAP = 1 << 6,
    NS_SHADER_USE_MASK_MAP = 1 << 7,
    NS_SHADER_USE_SCENE_UNIFORMS = 1 << 8,
    NS_SHADER_USE_STORAGE_BUFFER = 1 << 9,
    NS_SHADER_USE_WRITE_TEXTURE_SECONDARY = 1 << 10,
};

typedef struct ns_shader_resource_binding {
    u32 bit;
    const char *msl_param;  // null when MSL does not bind it per entry
    const char *hlsl_param; // null when HLSL declares it at module scope
    const char *name;
} ns_shader_resource_binding;

// Declaration order, shared by entry signatures, helper signatures and calls.
static const ns_shader_resource_binding ns_shader_resources[] = {
    {NS_SHADER_USE_GLOBAL_ID, "uint3 ns_global_id", "uint3 ns_global_id", "ns_global_id"},
    {NS_SHADER_USE_VERTEX_ID, "uint ns_vertex_id", "uint ns_vertex_id", "ns_vertex_id"},
    {NS_SHADER_USE_WRITE_TEXTURE, "texture2d<float, access::write> ns_write_texture", ns_null, "ns_write_texture"},
    {NS_SHADER_USE_WRITE_TEXTURE_SECONDARY, "texture2d<float, access::write> ns_secondary_write_texture", ns_null, "ns_secondary_write_texture"},
    {NS_SHADER_USE_READ_TEXTURE, "texture2d<float, access::read> ns_read_texture", ns_null, "ns_read_texture"},
    {NS_SHADER_USE_ROOT, "constant float4* ns_root", ns_null, "ns_root"},
    {NS_SHADER_USE_SHADOW_MAP, "depth2d<float> ns_shadow_map", ns_null, "ns_shadow_map"},
    {NS_SHADER_USE_TEXTURE_MAP, "texture2d<float> ns_texture_map", ns_null, "ns_texture_map"},
    {NS_SHADER_USE_MASK_MAP, "texture2d<float> ns_mask_map", ns_null, "ns_mask_map"},
    {NS_SHADER_USE_SCENE_UNIFORMS, "constant ns_scene_uniforms& ns_uniforms", ns_null, "ns_uniforms"},
};

// Per-fn record of the resources it and its callees reach.
typedef struct ns_shader_fn_use {
    i32 fn_index;
    u32 mask;
    i32 *storage_buffers;
} ns_shader_fn_use;

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
    ns_shader_fn_use *fn_uses; // resources each collected fn reaches

    ns_shader_entry_desc *entries;

    i32 *vs_inputs; // struct symbol indices used as vertex input
    i32 *stage_ios; // struct symbol indices used as stage io (vs out == fs in)

    ns_shader_local *locals;
    ns_bool uses_shadow_map;
    ns_bool uses_texture_map;
    ns_bool uses_mask_map;
    ns_bool uses_scene_uniforms;
    ns_bool uses_global_id;
    ns_bool uses_vertex_id;
    ns_bool uses_root;
    ns_bool uses_read_texture;
    ns_bool uses_write_texture;
    ns_bool uses_write_texture_secondary;
    i32 *storage_buffers;
    // Storage buffer indices some fn in the program stores to. A buffer absent
    // from this list is declared read-only, which every backend wants: MSL warns
    // about a writable resource in a non-void vertex fn, and WGSL forbids a
    // read_write storage buffer in a vertex stage outright. The set is
    // program-wide, not per fn, so the type of `ns_storage_buffer_<n>` stays the
    // same in an entry, in a helper's parameter list and at its call sites.
    i32 *storage_writes;
} ns_shader_emit;

#define ns_shader_try(x)                                                                                                                             \
    do {                                                                                                                                             \
        ns_return_void _r = (x);                                                                                                                     \
        if (ns_return_is_error(_r)) return _r;                                                                                                       \
    } while (0)

#define ns_shader_loc(e, n) ns_ast_state_loc((e)->ctx, (n)->state)

static ns_return_void ns_shader_emit_expr(ns_shader_emit *e, i32 i, ns_str *dst);
static ns_return_void ns_shader_emit_delimited_expr(ns_shader_emit *e, i32 i, ns_str *dst);
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

static ns_bool ns_shader_ident_char(i8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static ns_bool ns_shader_wgsl_reserved(ns_str token) {
    static const char *words[] = {
        "NULL", "Self", "abstract", "active", "alignas", "alignof", "asm", "asm_fragment", "async", "attribute", "auto", "await",
        "become", "bf16", "binding_array", "cast", "catch", "class", "co_await", "co_return", "co_yield", "coherent", "column_major",
        "common", "compile", "compile_fragment", "concept", "const_cast", "consteval", "constexpr", "constinit", "crate", "debug", "decltype",
        "delete", "demote", "demote_to_helper", "do", "dynamic_cast", "enum", "explicit", "export", "extends", "extern", "external", "f16",
        "f64", "fallthrough", "filter", "final", "finally", "friend", "from", "fxgroup", "get", "goto", "groupshared", "handle", "highp",
        "i8", "i16", "i64", "impl", "implements", "import", "inline", "instanceof", "interface", "layout", "lowp", "macro", "macro_rules",
        "mat", "match", "mediump", "meta", "mod", "module", "move", "mut", "mutable", "namespace", "new", "nil", "noexport", "noexcept",
        "noinline", "nointerpolation", "noperspective", "null", "nullptr", "of", "operator", "package", "packoffset", "partition", "pass", "patch",
        "pixelfragment", "precise", "precision", "premerge", "priv", "protected", "pub", "public", "readonly", "ref", "regardless", "register",
        "reinterpret_cast", "require", "resource", "restrict", "self", "set", "shared", "sizeof", "smooth", "snorm", "static", "static_assert",
        "static_cast", "std", "subroutine", "super", "target", "template", "this", "thread_local", "throw", "trait", "try", "type", "typedef",
        "typeid", "typename", "typeof", "u8", "u16", "u64", "union", "unless", "unorm", "unsafe", "unsized", "use", "using", "varying",
        "vec", "virtual", "void", "volatile", "wgsl", "where", "with", "writeonly", "yield",
    };
    for (szt i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        if (ns_str_equals(token, ns_str_cstr(words[i]))) return true;
    }
    return false;
}

// Nano Script deliberately permits several names which WGSL reserves for
// future language features. Prefix only whole reserved identifiers after
// emission so declarations and all their references stay in sync.
static ns_str ns_shader_escape_wgsl_identifiers(ns_str source) {
    ns_str out = {.data = ns_null, .len = 0, .dynamic = true};
    for (i32 i = 0; i < source.len;) {
        i8 c = source.data[i];
        if (!ns_shader_ident_char(c) || (c >= '0' && c <= '9')) {
            ns_str_append_len(&out, source.data + i, 1);
            i++;
            continue;
        }
        i32 end = i + 1;
        while (end < source.len && ns_shader_ident_char(source.data[end])) end++;
        ns_str token = ns_str_range(source.data + i, end - i);
        if (ns_shader_wgsl_reserved(token)) ns_shader_cstr(&out, "ns_");
        ns_shader_str(&out, token);
        i = end;
    }
    ns_array_free(source.data);
    return out;
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
    if (target == NS_SHADER_WGSL) return "f32";
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
    if (ns_str_equals(s, ns_str_cstr("wgsl")) || ns_str_equals(s, ns_str_cstr("webgpu"))) return NS_SHADER_WGSL;
    return NS_SHADER_TARGET_UNKNOWN;
}

ns_str ns_shader_target_name(ns_shader_target t) {
    switch (t) {
    case NS_SHADER_MSL: return ns_str_cstr("msl");
    case NS_SHADER_GLSL_VULKAN: return ns_str_cstr("glsl");
    case NS_SHADER_HLSL: return ns_str_cstr("hlsl");
    case NS_SHADER_WGSL: return ns_str_cstr("wgsl");
    default: return ns_str_cstr("unknown");
    }
}

ns_str ns_shader_entry_name(ns_shader_target t, ns_str fn_name) {
    // GLSL entry points are always the generated `void main()` wrapper.
    if (t == NS_SHADER_GLSL_VULKAN) return ns_str_cstr("main");
    return fn_name;
}

u64 ns_shader_source_hash(ns_str source) {
    // FNV-1a 64, matching the content hash `ns build` stamps its inputs with.
    u64 hash = 14695981039346656037ull;
    for (i32 i = 0; i < source.len; ++i) {
        hash ^= (u64)(u8)source.data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// symbol helpers
// ---------------------------------------------------------------------------
static ns_bool ns_shader_is_main_tu(ns_symbol *s) { return s->lib.len == 0 || ns_str_equals(s->lib, ns_str_cstr("main")); }

// Node indices are relative to the translation unit that parsed them, so a
// transpiled fn and every fn it calls must live in the unit being walked.
// That unit is the caller's file for an application shader and the module's
// own file for a shader shipped by a `use`d module (lib/dynamic.ns).
static ns_bool ns_shader_in_unit(ns_ast_ctx *ctx, ns_symbol *s) {
    return s->fn.ctx == ns_null || s->fn.ctx == ctx;
}

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
    if (target == NS_SHADER_WGSL) {
        switch (dim) {
        case 2: return "vec2<f32>";
        case 3: return "vec3<f32>";
        case 4: return "vec4<f32>";
        case 16: return "mat4x4<f32>";
        default: return "f32";
        }
    }
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

// Callee lookup. A fn is only transpilable from the unit it was parsed in, so
// the unit being emitted wins over a same-named fn anywhere else.
static ns_symbol *ns_shader_find_fn(ns_vm *vm, ns_ast_ctx *ctx, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        if (s->type == NS_SYMBOL_FN && s->fn.ctx == ctx && ns_str_equals(s->name, name)) return s;
    }
    return ns_shader_find_global(vm, name);
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
    t = ns_enum_underlying_type(e->vm, t);
    if (ns_type_is_ref(t)) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: ref types are not supported in shader fns.");
    if (ns_type_is_array(t)) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: array types are not supported in shader fns.");
    switch (t.type) {
    case NS_TYPE_F32:
    case NS_TYPE_F64: ns_shader_cstr(dst, e->target == NS_SHADER_WGSL ? "f32" : "float"); break; // f64 demoted: shaders are f32
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32: ns_shader_cstr(dst, e->target == NS_SHADER_WGSL ? "i32" : "int"); break;
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32: ns_shader_cstr(dst, e->target == NS_SHADER_WGSL ? "u32" : "uint"); break;
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
    t = ns_enum_underlying_type(e->vm, t);
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
    // Fragment derivatives. WGSL names are remapped in ns_shader_builtin_name.
    {"ddx", "dfdx", "dFdx", "ddx", false},         {"ddy", "dfdy", "dFdy", "ddy", false},
    {"shader_discard", "discard_fragment", "discard", "discard", false},
    {"shader_sample_shadow", "ns_shadow_compare", "ns_shadow_compare", "ns_shadow_compare", true},
    {"shader_sample_texture", "ns_texture_sample", "ns_texture_sample", "ns_texture_sample", false},
    {"shader_sample_texture_nearest", "ns_texture_sample_nearest", "ns_texture_sample_nearest", "ns_texture_sample_nearest", false},
    {"shader_sample_mask", "ns_mask_sample", "ns_mask_sample", "ns_mask_sample", false},
    {"shader_transform_position", "ns_transform_position", "ns_transform_position", "ns_transform_position", false},
    {"shader_transform_normal", "ns_transform_normal", "ns_transform_normal", "ns_transform_normal", false},
    {"shader_shadow_clip_position", "ns_shadow_clip_position", "ns_shadow_clip_position", "ns_shadow_clip_position", false},
    {"shader_scene_selected", "ns_scene_selected", "ns_scene_selected", "ns_scene_selected", true},
    {"shader_scene_textured", "ns_scene_textured", "ns_scene_textured", "ns_scene_textured", true},
    {"shader_scene_receives_shadow", "ns_scene_receives_shadow", "ns_scene_receives_shadow", "ns_scene_receives_shadow", true},
    {"shader_global_id_x", "ns_global_id_x", "ns_global_id_x", "ns_global_id_x", true},
    {"shader_global_id_y", "ns_global_id_y", "ns_global_id_y", "ns_global_id_y", true},
    {"shader_global_id_z", "ns_global_id_z", "ns_global_id_z", "ns_global_id_z", true},
    {"shader_vertex_id", "ns_vertex_id", "ns_vertex_id", "ns_vertex_id", true},
    {"shader_root_f32", "ns_root_f32", "ns_root_f32", "ns_root_f32", true},
    {"shader_buffer_i32", "ns_storage_buffer", "ns_storage_buffer", "ns_storage_buffer", true},
    {"shader_buffer_store_i32", "ns_storage_buffer", "ns_storage_buffer", "ns_storage_buffer", true},
    {"shader_read_texture", "ns_read_texture", "ns_read_texture", "ns_read_texture", false},
    {"shader_write_texture", "ns_write_texture", "ns_write_texture", "ns_write_texture", false},
    {"shader_write_texture_secondary", "ns_secondary_write_texture", "ns_secondary_write_texture", "ns_secondary_write_texture", false},
};

static const ns_shader_builtin *ns_shader_find_builtin(ns_str name) {
    for (szt i = 0; i < sizeof(ns_shader_builtins) / sizeof(ns_shader_builtins[0]); ++i) {
        if (ns_str_equals(name, ns_str_cstr(ns_shader_builtins[i].name))) return &ns_shader_builtins[i];
    }
    return ns_null;
}

static const char *ns_shader_builtin_name(const ns_shader_builtin *b, ns_shader_target t) {
    if (t == NS_SHADER_WGSL) {
        if (ns_str_equals(ns_str_cstr(b->name), ns_str_cstr("ddx"))) return "dpdx";
        if (ns_str_equals(ns_str_cstr(b->name), ns_str_cstr("ddy"))) return "dpdy";
        return b->glsl;
    }
    switch (t) {
    case NS_SHADER_MSL: return b->msl;
    case NS_SHADER_GLSL_VULKAN: return b->glsl;
    case NS_SHADER_HLSL: return b->hlsl;
    default: return b->glsl;
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

// Declared length of a fixed-capacity local array, or 0.
static i32 ns_shader_local_array_len(ns_shader_emit *e, ns_str name) {
    for (i32 i = (i32)ns_array_length(e->locals) - 1; i >= 0; --i) {
        if (ns_str_equals(e->locals[i].name, name)) return e->locals[i].array_len;
    }
    return 0;
}

// Element type of an array type, or unknown. An array type carries its
// element's type and symbol index with the array flag set.
static ns_type ns_shader_array_element(ns_type t) {
    if (!ns_type_is_array(t)) return ns_type_unknown;
    return (ns_type){.type = t.type, .ref = t.ref, .array = false, .mut = t.mut, .stack = true, .index = t.index};
}

// A global `lit` is a compile-time constant with no counterpart in any shader
// language, so it folds to its value. Locals shadow globals, so a name bound
// inside the shader fn is never folded.
static ns_symbol *ns_shader_lit_symbol(ns_shader_emit *e, ns_str name) {
    if (!ns_type_is_unknown(ns_shader_local_type(e, name))) return ns_null;
    ns_symbol *s = ns_shader_find_global(e->vm, name);
    return s && s->type == NS_SYMBOL_VALUE && s->is_lit ? s : ns_null;
}

// Integer constant expression over literals and `lit` bindings: the same
// arithmetic a `lit` initializer allows, which is what a local array length is
// written with (`[i32](FACES * 3)`).
static ns_return_void ns_shader_const_expr(ns_shader_emit *e, i32 node, i64 *out, ns_code_loc loc) {
    const char *shape = "shader: a local array needs a constant length, for example [float3](4).";
    if (node == 0) return ns_return_error(void, loc, NS_ERR_EVAL, shape);
    ns_ast_t *n = &e->ctx->nodes[node];
    if (n->type == NS_AST_EXPR) return ns_shader_const_expr(e, n->expr.body, out, loc);
    if (n->type == NS_AST_CAST_EXPR) return ns_shader_const_expr(e, n->cast_expr.expr, out, loc);
    if (n->type == NS_AST_UNARY_EXPR) {
        i64 operand = 0;
        ns_shader_try(ns_shader_const_expr(e, n->unary_expr.expr, &operand, loc));
        if (n->unary_expr.op.type != NS_TOKEN_ADD_OP) return ns_return_error(void, loc, NS_ERR_EVAL, shape);
        if (ns_str_equals(n->unary_expr.op.val, ns_str_cstr("-"))) operand = -operand;
        *out = operand;
        return ns_return_ok_void;
    }
    if (n->type == NS_AST_BINARY_EXPR) {
        i64 left = 0;
        i64 right = 0;
        ns_shader_try(ns_shader_const_expr(e, n->binary_expr.left, &left, loc));
        ns_shader_try(ns_shader_const_expr(e, n->binary_expr.right, &right, loc));
        ns_str op = n->binary_expr.op.val;
        if (ns_str_equals(op, ns_str_cstr("+"))) *out = left + right;
        else if (ns_str_equals(op, ns_str_cstr("-"))) *out = left - right;
        else if (ns_str_equals(op, ns_str_cstr("*"))) *out = left * right;
        else if (ns_str_equals(op, ns_str_cstr("/")) && right != 0) *out = left / right;
        else if (ns_str_equals(op, ns_str_cstr("%")) && right != 0) *out = left % right;
        else return ns_return_error(void, loc, NS_ERR_EVAL, shape);
        return ns_return_ok_void;
    }
    if (n->type != NS_AST_PRIMARY_EXPR) return ns_return_error(void, loc, NS_ERR_EVAL, shape);

    if (n->primary_expr.token.type == NS_TOKEN_INT_LITERAL) {
        char digits[32];
        ns_str body = ns_shader_literal_body(n->primary_expr.token);
        if (body.len <= 0 || body.len >= (i32)sizeof(digits)) return ns_return_error(void, loc, NS_ERR_EVAL, shape);
        memcpy(digits, body.data, (szt)body.len);
        digits[body.len] = 0;
        *out = strtoll(digits, ns_null, 10);
        return ns_return_ok_void;
    }
    if (n->primary_expr.token.type == NS_TOKEN_IDENTIFIER) {
        ns_symbol *s = ns_shader_lit_symbol(e, n->primary_expr.token.val);
        ns_type t = s ? ns_enum_underlying_type(e->vm, s->val.t) : ns_type_unknown;
        if (!s || ns_type_is_float(t) || !ns_type_is_number(t)) return ns_return_error(void, loc, NS_ERR_EVAL, shape);
        ns_value v = s->val;
        v.t = t;
        if (ns_type_is(t, NS_TYPE_I32)) *out = ns_eval_number_i32(e->vm, v);
        else if (ns_type_is(t, NS_TYPE_U32)) *out = ns_eval_number_u32(e->vm, v);
        else *out = ns_eval_number_i64(e->vm, v);
        return ns_return_ok_void;
    }
    return ns_return_error(void, loc, NS_ERR_EVAL, shape);
}

// Constant length of a local array declaration. A shader array is sized at
// compile time on every backend.
static ns_return_void ns_shader_const_i32(ns_shader_emit *e, i32 node, ns_bool literal, i32 *out, ns_code_loc loc) {
    if (literal) {
        return ns_return_error(void, loc, NS_ERR_EVAL,
                               "shader: an array literal is not supported in a shader fn; declare a length, for example [float3](4).");
    }
    i64 value = 0;
    ns_shader_try(ns_shader_const_expr(e, node, &value, loc));
    if (value <= 0 || value > NS_SHADER_MAX_ARRAY_LEN) {
        snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: a local array length must be between 1 and %d.", NS_SHADER_MAX_ARRAY_LEN);
        return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
    }
    *out = (i32)value;
    return ns_return_ok_void;
}

static ns_return_void ns_shader_storage_buffer_index(ns_shader_emit *e, i32 node, i32 *out, ns_code_loc loc) {
    i64 value = 0;
    ns_return_void parsed = ns_shader_const_expr(e, node, &value, loc);
    // Keep the largest emitted binding (WGSL reserves 0...6) representable;
    // the active GPU backend validates its own, much smaller, platform limit
    // when the shader is created.
    if (ns_return_is_error(parsed) || value < 0 || value > 0x7ffffff8LL) {
        snprintf(ns_shader_err, sizeof(ns_shader_err),
                 "shader: storage buffer index must be a compile-time integer from 0 to 2147483640.");
        return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
    }
    *out = (i32)value;
    return ns_return_ok_void;
}

static void ns_shader_add_storage_buffer(i32 **buffers, i32 index) {
    if (!ns_shader_index_in(*buffers, index)) ns_array_push(*buffers, index);
}

static void ns_shader_merge_storage_buffers(i32 **dst, i32 *src) {
    for (i32 i = 0, l = (i32)ns_array_length(src); i < l; ++i) ns_shader_add_storage_buffer(dst, src[i]);
}

// A buffer nothing in the program stores to is declared read-only.
static ns_bool ns_shader_storage_is_const(ns_shader_emit *e, i32 index) {
    return !ns_shader_index_in(e->storage_writes, index);
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
        case NS_TOKEN_IDENTIFIER: {
            ns_type local = ns_shader_local_type(e, n->primary_expr.token.val);
            if (!ns_type_is_unknown(local)) return local;
            ns_symbol *lit = ns_shader_lit_symbol(e, n->primary_expr.token.val);
            if (lit) return lit->val.t;
            ns_symbol *s = ns_vm_find_symbol(e->vm, n->primary_expr.token.val, false);
            return s && s->type == NS_SYMBOL_ENUM ? s->en.t : ns_type_unknown;
        }
        default: return ns_type_unknown;
        }
    }
    case NS_AST_INDEX_EXPR: return ns_shader_array_element(ns_shader_infer(e, n->index_expr.table));
    case NS_AST_MEMBER_EXPR: {
        ns_type lt = ns_shader_infer(e, n->member_expr.left);
        if (ns_type_is(lt, NS_TYPE_ENUM)) return lt;
        if (ns_type_is_array(lt)) return ns_type_i32; // `.len` of a fixed array
        if (!ns_type_is(lt, NS_TYPE_STRUCT)) return ns_type_unknown;
        ns_ast_t *r = &e->ctx->nodes[n->member_expr.right];
        if (r->type != NS_AST_PRIMARY_EXPR) return ns_type_unknown;
        ns_symbol *s = &e->vm->symbols[ns_type_index(lt)];
        for (i32 f = 0, l = (i32)ns_array_length(s->st.fields); f < l; ++f) {
            if (ns_str_equals(s->st.fields[f].name, r->primary_expr.token.val)) return s->st.fields[f].t;
        }
        i32 swizzle = ns_simd_swizzle(e->vm, lt, r->primary_expr.token.val, ns_null);
        if (swizzle == 1) return ns_type_f32;
        if (swizzle >= 2) return ns_simd_type_for_dim(e->vm, swizzle);
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
        lt = ns_enum_underlying_type(e->vm, lt);
        rt = ns_enum_underlying_type(e->vm, rt);
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

static u32 ns_shader_use_mask(ns_shader_emit *e) {
    u32 mask = 0;
    if (e->uses_global_id) mask |= NS_SHADER_USE_GLOBAL_ID;
    if (e->uses_vertex_id) mask |= NS_SHADER_USE_VERTEX_ID;
    if (e->uses_write_texture) mask |= NS_SHADER_USE_WRITE_TEXTURE;
    if (e->uses_write_texture_secondary) mask |= NS_SHADER_USE_WRITE_TEXTURE_SECONDARY;
    if (e->uses_read_texture) mask |= NS_SHADER_USE_READ_TEXTURE;
    if (e->uses_root) mask |= NS_SHADER_USE_ROOT;
    if (e->uses_shadow_map) mask |= NS_SHADER_USE_SHADOW_MAP;
    if (e->uses_texture_map) mask |= NS_SHADER_USE_TEXTURE_MAP;
    if (e->uses_mask_map) mask |= NS_SHADER_USE_MASK_MAP;
    if (e->uses_scene_uniforms) mask |= NS_SHADER_USE_SCENE_UNIFORMS;
    if (ns_array_length(e->storage_buffers) != 0) mask |= NS_SHADER_USE_STORAGE_BUFFER;
    return mask;
}

static void ns_shader_set_use_mask(ns_shader_emit *e, u32 mask) {
    e->uses_global_id = (mask & NS_SHADER_USE_GLOBAL_ID) != 0;
    e->uses_vertex_id = (mask & NS_SHADER_USE_VERTEX_ID) != 0;
    e->uses_write_texture = (mask & NS_SHADER_USE_WRITE_TEXTURE) != 0;
    e->uses_write_texture_secondary = (mask & NS_SHADER_USE_WRITE_TEXTURE_SECONDARY) != 0;
    e->uses_read_texture = (mask & NS_SHADER_USE_READ_TEXTURE) != 0;
    e->uses_root = (mask & NS_SHADER_USE_ROOT) != 0;
    e->uses_shadow_map = (mask & NS_SHADER_USE_SHADOW_MAP) != 0;
    e->uses_texture_map = (mask & NS_SHADER_USE_TEXTURE_MAP) != 0;
    e->uses_mask_map = (mask & NS_SHADER_USE_MASK_MAP) != 0;
    e->uses_scene_uniforms = (mask & NS_SHADER_USE_SCENE_UNIFORMS) != 0;
}

static u32 ns_shader_fn_mask(ns_shader_emit *e, i32 fn_index) {
    for (i32 i = 0, l = (i32)ns_array_length(e->fn_uses); i < l; ++i) {
        if (e->fn_uses[i].fn_index == fn_index) return e->fn_uses[i].mask;
    }
    return 0;
}

static i32 *ns_shader_fn_storage_buffers(ns_shader_emit *e, i32 fn_index) {
    for (i32 i = 0, l = (i32)ns_array_length(e->fn_uses); i < l; ++i) {
        if (e->fn_uses[i].fn_index == fn_index) return e->fn_uses[i].storage_buffers;
    }
    return ns_null;
}

static void ns_shader_free_fn_uses(ns_shader_fn_use *uses) {
    for (i32 i = 0, l = (i32)ns_array_length(uses); i < l; ++i) ns_array_free(uses[i].storage_buffers);
    ns_array_free(uses);
}

// The parameter (declaration) or argument (call) a target threads for `bit`,
// or null when the target reaches it without one.
static const char *ns_shader_resource_param(ns_shader_target target, const ns_shader_resource_binding *r) {
    if (target == NS_SHADER_MSL) return r->msl_param;
    if (target == NS_SHADER_HLSL) return r->hlsl_param;
    if (target == NS_SHADER_WGSL && r->bit == NS_SHADER_USE_GLOBAL_ID) return "ns_global_id: vec3<u32>";
    if (target == NS_SHADER_WGSL && r->bit == NS_SHADER_USE_VERTEX_ID) return "ns_vertex_id: u32";
    return ns_null;
}

// Append the threaded parameters or arguments of `mask`. `declare` emits typed
// parameters, otherwise the resource names.
static void ns_shader_emit_resource_list(ns_shader_emit *e, ns_str *dst, u32 mask, ns_bool declare, ns_bool *first) {
    for (szt i = 0; i < sizeof(ns_shader_resources) / sizeof(ns_shader_resources[0]); ++i) {
        const ns_shader_resource_binding *r = &ns_shader_resources[i];
        const char *param = ns_shader_resource_param(e->target, r);
        if (!param || !(mask & r->bit)) continue;
        if (!*first) ns_shader_cstr(dst, ", ");
        ns_shader_cstr(dst, declare ? param : r->name);
        *first = false;
    }
}

static void ns_shader_emit_storage_buffer_list(ns_shader_emit *e, ns_str *dst, i32 *storage_buffers, ns_bool declare, ns_bool *first) {
    if (e->target != NS_SHADER_MSL) return;
    for (i32 i = 0, l = (i32)ns_array_length(storage_buffers); i < l; ++i) {
        i32 index = storage_buffers[i];
        if (!*first) ns_shader_cstr(dst, ", ");
        if (declare) ns_shader_cstr(dst, ns_shader_storage_is_const(e, index) ? "device const int* " : "device int* ");
        ns_shader_cstr(dst, "ns_storage_buffer_");
        ns_shader_i32(dst, index);
        *first = false;
    }
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
            (ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_sample_texture")) ||
             ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_sample_texture_nearest")))) {
            e->uses_texture_map = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_sample_mask"))) {
            e->uses_mask_map = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_starts_with(callee->primary_expr.token.val, ns_str_cstr("shader_global_id_"))) {
            e->uses_global_id = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_vertex_id"))) {
            e->uses_vertex_id = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_root_f32"))) {
            e->uses_root = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_read_texture"))) {
            e->uses_read_texture = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_write_texture"))) {
            e->uses_write_texture = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_write_texture_secondary"))) {
            e->uses_write_texture_secondary = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            (ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_buffer_i32")) ||
             ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_buffer_store_i32")))) {
            i32 buffer_index = 0;
            ns_shader_try(ns_shader_storage_buffer_index(e, n->next, &buffer_index, ns_shader_loc(e, n)));
            ns_shader_add_storage_buffer(&e->storage_buffers, buffer_index);
            // Writability is a property of the buffer across the whole program,
            // so it is recorded outside the per-fn set the caller inherits.
            if (ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_buffer_store_i32"))) {
                ns_shader_add_storage_buffer(&e->storage_writes, buffer_index);
            }
        }
        if (callee->type == NS_AST_PRIMARY_EXPR &&
            (ns_str_starts_with(callee->primary_expr.token.val, ns_str_cstr("shader_transform_")) ||
             ns_str_equals(callee->primary_expr.token.val, ns_str_cstr("shader_shadow_clip_position")) ||
             ns_str_starts_with(callee->primary_expr.token.val, ns_str_cstr("shader_scene_")))) {
            e->uses_scene_uniforms = true;
        }
        if (callee->type == NS_AST_PRIMARY_EXPR && !ns_shader_find_builtin(callee->primary_expr.token.val)) {
            ns_symbol *s = ns_null;
            if (n->call_expr.rt >= 0 && n->call_expr.rt < (i32)ns_array_length(e->vm->symbols)) {
                s = &e->vm->symbols[n->call_expr.rt];
            }
            if (!s || s->type != NS_SYMBOL_FN) s = ns_shader_find_fn(e->vm, e->ctx, callee->primary_expr.token.val);
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
        if (ns_shader_index_in(e->fns, fn_index)) {
            ns_shader_set_use_mask(e, ns_shader_use_mask(e) | ns_shader_fn_mask(e, fn_index));
            ns_shader_merge_storage_buffers(&e->storage_buffers, ns_shader_fn_storage_buffers(e, fn_index));
            return ns_return_ok_void;
        }
    }
    if (ns_shader_index_in(e->fn_visit, fn_index)) {
        return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "shader: recursive fn calls are not supported in shaders.");
    }

    if (s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || s->fn.body == 0) {
        snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: `%.*s` is not a transpilable fn (native ref fns have no body).", s->name.len, s->name.data);
        return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, ns_shader_err);
    }
    if (!ns_shader_in_unit(e->ctx, s)) {
        snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: fn `%.*s` must be defined in the same file as the shader entry.", s->name.len, s->name.data);
        return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, ns_shader_err);
    }

    ns_ast_t *fn_node = &e->ctx->nodes[s->fn.ast];
    ns_code_loc loc = ns_shader_loc(e, fn_node);
    ns_shader_try(ns_shader_collect_type(e, s->fn.ret, loc));
    for (i32 a = 0, l = (i32)ns_array_length(s->fn.args); a < l; ++a) {
        ns_shader_try(ns_shader_collect_type(e, s->fn.args[a].val.t, loc));
    }

    // Collect this fn's resource usage on its own, then fold it back into the
    // caller's: what a callee reaches, its caller has to thread through.
    u32 outer = ns_shader_use_mask(e);
    i32 *outer_storage_buffers = e->storage_buffers;
    ns_shader_set_use_mask(e, 0);
    e->storage_buffers = ns_null;
    ns_array_push(e->fn_visit, fn_index);
    ns_return_void body = ns_shader_collect_stmt(e, s->fn.body, depth + 1);
    ns_array_set_length(e->fn_visit, ns_array_length(e->fn_visit) - 1);
    u32 used = ns_shader_use_mask(e);
    i32 *used_storage_buffers = e->storage_buffers;
    ns_shader_set_use_mask(e, outer | used);
    e->storage_buffers = outer_storage_buffers;
    ns_shader_merge_storage_buffers(&e->storage_buffers, used_storage_buffers);
    if (ns_return_is_error(body)) {
        ns_array_free(used_storage_buffers);
        return body;
    }
    ns_array_push(e->fn_uses, ((ns_shader_fn_use){.fn_index = fn_index, .mask = used, .storage_buffers = used_storage_buffers}));

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
        if (n->desig_expr.positional) {
            if (f < field_count) {
                provided[f] = field->field_def.expr;
                found = true;
            }
        } else {
            for (i32 k = 0; k < field_count; ++k) {
                if (ns_str_equals(s->st.fields[k].name, field->field_def.name.val)) {
                    provided[k] = field->field_def.expr;
                    found = true;
                    break;
                }
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
    if (e->target == NS_SHADER_WGSL) ns_shader_cstr(&decl, "var ns_t");
    else ns_shader_str(&decl, s->name);
    if (e->target != NS_SHADER_WGSL) ns_shader_cstr(&decl, " ns_t");
    ns_shader_i32(&decl, tmp);
    if (e->target == NS_SHADER_WGSL) {
        ns_shader_cstr(&decl, ": ");
        ns_shader_str(&decl, s->name);
    }
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

// Emit a folded `lit` constant. Enums lower to their underlying integer, and
// floats always carry a decimal point because GLSL does not convert an integer
// literal to float implicitly.
static ns_return_void ns_shader_emit_lit(ns_shader_emit *e, ns_symbol *s, ns_code_loc loc, ns_str *dst) {
    ns_type t = ns_enum_underlying_type(e->vm, s->val.t);
    char literal[64];
    i32 len;
    if (ns_type_is(t, NS_TYPE_BOOL)) {
        ns_shader_cstr(dst, s->val.b ? "true" : "false");
        return ns_return_ok_void;
    }
    // Each width reads its own union member; the value carries no wider
    // representation to convert from.
    ns_value v = s->val;
    v.t = t;
    switch (t.type) {
    case NS_TYPE_I8: len = snprintf(literal, sizeof(literal), "%d", (i32)ns_eval_number_i8(e->vm, v)); break;
    case NS_TYPE_I16: len = snprintf(literal, sizeof(literal), "%d", (i32)ns_eval_number_i16(e->vm, v)); break;
    case NS_TYPE_I32: len = snprintf(literal, sizeof(literal), "%d", ns_eval_number_i32(e->vm, v)); break;
    case NS_TYPE_I64: len = snprintf(literal, sizeof(literal), "%lld", (long long)ns_eval_number_i64(e->vm, v)); break;
    case NS_TYPE_U8: len = snprintf(literal, sizeof(literal), "%u", (u32)ns_eval_number_u8(e->vm, v)); break;
    case NS_TYPE_U16: len = snprintf(literal, sizeof(literal), "%u", (u32)ns_eval_number_u16(e->vm, v)); break;
    case NS_TYPE_U32: len = snprintf(literal, sizeof(literal), "%u", ns_eval_number_u32(e->vm, v)); break;
    case NS_TYPE_U64: len = snprintf(literal, sizeof(literal), "%llu", (unsigned long long)ns_eval_number_u64(e->vm, v)); break;
    case NS_TYPE_F32: len = snprintf(literal, sizeof(literal), "%.9g", (f64)ns_eval_number_f32(e->vm, v)); break;
    case NS_TYPE_F64: len = snprintf(literal, sizeof(literal), "%.17g", ns_eval_number_f64(e->vm, v)); break;
    default:
        return ns_return_error(void, loc, NS_ERR_EVAL, "shader: only number, bool and enum lit values are supported in shader fns.");
    }
    // GLSL has no implicit int-to-float conversion, so a float value always
    // carries a decimal point.
    if (ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64)) {
        if (len > 0 && !strpbrk(literal, ".eEnN") && len + 2 < (i32)sizeof(literal)) {
            len += snprintf(literal + len, sizeof(literal) - (szt)len, ".0");
        }
    }
    ns_str_append_len(dst, literal, len);
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
        case NS_TOKEN_IDENTIFIER: {
            ns_symbol *lit = ns_shader_lit_symbol(e, n->primary_expr.token.val);
            if (lit) return ns_shader_emit_lit(e, lit, loc, dst);
            ns_shader_str(dst, n->primary_expr.token.val);
            return ns_return_ok_void;
        }
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
        ns_str op = n->binary_expr.op.val;
        ns_bool assignment =
            ns_str_equals(op, ns_str_cstr("=")) ||
            ns_str_equals(op, ns_str_cstr("+=")) ||
            ns_str_equals(op, ns_str_cstr("-=")) ||
            ns_str_equals(op, ns_str_cstr("*=")) ||
            ns_str_equals(op, ns_str_cstr("/=")) ||
            ns_str_equals(op, ns_str_cstr("%=")) ||
            ns_str_equals(op, ns_str_cstr("&=")) ||
            ns_str_equals(op, ns_str_cstr("|=")) ||
            ns_str_equals(op, ns_str_cstr("^=")) ||
            ns_str_equals(op, ns_str_cstr("<<=")) ||
            ns_str_equals(op, ns_str_cstr(">>="));
        // Preserve the Nano Script AST exactly. Target shader languages do
        // not necessarily reconstruct the same grouping when a nested binary
        // expression is emitted as a flat token stream. Assignment is already
        // a statement in the supported subset and cannot be parenthesized in
        // WGSL.
        if (!assignment) ns_shader_cstr(dst, "(");
        ns_shader_try(ns_shader_emit_expr(e, n->binary_expr.left, dst));
        ns_shader_cstr(dst, " ");
        if (ns_str_equals(op, ns_str_cstr("==="))) op = ns_str_cstr("==");
        else if (ns_str_equals(op, ns_str_cstr("!=="))) op = ns_str_cstr("!=");
        ns_shader_str(dst, op);
        ns_shader_cstr(dst, " ");
        ns_bool wgsl_shift = e->target == NS_SHADER_WGSL &&
            (ns_str_equals(op, ns_str_cstr("<<")) || ns_str_equals(op, ns_str_cstr(">>")) ||
             ns_str_equals(op, ns_str_cstr("<<=")) || ns_str_equals(op, ns_str_cstr(">>=")));
        if (wgsl_shift) ns_shader_cstr(dst, "u32(");
        ns_shader_try(ns_shader_emit_expr(e, n->binary_expr.right, dst));
        if (wgsl_shift) ns_shader_cstr(dst, ")");
        if (!assignment) ns_shader_cstr(dst, ")");
        return ns_return_ok_void;
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
        if (ns_type_is(lt, NS_TYPE_ENUM)) {
            ns_symbol *en = &e->vm->symbols[ns_type_index(lt)];
            i32 member = ns_enum_member_index(en, field);
            if (member < 0) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: unknown enum member.");
            char literal[32];
            ns_type underlying = en->en.underlying;
            i32 len;
            if (ns_type_is(underlying, NS_TYPE_I8) || ns_type_is(underlying, NS_TYPE_I16) ||
                ns_type_is(underlying, NS_TYPE_I32) || ns_type_is(underlying, NS_TYPE_I64)) {
                len = snprintf(literal, sizeof(literal), "%lld", (long long)(i64)en->en.members[member].value);
            } else {
                len = snprintf(literal, sizeof(literal), "%llu", (unsigned long long)en->en.members[member].value);
            }
            ns_str_append_len(dst, literal, len);
            return ns_return_ok_void;
        }
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
            // A fixed-capacity local array knows its length at transpile time.
            ns_ast_t *table = &e->ctx->nodes[n->member_expr.left];
            if (ns_type_is_array(lt) && table->type == NS_AST_PRIMARY_EXPR) {
                i32 len = ns_shader_local_array_len(e, table->primary_expr.token.val);
                if (len > 0) {
                    ns_shader_i32(dst, len);
                    return ns_return_ok_void;
                }
            }
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
        u32 callee_mask = 0;
        i32 *callee_storage_buffers = ns_null;
        const ns_shader_builtin *b = ns_shader_find_builtin(name);
        if (b) {
            ns_bool transform_position = ns_str_equals(name, ns_str_cstr("shader_transform_position"));
            ns_bool transform_normal = ns_str_equals(name, ns_str_cstr("shader_transform_normal"));
            ns_bool shadow_position = ns_str_equals(name, ns_str_cstr("shader_shadow_clip_position"));
            ns_bool selected = ns_str_equals(name, ns_str_cstr("shader_scene_selected"));
            ns_bool textured = ns_str_equals(name, ns_str_cstr("shader_scene_textured"));
            ns_bool receives_shadow = ns_str_equals(name, ns_str_cstr("shader_scene_receives_shadow"));
            ns_bool sample_texture = ns_str_equals(name, ns_str_cstr("shader_sample_texture"));
            ns_bool sample_texture_nearest = ns_str_equals(name, ns_str_cstr("shader_sample_texture_nearest"));
            ns_bool sample_mask = ns_str_equals(name, ns_str_cstr("shader_sample_mask"));
            ns_bool discard = ns_str_equals(name, ns_str_cstr("shader_discard"));
            ns_bool global_x = ns_str_equals(name, ns_str_cstr("shader_global_id_x"));
            ns_bool global_y = ns_str_equals(name, ns_str_cstr("shader_global_id_y"));
            ns_bool global_z = ns_str_equals(name, ns_str_cstr("shader_global_id_z"));
            ns_bool vertex_id = ns_str_equals(name, ns_str_cstr("shader_vertex_id"));
            ns_bool root_f32 = ns_str_equals(name, ns_str_cstr("shader_root_f32"));
            ns_bool buffer_i32 = ns_str_equals(name, ns_str_cstr("shader_buffer_i32"));
            ns_bool buffer_store_i32 = ns_str_equals(name, ns_str_cstr("shader_buffer_store_i32"));
            ns_bool read_texture = ns_str_equals(name, ns_str_cstr("shader_read_texture"));
            ns_bool write_texture = ns_str_equals(name, ns_str_cstr("shader_write_texture"));
            ns_bool write_texture_secondary = ns_str_equals(name, ns_str_cstr("shader_write_texture_secondary"));
            if (discard) {
                if (n->call_expr.arg_count != 0) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_discard takes no arguments.");
                ns_shader_cstr(dst, e->target == NS_SHADER_MSL ? "discard_fragment()" : "discard");
                return ns_return_ok_void;
            }
            if (global_x || global_y || global_z) {
                if (n->call_expr.arg_count != 0) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: global id intrinsic takes no arguments.");
                const char component = global_x ? 'x' : global_y ? 'y' : 'z';
                if (e->target == NS_SHADER_GLSL_VULKAN) {
                    ns_shader_cstr(dst, "int(gl_GlobalInvocationID.");
                    ns_str_append_len(dst, (const i8 *)&component, 1);
                    ns_shader_cstr(dst, ")");
                } else {
                    ns_shader_cstr(dst, e->target == NS_SHADER_WGSL ? "i32(ns_global_id." : "int(ns_global_id.");
                    ns_str_append_len(dst, (const i8 *)&component, 1);
                    ns_shader_cstr(dst, ")");
                }
                return ns_return_ok_void;
            }
            if (vertex_id) {
                if (n->call_expr.arg_count != 0) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: vertex id intrinsic takes no arguments.");
                ns_shader_cstr(dst, e->target == NS_SHADER_WGSL ? "i32(ns_vertex_id)" : "int(ns_vertex_id)");
                return ns_return_ok_void;
            }
            if (root_f32) {
                if (n->call_expr.arg_count != 1) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_root_f32 expects one word index.");
                ns_shader_cstr(dst, e->target == NS_SHADER_MSL ? "ns_root_f32(ns_root, " : "ns_root_f32(");
                ns_shader_try(ns_shader_emit_expr(e, n->next, dst));
                ns_shader_cstr(dst, ")");
                return ns_return_ok_void;
            }
            if (buffer_i32) {
                if (n->call_expr.arg_count != 2) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_buffer_i32 expects a buffer index and element index.");
                i32 buffer_index = 0;
                i32 buffer_arg = n->next;
                i32 element_index = e->ctx->nodes[buffer_arg].next;
                ns_shader_try(ns_shader_storage_buffer_index(e, buffer_arg, &buffer_index, loc));
                if (e->target == NS_SHADER_HLSL) {
                    ns_shader_cstr(dst, "asint(");
                    ns_shader_cstr(dst, "ns_storage_buffer_");
                    ns_shader_i32(dst, buffer_index);
                    ns_shader_cstr(dst, ".Load((");
                } else {
                    ns_shader_cstr(dst, "ns_storage_buffer_");
                    ns_shader_i32(dst, buffer_index);
                    ns_shader_cstr(dst, "[");
                }
                ns_shader_try(ns_shader_emit_expr(e, element_index, dst));
                if (e->target == NS_SHADER_HLSL) ns_shader_cstr(dst, ") * 4))");
                else if (e->target == NS_SHADER_WGSL) ns_shader_cstr(dst, "]");
                else ns_shader_cstr(dst, "]");
                return ns_return_ok_void;
            }
            if (buffer_store_i32) {
                if (n->call_expr.arg_count != 3) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_buffer_store_i32 expects a buffer index, element index and value.");
                i32 buffer_arg = n->next;
                i32 index = e->ctx->nodes[buffer_arg].next;
                i32 value = e->ctx->nodes[index].next;
                i32 buffer_index = 0;
                ns_shader_try(ns_shader_storage_buffer_index(e, buffer_arg, &buffer_index, loc));
                if (e->target == NS_SHADER_HLSL) {
                    ns_shader_cstr(dst, "ns_storage_buffer_");
                    ns_shader_i32(dst, buffer_index);
                    ns_shader_cstr(dst, ".Store((");
                    ns_shader_try(ns_shader_emit_expr(e, index, dst));
                    ns_shader_cstr(dst, ") * 4, asuint(");
                    ns_shader_try(ns_shader_emit_expr(e, value, dst));
                    ns_shader_cstr(dst, "))");
                } else {
                    ns_shader_cstr(dst, "ns_storage_buffer_");
                    ns_shader_i32(dst, buffer_index);
                    ns_shader_cstr(dst, "[");
                    ns_shader_try(ns_shader_emit_expr(e, index, dst));
                    ns_shader_cstr(dst, "] = ");
                    ns_shader_try(ns_shader_emit_expr(e, value, dst));
                }
                return ns_return_ok_void;
            }
            if (read_texture) {
                if (n->call_expr.arg_count != 2) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_read_texture expects x and y.");
                i32 x = n->next;
                i32 y = e->ctx->nodes[x].next;
                if (e->target == NS_SHADER_MSL) {
                    ns_shader_cstr(dst, "ns_read_texture.read(uint2(");
                } else if (e->target == NS_SHADER_GLSL_VULKAN) {
                    ns_shader_cstr(dst, "imageLoad(ns_read_texture, ivec2(");
                } else if (e->target == NS_SHADER_WGSL) {
                    ns_shader_cstr(dst, "textureLoad(ns_read_texture, vec2<i32>(");
                } else {
                    ns_shader_cstr(dst, "ns_read_texture.Load(int3(");
                }
                ns_shader_try(ns_shader_emit_expr(e, x, dst));
                ns_shader_cstr(dst, ", ");
                ns_shader_try(ns_shader_emit_expr(e, y, dst));
                if (e->target == NS_SHADER_HLSL) ns_shader_cstr(dst, ", 0))");
                else if (e->target == NS_SHADER_WGSL) ns_shader_cstr(dst, "), 0)");
                else ns_shader_cstr(dst, "))");
                return ns_return_ok_void;
            }
            if (write_texture || write_texture_secondary) {
                if (n->call_expr.arg_count != 3) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: write texture intrinsic expects x, y and float4 color arguments.");
                i32 x = n->next;
                i32 y = e->ctx->nodes[x].next;
                i32 color = e->ctx->nodes[y].next;
                const char *texture_name = write_texture_secondary ? "ns_secondary_write_texture" : "ns_write_texture";
                if (e->target == NS_SHADER_MSL) {
                    ns_shader_cstr(dst, texture_name);
                    ns_shader_cstr(dst, ".write(");
                    ns_shader_try(ns_shader_emit_expr(e, color, dst));
                    ns_shader_cstr(dst, ", uint2(");
                    ns_shader_try(ns_shader_emit_expr(e, x, dst));
                    ns_shader_cstr(dst, ", ");
                    ns_shader_try(ns_shader_emit_expr(e, y, dst));
                    ns_shader_cstr(dst, "))");
                } else if (e->target == NS_SHADER_GLSL_VULKAN) {
                    ns_shader_cstr(dst, "imageStore(");
                    ns_shader_cstr(dst, texture_name);
                    ns_shader_cstr(dst, ", ivec2(");
                    ns_shader_try(ns_shader_emit_expr(e, x, dst));
                    ns_shader_cstr(dst, ", ");
                    ns_shader_try(ns_shader_emit_expr(e, y, dst));
                    ns_shader_cstr(dst, "), ");
                    ns_shader_try(ns_shader_emit_expr(e, color, dst));
                    ns_shader_cstr(dst, ")");
                } else if (e->target == NS_SHADER_WGSL) {
                    ns_shader_cstr(dst, "textureStore(");
                    ns_shader_cstr(dst, texture_name);
                    ns_shader_cstr(dst, ", vec2<i32>(");
                    ns_shader_try(ns_shader_emit_expr(e, x, dst));
                    ns_shader_cstr(dst, ", ");
                    ns_shader_try(ns_shader_emit_expr(e, y, dst));
                    ns_shader_cstr(dst, "), ");
                    ns_shader_try(ns_shader_emit_expr(e, color, dst));
                    ns_shader_cstr(dst, ")");
                } else {
                    ns_shader_cstr(dst, texture_name);
                    ns_shader_cstr(dst, "[int2(");
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
                    ns_shader_cstr(dst, e->target == NS_SHADER_WGSL ? "vec4<f32>(" : e->target == NS_SHADER_GLSL_VULKAN ? "vec4(" : "float4(");
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
            if (sample_texture_nearest) {
                if (n->call_expr.arg_count != 1) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_sample_texture_nearest expects one float2 argument.");
                ns_shader_cstr(dst, e->target == NS_SHADER_MSL ? "ns_texture_sample_nearest(ns_texture_map, " : "ns_texture_sample_nearest(");
                ns_shader_try(ns_shader_emit_expr(e, n->next, dst));
                ns_shader_cstr(dst, ")");
                return ns_return_ok_void;
            }
            if (sample_mask) {
                if (n->call_expr.arg_count != 1) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: shader_sample_mask expects one float2 argument.");
                ns_shader_cstr(dst, e->target == NS_SHADER_MSL ? "ns_mask_sample(ns_mask_map, " : "ns_mask_sample(");
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
            ns_symbol *s = ns_null;
            if (n->call_expr.rt >= 0 && n->call_expr.rt < (i32)ns_array_length(e->vm->symbols)) {
                s = &e->vm->symbols[n->call_expr.rt];
            }
            if (!s || s->type != NS_SYMBOL_FN) s = ns_shader_find_fn(e->vm, e->ctx, name);
            if (!s || s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || !ns_shader_in_unit(e->ctx, s)) {
                snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: cannot call `%.*s` from a shader fn (not a user fn in this file).", name.len, name.data);
                return ns_return_error(void, loc, NS_ERR_EVAL, ns_shader_err);
            }
            ns_shader_str(dst, s->name);
            callee_mask = ns_shader_fn_mask(e, (i32)(s - e->vm->symbols));
            callee_storage_buffers = ns_shader_fn_storage_buffers(e, (i32)(s - e->vm->symbols));
        }
        ns_shader_cstr(dst, "(");
        i32 next = n->next;
        for (i32 a = 0; a < n->call_expr.arg_count; ++a) {
            if (a > 0) ns_shader_cstr(dst, ", ");
            ns_shader_try(ns_shader_emit_delimited_expr(e, next, dst));
            next = e->ctx->nodes[next].next;
        }
        // Pass on whatever the callee reaches through the stage intrinsics.
        ns_bool first_resource = n->call_expr.arg_count == 0;
        ns_shader_emit_resource_list(e, dst, callee_mask, false, &first_resource);
        ns_shader_emit_storage_buffer_list(e, dst, callee_storage_buffers, false, &first_resource);
        ns_shader_cstr(dst, ")");
        return ns_return_ok_void;
    }
    case NS_AST_DESIG_EXPR: return ns_shader_emit_desig(e, n, dst);
    case NS_AST_CAST_EXPR: {
        ns_return_type rt = ns_vm_parse_type_by_token(e->vm, n->cast_expr.type, loc);
        if (ns_return_is_error(rt)) return ns_return_change_type(void, rt);
        ns_type t = ns_enum_underlying_type(e->vm, rt.r);
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
    case NS_AST_INDEX_EXPR: {
        // Only the fixed-capacity local arrays declared below are indexable;
        // every other container is still rejected by its declaration.
        ns_type table = ns_shader_infer(e, n->index_expr.table);
        if (!ns_type_is_array(table)) {
            return ns_return_error(void, loc, NS_ERR_EVAL, "shader: only fixed-length local arrays can be indexed in shader fns.");
        }
        ns_shader_try(ns_shader_emit_expr(e, n->index_expr.table, dst));
        ns_shader_cstr(dst, "[");
        ns_shader_try(ns_shader_emit_expr(e, n->index_expr.expr, dst));
        ns_shader_cstr(dst, "]");
        return ns_return_ok_void;
    }
    case NS_AST_ARRAY_EXPR:
        // An array constructor only appears as a local declaration's
        // initializer, which the var-def statement emits as a declared length.
        return ns_return_error(void, loc, NS_ERR_EVAL, "shader: an array value can only initialize a local array binding in a shader fn.");
    case NS_AST_BLOCK_EXPR: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: closures are not supported in shader fns.");
    default: return ns_return_error(void, loc, NS_ERR_EVAL, "shader: unsupported expression in shader fn.");
    }
}

// Call arguments and control-flow syntax already delimit their expression. A
// binary expression also carries an outer pair to preserve its AST grouping,
// so emitting it normally produces forms such as `sqrt((a - b))` and
// `if ((a == b))`. Strip only that redundant outer pair; nested binary
// expressions keep their own grouping.
static ns_return_void ns_shader_emit_delimited_expr(ns_shader_emit *e, i32 i, ns_str *dst) {
    while (e->ctx->nodes[i].type == NS_AST_EXPR) i = e->ctx->nodes[i].expr.body;
    if (e->ctx->nodes[i].type != NS_AST_BINARY_EXPR) return ns_shader_emit_expr(e, i, dst);

    ns_str expr = {.data = ns_null, .len = 0, .dynamic = true};
    ns_return_void r = ns_shader_emit_expr(e, i, &expr);
    if (ns_return_is_error(r)) {
        ns_array_free(expr.data);
        return r;
    }
    if (expr.len >= 2 && expr.data[0] == '(' && expr.data[expr.len - 1] == ')') {
        ns_str_append_len(dst, expr.data + 1, expr.len - 2);
    } else {
        ns_shader_str(dst, expr);
    }
    ns_array_free(expr.data);
    return ns_return_ok_void;
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
        // `let name = [T](N)` with a constant N is the one container shape a
        // shader accepts: a fixed-capacity local array, which every target
        // declares by value with no initializer. Element writes come from
        // ordinary indexed assignments.
        if (n->var_def.expr != 0 && e->ctx->nodes[n->var_def.expr].type == NS_AST_ARRAY_EXPR) {
            ns_ast_t *array = &e->ctx->nodes[n->var_def.expr];
            i32 length = 0;
            ns_shader_try(ns_shader_const_i32(e, array->array_expr.count_expr, array->array_expr.literal, &length, loc));
            ns_type element = ns_shader_array_element(array->array_expr.rt);
            if (ns_type_is_unknown(element)) {
                return ns_return_error(void, loc, NS_ERR_EVAL, "shader: cannot infer the element type of a local array.");
            }
            ns_shader_try(ns_shader_collect_type(e, element, loc));

            ns_str line = {.data = ns_null, .len = 0, .dynamic = true};
            ns_return_void r = ns_return_ok_void;
            if (e->target == NS_SHADER_WGSL) {
                ns_shader_cstr(&line, "var ");
                ns_shader_str(&line, n->var_def.name.val);
                ns_shader_cstr(&line, ": array<");
                r = ns_shader_type_name(e, element, &line, loc);
                ns_shader_cstr(&line, ", ");
                ns_shader_i32(&line, length);
                ns_shader_cstr(&line, ">");
            } else {
                r = ns_shader_type_name(e, element, &line, loc);
                ns_shader_cstr(&line, " ");
                ns_shader_str(&line, n->var_def.name.val);
                ns_shader_cstr(&line, "[");
                ns_shader_i32(&line, length);
                ns_shader_cstr(&line, "]");
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
            ns_array_push(e->locals, ((ns_shader_local){.name = n->var_def.name.val, .t = array->array_expr.rt, .array_len = length}));
            return ns_return_ok_void;
        }
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
        ns_return_void r = ns_return_ok_void;
        if (e->target == NS_SHADER_WGSL) {
            ns_shader_cstr(&line, "var ");
            ns_shader_str(&line, n->var_def.name.val);
            ns_shader_cstr(&line, ": ");
            r = ns_shader_type_name(e, t, &line, loc);
        } else {
            r = ns_shader_type_name(e, t, &line, loc);
        }
        if (!ns_return_is_error(r)) {
            if (e->target != NS_SHADER_WGSL) {
                ns_shader_cstr(&line, " ");
                ns_shader_str(&line, n->var_def.name.val);
            }
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
        ns_return_void r = ns_shader_emit_delimited_expr(e, n->if_stmt.condition, &cond);
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
        ns_shader_cstr(&e->out, e->target == NS_SHADER_WGSL ? "for (var " : "for (int ");
        ns_shader_str(&e->out, name);
        if (e->target == NS_SHADER_WGSL) ns_shader_cstr(&e->out, ": i32");
        ns_shader_cstr(&e->out, " = ");
        ns_shader_str(&e->out, from);
        ns_shader_cstr(&e->out, "; ");
        ns_shader_str(&e->out, name);
        ns_shader_cstr(&e->out, " < ");
        ns_shader_str(&e->out, to);
        ns_shader_cstr(&e->out, "; ");
        if (e->target == NS_SHADER_WGSL) {
            ns_shader_str(&e->out, name);
            ns_shader_cstr(&e->out, "++");
        } else {
            ns_shader_cstr(&e->out, "++");
            ns_shader_str(&e->out, name);
        }
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
        ns_return_void r = ns_shader_emit_delimited_expr(e, n->loop_stmt.condition, &cond);
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

static i32 ns_shader_fragment_target(ns_str name) {
    if (name.len != 6 || strncmp(name.data, "color", 5) != 0 || name.data[5] < '0' || name.data[5] > '3') return -1;
    return name.data[5] - '0';
}

// A fragment output may carry one explicit hardware depth beside its colour
// targets. Keeping the spelling structural, like `position` and `color0`,
// makes depth available without adding another shader-only intrinsic.
static ns_bool ns_shader_is_fragment_depth(ns_struct_field *field) {
    return ns_str_equals(field->name, ns_str_cstr("depth")) && ns_type_is(field->t, NS_TYPE_F32);
}

static ns_bool ns_shader_is_fragment_output(ns_vm *vm, i32 st_index) {
    if (st_index < 0 || st_index >= (i32)ns_array_length(vm->symbols)) return false;
    ns_symbol *s = &vm->symbols[st_index];
    if (s->type != NS_SYMBOL_STRUCT) return false;
    if (ns_shader_is_simd(s)) return false;
    i32 field_count = (i32)ns_array_length(s->st.fields);
    if (field_count < 1 || field_count > 5) return false;
    i32 color_count = field_count;
    if (ns_shader_is_fragment_depth(&s->st.fields[field_count - 1])) color_count--;
    if (color_count < 1 || color_count > 4) return false;
    for (i32 f = 0; f < color_count; ++f) {
        ns_struct_field *field = &s->st.fields[f];
        if (ns_shader_fragment_target(field->name) != f || !ns_type_is(field->t, NS_TYPE_STRUCT)) return false;
        ns_symbol *field_type = &vm->symbols[ns_type_index(field->t)];
        if (!ns_shader_is_simd(field_type) || ns_shader_simd_dim(field_type->name) != 4) return false;
    }
    if (color_count < field_count && !ns_shader_is_fragment_depth(&s->st.fields[color_count])) return false;
    return true;
}

static ns_return_void ns_shader_emit_struct(ns_shader_emit *e, i32 st_index) {
    ns_symbol *s = &e->vm->symbols[st_index];
    // A struct imported from a lib module has its ast index in that module's
    // transient parse ctx, so the node cannot be dereferenced here; its fields
    // (names, types) live in vm->symbols and are all the emission needs.
    ns_code_loc loc = ns_shader_is_main_tu(s) ? ns_shader_loc(e, &e->ctx->nodes[s->st.ast]) : ns_code_loc_nil;
    ns_bool is_vs_in = ns_shader_index_in(e->vs_inputs, st_index);
    ns_bool is_io = ns_shader_index_in(e->stage_ios, st_index);
    ns_bool is_fragment_output = ns_shader_is_fragment_output(e->vm, st_index);

    ns_shader_cstr(&e->out, "struct ");
    ns_shader_str(&e->out, s->name);
    ns_shader_cstr(&e->out, " {\n");
    i32 texcoord = 0;
    for (i32 f = 0, l = (i32)ns_array_length(s->st.fields); f < l; ++f) {
        ns_struct_field *field = &s->st.fields[f];
        i32 fragment_target = is_fragment_output ? ns_shader_fragment_target(field->name) : -1;
        ns_bool fragment_depth = is_fragment_output && ns_shader_is_fragment_depth(field);
        ns_shader_cstr(&e->out, "    ");
        if (e->target == NS_SHADER_WGSL) {
            if (fragment_target >= 0) {
                ns_shader_cstr(&e->out, "@location("); ns_shader_i32(&e->out, fragment_target); ns_shader_cstr(&e->out, ") ");
            } else if (fragment_depth) {
                ns_shader_cstr(&e->out, "@builtin(frag_depth) ");
            } else if (is_vs_in) {
                ns_shader_cstr(&e->out, "@location("); ns_shader_i32(&e->out, f); ns_shader_cstr(&e->out, ") ");
            } else if (is_io && ns_shader_is_position_field(e->vm, field)) {
                ns_shader_cstr(&e->out, "@builtin(position) ");
            } else if (is_io) {
                ns_shader_cstr(&e->out, "@location("); ns_shader_i32(&e->out, texcoord++); ns_shader_cstr(&e->out, ") ");
            }
            ns_shader_str(&e->out, field->name);
            ns_shader_cstr(&e->out, ": ");
            ns_shader_try(ns_shader_type_name(e, field->t, &e->out, loc));
        } else {
            ns_shader_try(ns_shader_type_name(e, field->t, &e->out, loc));
            ns_shader_cstr(&e->out, " ");
            ns_shader_str(&e->out, field->name);
        }
        if (e->target == NS_SHADER_MSL) {
            if (fragment_target >= 0) {
                ns_shader_cstr(&e->out, " [[color(");
                ns_shader_i32(&e->out, fragment_target);
                ns_shader_cstr(&e->out, ")]]");
            } else if (fragment_depth) {
                ns_shader_cstr(&e->out, " [[depth(any)]]");
            } else if (is_vs_in) {
                ns_shader_cstr(&e->out, " [[attribute(");
                ns_shader_i32(&e->out, f);
                ns_shader_cstr(&e->out, ")]]");
            } else if (is_io && ns_shader_is_position_field(e->vm, field)) {
                ns_shader_cstr(&e->out, " [[position]]");
            }
        } else if (e->target == NS_SHADER_HLSL) {
            if (fragment_target >= 0) {
                ns_shader_cstr(&e->out, " : SV_Target");
                ns_shader_i32(&e->out, fragment_target);
            } else if (fragment_depth) {
                ns_shader_cstr(&e->out, " : SV_Depth");
            } else if (is_vs_in) {
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
        ns_shader_cstr(&e->out, e->target == NS_SHADER_WGSL ? ",\n" : ";\n");
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

    if (e->target == NS_SHADER_WGSL) {
        if (stage == NS_SHADER_STAGE_VERTEX) ns_shader_cstr(&e->out, "@vertex ");
        if (stage == NS_SHADER_STAGE_FRAGMENT) ns_shader_cstr(&e->out, "@fragment ");
        if (stage == NS_SHADER_STAGE_COMPUTE) ns_shader_cstr(&e->out, "@compute @workgroup_size(8, 8, 1) ");
        ns_shader_cstr(&e->out, "fn ");
        ns_shader_str(&e->out, s->name);
        ns_shader_cstr(&e->out, "(");
        for (i32 a = 0, l = (i32)ns_array_length(s->fn.args); a < l; ++a) {
            if (a > 0) ns_shader_cstr(&e->out, ", ");
            ns_shader_cstr(&e->out, "ns_arg_");
            ns_shader_str(&e->out, s->fn.args[a].name);
            ns_shader_cstr(&e->out, ": ");
            ns_shader_try(ns_shader_type_name(e, s->fn.args[a].val.t, &e->out, loc));
        }
        if (stage == NS_SHADER_STAGE_AUTO) {
            ns_bool first_resource = ns_array_length(s->fn.args) == 0;
            ns_shader_emit_resource_list(e, &e->out, ns_shader_fn_mask(e, fn_index), true, &first_resource);
        }
        if (stage == NS_SHADER_STAGE_COMPUTE && e->uses_global_id) {
            if (ns_array_length(s->fn.args) > 0) ns_shader_cstr(&e->out, ", ");
            ns_shader_cstr(&e->out, "@builtin(global_invocation_id) ns_global_id: vec3<u32>");
        }
        if (stage == NS_SHADER_STAGE_VERTEX && e->uses_vertex_id) {
            if (ns_array_length(s->fn.args) > 0) ns_shader_cstr(&e->out, ", ");
            ns_shader_cstr(&e->out, "@builtin(vertex_index) ns_vertex_id: u32");
        }
        ns_shader_cstr(&e->out, ")");
        if (!ns_type_is(s->fn.ret, NS_TYPE_VOID)) {
            ns_shader_cstr(&e->out, " -> ");
            if (stage == NS_SHADER_STAGE_FRAGMENT &&
                !(ns_type_is(s->fn.ret, NS_TYPE_STRUCT) && ns_shader_is_fragment_output(e->vm, (i32)ns_type_index(s->fn.ret)))) {
                ns_shader_cstr(&e->out, "@location(0) ");
            }
            ns_shader_try(ns_shader_type_name(e, s->fn.ret, &e->out, loc));
        }
        ns_shader_cstr(&e->out, " ");
        e->indent = 0;
        ns_ast_t *body = &e->ctx->nodes[s->fn.body];
        ns_shader_cstr(&e->out, "{\n");
        e->indent++;
        for (i32 a = 0, l = (i32)ns_array_length(s->fn.args); a < l; ++a) {
            ns_shader_pad(&e->out, e->indent);
            ns_shader_cstr(&e->out, "var ");
            ns_shader_str(&e->out, s->fn.args[a].name);
            ns_shader_cstr(&e->out, " = ns_arg_");
            ns_shader_str(&e->out, s->fn.args[a].name);
            ns_shader_cstr(&e->out, ";\n");
        }
        if (body->type == NS_AST_COMPOUND_STMT) {
            ns_ast_t *stmt = body;
            for (i32 bi = 0; bi < body->compound_stmt.count; ++bi) {
                ns_shader_try(ns_shader_emit_stmt(e, stmt->next));
                stmt = &e->ctx->nodes[stmt->next];
            }
        } else {
            ns_shader_try(ns_shader_emit_stmt(e, s->fn.body));
        }
        e->indent--;
        ns_shader_pad(&e->out, e->indent);
        ns_shader_cstr(&e->out, "}");
        ns_shader_cstr(&e->out, "\n\n");
        return ns_return_ok_void;
    }
    if (e->target == NS_SHADER_MSL) {
        if (stage == NS_SHADER_STAGE_VERTEX) ns_shader_cstr(&e->out, "vertex ");
        if (stage == NS_SHADER_STAGE_FRAGMENT) ns_shader_cstr(&e->out, "fragment ");
        if (stage == NS_SHADER_STAGE_COMPUTE) ns_shader_cstr(&e->out, "kernel ");
    }
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_COMPUTE) {
        ns_shader_cstr(&e->out, "[numthreads(8, 8, 1)]\n");
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
    ns_bool has_hidden_arg = false;
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_global_id) {
        if (ns_array_length(s->fn.args) > 0) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "uint3 ns_global_id [[thread_position_in_grid]]");
        has_hidden_arg = true;
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_VERTEX && e->uses_vertex_id) {
        if (ns_array_length(s->fn.args) > 0 || has_hidden_arg) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "uint ns_vertex_id [[vertex_id]]");
        has_hidden_arg = true;
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_write_texture) {
        if (ns_array_length(s->fn.args) > 0 || has_hidden_arg) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "texture2d<float, access::write> ns_write_texture [[texture(1)]]");
        has_hidden_arg = true;
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_write_texture_secondary) {
        if (ns_array_length(s->fn.args) > 0 || has_hidden_arg) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "texture2d<float, access::write> ns_secondary_write_texture [[texture(15)]]");
        has_hidden_arg = true;
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_read_texture) {
        if (ns_array_length(s->fn.args) > 0 || has_hidden_arg) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "texture2d<float, access::read> ns_read_texture [[texture(0)]]");
        has_hidden_arg = true;
    }
    if (e->target == NS_SHADER_MSL && stage != NS_SHADER_STAGE_AUTO && e->uses_root) {
        if (ns_array_length(s->fn.args) > 0 || has_hidden_arg) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "constant float4* ns_root [[buffer(0)]]");
        has_hidden_arg = true;
    }
    if (e->target == NS_SHADER_MSL && stage != NS_SHADER_STAGE_AUTO) {
        for (i32 i = 0, l = (i32)ns_array_length(e->storage_buffers); i < l; ++i) {
            i32 index = e->storage_buffers[i];
            if (ns_array_length(s->fn.args) > 0 || has_hidden_arg) ns_shader_cstr(&e->out, ", ");
            ns_shader_cstr(&e->out, ns_shader_storage_is_const(e, index) ? "device const int* ns_storage_buffer_" : "device int* ns_storage_buffer_");
            ns_shader_i32(&e->out, index);
            ns_shader_cstr(&e->out, " [[buffer(");
            ns_shader_i32(&e->out, NS_SHADER_STORAGE_BINDING_BASE + index);
            ns_shader_cstr(&e->out, ")]]");
            has_hidden_arg = true;
        }
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_FRAGMENT && e->uses_shadow_map) {
        ns_shader_cstr(&e->out, ", depth2d<float> ns_shadow_map [[texture(0)]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_FRAGMENT && e->uses_texture_map) {
        ns_shader_cstr(&e->out, ", texture2d<float> ns_texture_map [[texture(1)]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_FRAGMENT && e->uses_mask_map) {
        ns_shader_cstr(&e->out, ", texture2d<float> ns_mask_map [[texture(2)]]");
    }
    if (e->target == NS_SHADER_MSL && stage == NS_SHADER_STAGE_VERTEX && e->uses_scene_uniforms) {
        ns_shader_cstr(&e->out, ", constant ns_scene_uniforms& ns_uniforms [[buffer(1)]]");
    }
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_COMPUTE && e->uses_global_id) {
        if (ns_array_length(s->fn.args) > 0) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "uint3 ns_global_id : SV_DispatchThreadID");
    }
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_VERTEX && e->uses_vertex_id) {
        if (ns_array_length(s->fn.args) > 0) ns_shader_cstr(&e->out, ", ");
        ns_shader_cstr(&e->out, "uint ns_vertex_id : SV_VertexID");
    }
    // A helper fn takes the resources it reaches as plain parameters, in the
    // same order its call sites pass them.
    if (stage == NS_SHADER_STAGE_AUTO) {
        ns_bool first = ns_array_length(s->fn.args) == 0;
        ns_shader_emit_resource_list(e, &e->out, ns_shader_fn_mask(e, fn_index), true, &first);
        ns_shader_emit_storage_buffer_list(e, &e->out, ns_shader_fn_storage_buffers(e, fn_index), true, &first);
    }
    ns_shader_cstr(&e->out, ")");
    if (e->target == NS_SHADER_HLSL && stage == NS_SHADER_STAGE_FRAGMENT &&
        !(ns_type_is(s->fn.ret, NS_TYPE_STRUCT) && ns_shader_is_fragment_output(e->vm, (i32)ns_type_index(s->fn.ret)))) {
        ns_shader_cstr(&e->out, " : SV_Target");
    }
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
        ns_shader_cstr(&e->out, "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n\nvoid main() {\n    ");
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
        // Fragment varyings in; the stage-io position field (if any) is fed
        // from gl_FragCoord. A color0..color3 output struct maps to MRT slots.
        ns_bool mrt = ns_type_is(s->fn.ret, NS_TYPE_STRUCT) && ns_shader_is_fragment_output(e->vm, (i32)ns_type_index(s->fn.ret));
        ns_symbol *out_st = mrt ? &e->vm->symbols[ns_type_index(s->fn.ret)] : ns_null;
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
        if (mrt) {
            for (i32 f = 0, l = (i32)ns_array_length(out_st->st.fields); f < l; ++f) {
                i32 target = ns_shader_fragment_target(out_st->st.fields[f].name);
                if (target < 0) continue;
                ns_shader_cstr(&e->out, "layout(location = ");
                ns_shader_i32(&e->out, target);
                ns_shader_cstr(&e->out, ") out vec4 ns_frag_color");
                ns_shader_i32(&e->out, target);
                ns_shader_cstr(&e->out, ";\n");
            }
        } else {
            ns_shader_cstr(&e->out, "layout(location = 0) out vec4 ns_frag_color;\n");
        }
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
        ns_shader_cstr(&e->out, ");\n    ");
        if (mrt) {
            ns_shader_str(&e->out, out_st->name);
            ns_shader_cstr(&e->out, " ns_ret = ");
            ns_shader_str(&e->out, s->name);
            ns_shader_cstr(&e->out, "(ns_in);\n");
            for (i32 f = 0, l = (i32)ns_array_length(out_st->st.fields); f < l; ++f) {
                i32 target = ns_shader_fragment_target(out_st->st.fields[f].name);
                if (target < 0) {
                    if (ns_shader_is_fragment_depth(&out_st->st.fields[f])) {
                        ns_shader_cstr(&e->out, "    gl_FragDepth = ns_ret.depth;\n");
                    }
                    continue;
                }
                ns_shader_cstr(&e->out, "    ns_frag_color");
                ns_shader_i32(&e->out, target);
                ns_shader_cstr(&e->out, " = ns_ret.");
                ns_shader_str(&e->out, out_st->st.fields[f].name);
                ns_shader_cstr(&e->out, ";\n");
            }
            ns_shader_cstr(&e->out, "}\n");
        } else {
            ns_shader_cstr(&e->out, "ns_frag_color = ");
            ns_shader_str(&e->out, s->name);
            ns_shader_cstr(&e->out, "(ns_in);\n}\n");
        }
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

    if (entry->stage == NS_SHADER_STAGE_VERTEX && ns_array_length(s->fn.args) == 0) {
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
        if (!ns_shader_index_in(e->stage_ios, io_index)) ns_array_push(e->stage_ios, io_index);
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
        if (!ret_ok && ns_type_is(s->fn.ret, NS_TYPE_STRUCT)) {
            ret_ok = ns_shader_is_fragment_output(e->vm, (i32)ns_type_index(s->fn.ret));
        }
        if (!ret_ok) return ns_return_error(void, loc, NS_ERR_EVAL, "shader: a fragment entry must return float4 or a color0..color3 float4 output struct with optional trailing `depth: f32`.");
        if (!ns_shader_index_in(e->stage_ios, in_index)) ns_array_push(e->stage_ios, in_index);
    }
    return ns_return_ok_void;
}

// ---------------------------------------------------------------------------
// program transpile
// ---------------------------------------------------------------------------
ns_return_str ns_shader_transpile_program(ns_vm *vm, ns_ast_ctx *ctx, ns_shader_entry_desc *entries, i32 count, ns_shader_target target) {
    if (target == NS_SHADER_TARGET_UNKNOWN) {
        return ns_return_error(str, ns_code_loc_nil, NS_ERR_EVAL, "shader: unknown target, expected msl | glsl | hlsl | wgsl.");
    }
    if (count <= 0) return ns_return_error(str, ns_code_loc_nil, NS_ERR_EVAL, "shader: no entry fns to transpile.");
    if (target == NS_SHADER_GLSL_VULKAN && count > 1) {
        return ns_return_error(str, ns_code_loc_nil, NS_ERR_EVAL, "shader: glsl emits one source per stage; transpile entries one at a time.");
    }

    // `lit` bindings fold into the emitted source, so their values must exist
    // before any expression is emitted. Callers that only type-check the main
    // translation unit (the shader CLI, transpiler tests) never evaluate them.
    // Their semantic checker excludes calls and mutable data, so evaluating a
    // literal constant expression here has no runtime side effects.
    for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
        i32 node = ctx->sections[i];
        if (ctx->nodes[node].type != NS_AST_VAR_DEF || !ctx->nodes[node].var_def.is_lit) continue;
        ns_return_value evaluated = ns_eval_var_def(vm, ctx, node);
        if (ns_return_is_error(evaluated)) return ns_return_change_type(str, evaluated);
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
        ns_shader_free_fn_uses(e.fn_uses);                                                                                                         \
        ns_array_free(e.entries);                                                                                                                    \
        ns_array_free(e.vs_inputs);                                                                                                                  \
        ns_array_free(e.stage_ios);                                                                                                                  \
        ns_array_free(e.locals);                                                                                                                     \
        ns_array_free(e.storage_buffers);                                                                                                            \
        ns_array_free(e.storage_writes);                                                                                                             \
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
        if (s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || s->fn.body == 0 || !ns_shader_in_unit(ctx, s)) {
            snprintf(ns_shader_err, sizeof(ns_shader_err), "shader: `%.*s` is not a transpilable fn (must be a non-ref fn defined in the transpiled file).",
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
    if (target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out, "\n");
        if (e.uses_write_texture) ns_shader_cstr(&e.out, "requires texture_formats_tier1;\n\n");
    }
    if (e.uses_read_texture && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out, "layout(set = 0, binding = 0, r11f_g11f_b10f) uniform readonly image2D ns_read_texture;\n\n");
    }
    if (e.uses_read_texture && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out, "Texture2D<float4> ns_read_texture : register(t0);\n\n");
    }
    if (e.uses_read_texture && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out, "@group(0) @binding(0) var ns_read_texture: texture_2d<f32>;\n\n");
    }
    if (e.uses_write_texture && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out, "layout(set = 0, binding = 1, r11f_g11f_b10f) uniform writeonly image2D ns_write_texture;\n\n");
    }
    if (e.uses_write_texture && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out, "RWTexture2D<float4> ns_write_texture : register(u1);\n\n");
    }
    if (e.uses_write_texture && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out, "@group(0) @binding(1) var ns_write_texture: texture_storage_2d<rg11b10ufloat, write>;\n\n");
    }
    if (e.uses_write_texture_secondary && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out, "layout(set = 0, binding = 15, rgba8) uniform writeonly image2D ns_secondary_write_texture;\n\n");
    }
    if (e.uses_write_texture_secondary && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out, "RWTexture2D<float4> ns_secondary_write_texture : register(u15);\n\n");
    }
    if (e.uses_write_texture_secondary && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out, "@group(0) @binding(15) var ns_secondary_write_texture: texture_storage_2d<rgba8unorm, write>;\n\n");
    }
    for (i32 i = 0, l = (i32)ns_array_length(e.storage_buffers); i < l; ++i) {
        i32 index = e.storage_buffers[i];
        ns_bool read_only = ns_shader_storage_is_const(&e, index);
        if (target == NS_SHADER_GLSL_VULKAN) {
            ns_shader_cstr(&e.out, "layout(set = 0, binding = ");
            ns_shader_i32(&e.out, NS_SHADER_STORAGE_BINDING_BASE + index);
            ns_shader_cstr(&e.out, ", std430) ");
            if (read_only) ns_shader_cstr(&e.out, "readonly ");
            ns_shader_cstr(&e.out, "buffer ns_storage_block_");
            ns_shader_i32(&e.out, index);
            ns_shader_cstr(&e.out, " { int values[]; } ns_storage_");
            ns_shader_i32(&e.out, index);
            ns_shader_cstr(&e.out, ";\n#define ns_storage_buffer_");
            ns_shader_i32(&e.out, index);
            ns_shader_cstr(&e.out, " ns_storage_");
            ns_shader_i32(&e.out, index);
            ns_shader_cstr(&e.out, ".values\n\n");
        }
        if (target == NS_SHADER_HLSL) {
            // A read-only buffer stays a UAV: dropping the RW would move it to
            // the SRV register space and change the binding the host sets up.
            ns_shader_cstr(&e.out, "RWByteAddressBuffer ns_storage_buffer_");
            ns_shader_i32(&e.out, index);
            ns_shader_cstr(&e.out, " : register(u");
            ns_shader_i32(&e.out, NS_SHADER_STORAGE_BINDING_BASE + index);
            ns_shader_cstr(&e.out, ");\n\n");
        }
        if (target == NS_SHADER_WGSL) {
            ns_shader_cstr(&e.out, "@group(0) @binding(");
            ns_shader_i32(&e.out, NS_SHADER_WGSL_STORAGE_BINDING_BASE + index);
            ns_shader_cstr(&e.out, read_only ? ") var<storage, read> ns_storage_buffer_" : ") var<storage, read_write> ns_storage_buffer_");
            ns_shader_i32(&e.out, index);
            ns_shader_cstr(&e.out, ": array<i32>;\n\n");
        }
    }
    if (e.uses_root && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out, "inline float ns_root_f32(constant float4* root, int index) { return root[index / 4][index % 4]; }\n\n");
    }
    if (e.uses_root && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out, "layout(set = 0, binding = 2, std140) uniform ns_root_block { vec4 values[");
        ns_shader_i32(&e.out, NS_SHADER_ROOT_BLOCK_VEC4S);
        ns_shader_cstr(&e.out,
            "]; } ns_root;\n"
            "float ns_root_f32(int index) { return ns_root.values[index / 4][index % 4]; }\n\n");
    }
    if (e.uses_root && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out, "cbuffer ns_root : register(b2) { float4 ns_root_values[");
        ns_shader_i32(&e.out, NS_SHADER_ROOT_BLOCK_VEC4S);
        ns_shader_cstr(&e.out,
            "]; };\n"
            "float ns_root_f32(int index) { return ns_root_values[index / 4][index % 4]; }\n\n");
    }
    if (e.uses_root && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out, "struct ns_root_block { values: array<vec4<f32>, ");
        ns_shader_i32(&e.out, NS_SHADER_ROOT_BLOCK_VEC4S);
        ns_shader_cstr(&e.out,
            ">, };\n"
            "@group(0) @binding(2) var<uniform> ns_root: ns_root_block;\n"
            "fn ns_root_f32(index: i32) -> f32 { return ns_root.values[u32(index) / 4u][u32(index) % 4u]; }\n\n");
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
    if (e.uses_scene_uniforms && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out,
            "struct ns_scene_uniforms {\n"
            "    model: mat4x4<f32>,\n"
            "    view_projection: mat4x4<f32>,\n"
            "    light_view_projection: mat4x4<f32>,\n"
            "    params: vec4<f32>,\n"
            "};\n"
            "@group(0) @binding(2) var<uniform> ns_uniforms: ns_scene_uniforms;\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out,
            "inline float ns_shadow_compare(depth2d<float> map, float3 coord) {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) return 1.0;\n"
            "    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear, compare_func::less_equal);\n"
            "    return map.sample_compare(s, coord.xy, coord.z);\n"
            "}\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out,
            "layout(set = 0, binding = 0) uniform sampler2DShadow ns_shadow_map;\n"
            "float ns_shadow_compare(vec3 coord) {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) return 1.0;\n"
            "    return texture(ns_shadow_map, coord);\n"
            "}\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out,
            "Texture2D<float> ns_shadow_map : register(t0);\n"
            "SamplerComparisonState ns_shadow_sampler : register(s0);\n"
            "float ns_shadow_compare(float3 coord) {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) return 1.0;\n"
            "    return ns_shadow_map.SampleCmpLevelZero(ns_shadow_sampler, coord.xy, coord.z);\n"
            "}\n\n");
    }
    if (e.uses_shadow_map && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out,
            "@group(0) @binding(0) var ns_shadow_map: texture_depth_2d;\n"
            "@group(0) @binding(1) var ns_shadow_sampler: sampler_comparison;\n"
            "fn ns_shadow_compare(coord: vec3<f32>) -> f32 {\n"
            "    if (coord.x <= 0.0 || coord.x >= 1.0 || coord.y <= 0.0 || coord.y >= 1.0 || coord.z <= 0.0 || coord.z >= 1.0) { return 1.0; }\n"
            "    return textureSampleCompare(ns_shadow_map, ns_shadow_sampler, coord.xy, coord.z);\n"
            "}\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out,
            "inline float4 ns_texture_sample(texture2d<float> map, float2 coord) {\n"
            "    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);\n"
            "    return map.sample(s, coord);\n"
            "}\n"
            // A normalized coordinate spans the texture, so the texel it names is
            // floor(coord * size); the last texel is reached only by clamping the
            // result. Scaling by size - 1 instead spreads that one texel of slack
            // across the whole axis, and the accumulated half texel tips the floor
            // over exactly halfway along it: every fetch past the middle comes back
            // one texel early, so the two rows and the two columns either side of
            // the centre read the same texel and a one-to-one copy grows a seam
            // down the middle of the image.
            "inline float4 ns_texture_sample_nearest(texture2d<float> map, float2 coord) {\n"
            "    float2 size = float2(map.get_width(), map.get_height());\n"
            "    float2 texel = clamp(coord, float2(0.0), float2(1.0)) * size;\n"
            "    uint2 pixel = uint2(min(texel, size - 1.0));\n"
            "    return map.read(pixel);\n"
            "}\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out,
            "layout(set = 0, binding = 1) uniform sampler2D ns_texture_map;\n"
            "vec4 ns_texture_sample(vec2 coord) { return texture(ns_texture_map, coord); }\n"
            "vec4 ns_texture_sample_nearest(vec2 coord) {\n"
            "    ivec2 size = textureSize(ns_texture_map, 0);\n"
            "    vec2 texel = clamp(coord, vec2(0.0), vec2(1.0)) * vec2(size);\n"
            "    ivec2 pixel = min(ivec2(texel), size - ivec2(1));\n"
            "    return texelFetch(ns_texture_map, pixel, 0);\n"
            "}\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out,
            "Texture2D<float4> ns_texture_map : register(t1);\n"
            "float4 ns_texture_sample(float2 coord) {\n"
            "    uint w, h; ns_texture_map.GetDimensions(w, h);\n"
            "    float2 texel = saturate(coord) * float2(w, h);\n"
            "    return ns_texture_map.Load(int3(min(int2(texel), int2(w - 1, h - 1)), 0));\n"
            "}\n"
            "float4 ns_texture_sample_nearest(float2 coord) {\n"
            "    return ns_texture_sample(coord);\n"
            "}\n\n");
    }
    if (e.uses_texture_map && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out,
            "@group(0) @binding(3) var ns_texture_map: texture_2d<f32>;\n"
            "@group(0) @binding(4) var ns_texture_sampler: sampler;\n"
            "fn ns_texture_sample(coord: vec2<f32>) -> vec4<f32> {\n"
            "    return textureSampleLevel(ns_texture_map, ns_texture_sampler, coord, 0.0);\n"
            "}\n"
            "fn ns_texture_sample_nearest(coord: vec2<f32>) -> vec4<f32> {\n"
            "    let size = vec2<i32>(textureDimensions(ns_texture_map));\n"
            "    let bounded = clamp(coord, vec2<f32>(0.0), vec2<f32>(1.0));\n"
            "    let pixel = min(vec2<i32>(bounded * vec2<f32>(size)), size - vec2<i32>(1));\n"
            "    return textureLoad(ns_texture_map, pixel, 0);\n"
            "}\n\n");
    }
    if (e.uses_mask_map && target == NS_SHADER_MSL) {
        ns_shader_cstr(&e.out,
            "inline float4 ns_mask_sample(texture2d<float> map, float2 coord) {\n"
            "    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);\n"
            "    return map.sample(s, coord);\n"
            "}\n\n");
    }
    if (e.uses_mask_map && target == NS_SHADER_GLSL_VULKAN) {
        ns_shader_cstr(&e.out,
            "layout(set = 0, binding = 2) uniform sampler2D ns_mask_map;\n"
            "vec4 ns_mask_sample(vec2 coord) { return texture(ns_mask_map, coord); }\n\n");
    }
    if (e.uses_mask_map && target == NS_SHADER_HLSL) {
        ns_shader_cstr(&e.out,
            "Texture2D<float4> ns_mask_map : register(t2);\n"
            "float4 ns_mask_sample(float2 coord) {\n"
            "    uint w, h; ns_mask_map.GetDimensions(w, h);\n"
            "    return ns_mask_map.Load(int3(int2(saturate(coord) * float2(w - 1, h - 1)), 0));\n"
            "}\n\n");
    }
    if (e.uses_mask_map && target == NS_SHADER_WGSL) {
        ns_shader_cstr(&e.out,
            "@group(0) @binding(5) var ns_mask_map: texture_2d<f32>;\n"
            "@group(0) @binding(6) var ns_mask_sampler: sampler;\n"
            "fn ns_mask_sample(coord: vec2<f32>) -> vec4<f32> {\n"
            "    return textureSampleLevel(ns_mask_map, ns_mask_sampler, coord, 0.0);\n"
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

    ns_str out = target == NS_SHADER_WGSL ? ns_shader_escape_wgsl_identifiers(e.out) : e.out;
    ns_array_free(e.pre.data);
    ns_array_free(e.structs);
    ns_array_free(e.fns);
    ns_array_free(e.fn_visit);
    ns_shader_free_fn_uses(e.fn_uses);
    ns_array_free(e.entries);
    ns_array_free(e.vs_inputs);
    ns_array_free(e.stage_ios);
    ns_array_free(e.locals);
    ns_array_free(e.storage_buffers);
    ns_array_free(e.storage_writes);
    return ns_return_ok(str, out);
}

ns_return_str ns_shader_transpile(ns_vm *vm, ns_ast_ctx *ctx, i32 fn_index, ns_shader_target target, ns_shader_stage stage) {
    ns_shader_entry_desc entry = {.fn_index = fn_index, .stage = stage};
    return ns_shader_transpile_program(vm, ctx, &entry, 1, target);
}

// ---------------------------------------------------------------------------
// `mod shader` intrinsic dispatch (mirrors ns_vm_call_std)
// ---------------------------------------------------------------------------
// Packed component count of one vertex-input field: f32 -> 1, float2/3/4 ->
// 2/3/4. Vertex buffers driven through the scalar-ID gpu helpers are tightly
// packed 32-bit float data, so any other field type is rejected.
static ns_return_bool ns_shader_vertex_field_components(ns_vm *vm, ns_struct_field *field, i32 *dim) {
    ns_type t = ns_enum_underlying_type(vm, field->t);
    if (ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64)) {
        *dim = 1;
        return ns_return_ok(bool, true);
    }
    if (ns_type_is(t, NS_TYPE_STRUCT)) {
        ns_symbol *s = &vm->symbols[ns_type_index(t)];
        if (ns_shader_is_simd(s)) {
            i32 d = ns_shader_simd_dim(s->name);
            if (d >= 2 && d <= 4) {
                *dim = d;
                return ns_return_ok(bool, true);
            }
        }
    }
    return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: vertex input fields must be f32 or float2/3/4 to reflect a vertex layout.");
}

typedef struct ns_shader_group_resource {
    u64 object;
    i32 slot;
    ns_value_type value_type;
    ns_bool texture;
} ns_shader_group_resource;

static ns_struct_field *ns_shader_struct_field_named(ns_symbol *st, const char *name) {
    ns_str field_name = ns_str_cstr((i8 *)name);
    for (i32 i = 0, l = (i32)ns_array_length(st->st.fields); i < l; ++i) {
        if (ns_str_equals(st->st.fields[i].name, field_name)) return &st->st.fields[i];
    }
    return ns_null;
}

// Resource definitions are recognized structurally; no application texture
// name or slot is built into the shader runtime.
static ns_return_bool ns_shader_group_resource_at(ns_vm *vm, ns_value group, i32 index,
                                                   ns_shader_group_resource *out) {
    if (!ns_type_is(group.t, NS_TYPE_STRUCT)) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                               "shader: a bind group must be a strongly typed user struct.");
    }
    ns_symbol *group_st = &vm->symbols[ns_type_index(group.t)];
    i32 count = (i32)ns_array_length(group_st->st.fields);
    if (index < 0 || index >= count) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                               "shader: bind-group resource index out of range.");
    }

    ns_struct_field *group_field = &group_st->st.fields[index];
    if (!ns_type_is(group_field->t, NS_TYPE_STRUCT)) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                               "shader: every bind-group field must be a typed resource definition struct.");
    }
    ns_symbol *resource_st = &vm->symbols[ns_type_index(group_field->t)];
    ns_struct_field *object = ns_shader_struct_field_named(resource_st, "object");
    ns_struct_field *slot = ns_shader_struct_field_named(resource_st, "slot");
    ns_struct_field *value_type = ns_shader_struct_field_named(resource_st, "value_type");
    ns_bool texture = object && ns_type_is(object->t, NS_TYPE_U32);
    ns_bool buffer = object && ns_type_is(object->t, NS_TYPE_U64);
    if ((!texture && !buffer) || !slot || !ns_type_is(slot->t, NS_TYPE_I32) ||
        !value_type || !ns_type_is(value_type->t, NS_TYPE_TYPE)) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                               "shader: resource definition requires object: u32|u64, slot: i32, value_type: type.");
    }

    i8 *group_data = ns_type_in_stack(group.t) ? &vm->stack[group.o] : (i8 *)group.o;
    i8 *resource_data = group_data + group_field->o;
    ns_shader_group_resource resource = {0};
    resource.texture = texture;
    memcpy(&resource.slot, resource_data + slot->o, sizeof(resource.slot));
    i32 type_id = 0;
    memcpy(&type_id, resource_data + value_type->o, sizeof(type_id));
    if (type_id < NS_TYPE_I8 || type_id > NS_TYPE_BOOL) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                               "shader: resource value_type must be a concrete builtin scalar type.");
    }
    resource.value_type = (ns_value_type)type_id;
    if (resource.slot < 0) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                               "shader: resource slot must be non-negative.");
    }
    if (texture) {
        u32 id = 0;
        memcpy(&id, resource_data + object->o, sizeof(id));
        resource.object = id;
    } else {
        memcpy(&resource.object, resource_data + object->o, sizeof(resource.object));
    }

    // Buffer and texture slots are separate namespaces.
    for (i32 i = 0; i < index; ++i) {
        ns_shader_group_resource previous = {0};
        ns_return_bool decoded = ns_shader_group_resource_at(vm, group, i, &previous);
        if (ns_return_is_error(decoded)) return decoded;
        if (previous.texture == resource.texture && previous.slot == resource.slot) {
            return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                                   "shader: duplicate resource slot in bind group.");
        }
    }
    *out = resource;
    return ns_return_ok(bool, true);
}

// ---------------------------------------------------------------------------
// host execution of compute fns
// ---------------------------------------------------------------------------
//
// The stage intrinsics a compute fn calls are ordinary VM fns, so binding the
// resources they read lets the interpreter run the fn one invocation at a
// time. Textures are plain f32 arrays of four components per texel.
typedef struct ns_shader_host {
    f32 *read;
    f32 *write;
    f32 *write_secondary;
    i32 width, height;
    f32 root[NS_SHADER_ROOT_WORDS];
    i32 global_id[3];
    ns_bool bound;
} ns_shader_host;

static ns_shader_host _host = {0};

static i32 ns_shader_host_texel(i32 x, i32 y) {
    if (!_host.bound || x < 0 || y < 0 || x >= _host.width || y >= _host.height) return -1;
    return (y * _host.width + x) * 4;
}

static ns_return_bool ns_shader_host_vm_call(ns_vm *vm, ns_str name, ns_call *call) {
    if (ns_str_equals(name, ns_str_cstr("shader_host_bind"))) {
        f32 *read = (f32 *)ns_eval_array_raw(vm, vm->symbol_stack[call->arg_offset].val);
        f32 *write = (f32 *)ns_eval_array_raw(vm, vm->symbol_stack[call->arg_offset + 1].val);
        i32 width = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 2].val);
        i32 height = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 3].val);
        szt texels = (szt)(width > 0 ? width : 0) * (szt)(height > 0 ? height : 0);
        ns_bool ok = read && write && width > 0 && height > 0 &&
                     ns_array_length(read) >= texels * 4 && ns_array_length(write) >= texels * 4;
        if (ok) {
            _host.read = read;
            _host.write = write;
            _host.write_secondary = ns_null;
            _host.width = width;
            _host.height = height;
            _host.bound = true;
        }
        call->ret = (ns_value){.t = ns_type_bool, .b = ok};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_host_bind_secondary"))) {
        f32 *write = (f32 *)ns_eval_array_raw(vm, vm->symbol_stack[call->arg_offset].val);
        szt texels = (szt)(_host.width > 0 ? _host.width : 0) * (szt)(_host.height > 0 ? _host.height : 0);
        ns_bool ok = _host.bound && write && ns_array_length(write) >= texels * 4;
        if (ok) _host.write_secondary = write;
        call->ret = (ns_value){.t = ns_type_bool, .b = ok};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_host_root"))) {
        f32 *words = (f32 *)ns_eval_array_raw(vm, vm->symbol_stack[call->arg_offset].val);
        ns_bool ok = words && ns_array_length(words) >= NS_SHADER_ROOT_WORDS;
        if (ok) memcpy(_host.root, words, sizeof(_host.root));
        call->ret = (ns_value){.t = ns_type_bool, .b = ok};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_host_invocation"))) {
        for (i32 i = 0; i < 3; ++i) {
            _host.global_id[i] = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + i].val);
        }
        call->ret = ns_nil;
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_host_swap"))) {
        f32 *read = _host.read;
        _host.read = _host.write;
        _host.write = read;
        call->ret = ns_nil;
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_host_release"))) {
        memset(&_host, 0, sizeof(_host));
        call->ret = ns_nil;
        return ns_return_ok(bool, true);
    }

    ns_bool global_x = ns_str_equals(name, ns_str_cstr("shader_global_id_x"));
    ns_bool global_y = ns_str_equals(name, ns_str_cstr("shader_global_id_y"));
    ns_bool global_z = ns_str_equals(name, ns_str_cstr("shader_global_id_z"));
    if (global_x || global_y || global_z) {
        call->ret = (ns_value){.t = ns_type_i32, .i32 = _host.global_id[global_x ? 0 : global_y ? 1 : 2]};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_root_f32"))) {
        i32 index = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset].val);
        f32 word = index >= 0 && index < NS_SHADER_ROOT_WORDS ? _host.root[index] : 0.0f;
        call->ret = (ns_value){.t = ns_type_f32, .f32 = word};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_buffer_i32"))) {
        call->ret = (ns_value){.t = ns_type_i32, .i32 = 0};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_buffer_store_i32"))) {
        call->ret = ns_nil;
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_read_texture")) || ns_str_equals(name, ns_str_cstr("shader_write_texture")) ||
        ns_str_equals(name, ns_str_cstr("shader_write_texture_secondary"))) {
        if (!_host.bound) {
            return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                                   "shader: texture intrinsics need shader_host_bind outside a transpiled shader.");
        }
        i32 x = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset].val);
        i32 y = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 1].val);
        i32 texel = ns_shader_host_texel(x, y);
        ns_type float4_t = call->callee->fn.ret;
        if (ns_str_equals(name, ns_str_cstr("shader_write_texture")) || ns_str_equals(name, ns_str_cstr("shader_write_texture_secondary"))) {
            ns_value color = vm->symbol_stack[call->arg_offset + 2].val;
            f32 *destination = ns_str_equals(name, ns_str_cstr("shader_write_texture_secondary")) ? _host.write_secondary : _host.write;
            if (!destination) return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: secondary write texture is not bound.");
            if (texel >= 0) {
                // A float4 is four f32 fields in declaration order.
                for (i32 i = 0; i < 4; ++i) {
                    ns_value field = (ns_value){.t = ns_type_set_stack(ns_type_f32, true), .o = color.o + (u64)i * sizeof(f32)};
                    destination[texel + i] = ns_eval_number_f32(vm, field);
                }
            }
            call->ret = ns_nil;
            return ns_return_ok(bool, true);
        }
        i32 size = 4 * (i32)sizeof(f32);
        ns_value out = (ns_value){.t = ns_type_set_stack(float4_t, true), .o = ns_eval_alloc(vm, size)};
        for (i32 i = 0; i < 4; ++i) {
            f32 value = texel >= 0 ? _host.read[texel + i] : 0.0f;
            memcpy(&vm->stack[out.o + (u64)i * sizeof(f32)], &value, sizeof(f32));
        }
        ns_array_set_length(vm->stack, out.o + (u64)size);
        call->ret = out;
        return ns_return_ok(bool, true);
    }
    return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "unknown shader fn.");
}

ns_return_bool ns_shader_vm_call(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_str name = call->callee->name;

    if (ns_str_starts_with(name, ns_str_cstr("shader_host_")) || ns_str_starts_with(name, ns_str_cstr("shader_global_id_")) ||
        ns_str_equals(name, ns_str_cstr("shader_root_f32")) || ns_str_equals(name, ns_str_cstr("shader_read_texture")) ||
        ns_str_equals(name, ns_str_cstr("shader_write_texture")) || ns_str_equals(name, ns_str_cstr("shader_write_texture_secondary")) || ns_str_equals(name, ns_str_cstr("shader_buffer_i32")) ||
        ns_str_equals(name, ns_str_cstr("shader_buffer_store_i32"))) {
        return ns_shader_host_vm_call(vm, name, call);
    }

    if (ns_str_equals(name, ns_str_cstr("ddx")) || ns_str_equals(name, ns_str_cstr("ddy"))) {
        // Fragment derivatives have no host meaning; a CPU call is a zero.
        call->ret = (ns_value){.t = ns_type_f32, .f32 = 0.0f};
        return ns_return_ok(bool, true);
    }
    if (ns_str_equals(name, ns_str_cstr("shader_discard"))) {
        call->ret = (ns_value){.t = ns_type_void};
        return ns_return_ok(bool, true);
    }

    if (ns_str_equals(name, ns_str_cstr("shader_source_hash"))) {
        ns_str source = ns_eval_str(vm, vm->symbol_stack[call->arg_offset].val);
        call->ret = (ns_value){.t = ns_type_u64, .u64 = ns_shader_source_hash(source)};
        return ns_return_ok(bool, true);
    }

    ns_bool is_group_count = ns_str_equals(name, ns_str_cstr("shader_group_binding_count"));
    ns_bool is_group_object = ns_str_equals(name, ns_str_cstr("shader_group_binding_object"));
    ns_bool is_group_slot = ns_str_equals(name, ns_str_cstr("shader_group_binding_slot"));
    ns_bool is_group_value_type = ns_str_equals(name, ns_str_cstr("shader_group_binding_value_type"));
    ns_bool is_group_texture = ns_str_equals(name, ns_str_cstr("shader_group_binding_is_texture"));
    if (is_group_count || is_group_object || is_group_slot || is_group_value_type || is_group_texture) {
        ns_value group = vm->symbol_stack[call->arg_offset].val;
        if (!ns_type_is(group.t, NS_TYPE_STRUCT)) {
            return ns_return_error(bool, vm->loc, NS_ERR_EVAL,
                                   "shader: a bind group must be a strongly typed user struct.");
        }
        ns_symbol *group_st = &vm->symbols[ns_type_index(group.t)];
        i32 count = (i32)ns_array_length(group_st->st.fields);
        for (i32 i = 0; i < count; ++i) {
            ns_shader_group_resource ignored = {0};
            ns_return_bool decoded = ns_shader_group_resource_at(vm, group, i, &ignored);
            if (ns_return_is_error(decoded)) return decoded;
        }
        if (is_group_count) {
            call->ret = (ns_value){.t = ns_type_i32, .i32 = count};
            return ns_return_ok(bool, true);
        }
        i32 index = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 1].val);
        ns_shader_group_resource resource = {0};
        ns_return_bool decoded = ns_shader_group_resource_at(vm, group, index, &resource);
        if (ns_return_is_error(decoded)) return decoded;
        if (is_group_object) call->ret = (ns_value){.t = ns_type_u64, .u64 = resource.object};
        else if (is_group_slot) call->ret = (ns_value){.t = ns_type_i32, .i32 = resource.slot};
        else if (is_group_value_type) call->ret = (ns_value){.t = ns_type_type, .i32 = (i32)resource.value_type};
        else call->ret = (ns_value){.t = ns_type_bool, .b = resource.texture};
        return ns_return_ok(bool, true);
    }

    ns_bool is_name = ns_str_equals(name, ns_str_cstr("shader_name"));
    ns_bool is_transpile = ns_str_equals(name, ns_str_cstr("shader_transpile"));
    ns_bool is_transpile_stage = ns_str_equals(name, ns_str_cstr("shader_transpile_stage"));
    ns_bool is_entry = ns_str_equals(name, ns_str_cstr("shader_entry"));
    ns_bool is_vertex_stride = ns_str_equals(name, ns_str_cstr("shader_vertex_stride"));
    ns_bool is_attr_count = ns_str_equals(name, ns_str_cstr("shader_vertex_attr_count"));
    ns_bool is_attr_offset = ns_str_equals(name, ns_str_cstr("shader_vertex_attr_offset"));
    ns_bool is_attr_size = ns_str_equals(name, ns_str_cstr("shader_vertex_attr_size"));
    if (!is_name && !is_transpile && !is_transpile_stage && !is_entry && !is_vertex_stride && !is_attr_count && !is_attr_offset && !is_attr_size) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "unknown shader fn.");
    }

    ns_value fv = vm->symbol_stack[call->arg_offset].val;
    if (!ns_type_is(fv.t, NS_TYPE_FN)) {
        return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: the first argument must be a fn.");
    }
    i32 fn_index = (i32)ns_type_index(fv.t);

    // Transpile/reflection walk the target fn's AST, which lives in the
    // context the fn was parsed from — not necessarily the caller's (e.g. a
    // script fn handed to a lib fn like gpu_shader_graphics).
    {
        ns_symbol *fsym = &vm->symbols[fn_index];
        if (fsym->type == NS_SYMBOL_FN && fsym->fn.ctx) ctx = fsym->fn.ctx;
    }

    if (is_name) {
        call->ret = (ns_value){.t = ns_type_str, .o = ns_vm_push_string(vm, vm->symbols[fn_index].name)};
        return ns_return_ok(bool, true);
    }

    if (is_vertex_stride || is_attr_count || is_attr_offset || is_attr_size) {
        ns_symbol *s = &vm->symbols[fn_index];
        if (s->type != NS_SYMBOL_FN || (i32)ns_array_length(s->fn.args) != 1 || !ns_type_is(s->fn.args[0].val.t, NS_TYPE_STRUCT) ||
            ns_shader_is_simd(&vm->symbols[ns_type_index(s->fn.args[0].val.t)])) {
            return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: vertex layout reflection needs a vertex fn taking one user struct parameter.");
        }
        ns_symbol *in = &vm->symbols[ns_type_index(s->fn.args[0].val.t)];
        i32 count = (i32)ns_array_length(in->st.fields);
        i32 attr = -1;
        if (is_attr_offset || is_attr_size) {
            attr = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset + 1].val);
            if (attr < 0 || attr >= count) {
                return ns_return_error(bool, vm->loc, NS_ERR_EVAL, "shader: vertex attribute index out of range.");
            }
        }
        i32 offset = 0, result = count;
        for (i32 f = 0; f < count; ++f) {
            i32 dim = 0;
            ns_return_bool rd = ns_shader_vertex_field_components(vm, &in->st.fields[f], &dim);
            if (ns_return_is_error(rd)) return rd;
            if (f == attr) result = is_attr_offset ? offset : dim;
            offset += dim * 4;
        }
        if (is_vertex_stride) result = offset;
        call->ret = (ns_value){.t = ns_type_i32, .i32 = result};
        return ns_return_ok(bool, true);
    }

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
