// GPU device management — platform fallback.
//
// A real GPU backend is selected per platform in gpu.h: Metal on Apple
// (gpu.metal.m), DirectX 12 on Windows (gpu.dx12.c). On platforms without a
// backend (currently Linux) this file provides no-op definitions for the whole
// gpu.h API so the statically linked standard library keeps resolving every
// gpu_* symbol registered in src/ns_vm_lib.c. ns programs that drive the GPU
// still load and run here; they simply render nothing.
#include "gpu.h"

#if !defined(NS_GPU_METAL) && !defined(NS_GPU_DX12)

ns_bool gpu_request_device(view *v) {
    ns_unused(v);
    // No GPU backend wired up on this platform yet.
    return false;
}

void gpu_destroy_device(void) {}

gpu_texture gpu_create_texture(gpu_texture_desc *desc) { ns_unused(desc); return (gpu_texture){0}; }
gpu_sampler gpu_create_sampler(gpu_sampler_desc *desc) { ns_unused(desc); return (gpu_sampler){0}; }
gpu_buffer gpu_create_buffer_desc(gpu_buffer_desc *desc) { ns_unused(desc); return (gpu_buffer){0}; }
gpu_shader gpu_create_shader(gpu_shader_desc *desc) { ns_unused(desc); return (gpu_shader){0}; }
gpu_pipeline gpu_create_pipeline(gpu_pipeline_desc *desc) { ns_unused(desc); return (gpu_pipeline){0}; }
gpu_binding gpu_create_binding(gpu_binding_desc *desc) { ns_unused(desc); return (gpu_binding){0}; }
gpu_mesh gpu_create_mesh(gpu_mesh_desc *desc) { ns_unused(desc); return (gpu_mesh){0}; }
gpu_render_pass gpu_create_render_pass(gpu_render_pass_desc *desc) { ns_unused(desc); return (gpu_render_pass){0}; }

void gpu_destroy_texture(gpu_texture texture) { ns_unused(texture); }
void gpu_destroy_sampler(gpu_sampler sampler) { ns_unused(sampler); }
void gpu_destroy_buffer(gpu_buffer buffer) { ns_unused(buffer); }
void gpu_destroy_shader(gpu_shader shader) { ns_unused(shader); }
void gpu_destroy_pipeline(gpu_pipeline pipeline) { ns_unused(pipeline); }
void gpu_destroy_binding(gpu_binding binding) { ns_unused(binding); }
void gpu_destroy_mesh(gpu_mesh mesh) { ns_unused(mesh); }
void gpu_destroy_render_pass(gpu_render_pass pass) { ns_unused(pass); }

gpu_pipeline_reflection gpu_pipeline_get_reflection(gpu_pipeline pipeline) {
    ns_unused(pipeline);
    return (gpu_pipeline_reflection){0};
}
void gpu_update_texture(gpu_texture texture, ns_data data) { ns_unused(texture); ns_unused(data); }
void gpu_update_buffer_desc(gpu_buffer buffer, ns_data data) { ns_unused(buffer); ns_unused(data); }

void gpu_begin_render_pass(gpu_render_pass pass) { ns_unused(pass); }
void gpu_set_viewport(int x, int y, int width, int height) { ns_unused(x); ns_unused(y); ns_unused(width); ns_unused(height); }
void gpu_set_scissor(int x, int y, int width, int height) { ns_unused(x); ns_unused(y); ns_unused(width); ns_unused(height); }
void gpu_set_pipeline(gpu_pipeline pipeline) { ns_unused(pipeline); }
void gpu_set_binding(gpu_binding binding) { ns_unused(binding); }
void gpu_set_mesh(gpu_mesh mesh) { ns_unused(mesh); }
void gpu_draw(int base, int count, int instance_count) { ns_unused(base); ns_unused(count); ns_unused(instance_count); }
void gpu_end_pass(void) {}
void gpu_commit(void) {}

#endif

const char *gpu_shader_target(void) {
#if defined(NS_GPU_METAL)
    return "msl";
#elif defined(NS_GPU_DX12)
    return "hlsl";
#else
    return "glsl";
#endif
}

u32 gpu_create_buffer(i32 byte_len, i32 usage) {
    if (byte_len < 0) byte_len = 0;
    return gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = byte_len,
        .type = BUFFER_VERTEX,
        .usage = (gpu_usage)usage,
    }).id;
}

void gpu_update_buffer(u32 buffer_id, void *data, i32 byte_len) {
    if (byte_len < 0) byte_len = 0;
    gpu_update_buffer_desc((gpu_buffer){buffer_id}, (ns_data){data, (szt)byte_len});
}

