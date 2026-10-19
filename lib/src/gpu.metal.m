#include <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <AvailabilityMacros.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include <objc/objc.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
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

// PVRTC has no mapping: Apple deprecated it (macOS 15, iOS 18) in favour
// of ASTC, ETC2 and BC, so a PVRTC texture is refused and its loader falls
// back to the source picture.
static MTLPixelFormat _mtl_pixel_format(gpu_pixel_format format) {
    switch (format) {
        case PIXELFORMAT_ASTC_4x4_RGBA: return MTLPixelFormatASTC_4x4_LDR;
        case PIXELFORMAT_ASTC_4x4_SRGBA: return MTLPixelFormatASTC_4x4_sRGB;
        case PIXELFORMAT_ASTC_6x6_RGBA: return MTLPixelFormatASTC_6x6_LDR;
        case PIXELFORMAT_ASTC_6x6_SRGBA: return MTLPixelFormatASTC_6x6_sRGB;
        default: break;
    }
    if (@available(macOS 11.0, iOS 8.0, *)) {
        switch (format) {
            case PIXELFORMAT_ETC2_RGB8: return MTLPixelFormatETC2_RGB8;
            case PIXELFORMAT_ETC2_SRGB8: return MTLPixelFormatETC2_RGB8_sRGB;
            case PIXELFORMAT_ETC2_RGB8A1: return MTLPixelFormatETC2_RGB8A1;
            case PIXELFORMAT_ETC2_RGBA8: return MTLPixelFormatEAC_RGBA8;
            case PIXELFORMAT_ETC2_SRGB8A8: return MTLPixelFormatEAC_RGBA8_sRGB;
            case PIXELFORMAT_ETC2_RG11: return MTLPixelFormatEAC_RG11Unorm;
            case PIXELFORMAT_ETC2_RG11SN: return MTLPixelFormatEAC_RG11Snorm;
            default: break;
        }
    }
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
        case PIXELFORMAT_DEPTH16: return MTLPixelFormatDepth16Unorm;
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
        case PIXELFORMAT_ASTC_6x6_RGBA:
        case PIXELFORMAT_ASTC_6x6_SRGBA:
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

static MTLStencilOperation _mtl_stencil_op(gpu_stencil_op op) {
    switch (op) {
        case STENCIL_OP_ZERO: return MTLStencilOperationZero;
        case STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
        case STENCIL_OP_INCR_CLAMP: return MTLStencilOperationIncrementClamp;
        case STENCIL_OP_DECR_CLAMP: return MTLStencilOperationDecrementClamp;
        case STENCIL_OP_INVERT: return MTLStencilOperationInvert;
        case STENCIL_OP_INCR_WRAP: return MTLStencilOperationIncrementWrap;
        case STENCIL_OP_DECR_WRAP: return MTLStencilOperationDecrementWrap;
        case STENCIL_OP_KEEP: return MTLStencilOperationKeep;
    }
    return MTLStencilOperationKeep;
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

// ---- compiled-pipeline cache ------------------------------------------------
//
// Metal cannot serialize a library it compiled from source, so the source is
// still compiled every launch; what a launch can skip is turning those
// functions into pipeline states, which is the expensive half. A per-shader
// MTLBinaryArchive holds the pipelines the shader was drawn with, and storage
// keeps the archive on disk under the shader name and a hash of its source.
// Every tier Metal runs on supports this - macOS, iOS and visionOS alike - and
// none of them can run the offline metal compiler, which is why the archive is
// what gets cached rather than a metallib.
//
// storage is reached through dlsym rather than a link dependency: gpu must keep
// building where there is no storage module, and a program that never called
// storage_init simply compiles its pipelines every launch.

#define GPU_MTL_CACHE_NAME_CAPACITY 128

// A shader keeps a pipeline per render state and pass formats it is drawn
// with, so a shader drawn in two passes a frame - a shadow pass and the main
// pass - builds each pipeline once. Past GPU_MTL_PSO_SLOTS states the oldest
// pipeline is replaced and rebuilt on its next use.
#define GPU_MTL_PSO_SLOTS 8
// A pipeline rebuilt after eviction comes from Metal's in-process cache faster
// than from a binary archive lookup, so the archive is consulted once per
// render state and the repeats take the plain path. These are the states
// already looked up this run; a shader with more of them than fit leaves the
// rest uncached rather than growing the record.
#define GPU_MTL_ARCHIVE_KEY_SLOTS 16

// Everything a render pipeline is built from besides the shader functions:
// the render state without its depth-stencil half, and the pass formats.
typedef struct gpu_mtl_pipeline_key {
    gpu_v2_state_desc state;
    MTLPixelFormat colors[4];
    MTLPixelFormat depth;
    MTLPixelFormat stencil;
} gpu_mtl_pipeline_key;

typedef struct gpu_mtl_pso_entry {
    gpu_mtl_pipeline_key key;
    id<MTLRenderPipelineState> pso;
} gpu_mtl_pso_entry;

typedef const char *(*gpu_mtl_cache_path_fn)(const char *name, u64 hash);
typedef int (*gpu_mtl_cache_adopt_fn)(const char *name, u64 hash, const char *path);

static struct {
    gpu_mtl_cache_path_fn path;
    gpu_mtl_cache_adopt_fn adopt;
    bool resolved;
} _mtl_cache;

static bool mtl_cache_available(void) {
    if (!_mtl_cache.resolved) {
        _mtl_cache.resolved = true;
        _mtl_cache.path = (gpu_mtl_cache_path_fn)dlsym(RTLD_DEFAULT, "storage_cache_path");
        _mtl_cache.adopt = (gpu_mtl_cache_adopt_fn)dlsym(RTLD_DEFAULT, "storage_cache_adopt");
    }
    return _mtl_cache.path && _mtl_cache.adopt;
}

typedef struct gpu_shader_mtl {
    id<MTLLibrary> vertex_lib;
    id<MTLLibrary> fragment_lib;
    id<MTLFunction> vertex_func;
    id<MTLFunction> fragment_func;
    id<MTLLibrary> compute_lib;
    id<MTLFunction> compute_func;
    id<MTLComputePipelineState> compute_pso;
    gpu_mtl_pso_entry v2_psos[GPU_MTL_PSO_SLOTS];
    u32 v2_pso_count;
    u32 v2_pso_next; // the slot replaced once all are taken
    // Compiled-pipeline cache: `archive` holds every pipeline this shader has
    // been drawn with, `cache_name`/`cache_hash` address its storage entry.
    id<MTLBinaryArchive> archive;
    char cache_name[GPU_MTL_CACHE_NAME_CAPACITY];
    u64 cache_hash;
    gpu_mtl_pipeline_key archive_keys[GPU_MTL_ARCHIVE_KEY_SLOTS];
    u32 archive_key_count;
    bool uses_root;
    bool uses_read_texture;
    bool uses_write_texture;
    bool uses_write_texture_secondary;
    bool uses_texture_map;
    bool uses_mask_map;
    bool uses_texture_table;
    bool uses_shadow_map;
} gpu_shader_mtl;

typedef struct gpu_texture_mtl {
    id<MTLTexture> texture;
    NSUInteger width, height, depth;
    gpu_pixel_format format;
    gpu_texture_type type;
    MTLResourceOptions resource_options;
    bool transient_render_target;
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

#define GPU_MTL_DSO_CACHE_SIZE 64

typedef struct gpu_mtl_dso_entry {
    i32 depth_compare;
    i32 depth_write;
    i32 stencil_compare;
    i32 stencil_pass_op;
    id<MTLDepthStencilState> dso;
} gpu_mtl_dso_entry;

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
    id<MTLBlitCommandEncoder> blit_encoder;
    id<CAMetalDrawable> cur_drawable;
    // A CAMetalLayer drawable may be read and presented once. The host frame
    // owns that single acquire/present pair, so a mid-frame gpu_commit() cannot
    // hand the drawable back a second time.
    bool drawable_presented;
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
    // gpu_set_texture_table, bound at GPU_MTL_TEXTURE_TABLE_BASE.
    u32 v2_texture_table[GPU_TEXTURE_TABLE_SIZE];
    MTLPixelFormat v2_pass_colors[4];
    MTLPixelFormat v2_pass_depth;
    MTLPixelFormat v2_pass_stencil;
    // A screen pass always carries the screen depth plane (one pipeline
    // format for every screen pass); only a depth screen pass tests it.
    bool v2_pass_depth_test;
    u32 v2_stencil_ref;
    // The screen pass's depth-stencil buffer, sized to the drawable on demand,
    // and whether this frame's depth has been cleared yet.
    id<MTLTexture> screen_depth_stencil;
    bool screen_depth_cleared;
    // Depth-stencil states are independent of the pipeline, so they are
    // shared across shaders and switching stencil modes never rebuilds a PSO.
    gpu_mtl_dso_entry dso_cache[GPU_MTL_DSO_CACHE_SIZE];
    u32 dso_count;

} gpu_state_mtl;

static gpu_state_mtl _state = {0};
static id<MTLTexture> immersive_color;
static id<MTLTexture> immersive_depth;
static bool immersive_frame;
static u32 immersive_slice;
static const gpu_v2_ops _mtl_v2_ops;
static void mtl_v2_fx_destroy_all(void);
static void mtl_v2_ensure_frame(void);
static void mtl_v2_label_encoder(id<MTLCommandEncoder> encoder, const char *label);

static void gpu_mtl_configure_view(MTKView *view, id<MTLDevice> device) {
    if (view == nil) return;
    [view setDevice:device];
    // Screen depth/stencil belongs to the explicit GPU passes, not MTKView.
    [view setDepthStencilPixelFormat:MTLPixelFormatInvalid];

    CAMetalLayer *layer = (CAMetalLayer *)[view layer];
    [layer setMaximumDrawableCount:GPU_SWAP_BUFFER_COUNT];
#if TARGET_OS_OSX
    [layer setDisplaySyncEnabled:GPU_PRESENT_SYNC_INTERVAL != 0];
#endif
}

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
    _state.semaphore = dispatch_semaphore_create(GPU_SWAP_BUFFER_COUNT);
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
                gpu_mtl_configure_view(_state.view, _state.device.device);
            }
        };
        if ([NSThread isMainThread]) attach_view();
        else dispatch_sync(dispatch_get_main_queue(), attach_view);
