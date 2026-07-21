#pragma once

#include "gpu_const.h"
#include "os.h"
#include "ns_type.h"

#include "view.h"

// Platform-forced GPU backend selection.
//
// The backend is chosen at compile time from the platform define rather than an
// opt-in flag: Metal on Apple, DirectX 12 on Windows. Other platforms (Linux)
// fall back to the no-op stub in gpu.c until a Vulkan backend is wired up.
// Only amd64 and aarch64 targets are supported.
#if defined(NS_DARWIN)
#  define NS_GPU_METAL 1
#elif defined(NS_WIN)
#  define NS_GPU_DX12 1
#endif

typedef enum gpu_resource_type {
    none,
    webgl,
    webgpu,
    union_gpu,
} gpu_backend;
typedef struct { u32 id; } gpu_texture, gpu_sampler, gpu_buffer, gpu_shader, gpu_pipeline, gpu_binding, gpu_render_pass, gpu_mesh;
typedef struct { f32 r, g, b, a; } gpu_color;

typedef struct gpu_texture_desc {
    int width, height, depth;
    ns_data data;
    gpu_pixel_format format;
    gpu_texture_type type;
    gpu_texture_usage usage;
    gpu_usage resource_usage;
} gpu_texture_desc;

typedef struct gpu_sampler_desc {
    gpu_filter min_filter, mag_filter, mip_filter;
    gpu_wrap wrap_u, wrap_v, wrap_w;
    float min_lod;
    float max_lod;
    gpu_compare_func compare_func;
    u32 max_anisotropy;
    ns_str label;
} gpu_sampler_desc;

typedef struct gpu_buffer_desc {
    int size;
    ns_data data;
    gpu_buffer_type type;
    gpu_usage usage;
} gpu_buffer_desc;

typedef struct gpu_attribute_desc {
    ns_str name;
    gpu_attribute_format type;
    int size;
    int stride;
} gpu_attribute_desc;

typedef struct gpu_uniform_desc {
    ns_str name;
    gpu_uniform_type type;
    int size;
} gpu_uniform_desc;

typedef struct gpu_shader_uniform_block_desc {
    int size;
    ns_str name;
    gpu_uniform_layout layout;
    gpu_uniform_desc uniforms[GPU_BLOCK_UNIFORM_COUNT];
} gpu_shader_uniform_block_desc;

typedef struct gpu_shader_texture_desc {
    ns_bool used;
    gpu_texture_type type;
    gpu_texture_sample_type sample_type;
} gpu_shader_texture_desc;

typedef struct gpu_shader_sampler_desc {
    ns_bool used;
    gpu_sampler_type type;
} gpu_shader_sampler_desc;

typedef struct gpu_shader_texture_sampler_mapping {
    ns_bool used;
    int texture_slot;
    int sampler_slot;
} gpu_shader_texture_sampler_mapping;

typedef struct gpu_shader_stage_desc {
    ns_str source;
    ns_data bytecode;
    ns_str entry;
    gpu_shader_uniform_block_desc blocks[GPU_BLOCK_COUNT];
    gpu_shader_texture_desc textures[GPU_SHADER_TEXTURE_COUNT];
    gpu_shader_sampler_desc samplers[GPU_SHADER_SAMPLER_COUNT];
    gpu_shader_texture_sampler_mapping mappings[GPU_SHADER_TEXTURE_COUNT];
} gpu_shader_stage_desc;

typedef struct gpu_shader_desc {
    gpu_attribute_desc attributes[GPU_ATTRIBUTE_COUNT];
    gpu_shader_stage_desc vertex;
    gpu_shader_stage_desc fragment;
    ns_str label;
} gpu_shader_desc;

typedef struct gpu_vertex_buffer_layout_state {
    int stride;
    gpu_vertex_step step_func;
    int step_rate;
} gpu_vertex_buffer_layout_state;

typedef struct gpu_vertex_attribute_state {
    int buffer_index;
    int offset;
    int size;
    gpu_attribute_format format;
} gpu_vertex_attribute_state;

