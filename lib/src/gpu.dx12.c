// DirectX 12 GPU v2 backend (Windows).
//
// The Windows renderer uses the same address/root/state/pass seam as Metal.
// Shader/texture heap support is still incremental work, but the backend API
// is exclusively memory/root/state/pass based.
#include "gpu.h"

#ifdef NS_GPU_DX12

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdlib.h>
#include <string.h>

extern void *view_win_hwnd(void);
extern int view_win_width(void);
extern int view_win_height(void);

typedef struct gpu_dx12_memory {
    ID3D12Resource *resource;
    void *mapped;
    u64 size;
} gpu_dx12_memory;

typedef struct gpu_state_dx12 {
    ns_bool valid;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    IDXGISwapChain3 *swapchain;
    ID3D12DescriptorHeap *rtv_heap;
    UINT rtv_descriptor_size;
    ID3D12Resource *render_targets[GPU_SWAP_BUFFER_COUNT];
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    ID3D12Fence *fence;
    HANDLE fence_event;
    UINT64 fence_value;
    UINT frame_index;
    ns_bool pass_open;
    gpu_dx12_memory memory[GPU_RESOURCE_POOL_SIZE];
} gpu_state_dx12;

static gpu_state_dx12 _state;
static const gpu_v2_ops _dx12_v2_ops;

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
    IDXGIFactory4 *factory = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory))) return false;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                                 &IID_ID3D12Device, (void **)&_state.device))) {
        IDXGIFactory4_Release(factory);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(_state.device, &queue_desc,
                                               &IID_ID3D12CommandQueue, (void **)&_state.queue))) {
        IDXGIFactory4_Release(factory);
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 swap_desc = {0};
    swap_desc.BufferCount = GPU_SWAP_BUFFER_COUNT;
    swap_desc.Width = (UINT)view_win_width();
    swap_desc.Height = (UINT)view_win_height();
    swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.SampleDesc.Count = 1;
    IDXGISwapChain1 *swapchain = NULL;
    if (FAILED(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)_state.queue, hwnd,
                                                    &swap_desc, NULL, NULL, &swapchain))) {
        IDXGIFactory4_Release(factory);
        return false;
    }
    IDXGISwapChain1_QueryInterface(swapchain, &IID_IDXGISwapChain3, (void **)&_state.swapchain);
    IDXGISwapChain1_Release(swapchain);
    IDXGIFactory4_Release(factory);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {0};
    heap_desc.NumDescriptors = GPU_SWAP_BUFFER_COUNT;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(_state.device, &heap_desc,
                                                  &IID_ID3D12DescriptorHeap, (void **)&_state.rtv_heap))) return false;
    _state.rtv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        _state.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(_state.rtv_heap, &rtv);
    for (UINT i = 0; i < GPU_SWAP_BUFFER_COUNT; ++i) {
        IDXGISwapChain3_GetBuffer(_state.swapchain, i, &IID_ID3D12Resource,
                                  (void **)&_state.render_targets[i]);
        ID3D12Device_CreateRenderTargetView(_state.device, _state.render_targets[i], NULL, rtv);
        rtv.ptr += _state.rtv_descriptor_size;
    }
    ID3D12Device_CreateCommandAllocator(_state.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        &IID_ID3D12CommandAllocator, (void **)&_state.command_allocator);
    ID3D12Device_CreateCommandList(_state.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   _state.command_allocator, NULL,
                                   &IID_ID3D12GraphicsCommandList, (void **)&_state.command_list);
    ID3D12GraphicsCommandList_Close(_state.command_list);
    ID3D12Device_CreateFence(_state.device, 0, D3D12_FENCE_FLAG_NONE,
                             &IID_ID3D12Fence, (void **)&_state.fence);
    _state.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    _state.valid = true;
    // Shader/root binding is not implemented by this incremental backend yet.
    gpu_v2_set_backend(&_dx12_v2_ops, 0, 0);
    return true;
}

void gpu_destroy_device(void) {
    if (!_state.valid) return;
    gpu_v2_set_backend(NULL, 0, 0);
    gpu_dx12_wait_for_gpu();
    for (u32 i = 0; i < GPU_RESOURCE_POOL_SIZE; ++i) {
        if (_state.memory[i].resource) {
            if (_state.memory[i].mapped) ID3D12Resource_Unmap(_state.memory[i].resource, 0, NULL);
            ID3D12Resource_Release(_state.memory[i].resource);
        }
    }
    if (_state.fence_event) CloseHandle(_state.fence_event);
    for (UINT i = 0; i < GPU_SWAP_BUFFER_COUNT; ++i)
        if (_state.render_targets[i]) ID3D12Resource_Release(_state.render_targets[i]);
    if (_state.command_list) ID3D12GraphicsCommandList_Release(_state.command_list);
    if (_state.command_allocator) ID3D12CommandAllocator_Release(_state.command_allocator);
    if (_state.fence) ID3D12Fence_Release(_state.fence);
    if (_state.rtv_heap) ID3D12DescriptorHeap_Release(_state.rtv_heap);
    if (_state.swapchain) IDXGISwapChain3_Release(_state.swapchain);
    if (_state.queue) ID3D12CommandQueue_Release(_state.queue);
    if (_state.device) ID3D12Device_Release(_state.device);
    memset(&_state, 0, sizeof(_state));
}

