#include <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <AvailabilityMacros.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include <objc/objc.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#import <dispatch/semaphore.h>
#import <QuartzCore/CoreAnimation.h> // needed for CAMetalDrawable

#include "gpu.h"

// Metal exposes 31 buffer argument indices (0...30) per shader stage. Nano
// Script reserves 0...2 for root/uniform plumbing, leaving the rest as
// shader_buffer_* storage slots.
enum {
    GPU_MTL_STORAGE_BINDING_BASE = 3,
    GPU_MTL_BUFFER_BINDING_COUNT = 31,
    GPU_MTL_STORAGE_SLOT_COUNT = GPU_MTL_BUFFER_BINDING_COUNT - GPU_MTL_STORAGE_BINDING_BASE,
};

static MTLTextureType _mtl_texture_type(gpu_texture_type type) {
    switch (type) {
        case TEXTURE_2D: return MTLTextureType2D;
        case TEXTURE_CUBE: return MTLTextureTypeCube;
        case TEXTURE_3D: return MTLTextureType3D;
        case TEXTURE_ARRAY: return MTLTextureType2DArray;
    }
    return MTLTextureType2D;
}

static MTLPixelFormat _mtl_pixel_format(gpu_pixel_format format) {
#if TARGET_OS_IOS
    if (@available(iOS 16.4, *)) {
        switch (format) {
            case PIXELFORMAT_BC1_RGBA: return MTLPixelFormatBC1_RGBA;
            case PIXELFORMAT_BC2_RGBA: return MTLPixelFormatBC2_RGBA;
            case PIXELFORMAT_BC3_RGBA: return MTLPixelFormatBC3_RGBA;
            case PIXELFORMAT_BC3_SRGBA: return MTLPixelFormatBC3_RGBA_sRGB;
            case PIXELFORMAT_BC4_R: return MTLPixelFormatBC4_RUnorm;
            case PIXELFORMAT_BC4_RSN: return MTLPixelFormatBC4_RSnorm;
            case PIXELFORMAT_BC5_RG: return MTLPixelFormatBC5_RGUnorm;
            case PIXELFORMAT_BC5_RGSN: return MTLPixelFormatBC5_RGSnorm;
            case PIXELFORMAT_BC6H_RGBF: return MTLPixelFormatBC6H_RGBFloat;
            case PIXELFORMAT_BC6H_RGBUF: return MTLPixelFormatBC6H_RGBUfloat;
            case PIXELFORMAT_BC7_RGBA: return MTLPixelFormatBC7_RGBAUnorm;
            case PIXELFORMAT_BC7_SRGBA: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
            default: break;
        }
    }
#else
    switch (format) {
        case PIXELFORMAT_BC1_RGBA: return MTLPixelFormatBC1_RGBA;
        case PIXELFORMAT_BC2_RGBA: return MTLPixelFormatBC2_RGBA;
        case PIXELFORMAT_BC3_RGBA: return MTLPixelFormatBC3_RGBA;
        case PIXELFORMAT_BC3_SRGBA: return MTLPixelFormatBC3_RGBA_sRGB;
        case PIXELFORMAT_BC4_R: return MTLPixelFormatBC4_RUnorm;
        case PIXELFORMAT_BC4_RSN: return MTLPixelFormatBC4_RSnorm;
        case PIXELFORMAT_BC5_RG: return MTLPixelFormatBC5_RGUnorm;
        case PIXELFORMAT_BC5_RGSN: return MTLPixelFormatBC5_RGSnorm;
        case PIXELFORMAT_BC6H_RGBF: return MTLPixelFormatBC6H_RGBFloat;
        case PIXELFORMAT_BC6H_RGBUF: return MTLPixelFormatBC6H_RGBUfloat;
        case PIXELFORMAT_BC7_RGBA: return MTLPixelFormatBC7_RGBAUnorm;
        case PIXELFORMAT_BC7_SRGBA: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        default: break;
    }
#endif
    switch (format) {
        case PIXELFORMAT_R8: return MTLPixelFormatR8Unorm;
        case PIXELFORMAT_R8SN: return MTLPixelFormatR8Snorm;
        case PIXELFORMAT_R8UI: return MTLPixelFormatR8Uint;
        case PIXELFORMAT_R8SI: return MTLPixelFormatR8Sint;
        case PIXELFORMAT_R16: return MTLPixelFormatR16Unorm;
        case PIXELFORMAT_R16SN: return MTLPixelFormatR16Snorm;
        case PIXELFORMAT_R16UI: return MTLPixelFormatR16Uint;
        case PIXELFORMAT_R16SI: return MTLPixelFormatR16Sint;
        case PIXELFORMAT_R16F: return MTLPixelFormatR16Float;
        case PIXELFORMAT_RG8: return MTLPixelFormatRG8Unorm;
        case PIXELFORMAT_RG8SN: return MTLPixelFormatRG8Snorm;
        case PIXELFORMAT_RG8UI: return MTLPixelFormatRG8Uint;
        case PIXELFORMAT_RG8SI: return MTLPixelFormatRG8Sint;
        case PIXELFORMAT_R32UI: return MTLPixelFormatR32Uint;
        case PIXELFORMAT_R32SI: return MTLPixelFormatR32Sint;
        case PIXELFORMAT_R32F: return MTLPixelFormatR32Float;
        case PIXELFORMAT_RG16: return MTLPixelFormatRG16Unorm;
        case PIXELFORMAT_RG16SN: return MTLPixelFormatRG16Snorm;
        case PIXELFORMAT_RG16UI: return MTLPixelFormatRG16Uint;
        case PIXELFORMAT_RG16SI: return MTLPixelFormatRG16Sint;
        case PIXELFORMAT_RG16F: return MTLPixelFormatRG16Float;
        case PIXELFORMAT_RGBA8: return MTLPixelFormatRGBA8Unorm;
        case PIXELFORMAT_SRGB8A8: return MTLPixelFormatRGBA8Unorm_sRGB;
        case PIXELFORMAT_RGBA8SN: return MTLPixelFormatRGBA8Snorm;
        case PIXELFORMAT_RGBA8UI: return MTLPixelFormatRGBA8Uint;
        case PIXELFORMAT_RGBA8SI: return MTLPixelFormatRGBA8Sint;
        case PIXELFORMAT_BGRA8: return MTLPixelFormatBGRA8Unorm;
        case PIXELFORMAT_RGB10A2: return MTLPixelFormatRGB10A2Unorm;
        case PIXELFORMAT_RG11B10F: return MTLPixelFormatRG11B10Float;
        case PIXELFORMAT_RGB9E5: return MTLPixelFormatRGB9E5Float;
        case PIXELFORMAT_RG32UI: return MTLPixelFormatRG32Uint;
        case PIXELFORMAT_RG32SI: return MTLPixelFormatRG32Sint;
        case PIXELFORMAT_RG32F: return MTLPixelFormatRG32Float;
        case PIXELFORMAT_RGBA16: return MTLPixelFormatRGBA16Unorm;
        case PIXELFORMAT_RGBA16SN: return MTLPixelFormatRGBA16Snorm;
        case PIXELFORMAT_RGBA16UI: return MTLPixelFormatRGBA16Uint;
        case PIXELFORMAT_RGBA16SI: return MTLPixelFormatRGBA16Sint;
        case PIXELFORMAT_RGBA16F: return MTLPixelFormatRGBA16Float;
        case PIXELFORMAT_RGBA32UI: return MTLPixelFormatRGBA32Uint;
        case PIXELFORMAT_RGBA32SI: return MTLPixelFormatRGBA32Sint;
        case PIXELFORMAT_RGBA32F: return MTLPixelFormatRGBA32Float;
        case PIXELFORMAT_DEPTH: return MTLPixelFormatDepth32Float;
        case PIXELFORMAT_DEPTH_STENCIL: return MTLPixelFormatDepth32Float_Stencil8;
        case _PIXELFORMAT_DEFAULT:
        case PIXELFORMAT_NONE:
        case PIXELFORMAT_BC1_RGBA:
        case PIXELFORMAT_BC2_RGBA:
        case PIXELFORMAT_BC3_RGBA:
        case PIXELFORMAT_BC3_SRGBA:
        case PIXELFORMAT_BC4_R:
        case PIXELFORMAT_BC4_RSN:
        case PIXELFORMAT_BC5_RG:
        case PIXELFORMAT_BC5_RGSN:
        case PIXELFORMAT_BC6H_RGBF:
        case PIXELFORMAT_BC6H_RGBUF:
        case PIXELFORMAT_BC7_RGBA:
        case PIXELFORMAT_BC7_SRGBA:
        case PIXELFORMAT_PVRTC_RGB_2BPP:
        case PIXELFORMAT_PVRTC_RGB_4BPP:
        case PIXELFORMAT_PVRTC_RGBA_2BPP:
        case PIXELFORMAT_PVRTC_RGBA_4BPP:
        case PIXELFORMAT_ETC2_RGB8:
        case PIXELFORMAT_ETC2_SRGB8:
        case PIXELFORMAT_ETC2_RGB8A1:
        case PIXELFORMAT_ETC2_RGBA8:
        case PIXELFORMAT_ETC2_SRGB8A8:
        case PIXELFORMAT_ETC2_RG11:
        case PIXELFORMAT_ETC2_RG11SN:
        case PIXELFORMAT_ASTC_4x4_RGBA:
        case PIXELFORMAT_ASTC_4x4_SRGBA:
        case _PIXELFORMAT_NUM:
            return MTLPixelFormatInvalid;
    }
    return MTLPixelFormatInvalid;
}

