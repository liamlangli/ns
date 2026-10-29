// Exercises IOSurface import and frame lifetime without capturing the desktop
// or requesting any OS permission. Uses a synthetic producer pixel buffer.
#include "../lib/src/gpu.metal.m"
int main(void) {
    @autoreleasepool {
        _state.device.device = MTLCreateSystemDefaultDevice();
        if (!_state.device.device) { puts("desktop_metal_test: no Metal device, skipped"); return 0; }
        _state.cmd_queue = [_state.device.device newCommandQueue];
        _state.texture_count = 1;
        NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES};
        CVPixelBufferRef pixel = NULL;
        assert(CVPixelBufferCreate(NULL, 16, 8, kCVPixelFormatType_32BGRA, (CFDictionaryRef)attributes, &pixel) == kCVReturnSuccess);
        CVPixelBufferLockBaseAddress(pixel, 0);
        uint8_t *data = CVPixelBufferGetBaseAddress(pixel);
        size_t row = CVPixelBufferGetBytesPerRow(pixel);
        for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 16; ++x) {
            uint8_t *p = data + row * y + x * 4;
            p[0] = (uint8_t)(x + y); p[1] = 42; p[2] = 99; p[3] = 255;
        }
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        u32 texture = mtl_v2_texture_create(1, 1, 1, PIXELFORMAT_BGRA8, TEXTURE_USAGE_READ, 1, TEXTURE_2D, "desktop import test image");
        assert(texture != 0);
        assert(!gpu_metal_import_pixel_buffer(texture, pixel, "desktop import test image"));
        _state.cmd_buffer = [_state.cmd_queue commandBuffer];
        assert(gpu_metal_import_pixel_buffer(texture, pixel, "desktop import test image"));
        assert(_state.textures[texture].width == 16 && _state.textures[texture].height == 8);
        assert(_state.textures[texture].texture.iosurface != NULL);
        id<MTLBuffer> readback = [_state.device.device newBufferWithLength:256 * 8 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [_state.cmd_buffer blitCommandEncoder];
        [blit copyFromTexture:_state.textures[texture].texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(16, 8, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 8];
        [blit endEncoding];
        // The producer releases its last reference before GPU execution. The
        // import must keep the surface from returning to the producer's pool.
        CVPixelBufferRelease(pixel);
        [_state.cmd_buffer commit]; [_state.cmd_buffer waitUntilCompleted];
        assert(_state.cmd_buffer.status == MTLCommandBufferStatusCompleted);
        const uint8_t *result = readback.contents;
        for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 16; ++x) {
            const uint8_t *p = result + 256 * y + x * 4;
            assert(p[0] == x + y && p[1] == 42 && p[2] == 99 && p[3] == 255);
        }
        mtl_v2_texture_destroy(texture);
        // Capture rebuilds and interactive resize must recycle destroyed slots.
        for (int i = 0; i < GPU_RESOURCE_POOL_SIZE + 10; ++i) {
            u32 slot = mtl_v2_texture_create(1, 1, 1, PIXELFORMAT_RGBA8, TEXTURE_USAGE_READ, 1, TEXTURE_2D, "desktop texture recycle test");
            assert(slot == texture); mtl_v2_texture_destroy(slot);
        }
        assert(_state.texture_count == 2);
        if (_state.pixel_buffer_cache) CFRelease(_state.pixel_buffer_cache);
        [readback release]; [_state.cmd_queue release]; [_state.device.device release];
        puts("desktop_metal_test: IOSurface pixels, GPU lifetime, slot recycling passed");
    }
    return 0;
}
