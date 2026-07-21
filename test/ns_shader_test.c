#include "ns_test.h"
#include "ns_shader.h"

#if NS_WIN
    #include <io.h>
    #define ns_shader_test_dup _dup
    #define ns_shader_test_dup2 _dup2
    #define ns_shader_test_close _close
    #define ns_shader_test_fileno _fileno
    #define ns_shader_test_null "NUL"
#else
    #include <unistd.h>
    #define ns_shader_test_dup dup
    #define ns_shader_test_dup2 dup2
    #define ns_shader_test_close close
    #define ns_shader_test_fileno fileno
    #define ns_shader_test_null "/dev/null"
#endif

// Transpiles the gui.ns-style vertex/fragment pair (over `use simd` structs) to
// every target and checks the stage-specific markers, then drives the runtime
// `use shader` intrinsics end to end. Run from the repo root so the debug
// NS_REF_PATH ("lib") resolves lib/simd.ns and lib/shader.ns.

static const char *ns_shader_test_src =
    "use std\n"
    "use simd\n"
    "use shader\n"
    "enum shade: u8 { dark = 2, light, }\n"
    "struct VertexData {\n"
    "    position: float3,\n"
    "    uv: float2\n"
    "}\n"
    "struct FragmentInput {\n"
    "    position: float4,\n"
    "    uv: float2,\n"
    "    color: float4\n"
    "}\n"
    "fn brighten(c: float4, gain: f32) float4 {\n"
    "    return float4 { x: c.x * gain, y: c.y * gain, z: c.z * gain, w: c.w }\n"
    "}\n"
    "fn half_gain(a: f32) f32 {\n"
    "    return a + 0.25h + 0.25hb\n"
    "}\n"
    "fn enum_gain() f32 {\n"
    "    return shade.light as f32\n"
    "}\n"
    "fn vs_main(data: VertexData) FragmentInput {\n"
    "    let pos = data.position\n"
    "    return FragmentInput {\n"
    "        position: float4 { x: pos.x, y: pos.y, z: pos.z, w: 1.0 },\n"
    "        uv: data.uv,\n"
    "        color: float4 { x: 1.0, y: 1.0, z: 1.0, w: 1.0 },\n"
    "    }\n"
    "}\n"
    "fn fs_main(data: FragmentInput) float4 {\n"
    "    return brighten(data.color, half_gain(0.5 as f32) + enum_gain())\n"
    "}\n"
    "fn fs_shadow(data: FragmentInput) float4 {\n"
    "    let uv = data.uv\n"
    "    let position = data.position\n"
    "    let visibility = shader_sample_shadow(float3 { uv.x, uv.y, position.z })\n"
    "    return brighten(data.color, visibility)\n"
    "}\n"
    "fn vs_scene(data: VertexData) FragmentInput {\n"
    "    let normal = shader_transform_normal(data.position)\n"
    "    let shadow_position = shader_shadow_clip_position(data.position)\n"
    "    return FragmentInput {\n"
    "        position: shader_transform_position(data.position),\n"
    "        uv: data.uv,\n"
    "        color: float4 { x: normal.x, y: normal.y, z: shadow_position.z, w: shader_scene_selected() },\n"
    "    }\n"
    "}\n"
    "fn fs_texture(data: FragmentInput) float4 {\n"
    "    let texture_color = shader_sample_texture(data.uv)\n"
    "    let gain = shader_scene_selected() + shader_scene_textured() + shader_scene_receives_shadow()\n"
    "    return brighten(texture_color, gain)\n"
    "}\n"
    "fn cs_main() void {\n"
    "    let n = 40 + 2\n"
    "}\n"
    "fn cs_texture() void {\n"
    "    let x = shader_global_id_x()\n"
    "    let y = shader_global_id_y()\n"
    "    let z = shader_global_id_z()\n"
    "    shader_write_texture(x, y, float4 { x: 0.25, y: 0.5, z: 0.75, w: (z + 1) as f32 })\n"
    "}\n"
    "fn bad_print(data: FragmentInput) float4 {\n"
    "    print(\"no\")\n"
    "    return data.color\n"
    "}\n"
    "fn plain(a: f32) f32 {\n"
    "    return a\n"
    "}\n";

static ns_bool ns_shader_test_has(ns_str hay, const char *needle) { return ns_str_index_of(hay, ns_str_cstr(needle)) != -1; }

static i32 ns_shader_test_silence_stderr(void) {
    fflush(stderr);
    i32 saved = ns_shader_test_dup(ns_shader_test_fileno(stderr));
    FILE *null = fopen(ns_shader_test_null, "w");
    if (saved >= 0 && null) ns_shader_test_dup2(ns_shader_test_fileno(null), ns_shader_test_fileno(stderr));
    if (null) fclose(null);
    return saved;
}

