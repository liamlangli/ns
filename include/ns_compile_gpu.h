#pragma once

#include "ns_ssa.h"

// Optional compiler acceleration, separate from the program's GPU runtime.
// The batch owns one ns_array per function. NULL means unavailable/failed;
// callers must use ns_ssa_last_uses instead. No SSA is mutated.
typedef struct ns_compile_gpu_stats {
    double total_ms;       // packing + device initialization + execution + unpacking
    double device_ms;      // GPU command time, excluding host preparation
    u32 events;
    u32 values;
    char error[256];
} ns_compile_gpu_stats;

i32 **ns_compile_gpu_last_uses(ns_ssa_module *m, ns_compile_gpu_stats *stats);
void ns_compile_gpu_free(i32 **batch);
// Opt-in with NS_COMPILER_BACKEND=gpu; CPU is the default. Failures fall back.
i32 **ns_compile_gpu_prepare(ns_ssa_module *m);