static MTLLoadAction _mtl_load_action(gpu_load_action action) {
    switch (action) {
        case LOAD_ACTION_CLEAR: return MTLLoadActionClear;
        case LOAD_ACTION_LOAD: return MTLLoadActionLoad;
        case LOAD_ACTION_DONTCARE: return MTLLoadActionDontCare;
    }
    return MTLLoadActionDontCare;
}

static MTLCompareFunction _mtl_compare_function(gpu_compare_func func) {
    switch (func) {
        case COMPARE_NEVER: return MTLCompareFunctionNever;
        case COMPARE_LESS: return MTLCompareFunctionLess;
        case COMPARE_EQUAL: return MTLCompareFunctionEqual;
        case COMPARE_LESS_EQUAL: return MTLCompareFunctionLessEqual;
        case COMPARE_GREATER: return MTLCompareFunctionGreater;
        case COMPARE_NOT_EQUAL: return MTLCompareFunctionNotEqual;
        case COMPARE_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
        case COMPARE_ALWAYS:
        case COMPARE_AUTO:
            return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

static MTLCullMode _mtl_cull_mode(gpu_cull_mode mode) {
    switch (mode) {
        case CULL_NONE: return MTLCullModeNone;
        case CULL_FRONT: return MTLCullModeFront;
        case CULL_BACK: return MTLCullModeBack;
    }
    return MTLCullModeNone;
}

static MTLWinding _mtl_winding(gpu_face_winding winding) {
    switch (winding) {
        case FACE_WINDING_CCW: return MTLWindingCounterClockwise;
        case FACE_WINDING_CW: return MTLWindingClockwise;
    }
    return MTLWindingCounterClockwise;
}

static MTLPrimitiveType _mtl_primitive_type(gpu_primitive_type type) {
    switch (type) {
        case PRIMITIVE_POINTS: return MTLPrimitiveTypePoint;
        case PRIMITIVE_LINES: return MTLPrimitiveTypeLine;
        case PRIMITIVE_LINE_STRIP:
        case PRIMITIVE_LINE_LOOP:
            return MTLPrimitiveTypeLineStrip;
        case PRIMITIVE_TRIANGLES: return MTLPrimitiveTypeTriangle;
        case PRIMITIVE_TRIANGLE_STRIP:
        case PRIMITIVE_TRIANGLE_FAN:
            return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

static MTLIndexType _mtl_index_type(gpu_index_type type) {
    switch (type) {
        case INDEX_UINT16: return MTLIndexTypeUInt16;
        case INDEX_UINT32: return MTLIndexTypeUInt32;
        case INDEX_NONE: return MTLIndexTypeUInt16;
    }
    return MTLIndexTypeUInt16;
}

typedef struct gpu_shader_mtl {
    id<MTLLibrary> vertex_lib;
    id<MTLLibrary> fragment_lib;
    id<MTLFunction> vertex_func;
    id<MTLFunction> fragment_func;
    id<MTLLibrary> compute_lib;
    id<MTLFunction> compute_func;
    id<MTLComputePipelineState> compute_pso;
    id<MTLRenderPipelineState> v2_pso;
    id<MTLDepthStencilState> v2_dso;
    gpu_v2_state_desc v2_state;
    MTLPixelFormat v2_colors[4];
    MTLPixelFormat v2_depth;
    bool v2_pipeline_valid;
    bool uses_root;
    bool uses_read_texture;
    bool uses_write_texture;
    bool uses_write_texture_secondary;
    bool uses_texture_map;
    bool uses_mask_map;
    bool uses_shadow_map;
} gpu_shader_mtl;

typedef struct gpu_texture_mtl {
    id<MTLTexture> texture;
    NSUInteger width, height, depth;
    gpu_pixel_format format;
    gpu_texture_type type;
    MTLResourceOptions resource_options;
} gpu_texture_mtl;

typedef struct gpu_sampler_mtl {
    id<MTLSamplerState> sampler;
    gpu_filter min_filter, mag_filter, mip_filter;
    gpu_wrap wrap_u, wrap_v, wrap_w;
} gpu_sampler_mtl;

typedef struct gpu_swapchain_mtl {
    int width, height, sample_count;
    gpu_pixel_format color_format;
    gpu_pixel_format depth_stencil_format;
    id<MTLTexture> color_texture;
    id<MTLTexture> depth_stencil_texture;
} gpu_swapchain_mtl;

typedef struct gpu_device_mtl {
    id<MTLDevice> device;
} gpu_device_mtl;

typedef struct gpu_state_mtl {
    bool valid;
    int frame_index;
    gpu_device_mtl device;
    view *owner_view;
    MTKView *view;
    dispatch_semaphore_t semaphore;

    gpu_swapchain_mtl swapchain;

    id<MTLCommandQueue> cmd_queue;
    id<MTLCommandBuffer> cmd_buffer;
    id<MTLRenderCommandEncoder> cmd_encoder;
    id<CAMetalDrawable> cur_drawable;
    id<MTLCaptureScope> capture_scope;
    
    gpu_shader_mtl shaders[GPU_RESOURCE_POOL_SIZE];
    gpu_texture_mtl textures[GPU_RESOURCE_POOL_SIZE];
    gpu_sampler_mtl samplers[GPU_RESOURCE_POOL_SIZE];
    
    u32 shader_count;
    u32 texture_count;
    u32 sampler_count;
    u32 screen_pass_count;
    id<MTLBuffer> v2_memory[GPU_RESOURCE_POOL_SIZE];
    u64 v2_memory_size[GPU_RESOURCE_POOL_SIZE];
    u32 v2_shader;
    gpu_v2_state_desc v2_render_state;
    id<MTLBuffer> v2_root_buffer;
    u64 v2_root_offset;
    id<MTLBuffer> *v2_storage_buffers;
    u64 *v2_storage_offsets;
    u32 v2_storage_slot_count;
    MTLPixelFormat v2_pass_colors[4];
    MTLPixelFormat v2_pass_depth;

} gpu_state_mtl;

static gpu_state_mtl _state = {0};
static const gpu_v2_ops _mtl_v2_ops;

static NSURL *gpu_mtl_capture_output_url(void) {
    NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    [formatter setDateFormat: @"yyyyMMdd-HHmmss"];
    NSString *stamp = [formatter stringFromDate: [NSDate date]];
    NSString *name = [NSString stringWithFormat: @"metal-frame-%@-%04d.gputrace", stamp, _state.frame_index];
    NSString *path = [cwd stringByAppendingPathComponent: name];
#ifndef ENABLE_ARC
    [formatter release];
#endif
    return [NSURL fileURLWithPath: path];
}

static BOOL gpu_mtl_capture_start(MTLCaptureDestination destination, NSURL *output_url, NSError **error) {
    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    MTLCaptureDescriptor *desc = [MTLCaptureDescriptor new];
    desc.captureObject = _state.capture_scope != nil ? (id)_state.capture_scope : (id)_state.cmd_queue;
    desc.destination = destination;
    if (destination == MTLCaptureDestinationGPUTraceDocument) {
        desc.outputURL = output_url;
    }

    BOOL ok = [mgr startCaptureWithDescriptor: desc error: error];
#ifndef ENABLE_ARC
    [desc release];
#endif
    return ok;
}

static void gpu_mtl_capture_begin_if_requested(void) {
    view *owner = _state.owner_view;
    if (!owner || !owner->capture_required || owner->capture_started) {
        return;
    }

    owner->capture_required = false;

    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    if ([mgr isCapturing]) {
        NSLog(@"Metal GPU capture skipped: another capture is already running");
        return;
    }

    if (_state.capture_scope == nil && _state.cmd_queue != nil) {
        _state.capture_scope = [mgr newCaptureScopeWithCommandQueue: _state.cmd_queue];
        [_state.capture_scope setLabel: @"ns frame"];
    }

    NSURL *output_url = gpu_mtl_capture_output_url();
    BOOL file_supported = [mgr supportsDestination: MTLCaptureDestinationGPUTraceDocument];
    if (!file_supported) {
        NSLog(@"Metal GPU capture file destination reports unsupported; trying anyway: %@", [output_url path]);
    }

    NSError *error = nil;
    BOOL ok = gpu_mtl_capture_start(MTLCaptureDestinationGPUTraceDocument, output_url, &error);
    if (ok && !error) {
        if (_state.capture_scope != nil) [_state.capture_scope beginScope];
        owner->capture_started = true;
        NSLog(@"Metal GPU capture started: %@", [output_url path]);
        return;
    }

    NSLog(@"Metal GPU trace document capture failed: %@", error);
    if (![mgr supportsDestination: MTLCaptureDestinationDeveloperTools]) {
        NSLog(@"Metal GPU capture failed: no supported fallback destination");
        return;
    }

    error = nil;
    ok = gpu_mtl_capture_start(MTLCaptureDestinationDeveloperTools, nil, &error);
    if (!ok || error) {
        NSLog(@"Metal GPU Developer Tools capture failed: %@", error);
        return;
    }

    if (_state.capture_scope != nil) [_state.capture_scope beginScope];
    owner->capture_started = true;
    NSLog(@"Metal GPU capture started in Developer Tools; this runtime cannot save a .gputrace file directly");
}

static void gpu_mtl_capture_end_if_started(void) {
    view *owner = _state.owner_view;
    if (!owner || !owner->capture_started) {
        return;
    }

    MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
    if (_state.capture_scope != nil) {
        [_state.capture_scope endScope];
    }
    if ([mgr isCapturing]) {
        [mgr stopCapture];
    }
    owner->capture_started = false;
    NSLog(@"Metal GPU capture saved");
}

ns_bool gpu_request_device(view* v) {
    if (_state.valid && _state.device.device != nil && _state.owner_view == v) return true;
    _state.owner_view = v;
    _state.semaphore = dispatch_semaphore_create(1);
    if (v != NULL && v->gpu_device != NULL) {
        _state.device.device = (__bridge id<MTLDevice>)v->gpu_device;
    } else {
        _state.device.device = MTLCreateSystemDefaultDevice();
    }

    if (nil == _state.device.device) {
        _state.valid = false;
        return false;
    }

    if (v != NULL && v->native_window != NULL) {
#if TARGET_OS_OSX
        // Hosted apps run the Nano Script VM on a worker so view_run() can
        // wait without blocking SwiftUI's main actor. Keep AppKit access on
        // the main thread even when GPU setup originates from that worker.
        void (^attach_view)(void) = ^{
            NSWindow *window = (__bridge NSWindow *)v->native_window;
            NSView *content_view = [window contentView];
            if ([content_view isKindOfClass: [MTKView class]]) {
                _state.view = (MTKView *)content_view;
                [_state.view setDevice: _state.device.device];
            }
        };
        if ([NSThread isMainThread]) attach_view();
        else dispatch_sync(dispatch_get_main_queue(), attach_view);
#else
        _state.view = (__bridge MTKView *)v->native_window;
        [_state.view setDevice:_state.device.device];
#endif
    }

    _state.valid = true;
    _state.cmd_queue = [_state.device.device newCommandQueue];
    _state.cmd_buffer = nil;
    _state.cmd_encoder = nil;
    _state.cur_drawable = nil;
    _state.capture_scope = nil;
    _state.frame_index = 0;
    
    _state.shader_count = 1;
    _state.texture_count = 1;
    _state.sampler_count = 1;
    _state.screen_pass_count = 0;
    free(_state.v2_storage_buffers);
    free(_state.v2_storage_offsets);
    _state.v2_storage_slot_count = GPU_MTL_STORAGE_SLOT_COUNT;
    _state.v2_storage_buffers = calloc(_state.v2_storage_slot_count, sizeof(*_state.v2_storage_buffers));
    _state.v2_storage_offsets = calloc(_state.v2_storage_slot_count, sizeof(*_state.v2_storage_offsets));
    if (!_state.v2_storage_buffers || !_state.v2_storage_offsets) {
        free(_state.v2_storage_buffers);
        free(_state.v2_storage_offsets);
        _state.v2_storage_buffers = nil;
        _state.v2_storage_offsets = NULL;
        _state.v2_storage_slot_count = 0;
        _state.valid = false;
        return false;
    }
    gpu_v2_set_backend(&_mtl_v2_ops,
                       GPU_CAP_BINDLESS_TEXTURES | GPU_CAP_INDIRECT_DRAW | GPU_CAP_READBACK,
                       _state.v2_storage_slot_count);
    
    return true;
}

void gpu_destroy_device() {
    if (!_state.valid) return;
    // Stop new frames before draining the command buffer submitted by the
    // last draw. Its completion handler owns the outstanding semaphore
    // signal, so releasing the semaphore while that frame is still running
    // makes libdispatch trap during application shutdown.
    _state.valid = false;
    if (_state.semaphore != nil) {
        dispatch_semaphore_wait(_state.semaphore, DISPATCH_TIME_FOREVER);
        dispatch_semaphore_signal(_state.semaphore);
    }
    gpu_v2_set_backend(NULL, 0, 0);
    for (i32 i = 0; i < GPU_RESOURCE_POOL_SIZE; ++i) {
#ifndef ENABLE_ARC
        [_state.v2_memory[i] release];
#endif
        _state.v2_memory[i] = nil;
        _state.v2_memory_size[i] = 0;
    }
    free(_state.v2_storage_buffers);
    free(_state.v2_storage_offsets);
    _state.v2_storage_buffers = nil;
    _state.v2_storage_offsets = NULL;
    _state.v2_storage_slot_count = 0;
#ifndef ENABLE_ARC
    [_state.cmd_encoder release];
    [_state.cmd_buffer release];
    [_state.capture_scope release];
    [_state.cmd_queue release];
    [_state.device.device release];
    dispatch_release(_state.semaphore);
#endif
    memset(&_state, 0, sizeof(_state));
}

void gpu_mtl_begin_frame(MTKView *view) {
    if (!_state.valid || _state.device.device == nil || _state.semaphore == nil) {
        return;
    }

    _state.swapchain = (gpu_swapchain_mtl) {
        .width = (int) [view drawableSize].width,
        .height = (int) [view drawableSize].height,
        .sample_count = (int) [view sampleCount],
        .color_format = PIXELFORMAT_BGRA8,
        .depth_stencil_format = PIXELFORMAT_NONE,
        .color_texture = [view multisampleColorTexture],
        .depth_stencil_texture = [view depthStencilTexture],
    };
    
    dispatch_semaphore_wait(_state.semaphore, DISPATCH_TIME_FOREVER);
    gpu_mtl_capture_begin_if_requested();
    _state.cmd_buffer = [_state.cmd_queue commandBuffer];
    [_state.cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> _) { dispatch_semaphore_signal(_state.semaphore); }];
    _state.cur_drawable = [view currentDrawable];
    _state.screen_pass_count = 0;
}

void gpu_commit() {
    if (!_state.valid || nil == _state.cmd_buffer) {
        gpu_v2_frame_end();
        return;
    }
    assert(nil == _state.cmd_encoder);

    if (nil != _state.cur_drawable) {
        [_state.cmd_buffer presentDrawable: _state.cur_drawable];
        _state.cur_drawable = nil;
    }

    [_state.cmd_buffer commit];
    gpu_mtl_capture_end_if_started();
    _state.cmd_buffer = nil;
    _state.frame_index += 1;
    gpu_v2_frame_end();
}

id<MTLLibrary> _mtl_library_from_bytecode(ns_data src) {
    NSError *err = nil;
    dispatch_data_t data = dispatch_data_create(src.data, src.len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> lib = [_state.device.device newLibraryWithData: data error: &err];
    if (nil == lib) {
        NSLog(@"Error: %@", err);
        NSLog(@"Source: %s", [err.localizedDescription UTF8String]);
    }
#ifndef ENABLE_ARC
    dispatch_release(data);
#endif
    return lib;
}

id<MTLLibrary> _mtl_library_from_code(ns_str src) {
    NSError *err = nil;
    NSString *source = [[NSString alloc] initWithBytes: src.data length: src.len encoding: NSUTF8StringEncoding];
    id<MTLLibrary> lib = source
        ? [_state.device.device newLibraryWithSource: source options: nil error: &err]
        : nil;
    if (nil == lib) {
        NSLog(@"Error: %@", err);
        NSLog(@"Source: %s", [err.localizedDescription UTF8String]);
    }
#ifndef ENABLE_ARC
    [source release];
#endif
    return lib;
}

void gpu_set_viewport(int x, int y, int width, int height) {
    assert(nil != _state.cmd_encoder);
    MTLViewport viewport = {
        .originX = (double)x,
        .originY = (double)y,
        .width = (double)width,
        .height = (double)height,
        .znear = 0.0,
        .zfar = 1.0,
    };
    [_state.cmd_encoder setViewport: viewport];
}

void gpu_set_scissor(int x, int y, int width, int height) {
    assert(nil != _state.cmd_encoder);
    MTLScissorRect scissor = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    [_state.cmd_encoder setScissorRect: scissor];
}

// Nano Script GPU v2 backend.

static ns_bool mtl_v2_mem_create(u32 slot, u64 size, u32 flags, const char *name, u64 *base_va) {
    ns_unused(flags);
    if (slot >= GPU_RESOURCE_POOL_SIZE || size == 0 || size > (u64)NSUIntegerMax || !name || !name[0]) return false;
    id<MTLBuffer> buffer = [_state.device.device newBufferWithLength:(NSUInteger)size
                                                             options:MTLResourceStorageModeShared];
    if (!buffer) return false;
    buffer.label = [NSString stringWithUTF8String:name];
    _state.v2_memory[slot] = buffer;
    _state.v2_memory_size[slot] = size;
    if (base_va) *base_va = 0;
    return true;
}

static void mtl_v2_mem_destroy(u32 slot) {
    if (slot >= GPU_RESOURCE_POOL_SIZE) return;
#ifndef ENABLE_ARC
    [_state.v2_memory[slot] release];
#endif
    _state.v2_memory[slot] = nil;
    _state.v2_memory_size[slot] = 0;
}

static void mtl_v2_mem_write(u32 slot, u64 offset, const void *src, u64 size) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot] || !src ||
        offset > _state.v2_memory_size[slot] || size > _state.v2_memory_size[slot] - offset) return;
    memcpy((u8 *)[_state.v2_memory[slot] contents] + offset, src, (size_t)size);
}

static ns_bool mtl_v2_mem_read(u32 slot, u64 offset, void *dst, u64 size) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot] || !dst ||
        offset > _state.v2_memory_size[slot] || size > _state.v2_memory_size[slot] - offset) return false;
    memcpy(dst, (u8 *)[_state.v2_memory[slot] contents] + offset, (size_t)size);
    return true;
}

