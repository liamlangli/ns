// GPU device management — platform fallback and the portable v2 core.
//
// A real GPU backend is selected per platform in gpu.h: Metal on Apple
// (gpu.metal.m), DirectX 12 on Windows (gpu.dx12.c). On platforms without a
// backend (currently Linux) this file provides no-op definitions for the whole
// gpu.h API so the statically linked standard library keeps resolving every
// gpu_* symbol registered in src/ns_vm_lib.c. ns programs that drive the GPU
// still load and run here; they simply render nothing.
//
// The v2 core at the bottom of this file (doc/gpu.md) is compiled on every
// platform: it owns virtual addressing, host-side shadows, the frame ring,
// and the render-state registry, and forwards to whichever backend registered
// gpu_v2_ops.
#include "gpu.h"

#include <stdlib.h>
#include <string.h>

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
void gpu_commit(void) { gpu_v2_frame_end(); }

ns_bool gpu_dispatch_compute_source(const char *source, const char *entry, i32 threads_x, i32 threads_y, i32 threads_z) {
    ns_unused(source);
    ns_unused(entry);
    ns_unused(threads_x);
    ns_unused(threads_y);
    ns_unused(threads_z);
    return false;
}

ns_bool gpu_dispatch_compute_texture_source(const char *source, const char *entry, u32 texture_id,
                                            i32 threads_x, i32 threads_y, i32 threads_z) {
    ns_unused(source);
    ns_unused(entry);
    ns_unused(texture_id);
    ns_unused(threads_x);
    ns_unused(threads_y);
    ns_unused(threads_z);
    return false;
}

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

u32 gpu_create_index_buffer(i32 byte_len, i32 usage) {
    if (byte_len <= 0) return 0;
    return gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = byte_len,
        .type = BUFFER_INDEX,
        .usage = (gpu_usage)usage,
    }).id;
}

u32 gpu_create_uniform_buffer(i32 byte_len, i32 usage) {
    if (byte_len <= 0) return 0;
    return gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = byte_len,
        .type = BUFFER_UNIFORM,
        .usage = (gpu_usage)usage,
    }).id;
}

void gpu_update_buffer(u32 buffer_id, void *data, i32 byte_len) {
    if (byte_len < 0) byte_len = 0;
    gpu_update_buffer_desc((gpu_buffer){buffer_id}, (ns_data){data, (szt)byte_len});
}