typedef struct gpu_vertex_layout_state {
    gpu_vertex_buffer_layout_state buffers[GPU_VERTEX_BUFFER_COUNT];
    gpu_vertex_attribute_state attributes[GPU_ATTRIBUTE_COUNT];
} gpu_vertex_layout_state;

typedef struct gpu_stencil_face_state {
    gpu_compare_func compare_func;
    gpu_stencil_op fail_op;
    gpu_stencil_op pass_op;
    gpu_stencil_op depth_fail_op;
} gpu_stencil_face_state;

typedef struct gpu_stencil_state {
    ns_bool enabled;
    gpu_stencil_face_state front;
    gpu_stencil_face_state back;
    u8 read_mask;
    u8 write_mask;
    u8 ref;
} gpu_stencil_state;

typedef struct gpu_depth_state {
    gpu_pixel_format format;
    gpu_compare_func compare_func;
    ns_bool write_enabled;
    f32 bias, bias_slope_scale, bias_clamp;
} gpu_depth_state;

typedef struct gpu_blend_state {
    ns_bool enabled;
    gpu_blend_factor src_factor;
    gpu_blend_factor dst_factor;
    gpu_blend_op op;
    gpu_blend_factor src_factor_alpha;
    gpu_blend_factor dst_factor_alpha;
    gpu_blend_op op_alpha;
} gpu_blend_state;

typedef struct gpu_color_target_state {
    gpu_pixel_format format;
    gpu_color_mask color_mask;
    gpu_blend_state blend;
} gpu_color_target_state;

typedef struct gpu_pipeline_desc {
    gpu_shader shader;
    gpu_vertex_layout_state layout;
    gpu_depth_state depth;
    gpu_stencil_state stencil;

    int color_count;
    gpu_color_target_state colors[GPU_ATTACHMENT_COUNT];

    gpu_primitive_type primitive_type;
    gpu_index_type index_type;
    gpu_cull_mode cull_mode;
    gpu_face_winding face_winding;

    int sample_count;
    gpu_color blend_color;
    ns_bool alpha_to_coverage;
    ns_str label;
} gpu_pipeline_desc;

typedef struct gpu_stage_binding {
    gpu_texture textures[GPU_SHADER_TEXTURE_COUNT];
    gpu_sampler samplers[GPU_SHADER_SAMPLER_COUNT];
} gpu_stage_binding;

typedef struct gpu_binding_texture_desc {
    gpu_texture texture;
    gpu_sampler sampler;
    ns_str name;
} gpu_binding_texture_desc;

typedef struct gpu_binding_buffer_desc {
    gpu_buffer buffer;
    u32 offset;
    ns_str name;
} gpu_binding_buffer_desc;

typedef struct gpu_binding_desc {
    gpu_pipeline pipeline;
    gpu_binding_buffer_desc buffers[GPU_SHADER_BUFFER_COUNT];
    gpu_binding_texture_desc textures[GPU_SHADER_TEXTURE_COUNT];
    u32 group;
    ns_str label;
} gpu_binding_desc;

typedef struct gpu_mesh_desc {
    gpu_buffer buffers[GPU_VERTEX_BUFFER_COUNT];
    u32 buffer_offsets[GPU_VERTEX_BUFFER_COUNT];
    ns_str buffers_names[GPU_VERTEX_BUFFER_COUNT];
    gpu_pipeline pipeline;
    gpu_buffer index_buffer;
    u32 index_buffer_offset;
    gpu_index_type index_type;
    ns_str label;
} gpu_mesh_desc;

typedef struct gpu_color_attachment_action {
    gpu_load_action load_action;
    gpu_store_action store_action;
    gpu_color clear_value;
} gpu_color_attachment_action;

typedef struct gpu_depth_attachment_action {
    gpu_load_action load_action;
    gpu_store_action store_action;
    f32 clear_value;
} gpu_depth_attachment_action;

