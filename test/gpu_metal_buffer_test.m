// Freed allocations must never be rebound by a later draw or dispatch.
#include "../lib/src/gpu.metal.m"

static NSUInteger destroyed_buffers;

@interface BindingBuffer : NSObject
@end
@implementation BindingBuffer
- (void)dealloc {
    destroyed_buffers += 1;
    [super dealloc];
}
@end

@interface BindingEncoder : NSObject
@property(assign) id<MTLBuffer> expected;
@property NSUInteger vertex_calls;
@property NSUInteger fragment_calls;
@property NSUInteger compute_calls;
@end
@implementation BindingEncoder
- (void)checkBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index {
    assert(buffer == self.expected);
    assert(offset == 512);
    assert(index == GPU_MTL_STORAGE_BINDING_BASE + 1);
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self checkBuffer:buffer offset:offset index:index];
    self.vertex_calls += 1;
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self checkBuffer:buffer offset:offset index:index];
    self.fragment_calls += 1;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self checkBuffer:buffer offset:offset index:index];
    self.compute_calls += 1;
}
@end

int main(void) {
    @autoreleasepool {
        _state.v2_storage_slot_count = GPU_MTL_STORAGE_SLOT_COUNT;
        u32 last = _state.v2_storage_slot_count - 1;
        _state.v2_memory[0] = (id<MTLBuffer>)[BindingBuffer new];
        _state.v2_memory[1] = (id<MTLBuffer>)[BindingBuffer new];
        _state.v2_memory_size[0] = _state.v2_memory_size[1] = 1024;
        mtl_v2_set_root(0, 256, 0);
        mtl_v2_set_storage(GPU_MTL_STORAGE_BINDING_BASE, 0, 128, 0);
        mtl_v2_set_storage(GPU_MTL_STORAGE_BINDING_BASE + last, 0, 768, 0);
        mtl_v2_set_storage(GPU_MTL_STORAGE_BINDING_BASE + 1, 1, 512, 0);

        // An unused/out-of-range slot must leave live bindings alone.
        mtl_v2_mem_destroy(2);
        mtl_v2_mem_destroy(GPU_RESOURCE_POOL_SIZE);
        assert(_state.v2_root_buffer == _state.v2_memory[0]);
        assert(_state.v2_root_offset == 256);

        mtl_v2_mem_destroy(0);
        assert(destroyed_buffers == 1);
        assert(_state.v2_memory[0] == nil && _state.v2_memory_size[0] == 0);
        assert(_state.v2_root_buffer == nil && _state.v2_root_offset == 0);
        assert(_state.v2_storage_buffers[0] == nil && _state.v2_storage_offsets[0] == 0);
        assert(_state.v2_storage_buffers[last] == nil && _state.v2_storage_offsets[last] == 0);
        assert(_state.v2_storage_buffers[1] == _state.v2_memory[1]);
        assert(_state.v2_storage_offsets[1] == 512);

        // Reusing the allocation slot must not revive its previous bindings.
        _state.v2_memory[0] = (id<MTLBuffer>)[BindingBuffer new];
        _state.v2_memory_size[0] = 1024;
        BindingEncoder *encoder = [BindingEncoder new];
        encoder.expected = _state.v2_memory[1];
        gpu_shader_mtl shader = {.uses_root = true};
        mtl_v2_bind_root(&shader, (id<MTLCommandEncoder>)encoder, false);
        mtl_v2_bind_root(&shader, (id<MTLCommandEncoder>)encoder, true);
        assert(encoder.vertex_calls == 1 && encoder.fragment_calls == 1);
        assert(encoder.compute_calls == 1);

        mtl_v2_mem_destroy(0);
        mtl_v2_mem_destroy(1);
        mtl_v2_mem_destroy(1);
        assert(destroyed_buffers == 3);
        assert(_state.v2_storage_buffers[1] == nil && _state.v2_storage_offsets[1] == 0);
        mtl_v2_bind_root(&shader, (id<MTLCommandEncoder>)encoder, false);
        mtl_v2_bind_root(&shader, (id<MTLCommandEncoder>)encoder, true);
        assert(encoder.vertex_calls == 1 && encoder.fragment_calls == 1);
        assert(encoder.compute_calls == 1);
        [encoder release];
        puts("Metal buffer binding lifetime regression passed");
    }
    return 0;
}