void gpu_update_texture_id(u32 texture_id, void *data, i32 byte_len) {
    if (!texture_id || !data || byte_len <= 0) return;
    gpu_update_texture((gpu_texture){texture_id}, (ns_data){data, (szt)byte_len});
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

u32 gpu_create_pipeline_layout_indexed_ex(u32 shader_id, i32 vertex_stride, i32 *attr_offsets, i32 *attr_sizes, i32 *attr_formats, i32 attr_count,
                                          i32 color_format, i32 primitive_type, i32 index_type, i32 depth_format, i32 depth_compare,
                                          ns_bool depth_write, i32 cull_mode, ns_bool blend_enabled) {
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
    desc.depth.format = (gpu_pixel_format)depth_format;
    desc.depth.compare_func = (gpu_compare_func)depth_compare;
    desc.depth.write_enabled = depth_write;
    desc.color_count = color_format == PIXELFORMAT_NONE ? 0 : 1;
    if (desc.color_count > 0) {
        desc.colors[0].format = (gpu_pixel_format)color_format;
        desc.colors[0].color_mask = COLOR_MASK_ALL;
        desc.colors[0].blend = (gpu_blend_state){
            .enabled = blend_enabled,
            .src_factor = BLEND_FACTOR_SRC_ALPHA,
            .dst_factor = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .op = BLEND_OP_ADD,
            .src_factor_alpha = BLEND_FACTOR_ONE,
            .dst_factor_alpha = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .op_alpha = BLEND_OP_ADD,
        };
    }
    desc.primitive_type = (gpu_primitive_type)primitive_type;
    desc.index_type = (gpu_index_type)index_type;
    desc.cull_mode = (gpu_cull_mode)cull_mode;
    desc.face_winding = FACE_WINDING_CCW;
    desc.sample_count = 1;

    return gpu_create_pipeline(&desc).id;
}

u32 gpu_create_pipeline_layout_ex(u32 shader_id, i32 vertex_stride, i32 *attr_offsets, i32 *attr_sizes, i32 *attr_formats, i32 attr_count,
                                  i32 color_format, i32 primitive_type, i32 depth_format, i32 depth_compare,
                                  ns_bool depth_write, i32 cull_mode, ns_bool blend_enabled) {
    return gpu_create_pipeline_layout_indexed_ex(shader_id, vertex_stride, attr_offsets, attr_sizes, attr_formats, attr_count,
                                                 color_format, primitive_type, INDEX_NONE, depth_format, depth_compare,
                                                 depth_write, cull_mode, blend_enabled);
}

u32 gpu_create_mesh_1(u32 pipeline_id, u32 vertex_buffer_id) {
    return gpu_create_mesh(&(gpu_mesh_desc){
        .buffers = {{vertex_buffer_id}},
        .pipeline = (gpu_pipeline){pipeline_id},
    }).id;
}

u32 gpu_create_mesh_indexed(u32 pipeline_id, u32 vertex_buffer_id, u32 index_buffer_id, i32 index_type) {
    if (!pipeline_id || !vertex_buffer_id || !index_buffer_id || index_type == INDEX_NONE) return 0;
    return gpu_create_mesh(&(gpu_mesh_desc){
        .buffers = {{vertex_buffer_id}},
        .pipeline = (gpu_pipeline){pipeline_id},
        .index_buffer = (gpu_buffer){index_buffer_id},
        .index_type = (gpu_index_type)index_type,
    }).id;
}

u32 gpu_create_texture_2d(i32 width, i32 height, i32 format, i32 usage) {
    if (width <= 0 || height <= 0) return 0;
    return gpu_create_texture(&(gpu_texture_desc){
        .width = width,
        .height = height,
        .depth = 1,
        .format = (gpu_pixel_format)format,
        .type = TEXTURE_2D,
        .usage = (gpu_texture_usage)usage,
        .resource_usage = (usage & TEXTURE_USAGE_RENDER_TARGET) ? USAGE_PRIVATE : USAGE_DEFAULT,
    }).id;
}

u32 gpu_create_texture_binding(u32 pipeline_id, u32 texture_id, const char *name) {
    if (!pipeline_id || !texture_id || !name) return 0;
    return gpu_create_binding(&(gpu_binding_desc){
        .pipeline = (gpu_pipeline){pipeline_id},
        .textures = {{
            .texture = (gpu_texture){texture_id},
            .name = ns_str_cstr((char *)name),
        }},
    }).id;
}

u32 gpu_create_buffer_texture_binding(u32 pipeline_id, u32 buffer_id, const char *buffer_name,
                                      u32 texture0_id, const char *texture0_name, u32 texture1_id, const char *texture1_name) {
    if (!pipeline_id || !buffer_id || !buffer_name) return 0;
    gpu_binding_desc desc = {0};
    desc.pipeline = (gpu_pipeline){pipeline_id};
    desc.buffers[0] = (gpu_binding_buffer_desc){
        .buffer = (gpu_buffer){buffer_id},
        .name = ns_str_cstr((char *)buffer_name),
    };
    if (texture0_id && texture0_name) {
        desc.textures[0] = (gpu_binding_texture_desc){
            .texture = (gpu_texture){texture0_id},
            .name = ns_str_cstr((char *)texture0_name),
        };
    }
    if (texture1_id && texture1_name) {
        desc.textures[1] = (gpu_binding_texture_desc){
            .texture = (gpu_texture){texture1_id},
            .name = ns_str_cstr((char *)texture1_name),
        };
    }
    return gpu_create_binding(&desc).id;
}

u32 gpu_create_depth_pass(u32 depth_texture_id) {
    if (!depth_texture_id) return 0;
    return gpu_create_render_pass(&(gpu_render_pass_desc){
        .depth = {
            .desc = {.texture = (gpu_texture){depth_texture_id}},
            .load_action = LOAD_ACTION_CLEAR,
            .store_action = STORE_ACTION_STORE,
            .clear_value = 1.0f,
        },
    }).id;
}

u32 gpu_create_screen_pass(f64 r, f64 g, f64 b, f64 a) {
    return gpu_create_render_pass(&(gpu_render_pass_desc){
        .colors = {{
            .load_action = LOAD_ACTION_CLEAR,
            .store_action = STORE_ACTION_STORE,
            .clear_value = {(f32)r, (f32)g, (f32)b, (f32)a},
        }},
        .depth = {
            .load_action = LOAD_ACTION_CLEAR,
            .store_action = STORE_ACTION_DONTCARE,
            .clear_value = 1.0f,
        },
        .screen = true,
    }).id;
}

void gpu_destroy_buffer_id(u32 buffer_id) {
    gpu_destroy_buffer((gpu_buffer){buffer_id});
}

void gpu_destroy_texture_id(u32 texture_id) {
    gpu_destroy_texture((gpu_texture){texture_id});
}

void gpu_destroy_binding_id(u32 binding_id) {
    gpu_destroy_binding((gpu_binding){binding_id});
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

void gpu_destroy_render_pass_id(u32 pass_id) {
    gpu_destroy_render_pass((gpu_render_pass){pass_id});
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

void gpu_set_binding_id(u32 binding_id) {
    gpu_set_binding((gpu_binding){binding_id});
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

// ============================================================================
// v2 portable core (doc/gpu.md)
// ============================================================================

// Virtual addresses encode (slot + 1) in the bits above the 40-bit offset, so
// one allocation spans at most 1 TiB and address arithmetic never leaves it.
enum { GPU_V2_OFFSET_BITS = 40 };
#define GPU_V2_OFFSET_MASK ((1ull << GPU_V2_OFFSET_BITS) - 1)

typedef struct gpu_v2_slot {
    u64 size;
    u32 flags;
    ns_bool used;
    ns_bool backend;  // backing lives in the backend, not in `shadow`
    u64 base_va;      // nonzero when the backend exposed a real device address
    u8 *shadow;       // host backing for slots created without a backend
} gpu_v2_slot;

typedef struct gpu_v2_core {
    const gpu_v2_ops *ops;
    u32 caps;

    gpu_v2_slot *slots;
    u32 slot_count;

    gpu_addr ring_base;
    u64 ring_head;
    u32 ring_section;

    gpu_v2_state_desc states[GPU_V2_STATE_POOL_SIZE];
    u32 state_count;
} gpu_v2_core;

static gpu_v2_core _v2 = {0};

void gpu_v2_set_backend(const gpu_v2_ops *ops, u32 caps) {
    _v2.ops = ops;
    _v2.caps = ops ? caps : 0;
}

u32 gpu_caps(void) {
    return _v2.caps;
}

static gpu_addr gpu_v2_encode(u32 slot, u64 offset) {
    gpu_v2_slot *s = &_v2.slots[slot];
    if (s->base_va) return s->base_va + offset;
    return ((u64)(slot + 1) << GPU_V2_OFFSET_BITS) | offset;
}

// Resolve an address to (slot, offset). Real device ranges are matched first;
// the virtual encoding cannot collide with them in practice because backends
// only report base_va on tiers whose allocations the core never virtualizes.
static ns_bool gpu_v2_decode(gpu_addr addr, u32 *out_slot, u64 *out_offset) {
    if (!addr) return false;
    for (u32 i = 0; i < _v2.slot_count; i++) {
        gpu_v2_slot *s = &_v2.slots[i];
        if (!s->used || !s->base_va) continue;
        if (addr >= s->base_va && addr < s->base_va + s->size) {
            *out_slot = i;
            *out_offset = addr - s->base_va;
            return true;
        }
    }
    u64 slot_bits = addr >> GPU_V2_OFFSET_BITS;
    if (slot_bits == 0 || slot_bits > _v2.slot_count) return false;
    u32 slot = (u32)(slot_bits - 1);
    gpu_v2_slot *s = &_v2.slots[slot];
    u64 offset = addr & GPU_V2_OFFSET_MASK;
    if (!s->used || s->base_va || offset >= s->size) return false;
    *out_slot = slot;
    *out_offset = offset;
    return true;
}

gpu_addr gpu_malloc(u64 size, u32 flags) {
    if (size == 0 || size > GPU_V2_OFFSET_MASK) return 0;

    u32 slot = _v2.slot_count;
    for (u32 i = 0; i < _v2.slot_count; i++) {
        if (!_v2.slots[i].used) { slot = i; break; }
    }
    if (slot == _v2.slot_count) {
        if (((u64)_v2.slot_count + 1) << GPU_V2_OFFSET_BITS == 0) return 0;
        gpu_v2_slot *grown = (gpu_v2_slot *)realloc(_v2.slots, sizeof(gpu_v2_slot) * (_v2.slot_count + 1));
        if (!grown) return 0;
        _v2.slots = grown;
        _v2.slot_count++;
    }

    gpu_v2_slot *s = &_v2.slots[slot];
    memset(s, 0, sizeof(*s));
    s->size = size;
    s->flags = flags;

    if (_v2.ops && _v2.ops->mem_create) {
        u64 base_va = 0;
        if (!_v2.ops->mem_create(slot, size, flags, &base_va)) return 0;
        s->backend = true;
        s->base_va = base_va;
    } else {
        // No backend: keep the allocation host-side so headless programs run
        // with deterministic (zeroed) memory semantics.
        s->shadow = (u8 *)calloc(1, size);
        if (!s->shadow) return 0;
    }

    s->used = true;
    return gpu_v2_encode(slot, 0);
}

void gpu_free(gpu_addr addr) {
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(addr, &slot, &offset)) return;
    if (offset != 0) {
        ns_warn("gpu", "gpu_free: address is not an allocation base.\n");
        return;
    }
    gpu_v2_slot *s = &_v2.slots[slot];
    if (s->backend && _v2.ops && _v2.ops->mem_destroy) _v2.ops->mem_destroy(slot);
    if (s->shadow) free(s->shadow);
    memset(s, 0, sizeof(*s));
}

void gpu_write(gpu_addr dst, const void *src, u64 size) {
    if (!src || size == 0) return;
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(dst, &slot, &offset)) return;
    gpu_v2_slot *s = &_v2.slots[slot];
    if (size > s->size - offset) {
        ns_warn("gpu", "gpu_write: range exceeds allocation.\n");
        return;
    }
    if (s->backend) {
        if (_v2.ops && _v2.ops->mem_write) _v2.ops->mem_write(slot, offset, src, size);
        return;
    }
    memcpy(s->shadow + offset, src, size);
}

ns_bool gpu_read(gpu_addr src, void *dst, u64 size) {
    if (!dst || size == 0) return false;
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(src, &slot, &offset)) return false;
    gpu_v2_slot *s = &_v2.slots[slot];
    if (size > s->size - offset) return false;
    if (s->backend) {
        if (_v2.ops && _v2.ops->mem_read) return _v2.ops->mem_read(slot, offset, dst, size);
        return false;
    }
    memcpy(dst, s->shadow + offset, size);
    return true;
}

void *gpu_addr_host(gpu_addr addr) {
    if (!(_v2.caps & GPU_CAP_RAW_POINTERS)) return NULL;
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(addr, &slot, &offset)) return NULL;
    gpu_v2_slot *s = &_v2.slots[slot];
    if (!s->backend || !_v2.ops || !_v2.ops->mem_host_ptr) return NULL;
    u8 *base = (u8 *)_v2.ops->mem_host_ptr(slot);
    return base ? base + offset : NULL;
}