typedef struct gpu_stencil_attachment_action {
    gpu_load_action load_action;
    gpu_store_action store_action;
    u8 clear_value;
} gpu_stencil_attachment_action;

typedef struct gpu_pass_action {
    gpu_color_attachment_action color_action[GPU_ATTACHMENT_COUNT];
    gpu_depth_attachment_action depth_action;
    gpu_stencil_attachment_action stencil_action;
} gpu_pass_action;

typedef struct gpu_attachment_desc {
    gpu_texture texture;
    int mip_level;
    int slice;
} gpu_attachment_desc;

typedef struct gpu_render_pass_color_attachment {
    gpu_attachment_desc desc;
    gpu_load_action load_action;
    gpu_store_action store_action;
    gpu_color clear_value;
} gpu_render_pass_color_attachment;

typedef struct gpu_render_pass_depth_stencil_attachment {
    gpu_attachment_desc desc;
    gpu_load_action load_action;
    gpu_store_action store_action;
    f32 clear_value;
    ns_bool private;
} gpu_render_pass_depth_stencil_attachment;

typedef struct gpu_render_pass_desc {
    int width;
    int height;
    gpu_render_pass_color_attachment colors[GPU_ATTACHMENT_COUNT];
    gpu_render_pass_depth_stencil_attachment depth;
    gpu_render_pass_depth_stencil_attachment stencil;
    ns_bool screen;
    ns_str label;
    u32 id;
} gpu_render_pass_desc;

typedef struct gpu_pipeline_reflection {
    gpu_vertex_layout_state layout;
    gpu_uniform_desc global_uniforms[GPU_BLOCK_UNIFORM_COUNT];
    gpu_shader_uniform_block_desc uniform_blocks[GPU_BLOCK_UNIFORM_COUNT];
    gpu_shader_texture_desc textures[GPU_SHADER_TEXTURE_COUNT];
    gpu_index_type index_type;
} gpu_pipeline_reflection;

ns_bool gpu_request_device(view *v);
void gpu_destroy_device(void);

gpu_texture gpu_create_texture(gpu_texture_desc *desc);
gpu_sampler gpu_create_sampler(gpu_sampler_desc *desc);
gpu_buffer gpu_create_buffer_desc(gpu_buffer_desc *desc);
gpu_shader gpu_create_shader(gpu_shader_desc *desc);
gpu_pipeline gpu_create_pipeline(gpu_pipeline_desc *desc);
gpu_binding gpu_create_binding(gpu_binding_desc *desc);
gpu_mesh gpu_create_mesh(gpu_mesh_desc *desc);
gpu_render_pass gpu_create_render_pass(gpu_render_pass_desc *desc);

void gpu_destroy_texture(gpu_texture texture);
void gpu_destroy_sampler(gpu_sampler sampler);
void gpu_destroy_buffer(gpu_buffer buffer);
void gpu_destroy_shader(gpu_shader shader);
void gpu_destroy_pipeline(gpu_pipeline pipeline);
void gpu_destroy_binding(gpu_binding binding);
void gpu_destroy_mesh(gpu_mesh mesh);
void gpu_destroy_render_pass(gpu_render_pass pass);

gpu_pipeline_reflection gpu_pipeline_get_reflection(gpu_pipeline pipeline);
void gpu_update_texture(gpu_texture texture, ns_data data);
void gpu_update_buffer_desc(gpu_buffer buffer, ns_data data);

void gpu_begin_render_pass(gpu_render_pass pass);
void gpu_set_viewport(int x, int y, int width, int height);
void gpu_set_scissor(int x, int y, int width, int height);
void gpu_set_pipeline(gpu_pipeline pipeline);
void gpu_set_binding(gpu_binding binding);
void gpu_set_mesh(gpu_mesh mesh);
void gpu_draw(int base, int count, int instance_count);
void gpu_end_pass(void);
void gpu_commit(void);

