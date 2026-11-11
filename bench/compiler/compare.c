// Compile actual source through the shared front end, then time the same SSA
// through CPU and GPU-assisted native backends. Timing excludes validation.
#include "ns_compile_gpu.h"
#include "ns_aarch.h"
#include "ns_amd64.h"
#include "ns_os.h"
#include "ns_profile.h"
#include "ns_token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int order(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *v, int n) { qsort(v, (size_t)n, sizeof(*v), order); return v[n / 2]; }
static void fail(const char *message) { fprintf(stderr, "compiler benchmark: %s\n", message); exit(1); }

static void free_ast(ns_ast_ctx *ctx) {
    ns_array_free(ctx->nodes); ns_array_free(ctx->sections); ns_array_free(ctx->stack);
    ns_array_free(ctx->op_stack); ns_array_free(ctx->expr_stack); ns_array_free(ctx->scopes);
}

static ns_return_ptr emit(ns_ssa_module *m) {
#ifdef __aarch64__
    return ns_aarch_from_ssa(m);
#else
    return ns_amd64_from_ssa(m);
#endif
}
static void release(void *bin) {
#ifdef __aarch64__
    ns_aarch_free(bin);
#else
    ns_amd64_free(bin);
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) fail("expected input.ns [odd repetition count]");
    int repeats = argc > 2 ? atoi(argv[2]) : 9;
    if (repeats < 3 || repeats > 101 || repeats % 2 == 0) fail("repetitions must be odd, 3..101");
    ns_str source = ns_os_read_file(ns_str_cstr(argv[1]));
    if (!source.len) fail("empty input");
    double lex[101], cached[101], uncached[101];
    for (int iteration = 0; iteration < repeats; ++iteration) {
        ns_token_t token = {.line = 1};
        i32 offset = 0;
        double begin = ns_profile_now_ms();
        do { offset = ns_next_token(&token, source, ns_str_cstr(argv[1]), offset); }
        while (token.type != NS_TOKEN_EOF && offset <= source.len);
        lex[iteration] = ns_profile_now_ms() - begin;
        for (int step = 0; step < 2; ++step) {
            int cache = (iteration + step) % 2;
            setenv("NS_PARSE_TOKEN_CACHE", cache ? "1" : "0", 1);
            ns_ast_ctx parsed = {0};
            begin = ns_profile_now_ms();
            if (ns_return_is_error(ns_ast_parse(&parsed, source, ns_str_cstr(argv[1])))) fail("parse failed");
            double elapsed = ns_profile_now_ms() - begin;
            if (cache) cached[iteration] = elapsed; else uncached[iteration] = elapsed;
            free_ast(&parsed);
        }
    }
    unsetenv("NS_PARSE_TOKEN_CACHE");
    ns_ast_ctx ctx = {0};
    double start = ns_profile_now_ms();
    if (ns_return_is_error(ns_ast_parse(&ctx, source, ns_str_cstr(argv[1])))) fail("parse failed");
    ns_return_ptr built = ns_ssa_build_with_runtime_paths_options(&ctx, ns_str_cstr("lib"), ns_str_cstr("bin"), ns_str_null, false);
    if (ns_return_is_error(built)) fail("SSA lowering failed");
    double frontend = ns_profile_now_ms() - start;
    ns_ssa_module *m = built.r;
    ns_compile_gpu_stats stats;
    i32 **batch = ns_compile_gpu_last_uses(m, &stats);
    if (!batch) fail(stats.error);
    double cold = stats.total_ms;
    u32 values = stats.values, events = stats.events;
    i32 *counts = NULL;
    for (szt f = 0; f < ns_array_length(batch); ++f) {
        int n = (int)ns_array_length(batch[f]);
        ns_array_push(counts, n);
        i32 *cpu = ns_ssa_last_uses(&m->fns[f], n);
        if (n && memcmp(cpu, batch[f], (size_t)n * sizeof(i32))) fail("GPU/CPU last-use mismatch");
        ns_array_free(cpu);
    }
    ns_compile_gpu_free(batch);
    double cpu[101], gpu[101], device[101], native_cpu[101], native_gpu[101];
    for (int iteration = 0; iteration < repeats; ++iteration) {
        // Alternate order to reduce systematic thermal/order bias.
        for (int step = 0; step < 2; ++step) {
            int use_gpu = (iteration + step) % 2;
            if (use_gpu) setenv("NS_COMPILER_BACKEND", "gpu", 1);
            else unsetenv("NS_COMPILER_BACKEND");
            start = ns_profile_now_ms();
            if (use_gpu) {
                batch = ns_compile_gpu_last_uses(m, &stats);
                if (!batch) fail(stats.error);
                device[iteration] = stats.device_ms;
                ns_compile_gpu_free(batch);
                gpu[iteration] = ns_profile_now_ms() - start;
            } else {
                // Include result allocation/free on both sides.
                for (szt f = 0; f < ns_array_length(m->fns); ++f) {
                    i32 *last = ns_ssa_last_uses(&m->fns[f], counts[f]);
                    ns_array_free(last);
                }
                cpu[iteration] = ns_profile_now_ms() - start;
            }
            start = ns_profile_now_ms();
            ns_return_ptr binary = emit(m);
            double elapsed = ns_profile_now_ms() - start;
            if (ns_return_is_error(binary)) fail("native emission failed");
            release(binary.r);
            if (use_gpu) native_gpu[iteration] = elapsed;
            else native_cpu[iteration] = elapsed;
        }
    }
    printf("{\"functions\":%zu,\"values\":%u,\"events\":%u,\"frontend_ms\":%.6f,"
           "\"lex_ms\":%.6f,\"uncached_parse_ms\":%.6f,\"cached_parse_ms\":%.6f,"
           "\"cold_gpu_analysis_ms\":%.6f,\"cpu_analysis_ms\":%.6f,\"gpu_analysis_ms\":%.6f,"
           "\"gpu_device_ms\":%.6f,\"cpu_backend_ms\":%.6f,\"gpu_backend_ms\":%.6f}\n",
           (size_t)ns_array_length(m->fns), values, events, frontend,
           median(lex, repeats), median(uncached, repeats), median(cached, repeats), cold,
           median(cpu, repeats), median(gpu, repeats), median(device, repeats),
           median(native_cpu, repeats), median(native_gpu, repeats));
    ns_array_free(counts);
    ns_ssa_module_free(m);
    free_ast(&ctx);
    ns_str_free(source);
    return 0;
}