gpu_addr gpu_frame_alloc(u64 size, u32 align) {
    if (size == 0) return 0;
    if (align == 0 || (align & (align - 1)) != 0) align = GPU_V2_ALLOC_ALIGN;

    if (!_v2.ring_base) {
        _v2.ring_base = gpu_malloc(GPU_V2_FRAME_RING_SIZE, GPU_MEM_SHARED);
        if (!_v2.ring_base) return 0;
        _v2.ring_head = 0;
        _v2.ring_section = 0;
    }

    u64 section_size = GPU_V2_FRAME_RING_SIZE / GPU_SWAP_BUFFER_COUNT;
    u64 head = (_v2.ring_head + align - 1) & ~((u64)align - 1);
    if (head + size > section_size) {
        ns_warn("gpu", "gpu_frame_alloc: frame ring exhausted.\n");
        return 0;
    }
    _v2.ring_head = head + size;
    return _v2.ring_base + (u64)_v2.ring_section * section_size + head;
}

void gpu_v2_frame_end(void) {
    _v2.ring_section = (_v2.ring_section + 1) % GPU_SWAP_BUFFER_COUNT;
    _v2.ring_head = 0;
}

u32 gpu_texture_create(i32 width, i32 height, i32 depth_or_layers,
                       i32 format, u32 usage, i32 mip_count, i32 kind) {
    if (width <= 0 || height <= 0) return 0;
    if (!_v2.ops || !_v2.ops->texture_create) return 0;
    return _v2.ops->texture_create(width, height, depth_or_layers < 1 ? 1 : depth_or_layers,
                                   format, usage, mip_count < 1 ? 1 : mip_count, kind);
}