const char *gpu_shader_target(void);
ns_bool gpu_dispatch_compute_source(const char *source, const char *entry, i32 threads_x, i32 threads_y, i32 threads_z);
ns_bool gpu_dispatch_compute_texture_source(const char *source, const char *entry, u32 texture_id,
                                            i32 threads_x, i32 threads_y, i32 threads_z);
u32 gpu_create_buffer(i32 byte_len, i32 usage);
u32 gpu_create_index_buffer(i32 byte_len, i32 usage);
u32 gpu_create_uniform_buffer(i32 byte_len, i32 usage);
void gpu_update_buffer(u32 buffer_id, void *data, i32 byte_len);
void gpu_update_texture_id(u32 texture_id, void *data, i32 byte_len);
u32 gpu_create_shader_source(const char *vertex_source, const char *fragment_source, const char *vertex_entry, const char *fragment_entry);
u32 gpu_create_pipeline_layout(u32 shader_id, i32 vertex_stride, i32 *attr_offsets, i32 *attr_sizes, i32 *attr_formats, i32 attr_count,
                               i32 color_format, i32 primitive_type);
u32 gpu_create_pipeline_layout_ex(u32 shader_id, i32 vertex_stride, i32 *attr_offsets, i32 *attr_sizes, i32 *attr_formats, i32 attr_count,
                                  i32 color_format, i32 primitive_type, i32 depth_format, i32 depth_compare,
                                  ns_bool depth_write, i32 cull_mode, ns_bool blend_enabled);
u32 gpu_create_pipeline_layout_indexed_ex(u32 shader_id, i32 vertex_stride, i32 *attr_offsets, i32 *attr_sizes, i32 *attr_formats, i32 attr_count,
                                          i32 color_format, i32 primitive_type, i32 index_type, i32 depth_format, i32 depth_compare,
                                          ns_bool depth_write, i32 cull_mode, ns_bool blend_enabled);
u32 gpu_create_mesh_1(u32 pipeline_id, u32 vertex_buffer_id);
u32 gpu_create_mesh_indexed(u32 pipeline_id, u32 vertex_buffer_id, u32 index_buffer_id, i32 index_type);
u32 gpu_create_texture_2d(i32 width, i32 height, i32 format, i32 usage);
u32 gpu_create_texture_binding(u32 pipeline_id, u32 texture_id, const char *name);
u32 gpu_create_buffer_texture_binding(u32 pipeline_id, u32 buffer_id, const char *buffer_name,
                                      u32 texture0_id, const char *texture0_name, u32 texture1_id, const char *texture1_name);
u32 gpu_create_depth_pass(u32 depth_texture_id);
u32 gpu_create_screen_pass(f64 r, f64 g, f64 b, f64 a);
void gpu_destroy_texture_id(u32 texture_id);
void gpu_destroy_binding_id(u32 binding_id);
void gpu_destroy_buffer_id(u32 buffer_id);
void gpu_destroy_shader_id(u32 shader_id);
void gpu_destroy_pipeline_id(u32 pipeline_id);
void gpu_destroy_mesh_id(u32 mesh_id);
void gpu_destroy_render_pass_id(u32 pass_id);
void gpu_begin_render_pass_id(u32 pass_id);
void gpu_set_pipeline_id(u32 pipeline_id);
void gpu_set_mesh_id(u32 mesh_id);
void gpu_set_binding_id(u32 binding_id);

int gpu_pixel_format_row_count(gpu_pixel_format format, int height);
int gpu_pixel_format_size(gpu_pixel_format format);
int gpu_pixel_format_surface_pitch(gpu_pixel_format format, int width, int height, int row_alignment);
int gpu_pixel_format_row_pitch(gpu_pixel_format format, int width, int row_alignment);