static void dx12_resource_set_name(ID3D12Resource *resource, const char *name) {
    if (!resource || !name || !name[0]) return;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
    if (length <= 0) return;
    WCHAR *wide_name = (WCHAR *)malloc((size_t)length * sizeof(*wide_name));
    if (!wide_name) return;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide_name, length) > 0) {
        ID3D12Resource_SetName(resource, wide_name);
    }
    free(wide_name);
}

static ns_bool dx12_mem_create(u32 slot, u64 size, u32 flags, const char *name, u64 *base_va) {
    ns_unused(flags);
    if (slot >= GPU_RESOURCE_POOL_SIZE || size == 0 || !name || !name[0]) return false;
    D3D12_HEAP_PROPERTIES heap = {0};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {0};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    gpu_dx12_memory *memory = &_state.memory[slot];
    if (FAILED(ID3D12Device_CreateCommittedResource(_state.device, &heap, D3D12_HEAP_FLAG_NONE,
                                                    &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                    &IID_ID3D12Resource, (void **)&memory->resource))) return false;
    dx12_resource_set_name(memory->resource, name);
    if (FAILED(ID3D12Resource_Map(memory->resource, 0, NULL, &memory->mapped))) {
        ID3D12Resource_Release(memory->resource);
        memset(memory, 0, sizeof(*memory));
        return false;
    }
    memory->size = size;
    if (base_va) *base_va = 0;
    return true;
}

static void dx12_mem_destroy(u32 slot) {
    if (slot >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_dx12_memory *memory = &_state.memory[slot];
    if (memory->resource) {
        if (memory->mapped) ID3D12Resource_Unmap(memory->resource, 0, NULL);
        ID3D12Resource_Release(memory->resource);
    }
    memset(memory, 0, sizeof(*memory));
}

static void dx12_mem_write(u32 slot, u64 offset, const void *src, u64 size) {
    gpu_dx12_memory *memory = slot < GPU_RESOURCE_POOL_SIZE ? &_state.memory[slot] : NULL;
    if (!memory || !memory->mapped || !src || offset > memory->size || size > memory->size - offset) return;
    memcpy((u8 *)memory->mapped + offset, src, (size_t)size);
}

static ns_bool dx12_mem_read(u32 slot, u64 offset, void *dst, u64 size) {
    gpu_dx12_memory *memory = slot < GPU_RESOURCE_POOL_SIZE ? &_state.memory[slot] : NULL;
    if (!memory || !memory->mapped || !dst || offset > memory->size || size > memory->size - offset) return false;
    memcpy(dst, (u8 *)memory->mapped + offset, (size_t)size);
    return true;
}

static void *dx12_mem_host_ptr(u32 slot) {
    return slot < GPU_RESOURCE_POOL_SIZE ? _state.memory[slot].mapped : NULL;
}

static void dx12_screen_pass_begin(gpu_color clear) {
    if (!_state.valid || _state.pass_open) return;
    _state.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(_state.swapchain);
    ID3D12CommandAllocator_Reset(_state.command_allocator);
    ID3D12GraphicsCommandList_Reset(_state.command_list, _state.command_allocator, NULL);
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
    const float color[4] = {clear.r, clear.g, clear.b, clear.a};
    ID3D12GraphicsCommandList_ClearRenderTargetView(_state.command_list, rtv, color, 0, NULL);
    _state.pass_open = true;
}

static void dx12_pass_end(void) {
    if (!_state.pass_open) return;
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = _state.render_targets[_state.frame_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(_state.command_list, 1, &barrier);
    ID3D12GraphicsCommandList_Close(_state.command_list);
    _state.pass_open = false;
}

void gpu_set_viewport(int x, int y, int width, int height) {
    if (!_state.valid || !_state.pass_open) return;
    D3D12_VIEWPORT viewport = {(FLOAT)x, (FLOAT)y, (FLOAT)width, (FLOAT)height, 0.0f, 1.0f};
    ID3D12GraphicsCommandList_RSSetViewports(_state.command_list, 1, &viewport);
}

void gpu_set_scissor(int x, int y, int width, int height) {
    if (!_state.valid || !_state.pass_open) return;
    D3D12_RECT rect = {x, y, x + width, y + height};
    ID3D12GraphicsCommandList_RSSetScissorRects(_state.command_list, 1, &rect);
}

void gpu_commit(void) {
    if (!_state.valid) {
        gpu_v2_frame_end();
        return;
    }
    if (_state.pass_open) dx12_pass_end();
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)_state.command_list};
    ID3D12CommandQueue_ExecuteCommandLists(_state.queue, 1, lists);
    IDXGISwapChain3_Present(_state.swapchain, 1, 0);
    gpu_dx12_wait_for_gpu();
    gpu_v2_frame_end();
}

static const gpu_v2_ops _dx12_v2_ops = {
    .mem_create = dx12_mem_create,
    .mem_destroy = dx12_mem_destroy,
    .mem_write = dx12_mem_write,
    .mem_read = dx12_mem_read,
    .mem_host_ptr = dx12_mem_host_ptr,
    .screen_pass_begin = dx12_screen_pass_begin,
    .pass_end = dx12_pass_end,
};

#endif // NS_GPU_DX12
