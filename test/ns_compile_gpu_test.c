#include "ns_compile_gpu.h"
#include "ns_aarch.h"
#include "ns_amd64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define setenv(key, value, overwrite) _putenv_s(key, value)
#define unsetenv(key) _putenv_s(key, "")
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "ns_compile_gpu_test:%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static u32 seed = 314159;
static u32 random_u32(void) { seed = seed * 1664525u + 1013904223u; return seed; }

static void append(ns_ssa_fn *fn, i32 block, ns_ssa_inst inst) {
    ns_array_push(fn->blocks[block].insts, (i32)ns_array_length(fn->insts));
    ns_array_push(fn->insts, inst);
}

static void compare(ns_ssa_module *m, i32 **batch) {
    CHECK(ns_array_length(batch) == ns_array_length(m->fns));
    for (szt f = 0; f < ns_array_length(m->fns); ++f) {
        i32 n = (i32)ns_array_length(batch[f]);
        i32 *cpu = ns_ssa_last_uses(&m->fns[f], n);
        CHECK(n == 0 || memcmp(cpu, batch[f], (szt)n * sizeof(i32)) == 0);
        ns_array_free(cpu);
    }
}

static void check_native(ns_ssa_module *m) {
    unsetenv("NS_COMPILER_BACKEND");
    ns_return_ptr a = ns_aarch_from_ssa(m), x = ns_amd64_from_ssa(m);
    CHECK(!ns_return_is_error(a) && !ns_return_is_error(x));
    setenv("NS_COMPILER_BACKEND", "gpu", 1);
    ns_return_ptr b = ns_aarch_from_ssa(m), y = ns_amd64_from_ssa(m);
    CHECK(!ns_return_is_error(b) && !ns_return_is_error(y));
    ns_aarch_module_bin *aa = a.r, *bb = b.r;
    ns_amd64_module_bin *xx = x.r, *yy = y.r;
    CHECK(ns_array_length(aa->fns) == ns_array_length(bb->fns));
    CHECK(ns_array_length(xx->fns) == ns_array_length(yy->fns));
    for (szt i = 0; i < ns_array_length(aa->fns); ++i) {
        CHECK(ns_array_length(aa->fns[i].text) == ns_array_length(bb->fns[i].text));
        CHECK(memcmp(aa->fns[i].text, bb->fns[i].text, ns_array_length(aa->fns[i].text)) == 0);
    }
    for (szt i = 0; i < ns_array_length(xx->fns); ++i) {
        CHECK(ns_array_length(xx->fns[i].text) == ns_array_length(yy->fns[i].text));
        CHECK(memcmp(xx->fns[i].text, yy->fns[i].text, ns_array_length(xx->fns[i].text)) == 0);
    }
    ns_aarch_free(aa); ns_aarch_free(bb);
    ns_amd64_free(xx); ns_amd64_free(yy);
    unsetenv("NS_COMPILER_BACKEND");
}

int main(void) {
    // Deliberately include non-SSA inputs: duplicate definitions, undefined
    // values, invalid phi predecessors and reads preceding definitions.
    ns_ssa_module *m = ns_malloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    for (i32 f = 0; f < 64; ++f) {
        ns_ssa_fn fn = {0};
        for (i32 b = 0; b < 4; ++b) { ns_ssa_block block = {.id = b}; ns_array_push(fn.blocks, block); }
        for (i32 i = 0; i < 257; ++i) {
            ns_ssa_inst in = {.op = NS_SSA_OP_ADD, .dst = (i32)(random_u32() % 160),
                .a = (i32)(random_u32() % 164) - 2, .b = (i32)(random_u32() % 164) - 2,
                .target0 = (i32)(random_u32() % 7) - 2, .target1 = (i32)(random_u32() % 7) - 2};
            if (i % 11 == 0) {
                in.op = NS_SSA_OP_PHI;
                ns_ssa_phi_edge edge = {.pred = (i32)(random_u32() % 6) - 1, .value = (i32)(random_u32() % 162)};
                ns_array_push(in.phi_edges, edge);
            } else if (i % 7 == 0) in.op = NS_SSA_OP_ARG;
            else if (i % 13 == 0) in.op = NS_SSA_OP_CALL;
            else if (i % 5 == 0) in.op = NS_SSA_OP_ARRAY_STORE;
            append(&fn, i / 65, in);
        }
        ns_array_push(m->fns, fn);
    }
    ns_ssa_fn empty = {0};
    ns_array_push(m->fns, empty);
    ns_compile_gpu_stats stats;
    i32 **batch = ns_compile_gpu_last_uses(m, &stats);
    if (!batch) {
        fprintf(stderr, "ns_compile_gpu_test: unavailable: %s\n", stats.error);
        ns_ssa_module_free(m);
        return getenv("NS_REQUIRE_COMPILER_GPU") ? 1 : 0;
    }
    compare(m, batch);
    ns_compile_gpu_free(batch);
    ns_ssa_module_free(m);

    const char *source =
        "fn helper(a: i32, b: i32) i32 { return a * b + 1 }\n"
        "fn main() i32 { let sum = 0\n let a = [i32](17)\n"
        "for i in 0 to 17 { a[i] = helper(i, 2)\n"
        "if i % 2 == 0 { sum = sum + a[i] } else { sum = sum - i } }\n"
        "assert sum == 89\n return sum }\n";
    ns_ast_ctx ctx = {0};
    CHECK(!ns_return_is_error(ns_ast_parse(&ctx, ns_str_cstr((char *)source), ns_str_cstr("<gpu compiler test>"))));
    ns_return_ptr built = ns_ssa_build_with_runtime_paths_options(&ctx, ns_str_null, ns_str_null, ns_str_null, false);
    CHECK(!ns_return_is_error(built));
    m = built.r;
    batch = ns_compile_gpu_last_uses(m, &stats);
    CHECK(batch);
    compare(m, batch);
    ns_compile_gpu_free(batch);
    check_native(m);
    setenv("NS_COMPILER_GPU_LIBRARY", "/nonexistent/ns_compile_gpu.dylib", 1);
    CHECK(ns_compile_gpu_last_uses(m, &stats) == NULL);
    CHECK(stats.error[0]);
    check_native(m); // Failed device/service must preserve ordinary compilation.
    unsetenv("NS_COMPILER_GPU_LIBRARY");
    ns_ssa_module_free(m);
    puts("ns_compile_gpu_test: passed (randomized analysis, native byte parity, CPU fallback)");
    return 0;
}
