// Compiler-only Metal service. No application view/GPU runtime is linked here.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "ns_compile_gpu_abi.h"
#include <stdio.h>

static NSString *const source = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Event { int value, block, at, definition; };\n"
"struct Value { atomic_int count, block, at, last; };\n"
"kernel void initialize(device Value *v [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
" atomic_store_explicit(&v[i].count, 0, memory_order_relaxed);\n"
" atomic_store_explicit(&v[i].block, -1, memory_order_relaxed);\n"
" atomic_store_explicit(&v[i].at, 0, memory_order_relaxed);\n"
" atomic_store_explicit(&v[i].last, -1, memory_order_relaxed); }\n"
"kernel void definitions(device const Event *e [[buffer(0)]], device Value *v [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
" Event x = e[i]; if (!x.definition) return;\n"
" atomic_fetch_add_explicit(&v[x.value].count, 1, memory_order_relaxed);\n"
" atomic_store_explicit(&v[x.value].block, x.block, memory_order_relaxed);\n"
" atomic_store_explicit(&v[x.value].at, x.at, memory_order_relaxed); }\n"
"kernel void uses(device const Event *e [[buffer(0)]], device Value *v [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
" Event x = e[i]; if (x.definition) return;\n"
" int block = atomic_load_explicit(&v[x.value].block, memory_order_relaxed);\n"
" int at = atomic_load_explicit(&v[x.value].at, memory_order_relaxed);\n"
" int last = x.block < 0 || x.block != block || x.at < at ? 0x7fffffff : x.at;\n"
" atomic_fetch_max_explicit(&v[x.value].last, last, memory_order_relaxed); }\n"
"kernel void finish(device Value *v [[buffer(0)]], device int *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
" out[i] = atomic_load_explicit(&v[i].count, memory_order_relaxed) != 1 ? 0x7fffffff : atomic_load_explicit(&v[i].last, memory_order_relaxed); }\n";

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLComputePipelineState> pipelines[4];
static char initialization_error[256];

static void initialize(void) {
    device = MTLCreateSystemDefaultDevice();
    if (!device) { snprintf(initialization_error, sizeof(initialization_error), "no Metal device"); return; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) { snprintf(initialization_error, sizeof(initialization_error), "%s", error.localizedDescription.UTF8String ?: "cannot compile compiler Metal kernels"); return; }
    NSArray<NSString *> *names = @[@"initialize", @"definitions", @"uses", @"finish"];
    for (int i = 0; i < 4; ++i) {
        pipelines[i] = [device newComputePipelineStateWithFunction:[library newFunctionWithName:names[i]] error:&error];
        if (!pipelines[i]) { snprintf(initialization_error, sizeof(initialization_error), "%s", error.localizedDescription.UTF8String ?: "cannot create compiler compute pipeline"); return; }
    }
    queue = [device newCommandQueue];
    queue.label = @"NS compiler analysis queue";
    if (!queue) snprintf(initialization_error, sizeof(initialization_error), "cannot create compiler command queue");
}

// Every call owns its buffers/command encoders. The immutable pipeline cache
// is initialized once, so concurrent manifest target builds are independent.
int ns_compile_gpu_metal_v1(const ns_compile_gpu_event *events, uint32_t count,
    uint32_t values, int32_t *last, double *device_ms, char *error, uint32_t error_size) {
    @autoreleasepool {
        static dispatch_once_t once;
        dispatch_once(&once, ^{ initialize(); });
        if (!queue) {
            snprintf(error, error_size, "%s", initialization_error);
            return 0;
        }
        NSUInteger event_size = (NSUInteger)count * sizeof(*events);
        NSUInteger state_size = (NSUInteger)values * 4 * sizeof(int32_t);
        NSUInteger result_size = (NSUInteger)values * sizeof(int32_t);
        if (!values || !last || event_size > device.maxBufferLength || state_size > device.maxBufferLength) {
            snprintf(error, error_size, "compiler analysis batch exceeds Metal buffer limits");
            return 0;
        }
        id<MTLBuffer> input = count ? [device newBufferWithBytes:events length:event_size options:MTLResourceStorageModeShared] : nil;
        id<MTLBuffer> state = [device newBufferWithLength:state_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:result_size options:MTLResourceStorageModeShared];
        input.label = @"NS compiler SSA definition and use events";
        state.label = @"NS compiler SSA definition counts and last uses";
        output.label = @"NS compiler last-use analysis results";
        if ((count && !input) || !state || !output) {
            snprintf(error, error_size, "cannot allocate compiler analysis buffers");
            return 0;
        }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        command.label = @"NS compiler last-use analysis";
        if (!command) { snprintf(error, error_size, "cannot create compiler command buffer"); return 0; }
        NSArray<NSString *> *labels = @[@"compiler initialize SSA values", @"compiler collect definitions", @"compiler reduce last uses", @"compiler publish last uses"];
        for (int stage = 0; stage < 4; ++stage) {
            NSUInteger threads = stage == 1 || stage == 2 ? count : values;
            if (!threads) continue;
            // Encoder boundaries order device writes between passes.
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (!encoder) { snprintf(error, error_size, "cannot create compiler analysis encoder"); return 0; }
            encoder.label = labels[stage];
            [encoder setComputePipelineState:pipelines[stage]];
            [encoder setBuffer:stage == 1 || stage == 2 ? input : state offset:0 atIndex:0];
            if (stage != 0) [encoder setBuffer:stage == 3 ? output : state offset:0 atIndex:1];
            NSUInteger width = MIN((NSUInteger)256, pipelines[stage].maxTotalThreadsPerThreadgroup);
            [encoder dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
            [encoder endEncoding];
        }
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            snprintf(error, error_size, "%s", command.error.localizedDescription.UTF8String ?: "compiler GPU execution failed");
            return 0;
        }
        *device_ms = (command.GPUEndTime - command.GPUStartTime) * 1000.0;
        memcpy(last, output.contents, result_size);
        return 1;
    }
}
