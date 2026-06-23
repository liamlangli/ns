// DirectX 12 GPU backend (Windows).
//
// Selected automatically on Windows via NS_GPU_DX12 (see lib/include/gpu.h).
// This is a structured skeleton that mirrors the layout of the Metal backend
// (lib/src/gpu.metal.m): a single static device/swapchain state plus fixed-size
// resource pools where a handle's `.id` is the pool slot.
//
// Device, command queue, swapchain, descriptor heaps, command list and frame
// fence are wired up for real so a window can be cleared and presented. Resource
// creation, pipeline/shader compilation and draw submission are scaffolded with
// `// TODO` markers — enough structure to fill in incrementally. Only amd64 and
// aarch64 Windows targets are supported.
#include "gpu.h"

#ifdef NS_GPU_DX12

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

// Win32 surface accessors implemented in view.win.c.
extern void *view_win_hwnd(void);
extern int view_win_width(void);
extern int view_win_height(void);

typedef struct gpu_buffer_dx12 {
    ID3D12Resource *resource;
    int size;
    ns_bool in_use;
} gpu_buffer_dx12;

typedef struct gpu_texture_dx12 {
    ID3D12Resource *resource;
    int width, height, depth;
    gpu_pixel_format format;
    ns_bool in_use;
} gpu_texture_dx12;

typedef struct gpu_shader_dx12 {
    ID3DBlob *vertex_blob;
    ID3DBlob *fragment_blob;
    ns_bool in_use;
} gpu_shader_dx12;

typedef struct gpu_pipeline_dx12 {
    ID3D12PipelineState *pso;
    ID3D12RootSignature *root_signature;
    gpu_pipeline_reflection reflection;
    ns_bool in_use;
} gpu_pipeline_dx12;

typedef struct gpu_binding_dx12 {
    gpu_pipeline pipeline;
    ns_bool in_use;
} gpu_binding_dx12;

typedef struct gpu_mesh_dx12 {
    gpu_buffer buffers[GPU_VERTEX_BUFFER_COUNT];
    u32 buffer_offsets[GPU_VERTEX_BUFFER_COUNT];
    gpu_buffer index_buffer;
    u32 index_buffer_offset;
    gpu_index_type index_type;
    ns_bool in_use;
} gpu_mesh_dx12;

typedef struct gpu_render_pass_dx12 {
    int width, height;
    ns_bool screen;
    gpu_color clear_value;
    ns_bool in_use;
} gpu_render_pass_dx12;

typedef struct gpu_state_dx12 {
    ns_bool valid;

    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    IDXGISwapChain3 *swapchain;

    ID3D12DescriptorHeap *rtv_heap;
    ID3D12DescriptorHeap *dsv_heap;
    UINT rtv_descriptor_size;
    ID3D12Resource *render_targets[GPU_SWAP_BUFFER_COUNT];

    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;

    ID3D12Fence *fence;
    HANDLE fence_event;
    UINT64 fence_value;
    UINT frame_index;

    // Resource pools. Slot 0 is reserved as the invalid handle.
    gpu_buffer_dx12 buffers[GPU_RESOURCE_POOL_SIZE];
    gpu_texture_dx12 textures[GPU_RESOURCE_POOL_SIZE];
    gpu_shader_dx12 shaders[GPU_RESOURCE_POOL_SIZE];
    gpu_pipeline_dx12 pipelines[GPU_RESOURCE_POOL_SIZE];
    gpu_binding_dx12 bindings[GPU_RESOURCE_POOL_SIZE];
    gpu_mesh_dx12 meshes[GPU_RESOURCE_POOL_SIZE];
    gpu_render_pass_dx12 render_passes[GPU_RESOURCE_POOL_SIZE];
} gpu_state_dx12;

static gpu_state_dx12 _state;

// ---- pool allocation -------------------------------------------------------
// Linear scan over a pool for the first free slot. Slot 0 is reserved so a
// zeroed handle (.id == 0) always reads as "invalid".
#define GPU_DX12_ALLOC(pool)                              \
    ({                                                    \
        u32 _slot = 0;                                    \
        for (u32 _i = 1; _i < GPU_RESOURCE_POOL_SIZE; ++_i) { \
            if (!_state.pool[_i].in_use) { _state.pool[_i].in_use = true; _slot = _i; break; } \
        }                                                 \
        _slot;                                            \
    })

// ---- device / swapchain ----------------------------------------------------

static void gpu_dx12_wait_for_gpu(void) {
    const UINT64 target = ++_state.fence_value;
    ID3D12CommandQueue_Signal(_state.queue, _state.fence, target);
    if (ID3D12Fence_GetCompletedValue(_state.fence) < target) {
        ID3D12Fence_SetEventOnCompletion(_state.fence, target, _state.fence_event);
        WaitForSingleObject(_state.fence_event, INFINITE);
    }
}

