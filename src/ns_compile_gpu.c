#include "ns_compile_gpu.h"
#include "ns_compile_gpu_abi.h"
#include "ns_profile.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

static void ns_compile_gpu_event_add(ns_compile_gpu_event **events, i32 base,
    i32 nvalues, i32 value, i32 block, i32 at, i32 definition) {
    if (value < 0 || value >= nvalues) return;
    ns_compile_gpu_event event = {base + value, block, at, definition};
    ns_array_push(*events, event);
}

void ns_compile_gpu_free(i32 **batch) {
    for (szt i = 0; i < ns_array_length(batch); ++i) ns_array_free(batch[i]);
    ns_array_free(batch);
}

i32 **ns_compile_gpu_last_uses(ns_ssa_module *m, ns_compile_gpu_stats *stats) {
    ns_compile_gpu_stats local = {0};
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    double start = ns_profile_now_ms();
    ns_compile_gpu_run_v1 run = NULL;
    if (!m || ns_array_length(m->fns) == 0) goto unavailable;
#ifdef __APPLE__
    char path[PATH_MAX];
    const char *override = getenv("NS_COMPILER_GPU_LIBRARY");
    if (override) {
        if (snprintf(path, sizeof(path), "%s", override) >= (int)sizeof(path)) goto unavailable;
    } else {
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) != 0) goto unavailable;
        char resolved[PATH_MAX];
        if (!realpath(path, resolved)) goto unavailable;
        char *slash = strrchr(resolved, '/');
        if (!slash) goto unavailable;
        *slash = 0;
        if (snprintf(path, sizeof(path), "%s/ns_compile_gpu.dylib", resolved) >= (int)sizeof(path)) goto unavailable;
    }
    // Keep the service loaded: its thread-safe pipeline cache survives builds.
    // dlopen is reference-counted; balance this reference at the end of the call.
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!library) goto unavailable;
    run = (ns_compile_gpu_run_v1)dlsym(library, "ns_compile_gpu_metal_v1");
    if (!run) { dlclose(library); goto unavailable; }
#endif
    if (!run) goto unavailable;

    i32 **batch = NULL;
    i32 *offsets = NULL;
    ns_compile_gpu_event *events = NULL;
    i32 total = 0;
    for (szt fi = 0; fi < ns_array_length(m->fns); ++fi) {
        ns_ssa_fn *fn = &m->fns[fi];
        i32 count = 0;
        for (szt i = 0; i < ns_array_length(fn->insts); ++i) {
            if (fn->insts[i].dst == INT_MAX) goto too_large;
            if (fn->insts[i].dst >= count) count = fn->insts[i].dst + 1;
        }
        if (count > INT_MAX - total) goto too_large;
        ns_array_push(offsets, total);
        total += count;
        i32 *last = NULL;
        ns_array_set_length(last, count);
        ns_array_push(batch, last);
    }
    if (!total) goto too_large;
    for (szt fi = 0; fi < ns_array_length(m->fns); ++fi) {
        ns_ssa_fn *fn = &m->fns[fi];
        i32 count = (i32)ns_array_length(batch[fi]), base = offsets[fi];
        for (i32 bi = 0; bi < (i32)ns_array_length(fn->blocks); ++bi) {
            ns_ssa_block *bb = &fn->blocks[bi];
            i32 next_call = -1;
            for (i32 ii = (i32)ns_array_length(bb->insts) - 1; ii >= 0; --ii) {
                ns_ssa_inst *in = &fn->insts[bb->insts[ii]];
                ns_compile_gpu_event_add(&events, base, count, in->dst, bi, ii, 1);
                if (in->op == NS_SSA_OP_PHI) {
                    for (i32 p = 0; p < 2 + (i32)ns_array_length(in->phi_edges); ++p) {
                        i32 value = p == 0 ? in->a : p == 1 ? in->b : in->phi_edges[p - 2].value;
                        i32 pred = p == 0 ? in->target0 : p == 1 ? in->target1 : in->phi_edges[p - 2].pred;
                        i32 at = 0;
                        if (pred >= 0 && pred < (i32)ns_array_length(fn->blocks))
                            at = (i32)ns_array_length(fn->blocks[pred].insts);
                        else pred = -1;
                        ns_compile_gpu_event_add(&events, base, count, value, pred, at, 0);
                    }
                } else {
                    i32 at = in->op == NS_SSA_OP_ARG && next_call >= 0 ? next_call : ii;
                    ns_compile_gpu_event_add(&events, base, count, in->a, bi, at, 0);
                    ns_compile_gpu_event_add(&events, base, count, in->b, bi, at, 0);
                    if (in->op == NS_SSA_OP_ARRAY_STORE)
                        ns_compile_gpu_event_add(&events, base, count, in->target0, bi, at, 0);
                }
                if (in->op == NS_SSA_OP_CALL) next_call = ii;
            }
        }
    }
    if (ns_array_length(events) > UINT32_MAX) goto too_large;
    stats->events = (u32)ns_array_length(events);
    stats->values = (u32)total;
    i32 *result = ns_malloc((szt)total * sizeof(i32));
    int ok = run(events, stats->events, stats->values, result, &stats->device_ms,
                 stats->error, sizeof(stats->error));
    if (ok) for (szt fi = 0; fi < ns_array_length(batch); ++fi)
        if (ns_array_length(batch[fi])) memcpy(batch[fi], result + offsets[fi], ns_array_length(batch[fi]) * sizeof(i32));
    ns_free(result);
    ns_array_free(events);
    ns_array_free(offsets);
