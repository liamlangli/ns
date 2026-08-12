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

typedef struct { f32 r, g, b, a; } gpu_color;

ns_bool gpu_request_device(view *v);
void gpu_destroy_device(void);
void gpu_set_viewport(int x, int y, int width, int height);
void gpu_set_scissor(int x, int y, int width, int height);
void gpu_commit(void);

const char *gpu_shader_target(void);

int gpu_pixel_format_row_count(gpu_pixel_format format, int height);
int gpu_pixel_format_size(gpu_pixel_format format);
int gpu_pixel_format_surface_pitch(gpu_pixel_format format, int width, int height, int row_alignment);
int gpu_pixel_format_row_pitch(gpu_pixel_format format, int width, int row_alignment);

// ============================================================================
// Public Nano Script API (doc/gpu.md): the GPU as a processor with memory.
//
// GPU memory is addressed with 64-bit gpu_addr values obtained from
// gpu_malloc; suballocation is plain address arithmetic. Textures and
// samplers are u32 bindless heap indices. Shader input is a single root
// pointer. There are no buffer, mesh, binding, or pipeline objects; render
// state is a small value-cached key and the backend hashes (shader, state,
// pass formats) into an internal PSO cache.
//
// ============================================================================

// 64-bit GPU address; 0 is null. On GPU_CAP_RAW_POINTERS tiers this is a real
// device address; elsewhere it is a virtual (slot << 40 | offset) encoding
// translated by the module. Either way it is opaque-but-arithmetic: adding a
// byte offset inside one allocation is always valid.
typedef u64 gpu_addr;

u32 gpu_caps(void);
// Number of zero-based shader_buffer_* slots exposed by the active backend.
// Returns 0 without a device.
u32 gpu_storage_slot_count(void);

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
void gpu_set_storage(gpu_addr addr);
void gpu_set_storage_at(i32 index, gpu_addr addr);
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
    void (*set_storage)(u32 binding, u32 slot, u64 offset, gpu_addr addr);
    void (*draw)(i32 vertex_base, i32 vertex_count, i32 instance_count);
    void (*draw_indexed)(u32 index_slot, u64 index_offset, i32 index_type,
                         i32 index_count, i32 instance_count, i32 base_vertex);
    void (*draw_indirect)(u32 args_slot, u64 args_offset, i32 draw_count, i32 stride);
    void (*dispatch)(i32 x, i32 y, i32 z);
    void (*dispatch_indirect)(u32 args_slot, u64 args_offset);
    void (*signal_after)(u32 slot, u64 offset, u64 value);
    void (*wait_before)(u32 slot, u64 offset, u64 value);
} gpu_v2_ops;

void gpu_v2_set_backend(const gpu_v2_ops *ops, u32 caps, u32 storage_slot_count);
void gpu_v2_frame_end(void); // backends call this from gpu_commit to recycle the ring
