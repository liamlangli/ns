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
gpu_buffer gpu_create_buffer(gpu_buffer_desc *desc) { ns_unused(desc); return (gpu_buffer){0}; }
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
void gpu_update_buffer(gpu_buffer buffer, ns_data data) { ns_unused(buffer); ns_unused(data); }

void gpu_begin_render_pass(gpu_render_pass pass) { ns_unused(pass); }
void gpu_set_viewport(int x, int y, int width, int height) { ns_unused(x); ns_unused(y); ns_unused(width); ns_unused(height); }
void gpu_set_scissor(int x, int y, int width, int height) { ns_unused(x); ns_unused(y); ns_unused(width); ns_unused(height); }
void gpu_set_pipeline(gpu_pipeline pipeline) { ns_unused(pipeline); }
void gpu_set_binding(gpu_binding binding) { ns_unused(binding); }
void gpu_set_mesh(gpu_mesh mesh) { ns_unused(mesh); }
void gpu_draw(int base, int count, int instance_count) { ns_unused(base); ns_unused(count); ns_unused(instance_count); }
void gpu_end_pass(void) {}
void gpu_commit(void) {}

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

#endif