void gpu_texture_upload(u32 tex, i32 mip, i32 layer, const void *data, u64 size) {
    if (!tex || !data || size == 0) return;
    if (_v2.ops && _v2.ops->texture_upload) _v2.ops->texture_upload(tex, mip, layer, data, size);
}

void gpu_texture_destroy(u32 tex) {
    if (tex && _v2.ops && _v2.ops->texture_destroy) _v2.ops->texture_destroy(tex);
}

u32 gpu_sampler_create(i32 min_filter, i32 mag_filter, i32 mip_filter,
                       i32 wrap_u, i32 wrap_v, i32 wrap_w,
                       i32 compare_func, i32 max_anisotropy) {
    if (!_v2.ops || !_v2.ops->sampler_create) return 0;
    return _v2.ops->sampler_create(min_filter, mag_filter, mip_filter,
                                   wrap_u, wrap_v, wrap_w, compare_func, max_anisotropy);
}

void gpu_sampler_destroy(u32 smp) {
    if (smp && _v2.ops && _v2.ops->sampler_destroy) _v2.ops->sampler_destroy(smp);
}

u32 gpu_shader_graphics_create(const char *vs_src, const char *fs_src,
                               const char *vs_entry, const char *fs_entry) {
    if (!vs_src || !fs_src || !vs_entry || !fs_entry) return 0;
    if (!_v2.ops || !_v2.ops->shader_graphics_create) return 0;
    return _v2.ops->shader_graphics_create(vs_src, fs_src, vs_entry, fs_entry);
}