u32 gpu_create_shader_source(const char *vertex_source, const char *fragment_source, const char *vertex_entry, const char *fragment_entry) {
    if (!vertex_source || !fragment_source || !vertex_entry || !fragment_entry) return 0;
    return gpu_create_shader(&(gpu_shader_desc){
        .vertex = {
            .source = ns_str_cstr((char *)vertex_source),
            .entry = ns_str_cstr((char *)vertex_entry),
        },
        .fragment = {
            .source = ns_str_cstr((char *)fragment_source),
            .entry = ns_str_cstr((char *)fragment_entry),
        },
    }).id;
}

u32 gpu_create_pipeline_layout(u32 shader_id, i32 vertex_stride, i32 *attr_offsets, i32 *attr_sizes, i32 *attr_formats, i32 attr_count,
                               i32 color_format, i32 primitive_type) {
    if (attr_count < 0) attr_count = 0;
    if (attr_count > GPU_ATTRIBUTE_COUNT) attr_count = GPU_ATTRIBUTE_COUNT;

    gpu_pipeline_desc desc = {0};
    desc.shader = (gpu_shader){shader_id};
    desc.layout.buffers[0] = (gpu_vertex_buffer_layout_state){
        .stride = vertex_stride,
        .step_func = VERTEX_STEP_PER_VERTEX,
        .step_rate = 1,
    };
    for (i32 i = 0; i < attr_count; i++) {
        desc.layout.attributes[i] = (gpu_vertex_attribute_state){
            .buffer_index = 0,
            .offset = attr_offsets ? attr_offsets[i] : 0,
            .size = attr_sizes ? attr_sizes[i] : 0,
            .format = attr_formats ? (gpu_attribute_format)attr_formats[i] : ATTRIBUTE_FORMAT_INVALID,
        };
    }
    desc.depth.format = PIXELFORMAT_NONE;
    desc.color_count = 1;
    desc.colors[0].format = (gpu_pixel_format)color_format;
    desc.colors[0].color_mask = COLOR_MASK_ALL;
    desc.primitive_type = (gpu_primitive_type)primitive_type;
    desc.index_type = INDEX_NONE;
    desc.cull_mode = CULL_NONE;
    desc.face_winding = FACE_WINDING_CCW;
    desc.sample_count = 1;

    return gpu_create_pipeline(&desc).id;
}

u32 gpu_create_mesh_1(u32 pipeline_id, u32 vertex_buffer_id) {
    return gpu_create_mesh(&(gpu_mesh_desc){
        .buffers = {{vertex_buffer_id}},
        .pipeline = (gpu_pipeline){pipeline_id},
    }).id;
}

void gpu_destroy_buffer_id(u32 buffer_id) {
    gpu_destroy_buffer((gpu_buffer){buffer_id});
}

void gpu_destroy_shader_id(u32 shader_id) {
    gpu_destroy_shader((gpu_shader){shader_id});
}

void gpu_destroy_pipeline_id(u32 pipeline_id) {
    gpu_destroy_pipeline((gpu_pipeline){pipeline_id});
}

void gpu_destroy_mesh_id(u32 mesh_id) {
    gpu_destroy_mesh((gpu_mesh){mesh_id});
}

void gpu_begin_render_pass_id(u32 pass_id) {
    gpu_begin_render_pass((gpu_render_pass){pass_id});
}

void gpu_set_pipeline_id(u32 pipeline_id) {
    gpu_set_pipeline((gpu_pipeline){pipeline_id});
}

void gpu_set_mesh_id(u32 mesh_id) {
    gpu_set_mesh((gpu_mesh){mesh_id});
}

// Pixel-format math is backend agnostic; provide a minimal RGBA8 fallback so the
// helpers return sane values when no backend is present.
int gpu_pixel_format_size(gpu_pixel_format format) {
    ns_unused(format);
    return 4;
}

int gpu_pixel_format_row_count(gpu_pixel_format format, int height) {
    ns_unused(format);
    return height;
}

int gpu_pixel_format_row_pitch(gpu_pixel_format format, int width, int row_alignment) {
    int pitch = gpu_pixel_format_size(format) * width;
    if (row_alignment > 1) {
        pitch = ((pitch + row_alignment - 1) / row_alignment) * row_alignment;
    }
    return pitch;
}

int gpu_pixel_format_surface_pitch(gpu_pixel_format format, int width, int height, int row_alignment) {
    int row_pitch = gpu_pixel_format_row_pitch(format, width, row_alignment);
    return row_pitch * gpu_pixel_format_row_count(format, height);
}
