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

void gpu_set_viewport(int x, int y, int width, int height) { ns_unused(x); ns_unused(y); ns_unused(width); ns_unused(height); }
void gpu_set_scissor(int x, int y, int width, int height) { ns_unused(x); ns_unused(y); ns_unused(width); ns_unused(height); }
void gpu_commit(void) { gpu_v2_frame_end(); }

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
    u32 storage_slot_count;

    gpu_v2_slot *slots;
    u32 slot_count;

    gpu_addr ring_base;
    u64 ring_head;
    u32 ring_section;

    gpu_v2_state_desc states[GPU_V2_STATE_POOL_SIZE];
    u32 state_count;
} gpu_v2_core;

static gpu_v2_core _v2 = {0};

void gpu_v2_set_backend(const gpu_v2_ops *ops, u32 caps, u32 storage_slot_count) {
    _v2.ops = ops;
    _v2.caps = ops ? caps : 0;
    _v2.storage_slot_count = ops ? storage_slot_count : 0;
}

u32 gpu_caps(void) {
    return _v2.caps;
}

u32 gpu_storage_slot_count(void) {
    return _v2.storage_slot_count;
}

// Generated shaders name every storage resource ns_storage_buffer_<slot> on
// all targets. Find the highest referenced slot without imposing a compiler
// ceiling; the active backend's runtime limit is authoritative.
static u32 gpu_shader_storage_slot_count(const char *source) {
    static const char prefix[] = "ns_storage_buffer_";
    u32 count = 0;
    if (!source) return 0;
    const char *at = source;
    while ((at = strstr(at, prefix)) != NULL) {
        at += sizeof(prefix) - 1;
        if (*at < '0' || *at > '9') continue;
        u64 index = 0;
        while (*at >= '0' && *at <= '9') {
            u64 digit = (u64)(*at - '0');
            if (index > (0xffffffffu - digit) / 10u) index = 0xffffffffu;
            else index = index * 10u + digit;
            at++;
        }
        u32 required = index >= 0xffffffffu ? 0xffffffffu : (u32)index + 1;
        if (required > count) count = required;
    }
    return count;
}

static ns_bool gpu_shader_storage_slots_valid(const char *source, const char *operation) {
    u32 required = gpu_shader_storage_slot_count(source);
    if (required <= _v2.storage_slot_count) return true;
    ns_warn("gpu", "%s: shader requires %u storage slots, but the current platform supports %u.\n",
            operation, required, _v2.storage_slot_count);
    return false;
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

    // Keep every swap section's base aligned: integer division of the 4 MiB
    // ring by three otherwise produces the invalid Metal offset 0x155555.
    u64 section_size = (GPU_V2_FRAME_RING_SIZE / GPU_SWAP_BUFFER_COUNT) & ~((u64)GPU_V2_ALLOC_ALIGN - 1);
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
    if (_v2.ops && _v2.ops->texture_upload) {
        _v2.ops->texture_upload(tex, mip, layer, data, size);
        return;
    }
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
    if (!gpu_shader_storage_slots_valid(vs_src, "gpu_shader_graphics_create") ||
        !gpu_shader_storage_slots_valid(fs_src, "gpu_shader_graphics_create")) return 0;
    return _v2.ops->shader_graphics_create(vs_src, fs_src, vs_entry, fs_entry);
}

u32 gpu_shader_compute_create(const char *src, const char *entry) {
    if (!src || !entry) return 0;
    if (!_v2.ops || !_v2.ops->shader_compute_create) return 0;
    if (!gpu_shader_storage_slots_valid(src, "gpu_shader_compute_create")) return 0;
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

void gpu_set_storage(gpu_addr addr) {
    gpu_set_storage_at(0, addr);
}

void gpu_set_storage_at(i32 index, gpu_addr addr) {
    if (index < 0 || (u32)index >= _v2.storage_slot_count) return;
    u32 slot;
    u64 offset;
    if (!gpu_v2_decode(addr, &slot, &offset)) return;
    if (_v2.ops && _v2.ops->set_storage) _v2.ops->set_storage((u32)(3 + index), slot, offset, addr);
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