#ifdef __APPLE__
    dlclose(library);
#endif
    stats->total_ms = ns_profile_now_ms() - start;
    if (!ok) { ns_compile_gpu_free(batch); return NULL; }
    return batch;

too_large:
    ns_compile_gpu_free(batch);
    ns_array_free(events);
    ns_array_free(offsets);
#ifdef __APPLE__
    dlclose(library);
#endif
    snprintf(stats->error, sizeof(stats->error), "empty or oversized SSA batch");
    stats->total_ms = ns_profile_now_ms() - start;
    return NULL;
unavailable:
    snprintf(stats->error, sizeof(stats->error), "compiler GPU service unavailable (requires macOS Metal and ns_compile_gpu.dylib)");
    stats->total_ms = ns_profile_now_ms() - start;
    return NULL;
}

i32 **ns_compile_gpu_prepare(ns_ssa_module *m) {
    const char *backend = getenv("NS_COMPILER_BACKEND");
    if (!backend || strcmp(backend, "gpu") != 0) return NULL;
    ns_compile_gpu_stats stats;
    ns_str name = ns_str_cstr("gpu_last_uses"), lib = ns_str_cstr("compiler");
    double start = ns_profile_now_ms();
    ns_profile_scope_enter(name, lib);
    i32 **batch = ns_compile_gpu_last_uses(m, &stats);
    ns_profile_record_scope(name, lib, 0, start, ns_profile_now_ms() - start);
    if (!batch) {
        ns_warn("compiler", "%s; using CPU last-use analysis\n", stats.error);
        return NULL;
    }
    if (getenv("NS_COMPILER_GPU_VERIFY")) {
        for (szt fi = 0; fi < ns_array_length(batch); ++fi) {
            i32 n = (i32)ns_array_length(batch[fi]);
            i32 *cpu = ns_ssa_last_uses(&m->fns[fi], n);
            ns_bool same = n == 0 || memcmp(cpu, batch[fi], (szt)n * sizeof(i32)) == 0;
            ns_array_free(cpu);
            if (!same) {
                ns_warn("compiler", "GPU last-use mismatch; using CPU analysis\n");
                ns_compile_gpu_free(batch);
                return NULL;
            }
        }
    }
    return batch;
}
