// Exercise the real dispatch path with an encoder that only supports uniform
// threadgroups, as on the visionOS simulator. No GPU or window is required.
#include "../lib/src/gpu.metal.m"

@interface DispatchPipeline : NSObject
@property NSUInteger threadExecutionWidth;
@property NSUInteger maxTotalThreadsPerThreadgroup;
@end
@implementation DispatchPipeline
@end

@interface DispatchEncoder : NSObject
@property(copy) NSString *label;
@property MTLSize groups;
@property MTLSize threads;
@property NSUInteger calls;
@property NSUInteger ends;
@end
@implementation DispatchEncoder
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline {
    assert(pipeline != nil);
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads {
    (void)grid;
    (void)threads;
    assert(!"nonuniform dispatch is unsupported on this device");
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads {
    self.groups = groups;
    self.threads = threads;
    self.calls += 1;
}
- (void)endEncoding { self.ends += 1; }
@end

@interface DispatchBuffer : NSObject
@property(strong) DispatchEncoder *encoder;
@end
@implementation DispatchBuffer
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    return (id<MTLComputeCommandEncoder>)self.encoder;
}
@end

int main(void) {
    @autoreleasepool {
        DispatchPipeline *pipeline = [DispatchPipeline new];
        DispatchEncoder *encoder = [DispatchEncoder new];
        DispatchBuffer *buffer = [DispatchBuffer new];
        buffer.encoder = encoder;
        _state.cmd_buffer = (id<MTLCommandBuffer>)buffer;
        _state.shader_count = 2;
        _state.v2_shader = 1;
        _state.shaders[1].compute_pso = (id<MTLComputePipelineState>)pipeline;

        const i32 grids[][3] = {
            {1, 1, 1}, {16, 1, 1}, {256, 256, 1}, {32, 128, 128},
            {17, 1, 1}, {9, 13, 1}, {30, 18, 10}, {1, 1, 127},
            {1, 256, 1}, {257, 129, 3}, {2147483647, 1, 1}
        };
        const NSUInteger budgets[] = {1, 7, 32, 64, 1024};
        const NSUInteger widths[] = {1, 16, 32, 64};
        for (NSUInteger w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
            pipeline.threadExecutionWidth = widths[w];
            for (NSUInteger b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
                pipeline.maxTotalThreadsPerThreadgroup = budgets[b];
                for (NSUInteger i = 0; i < sizeof(grids) / sizeof(grids[0]); ++i) {
                    NSUInteger before = encoder.calls;
                    mtl_v2_dispatch("uniform dispatch regression", grids[i][0], grids[i][1], grids[i][2]);
                    assert(encoder.calls == before + 1);
                    assert(encoder.ends == encoder.calls);
                    assert([encoder.label isEqualToString:@"uniform dispatch regression"]);
                    MTLSize groups = encoder.groups, threads = encoder.threads;
                    assert(threads.width && threads.height && threads.depth);
                    assert(threads.width * threads.height * threads.depth <= budgets[b]);
                    assert(groups.width * threads.width == (NSUInteger)grids[i][0]);
                    assert(groups.height * threads.height == (NSUInteger)grids[i][1]);
                    assert(groups.depth * threads.depth == (NSUInteger)grids[i][2]);
                }
            }
        }
        NSUInteger before = encoder.calls;
        mtl_v2_dispatch("empty x", 0, 8, 1);
        mtl_v2_dispatch("empty y", 8, 0, 1);
        mtl_v2_dispatch("empty z", 8, 8, 0);
        mtl_v2_dispatch("negative x", -1, 8, 1);
        mtl_v2_dispatch("negative y", 8, -1, 1);
        mtl_v2_dispatch("negative z", 8, 8, -1);
        assert(encoder.calls == before);
        puts("Metal uniform dispatch regression passed");
    }
    return 0;
}
