# GPU Module v2 Design

> Reference: Sebastian Aaltonen, ["No Graphics API"](https://www.sebastianaaltonen.com/blog/no-graphics-api)
> (Dec 2025). Related reading: the
> [Hacker News discussion](https://news.ycombinator.com/item?id=46293062),
> ["Thoughts on No Graphics API"](https://www.corsix.org/content/thoughts-on-no-graphics-api),
> and ["Writing a bindless GPU abstraction layer"](https://www.kevin-gibson.com/blog/writing-a-bindless-gpu-abstraction-layer/).

## Goal

Redesign the `gpu` built-in module around the ideas in "No Graphics API":
treat the GPU as a processor with memory rather than a state machine with
bound objects. The result is a smaller, flatter API that removes the
combinatorial FFI wrappers of v1 and makes new rendering techniques (dynamic
geometry, compute pipelines, GPU-driven drawing) expressible from ns without
touching C.

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

## What v1 gets wrong today

The v1 surface (`lib/include/gpu.h`, `lib/gpu.ns`) is a sokol-style
descriptor API plus a scalar FFI tail. Concrete inflexibilities:

1. **Combinatorial wrappers.** Every binding shape needs a new C symbol:
   `gpu_create_texture_binding` (1 texture), `gpu_create_buffer_texture_binding`
   (exactly 1 buffer + 2 textures), `gpu_create_mesh_1`,
   `gpu_create_mesh_indexed`, `gpu_create_pipeline_layout`,
   `_layout_ex`, `_layout_indexed_ex`… A third texture or a second uniform
   buffer means editing C on three backends.
2. **Geometry welded to pipelines.** `gpu_mesh` captures vertex buffers *and*
   a pipeline; `gpu_binding` captures a pipeline too. Sharing one vertex pool
   across shaders, or drawing one mesh with two shaders, needs duplicate
   objects.
3. **Vertex layout baked into the PSO.** `gpu_pipeline_desc.layout` fixes
   stride/offset/format per pipeline (max 8 attributes), so every vertex
   format change is a new pipeline, and interleaved vs. deinterleaved data is
   an API-level decision instead of a shader detail.
4. **Name-based binding.** Bindings resolve uniform/texture slots by string
   name against shader reflection at create time — per-object, per-pipeline
   descriptor churn the article shows modern hardware doesn't need.
5. **Compute is a dead end.** `gpu_dispatch_compute_source` recompiles source
   on every dispatch, binds at most one texture, and cannot see any buffer.
   Compute cannot feed a draw.
6. **No dynamic data path.** Updating a buffer mid-frame
   (`gpu_update_buffer`) has no ring/streaming story; `ui.c` works around it
   with its own buffer juggling (145 gpu call sites).
7. **No sync control at all.** Single implicit queue, `gpu_commit()` at frame
   end. Fine on Metal's tracked resources; already awkward on DX12, and a
   wall for async compute or GPU-driven techniques.

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
  `end_pass`, but passes are plain calls with attachment arguments — the v1
  `gpu_render_pass` object goes away.
- **Removing pipeline state entirely.** Metal, DX12, and WebGPU all compile
  monolithic pipelines under the hood. Instead of exposing that, the backend
  keeps an internal PSO cache keyed by (shader, render state, attachment
  formats). The *API* stops forcing users to enumerate permutations; the
  cache pays the cost once per new combination.
- **User-visible texture layout transitions.** Backends keep handling
  layout/decompression internally, as v1 already does.
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

## Core API (C, `lib/include/gpu.h` v2)

Everything below is FFI-safe: scalars, `u64` addresses, `u32` indices,
pointers+lengths. No descriptor structs cross the boundary; no arrays are
fixed at 8.

Naming note: while v1 and v2 coexist, v2 entry points that would collide
with a live v1 symbol take a v2 spelling — `gpu_texture_create` (v1
`gpu_create_texture`), `gpu_draw_vertices` (v1 `gpu_draw`),
`gpu_pass_begin`/`gpu_pass_end`/`gpu_screen_pass_begin` (v1
`gpu_begin_render_pass`/`gpu_end_pass`/`gpu_create_screen_pass`). When v1
is deleted these become the only names.

### Device and frame

```c
ns_bool gpu_request_device(view *v);
void    gpu_destroy_device(void);
u32     gpu_caps(void);

void gpu_commit(void);        // submit + present, recycles the frame ring
```

### Memory

```c
typedef u64 gpu_addr;                    // 0 = null

enum gpu_mem_flags {
    GPU_MEM_DEVICE = 0,                  // GPU-only (render targets aside, the default)
    GPU_MEM_SHARED = 1,                  // CPU-visible, persistently mapped
};

gpu_addr gpu_malloc(u64 size, u32 flags);
void     gpu_free(gpu_addr addr);

// Write/read through the frame's transfer stream (device memory) or memcpy
// (shared memory). Valid on every tier.
void     gpu_write(gpu_addr dst, const void *src, u64 size);
ns_bool  gpu_read(gpu_addr src, void *dst, u64 size);   // blocking, tools/tests

// Host pointer of a SHARED allocation. NULL unless GPU_CAP_RAW_POINTERS.
void    *gpu_addr_host(gpu_addr addr);

// Per-frame transient allocation from an internal ring; freed automatically
// when the frame's GPU work completes. The dynamic-data workhorse (ui.c).
gpu_addr gpu_frame_alloc(u64 size, u32 align);
```

Suballocation is user-side address arithmetic; `gpu_addr + offset` is
always valid within one allocation.

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

`usage` keeps v1's read/write/render-target bits; storage (UAV) access uses
the same index. Samplers come from a small global heap — the article's
observation that real programs need a handful.

### Shaders and render state

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
void gpu_pass_begin(u32 color0, u32 color1, u32 color2, u32 color3,
                    u32 depth, u32 load_flags,
                    f64 r, f64 g, f64 b, f64 a, f64 depth_clear);
void gpu_screen_pass_begin(f64 r, f64 g, f64 b, f64 a);
void gpu_pass_end(void);

void gpu_set_viewport(i32 x, i32 y, i32 w, i32 h);   // shared with v1
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

void gpu_draw_vertices(i32 vertex_base, i32 vertex_count, i32 instance_count);
void gpu_draw_indexed(gpu_addr indices, i32 index_type,
                      i32 index_count, i32 instance_count, i32 base_vertex);

// GPU-driven: argument structs read from GPU memory (GPU_CAP_INDIRECT_DRAW).
void gpu_draw_indirect(gpu_addr args, i32 draw_count, i32 stride);
void gpu_dispatch(i32 x, i32 y, i32 z);
void gpu_dispatch_indirect(gpu_addr args);
```

Compute uses the same flow: `gpu_set_shader(compute)`, `gpu_set_root*`,
`gpu_dispatch`. The v1 recompile-per-dispatch path disappears.

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

## ns surface (`lib/gpu.ns` v2)

The declarations live in the "v2" section of `lib/gpu.ns` and mirror the C
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
- `gpu_pass_begin_target(color, depth, ...)` targets rich handles and sets
  the viewport from the color attachment's size.
- `gpu_render_state_new` round-trips the same value-cached id as the raw
  call while keeping every field readable.

Constructors are `gpu_texture_new`/`gpu_texture_new_2d`/`gpu_texture_none`,
`gpu_sampler_new`, `gpu_render_state_new`, `gpu_memory_alloc`; teardown is
`gpu_*_release`/`gpu_memory_free`; binding is `gpu_shader_bind` /
`gpu_render_state_bind`.

Two sugar fns wrap the `shader` transpiler, replacing v1's
`gpu_create_pipeline` and the recompile-per-dispatch `dispatch_gpu`:

```ns
// Transpile ns fns for the active backend. Unlike a v1 pipeline the result
// carries no vertex layout or attachment formats and can be drawn with any
// state in any pass; the compute variant returns a persistent shader for
// gpu_dispatch. The returned gpu_shader records target and entry names.
fn gpu_shader_graphics(vs: any, fs: any) gpu_shader
fn gpu_shader_compute(f: any) gpu_shader
```

A frame, before and after:

```ns
// v1: shader -> pipeline(+layout) -> mesh(pipeline+buffers) -> binding(pipeline+names)
let pipeline = gpu_create_pipeline_layout_ex(shader_id, stride, offs, sizes, fmts, n,
                                             color_fmt, prim, depth_fmt, cmp, true, cull, true)
let mesh = gpu_create_mesh_indexed(pipeline, vbuf, ibuf, GPU_INDEX_UINT32)
let binding = gpu_create_texture_binding(pipeline, tex, "u_texture")
gpu_begin_render_pass_id(pass)
gpu_set_pipeline_id(pipeline)
gpu_set_mesh_id(mesh)
gpu_set_binding_id(binding)
gpu_draw(0, index_count, 1)

// v2: data is data; the draw names everything it needs
let args = sprite_args(g_vertices, view_size(), g_atlas, g_linear)
gpu_screen_pass_begin(0.1, 0.1, 0.1, 1.0)
gpu_set_shader(g_shader)
gpu_set_state(g_alpha_blend)
gpu_set_root_data(ref args, 24)
gpu_draw_indexed(g_indices, GPU_INDEX_UINT32, index_count, 1, 0)
gpu_pass_end()
gpu_commit()
```

And a compute-fed indirect draw, impossible in v1:

```ns
gpu_set_shader(g_cull_compute)
gpu_set_root_data(ref cull_args, 32)
gpu_dispatch(instance_count / 64 + 1, 1, 1)     // writes draw args + count

gpu_screen_pass_begin(0.0, 0.0, 0.0, 1.0)
gpu_set_shader(g_scene_shader)
gpu_set_state(g_opaque)
gpu_set_root(g_scene_args)                       // resides in GPU memory
gpu_draw_indirect(g_indirect_args, max_draws, 16)
gpu_pass_end()
```

## Backend notes

- **Metal (first target).** `MTLHeap` + `newBufferWithLength` for
  `gpu_malloc`; `gpuAddress` gives real VAs (GPU_CAP_RAW_POINTERS on
  Apple-silicon UMA, SHARED == DEVICE). Textures live in one argument-buffer
  heap; `useResource:` / `useHeap:` covers residency per pass. Root pointer
  via `setVertexBytes`/`setFragmentBytes` of 8 bytes, or the frame ring.
  PSO cache keyed by (shader, state, pass formats). Implicit hazard tracking
  already gives the default barrier model; split barriers map to
  `MTLEvent`/`MTLSharedEvent` between encoders.
- **DX12.** Pool allocations in large committed buffers;
  `GetGPUVirtualAddress` exists, but HLSL lacks raw pointers, so addresses
  stay pooled (`pool << 40 | offset`) and derefs compile to
  `ResourceDescriptorHeap[pool].Load(offset)` under SM 6.6. Root signature:
  one 64-bit root constant (the root address) + the shared descriptor heap.
  Barriers: enhanced barriers with a global scope between passes; split
  barriers via fence signals on the compute queue.
- **WebGPU (`target = "wasm"`).** Portable browser tier:
  pooled buffers in bind group 0, root struct in a uniform slot, textures
  patched into bind group 1 per draw using the transpiler's root reflection.
  No indirect-count draws; `gpu_draw_indirect` loops on the CPU or degrades
  to `draw_count` fixed submissions. Caps report no raw pointers, no async
  compute. The generated `ns-wasm.js` middleware requests the adapter/device,
  configures the full-page canvas, maps legacy and v2 imports to WebGPU
  resources and command passes, and rebuilds after device loss. Build-time
  WGSL and vertex reflection come from the Wasm `ns.shaders` custom section,
  so the browser never compiles Nano Script source. `view_create` returns the
  canvas-backed `ref view` passed to the unchanged typed
  `gpu_request_device(v: ref view)` API.
- **Linux/null.** Same no-op fallback contract as v1: every call safe,
  `gpu_request_device` returns false, `gpu_caps()` returns 0.

## Migration plan

Phase 0 — carve the seam. **Landed.** v2 entry points live beside v1 in
`gpu.h` / `gpu_const.h` / `gpu.ns`; the portable core in `lib/src/gpu.c`
(compiled on every platform) owns virtual addressing with host-backed
memory on the null tier, `gpu_write`/`gpu_read`, the frame ring rotated by
`gpu_v2_frame_end()` from each backend's `gpu_commit`, the value-cached
state registry, and `gpu_caps()`. Backends will register `gpu_v2_ops` (the
seam struct in `gpu.h`) from `gpu_request_device`; until then resource
creation returns 0 and submission is a safe no-op. Covered headless by
`test/gpu_v2_test.ns`. No callers changed.

Phase 1 — Metal core. Memory (`gpu_malloc`/`gpu_write`/`gpu_frame_alloc`),
texture heap indices, `gpu_create_state` + PSO cache, root pointer,
`gpu_draw`/`gpu_draw_indexed`. Port `sample/ns/shader.ns` and
`test/gpu_pipeline_test.ns` to v2 as the acceptance bar.

Phase 2 — shader transpiler. `gpu_addr`/`gpu_load`/`gpu_store`,
heap-index texture sampling, `vertex_id`/`instance_id`/`thread_id`
builtins, root-struct reflection emission.

Phase 3 — consumers. Port `lib/src/ui.c` (draw-list rendering collapses
onto `gpu_frame_alloc` + one root struct) and `nscode/native`. Persistent
compute shaders replace `gpu_dispatch_compute_source` in `dispatch_gpu`.

Phase 4 — browser and DX12 tiers. **Browser core landed.** Wasm projects use
WebGPU for buffer/texture resources, render and compute submission, indirect
commands, root data, and pre-transpiled WGSL metadata. DX12 and the remaining
capability-dependent split-barrier work continue in this phase.

Phase 5 — deprecate v1. `gpu_mesh`/`gpu_binding`/`gpu_pipeline_layout*`
first become pure-ns compatibility wrappers over v2 in `lib/gpu.ns`, then
the C symbols and the per-shape FFI tail are deleted.

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
  Vulkan lands (v1 also caps at 4).