static void *mtl_v2_mem_host_ptr(u32 slot) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot]) return NULL;
    return [_state.v2_memory[slot] contents];
}

static u32 mtl_v2_texture_create(i32 width, i32 height, i32 depth_or_layers,
                                 i32 format, u32 usage, i32 mip_count, i32 kind) {
    if (width <= 0 || height <= 0 || depth_or_layers <= 0 ||
        _state.texture_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor new];
    desc.textureType = _mtl_texture_type((gpu_texture_type)kind);
    desc.pixelFormat = _mtl_pixel_format((gpu_pixel_format)format);
    desc.width = (NSUInteger)width;
    desc.height = (NSUInteger)height;
    desc.mipmapLevelCount = (NSUInteger)(mip_count > 0 ? mip_count : 1);
    if (kind == TEXTURE_3D) desc.depth = (NSUInteger)depth_or_layers;
    else if (kind == TEXTURE_ARRAY) desc.arrayLength = (NSUInteger)depth_or_layers;
    else desc.arrayLength = 1;
    desc.resourceOptions = MTLResourceStorageModeShared;
    desc.usage = 0;
    if (usage == TEXTURE_USAGE_DEFAULT || (usage & TEXTURE_USAGE_READ)) desc.usage |= MTLTextureUsageShaderRead;
    if (usage & TEXTURE_USAGE_WRITE) desc.usage |= MTLTextureUsageShaderWrite;
    if (usage & TEXTURE_USAGE_RENDER_TARGET) desc.usage |= MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [_state.device.device newTextureWithDescriptor:desc];
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!texture) return 0;
    u32 id = _state.texture_count++;
    _state.textures[id] = (gpu_texture_mtl){
        .texture = texture,
        .width = (NSUInteger)width,
        .height = (NSUInteger)height,
        .depth = (NSUInteger)depth_or_layers,
        .format = (gpu_pixel_format)format,
        .type = (gpu_texture_type)kind,
        .resource_options = MTLResourceStorageModeShared,
    };
    return id;
}