ns_bool gpu_request_device(view *v) {
    ns_unused(v);
    if (_state.valid) return true;

    HWND hwnd = (HWND)view_win_hwnd();
    if (!hwnd) return false;

    const int width = view_win_width();
    const int height = view_win_height();

    IDXGIFactory4 *factory = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory))) {
        return false;
    }

    if (FAILED(D3D12CreateDevice((IUnknown *)NULL, D3D_FEATURE_LEVEL_11_0,
                                 &IID_ID3D12Device, (void **)&_state.device))) {
        IDXGIFactory4_Release(factory);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(ID3D12Device_CreateCommandQueue(_state.device, &queue_desc,
                                               &IID_ID3D12CommandQueue, (void **)&_state.queue))) {
        IDXGIFactory4_Release(factory);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.BufferCount = GPU_SWAP_BUFFER_COUNT;
    sc_desc.Width = (UINT)width;
    sc_desc.Height = (UINT)height;
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.SampleDesc.Count = 1;

    IDXGISwapChain1 *swapchain1 = NULL;
    if (FAILED(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)_state.queue, hwnd,
                                                    &sc_desc, NULL, NULL, &swapchain1))) {
        IDXGIFactory4_Release(factory);
        return false;
    }
    IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3, (void **)&_state.swapchain);
    IDXGISwapChain1_Release(swapchain1);
    IDXGIFactory4_Release(factory);

    _state.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(_state.swapchain);

    // RTV descriptor heap + a render target view per swapchain buffer.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = GPU_SWAP_BUFFER_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12Device_CreateDescriptorHeap(_state.device, &rtv_heap_desc,
                                      &IID_ID3D12DescriptorHeap, (void **)&_state.rtv_heap);
    _state.rtv_descriptor_size =
        ID3D12Device_GetDescriptorHandleIncrementSize(_state.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(_state.rtv_heap, &rtv_handle);
    for (UINT i = 0; i < GPU_SWAP_BUFFER_COUNT; ++i) {
        IDXGISwapChain3_GetBuffer(_state.swapchain, i, &IID_ID3D12Resource,
                                  (void **)&_state.render_targets[i]);
        ID3D12Device_CreateRenderTargetView(_state.device, _state.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += _state.rtv_descriptor_size;
    }

    ID3D12Device_CreateCommandAllocator(_state.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        &IID_ID3D12CommandAllocator, (void **)&_state.command_allocator);
    ID3D12Device_CreateCommandList(_state.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   _state.command_allocator, NULL,
                                   &IID_ID3D12GraphicsCommandList, (void **)&_state.command_list);
    ID3D12GraphicsCommandList_Close(_state.command_list);

    ID3D12Device_CreateFence(_state.device, 0, D3D12_FENCE_FLAG_NONE,
                             &IID_ID3D12Fence, (void **)&_state.fence);
    _state.fence_value = 0;
    _state.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    _state.valid = true;
    return true;
}

void gpu_destroy_device(void) {
    if (!_state.valid) return;
    gpu_dx12_wait_for_gpu();
    if (_state.fence_event) CloseHandle(_state.fence_event);
    for (UINT i = 0; i < GPU_SWAP_BUFFER_COUNT; ++i) {
        if (_state.render_targets[i]) ID3D12Resource_Release(_state.render_targets[i]);
    }
    if (_state.command_list) ID3D12GraphicsCommandList_Release(_state.command_list);
    if (_state.command_allocator) ID3D12CommandAllocator_Release(_state.command_allocator);
    if (_state.fence) ID3D12Fence_Release(_state.fence);
    if (_state.rtv_heap) ID3D12DescriptorHeap_Release(_state.rtv_heap);
    if (_state.dsv_heap) ID3D12DescriptorHeap_Release(_state.dsv_heap);
    if (_state.swapchain) IDXGISwapChain3_Release(_state.swapchain);
    if (_state.queue) ID3D12CommandQueue_Release(_state.queue);
    if (_state.device) ID3D12Device_Release(_state.device);
    memset(&_state, 0, sizeof(_state));
}

// ---- resources -------------------------------------------------------------

gpu_buffer gpu_create_buffer(gpu_buffer_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(buffers);
    if (slot == 0) return (gpu_buffer){0};
    gpu_buffer_dx12 *b = &_state.buffers[slot];
    b->size = desc ? desc->size : 0;
    // TODO: create an upload/default ID3D12Resource of `size` bytes and, when
    // desc->data is provided, map+memcpy the initial contents.
    return (gpu_buffer){slot};
}

gpu_texture gpu_create_texture(gpu_texture_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(textures);
    if (slot == 0) return (gpu_texture){0};
    gpu_texture_dx12 *t = &_state.textures[slot];
    if (desc) {
        t->width = desc->width;
        t->height = desc->height;
        t->depth = desc->depth;
        t->format = desc->format;
    }
    // TODO: create a committed ID3D12Resource (DXGI format mapped from
    // gpu_pixel_format) and upload desc->data via a staging buffer.
    return (gpu_texture){slot};
}

gpu_sampler gpu_create_sampler(gpu_sampler_desc *desc) {
    ns_unused(desc);
    // TODO: store a D3D12_SAMPLER_DESC in a sampler descriptor heap.
    return (gpu_sampler){0};
}

gpu_shader gpu_create_shader(gpu_shader_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(shaders);
    if (slot == 0) return (gpu_shader){0};
    // TODO: D3DCompile desc->vertex.source / desc->fragment.source (HLSL) into
    // the vertex_blob / fragment_blob with vs_5_0 / ps_5_0 targets.
    return (gpu_shader){slot};
}

gpu_pipeline gpu_create_pipeline(gpu_pipeline_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(pipelines);
    if (slot == 0) return (gpu_pipeline){0};
    if (desc) _state.pipelines[slot].reflection.layout = desc->layout;
    // TODO: build a root signature + D3D12_GRAPHICS_PIPELINE_STATE_DESC from the
    // shader blobs, vertex layout, blend/depth/stencil state and create the PSO.
    return (gpu_pipeline){slot};
}

gpu_binding gpu_create_binding(gpu_binding_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(bindings);
    if (slot == 0) return (gpu_binding){0};
    if (desc) _state.bindings[slot].pipeline = desc->pipeline;
    // TODO: stage buffer/texture/sampler descriptors into a descriptor table.
    return (gpu_binding){slot};
}

gpu_mesh gpu_create_mesh(gpu_mesh_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(meshes);
    if (slot == 0) return (gpu_mesh){0};
    gpu_mesh_dx12 *m = &_state.meshes[slot];
    if (desc) {
        for (int i = 0; i < GPU_VERTEX_BUFFER_COUNT; ++i) {
            m->buffers[i] = desc->buffers[i];
            m->buffer_offsets[i] = desc->buffer_offsets[i];
        }
        m->index_buffer = desc->index_buffer;
        m->index_buffer_offset = desc->index_buffer_offset;
        m->index_type = desc->index_type;
    }
    return (gpu_mesh){slot};
}

gpu_render_pass gpu_create_render_pass(gpu_render_pass_desc *desc) {
    u32 slot = GPU_DX12_ALLOC(render_passes);
    if (slot == 0) return (gpu_render_pass){0};
    gpu_render_pass_dx12 *p = &_state.render_passes[slot];
    if (desc) {
        p->width = desc->width;
        p->height = desc->height;
        p->screen = desc->screen;
        p->clear_value = desc->colors[0].clear_value;
    }
    return (gpu_render_pass){slot};
}

void gpu_destroy_texture(gpu_texture texture) {
    if (texture.id == 0 || texture.id >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_texture_dx12 *t = &_state.textures[texture.id];
    if (t->resource) ID3D12Resource_Release(t->resource);
    memset(t, 0, sizeof(*t));
}

void gpu_destroy_sampler(gpu_sampler sampler) { ns_unused(sampler); }

void gpu_destroy_buffer(gpu_buffer buffer) {
    if (buffer.id == 0 || buffer.id >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_buffer_dx12 *b = &_state.buffers[buffer.id];
    if (b->resource) ID3D12Resource_Release(b->resource);
    memset(b, 0, sizeof(*b));
}

void gpu_destroy_shader(gpu_shader shader) {
    if (shader.id == 0 || shader.id >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_shader_dx12 *s = &_state.shaders[shader.id];
    if (s->vertex_blob) ID3D10Blob_Release(s->vertex_blob);
    if (s->fragment_blob) ID3D10Blob_Release(s->fragment_blob);
    memset(s, 0, sizeof(*s));
}

void gpu_destroy_pipeline(gpu_pipeline pipeline) {
    if (pipeline.id == 0 || pipeline.id >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_pipeline_dx12 *p = &_state.pipelines[pipeline.id];
    if (p->pso) ID3D12PipelineState_Release(p->pso);
    if (p->root_signature) ID3D12RootSignature_Release(p->root_signature);
    memset(p, 0, sizeof(*p));
}

void gpu_destroy_binding(gpu_binding binding) {
    if (binding.id == 0 || binding.id >= GPU_RESOURCE_POOL_SIZE) return;
    memset(&_state.bindings[binding.id], 0, sizeof(gpu_binding_dx12));
}

void gpu_destroy_mesh(gpu_mesh mesh) {
    if (mesh.id == 0 || mesh.id >= GPU_RESOURCE_POOL_SIZE) return;
    memset(&_state.meshes[mesh.id], 0, sizeof(gpu_mesh_dx12));
}

void gpu_destroy_render_pass(gpu_render_pass pass) {
    if (pass.id == 0 || pass.id >= GPU_RESOURCE_POOL_SIZE) return;
    memset(&_state.render_passes[pass.id], 0, sizeof(gpu_render_pass_dx12));
}

gpu_pipeline_reflection gpu_pipeline_get_reflection(gpu_pipeline pipeline) {
    if (pipeline.id == 0 || pipeline.id >= GPU_RESOURCE_POOL_SIZE) {
        return (gpu_pipeline_reflection){0};
    }
    return _state.pipelines[pipeline.id].reflection;
}

void gpu_update_texture(gpu_texture texture, ns_data data) {
    ns_unused(texture);
    ns_unused(data);
    // TODO: upload `data` into the texture resource via a staging buffer.
}

void gpu_update_buffer(gpu_buffer buffer, ns_data data) {
    ns_unused(buffer);
    ns_unused(data);
    // TODO: map the buffer resource and memcpy `data`.
}

// ---- frame submission ------------------------------------------------------

void gpu_begin_render_pass(gpu_render_pass pass) {
    if (!_state.valid) return;
    gpu_render_pass_dx12 *p =
        (pass.id && pass.id < GPU_RESOURCE_POOL_SIZE) ? &_state.render_passes[pass.id] : NULL;

    _state.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(_state.swapchain);
    ID3D12CommandAllocator_Reset(_state.command_allocator);
    ID3D12GraphicsCommandList_Reset(_state.command_list, _state.command_allocator, NULL);

    // Transition the current back buffer PRESENT -> RENDER_TARGET.
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = _state.render_targets[_state.frame_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(_state.command_list, 1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(_state.rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)_state.frame_index * _state.rtv_descriptor_size;
    ID3D12GraphicsCommandList_OMSetRenderTargets(_state.command_list, 1, &rtv, FALSE, NULL);

    const float clear[4] = {
        p ? p->clear_value.r : 0.0f,
        p ? p->clear_value.g : 0.0f,
        p ? p->clear_value.b : 0.0f,
        p ? p->clear_value.a : 1.0f,
    };
    ID3D12GraphicsCommandList_ClearRenderTargetView(_state.command_list, rtv, clear, 0, NULL);
}

void gpu_set_viewport(int x, int y, int width, int height) {
    if (!_state.valid) return;
    D3D12_VIEWPORT vp = {(FLOAT)x, (FLOAT)y, (FLOAT)width, (FLOAT)height, 0.0f, 1.0f};
    ID3D12GraphicsCommandList_RSSetViewports(_state.command_list, 1, &vp);
}

void gpu_set_scissor(int x, int y, int width, int height) {
    if (!_state.valid) return;
    D3D12_RECT rect = {x, y, x + width, y + height};
    ID3D12GraphicsCommandList_RSSetScissorRects(_state.command_list, 1, &rect);
}

void gpu_set_pipeline(gpu_pipeline pipeline) {
    ns_unused(pipeline);
    // TODO: SetPipelineState + SetGraphicsRootSignature from the pool entry.
}

void gpu_set_binding(gpu_binding binding) {
    ns_unused(binding);
    // TODO: bind descriptor tables / root constants for this binding group.
}

void gpu_set_mesh(gpu_mesh mesh) {
    ns_unused(mesh);
    // TODO: IASetVertexBuffers / IASetIndexBuffer from the mesh's buffers.
}

void gpu_draw(int base, int count, int instance_count) {
    ns_unused(base);
    ns_unused(count);
    ns_unused(instance_count);
    // TODO: DrawInstanced / DrawIndexedInstanced once pipeline + mesh are bound.
}

void gpu_end_pass(void) {
    if (!_state.valid) return;
    // Transition the back buffer RENDER_TARGET -> PRESENT and close the list.
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = _state.render_targets[_state.frame_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(_state.command_list, 1, &barrier);
    ID3D12GraphicsCommandList_Close(_state.command_list);
}

void gpu_commit(void) {
    if (!_state.valid) return;
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)_state.command_list};
    ID3D12CommandQueue_ExecuteCommandLists(_state.queue, 1, lists);
    IDXGISwapChain3_Present(_state.swapchain, 1, 0);
    gpu_dx12_wait_for_gpu();
}

// ---- pixel format helpers (backend agnostic) -------------------------------

int gpu_pixel_format_size(gpu_pixel_format format) {
    ns_unused(format);
    return 4; // TODO: full gpu_pixel_format -> byte-size table.
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
    return gpu_pixel_format_row_pitch(format, width, row_alignment) *
           gpu_pixel_format_row_count(format, height);
}

#endif // NS_GPU_DX12