// ============================================================================
// v2 API (doc/gpu.md): the GPU as a processor with memory.
//
// GPU memory is addressed with 64-bit gpu_addr values obtained from
// gpu_malloc; suballocation is plain address arithmetic. Textures and
// samplers are u32 bindless heap indices. Shader input is a single root
// pointer. There are no buffer, mesh, binding, or pipeline objects; render
// state is a small value-cached key and the backend hashes (shader, state,
// pass formats) into an internal PSO cache.
//
// v1 above stays intact while consumers migrate; colliding names take the
// v2 spelling (gpu_texture_create vs gpu_create_texture, gpu_draw_vertices
// vs gpu_draw) and become the only names when v1 is removed.
// ============================================================================

// 64-bit GPU address; 0 is null. On GPU_CAP_RAW_POINTERS tiers this is a real
// device address; elsewhere it is a virtual (slot << 40 | offset) encoding
// translated by the module. Either way it is opaque-but-arithmetic: adding a
// byte offset inside one allocation is always valid.
typedef u64 gpu_addr;

u32 gpu_caps(void);

// ---- memory ----------------------------------------------------------------
// Works with or without a device: the null tier keeps allocations host-side
// (programs run headless, nothing renders), real tiers back them with GPU
// memory. Allocation size is limited to 1 TiB by the virtual encoding.
gpu_addr gpu_malloc(u64 size, u32 flags);       // gpu_mem_flags
void     gpu_free(gpu_addr addr);               // addr must be an allocation base
void     gpu_write(gpu_addr dst, const void *src, u64 size);
ns_bool  gpu_read(gpu_addr src, void *dst, u64 size);
void    *gpu_addr_host(gpu_addr addr);          // NULL unless GPU_CAP_RAW_POINTERS on shared memory
gpu_addr gpu_frame_alloc(u64 size, u32 align);  // transient, recycled by gpu_commit

// ---- textures and samplers (bindless heap indices, 0 = invalid) ------------
u32  gpu_texture_create(i32 width, i32 height, i32 depth_or_layers,
                        i32 format, u32 usage, i32 mip_count, i32 kind);
void gpu_texture_upload(u32 tex, i32 mip, i32 layer, const void *data, u64 size);
void gpu_texture_destroy(u32 tex);
u32  gpu_sampler_create(i32 min_filter, i32 mag_filter, i32 mip_filter,
                        i32 wrap_u, i32 wrap_v, i32 wrap_w,
                        i32 compare_func, i32 max_anisotropy);
void gpu_sampler_destroy(u32 smp);

// ---- shaders and render state ----------------------------------------------
u32  gpu_shader_graphics_create(const char *vs_src, const char *fs_src,
                                const char *vs_entry, const char *fs_entry);
u32  gpu_shader_compute_create(const char *src, const char *entry);
void gpu_shader_destroy(u32 shader);

// Immutable, value-deduplicated state key; equal arguments return the same id.
u32 gpu_state_create(i32 primitive_type, i32 cull_mode, i32 face_winding,
                     i32 depth_compare, ns_bool depth_write,
                     i32 blend_preset, u32 color_mask);

// ---- passes ------------------------------------------------------------------
// Attachments are texture indices, 0 = unused; load_flags packs a
// gpu_load_action per attachment (GPU_PASS_*_SHIFT).
void gpu_pass_begin(u32 color0, u32 color1, u32 color2, u32 color3,
                    u32 depth, u32 load_flags,
                    f64 r, f64 g, f64 b, f64 a, f64 depth_clear);
void gpu_screen_pass_begin(f64 r, f64 g, f64 b, f64 a);
void gpu_pass_end(void);

// ---- binding and drawing -----------------------------------------------------
void gpu_set_shader(u32 shader);
void gpu_set_state(u32 state);
void gpu_set_root(gpu_addr args);
void gpu_set_root_data(const void *data, u64 size); // frame-ring copy + set_root
void gpu_draw_vertices(i32 vertex_base, i32 vertex_count, i32 instance_count);
void gpu_draw_indexed(gpu_addr indices, i32 index_type,
                      i32 index_count, i32 instance_count, i32 base_vertex);