static void mtl_v2_texture_upload(u32 tex, i32 mip, i32 layer, const void *data, u64 size) {
    if (!tex || tex >= _state.texture_count || !data || size == 0 || mip < 0 || layer < 0) return;
    gpu_texture_mtl *texture = &_state.textures[tex];
    NSUInteger width = MAX((NSUInteger)1, texture->width >> mip);
    NSUInteger height = MAX((NSUInteger)1, texture->height >> mip);
    NSUInteger depth = texture->type == TEXTURE_3D
                           ? MAX((NSUInteger)1, texture->depth >> mip)
                           : 1;
    NSUInteger row = (NSUInteger)gpu_pixel_format_row_pitch(texture->format, (i32)width, 1);
    NSUInteger image = (NSUInteger)gpu_pixel_format_surface_pitch(texture->format, (i32)width, (i32)height, 1);
    MTLRegion region = texture->type == TEXTURE_3D
                           ? MTLRegionMake3D(0, 0, 0, width, height, depth)
                           : MTLRegionMake2D(0, 0, width, height);
    [texture->texture replaceRegion:region
                       mipmapLevel:(NSUInteger)mip
                             slice:(NSUInteger)layer
                         withBytes:data
                       bytesPerRow:row
                     bytesPerImage:image];
}

static void mtl_v2_texture_destroy(u32 tex) {
    if (!tex || tex >= GPU_RESOURCE_POOL_SIZE) return;
#ifndef ENABLE_ARC
    [_state.textures[tex].texture release];
#endif
    memset(&_state.textures[tex], 0, sizeof(_state.textures[tex]));
}

