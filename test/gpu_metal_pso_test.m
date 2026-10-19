// A shader drawn under several render states keeps one pipeline per state:
// alternating between them, as a shadow pass and the main pass do every
// frame, must reuse the pipelines instead of rebuilding one per draw. The
// backend is included whole so its per-shader cache can be inspected.
#include "../lib/src/gpu.metal.m"

static const char *test_vs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 test_vs(uint vid [[vertex_id]]) { return float4(float(vid), 0.0, 0.0, 1.0); }\n";
static const char *test_fs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "fragment float4 test_fs() { return float4(1.0); }\n";

static void test_pass(MTLPixelFormat color, MTLPixelFormat depth) {
    memset(_state.v2_pass_colors, 0, sizeof(_state.v2_pass_colors));
    _state.v2_pass_colors[0] = color;
    _state.v2_pass_depth = depth;
    _state.v2_pass_stencil = MTLPixelFormatInvalid;
}

int main(void) {
    @autoreleasepool {
        _state.device.device = MTLCreateSystemDefaultDevice();
        if (!_state.device.device) {
            printf("gpu_metal_pso_test: no Metal device, skipped\n");
            return 0;
        }
        _state.shader_count = 1; // 0 is the null shader, as after device creation
        u32 shader_id = mtl_v2_shader_graphics_create(test_vs, test_fs, "test_vs", "test_fs", "", 0);
        gpu_shader_mtl *shader = &_state.shaders[shader_id];
        assert(shader_id != 0 && shader->vertex_func && shader->fragment_func);

        gpu_v2_state_desc opaque;
        memset(&opaque, 0, sizeof(opaque));
        opaque.color_mask = 0xf;
        opaque.blend_preset = GPU_BLEND_OFF;
        gpu_v2_state_desc blended = opaque;
        blended.blend_preset = GPU_BLEND_ALPHA;

        // Main pass, opaque.
        _state.v2_render_state = opaque;
        test_pass(MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float);
        id<MTLRenderPipelineState> main_pso = mtl_v2_ensure_pipeline(shader);
        assert(main_pso && shader->v2_pso_count == 1);

        // Depth-only shadow pass: different pass formats.
        test_pass(MTLPixelFormatInvalid, MTLPixelFormatDepth16Unorm);
        id<MTLRenderPipelineState> shadow_pso = mtl_v2_ensure_pipeline(shader);
        assert(shadow_pso && shadow_pso != main_pso && shader->v2_pso_count == 2);

        // Main pass, blended: a different render state.
        _state.v2_render_state = blended;
        test_pass(MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float);
        id<MTLRenderPipelineState> blend_pso = mtl_v2_ensure_pipeline(shader);
        assert(blend_pso && blend_pso != main_pso && shader->v2_pso_count == 3);

        // Depth compare and write live in the depth-stencil state: no new pipeline.
        blended.depth_compare = COMPARE_GREATER;
        blended.depth_write = true;
        _state.v2_render_state = blended;
        assert(mtl_v2_ensure_pipeline(shader) == blend_pso && shader->v2_pso_count == 3);

        // Frame after frame, the same three states reuse the same pipelines.
        for (int frame = 0; frame < 4; ++frame) {
            _state.v2_render_state = opaque;
            test_pass(MTLPixelFormatInvalid, MTLPixelFormatDepth16Unorm);
            assert(mtl_v2_ensure_pipeline(shader) == shadow_pso);
            test_pass(MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float);
            assert(mtl_v2_ensure_pipeline(shader) == main_pso);
            _state.v2_render_state = blended;
            assert(mtl_v2_ensure_pipeline(shader) == blend_pso);
        }
        assert(shader->v2_pso_count == 3);

        // More states than slots: the cache stays bounded, and the newest
        // state is still served from it.
        id<MTLRenderPipelineState> last = nil;
        for (u32 mask = 1; mask <= GPU_MTL_PSO_SLOTS + 2; ++mask) {
            opaque.color_mask = mask & 0xf;
            opaque.cull_mode = (i32)(mask >> 4);
            _state.v2_render_state = opaque;
            last = mtl_v2_ensure_pipeline(shader);
            assert(last);
        }
        assert(shader->v2_pso_count == GPU_MTL_PSO_SLOTS);
        assert(mtl_v2_ensure_pipeline(shader) == last);

        mtl_v2_shader_destroy(shader_id);
        assert(shader->v2_pso_count == 0);
    }
    printf("gpu_metal_pso_test: passed\n");
    return 0;
}
