#include "gpu.h"
#include <assert.h>
#include <stdio.h>

static int creates, encodes, destroys;
static ns_bool supported(i32 kind) { return kind == GPU_FX_SPATIAL || kind == GPU_FX_TEMPORAL; }
static u32 create(i32 kind, u32 c, u32 d, u32 m, u32 o) {
    assert(kind == GPU_FX_TEMPORAL && c == 1 && d == 2 && m == 3 && o == 4);
    creates++;
    return 17;
}
static ns_bool encode(u32 handle, u32 c, u32 d, u32 m, u32 prev, u32 o, const gpu_fx_frame *f) {
    assert(handle == 17 && c == 1 && d == 2 && m == 3 && prev == 5 && o == 4);
    assert(f->jitter_x == 0.25f && f->reset == 1 && f->aspect_ratio == 2.0f);
    encodes++;
    return true;
}
static void destroy(u32 handle) { assert(handle == 17); destroys++; }
int main(void) {
    _Static_assert(sizeof(gpu_fx_frame) == 40, "gpu_fx_frame FFI layout");
    gpu_fx_frame f = {.jitter_x = 0.25f, .aspect_ratio = 2.0f, .reset = 1};
    assert(!gpu_fx_supported(GPU_FX_SPATIAL));
    assert(!gpu_fx_create(GPU_FX_SPATIAL, 1, 0, 0, 4));
    assert(!gpu_fx_encode(17, 1, 2, 3, 5, 4, &f));
    gpu_fx_destroy(17);
    const gpu_v2_ops ops = {.fx_supported = supported, .fx_create = create, .fx_encode = encode, .fx_destroy = destroy};
    gpu_v2_set_backend(&ops, 0, 0);
    assert(gpu_fx_supported(GPU_FX_SPATIAL));
    assert(!gpu_fx_supported(0) && !gpu_fx_supported(4));
    assert(!gpu_fx_create(GPU_FX_FRAME_INTERPOLATION, 1, 2, 3, 4));
    assert(!gpu_fx_create(GPU_FX_TEMPORAL, 1, 0, 3, 4));
    assert(!gpu_fx_create(GPU_FX_TEMPORAL, 1, 2, 0, 4));
    assert(!gpu_fx_create(GPU_FX_TEMPORAL, 0, 2, 3, 4));
    assert(!gpu_fx_create(GPU_FX_TEMPORAL, 1, 2, 3, 1));
    assert(creates == 0);
    assert(gpu_fx_create(GPU_FX_TEMPORAL, 1, 2, 3, 4) == 17 && creates == 1);
    assert(!gpu_fx_encode(0, 1, 2, 3, 5, 4, &f));
    assert(!gpu_fx_encode(17, 1, 2, 3, 5, 4, NULL));
    assert(!gpu_fx_encode(17, 1, 2, 3, 4, 4, &f));
    assert(!gpu_fx_encode(17, 1, 2, 3, 5, 1, &f));
    assert(encodes == 0);
    assert(gpu_fx_encode(17, 1, 2, 3, 5, 4, &f) && encodes == 1);
    gpu_fx_destroy(0);
    assert(destroys == 0);
    gpu_fx_destroy(17);
    assert(destroys == 1);
    const gpu_v2_ops unsupported = {0};
    gpu_v2_set_backend(&unsupported, 0, 0);
    assert(!gpu_fx_supported(GPU_FX_SPATIAL));
    assert(!gpu_fx_encode(17, 1, 2, 3, 5, 4, &f));
    gpu_fx_destroy(17);
    gpu_v2_set_backend(NULL, 0, 0);
    puts("PASS: GPU FX fallback, argument validation, dispatch and FFI layout.");
}
