#include "gpu.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static unsigned char *memory[GPU_RESOURCE_POOL_SIZE];
static ns_bool acquired;
static int acquisitions;
static void begin(void) { acquired = true; acquisitions++; }
static ns_bool create(u32 slot, u64 size, u32 flags, const char *name, u64 *base) {
    (void)flags; (void)name;
    memory[slot] = calloc(1, (size_t)size);
    *base = 0;
    return memory[slot] != NULL;
}
static void destroy(u32 slot) { free(memory[slot]); memory[slot] = NULL; }
static void write_bytes(u32 slot, u64 offset, const void *src, u64 size) {
    assert(acquired && "CPU writes must acquire the frame before touching shared memory");
    memcpy(memory[slot] + offset, src, (size_t)size);
}
static ns_bool read_bytes(u32 slot, u64 offset, void *dst, u64 size) {
    memcpy(dst, memory[slot] + offset, (size_t)size);
    return true;
}
static void *host_ptr(u32 slot) { return memory[slot]; }
int main(void) {
    const gpu_v2_ops ops = {.frame_begin = begin, .mem_create = create, .mem_destroy = destroy,
        .mem_write = write_bytes, .mem_read = read_bytes, .mem_host_ptr = host_ptr};
    gpu_v2_set_backend(&ops, 0, 8);
    gpu_addr persistent = gpu_malloc(16, GPU_MEM_DEVICE, "frame acquisition test parameters");
    gpu_addr framed = gpu_malloc(16, GPU_MEM_SHARED | GPU_MEM_FRAMES, "frame acquisition test uniform");
    assert(persistent && framed);
    for (int frame = 0; frame < 6; frame++) {
        acquired = false;
        int before = acquisitions;
        gpu_addr ring = gpu_frame_alloc(16, 0);
        assert(ring && acquired && acquisitions > before);
        float values[4] = {(float)frame, -2.5f, 1.25f, 100.0f};
        gpu_write(ring, values, sizeof(values));
        acquired = false;
        gpu_write(framed, values, sizeof(values));
        assert(acquired);
        acquired = false;
        gpu_write(persistent, values, sizeof(values));
        assert(acquired);
        gpu_v2_flush_uploads();
        float got[4];
        assert(gpu_read(persistent, got, sizeof(got)) && memcmp(values, got, sizeof(got)) == 0);
        gpu_v2_frame_end();
    }
    gpu_free(persistent);
    gpu_free(framed);
    gpu_v2_set_backend(NULL, 0, 0);
    puts("PASS: acquire frame before ring, framed uniform and staged parameter writes.");
    return 0;
}