static void ns_shader_test_restore_stderr(i32 saved) {
    fflush(stderr);
    if (saved >= 0) {
        ns_shader_test_dup2(saved, ns_shader_test_fileno(stderr));
        ns_shader_test_close(saved);
    }
}

#define ns_shader_expect_transpile_error(call, msg)                                                                                                 \
    do {                                                                                                                                             \
        i32 stderr_saved = ns_shader_test_silence_stderr();                                                                                          \
        ns_return_str ret = (call);                                                                                                                  \
        ns_shader_test_restore_stderr(stderr_saved);                                                                                                 \
        ns_expect(ns_return_is_error(ret), msg);                                                                                                     \
    } while (0)

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

    ns_expect(ns_shader_eval_bool(
                  "use simd\n"
                  "fn main() bool {\n"
                  "    let v = float2 { x: 1.0, y: 2.0 }\n"
                  "    let q: quatf = float4 { x: 0.0, y: 0.0, z: 0.0, w: 1.0 }\n"
                  "    let m = mat4 { col0: q, col1: q, col2: q, col3: q }\n"
                  "    return v.x == 1.0 && q.w == 1.0\n"
                  "}\n"),
              "simd exposes snake-case float vectors, quatf, and mat4.");

    ns_vm vm = {0};
    ns_ast_ctx ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&ctx, ns_str_cstr((i8 *)ns_shader_test_src), ns_str_cstr("<ns_shader_test>"));
    ns_expect(!ns_return_is_error(parsed) && parsed.r, "shader test source parses.");
    ns_return_bool vm_ok = ns_vm_parse(&vm, &ctx);
    ns_expect(!ns_return_is_error(vm_ok) && vm_ok.r, "shader test source type-checks.");

    i32 vs = ns_shader_test_fn(&vm, "vs_main");
    i32 fs = ns_shader_test_fn(&vm, "fs_main");
    i32 fs_shadow = ns_shader_test_fn(&vm, "fs_shadow");
    i32 vs_scene = ns_shader_test_fn(&vm, "vs_scene");
    i32 fs_texture = ns_shader_test_fn(&vm, "fs_texture");
    i32 cs = ns_shader_test_fn(&vm, "cs_main");
    i32 cs_texture = ns_shader_test_fn(&vm, "cs_texture");
    ns_expect(vs >= 0 && fs >= 0 && fs_shadow >= 0 && vs_scene >= 0 && fs_texture >= 0 && cs >= 0 && cs_texture >= 0,
              "shader entry symbols exist.");

    // --- target/stage helpers ---
    ns_expect(ns_shader_target_from_str(ns_str_cstr("msl")) == NS_SHADER_MSL, "target msl resolves.");
    ns_expect(ns_shader_target_from_str(ns_str_cstr("glsl")) == NS_SHADER_GLSL_VULKAN, "target glsl resolves.");
    ns_expect(ns_shader_target_from_str(ns_str_cstr("hlsl")) == NS_SHADER_HLSL, "target hlsl resolves.");
    ns_expect(ns_shader_target_from_str(ns_str_cstr("wgsl")) == NS_SHADER_TARGET_UNKNOWN, "unknown target rejected.");
    ns_expect(ns_shader_stage_infer(&vm, &ctx, vs) == NS_SHADER_STAGE_VERTEX, "vs_main infers vertex stage.");
    ns_expect(ns_shader_stage_infer(&vm, &ctx, fs) == NS_SHADER_STAGE_FRAGMENT, "fs_main infers fragment stage.");
    ns_expect(ns_shader_stage_infer(&vm, &ctx, cs) == NS_SHADER_STAGE_COMPUTE, "cs_main infers compute stage.");
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
            ns_expect(ns_shader_test_has(r.r, "float4(pos.x, pos.y, pos.z, 1.0)"), "msl float4 literal becomes a constructor.");
            ns_expect(ns_shader_test_has(r.r, "float4 brighten(float4 c, float gain)"), "msl helper fn emitted.");
            ns_expect(ns_shader_test_has(r.r, "half(0.25)"), "msl half and brain-float suffixes lower to half.");
            ns_expect(ns_shader_test_has(r.r, "float(3)"), "msl enum members lower to their integer constants.");
            ns_expect(ns_shader_test_has(r.r, "float3 pos = data.position"), "msl let binding types are inferred.");
            ns_array_free(r.r.data);
        }
    }

    // --- GPU-resident scene intrinsics: matrices, flags, and material texture ---
    {
        ns_return_str r = ns_shader_transpile(&vm, &ctx, vs_scene, NS_SHADER_MSL, NS_SHADER_STAGE_VERTEX);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "struct ns_scene_uniforms") &&
                      ns_shader_test_has(r.r, "[[buffer(1)]]") && ns_shader_test_has(r.r, "ns_uniforms.view_projection") &&
                      ns_shader_test_has(r.r, "ns_uniforms.light_view_projection") && ns_shader_test_has(r.r, "ns_uniforms.model"),
                  "msl scene vertex intrinsics use the shared transform uniform buffer.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, vs_scene, NS_SHADER_GLSL_VULKAN, NS_SHADER_STAGE_VERTEX);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "uniform ns_scene_uniform_block") &&
                      ns_shader_test_has(r.r, "ns_uniforms.view_projection") && ns_shader_test_has(r.r, "ns_uniforms.model"),
                  "glsl scene vertex intrinsics use the shared transform uniform block.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, vs_scene, NS_SHADER_HLSL, NS_SHADER_STAGE_VERTEX);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "cbuffer ns_uniforms") &&
                      ns_shader_test_has(r.r, "mul(ns_view_projection") && ns_shader_test_has(r.r, "mul(ns_model"),
                  "hlsl scene vertex intrinsics use the shared transform constant buffer.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, fs_texture, NS_SHADER_MSL, NS_SHADER_STAGE_FRAGMENT);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "texture2d<float> ns_texture_map [[texture(1)]]") &&
                      ns_shader_test_has(r.r, "ns_texture_sample(ns_texture_map") && ns_shader_test_has(r.r, "ns_uniforms.params"),
                  "msl material texture and scene flags lower to GPU resources.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);
    }

    // --- shadow-map intrinsic: resource declaration + 3x3 PCF lowering ---
    {
        ns_return_str r = ns_shader_transpile(&vm, &ctx, fs_shadow, NS_SHADER_MSL, NS_SHADER_STAGE_FRAGMENT);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "depth2d<float> ns_shadow_map [[texture(0)]]") &&
                      ns_shader_test_has(r.r, "sample_compare") && ns_shader_test_has(r.r, "compare_func::less_equal") &&
                      ns_shader_test_has(r.r, "lit / 9.0"),
                  "msl shadow sampling binds a comparison depth texture and emits PCF.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, fs_shadow, NS_SHADER_GLSL_VULKAN, NS_SHADER_STAGE_FRAGMENT);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "uniform sampler2DShadow ns_shadow_map") &&
                      ns_shader_test_has(r.r, "texture(ns_shadow_map, vec3") && ns_shader_test_has(r.r, "lit / 9.0"),
                  "glsl shadow sampling uses comparison sampling with PCF.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, fs_shadow, NS_SHADER_HLSL, NS_SHADER_STAGE_FRAGMENT);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "SamplerComparisonState ns_shadow_sampler") &&
                      ns_shader_test_has(r.r, "SampleCmpLevelZero") && ns_shader_test_has(r.r, "lit / 9.0"),
                  "hlsl shadow sampling uses comparison sampling with PCF.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);
    }

    // --- compute: zero-parameter void fns become native compute entries ---
    {
        ns_return_str r = ns_shader_transpile(&vm, &ctx, cs, NS_SHADER_MSL, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "kernel void cs_main()"), "msl compute entry transpiles.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, cs, NS_SHADER_HLSL, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "[numthreads(1, 1, 1)]"), "hlsl compute entry transpiles.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, cs, NS_SHADER_GLSL_VULKAN, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "layout(local_size_x = 1") &&
                      ns_shader_test_has(r.r, "cs_main();"),
                  "glsl compute entry transpiles.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);
    }

    // --- compute texture output and invocation coordinates ---
    {
        ns_return_str r = ns_shader_transpile(&vm, &ctx, cs_texture, NS_SHADER_MSL, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "[[thread_position_in_grid]]") &&
                      ns_shader_test_has(r.r, "texture2d<float, access::write>") && ns_shader_test_has(r.r, "ns_write_texture.write("),
                  "msl compute texture intrinsics transpile.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, cs_texture, NS_SHADER_HLSL, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "RWTexture2D<float4>") &&
                      ns_shader_test_has(r.r, "SV_DispatchThreadID") && ns_shader_test_has(r.r, "ns_write_texture[int2("),
                  "hlsl compute texture intrinsics transpile.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);

        r = ns_shader_transpile(&vm, &ctx, cs_texture, NS_SHADER_GLSL_VULKAN, NS_SHADER_STAGE_AUTO);
        ns_expect(!ns_return_is_error(r) && ns_shader_test_has(r.r, "writeonly image2D ns_write_texture") &&
                      ns_shader_test_has(r.r, "gl_GlobalInvocationID.x") && ns_shader_test_has(r.r, "imageStore("),
                  "glsl compute texture intrinsics transpile.");
        if (!ns_return_is_error(r)) ns_array_free(r.r.data);
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
            ns_expect(ns_shader_test_has(r.r, "half(0.25)"), "hlsl half and brain-float suffixes lower to half.");
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
        ns_shader_expect_transpile_error(ns_shader_transpile_program(&vm, &ctx, entries, 2, NS_SHADER_GLSL_VULKAN),
                                         "glsl rejects multi-entry programs.");
    }

    // --- error paths ---
    {
        i32 bad = ns_shader_test_fn(&vm, "bad_print");
        ns_shader_expect_transpile_error(ns_shader_transpile(&vm, &ctx, bad, NS_SHADER_MSL, NS_SHADER_STAGE_FRAGMENT),
                                         "fn calling print fails to transpile.");

        i32 plain = ns_shader_test_fn(&vm, "plain");
        ns_shader_expect_transpile_error(ns_shader_transpile(&vm, &ctx, plain, NS_SHADER_MSL, NS_SHADER_STAGE_AUTO),
                                         "uninferable stage fails.");

        ns_shader_expect_transpile_error(ns_shader_transpile(&vm, &ctx, vs, NS_SHADER_TARGET_UNKNOWN, NS_SHADER_STAGE_AUTO),
                                         "unknown target fails.");
    }

    // --- runtime dispatch through `use shader` ---
    {
        const char *src =
            "use simd\n"
            "use shader\n"
            "struct VertexData { position: float3, uv: float2 }\n"
            "struct FragmentInput { position: float4, uv: float2, color: float4 }\n"
            "fn vs_main(data: VertexData) FragmentInput {\n"
            "    let pos = data.position\n"
            "    return FragmentInput {\n"
            "        position: float4 { x: pos.x, y: pos.y, z: pos.z, w: 1.0 },\n"
            "        uv: data.uv,\n"
            "        color: float4 { x: 1.0, y: 1.0, z: 1.0, w: 1.0 },\n"
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

    // --- vertex-layout reflection intrinsics over the vertex input struct ---
    {
        const char *src =
            "use simd\n"
            "use shader\n"
            "struct VertexData { position: float3, uv: float2, weight: f32 }\n"
            "struct FragmentInput { position: float4, uv: float2 }\n"
            "fn vs_main(data: VertexData) FragmentInput {\n"
            "    let pos = data.position\n"
            "    return FragmentInput {\n"
            "        position: float4 { x: pos.x, y: pos.y, z: pos.z, w: data.weight },\n"
            "        uv: data.uv,\n"
            "    }\n"
            "}\n"
            "fn main() bool {\n"
            "    let stride_ok = shader_vertex_stride(vs_main) == 24 && shader_vertex_attr_count(vs_main) == 3\n"
            "    let offsets_ok = shader_vertex_attr_offset(vs_main, 0) == 0 && shader_vertex_attr_offset(vs_main, 1) == 12 && shader_vertex_attr_offset(vs_main, 2) == 20\n"
            "    let sizes_ok = shader_vertex_attr_size(vs_main, 0) == 3 && shader_vertex_attr_size(vs_main, 1) == 2 && shader_vertex_attr_size(vs_main, 2) == 1\n"
            "    return stride_ok && offsets_ok && sizes_ok\n"
            "}\n";
        ns_expect(ns_shader_eval_bool(src), "vertex-layout reflection packs f32/float2/3/4 fields.");
    }

    // --- gpu module owns the dispatch_gpu wrapper and reaches the dynamically
    // loaded backend; without a requested device submission returns false ---
    {
        const char *src =
            "use gpu\n"
            "fn cs_noop() void {}\n"
            "fn main() bool {\n"
            "    let target = gpu_shader_target()\n"
            "    let source = shader_transpile_stage(cs_noop, target, \"compute\")\n"
            "    let entry = shader_entry(cs_noop, target)\n"
            "    return !dispatch_gpu(cs_noop, 1, 1, 1) && !gpu_dispatch_compute_texture_source(source, entry, 0u, 1, 1, 1)\n"
            "}\n";
        ns_expect(ns_shader_eval_bool(src), "gpu compute dispatch paths transpile code and call the gpu dylib backend.");
    }

    return 0;
}
