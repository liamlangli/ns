#include "ns_test.h"
#include "ns_shader.h"

// Transpiles the gui.ns-style vertex/fragment pair (over `use simd` structs) to
// every target and checks the stage-specific markers, then drives the runtime
// `use shader` intrinsics end to end. Run from the repo root so the debug
// NS_REF_PATH ("lib") resolves lib/simd.ns and lib/shader.ns.

static const char *ns_shader_test_src =
    "use std\n"
    "use simd\n"
    "struct VertexData {\n"
    "    position: Float3,\n"
    "    uv: Float2\n"
    "}\n"
    "struct FragmentInput {\n"
    "    position: Float4,\n"
    "    uv: Float2,\n"
    "    color: Float4\n"
    "}\n"
    "fn brighten(c: Float4, gain: f32) Float4 {\n"
    "    return Float4 { x: c.x * gain, y: c.y * gain, z: c.z * gain, w: c.w }\n"
    "}\n"
    "fn vs_main(data: VertexData) FragmentInput {\n"
    "    let pos = data.position\n"
    "    return FragmentInput {\n"
    "        position: Float4 { x: pos.x, y: pos.y, z: pos.z, w: 1.0 },\n"
    "        uv: data.uv,\n"
    "        color: Float4 { x: 1.0, y: 1.0, z: 1.0, w: 1.0 },\n"
    "    }\n"
    "}\n"
    "fn fs_main(data: FragmentInput) Float4 {\n"
    "    return brighten(data.color, 0.5 as f32)\n"
    "}\n"
    "fn bad_print(data: FragmentInput) Float4 {\n"
    "    print(\"no\")\n"
    "    return data.color\n"
    "}\n"
    "fn plain(a: f32) f32 {\n"
    "    return a\n"
    "}\n";

static ns_bool ns_shader_test_has(ns_str hay, const char *needle) { return ns_str_index_of(hay, ns_str_cstr(needle)) != -1; }

static i32 ns_shader_test_fn(ns_vm *vm, const char *name) {
    for (i32 i = 0, l = (i32)ns_array_length(vm->symbols); i < l; ++i) {
        if (vm->symbols[i].type == NS_SYMBOL_FN && ns_str_equals(vm->symbols[i].name, ns_str_cstr(name))) return i;
    }
    return -1;
}

static ns_bool ns_shader_eval_bool(const char *src) {
    ns_vm vm = {0};
    ns_return_value r = ns_eval(&vm, ns_str_cstr((i8 *)src), ns_str_cstr("<ns_shader_test>"));
    if (ns_return_is_error(r)) {
        ns_warn("ns_shader_test", "eval error: %.*s\n", r.e.msg.len, r.e.msg.data);
        return false;
    }
    return ns_eval_bool(&vm, r.r);
}