#else
        void (^attach_view)(void) = ^{
            _state.view = (__bridge MTKView *)v->native_window;
            gpu_mtl_configure_view(_state.view, _state.device.device);
        };
        if ([NSThread isMainThread]) attach_view();
        else dispatch_sync(dispatch_get_main_queue(), attach_view);
#endif
    }

    _state.valid = true;
    _state.cmd_queue = [_state.device.device newCommandQueue];
    _state.cmd_buffer = nil;
    _state.cmd_encoder = nil;
    _state.blit_encoder = nil;
    _state.cur_drawable = nil;
    _state.drawable_presented = false;
    _state.capture_scope = nil;
    _state.frame_index = 0;
    
    _state.shader_count = 1;
    _state.texture_count = 1;
    _state.sampler_count = 1;
    _state.screen_pass_count = 0;
    _state.screen_depth_cleared = false;
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
        for (i32 i = 0; i < GPU_SWAP_BUFFER_COUNT; ++i) {
            dispatch_semaphore_wait(_state.semaphore, DISPATCH_TIME_FOREVER);
        }
        for (i32 i = 0; i < GPU_SWAP_BUFFER_COUNT; ++i) {
            dispatch_semaphore_signal(_state.semaphore);
        }
    }
    mtl_v2_fx_destroy_all();
    gpu_v2_set_backend(NULL, 0, 0);
    for (i32 i = 0; i < GPU_RESOURCE_POOL_SIZE; ++i) {
#ifndef ENABLE_ARC
        [_state.v2_memory[i] release];
#endif
        _state.v2_memory[i] = nil;
        _state.v2_memory_size[i] = 0;
    }
#ifndef ENABLE_ARC
    for (u32 i = 0; i < _state.dso_count; ++i) [_state.dso_cache[i].dso release];
    [_state.screen_depth_stencil release];
#endif
    free(_state.v2_storage_buffers);
    free(_state.v2_storage_offsets);
    _state.v2_storage_buffers = nil;
    _state.v2_storage_offsets = NULL;
    _state.v2_storage_slot_count = 0;
#ifndef ENABLE_ARC
    [_state.cmd_encoder release];
    [_state.blit_encoder release];
    [_state.cmd_buffer release];
    [_state.capture_scope release];
    [_state.cmd_queue release];
    [_state.device.device release];
    dispatch_release(_state.semaphore);
#endif
    memset(&_state, 0, sizeof(_state));
}

// Open a command buffer when the frame does not have one. A frame that flushes
// mid-way - the icon atlas bake commits before it drops its scratch targets -
// keeps drawing into a fresh buffer, so this is separate from the per-frame
// swapchain state that gpu_mtl_begin_frame() resets.
static void gpu_mtl_begin_cmd_buffer(void) {
    if (!_state.valid || _state.device.device == nil || _state.semaphore == nil) return;
    if (nil != _state.cmd_buffer) return;
    dispatch_semaphore_wait(_state.semaphore, DISPATCH_TIME_FOREVER);
    gpu_mtl_capture_begin_if_requested();
    _state.cmd_buffer = [_state.cmd_queue commandBuffer];
    [_state.cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> _) { dispatch_semaphore_signal(_state.semaphore); }];
}

// The drawable is acquired by the first screen pass rather than by the frame
// start: frames that only render offscreen never take one, and a frame that
// already presented never asks the layer for its drawable again.
// The compositor owns this command buffer and commits both eyes together.
void gpu_mtl_immersive_begin(id<MTLCommandBuffer> buffer, id<MTLTexture> color, id<MTLTexture> depth, u32 slice) {
    assert(_state.cmd_buffer == nil && _state.cmd_encoder == nil);
    immersive_frame = true;
    immersive_color = color;
    immersive_depth = depth;
    immersive_slice = slice;
    _state.cmd_buffer = buffer;
    _state.screen_pass_count = 0;
    _state.screen_depth_cleared = false;
}
void gpu_mtl_immersive_end(void) {
    assert(_state.cmd_encoder == nil);
    _state.cmd_buffer = nil;
    immersive_color = nil;
    immersive_depth = nil;
    immersive_frame = false;
}
void gpu_mtl_immersive_complete(void) {
    gpu_v2_flush_uploads();
    gpu_v2_frame_end();
}

static id<CAMetalDrawable> gpu_mtl_current_drawable(void) {
    if (nil != _state.cur_drawable || _state.drawable_presented) return _state.cur_drawable;
    if (nil == _state.view) return nil;
    _state.cur_drawable = [_state.view currentDrawable];
    return _state.cur_drawable;
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
        .depth_stencil_texture = nil, // v2 owns screen depth/stencil; never ask MTKView to allocate one.
    };

    _state.cur_drawable = nil;
    _state.drawable_presented = false;
    _state.screen_pass_count = 0;
    _state.screen_depth_cleared = false;
    gpu_mtl_begin_cmd_buffer();
}

// Present and end the host frame started by gpu_mtl_begin_frame(). The present
// belongs here rather than in gpu_commit() so a mid-frame flush submits its work
// without publishing a half-drawn drawable, and so the drawable is presented
// exactly once however many command buffers the frame took.
void gpu_mtl_end_frame(MTKView *view) {
    ns_unused(view);
    if (!_state.valid || _state.cmd_queue == nil) return;
    gpu_v2_flush_uploads();
    assert(nil == _state.cmd_encoder);
    assert(nil == _state.blit_encoder);

    // The frame's own buffer, when it has not committed yet: the drawable rides
    // along on it and the in-flight semaphore stays with the drawing work.
    id<MTLCommandBuffer> buffer = _state.cmd_buffer;
    ns_bool owns_frame_buffer = nil != buffer;
    if (nil == buffer && nil != _state.cur_drawable) {
        // The frame flushed its drawing already, so present from a bare buffer.
        // Command buffers execute in commit order, so this still follows the
        // drawing, and it must not take the semaphore the committed work holds -
        // waiting on it here would stall the CPU on the GPU every frame.
        buffer = [_state.cmd_queue commandBuffer];
    }

    if (nil != buffer && nil != _state.cur_drawable) {
        [buffer presentDrawable: _state.cur_drawable];
        _state.drawable_presented = true;
    }
    _state.cur_drawable = nil;

    if (nil != buffer) {
        [buffer commit];
    }
    if (owns_frame_buffer) {
        _state.cmd_buffer = nil;
        // A frame that returned before gpu_commit() still recycles its ring.
        gpu_v2_frame_end();
    }
    // The capture closes here rather than at commit: a frame that flushed
    // mid-way commits more than once, and only this last one carries the
    // present. Outside the buffer check so a frame that drew nothing to the
    // screen still closes the scope it opened.
    gpu_mtl_capture_end_if_started();
    _state.frame_index += 1;
}