u32 gpu_shader_compute_create(const char *src, const char *entry) {
    if (!src || !entry) return 0;
    if (!_v2.ops || !_v2.ops->shader_compute_create) return 0;
    return _v2.ops->shader_compute_create(src, entry);
}

void gpu_shader_destroy(u32 shader) {
    if (shader && _v2.ops && _v2.ops->shader_destroy) _v2.ops->shader_destroy(shader);
}

u32 gpu_state_create(i32 primitive_type, i32 cull_mode, i32 face_winding,
                     i32 depth_compare, ns_bool depth_write,
                     i32 blend_preset, u32 color_mask) {
    gpu_v2_state_desc desc = {
        .primitive_type = primitive_type,
        .cull_mode = cull_mode,
        .face_winding = face_winding,
        .depth_compare = depth_compare,
        .depth_write = depth_write,
        .blend_preset = blend_preset,
        .color_mask = color_mask,
    };
    for (u32 i = 0; i < _v2.state_count; i++) {
        if (memcmp(&_v2.states[i], &desc, sizeof(desc)) == 0) return i + 1;
    }
    if (_v2.state_count >= GPU_V2_STATE_POOL_SIZE) {
        ns_warn("gpu", "gpu_state_create: state pool exhausted.\n");
        return 0;
    }
    _v2.states[_v2.state_count] = desc;
    return ++_v2.state_count;
}