static u32 mtl_v2_sampler_create(i32 min_filter, i32 mag_filter, i32 mip_filter,
                                 i32 wrap_u, i32 wrap_v, i32 wrap_w,
                                 i32 compare_func, i32 max_anisotropy) {
    if (_state.sampler_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
    desc.minFilter = min_filter == FILTER_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    desc.magFilter = mag_filter == FILTER_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    desc.mipFilter = mip_filter == FILTER_NEAREST ? MTLSamplerMipFilterNearest
                                                  : (mip_filter == FILTER_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped);
    desc.sAddressMode = wrap_u == WRAP_REPEAT ? MTLSamplerAddressModeRepeat
                                              : (wrap_u == WRAP_MIRRORED_REPEAT ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge);
    desc.tAddressMode = wrap_v == WRAP_REPEAT ? MTLSamplerAddressModeRepeat
                                              : (wrap_v == WRAP_MIRRORED_REPEAT ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge);
    desc.rAddressMode = wrap_w == WRAP_REPEAT ? MTLSamplerAddressModeRepeat
                                              : (wrap_w == WRAP_MIRRORED_REPEAT ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge);
    desc.compareFunction = _mtl_compare_function((gpu_compare_func)compare_func);
    desc.maxAnisotropy = (NSUInteger)(max_anisotropy > 0 ? max_anisotropy : 1);
    id<MTLSamplerState> sampler = [_state.device.device newSamplerStateWithDescriptor:desc];
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!sampler) return 0;
    u32 id = _state.sampler_count++;
    _state.samplers[id] = (gpu_sampler_mtl){
        .sampler = sampler,
        .min_filter = (gpu_filter)min_filter,
        .mag_filter = (gpu_filter)mag_filter,
        .mip_filter = (gpu_filter)mip_filter,
        .wrap_u = (gpu_wrap)wrap_u,
        .wrap_v = (gpu_wrap)wrap_v,
        .wrap_w = (gpu_wrap)wrap_w,
    };
    return id;
}

static void mtl_v2_sampler_destroy(u32 smp) {
    if (!smp || smp >= GPU_RESOURCE_POOL_SIZE) return;
#ifndef ENABLE_ARC
    [_state.samplers[smp].sampler release];
#endif
    memset(&_state.samplers[smp], 0, sizeof(_state.samplers[smp]));
}

static bool mtl_source_uses(const char *source, const char *symbol) {
    return source && strstr(source, symbol) != NULL;
}