void gpu_commit() {
    if (immersive_frame) return; // Compositor Services owns submission.
    gpu_v2_flush_uploads();
    if (!_state.valid || nil == _state.cmd_buffer) {
        gpu_v2_frame_end();
        return;
    }
    assert(nil == _state.cmd_encoder);
    assert(nil == _state.blit_encoder);

    [_state.cmd_buffer commit];
    _state.cmd_buffer = nil;
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

// Compiling Metal source is by far the most expensive thing a launch does, and
// a program repeats it: the transpiler emits one self-contained source per
// entry, so every shader that shares a stage fn - four bloom passes over one
// vertex fn, the ui renderer's four fragment entries over one source - asks for
// the same text again. Identical source compiles once and the library is
// handed out with a reference of its own, since callers release what they get.
typedef struct gpu_mtl_library_entry {
    u64 hash;
    i32 len;
    char *source;
    id<MTLLibrary> library;
} gpu_mtl_library_entry;

static gpu_mtl_library_entry *_mtl_libraries;
static u32 _mtl_library_count;
static u32 _mtl_library_capacity;

static u64 _mtl_source_hash(ns_str src) {
    u64 hash = 14695981039346656037ull;
    for (i32 i = 0; i < src.len; ++i) {
        hash ^= (u64)(u8)src.data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// The hash narrows the search; the bytes decide, so a collision cannot hand
// back the wrong library.
static id<MTLLibrary> _mtl_library_lookup(ns_str src, u64 hash) {
    for (u32 i = 0; i < _mtl_library_count; ++i) {
        gpu_mtl_library_entry *entry = &_mtl_libraries[i];
        if (entry->hash != hash || entry->len != src.len) continue;
        if (memcmp(entry->source, src.data, (size_t)src.len) == 0) return entry->library;
    }
    return nil;
}

static void _mtl_library_remember(ns_str src, u64 hash, id<MTLLibrary> library) {
    if (_mtl_library_count == _mtl_library_capacity) {
        u32 capacity = _mtl_library_capacity ? _mtl_library_capacity * 2 : 32;
        gpu_mtl_library_entry *grown = (gpu_mtl_library_entry *)realloc(_mtl_libraries, capacity * sizeof(*grown));
        if (!grown) return;
        _mtl_libraries = grown;
        _mtl_library_capacity = capacity;
    }
    char *copy = (char *)malloc((size_t)src.len);
    if (!copy) return;
    memcpy(copy, src.data, (size_t)src.len);
#ifndef ENABLE_ARC
    [library retain];
#endif
    _mtl_libraries[_mtl_library_count++] = (gpu_mtl_library_entry){
        .hash = hash, .len = src.len, .source = copy, .library = library,
    };
}

id<MTLLibrary> _mtl_library_from_code(ns_str src) {
    u64 hash = _mtl_source_hash(src);
    id<MTLLibrary> cached = _mtl_library_lookup(src, hash);
    if (cached) {
#ifndef ENABLE_ARC
        [cached retain];
#endif
        return cached;
    }

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
    if (lib) _mtl_library_remember(src, hash, lib);
    return lib;
}

void gpu_set_viewport(int x, int y, int width, int height) {
    // A screen pass may legitimately fail to open while the MTKView has no
    // drawable (for example while occluded, resizing, or after a long loading
    // frame). The submission surface is void, so callers cannot branch on
    // pass creation; match the other backends and make subsequent state calls
    // harmless until a later frame acquires a drawable.
    if (nil == _state.cmd_encoder) return;
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
    if (nil == _state.cmd_encoder) return;
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
    // A freed buffer may still be the bound root or a storage binding, held
    // by plain pointer: the next draw or dispatch would hand Metal the
    // released object. Unbind it first.
    id<MTLBuffer> buffer = _state.v2_memory[slot];
    if (buffer != nil) {
        if (_state.v2_root_buffer == buffer) _state.v2_root_buffer = nil;
        for (u32 i = 0; i < _state.v2_storage_slot_count; ++i) {
            if (_state.v2_storage_buffers[i] != buffer) continue;
            _state.v2_storage_buffers[i] = nil;
            _state.v2_storage_offsets[i] = 0;
        }
    }
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

static ns_bool mtl_v2_mem_copy(u32 dst_slot, u64 dst_offset, u32 src_slot, u64 src_offset, u64 size) {
    if (dst_slot >= GPU_RESOURCE_POOL_SIZE || src_slot >= GPU_RESOURCE_POOL_SIZE) return false;
    if (!_state.v2_memory[dst_slot] || !_state.v2_memory[src_slot] || size == 0) return false;
    if (dst_offset > _state.v2_memory_size[dst_slot] || size > _state.v2_memory_size[dst_slot] - dst_offset) return false;
    if (src_offset > _state.v2_memory_size[src_slot] || size > _state.v2_memory_size[src_slot] - src_offset) return false;
    if (_state.cmd_encoder) return false;
    mtl_v2_ensure_frame();
    if (_state.cmd_buffer == nil) return false;
    if (_state.blit_encoder == nil) {
        _state.blit_encoder = [_state.cmd_buffer blitCommandEncoder];
        mtl_v2_label_encoder(_state.blit_encoder, "gpu write");
    }
    [_state.blit_encoder copyFromBuffer:_state.v2_memory[src_slot]
                           sourceOffset:(NSUInteger)src_offset
                               toBuffer:_state.v2_memory[dst_slot]
                      destinationOffset:(NSUInteger)dst_offset
                                   size:(NSUInteger)size];
    return true;
}

static void mtl_v2_mem_copy_end(void) {
    if (_state.blit_encoder == nil) return;
    [_state.blit_encoder endEncoding];
    _state.blit_encoder = nil;
}

static bool mtl_block_compressed(gpu_pixel_format format) {
    return format >= PIXELFORMAT_BC1_RGBA && format <= PIXELFORMAT_ASTC_6x6_SRGBA;
}

// Whether this GPU samples block-compressed `format`: ASTC from the Apple2
// family on, ETC2 and EAC on every Apple GPU, BC where the device says so
// (every Mac; an iPad from iOS 16.4 when its GPU has it).
static bool mtl_block_format_supported(gpu_pixel_format format) {
    id<MTLDevice> device = _state.device.device;
    if (format >= PIXELFORMAT_ASTC_4x4_RGBA && format <= PIXELFORMAT_ASTC_6x6_SRGBA) {
        return [device supportsFamily:MTLGPUFamilyApple2];
    }
    if (format >= PIXELFORMAT_ETC2_RGB8 && format <= PIXELFORMAT_ETC2_RG11SN) {
        return [device supportsFamily:MTLGPUFamilyApple1];
    }
    if (format >= PIXELFORMAT_BC1_RGBA && format <= PIXELFORMAT_BC7_SRGBA) {
        if (@available(macOS 11.0, iOS 16.4, *)) return device.supportsBCTextureCompression;
        return TARGET_OS_OSX;
    }
    return false;
}

static u32 mtl_v2_texture_create(i32 width, i32 height, i32 depth_or_layers,
                                 i32 format, u32 usage, i32 mip_count, i32 kind, const char *name) {
    if (width <= 0 || height <= 0 || depth_or_layers <= 0 ||
        _state.texture_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    MTLPixelFormat pixel_format = _mtl_pixel_format((gpu_pixel_format)format);
    if (pixel_format == MTLPixelFormatInvalid) return 0;
    bool block_compressed = mtl_block_compressed((gpu_pixel_format)format);
    if (block_compressed && !mtl_block_format_supported((gpu_pixel_format)format)) return 0;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor new];
    desc.textureType = _mtl_texture_type((gpu_texture_type)kind);
    desc.pixelFormat = pixel_format;
    desc.width = (NSUInteger)width;
    desc.height = (NSUInteger)height;
    desc.mipmapLevelCount = (NSUInteger)(mip_count > 0 ? mip_count : 1);
    if (kind == TEXTURE_3D) desc.depth = (NSUInteger)depth_or_layers;
    else if (kind == TEXTURE_ARRAY) desc.arrayLength = (NSUInteger)depth_or_layers;
    else desc.arrayLength = 1;
    // A render-target-only texture has no contents to preserve after its pass.
    // Tile GPUs can therefore keep it entirely on-chip, avoiding the resolve
    // to system memory that a Store action would otherwise force.
    bool transient_render_target = (usage & TEXTURE_USAGE_RENDER_TARGET) &&
                                   !(usage & (TEXTURE_USAGE_READ | TEXTURE_USAGE_WRITE));
    // Simulator requires private storage for depth/stencil textures, including
    // attachments sampled by a later pass. Uncompressed color uploads stay
    // shared so replaceRegion can write them. Transient attachments can fall
    // back to private storage.
    bool depth_stencil = format == PIXELFORMAT_DEPTH || format == PIXELFORMAT_DEPTH16 || format == PIXELFORMAT_DEPTH_STENCIL;
    MTLResourceOptions resource_options = (depth_stencil || transient_render_target)
                                             ? MTLResourceStorageModePrivate
                                             : MTLResourceStorageModeShared;
    if (transient_render_target) {
#if TARGET_OS_SIMULATOR
        // Simulator does not provide tile memory, even on an Apple GPU host.
#elif TARGET_OS_IOS || TARGET_OS_TV || (defined(TARGET_OS_VISION) && TARGET_OS_VISION)
        resource_options = MTLResourceStorageModeMemoryless;
#elif TARGET_OS_OSX
        if (@available(macOS 11.0, *)) {
            if ([_state.device.device supportsFamily:MTLGPUFamilyApple1]) {
                resource_options = MTLResourceStorageModeMemoryless;
            }
        }
#endif
    }
    // Block-compressed pictures live in private storage on every Apple GPU,
    // filled by a blit (mtl_v2_texture_upload_private). The GPU lays the
    // blocks out its own way there, which a CPU replaceRegion into a shared
    // texture does not reliably reproduce on iOS devices; using one path on
    // macOS too keeps it the path that is tested.
    if (block_compressed && !transient_render_target) resource_options = MTLResourceStorageModePrivate;
    // A texture the GPU renders or writes is its own to fill: shared storage
    // only costs bandwidth there, and the Metal validation layer flags it. A
    // CPU upload into one still lands through the blit.
    if ((usage & TEXTURE_USAGE_WRITE) || ((usage & TEXTURE_USAGE_RENDER_TARGET) && !transient_render_target)) {
        resource_options = MTLResourceStorageModePrivate;
    }
    desc.resourceOptions = resource_options;
    desc.usage = 0;
    if (usage == TEXTURE_USAGE_DEFAULT || (usage & TEXTURE_USAGE_READ)) desc.usage |= MTLTextureUsageShaderRead;
    if (usage & TEXTURE_USAGE_WRITE) desc.usage |= MTLTextureUsageShaderWrite;
    if (usage & TEXTURE_USAGE_RENDER_TARGET) desc.usage |= MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [_state.device.device newTextureWithDescriptor:desc];
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!texture) return 0;
    texture.label = [NSString stringWithUTF8String:name];
    u32 id = _state.texture_count++;
    _state.textures[id] = (gpu_texture_mtl){
        .texture = texture,
        .width = (NSUInteger)width,
        .height = (NSUInteger)height,
        .depth = (NSUInteger)depth_or_layers,
        .format = (gpu_pixel_format)format,
        .type = (gpu_texture_type)kind,
        .resource_options = resource_options,
        .transient_render_target = transient_render_target,
    };
    return id;
}

// Private storage is not CPU-visible. One shot command buffer copies the
// tightly packed blocks; the frame encoder stays untouched because
// gpu_mtl_end_frame requires it to be idle.
static void mtl_v2_texture_upload_private(gpu_texture_mtl *texture, i32 mip, i32 layer,
                                          const void *data, u64 size,
                                          NSUInteger width, NSUInteger height, NSUInteger depth,
                                          NSUInteger row, NSUInteger image) {
    u64 need = image == 0 ? size : (u64)image * (u64)depth;
    if (_state.cmd_queue == nil || _state.device.device == nil || size > (u64)NSUIntegerMax || size < need) {
        NSLog(@"ns: compressed texture upload skipped");
        return;
    }
    @autoreleasepool {
        id<MTLBuffer> staging = [_state.device.device newBufferWithBytes:data
                                                                   length:(NSUInteger)size
                                                                  options:MTLResourceStorageModeShared];
        if (!staging) {
            NSLog(@"ns: compressed texture upload failed to allocate %llu bytes", (unsigned long long)size);
            return;
        }
        id<MTLCommandBuffer> command = [_state.cmd_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        mtl_v2_label_encoder(blit, "compressed texture upload");
        [blit copyFromBuffer:staging
                sourceOffset:0
           sourceBytesPerRow:row
         sourceBytesPerImage:image
                  sourceSize:MTLSizeMake(width, height, depth)
                   toTexture:texture->texture
            destinationSlice:(NSUInteger)layer
            destinationLevel:(NSUInteger)mip
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.error) NSLog(@"ns: compressed texture upload failed: %@", command.error);
#ifndef ENABLE_ARC
        [staging release];
#endif
    }
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
    // A block-compressed format's row is a row of blocks and its image the
    // rows of blocks of one slice (gpu_pixel_format_row_pitch). A private
    // texture (every block-compressed one) is filled by a blit.
    if (texture->texture.storageMode == MTLStorageModePrivate) {
        mtl_v2_texture_upload_private(texture, mip, layer, data, size, width, height, depth, row, image);
        return;
    }
    MTLRegion region = texture->type == TEXTURE_3D
                           ? MTLRegionMake3D(0, 0, 0, width, height, depth)
                           : MTLRegionMake2D(0, 0, width, height);
    // bytesPerImage is the distance between slices of a 3D texture. Metal
    // requires 0 for every other type; `slice` already selects that image.
    // An iOS device applies a nonzero value to block-compressed formats as a
    // pixel-row stride, so a tightly packed ASTC or BC surface is stored torn
    // or blank.
    if (texture->type != TEXTURE_3D) image = 0;
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

// Metal's shader-validation device (MTL_SHADER_VALIDATION=1, or Xcode's
// Shader Validation option) crashes inside newBinaryArchiveWithDescriptor
// when it loads an archive that already holds pipelines. The archives are only
// a compile cache, so a validated run compiles every pipeline instead.
static bool mtl_shader_validation(void) {
    static int validation = -1;
    if (validation < 0) {
        const char *env = getenv("MTL_SHADER_VALIDATION");
        NSString *device = _state.device.device ? NSStringFromClass([_state.device.device class]) : nil;
        validation = (env && env[0] && strcmp(env, "0") != 0) ||
                     (device && [device rangeOfString:@"GPUDebug"].location != NSNotFound);
    }
    return validation == 1;
}

// Open the shader's archive, loading the entry storage already holds. A cache
// that cannot be read is not an error: the archive starts empty and the
// pipelines it gains are serialized over the unusable entry.
static void mtl_shader_cache_open(gpu_shader_mtl *record, const char *name, u64 hash) {
    record->cache_name[0] = '\0';
    record->cache_hash = hash;
    record->archive = nil;
    if (!name || !name[0] || !mtl_cache_available() || mtl_shader_validation()) return;
    snprintf(record->cache_name, sizeof(record->cache_name), "%s", name);

    const char *path = _mtl_cache.path(record->cache_name, hash);
    if (!path || !path[0]) {
        // No app data directory: storage_init was never called.
        record->cache_name[0] = '\0';
        return;
    }
    NSString *entry = [NSString stringWithUTF8String:path];
    MTLBinaryArchiveDescriptor *desc = [MTLBinaryArchiveDescriptor new];
    if (entry && [[NSFileManager defaultManager] fileExistsAtPath:entry]) {
        desc.url = [NSURL fileURLWithPath:entry];
    }
    NSError *error = nil;
    record->archive = [_state.device.device newBinaryArchiveWithDescriptor:desc error:&error];
    if (!record->archive && desc.url) {
        // An archive written by another OS or GPU is rejected on load; start over.
        desc.url = nil;
        error = nil;
        record->archive = [_state.device.device newBinaryArchiveWithDescriptor:desc error:&error];
    }
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!record->archive) record->cache_name[0] = '\0';
}

// True the first time this run asks for the current render state, which is the
// only time the archive is worth consulting.
static bool mtl_archive_first_use(gpu_shader_mtl *shader, const gpu_mtl_pipeline_key *key_ref) {
    gpu_mtl_pipeline_key key = *key_ref;
    for (u32 i = 0; i < shader->archive_key_count; ++i) {
        if (memcmp(&shader->archive_keys[i], &key, sizeof(key)) == 0) return false;
    }
    if (shader->archive_key_count >= GPU_MTL_ARCHIVE_KEY_SLOTS) return false;
    shader->archive_keys[shader->archive_key_count++] = key;
    return true;
}

// Serialize the archive over its storage entry. MTLBinaryArchive refuses to
// write onto an existing file, so it writes beside the entry and storage moves
// the result into place.
static void mtl_shader_cache_store(gpu_shader_mtl *record) {
    if (!record->archive || !record->cache_name[0] || !mtl_cache_available()) return;
    const char *path = _mtl_cache.path(record->cache_name, record->cache_hash);
    if (!path || !path[0]) return;
    NSString *scratch = [[NSString stringWithUTF8String:path] stringByAppendingString:@".part"];
    if (!scratch) return;
    [[NSFileManager defaultManager] removeItemAtPath:scratch error:nil];
    NSError *error = nil;
    if (![record->archive serializeToURL:[NSURL fileURLWithPath:scratch] error:&error]) {
        [[NSFileManager defaultManager] removeItemAtPath:scratch error:nil];
        // A device that cannot serialize archives keeps working uncached.
        record->cache_name[0] = '\0';
        return;
    }
    if (!_mtl_cache.adopt(record->cache_name, record->cache_hash, [scratch UTF8String])) {
        record->cache_name[0] = '\0';
    }
}

static u32 mtl_v2_shader_graphics_create(const char *vs_src, const char *fs_src,
                                         const char *vs_entry, const char *fs_entry,
                                         const char *name, u64 hash) {
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
    mtl_shader_cache_open(record, name, hash);
    record->uses_root = mtl_source_uses(vs_src, "ns_root") || mtl_source_uses(fs_src, "ns_root");
    record->uses_read_texture = mtl_source_uses(vs_src, "ns_read_texture") || mtl_source_uses(fs_src, "ns_read_texture");
    record->uses_write_texture = mtl_source_uses(vs_src, "ns_write_texture") || mtl_source_uses(fs_src, "ns_write_texture");
    record->uses_write_texture_secondary = mtl_source_uses(vs_src, "ns_secondary_write_texture") || mtl_source_uses(fs_src, "ns_secondary_write_texture");
    record->uses_texture_map = mtl_source_uses(vs_src, "ns_texture_map") || mtl_source_uses(fs_src, "ns_texture_map");
    record->uses_mask_map = mtl_source_uses(vs_src, "ns_mask_map") || mtl_source_uses(fs_src, "ns_mask_map");
    record->uses_texture_table = mtl_source_uses(fs_src, "ns_texture_table");
    record->uses_shadow_map = mtl_source_uses(vs_src, "ns_shadow_map") || mtl_source_uses(fs_src, "ns_shadow_map");
    return id;
}

static u32 mtl_v2_shader_compute_create(const char *src, const char *entry,
                                        const char *name, u64 hash) {
    if (_state.shader_count >= GPU_RESOURCE_POOL_SIZE) return 0;
    id<MTLLibrary> library = _mtl_library_from_code(ns_str_cstr(src));
    if (!library) return 0;
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:entry]];
    gpu_shader_mtl record = {0};
    mtl_shader_cache_open(&record, name, hash);

    id<MTLComputePipelineState> pipeline = nil;
    NSError *error = nil;
    if (function) {
        MTLComputePipelineDescriptor *desc = [MTLComputePipelineDescriptor new];
        desc.computeFunction = function;
        if (record.archive) {
            // Ask the archive alone first: a hit is the whole point of the
            // cache, and a miss has to be told apart from it so the compiled
            // pipeline can be recorded.
            desc.binaryArchives = @[record.archive];
            pipeline = [_state.device.device newComputePipelineStateWithDescriptor:desc
                                                                           options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                        reflection:NULL
                                                                             error:&error];
        }
        if (!pipeline) {
            error = nil;
            pipeline = [_state.device.device newComputePipelineStateWithDescriptor:desc
                                                                           options:MTLPipelineOptionNone
                                                                        reflection:NULL
                                                                             error:&error];
            if (pipeline && record.archive &&
                [record.archive addComputePipelineFunctionsWithDescriptor:desc error:nil]) {
                mtl_shader_cache_store(&record);
            }
        }
#ifndef ENABLE_ARC
        [desc release];
#endif
    }
    if (!pipeline) {
        NSLog(@"Failed to create v2 compute shader: %@", error);
#ifndef ENABLE_ARC
        [record.archive release];
        [function release];
        [library release];
#endif
        return 0;
    }
    u32 id = _state.shader_count++;
    record.compute_lib = library;
    record.compute_func = function;
    record.compute_pso = pipeline;
    record.uses_root = mtl_source_uses(src, "ns_root");
    record.uses_read_texture = mtl_source_uses(src, "ns_read_texture");
    record.uses_write_texture = mtl_source_uses(src, "ns_write_texture");
    record.uses_write_texture_secondary = mtl_source_uses(src, "ns_secondary_write_texture");
    _state.shaders[id] = record;
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
    for (u32 i = 0; i < record->v2_pso_count; ++i) [record->v2_psos[i].pso release];
    [record->archive release];
#endif
    memset(record, 0, sizeof(*record));
}

static void mtl_v2_ensure_frame(void) {
    if (_state.cmd_buffer == nil) gpu_mtl_begin_cmd_buffer();
}

// A pass label reaches the capture through the encoder name: Metal's frame
// debugger lists encoders by label, so naming them turns the capture navigator
// into the render graph.
static void mtl_v2_label_encoder(id<MTLCommandEncoder> encoder, const char *label) {
    if (encoder && label && label[0]) encoder.label = [NSString stringWithUTF8String:label];
}

static void mtl_v2_pass_begin(const char *label,
                              u32 color0, u32 color1, u32 color2, u32 color3,
                              u32 depth, u32 load_flags, gpu_color clear, f32 depth_clear) {
    mtl_v2_ensure_frame();
    if (_state.blit_encoder) mtl_v2_mem_copy_end();
    if (_state.cmd_buffer == nil || _state.cmd_encoder != nil) return;
    u32 colors[4] = {color0, color1, color2, color3};
    MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
    memset(_state.v2_pass_colors, 0, sizeof(_state.v2_pass_colors));
    _state.v2_pass_depth = MTLPixelFormatInvalid;
    _state.v2_pass_stencil = MTLPixelFormatInvalid;
    _state.v2_pass_depth_test = true;
    for (u32 i = 0; i < 4; ++i) {
        if (!colors[i] || colors[i] >= _state.texture_count) continue;
        gpu_texture_mtl *texture = &_state.textures[colors[i]];
        desc.colorAttachments[i].texture = texture->texture;
        desc.colorAttachments[i].storeAction = texture->transient_render_target
                                                   ? MTLStoreActionDontCare
                                                   : MTLStoreActionStore;
        desc.colorAttachments[i].loadAction = _mtl_load_action((gpu_load_action)((load_flags >> (i * 2)) & 3));
        desc.colorAttachments[i].clearColor = MTLClearColorMake(clear.r, clear.g, clear.b, clear.a);
        _state.v2_pass_colors[i] = _mtl_pixel_format(texture->format);
    }
    if (depth && depth < _state.texture_count) {
        gpu_texture_mtl *texture = &_state.textures[depth];
        desc.depthAttachment.texture = texture->texture;
        desc.depthAttachment.storeAction = texture->transient_render_target
                                               ? MTLStoreActionDontCare
                                               : MTLStoreActionStore;
        desc.depthAttachment.loadAction = _mtl_load_action((gpu_load_action)((load_flags >> 8) & 3));
        desc.depthAttachment.clearDepth = depth_clear;
        _state.v2_pass_depth = _mtl_pixel_format(texture->format);
    }
    _state.cmd_encoder = [_state.cmd_buffer renderCommandEncoderWithDescriptor:desc];
    mtl_v2_label_encoder(_state.cmd_encoder, label);
}

static id<MTLTexture> mtl_v2_screen_depth_stencil(NSUInteger width, NSUInteger height) {
    id<MTLTexture> target = _state.screen_depth_stencil;
    if (target && target.width == width && target.height == height) return target;
#ifndef ENABLE_ARC
    [_state.screen_depth_stencil release];
#endif
    _state.screen_depth_stencil = nil;
    if (width == 0 || height == 0) return nil;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    _state.screen_depth_stencil = [_state.device.device newTextureWithDescriptor:desc];
    _state.screen_depth_stencil.label = @"screen depth stencil";
    return _state.screen_depth_stencil;
}

static void mtl_v2_screen_pass_begin(const char *label, gpu_color clear, ns_bool depth, f32 depth_clear) {
    mtl_v2_ensure_frame();
    if (_state.blit_encoder) mtl_v2_mem_copy_end();
    if (_state.cmd_buffer == nil || _state.cmd_encoder != nil) return;
    id<CAMetalDrawable> drawable = immersive_frame ? nil : gpu_mtl_current_drawable();
    id<MTLTexture> screen = immersive_frame ? immersive_color : drawable.texture;
    if (screen == nil) return;
    MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
    desc.colorAttachments[0].texture = screen;
    desc.colorAttachments[0].slice = immersive_frame ? immersive_slice : 0;
    desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    desc.colorAttachments[0].loadAction = _state.screen_pass_count++ == 0 ? MTLLoadActionClear : MTLLoadActionLoad;
    desc.colorAttachments[0].clearColor = MTLClearColorMake(clear.r, clear.g, clear.b, clear.a);
    memset(_state.v2_pass_colors, 0, sizeof(_state.v2_pass_colors));
    _state.v2_pass_colors[0] = screen.pixelFormat;
    _state.v2_pass_depth = MTLPixelFormatInvalid;
    _state.v2_pass_stencil = MTLPixelFormatInvalid;
    _state.v2_pass_depth_test = depth;
    // The immersive eye targets come from the compositor with their own depth
    // and no stencil plane, so stencil states draw unmasked there.
    id<MTLTexture> target = immersive_frame ? nil : mtl_v2_screen_depth_stencil(screen.width, screen.height);
    if (target) {
        bool first = _state.screen_pass_count == 1;
        desc.stencilAttachment.texture = target;
        desc.stencilAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
        desc.stencilAttachment.storeAction = MTLStoreActionStore;
        desc.stencilAttachment.clearStencil = 0;
        desc.depthAttachment.texture = target;
        desc.depthAttachment.storeAction = MTLStoreActionStore;
        desc.depthAttachment.clearDepth = depth_clear;
        if (depth && !_state.screen_depth_cleared) {
            desc.depthAttachment.loadAction = MTLLoadActionClear;
            _state.screen_depth_cleared = true;
        } else {
            desc.depthAttachment.loadAction = first ? MTLLoadActionDontCare : MTLLoadActionLoad;
        }
        _state.v2_pass_stencil = target.pixelFormat;
        _state.v2_pass_depth = target.pixelFormat;
    }
    if (immersive_depth) {
        _state.v2_pass_depth_test = true;
        desc.depthAttachment.texture = immersive_depth;
        desc.depthAttachment.slice = immersive_slice;
        desc.depthAttachment.loadAction = _state.screen_pass_count == 1 ? MTLLoadActionClear : MTLLoadActionLoad;
        desc.depthAttachment.storeAction = MTLStoreActionStore;
        desc.depthAttachment.clearDepth = 0.0;
        _state.v2_pass_depth = immersive_depth.pixelFormat;
    }
    _state.cmd_encoder = [_state.cmd_buffer renderCommandEncoderWithDescriptor:desc];
    mtl_v2_label_encoder(_state.cmd_encoder, label);
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

static void mtl_v2_set_stencil_ref(u32 ref) {
    _state.v2_stencil_ref = ref;
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

static void mtl_v2_set_texture_table(const u32 *textures) {
    memcpy(_state.v2_texture_table, textures, sizeof(_state.v2_texture_table));
}

// The table follows the three fixed fragment textures (shadow, map, mask).
#define GPU_MTL_TEXTURE_TABLE_BASE 3

static void mtl_v2_bind_texture_table(id<MTLRenderCommandEncoder> encoder) {
    id<MTLTexture> fallback = nil;
    for (u32 i = 0; i < GPU_TEXTURE_TABLE_SIZE && !fallback; ++i) {
        u32 index = _state.v2_texture_table[i];
        if (index && index < _state.texture_count) fallback = _state.textures[index].texture;
    }
    if (!fallback) return;
    id<MTLTexture> table[GPU_TEXTURE_TABLE_SIZE];
    for (u32 i = 0; i < GPU_TEXTURE_TABLE_SIZE; ++i) {
        u32 index = _state.v2_texture_table[i];
        id<MTLTexture> texture = index && index < _state.texture_count ? _state.textures[index].texture : nil;
        table[i] = texture ? texture : fallback;
    }
    [encoder setFragmentTextures:table withRange:NSMakeRange(GPU_MTL_TEXTURE_TABLE_BASE, GPU_TEXTURE_TABLE_SIZE)];
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
        if (shader->uses_texture_table) mtl_v2_bind_texture_table(render_encoder);
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

// The pipeline for the current render state and pass formats, built on the
// shader's first draw under them and kept for later ones; nil on failure.
static id<MTLRenderPipelineState> mtl_v2_ensure_pipeline(gpu_shader_mtl *shader) {
    if (!shader || !shader->vertex_func || !shader->fragment_func) return nil;
    // Depth and stencil test, depth write and stencil op live in the
    // depth-stencil state, not the pipeline: states that differ only there
    // share one PSO.
    gpu_mtl_pipeline_key key;
    memset(&key, 0, sizeof(key));
    key.state = _state.v2_render_state;
    key.state.depth_compare = 0;
    key.state.depth_write = false;
    key.state.stencil_compare = 0;
    key.state.stencil_pass_op = 0;
    memcpy(key.colors, _state.v2_pass_colors, sizeof(key.colors));
    key.depth = _state.v2_pass_depth;
    key.stencil = _state.v2_pass_stencil;
    for (u32 i = 0; i < shader->v2_pso_count; ++i) {
        if (memcmp(&shader->v2_psos[i].key, &key, sizeof(key)) == 0) return shader->v2_psos[i].pso;
    }
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
    desc.stencilAttachmentPixelFormat = _state.v2_pass_stencil;
    NSError *error = nil;
    id<MTLRenderPipelineState> pso = nil;
    if (shader->archive && mtl_archive_first_use(shader, &key)) {
        // Ask the archive alone first: a hit skips the compile, and a miss has
        // to be told apart from it so the compiled pipeline can be recorded.
        desc.binaryArchives = @[shader->archive];
        pso = [_state.device.device newRenderPipelineStateWithDescriptor:desc
                                                                 options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                              reflection:NULL
                                                                   error:&error];
        if (!pso) {
            error = nil;
            pso = [_state.device.device newRenderPipelineStateWithDescriptor:desc error:&error];
            // Each distinct render state is its own archive entry, so a shader
            // drawn under several states grows its entry until every
            // combination the program uses is on disk.
            if (pso && [shader->archive addRenderPipelineFunctionsWithDescriptor:desc error:nil]) {
                mtl_shader_cache_store(shader);
            }
        }
    }
    if (!pso) {
        error = nil;
        desc.binaryArchives = nil;
        pso = [_state.device.device newRenderPipelineStateWithDescriptor:desc error:&error];
    }
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!pso) {
        NSLog(@"Failed to create v2 render pipeline: %@", error);
        return nil;
    }
    u32 slot = shader->v2_pso_count;
    if (slot < GPU_MTL_PSO_SLOTS) {
        shader->v2_pso_count++;
    } else {
        slot = shader->v2_pso_next;
        shader->v2_pso_next = (slot + 1) % GPU_MTL_PSO_SLOTS;
#ifndef ENABLE_ARC
        [shader->v2_psos[slot].pso release];
#endif
    }
    shader->v2_psos[slot].key = key;
    shader->v2_psos[slot].pso = pso;
    return pso;
}

// The depth-stencil state for the current render state and pass. Without a
// depth or stencil attachment there is nothing to configure; with one, every
// draw sets a state, so a stencil-writing draw never leaks into the next.
static id<MTLDepthStencilState> mtl_v2_depth_stencil_state(void) {
    bool depth = _state.v2_pass_depth != MTLPixelFormatInvalid && _state.v2_pass_depth_test;
    bool stencil = _state.v2_pass_stencil != MTLPixelFormatInvalid &&
                   gpu_v2_stencil_enabled(&_state.v2_render_state);
    if (_state.v2_pass_depth == MTLPixelFormatInvalid && _state.v2_pass_stencil == MTLPixelFormatInvalid) return nil;
    gpu_mtl_dso_entry key = {
        .depth_compare = depth ? _state.v2_render_state.depth_compare : COMPARE_ALWAYS,
        .depth_write = depth && _state.v2_render_state.depth_write,
        .stencil_compare = stencil ? _state.v2_render_state.stencil_compare : COMPARE_ALWAYS,
        .stencil_pass_op = stencil ? _state.v2_render_state.stencil_pass_op : STENCIL_OP_KEEP,
    };
    for (u32 i = 0; i < _state.dso_count; ++i) {
        gpu_mtl_dso_entry *entry = &_state.dso_cache[i];
        if (entry->depth_compare == key.depth_compare && entry->depth_write == key.depth_write &&
            entry->stencil_compare == key.stencil_compare && entry->stencil_pass_op == key.stencil_pass_op) {
            return entry->dso;
        }
    }
    if (_state.dso_count >= GPU_MTL_DSO_CACHE_SIZE) {
        NSLog(@"gpu: depth-stencil state cache exhausted");
        return nil;
    }
    MTLDepthStencilDescriptor *desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = _mtl_compare_function((gpu_compare_func)key.depth_compare);
    desc.depthWriteEnabled = key.depth_write ? YES : NO;
    if (stencil) {
        MTLStencilDescriptor *face = [MTLStencilDescriptor new];
        face.stencilCompareFunction = _mtl_compare_function((gpu_compare_func)key.stencil_compare);
        face.stencilFailureOperation = MTLStencilOperationKeep;
        face.depthFailureOperation = MTLStencilOperationKeep;
        face.depthStencilPassOperation = _mtl_stencil_op((gpu_stencil_op)key.stencil_pass_op);
        face.readMask = 0xff;
        face.writeMask = 0xff;
        desc.frontFaceStencil = face;
        desc.backFaceStencil = face;
#ifndef ENABLE_ARC
        [face release];
#endif
    }
    key.dso = [_state.device.device newDepthStencilStateWithDescriptor:desc];
#ifndef ENABLE_ARC
    [desc release];
#endif
    if (!key.dso) return nil;
    _state.dso_cache[_state.dso_count++] = key;
    return key.dso;
}

static gpu_shader_mtl *mtl_v2_prepare_draw(void) {
    if (!_state.cmd_encoder || !_state.v2_shader || _state.v2_shader >= _state.shader_count) return NULL;
    gpu_shader_mtl *shader = &_state.shaders[_state.v2_shader];
    id<MTLRenderPipelineState> pso = mtl_v2_ensure_pipeline(shader);
    if (!pso) return NULL;
    [_state.cmd_encoder setRenderPipelineState:pso];
    [_state.cmd_encoder setCullMode:_mtl_cull_mode((gpu_cull_mode)_state.v2_render_state.cull_mode)];
    [_state.cmd_encoder setFrontFacingWinding:_mtl_winding((gpu_face_winding)_state.v2_render_state.face_winding)];
    id<MTLDepthStencilState> depth_stencil = mtl_v2_depth_stencil_state();
    if (depth_stencil) [_state.cmd_encoder setDepthStencilState:depth_stencil];
    if (_state.v2_pass_stencil != MTLPixelFormatInvalid) [_state.cmd_encoder setStencilReferenceValue:_state.v2_stencil_ref];
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

// Threads a dispatch aims to put in one threadgroup, matching the size the
// WGSL and HLSL backends declare for the same kernels.
#define GPU_MTL_THREADGROUP_THREADS 64

// Grow groups across the grid's axes toward whole SIMD groups. Every axis
// must divide the grid exactly: dispatchThreadgroups works on simulators and
// devices without nonuniform dispatch support, but cannot trim edge groups.
// Kernels may omit bounds checks, so padding would execute unwanted threads.
static MTLSize mtl_v2_threadgroup_size(id<MTLComputePipelineState> pso, i32 x, i32 y, i32 z) {
    NSUInteger lanes = MAX((NSUInteger)1, pso.threadExecutionWidth);
    NSUInteger budget = MAX((NSUInteger)1, pso.maxTotalThreadsPerThreadgroup);
    NSUInteger total = ((GPU_MTL_THREADGROUP_THREADS + lanes - 1) / lanes) * lanes;
    if (total > budget) total = budget;
    NSUInteger grid[3] = {
        (NSUInteger)MAX(x, 1), (NSUInteger)MAX(y, 1), (NSUInteger)MAX(z, 1)
    };
    NSUInteger group[3] = {1, 1, 1};
    NSUInteger threads = 1;
    ns_bool grew = true;
    while (grew && threads < total) {
        grew = false;
        for (i32 axis = 0; axis < 3; ++axis) {
            if (threads * 2 > total) break;
            if (grid[axis] % (group[axis] * 2) != 0) continue;
            group[axis] *= 2;
            threads *= 2;
            grew = true;
        }
    }
    return MTLSizeMake(group[0], group[1], group[2]);
}

static void mtl_v2_dispatch(const char *label, i32 x, i32 y, i32 z) {
    if (x <= 0 || y <= 0 || z <= 0) return;
    if (!_state.v2_shader || _state.v2_shader >= _state.shader_count) return;
    gpu_shader_mtl *shader = &_state.shaders[_state.v2_shader];
    if (!shader->compute_pso) return;
    mtl_v2_ensure_frame();
    if (_state.blit_encoder) mtl_v2_mem_copy_end();
    if (_state.cmd_buffer == nil) return;
    id<MTLComputeCommandEncoder> encoder = [_state.cmd_buffer computeCommandEncoder];
    mtl_v2_label_encoder(encoder, label);
    [encoder setComputePipelineState:shader->compute_pso];
    mtl_v2_bind_root(shader, encoder, true);
    MTLSize group = mtl_v2_threadgroup_size(shader->compute_pso, x, y, z);
    MTLSize count = MTLSizeMake((NSUInteger)x / group.width,
                              (NSUInteger)y / group.height,
                              (NSUInteger)z / group.depth);
    [encoder dispatchThreadgroups:count threadsPerThreadgroup:group];
    [encoder endEncoding];
}

static void mtl_v2_dispatch_indirect(const char *label, u32 slot, u64 offset) {
    if (slot >= GPU_RESOURCE_POOL_SIZE || !_state.v2_memory[slot] ||
        !_state.v2_shader || _state.v2_shader >= _state.shader_count) return;
    gpu_shader_mtl *shader = &_state.shaders[_state.v2_shader];
    if (!shader->compute_pso) return;
    mtl_v2_ensure_frame();
    if (_state.blit_encoder) mtl_v2_mem_copy_end();
    id<MTLComputeCommandEncoder> encoder = [_state.cmd_buffer computeCommandEncoder];
    mtl_v2_label_encoder(encoder, label);
    [encoder setComputePipelineState:shader->compute_pso];
    mtl_v2_bind_root(shader, encoder, true);
    [encoder dispatchThreadgroupsWithIndirectBuffer:_state.v2_memory[slot]
                              indirectBufferOffset:(NSUInteger)offset
                             threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    [encoder endEncoding];
}

// MetalFX is loaded only by the feature dylib. The interpreter and generated
// projects do not gain a mandatory framework dependency; older OSes fall back.
#if __has_include(<MetalFX/MetalFX.h>) && !(defined(TARGET_OS_VISION) && TARGET_OS_VISION)
#import <MetalFX/MetalFX.h>
#include <math.h>
#define GPU_MTL_FX_COUNT 64

typedef struct {
    NSUInteger width, height;
    MTLPixelFormat format;
    MTLTextureUsage usage;
} gpu_mtl_fx_texture;
typedef struct {
    u32 handle;
    i32 kind;
    id effect;
    gpu_mtl_fx_texture color, depth, motion, output;
    bool history;
} gpu_mtl_fx;
static gpu_mtl_fx mtl_fx[GPU_MTL_FX_COUNT];
static u32 mtl_fx_serial;

static Class mtl_fx_class(i32 kind) {
    static void *framework;
    if (!framework) framework = dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY | RTLD_LOCAL);
    if (!framework) return Nil;
    switch (kind) {
        case GPU_FX_SPATIAL: return NSClassFromString(@"MTLFXSpatialScalerDescriptor");
        case GPU_FX_TEMPORAL: return NSClassFromString(@"MTLFXTemporalScalerDescriptor");
        case GPU_FX_FRAME_INTERPOLATION: return NSClassFromString(@"MTLFXFrameInterpolatorDescriptor");
        default: return Nil;
    }
}

static ns_bool mtl_v2_fx_supported(i32 kind) {
    if (!_state.valid || !_state.device.device) return false;
    Class cls = mtl_fx_class(kind);
    if (!cls || ![cls respondsToSelector:@selector(supportsDevice:)]) return false;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        return [cls supportsDevice:_state.device.device];
    }
    return false;
}

static id<MTLTexture> mtl_fx_texture(u32 index) {
    if (!index || index >= _state.texture_count) return nil;
    id<MTLTexture> texture = _state.textures[index].texture;
    if (texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
        texture.storageMode == MTLStorageModeMemoryless) return nil;
    return texture;
}

static bool mtl_fx_shape(id<MTLTexture> a, id<MTLTexture> b) {
    return a && b && a.width == b.width && a.height == b.height;
}
static gpu_mtl_fx_texture mtl_fx_key(id<MTLTexture> t, MTLTextureUsage usage) {
    return (gpu_mtl_fx_texture){t.width, t.height, t.pixelFormat, usage};
}
static bool mtl_fx_match(id<MTLTexture> t, gpu_mtl_fx_texture key) {
    return t && t.width == key.width && t.height == key.height && t.pixelFormat == key.format &&
           (t.usage & key.usage) == key.usage;
}
static bool mtl_fx_usage(id<MTLTexture> t, MTLTextureUsage usage) {
    return t && (t.usage & usage) == usage;
}

static u32 mtl_v2_fx_create(i32 kind, u32 color, u32 depth, u32 motion, u32 output) {
    if (!mtl_v2_fx_supported(kind)) return 0;
    id<MTLTexture> c = mtl_fx_texture(color), o = mtl_fx_texture(output);
    id<MTLTexture> d = mtl_fx_texture(depth), m = mtl_fx_texture(motion);
    if (!c || !o || c == o || o.storageMode != MTLStorageModePrivate ||
        o.width < c.width || o.height < c.height) return 0;
    if (kind == GPU_FX_FRAME_INTERPOLATION && !mtl_fx_shape(c, o)) return 0;
    if (kind == GPU_FX_TEMPORAL && !mtl_fx_shape(c, d)) return 0;
    if (kind != GPU_FX_SPATIAL && (!d || !m || !mtl_fx_shape(d, m) ||
        d.width > c.width || d.height > c.height ||
        (m.pixelFormat != MTLPixelFormatRG16Float && m.pixelFormat != MTLPixelFormatRG32Float) ||
        (d.pixelFormat != MTLPixelFormatDepth32Float && d.pixelFormat != MTLPixelFormatR32Float))) return 0;
    gpu_mtl_fx *slot = NULL;
    for (u32 i = 0; i < GPU_MTL_FX_COUNT; i++) if (!mtl_fx[i].handle) { slot = &mtl_fx[i]; break; }
    if (!slot || mtl_fx_serial == UINT32_MAX) return 0;
    id effect = nil;
    MTLTextureUsage cu = 0, du = 0, mu = 0, ou = 0;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        if (kind == GPU_FX_SPATIAL) {
            MTLFXSpatialScalerDescriptor *desc = [mtl_fx_class(kind) new];
            desc.inputWidth = c.width; desc.inputHeight = c.height;
            desc.outputWidth = o.width; desc.outputHeight = o.height;
            desc.colorTextureFormat = c.pixelFormat; desc.outputTextureFormat = o.pixelFormat;
            desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeHDR;
            id<MTLFXSpatialScaler> fx = [desc newSpatialScalerWithDevice:_state.device.device];
            effect = fx;
            cu = fx.colorTextureUsage; ou = fx.outputTextureUsage;
#ifndef ENABLE_ARC
            [desc release];
#endif
        } else if (kind == GPU_FX_TEMPORAL) {
            MTLFXTemporalScalerDescriptor *desc = [mtl_fx_class(kind) new];
            desc.inputWidth = c.width; desc.inputHeight = c.height;
            desc.outputWidth = o.width; desc.outputHeight = o.height;
            desc.colorTextureFormat = c.pixelFormat; desc.outputTextureFormat = o.pixelFormat;
            desc.depthTextureFormat = d.pixelFormat; desc.motionTextureFormat = m.pixelFormat;
            desc.autoExposureEnabled = YES;
            id<MTLFXTemporalScaler> fx = [desc newTemporalScalerWithDevice:_state.device.device];
            effect = fx;
            cu = fx.colorTextureUsage; du = fx.depthTextureUsage; mu = fx.motionTextureUsage; ou = fx.outputTextureUsage;
#ifndef ENABLE_ARC
            [desc release];
#endif
        }
    }
#if __has_include(<MetalFX/MTLFXFrameInterpolator.h>)
    if (@available(macOS 26.0, iOS 26.0, *)) {
        if (kind == GPU_FX_FRAME_INTERPOLATION) {
            MTLFXFrameInterpolatorDescriptor *desc = [mtl_fx_class(kind) new];
            desc.inputWidth = d.width; desc.inputHeight = d.height;
            desc.outputWidth = o.width; desc.outputHeight = o.height;
            desc.colorTextureFormat = c.pixelFormat; desc.outputTextureFormat = o.pixelFormat;
            desc.depthTextureFormat = d.pixelFormat; desc.motionTextureFormat = m.pixelFormat;
            id<MTLFXFrameInterpolator> fx = [desc newFrameInterpolatorWithDevice:_state.device.device];
            effect = fx;
            cu = fx.colorTextureUsage; du = fx.depthTextureUsage; mu = fx.motionTextureUsage; ou = fx.outputTextureUsage;
#ifndef ENABLE_ARC
            [desc release];
#endif
        }
    }
#endif
    if (!effect || !mtl_fx_usage(c, cu) || !mtl_fx_usage(o, ou) ||
        (kind != GPU_FX_SPATIAL && (!mtl_fx_usage(d, du) || !mtl_fx_usage(m, mu)))) {
#ifndef ENABLE_ARC
        [effect release];
#endif
        return 0;
    }
    // Keep scalar keys: encodes may replace all bound textures, and destroying
    // an input must not leave a dangling template pointer in this record.
    *slot = (gpu_mtl_fx){ .handle = ++mtl_fx_serial, .kind = kind, .effect = effect,
                         .color = mtl_fx_key(c, cu), .depth = mtl_fx_key(d, du),
                         .motion = mtl_fx_key(m, mu), .output = mtl_fx_key(o, ou) };
    return slot->handle;
}

static void mtl_v2_fx_destroy(u32 handle) {
    for (u32 i = 0; i < GPU_MTL_FX_COUNT; i++) if (mtl_fx[i].handle == handle) {
#ifndef ENABLE_ARC
        [mtl_fx[i].effect release];
#endif
        memset(&mtl_fx[i], 0, sizeof(mtl_fx[i]));
        return;
    }
}
static void mtl_v2_fx_destroy_all(void) {
    for (u32 i = 0; i < GPU_MTL_FX_COUNT; i++) if (mtl_fx[i].handle) mtl_v2_fx_destroy(mtl_fx[i].handle);
}

static ns_bool mtl_v2_fx_encode(u32 handle, u32 color, u32 depth, u32 motion,
                               u32 previous, u32 output, const gpu_fx_frame *p) {
    gpu_mtl_fx *slot = NULL;
    for (u32 i = 0; i < GPU_MTL_FX_COUNT; i++) if (mtl_fx[i].handle == handle) { slot = &mtl_fx[i]; break; }
    if (!slot || _state.cmd_encoder || !_state.valid) return false;
    id<MTLTexture> c = mtl_fx_texture(color), o = mtl_fx_texture(output);
    id<MTLTexture> d = mtl_fx_texture(depth), m = mtl_fx_texture(motion), prev = mtl_fx_texture(previous);
    // Honor the exact usage requirements returned by this effect instance.
    if (!mtl_fx_match(c, slot->color) ||
        !mtl_fx_match(o, slot->output) ||
        o.storageMode != MTLStorageModePrivate || c == o || o == d || o == m || o == prev) return false;
    if (slot->kind != GPU_FX_SPATIAL &&
        (!mtl_fx_match(d, slot->depth) ||
         !mtl_fx_match(m, slot->motion) ||
         !isfinite(p->jitter_x) || !isfinite(p->jitter_y) ||
         !isfinite(p->motion_scale_x) || !isfinite(p->motion_scale_y))) return false;
    if (slot->kind == GPU_FX_FRAME_INTERPOLATION &&
        (!mtl_fx_match(prev, slot->color) || prev == c ||
         !isfinite(p->delta_time) || p->delta_time <= 0 ||
         !isfinite(p->near_plane) || p->near_plane <= 0 ||
         !isfinite(p->far_plane) || p->far_plane <= p->near_plane ||
         !isfinite(p->field_of_view) || p->field_of_view <= 0 || p->field_of_view >= 180 ||
         !isfinite(p->aspect_ratio) || p->aspect_ratio <= 0)) return false;
    mtl_v2_ensure_frame();
    if (!_state.cmd_buffer) return false;
    if (_state.blit_encoder) mtl_v2_mem_copy_end();
    bool reset = p->reset || !slot->history;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        if (slot->kind == GPU_FX_SPATIAL) {
            id<MTLFXSpatialScaler> fx = slot->effect;
            fx.colorTexture = c; fx.outputTexture = o;
            fx.inputContentWidth = c.width; fx.inputContentHeight = c.height;
            [_state.cmd_buffer pushDebugGroup:@"MetalFX HDR spatial upscale"];
            [fx encodeToCommandBuffer:_state.cmd_buffer];
            [_state.cmd_buffer popDebugGroup];
        } else if (slot->kind == GPU_FX_TEMPORAL) {
            id<MTLFXTemporalScaler> fx = slot->effect;
            fx.colorTexture = c; fx.depthTexture = d; fx.motionTexture = m; fx.outputTexture = o;
            fx.inputContentWidth = c.width; fx.inputContentHeight = c.height;
            fx.jitterOffsetX = p->jitter_x; fx.jitterOffsetY = p->jitter_y;
            fx.motionVectorScaleX = p->motion_scale_x; fx.motionVectorScaleY = p->motion_scale_y;
            fx.depthReversed = YES; fx.reset = reset; fx.preExposure = 1.0f;
            [_state.cmd_buffer pushDebugGroup:@"MetalFX temporal upscale"];
            [fx encodeToCommandBuffer:_state.cmd_buffer];
            [_state.cmd_buffer popDebugGroup];
        }
    }
#if __has_include(<MetalFX/MTLFXFrameInterpolator.h>)
    if (@available(macOS 26.0, iOS 26.0, *)) {
        if (slot->kind == GPU_FX_FRAME_INTERPOLATION) {
            id<MTLFXFrameInterpolator> fx = slot->effect;
            fx.colorTexture = c; fx.prevColorTexture = prev; fx.outputTexture = o;
            fx.depthTexture = d; fx.motionTexture = m;
            fx.jitterOffsetX = p->jitter_x; fx.jitterOffsetY = p->jitter_y;
            fx.motionVectorScaleX = p->motion_scale_x; fx.motionVectorScaleY = p->motion_scale_y;
            fx.deltaTime = p->delta_time; fx.nearPlane = p->near_plane; fx.farPlane = p->far_plane;
            fx.fieldOfView = p->field_of_view; fx.aspectRatio = p->aspect_ratio;
            fx.depthReversed = YES; fx.shouldResetHistory = reset;
            [_state.cmd_buffer pushDebugGroup:@"MetalFX midpoint frame interpolation"];
            [fx encodeToCommandBuffer:_state.cmd_buffer];
            [_state.cmd_buffer popDebugGroup];
        }
    }
#endif
    slot->history = true;
    return true;
}
#else
static ns_bool mtl_v2_fx_supported(i32 kind) { ns_unused(kind); return false; }
static u32 mtl_v2_fx_create(i32 kind, u32 c, u32 d, u32 m, u32 o) { return 0; }
static ns_bool mtl_v2_fx_encode(u32 h, u32 c, u32 d, u32 m, u32 p, u32 o, const gpu_fx_frame *f) { return false; }
static void mtl_v2_fx_destroy(u32 handle) { ns_unused(handle); }
static void mtl_v2_fx_destroy_all(void) {}
#endif

static const gpu_v2_ops _mtl_v2_ops = {
    .fx_supported = mtl_v2_fx_supported,
    .fx_create = mtl_v2_fx_create,
    .fx_encode = mtl_v2_fx_encode,
    .fx_destroy = mtl_v2_fx_destroy,
    .mem_create = mtl_v2_mem_create,
    .mem_destroy = mtl_v2_mem_destroy,
    .mem_write = mtl_v2_mem_write,
    .mem_read = mtl_v2_mem_read,
    .mem_host_ptr = mtl_v2_mem_host_ptr,
    .mem_copy = mtl_v2_mem_copy,
    .mem_copy_end = mtl_v2_mem_copy_end,
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
    .set_stencil_ref = mtl_v2_set_stencil_ref,
    .set_root = mtl_v2_set_root,
    .set_storage = mtl_v2_set_storage,
    .set_texture_table = mtl_v2_set_texture_table,
    .draw = mtl_v2_draw,
    .draw_indexed = mtl_v2_draw_indexed,
    .draw_indirect = mtl_v2_draw_indirect,
    .dispatch = mtl_v2_dispatch,
    .dispatch_indirect = mtl_v2_dispatch_indirect,
};