int main() {
    // error paths return ns_return_error, which asserts in debug builds unless
    // the repl-recover escape hatch is set
    setenv("NS_REPL_RECOVER", "1", 1);

    ns_vm vm = {0};
    ns_ast_ctx ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&ctx, ns_str_cstr((i8 *)ns_shader_test_src), ns_str_cstr("<ns_shader_test>"));
    ns_expect(!ns_return_is_error(parsed) && parsed.r, "shader test source parses.");
    ns_return_bool vm_ok = ns_vm_parse(&vm, &ctx);
    ns_expect(!ns_return_is_error(vm_ok) && vm_ok.r, "shader test source type-checks.");

    i32 vs = ns_shader_test_fn(&vm, "vs_main");
    i32 fs = ns_shader_test_fn(&vm, "fs_main");
    ns_expect(vs >= 0 && fs >= 0, "vs_main and fs_main symbols exist.");

    // --- target/stage helpers ---
    ns_expect(ns_shader_target_from_str(ns_str_cstr("msl")) == NS_SHADER_MSL, "target msl resolves.");
    ns_expect(ns_shader_target_from_str(ns_str_cstr("glsl")) == NS_SHADER_GLSL_VULKAN, "target glsl resolves.");
    ns_expect(ns_shader_target_from_str(ns_str_cstr("hlsl")) == NS_SHADER_HLSL, "target hlsl resolves.");
    ns_expect(ns_shader_target_from_str(ns_str_cstr("wgsl")) == NS_SHADER_TARGET_UNKNOWN, "unknown target rejected.");
    ns_expect(ns_shader_stage_infer(&vm, &ctx, vs) == NS_SHADER_STAGE_VERTEX, "vs_main infers vertex stage.");
    ns_expect(ns_shader_stage_infer(&vm, &ctx, fs) == NS_SHADER_STAGE_FRAGMENT, "fs_main infers fragment stage.");
    ns_str glsl_entry = ns_shader_entry_name(NS_SHADER_GLSL_VULKAN, ns_str_cstr("vs_main"));
    ns_expect(ns_str_equals(glsl_entry, ns_str_cstr("main")), "glsl entry name is main.");
    ns_str msl_entry = ns_shader_entry_name(NS_SHADER_MSL, ns_str_cstr("vs_main"));
    ns_expect(ns_str_equals(msl_entry, ns_str_cstr("vs_main")), "msl entry name is the fn name.");

    // --- MSL: both stages in one source ---
    {
        ns_shader_entry_desc entries[2] = {{.fn_index = vs, .stage = NS_SHADER_STAGE_AUTO}, {.fn_index = fs, .stage = NS_SHADER_STAGE_AUTO}};
        ns_return_str r = ns_shader_transpile_program(&vm, &ctx, entries, 2, NS_SHADER_MSL);
        ns_expect(!ns_return_is_error(r), "msl program transpiles.");
        if (!ns_return_is_error(r)) {
            ns_expect(ns_shader_test_has(r.r, "#include <metal_stdlib>"), "msl header present.");
            ns_expect(ns_shader_test_has(r.r, "vertex FragmentInput vs_main(VertexData data [[stage_in]])"), "msl vertex entry signature.");
            ns_expect(ns_shader_test_has(r.r, "fragment float4 fs_main(FragmentInput data [[stage_in]])"), "msl fragment entry signature.");
            ns_expect(ns_shader_test_has(r.r, "[[attribute(0)]]"), "msl vertex input attributes.");
            ns_expect(ns_shader_test_has(r.r, "float4 position [[position]]"), "msl stage-io position attribute.");
            ns_expect(ns_shader_test_has(r.r, "float4(pos.x, pos.y, pos.z, 1.0)"), "msl Float4 literal becomes a constructor.");
            ns_expect(ns_shader_test_has(r.r, "float4 brighten(float4 c, float gain)"), "msl helper fn emitted.");
            ns_expect(ns_shader_test_has(r.r, "float3 pos = data.position"), "msl let binding types are inferred.");
            ns_array_free(r.r.data);
        }
    }

    // --- HLSL: both stages in one source ---
    {
        ns_shader_entry_desc entries[2] = {{.fn_index = vs, .stage = NS_SHADER_STAGE_AUTO}, {.fn_index = fs, .stage = NS_SHADER_STAGE_AUTO}};
        ns_return_str r = ns_shader_transpile_program(&vm, &ctx, entries, 2, NS_SHADER_HLSL);
        ns_expect(!ns_return_is_error(r), "hlsl program transpiles.");
        if (!ns_return_is_error(r)) {
            ns_expect(ns_shader_test_has(r.r, "float3 position : POSITION"), "hlsl vertex input semantics.");
            ns_expect(ns_shader_test_has(r.r, "float2 uv : TEXCOORD0"), "hlsl texcoord semantics.");
            ns_expect(ns_shader_test_has(r.r, "float4 position : SV_Position"), "hlsl stage-io position semantic.");
            ns_expect(ns_shader_test_has(r.r, "float4 fs_main(FragmentInput data) : SV_Target"), "hlsl fragment entry signature.");
            ns_array_free(r.r.data);
        }
    }

    // --- GLSL: one source per stage ---
    {
        ns_return_str r = ns_shader_transpile(&vm, &ctx, vs, NS_SHADER_GLSL_VULKAN, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r), "glsl vertex stage transpiles.");
        if (!ns_return_is_error(r)) {
            ns_expect(ns_shader_test_has(r.r, "#version 450"), "glsl version header.");
            ns_expect(ns_shader_test_has(r.r, "layout(location = 0) in vec3 ns_in_position"), "glsl vertex attributes.");
            ns_expect(ns_shader_test_has(r.r, "layout(location = 0) out vec2 ns_out_uv"), "glsl varyings.");
            ns_expect(ns_shader_test_has(r.r, "gl_Position = ns_ret.position"), "glsl wrapper writes gl_Position.");
            ns_expect(ns_shader_test_has(r.r, "vec4(pos.x, pos.y, pos.z, 1.0)"), "glsl vec4 constructor.");
            ns_array_free(r.r.data);
        }

        r = ns_shader_transpile(&vm, &ctx, fs, NS_SHADER_GLSL_VULKAN, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r), "glsl fragment stage transpiles.");
        if (!ns_return_is_error(r)) {
            ns_expect(ns_shader_test_has(r.r, "layout(location = 0) out vec4 ns_frag_color"), "glsl fragment output.");
            ns_expect(ns_shader_test_has(r.r, "gl_FragCoord"), "glsl fragment position feeds from gl_FragCoord.");
            ns_expect(ns_shader_test_has(r.r, "ns_frag_color = fs_main(ns_in)"), "glsl wrapper calls the ns fn.");
            ns_array_free(r.r.data);
        }

        ns_shader_entry_desc entries[2] = {{.fn_index = vs, .stage = NS_SHADER_STAGE_AUTO}, {.fn_index = fs, .stage = NS_SHADER_STAGE_AUTO}};
        ns_return_str multi = ns_shader_transpile_program(&vm, &ctx, entries, 2, NS_SHADER_GLSL_VULKAN);
        ns_expect(ns_return_is_error(multi), "glsl rejects multi-entry programs.");
    }

    // --- error paths ---
    {
        i32 bad = ns_shader_test_fn(&vm, "bad_print");
        ns_return_str r = ns_shader_transpile(&vm, &ctx, bad, NS_SHADER_MSL, NS_SHADER_STAGE_FRAGMENT);
        ns_expect(ns_return_is_error(r), "fn calling print fails to transpile.");

        i32 plain = ns_shader_test_fn(&vm, "plain");
        r = ns_shader_transpile(&vm, &ctx, plain, NS_SHADER_MSL, NS_SHADER_STAGE_AUTO);
        ns_expect(ns_return_is_error(r), "uninferable stage fails.");

        r = ns_shader_transpile(&vm, &ctx, vs, NS_SHADER_TARGET_UNKNOWN, NS_SHADER_STAGE_AUTO);
        ns_expect(ns_return_is_error(r), "unknown target fails.");
    }

    // --- runtime dispatch through `use shader` ---
    {
        const char *src =
            "use simd\n"
            "use shader\n"
            "struct VertexData { position: Float3, uv: Float2 }\n"
            "struct FragmentInput { position: Float4, uv: Float2, color: Float4 }\n"
            "fn vs_main(data: VertexData) FragmentInput {\n"
            "    let pos = data.position\n"
            "    return FragmentInput {\n"
            "        position: Float4 { x: pos.x, y: pos.y, z: pos.z, w: 1.0 },\n"
            "        uv: data.uv,\n"
            "        color: Float4 { x: 1.0, y: 1.0, z: 1.0, w: 1.0 },\n"
            "    }\n"
            "}\n"
            "fn main() bool {\n"
            "    let msl = shader_transpile(vs_main, \"msl\")\n"
            "    let glsl = shader_transpile_stage(vs_main, \"glsl\", \"vertex\")\n"
            "    let entry = shader_entry(vs_main, \"glsl\")\n"
            "    return msl.len > 0 && glsl.len > 0 && entry == \"main\"\n"
            "}\n";
        ns_expect(ns_shader_eval_bool(src), "runtime shader_transpile via use shader.");
    }

    return 0;
}