static u32 mtl_v2_shader_graphics_create(const char *vs_src, const char *fs_src,
                                         const char *vs_entry, const char *fs_entry) {
    if (!vs_src || !fs_src || !vs_entry || !fs_entry ||
        _state.shader_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    id<MTLLibrary> vertex_lib = _mtl_library_from_code(ns_str_cstr(vs_src));
    id<MTLLibrary> fragment_lib = _mtl_library_from_code(ns_str_cstr(fs_src));
    id<MTLFunction> vertex_func = vertex_lib
                                      ? [vertex_lib newFunctionWithName:[NSString stringWithUTF8String:vs_entry]]
                                      : nil;
    id<MTLFunction> fragment_func = fragment_lib
                                        ? [fragment_lib newFunctionWithName:[NSString stringWithUTF8String:fs_entry]]
                                        : nil;
    if (!vertex_lib || !fragment_lib || !vertex_func || !fragment_func) {
#ifndef ENABLE_ARC
        [vertex_func release];
        [fragment_func release];
        [vertex_lib release];
        [fragment_lib release];
#endif
        return 0;
    }
    u32 id = _state.shader_count++;
    gpu_shader_mtl *record = &_state.shaders[id];
    *record = (gpu_shader_mtl){
        .vertex_lib = vertex_lib,
        .fragment_lib = fragment_lib,
        .vertex_func = vertex_func,
        .fragment_func = fragment_func,
    };
    record->uses_root = mtl_source_uses(vs_src, "ns_root") || mtl_source_uses(fs_src, "ns_root");
    record->uses_read_texture = mtl_source_uses(vs_src, "ns_read_texture") || mtl_source_uses(fs_src, "ns_read_texture");
    record->uses_write_texture = mtl_source_uses(vs_src, "ns_write_texture") || mtl_source_uses(fs_src, "ns_write_texture");
    record->uses_write_texture_secondary = mtl_source_uses(vs_src, "ns_secondary_write_texture") || mtl_source_uses(fs_src, "ns_secondary_write_texture");
    record->uses_texture_map = mtl_source_uses(vs_src, "ns_texture_map") || mtl_source_uses(fs_src, "ns_texture_map");
    record->uses_mask_map = mtl_source_uses(vs_src, "ns_mask_map") || mtl_source_uses(fs_src, "ns_mask_map");
    record->uses_shadow_map = mtl_source_uses(vs_src, "ns_shadow_map") || mtl_source_uses(fs_src, "ns_shadow_map");
    return id;
}

static u32 mtl_v2_shader_compute_create(const char *src, const char *entry) {
    if (_state.shader_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    id<MTLLibrary> library = _mtl_library_from_code(ns_str_cstr(src));
    if (!library) return 0;
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:entry]];
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = function
        ? [_state.device.device newComputePipelineStateWithFunction:function error:&error]
        : nil;
    if (!pipeline) {
        NSLog(@"Failed to create v2 compute shader: %@", error);
#ifndef ENABLE_ARC
        [function release];
        [library release];
#endif
        return 0;
    }
    u32 id = _state.shader_count++;
    _state.shaders[id] = (gpu_shader_mtl){
        .compute_lib = library,
        .compute_func = function,
        .compute_pso = pipeline,
        .uses_root = mtl_source_uses(src, "ns_root"),
        .uses_read_texture = mtl_source_uses(src, "ns_read_texture"),
        .uses_write_texture = mtl_source_uses(src, "ns_write_texture"),
        .uses_write_texture_secondary = mtl_source_uses(src, "ns_secondary_write_texture"),
    };
    return id;
}

static void mtl_v2_shader_destroy(u32 shader) {
    if (!shader || shader >= GPU_RESOURCE_POOL_SIZE) return;
    gpu_shader_mtl *record = &_state.shaders[shader];
#ifndef ENABLE_ARC
    [record->vertex_func release];
    [record->fragment_func release];
    [record->vertex_lib release];
    [record->fragment_lib release];
    [record->compute_pso release];
    [record->compute_func release];
    [record->compute_lib release];
    [record->v2_pso release];
    [record->v2_dso release];
#endif
    memset(record, 0, sizeof(*record));
}

static void mtl_v2_ensure_frame(void) {
    if (_state.cmd_buffer == nil && _state.view != nil) gpu_mtl_begin_frame(_state.view);
}

static void mtl_v2_pass_begin(u32 color0, u32 color1, u32 color2, u32 color3,
                              u32 depth, u32 load_flags, gpu_color clear, f32 depth_clear) {
    mtl_v2_ensure_frame();
    if (_state.cmd_buffer == nil || _state.cmd_encoder != nil) return;
    u32 colors[4] = {color0, color1, color2, color3};
    MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
    memset(_state.v2_pass_colors, 0, sizeof(_state.v2_pass_colors));
    _state.v2_pass_depth = MTLPixelFormatInvalid;
    for (u32 i = 0; i < 4; ++i) {
        if (!colors[i] || colors[i] >= _state.texture_count) continue;
        gpu_texture_mtl *texture = &_state.textures[colors[i]];
        desc.colorAttachments[i].texture = texture->texture;
        desc.colorAttachments[i].storeAction = MTLStoreActionStore;
        desc.colorAttachments[i].loadAction = _mtl_load_action((gpu_load_action)((load_flags >> (i * 2)) & 3));
        desc.colorAttachments[i].clearColor = MTLClearColorMake(clear.r, clear.g, clear.b, clear.a);
        _state.v2_pass_colors[i] = _mtl_pixel_format(texture->format);
    }
    if (depth && depth < _state.texture_count) {
        gpu_texture_mtl *texture = &_state.textures[depth];
        desc.depthAttachment.texture = texture->texture;
        desc.depthAttachment.storeAction = MTLStoreActionStore;
        desc.depthAttachment.loadAction = _mtl_load_action((gpu_load_action)((load_flags >> 8) & 3));
        desc.depthAttachment.clearDepth = depth_clear;
        _state.v2_pass_depth = _mtl_pixel_format(texture->format);
    }
    _state.cmd_encoder = [_state.cmd_buffer renderCommandEncoderWithDescriptor:desc];
}

static void mtl_v2_screen_pass_begin(gpu_color clear) {
    mtl_v2_ensure_frame();
    if (_state.cmd_buffer == nil || _state.cmd_encoder != nil || _state.cur_drawable == nil) return;
    MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
    desc.colorAttachments[0].texture = _state.cur_drawable.texture;
    desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    desc.colorAttachments[0].loadAction = _state.screen_pass_count++ == 0 ? MTLLoadActionClear : MTLLoadActionLoad;
    desc.colorAttachments[0].clearColor = MTLClearColorMake(clear.r, clear.g, clear.b, clear.a);
    memset(_state.v2_pass_colors, 0, sizeof(_state.v2_pass_colors));
    _state.v2_pass_colors[0] = _state.cur_drawable.texture.pixelFormat;
    _state.v2_pass_depth = MTLPixelFormatInvalid;
    _state.cmd_encoder = [_state.cmd_buffer renderCommandEncoderWithDescriptor:desc];
}