void gpu_draw_indirect(gpu_addr args, i32 draw_count, i32 stride);
void gpu_dispatch(i32 x, i32 y, i32 z);
void gpu_dispatch_indirect(gpu_addr args);

// ---- synchronization ---------------------------------------------------------
// Implicit default: submission order with a full barrier between passes.
// Split barriers (futex-like: wait until *addr == value) allow overlap on
// GPU_CAP_ASYNC_COMPUTE tiers and degrade to the implicit barrier elsewhere.
void gpu_signal_after(gpu_addr addr, u64 value);
void gpu_wait_before(gpu_addr addr, u64 value);

// ---- internal backend seam (not part of the ns surface) ----------------------
// The portable core in gpu.c owns virtual addressing, the frame ring, and the
// state registry; a platform backend registers gpu_v2_ops from
// gpu_request_device to supply real GPU objects and submission. Every op may
// be NULL; the core then no-ops (resource creation returns 0). Address
// arguments arrive pre-translated as (slot, offset); ops that also receive
// the raw gpu_addr can use it on raw-pointer tiers.

typedef struct gpu_v2_state_desc {
    i32 primitive_type;
    i32 cull_mode;
    i32 face_winding;
    i32 depth_compare;
    ns_bool depth_write;
    i32 blend_preset;
    u32 color_mask;
} gpu_v2_state_desc;

typedef struct gpu_v2_ops {
    // memory: create backing for slot; may return a real device base address
    // through base_va (0 keeps the slot on virtual addressing).
    ns_bool (*mem_create)(u32 slot, u64 size, u32 flags, u64 *base_va);
    void    (*mem_destroy)(u32 slot);
    void    (*mem_write)(u32 slot, u64 offset, const void *src, u64 size);
    ns_bool (*mem_read)(u32 slot, u64 offset, void *dst, u64 size);
    void   *(*mem_host_ptr)(u32 slot);

    u32  (*texture_create)(i32 width, i32 height, i32 depth_or_layers,
                           i32 format, u32 usage, i32 mip_count, i32 kind);
    void (*texture_upload)(u32 tex, i32 mip, i32 layer, const void *data, u64 size);
    void (*texture_destroy)(u32 tex);
    u32  (*sampler_create)(i32 min_filter, i32 mag_filter, i32 mip_filter,
                           i32 wrap_u, i32 wrap_v, i32 wrap_w,
                           i32 compare_func, i32 max_anisotropy);
    void (*sampler_destroy)(u32 smp);
    u32  (*shader_graphics_create)(const char *vs_src, const char *fs_src,
                                   const char *vs_entry, const char *fs_entry);
    u32  (*shader_compute_create)(const char *src, const char *entry);
    void (*shader_destroy)(u32 shader);

    void (*pass_begin)(u32 color0, u32 color1, u32 color2, u32 color3,
                       u32 depth, u32 load_flags, gpu_color clear, f32 depth_clear);
    void (*screen_pass_begin)(gpu_color clear);
    void (*pass_end)(void);
    void (*set_shader)(u32 shader);
    void (*set_state)(const gpu_v2_state_desc *desc);
    void (*set_root)(u32 slot, u64 offset, gpu_addr addr);
    void (*draw)(i32 vertex_base, i32 vertex_count, i32 instance_count);
    void (*draw_indexed)(u32 index_slot, u64 index_offset, i32 index_type,
                         i32 index_count, i32 instance_count, i32 base_vertex);
    void (*draw_indirect)(u32 args_slot, u64 args_offset, i32 draw_count, i32 stride);
    void (*dispatch)(i32 x, i32 y, i32 z);
    void (*dispatch_indirect)(u32 args_slot, u64 args_offset);
    void (*signal_after)(u32 slot, u64 offset, u64 value);
    void (*wait_before)(u32 slot, u64 offset, u64 value);
} gpu_v2_ops;

void gpu_v2_set_backend(const gpu_v2_ops *ops, u32 caps);
void gpu_v2_frame_end(void); // backends call this from gpu_commit to recycle the ring
