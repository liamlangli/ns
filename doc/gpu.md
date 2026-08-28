# GPU Module

> Reference: Sebastian Aaltonen, ["No Graphics API"](https://www.sebastianaaltonen.com/blog/no-graphics-api)
> (Dec 2025). Related reading: the
> [Hacker News discussion](https://news.ycombinator.com/item?id=46293062),
> ["Thoughts on No Graphics API"](https://www.corsix.org/content/thoughts-on-no-graphics-api),
> and ["Writing a bindless GPU abstraction layer"](https://www.kevin-gibson.com/blog/writing-a-bindless-gpu-abstraction-layer/).

## Public API

The `gpu` built-in module is designed around the ideas in "No Graphics API":
treat the GPU as a processor with memory rather than a state machine with
bound objects. This memory-addressed API is the only Nano Script GPU surface,
including the native UI renderer and platform backends.

## The article's argument, in short

Modern GPU hardware has converged: unified cache hierarchies, scalar ISAs,
64-bit pointers usable directly in shaders, and bindless descriptor heaps.
Graphics APIs still expose abstractions designed for 2015 hardware:

- **Buffers as objects.** A buffer is really just memory. CUDA-style
  `gpuMalloc` returning a 64-bit device address replaces buffer objects,
  buffer views, and every buffer descriptor. Suballocation is pointer
  arithmetic. With UMA or PCIe ReBAR the CPU writes GPU memory directly.
- **Descriptor sets / bind groups.** With bindless heaps a shader can reach
  any texture by a `u32` heap index, and any memory by pointer. Shader input
  collapses to a **single root pointer** to a user-defined struct which may
  contain nested pointers, texture indices, and constants. `draw()` and
  `dispatch()` just take that pointer.
- **Textures stay opaque.** Layouts and compression (DCC etc.) are
  proprietary, so texture *storage* keeps an object; texture *access* is a
  heap index, not a binding.
- **Monolithic PSOs.** Full pipeline state at compile time causes permutation
  explosion and load stutter; most "state" is really a shader-compile detail
  the driver caches anyway.
- **Fine-grained barriers.** Drivers largely discard per-resource dependency
  lists; what the hardware needs is execution ordering plus cache flushes.
  A simple global barrier between passes covers most rendering. For overlap
  (async compute) the article proposes futex-like **split barriers**:
  `gpuSignalAfter(addr, value)` / `gpuWaitBefore(addr, value)` on a memory
  location.

## What we adopt, and what we deliberately do not

Adopted:

- GPU memory as first-class: `gpu_malloc` → 64-bit address (`gpu_addr`),
  pointer arithmetic in ns, persistent CPU access on shared memory.
- Single root pointer as the only shader input mechanism. No binding
  objects, no name lookup, no per-pipeline descriptor layouts.
- Bindless textures and samplers as `u32` heap indices stored in ordinary
  data (including inside GPU memory).
- No vertex layouts and no mesh objects: vertex fetch is shader code reading
  from a pointer. Only the index stream stays fixed-function, passed as an
  address at draw time.
- Draws and dispatches as plain calls taking counts and addresses, including
  indirect variants reading arguments from GPU memory.
- Passes ordered by simple global barriers by default; split-barrier
  signal/wait for overlap, gated behind a capability bit.

Not adopted (and why):

- **Removing render passes.** Tile-based GPUs (all Apple targets) and WebGPU
  require pass boundaries with load/store actions. We keep `begin_pass` /
  `end_pass`, but passes are plain calls with attachment arguments and no
  public pass object.
- **Removing pipeline state entirely.** Metal, DX12, and WebGPU all compile
  monolithic pipelines under the hood. Instead of exposing that, the backend
  keeps an internal PSO cache keyed by (shader, render state, attachment
  formats). The *API* stops forcing users to enumerate permutations; the
  cache pays the cost once per new combination.
- **User-visible texture layout transitions.** Backends keep handling
  layout/decompression internally.
- **Raw 256-bit descriptors in user memory.** Portability tiering (below)
  needs the module to own descriptor storage.

## Why ns can go further than the article

The article targets C++ users writing HLSL/MSL by hand, so HLSL's lack of
raw pointers is a real obstacle. ns is better positioned: the `shader`
module transpiles ns functions into MSL/HLSL/GLSL/WGSL, so **`gpu_addr` and
`gpu_texture` are language-level types whose lowering is per-backend**:

| Concept | Metal | DX12 (SM 6.6) | Vulkan (future) | WebGPU |
| --- | --- | --- | --- | --- |
| `gpu_addr` deref | device pointer (`gpuAddress`, argument buffer) | pooled `ByteAddressBuffer` via `ResourceDescriptorHeap[pool]` + offset | `VK_KHR_buffer_device_address` pointer | pooled storage buffer in a bind group + offset |
| `gpu_texture` index | texture in argument-buffer heap | `ResourceDescriptorHeap[index]` | descriptor-indexed heap | per-draw bind group patched from indices the root struct actually uses |
| root pointer | `setVertexBytes`/argument buffer address | root constants carrying address | push constants carrying address | small uniform buffer |

On backends without hardware pointers, `gpu_malloc` allocates from large
pooled buffers and returns a *virtual* address: `pool_index << 40 | offset`.
Address arithmetic still works because suballocation never crosses a pool.
The transpiler compiles a deref into a pool-indexed load. The public API is
identical on every tier; only performance differs.

Capability bits let advanced callers detect the tier:

```c
enum gpu_caps_flags {
    GPU_CAP_RAW_POINTERS      = 1 << 0, // real VAs, gpu_addr_host() works
    GPU_CAP_BINDLESS_TEXTURES = 1 << 1, // texture indices readable from GPU memory
    GPU_CAP_INDIRECT_DRAW     = 1 << 2,
    GPU_CAP_ASYNC_COMPUTE     = 1 << 3, // split barriers are real
    GPU_CAP_READBACK          = 1 << 4,
};
u32 gpu_caps(void);
```

## Core API (C, `lib/include/gpu.h`)

Everything below is FFI-safe: scalars, `u64` addresses, `u32` indices,
pointers+lengths. No descriptor structs cross the boundary; no arrays are
fixed at 8.

### Device and frame

```c
ns_bool gpu_request_device(view *v);
void    gpu_destroy_device(void);
u32     gpu_caps(void);
u32     gpu_storage_slot_count(void); // 0 without a device

void gpu_commit(void);        // submit + present, recycles the frame ring
```

### Memory

```c
typedef u64 gpu_addr;                    // 0 = null

enum gpu_mem_flags {
    GPU_MEM_DEVICE = 0,                  // GPU-only (render targets aside, the default)
    GPU_MEM_SHARED = 1,                  // CPU-visible, persistently mapped
};

gpu_addr gpu_malloc(u64 size, u32 flags, const char *name);
void     gpu_free(gpu_addr addr);

// Write/read through the frame's transfer stream (device memory) or memcpy
// (shared memory). Valid on every tier. gpu_read needs GPU_CAP_READBACK and
// does not order against work in flight - see below.
void     gpu_write(gpu_addr dst, const void *src, u64 size);
ns_bool  gpu_read(gpu_addr src, void *dst, u64 size);
```

`gpu_read` costs what the backend's allocations cost. Where `gpu_malloc` hands
back host-visible memory - Metal, whose allocations are persistently mapped
whichever flag they were made with - it is a plain memcpy out of the same bytes
the shaders read, not a pipeline drain, so reading a handful of words back to
patch them is cheap enough to do while a frame is being built.

What it does not do is wait. A read is not ordered against GPU work already
submitted, so reading a buffer a dispatch has just written can return the bytes
that were there before it. Read something a dispatch produced only once that
work has completed - a later frame, not the one that queued it.

```c
// Host pointer of a SHARED allocation. NULL unless GPU_CAP_RAW_POINTERS.
void    *gpu_addr_host(gpu_addr addr);

// Per-frame transient allocation from an internal ring; freed automatically
// when the frame's GPU work completes. The dynamic-data workhorse (ui.c).
gpu_addr gpu_frame_alloc(u64 size, u32 align);
```

Suballocation is user-side address arithmetic; `gpu_addr + offset` is
always valid within one allocation. Every allocation requires a non-empty,
specific debug `name`; the backend exposes it to GPU capture and debugging
tools (for example, as an `MTLBuffer` label or a DirectX 12 object name).

### Textures and samplers

```c
// Returns a bindless heap index (plain u32); 0 is invalid. The index is
// plain data: store it in structs, arrays, or GPU memory.
u32  gpu_texture_create(i32 width, i32 height, i32 depth_or_layers,
                        i32 format, u32 usage, i32 mip_count, i32 kind);
void gpu_texture_upload(u32 tex, i32 mip, i32 layer,
                        const void *data, u64 size);
void gpu_texture_destroy(u32 tex);

u32  gpu_sampler_create(i32 min_filter, i32 mag_filter, i32 mip_filter,
                        i32 wrap_u, i32 wrap_v, i32 wrap_w,
                        i32 compare_func, i32 max_anisotropy);
void gpu_sampler_destroy(u32 smp);
```

`usage` uses read/write/render-target bits; storage (UAV) access uses
the same index. A texture declared with only the render-target bit is transient:
its contents cannot be loaded or read after the pass, and the backend defaults
its store action to `dontcare`. Tile-based backends use memoryless storage for
such attachments when the device supports it. Add the read or write bit when
the rendered contents must survive the pass. Samplers come from a small global
heap — the article's observation that real programs need a handful.

### Shaders and render state

Shader creation is the most expensive thing a launch does, so it is cached at
two levels. Within a run, identical source compiles once: the transpiler emits
one self-contained source per entry, so passes sharing a stage fn (four bloom
passes over one vertex fn) hand the backend the same text repeatedly. Across
runs, the Metal backend keeps a per-shader `MTLBinaryArchive` of the pipeline
states the shader has been drawn with, stored through `storage`'s blob cache
under the entry-point names and a content hash of the source.

Both keys are derived inside `gpu_shader_*_create` rather than asked of the
caller, so every call site is cached without changing. The name comes from the
entry points and the hash from the source text, which means an edited shader fn
misses and recompiles instead of loading a stale binary.

Caching the archive rather than a compiled library is forced by the platform:
Metal cannot serialize a library it compiled from source, and the offline metal
compiler does not exist on iOS or visionOS. So the source is still compiled
every launch and what a warm launch skips is turning those functions into
pipeline states. The archive is consulted only the first time a shader meets a
given render state - Metal serves later repeats from its own in-process cache
faster than an archive lookup does, and a shader whose state changes between
draws asks thousands of times per second.

A cache miss is never an error. Storage that is unavailable, an archive written
by a different OS or GPU, and a device that cannot serialize archives all fall
back to compiling, which is what the program did before any of this existed.

```c
// shader = compiled program (vertex+fragment, or compute); state = immutable
// render-state key (NOT a PSO). Both plain u32 ids, 0 invalid.
u32  gpu_shader_graphics_create(const char *vs_src, const char *fs_src,
                                const char *vs_entry, const char *fs_entry);
u32  gpu_shader_compute_create(const char *src, const char *entry);
void gpu_shader_destroy(u32 shader);

// Cheap, value-cached: equal arguments return the same id. No shader, no
// vertex layout, no attachment formats — those come from the bound shader
// and the active pass. The backend hashes (shader, state, pass formats)
// into its internal PSO cache on first use.
u32 gpu_state_create(i32 primitive_type, i32 cull_mode, i32 face_winding,
                     i32 depth_compare, ns_bool depth_write,
                     i32 blend_preset,   // off / alpha / premultiplied / additive
                     u32 color_mask);
```

No `gpu_pipeline`, no `gpu_binding`, no `gpu_mesh`, no vertex layout tables.

### Passes

```c
// Attachments are texture indices; 0 = unused. No pass objects. load_flags
// packs a gpu_load_action (clear/load/dontcare) per attachment
// (GPU_PASS_COLOR0_SHIFT .. GPU_PASS_DEPTH_SHIFT).
//
// label states what the pass does and is required, like the gpu_malloc name.
void gpu_pass_begin(const char *label,
                    u32 color0, u32 color1, u32 color2, u32 color3,
                    u32 depth, u32 load_flags,
                    f64 r, f64 g, f64 b, f64 a, f64 depth_clear);
void gpu_screen_pass_begin(const char *label, f64 r, f64 g, f64 b, f64 a);
void gpu_pass_end(void);

void gpu_set_viewport(i32 x, i32 y, i32 w, i32 h);
void gpu_set_scissor(i32 x, i32 y, i32 w, i32 h);
```

### Binding and drawing

```c
void gpu_set_shader(u32 shader);
void gpu_set_state(u32 state);

// The single root argument. Either point at GPU memory you manage...
void gpu_set_root(gpu_addr args);
// ...or copy a small struct into the frame ring and point at that.
void gpu_set_root_data(const void *data, u64 size);

// Portable storage slots used by the current shader subset. The active
// platform's zero-based slot count is reported by gpu_storage_slot_count();
// shader creation fails when source refers to a slot outside that range.
void gpu_set_storage(gpu_addr addr);
void gpu_set_storage_at(i32 index, gpu_addr addr);

void gpu_draw_vertices(i32 vertex_base, i32 vertex_count, i32 instance_count);
void gpu_draw_indexed(gpu_addr indices, i32 index_type,
                      i32 index_count, i32 instance_count, i32 base_vertex);

// GPU-driven: argument structs read from GPU memory (GPU_CAP_INDIRECT_DRAW).
void gpu_draw_indirect(gpu_addr args, i32 draw_count, i32 stride);
void gpu_dispatch(const char *label, i32 x, i32 y, i32 z);
void gpu_dispatch_indirect(const char *label, gpu_addr args);
```

Compute uses the same flow: `gpu_set_shader(compute)`, `gpu_set_root*`,
`gpu_dispatch`. Each dispatch is its own compute pass, so it carries its own
label.

### Pass labels

Every pass names what it does: `"shadow depth"`, `"bloom downsample"`,
`"terrain raycast"`. The label becomes the command encoder name, so a Metal or
RenderDoc frame capture lists the frame as the render graph the program
actually submits instead of a column of anonymous passes. Name the work, not
the API call (`"g-buffer"`, not `"pass 3"`), and keep the name stable across
frames so captures stay comparable. An empty label degrades to a generic
placeholder rather than dropping the pass.

Compute texture intrinsics bind the read texture from root word 0, the primary
writable texture from word 1, and the optional RGBA8 secondary writable texture
from word 2. `shader_write_texture_secondary` lets one invocation emit an
auxiliary target without a second dispatch.

### Synchronization

```c
// Implicit default: passes and dispatches execute in submission order with a
// full barrier between passes. Correct everywhere, optimal almost everywhere.

// Split barriers for overlap, after the article's futex design: a signal
// writes `value` to a GPU address when preceding work completes; a wait
// blocks subsequent work until the address holds `value`. On tiers without
// GPU_CAP_ASYNC_COMPUTE these degrade to the implicit global barrier.
void gpu_signal_after(gpu_addr addr, u64 value);
void gpu_wait_before(gpu_addr addr, u64 value);
```

## Shader ABI

A v2 graphics shader is a pair of ns fns whose first parameter is a `ref`
to the root struct; builtins arrive as dedicated parameters. Vertex fetch is
ordinary code:

```ns
use simd
use gpu

struct sprite_vertex { pos: float2, uv: float2, col: float4 }

struct sprite_args {
    vertices: gpu_addr,      // -> [sprite_vertex]
    view_size: float2,
    atlas: gpu_texture,
    atlas_smp: gpu_sampler,
}

fn vs_sprite(args: ref sprite_args, vid: vertex_id) sprite_varying {
    let v = gpu_load[sprite_vertex](args.vertices, vid)
    let ndc = float2(v.pos.x / args.view_size.x * 2.0 - 1.0,
                     1.0 - v.pos.y / args.view_size.y * 2.0)
    return sprite_varying(float4(ndc, 0.0, 1.0), v.uv, v.col)
}

fn fs_sprite(args: ref sprite_args, in: sprite_varying) float4 {
    return in.col * sample(args.atlas, args.atlas_smp, in.uv)
}
```

Transpiler work this needs (`shader` module):

- `gpu_addr` as a typed device-address in shader code, with
  `gpu_load[T](addr, index)` / `gpu_store[T](addr, index, value)` intrinsics.
  Lowered to pointer deref (MSL/GLSL), `ByteAddressBuffer.Load` on a
  heap-indexed pool (HLSL), or a pooled storage-buffer access (WGSL).
- `gpu_texture`/`gpu_sampler` as heap indices; `sample(tex, smp, uv)`
  lowered per backend. On WebGPU the transpiler records which root-struct
  fields are texture indices so the runtime can patch a bind group per draw.
- Builtins: `vertex_id`, `instance_id`, `thread_id` (compute).
- Root struct reflection emitted once per shader (field offsets + which
  fields are addresses/textures) — used only by portable-tier backends, never
  by user code.

## ns surface (`lib/gpu.ns`)

The declarations in `lib/gpu.ns` mirror the C
surface one-to-one with FFI-safe scalars: addresses are `u64`, texture /
sampler / shader / state ids are `u32`, and bulk data crosses the boundary
as `[any]` plus a byte size. `gpu_addr_host` stays C-only — a raw host
pointer has no useful ns representation. Constants for capability bits,
memory flags, blend presets, load actions, and texture kinds sit alongside.

### Rich resource handles

On top of the raw ids, `lib/gpu.ns` defines CPU-side handle structs that
keep the id (`resource_id`, 0 = invalid) together with the description the
resource was created from. The structs never cross the FFI — only ids and
addresses do — so the metadata is free, exact, and available headless:

```ns
struct gpu_texture      { resource_id, width, height, depth_or_layers, format, usage, mip_count, kind }
struct gpu_sampler      { resource_id, min/mag/mip filters, wraps, compare_func, max_anisotropy }
struct gpu_shader       { resource_id, compute, target, vertex_entry, fragment_entry }
struct gpu_render_state { resource_id, primitive, cull, winding, depth, blend, mask }
struct gpu_memory       { addr, size, flags }
```

What the metadata buys on the CPU side:

- `gpu_texture_bytes(tex)` sizes uploads from format and extent
  (`gpu_pixel_format_*` layout math is declared to ns for this), and
  `gpu_texture_update_all(tex, data)` uploads a full mip-0 slice with no
  caller-side size bookkeeping.
- `gpu_memory_write/read/at` bounds-check offsets against the allocation's
  extent before an address ever reaches the backend.
- `gpu_pass_begin_target(label, color, depth, ...)` targets rich handles and sets
  the viewport from the color attachment's size.
- `gpu_render_state_new` round-trips the same value-cached id as the raw
  call while keeping every field readable.

Constructors are `gpu_texture_new`/`gpu_texture_new_2d`/`gpu_texture_none`,
`gpu_sampler_new`, `gpu_render_state_new`, `gpu_memory_alloc(size, flags, name)`; teardown is
`gpu_*_release`/`gpu_memory_free`; binding is `gpu_shader_bind` /
`gpu_render_state_bind`.

Two sugar fns wrap the `shader` transpiler and create persistent shader values:

```ns
// Transpile ns fns for the active backend. The result
// carries no vertex layout or attachment formats and can be drawn with any
// state in any pass; the compute variant returns a persistent shader for
// gpu_dispatch. The returned gpu_shader records target and entry names.
fn gpu_shader_graphics(vs: any, fs: any) gpu_shader
fn gpu_shader_compute(f: any) gpu_shader
```

A migrated frame:

```ns
// Data is data; the draw names everything it needs.
let args = sprite_args(g_vertices, view_size(), g_atlas, g_linear)
gpu_screen_pass_begin("sprites", 0.1, 0.1, 0.1, 1.0)
gpu_set_shader(g_shader)
gpu_set_state(g_alpha_blend)
gpu_set_root_data(ref args, 24)
gpu_draw_indexed(g_indices, GPU_INDEX_UINT32, index_count, 1, 0)
gpu_pass_end()
gpu_commit()
```

And a compute-fed indirect draw:

```ns
gpu_set_shader(g_cull_compute)
gpu_set_root_data(ref cull_args, 32)
gpu_dispatch("instance cull", instance_count / 64 + 1, 1, 1)  // writes draw args + count

gpu_screen_pass_begin("scene", 0.0, 0.0, 0.0, 1.0)
gpu_set_shader(g_scene_shader)
gpu_set_state(g_opaque)
gpu_set_root(g_scene_args)                       // resides in GPU memory
gpu_draw_indirect(g_indirect_args, max_draws, 16)
gpu_pass_end()
```

## Backend notes

- **Metal (first target).** `gpu_malloc` uses persistently mapped shared
  `MTLBuffer` allocations behind portable virtual addresses. Root and storage
  addresses bind the corresponding buffer plus offset; texture ids in the root
  select resources for the current shader. Render pipelines are compiled from
  (shader, state, pass formats), and Metal's tracked resources provide the
  default ordering model. Windowed devices present with display sync and three
  swap drawables by default, with up to three command buffers in flight.
- **DX12.** Pool allocations in large committed buffers;
  `GetGPUVirtualAddress` exists, but HLSL lacks raw pointers, so addresses
  stay pooled (`pool << 40 | offset`) and derefs compile to
  `ResourceDescriptorHeap[pool].Load(offset)` under SM 6.6. Root signature:
  one 64-bit root constant (the root address) + the shared descriptor heap.
  Barriers: enhanced barriers with a global scope between passes; split
  barriers via fence signals on the compute queue. The flip-discard swap chain
  presents at the display interval with three back buffers; per-buffer fences
  only stall when the CPU catches the buffer still owned by the GPU.
- **WebGPU (`target = "wasm"`).** Portable browser tier:
  pooled buffers in bind group 0, root struct in a uniform slot, textures
  patched into bind group 1 per draw using the transpiler's root reflection.
  No indirect-count draws; `gpu_draw_indirect` loops on the CPU or degrades
  to `draw_count` fixed submissions. Caps report no raw pointers, no async
  compute. The generated `ns-wasm.js` middleware requests the adapter/device,
  configures the full-page canvas, maps GPU imports to WebGPU
  resources and command passes, and rebuilds after device loss. Build-time
  WGSL and vertex reflection come from the Wasm `ns.shaders` custom section,
  so the browser never compiles Nano Script source. `view_create` returns the
  canvas-backed `ref view` passed to the unchanged typed
  `gpu_request_device(v: ref view)` API.
- **Linux/null.** Every call is safe,
  `gpu_request_device` returns false, `gpu_caps()` returns 0.

## Migration status

The Nano Script and Wasm import surfaces are v2-only. Samples and tests use
persistent graphics/compute shaders, root data, GPU addresses, and bindless
texture IDs. Browser WebGPU supports compute storage textures and requests
the optional `texture-formats-tier1` feature for `R11G11B10F`, falling back
internally to `RGBA16F` when the feature is unavailable.

## Open questions

- `gpu_load[T]` needs a bounded generic form in the shader subset; if
  parametric intrinsics are unwanted, per-type loads
  (`gpu_load_f32x4`, struct loads via `ref` cast) are the fallback.
- Whether `gpu_set_root_data` should accept any ns struct directly (VM
  copies value memory) or require an explicit `[u8]` pack step; the former
  is far more ergonomic, and struct layout already matches the shader ABI.
- Index buffers on WebGPU must come from pools flagged INDEX at
  `gpu_malloc` time (WebGPU usage bits are immutable); a `GPU_MEM_INDEX`
  flag or transparent dual-pool allocation — leaning transparent.
- Whether pass attachments beyond 4 colors matter for ns programs before
  Vulkan lands.