void gpu_pass_begin(u32 color0, u32 color1, u32 color2, u32 color3,
                    u32 depth, u32 load_flags,
                    f64 r, f64 g, f64 b, f64 a, f64 depth_clear) {
    if (!_v2.ops || !_v2.ops->pass_begin) return;
    gpu_color clear = {(f32)r, (f32)g, (f32)b, (f32)a};
    _v2.ops->pass_begin(color0, color1, color2, color3, depth, load_flags, clear, (f32)depth_clear);
}

void gpu_screen_pass_begin(f64 r, f64 g, f64 b, f64 a) {
    if (!_v2.ops || !_v2.ops->screen_pass_begin) return;
    gpu_color clear = {(f32)r, (f32)g, (f32)b, (f32)a};
    _v2.ops->screen_pass_begin(clear);
}

void gpu_pass_end(void) {
    if (_v2.ops && _v2.ops->pass_end) _v2.ops->pass_end();
}

void gpu_set_shader(u32 shader) {
    if (shader && _v2.ops && _v2.ops->set_shader) _v2.ops->set_shader(shader);
}

void gpu_set_state(u32 state) {
    if (!state || state > _v2.state_count) return;
    if (_v2.ops && _v2.ops->set_state) _v2.ops->set_state(&_v2.states[state - 1]);
}

void gpu_set_root(gpu_addr args) {
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(args, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->set_root) _v2.ops->set_root(slot, offset, args);
}

void gpu_set_root_data(const void *data, u64 size) {
    if (!data || size == 0) return;
    gpu_addr addr = gpu_frame_alloc(size, GPU_V2_ALLOC_ALIGN);
    if (!addr) return;
    gpu_write(addr, data, size);
    gpu_set_root(addr);
}

void gpu_draw_vertices(i32 vertex_base, i32 vertex_count, i32 instance_count) {
    if (vertex_count <= 0 || instance_count <= 0) return;
    if (_v2.ops && _v2.ops->draw) _v2.ops->draw(vertex_base, vertex_count, instance_count);
}

void gpu_draw_indexed(gpu_addr indices, i32 index_type,
                      i32 index_count, i32 instance_count, i32 base_vertex) {
    if (index_count <= 0 || instance_count <= 0 || index_type == INDEX_NONE) return;
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(indices, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->draw_indexed) {
        _v2.ops->draw_indexed(slot, offset, index_type, index_count, instance_count, base_vertex);
    }
}

void gpu_draw_indirect(gpu_addr args, i32 draw_count, i32 stride) {
    if (draw_count <= 0) return;
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(args, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->draw_indirect) _v2.ops->draw_indirect(slot, offset, draw_count, stride);
}

void gpu_dispatch(i32 x, i32 y, i32 z) {
    if (x <= 0 || y <= 0 || z <= 0) return;
    if (_v2.ops && _v2.ops->dispatch) _v2.ops->dispatch(x, y, z);
}

void gpu_dispatch_indirect(gpu_addr args) {
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(args, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->dispatch_indirect) _v2.ops->dispatch_indirect(slot, offset);
}

void gpu_signal_after(gpu_addr addr, u64 value) {
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(addr, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->signal_after) _v2.ops->signal_after(slot, offset, value);
}

void gpu_wait_before(gpu_addr addr, u64 value) {
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(addr, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->wait_before) _v2.ops->wait_before(slot, offset, value);
}
