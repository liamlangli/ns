// Validate the real texture allocation path against simulator restrictions.
#include "../lib/src/gpu.metal.m"

@interface TextureDevice : NSObject
@property(strong) MTLTextureDescriptor *descriptor;
@end
@implementation TextureDevice
- (BOOL)supportsFamily:(MTLGPUFamily)family {
    (void)family;
    return NO;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    BOOL depth = descriptor.pixelFormat == MTLPixelFormatDepth32Float ||
                 descriptor.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (depth) assert(descriptor.storageMode == MTLStorageModePrivate);
    assert(descriptor.storageMode != MTLStorageModeMemoryless);
    self.descriptor = descriptor;
    // The allocation path only stores/releases the returned object.
    return (id<MTLTexture>)[NSObject new];
}
@end

int main(void) {
    @autoreleasepool {
        TextureDevice *device = [TextureDevice new];
        _state.device.device = (id<MTLDevice>)device;
        _state.texture_count = 1;
        const i32 formats[] = {PIXELFORMAT_DEPTH, PIXELFORMAT_DEPTH_STENCIL, PIXELFORMAT_RGBA8};
        const u32 usages[] = {TEXTURE_USAGE_DEFAULT, TEXTURE_USAGE_READ,
                             TEXTURE_USAGE_READ | TEXTURE_USAGE_RENDER_TARGET,
                             TEXTURE_USAGE_RENDER_TARGET};
        for (NSUInteger f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
            for (NSUInteger u = 0; u < sizeof(usages) / sizeof(usages[0]); ++u) {
                u32 tex = mtl_v2_texture_create(32, 16, 1, formats[f], usages[u], 1, TEXTURE_2D);
                assert(tex != 0);
                bool transient = usages[u] == TEXTURE_USAGE_RENDER_TARGET;
                assert(_state.textures[tex].transient_render_target == transient);
                assert(device.descriptor.width == 32 && device.descriptor.height == 16);
                MTLStorageMode expected = formats[f] != PIXELFORMAT_RGBA8 || transient
                                             ? MTLStorageModePrivate : MTLStorageModeShared;
                assert(device.descriptor.storageMode == expected);
                MTLTextureUsage expected_usage = transient ? MTLTextureUsageRenderTarget
                                                          : MTLTextureUsageShaderRead;
                if (usages[u] & TEXTURE_USAGE_RENDER_TARGET) expected_usage |= MTLTextureUsageRenderTarget;
                assert(device.descriptor.usage == expected_usage);
                mtl_v2_texture_destroy(tex);
            }
        }
        _state.device.device = nil;
        [device release];
        puts("Metal texture storage regression passed");

        // Also let the actual host Metal device validate sampled depth targets.
        id<MTLDevice> metal = MTLCreateSystemDefaultDevice();
        if (metal) {
            _state.device.device = metal;
            for (NSUInteger f = 0; f < 2; ++f) {
                u32 tex = mtl_v2_texture_create(32, 16, 1, formats[f],
                    TEXTURE_USAGE_READ | TEXTURE_USAGE_RENDER_TARGET, 1, TEXTURE_2D);
                assert(tex != 0);
                assert(_state.textures[tex].texture.storageMode == MTLStorageModePrivate);
                mtl_v2_texture_destroy(tex);
            }
            _state.device.device = nil;
            [metal release];
            puts("Metal device depth texture allocation passed");
        } else {
            puts("SKIP: no Metal device for depth texture allocation");
        }
    }
    return 0;
}