static void mtl_v2_pass_end(void) {
    if (_state.cmd_encoder) {
        [_state.cmd_encoder endEncoding];
        _state.cmd_encoder = nil;
    }
}

static void mtl_v2_set_shader(u32 shader) {
    _state.v2_shader = shader;
}

static void mtl_v2_set_state(const gpu_v2_state_desc *desc) {
    if (desc) _state.v2_render_state = *desc;
}

static void mtl_v2_set_root(u32 slot, u64 offset, gpu_addr addr) {
    ns_unused(addr);
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot]) return;
    _state.v2_root_buffer = _state.v2_memory[slot];
    _state.v2_root_offset = offset;
}

static void mtl_v2_set_storage(u32 binding, u32 slot, u64 offset, gpu_addr addr) {
    ns_unused(addr);
    if (binding < GPU_MTL_STORAGE_BINDING_BASE ||
        binding - GPU_MTL_STORAGE_BINDING_BASE >= _state.v2_storage_slot_count ||
        slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot]) return;
    u32 index = binding - GPU_MTL_STORAGE_BINDING_BASE;
    _state.v2_storage_buffers[index] = _state.v2_memory[slot];
    _state.v2_storage_offsets[index] = offset;
}

static void mtl_v2_bind_root(gpu_shader_mtl *shader, id<MTLCommandEncoder> encoder, bool compute) {
    if (!shader) return;
    if (compute) {
        id<MTLComputeCommandEncoder> compute_encoder = (id<MTLComputeCommandEncoder>)encoder;
        if (shader->uses_root && _state.v2_root_buffer) [compute_encoder setBuffer:_state.v2_root_buffer offset:(NSUInteger)_state.v2_root_offset atIndex:0];
        for (u32 i = 0; i < _state.v2_storage_slot_count; ++i) {
            if (_state.v2_storage_buffers[i]) [compute_encoder setBuffer:_state.v2_storage_buffers[i] offset:(NSUInteger)_state.v2_storage_offsets[i] atIndex:GPU_MTL_STORAGE_BINDING_BASE + i];
        }
    } else {
        id<MTLRenderCommandEncoder> render_encoder = (id<MTLRenderCommandEncoder>)encoder;
        if (shader->uses_root && _state.v2_root_buffer) {
            [render_encoder setVertexBuffer:_state.v2_root_buffer offset:(NSUInteger)_state.v2_root_offset atIndex:0];
            [render_encoder setFragmentBuffer:_state.v2_root_buffer offset:(NSUInteger)_state.v2_root_offset atIndex:0];
        }
        for (u32 i = 0; i < _state.v2_storage_slot_count; ++i) {
            if (_state.v2_storage_buffers[i]) {
                [render_encoder setVertexBuffer:_state.v2_storage_buffers[i] offset:(NSUInteger)_state.v2_storage_offsets[i] atIndex:GPU_MTL_STORAGE_BINDING_BASE + i];
                [render_encoder setFragmentBuffer:_state.v2_storage_buffers[i] offset:(NSUInteger)_state.v2_storage_offsets[i] atIndex:GPU_MTL_STORAGE_BINDING_BASE + i];
            }
        }
    }
    if (!_state.v2_root_buffer) return;
    const f32 *words = (const f32 *)((const u8 *)[_state.v2_root_buffer contents] + _state.v2_root_offset);
    u32 texture0 = words[0] > 0.0f ? (u32)(words[0] + 0.5f) : 0;
    u32 texture1 = words[1] > 0.0f ? (u32)(words[1] + 0.5f) : 0;
    u32 texture2 = words[2] > 0.0f ? (u32)(words[2] + 0.5f) : 0;
    if (compute) {
        id<MTLComputeCommandEncoder> compute_encoder = (id<MTLComputeCommandEncoder>)encoder;
        if (shader->uses_read_texture && texture0 < _state.texture_count)
            [compute_encoder setTexture:_state.textures[texture0].texture atIndex:0];
        if (shader->uses_write_texture && texture1 < _state.texture_count)
            [compute_encoder setTexture:_state.textures[texture1].texture atIndex:1];
        if (shader->uses_write_texture_secondary && texture2 < _state.texture_count)
            [compute_encoder setTexture:_state.textures[texture2].texture atIndex:15];
    } else {
        id<MTLRenderCommandEncoder> render_encoder = (id<MTLRenderCommandEncoder>)encoder;
        if (shader->uses_shadow_map && texture0 < _state.texture_count)
            [render_encoder setFragmentTexture:_state.textures[texture0].texture atIndex:0];
        if (shader->uses_texture_map && texture0 < _state.texture_count)
            [render_encoder setFragmentTexture:_state.textures[texture0].texture atIndex:1];
        if (shader->uses_mask_map && texture1 < _state.texture_count)
            [render_encoder setFragmentTexture:_state.textures[texture1].texture atIndex:2];
    }
}

static ns_bool mtl_v2_ensure_pipeline(gpu_shader_mtl *shader) {
    if (!shader || !shader->vertex_func || !shader->fragment_func) return false;
    bool same = shader->v2_pipeline_valid &&
        memcmp(&shader->v2_state, &_state.v2_render_state, sizeof(gpu_v2_state_desc)) == 0 &&
        memcmp(shader->v2_colors, _state.v2_pass_colors, sizeof(shader->v2_colors)) == 0 &&
        shader->v2_depth == _state.v2_pass_depth;
    if (same) return true;
#ifndef ENABLE_ARC
    [shader->v2_pso release];
    [shader->v2_dso release];
#endif
    shader->v2_pso = nil;
    shader->v2_dso = nil;
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = shader->vertex_func;
    desc.fragmentFunction = shader->fragment_func;
    for (u32 i = 0; i < 4; ++i) {
        desc.colorAttachments[i].pixelFormat = _state.v2_pass_colors[i];
        desc.colorAttachments[i].writeMask = (MTLColorWriteMask)_state.v2_render_state.color_mask;
        if (_state.v2_render_state.blend_preset != GPU_BLEND_OFF) {
            desc.colorAttachments[i].blendingEnabled = YES;
            desc.colorAttachments[i].rgbBlendOperation = MTLBlendOperationAdd;
            desc.colorAttachments[i].alphaBlendOperation = MTLBlendOperationAdd;
            if (_state.v2_render_state.blend_preset == GPU_BLEND_ADDITIVE) {
                desc.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                desc.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactorOne;
            } else if (_state.v2_render_state.blend_preset == GPU_BLEND_PREMULTIPLIED) {
                desc.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorOne;
                desc.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            } else {
                desc.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                desc.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            }
            desc.colorAttachments[i].sourceAlphaBlendFactor = MTLBlendFactorOne;
            desc.colorAttachments[i].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
    }
    desc.depthAttachmentPixelFormat = _state.v2_pass_depth;
    NSError *error = nil;
    shader->v2_pso = [_state.device.device newRenderPipelineStateWithDescriptor:desc error:&error];
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!shader->v2_pso) {
        NSLog(@"Failed to create v2 render pipeline: %@", error);
        return false;
    }
    if (_state.v2_pass_depth != MTLPixelFormatInvalid) {
        MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = _mtl_compare_function((gpu_compare_func)_state.v2_render_state.depth_compare);
        depth.depthWriteEnabled = _state.v2_render_state.depth_write;
        shader->v2_dso = [_state.device.device newDepthStencilStateWithDescriptor:depth];
#ifndef ENABLE_ARC
        [depth release];
#endif
    }
    shader->v2_state = _state.v2_render_state;
    memcpy(shader->v2_colors, _state.v2_pass_colors, sizeof(shader->v2_colors));
    shader->v2_depth = _state.v2_pass_depth;
    shader->v2_pipeline_valid = true;
    return true;
}

static gpu_shader_mtl *mtl_v2_prepare_draw(void) {
    if (!_state.cmd_encoder || !_state.v2_shader || _state.v2_shader >= _state.shader_count) return NULL;
    gpu_shader_mtl *shader = &_state.shaders[_state.v2_shader];
    if (!mtl_v2_ensure_pipeline(shader)) return NULL;
    [_state.cmd_encoder setRenderPipelineState:shader->v2_pso];
    [_state.cmd_encoder setCullMode:_mtl_cull_mode((gpu_cull_mode)_state.v2_render_state.cull_mode)];
    [_state.cmd_encoder setFrontFacingWinding:_mtl_winding((gpu_face_winding)_state.v2_render_state.face_winding)];
    if (shader->v2_dso) [_state.cmd_encoder setDepthStencilState:shader->v2_dso];
    mtl_v2_bind_root(shader, _state.cmd_encoder, false);
    return shader;
}

static void mtl_v2_draw(i32 vertex_base, i32 vertex_count, i32 instance_count) {
    if (!mtl_v2_prepare_draw()) return;
    [_state.cmd_encoder drawPrimitives:_mtl_primitive_type((gpu_primitive_type)_state.v2_render_state.primitive_type)
                            vertexStart:(NSUInteger)vertex_base
                            vertexCount:(NSUInteger)vertex_count
                          instanceCount:(NSUInteger)instance_count];
}

static void mtl_v2_draw_indexed(u32 slot, u64 offset, i32 index_type,
                                i32 index_count, i32 instance_count, i32 base_vertex) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot] || !mtl_v2_prepare_draw()) return;
    [_state.cmd_encoder drawIndexedPrimitives:_mtl_primitive_type((gpu_primitive_type)_state.v2_render_state.primitive_type)
                                   indexCount:(NSUInteger)index_count
                                    indexType:_mtl_index_type((gpu_index_type)index_type)
                                  indexBuffer:_state.v2_memory[slot]
                            indexBufferOffset:(NSUInteger)offset
                                instanceCount:(NSUInteger)instance_count
                                   baseVertex:base_vertex
                                 baseInstance:0];
}

static void mtl_v2_draw_indirect(u32 slot, u64 offset, i32 draw_count, i32 stride) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot] || !mtl_v2_prepare_draw()) return;
    NSUInteger step = stride > 0 ? (NSUInteger)stride : sizeof(MTLDrawPrimitivesIndirectArguments);
    for (i32 i = 0; i < draw_count; ++i) {
        [_state.cmd_encoder drawPrimitives:_mtl_primitive_type((gpu_primitive_type)_state.v2_render_state.primitive_type)
                           indirectBuffer:_state.v2_memory[slot]
                     indirectBufferOffset:(NSUInteger)offset + (NSUInteger)i * step];
    }
}

static void mtl_v2_dispatch(i32 x, i32 y, i32 z) {
    if (!_state.v2_shader || _state.v2_shader >= _state.shader_count) return;
    gpu_shader_mtl *shader = &_state.shaders[_state.v2_shader];
    if (!shader->compute_pso) return;
    mtl_v2_ensure_frame();
    if (_state.cmd_buffer == nil) return;
    id<MTLComputeCommandEncoder> encoder = [_state.cmd_buffer computeCommandEncoder];
    [encoder setComputePipelineState:shader->compute_pso];
    mtl_v2_bind_root(shader, encoder, true);
    NSUInteger width = MIN((NSUInteger)8, shader->compute_pso.maxTotalThreadsPerThreadgroup);
    [encoder dispatchThreads:MTLSizeMake((NSUInteger)x, (NSUInteger)y, (NSUInteger)z)
       threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [encoder endEncoding];
}

static void mtl_v2_dispatch_indirect(u32 slot, u64 offset) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot] ||
        !_state.v2_shader || _state.v2_shader >= _state.shader_count) return;
    gpu_shader_mtl *shader = &_state.shaders[_state.v2_shader];
    if (!shader->compute_pso) return;
    mtl_v2_ensure_frame();
    id<MTLComputeCommandEncoder> encoder = [_state.cmd_buffer computeCommandEncoder];
    [encoder setComputePipelineState:shader->compute_pso];
    mtl_v2_bind_root(shader, encoder, true);
    [encoder dispatchThreadgroupsWithIndirectBuffer:_state.v2_memory[slot]
                              indirectBufferOffset:(NSUInteger)offset
                             threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    [encoder endEncoding];
}

static const gpu_v2_ops _mtl_v2_ops = {
    .mem_create = mtl_v2_mem_create,
    .mem_destroy = mtl_v2_mem_destroy,
    .mem_write = mtl_v2_mem_write,
    .mem_read = mtl_v2_mem_read,
    .mem_host_ptr = mtl_v2_mem_host_ptr,
    .texture_create = mtl_v2_texture_create,
    .texture_upload = mtl_v2_texture_upload,
    .texture_destroy = mtl_v2_texture_destroy,
    .sampler_create = mtl_v2_sampler_create,
    .sampler_destroy = mtl_v2_sampler_destroy,
    .shader_graphics_create = mtl_v2_shader_graphics_create,
    .shader_compute_create = mtl_v2_shader_compute_create,
    .shader_destroy = mtl_v2_shader_destroy,
    .pass_begin = mtl_v2_pass_begin,
    .screen_pass_begin = mtl_v2_screen_pass_begin,
    .pass_end = mtl_v2_pass_end,
    .set_shader = mtl_v2_set_shader,
    .set_state = mtl_v2_set_state,
    .set_root = mtl_v2_set_root,
    .set_storage = mtl_v2_set_storage,
    .draw = mtl_v2_draw,
    .draw_indexed = mtl_v2_draw_indexed,
    .draw_indirect = mtl_v2_draw_indirect,
    .dispatch = mtl_v2_dispatch,
    .dispatch_indirect = mtl_v2_dispatch_indirect,
};
