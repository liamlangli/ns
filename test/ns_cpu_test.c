#include "ns_test.h"
#include "ns_cpu.h"
#include "ns_ssa.h"

#include <unistd.h>

// ns_cpu: lower source to an image, run it, reject damaged images, and carry
// program state across a hot update.

static u8 *ns_cpu_test_image_at(const char *source, const char *name, ns_str ref_path) {
    ns_ast_ctx *ctx = calloc(1, sizeof(ns_ast_ctx));
    ns_return_bool parsed = ns_ast_parse(ctx, ns_str_cstr((i8 *)source), ns_str_cstr((i8 *)name));
    if (ns_return_is_error(parsed)) return ns_null;
    // The native lowering, as `ns run --cpu` uses; ns_ssa_build is the Wasm one.
    ns_return_ptr built = ns_ssa_build_with_runtime_paths_options(ctx, ref_path, ns_str_null, ns_str_null, false);
    if (ns_return_is_error(built)) return ns_null;
    ns_return_ptr image = ns_cpu_image_from_ssa(built.r);
    ns_ssa_module_free(built.r);
    return ns_return_is_error(image) ? ns_null : image.r;
}

static f64 ns_cpu_test_f64(i64 bits);

static u8 *ns_cpu_test_image(const char *source, const char *name) {
    return ns_cpu_test_image_at(source, name, ns_str_null);
}

// Native functions of the `mixt` test module: integer and float arguments
// interleaved, more of them than either register class holds.
static f64 mixt_a(i32 a, f64 b, i64 c, f32 d, i32 e, f64 f) {
    return a + b * 10 + (f64)c * 100 + d * 1000 + e * 10000 + f * 100000;
}

static f32 mixt_b(f32 a, i32 b, f32 c) { return a * (f32)b + c; }

static i32 mixt_c(f64 a, i32 b, i32 c, i32 d, i32 e, i32 f, i32 g, i32 h, i32 i, i32 j, f32 k, f32 l, f32 m, f32 n,
                  f32 o, f32 p, f32 q, f32 r, f32 s) {
    return (i32)a + b + c + d + e + f + g + h + i + j * 2 + (i32)(k + l + m + n + o + p + q + r + s);
}

static void *ns_cpu_test_resolve(void *user, const char *module, const char *name) {
    ns_unused(user);
    if (strcmp(module, "mixt") != 0) return ns_null;
    if (strcmp(name, "mixt_a") == 0) return (void *)mixt_a;
    if (strcmp(name, "mixt_b") == 0) return (void *)mixt_b;
    if (strcmp(name, "mixt_c") == 0) return (void *)mixt_c;
    return ns_null;
}

// A call shim as src/ns_embedded_ffi.c generates them for hosts without libffi.
static f32 ns_cpu_test_f32(u64 v) {
    u32 bits = (u32)v;
    f32 f;
    memcpy(&f, &bits, 4);
    return f;
}

static u64 ns_cpu_test_shim_c(void *target, const u64 *a) {
    typedef i32 (*fn_t)(f64, i32, i32, i32, i32, i32, i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32, f32);
    f64 a0;
    memcpy(&a0, &a[0], 8);
    return (u64)(i64)((fn_t)target)(a0, (i32)a[1], (i32)a[2], (i32)a[3], (i32)a[4], (i32)a[5], (i32)a[6], (i32)a[7],
                                    (i32)a[8], (i32)a[9], ns_cpu_test_f32(a[10]), ns_cpu_test_f32(a[11]),
                                    ns_cpu_test_f32(a[12]), ns_cpu_test_f32(a[13]), ns_cpu_test_f32(a[14]),
                                    ns_cpu_test_f32(a[15]), ns_cpu_test_f32(a[16]), ns_cpu_test_f32(a[17]),
                                    ns_cpu_test_f32(a[18]));
}

static i32 ns_cpu_test_shim_calls;

static ns_cpu_native_call ns_cpu_test_resolve_call(void *user, const char *module, const char *name, void **target) {
    ns_unused(user);
    if (strcmp(module, "mixt") != 0 || strcmp(name, "mixt_c") != 0) return ns_null;
    *target = (void *)mixt_c;
    ns_cpu_test_shim_calls++;
    return ns_cpu_test_shim_c;
}

static void ns_cpu_test_native_calls(void) {
    char dir[] = "/tmp/ns_cpu_test.XXXXXX";
    if (!mkdtemp(dir)) {
        ns_expect(false, "a temporary module directory is created.");
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/mixt.ns", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("mod mixt\n"
          "ref fn mixt_a(a: i32, b: f64, c: i64, d: f32, e: i32, f: f64) f64\n"
          "ref fn mixt_b(a: f32, b: i32, c: f32) f32\n"
          "ref fn mixt_c(a: f64, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32, h: i32, i: i32, j: i32, "
          "k: f32, l: f32, m: f32, n: f32, o: f32, p: f32, q: f32, r: f32, s: f32) i32\n", f);
    fclose(f);
    const char *source =
        "use mixt\n"
        "fn call_a() f64 {\n"
        "    let d: f32 = 3.5\n"
        "    return mixt_a(1, 2.0, 3, d, 5, 6.0)\n"
        "}\n"
        "fn call_b() f64 { return mixt_b(1.5, 4, 0.25) }\n"
        "fn call_c() i32 { return mixt_c(1.0, 1, 1, 1, 1, 1, 1, 1, 1, 7, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1.0) }\n"
        "fn main() i32 { return 0 }\n";
    u8 *image = ns_cpu_test_image_at(source, "<ns_cpu_test_ffi>", ns_str_cstr(dir));
    remove(path);
    rmdir(dir);
    ns_expect(image != ns_null, "a program calling native functions lowers to an image.");
    if (!image) return;

    ns_cpu_host host = {.resolve = ns_cpu_test_resolve, .resolve_call = ns_cpu_test_resolve_call};
    ns_return_ptr loaded = ns_cpu_load(image, ns_array_length(image), &host, ns_null);
    ns_expect(!ns_return_is_error(loaded), "the image links against the host's native functions.");
    if (ns_return_is_error(loaded)) {
        ns_array_free(image);
        return;
    }
    ns_cpu_module *m = loaded.r;
    ns_expect(ns_cpu_test_shim_calls > 0, "the host call shim resolver is asked first.");
    i64 out = 0;
    ns_expect(ns_cpu_call(m, "call_a", ns_null, 0, &out).s == NS_OK && ns_cpu_test_f64(out) == 653821.0,
              "interleaved integer and float arguments reach the native function.");
    ns_expect(ns_cpu_call(m, "call_b", ns_null, 0, &out).s == NS_OK && ns_cpu_test_f64(out) == 6.25,
              "an f32 result widens as the program expects.");
    ns_expect(ns_cpu_call(m, "call_c", ns_null, 0, &out).s == NS_OK && out == 28,
              "a host call shim passes nineteen arguments.");
    ns_cpu_unload(m);
    ns_array_free(image);
}

static ns_bool ns_cpu_test_call(ns_cpu_module *m, const char *fn, i64 a, i64 b, i64 *out) {
    i64 args[2] = {a, b};
    return !ns_return_is_error(ns_cpu_call(m, fn, args, 2, out));
}

static f64 ns_cpu_test_f64(i64 bits) {
    f64 v;
    memcpy(&v, &bits, 8);
    return v;
}

static i64 ns_cpu_test_bits(f64 v) {
    i64 bits;
    memcpy(&bits, &v, 8);
    return bits;
}

int main(void) {
    const char *v1 =
        "use std\n"
        "let counter: i32 = 0\n"
        "let scale: f64 = 1.5\n"
        "struct pair { a: i32, b: i32 }\n"
        "fn bump(n: i32) i32 {\n"
        "    counter = counter + n\n"
        "    return counter\n"
        "}\n"
        "fn fib(n: i32) i32 {\n"
        "    if n < 2 { return n }\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "}\n"
        "fn scaled(x: f64) f64 { return x * scale }\n"
        "fn swap_loop(n: i32) i32 {\n"
        "    let a = 1\n"
        "    let b = 2\n"
        "    let i = 0\n"
        "    loop i < n {\n"
        "        let t = a\n"
        "        a = b\n"
        "        b = t\n"
        "        i += 1\n"
        "    }\n"
        "    return a * 10 + b\n"
        "}\n"
        "fn pairs(n: i32) i32 {\n"
        "    let items = [pair](n)\n"
        "    for i in 0 to n { items[i] = pair { i, i * 2 } }\n"
        "    let total = 0\n"
        "    for i in 0 to n { total = total + items[i].a + items[i].b }\n"
        "    return total\n"
        "}\n"
        "fn text(n: i32) i32 {\n"
        "    let s = `n={n}`\n"
        "    if s == \"n=42\" { return s.len }\n"
        "    return -1\n"
        "}\n"
        "fn div(a: i32, b: i32) i32 { return a / b }\n"
        "fn main() i32 { return 0 }\n";

    u8 *image = ns_cpu_test_image(v1, "<ns_cpu_test_v1>");
    ns_expect(image != ns_null, "source lowers to an ns_cpu image.");
    if (!image) return 1;
    szt size = ns_array_length(image);
    ns_expect(ns_cpu_image_is(image, size), "image carries the ns_cpu magic.");

    ns_return_ptr loaded = ns_cpu_load(image, size, ns_null, ns_null);
    ns_expect(!ns_return_is_error(loaded), "image verifies and loads.");
    if (ns_return_is_error(loaded)) return 1;
    ns_cpu_module *m = loaded.r;
    ns_expect(!ns_return_is_error(ns_cpu_run_init(m)), "module globals initialize.");

    i64 out = 0;
    ns_expect(ns_cpu_test_call(m, "fib", 20, 0, &out) && out == 6765, "recursive calls return fib(20).");
    ns_expect(ns_cpu_test_call(m, "bump", 3, 0, &out) && out == 3, "a global updates across calls.");
    ns_expect(ns_cpu_test_call(m, "bump", 4, 0, &out) && out == 7, "a global keeps its value between calls.");
    ns_expect(ns_cpu_test_call(m, "scaled", ns_cpu_test_bits(2.0), 0, &out) && ns_cpu_test_f64(out) == 3.0,
              "a typed float global initializes to its float value.");
    ns_expect(ns_cpu_test_call(m, "swap_loop", 3, 0, &out) && out == 21, "loop phis swap in parallel.");
    ns_expect(ns_cpu_test_call(m, "pairs", 10, 0, &out) && out == 135, "struct arrays store and load.");
    ns_expect(ns_cpu_test_call(m, "text", 42, 0, &out) && out == 4, "string formatting and compare.");
    ns_expect(!ns_cpu_test_call(m, "div", 1, 0, &out), "division by zero faults instead of crashing.");
    ns_expect(ns_cpu_test_call(m, "div", 9, 3, &out) && out == 3, "the module runs again after a fault.");
    ns_expect(ns_return_is_error(ns_cpu_call(m, "missing", ns_null, 0, &out)), "an unknown fn is an error.");

    // A damaged image never loads into something that can escape its frame:
    // every truncation and a spread of single-byte flips is either rejected
    // or verified. Loading must not crash either way.
    i32 rejected = 0;
    for (szt cut = 0; cut < size; cut += 13) {
        ns_return_ptr r = ns_cpu_load(image, cut, ns_null, ns_null);
        if (ns_return_is_error(r)) rejected++;
        else ns_cpu_unload(r.r);
    }
    ns_expect(rejected == (i32)((size + 12) / 13), "every truncated image is rejected.");
    u8 *damaged = malloc(size);
    for (szt at = 8; at < size; at += 7) {
        memcpy(damaged, image, size);
        damaged[at] ^= 0x5a;
        ns_return_ptr r = ns_cpu_load(damaged, size, ns_null, ns_null);
        if (!ns_return_is_error(r)) ns_cpu_unload(r.r);
    }
    free(damaged);
    ns_expect(true, "flipped bytes never crash the loader.");

    // Hot update: a second build of the program replaces the first one while
    // the process keeps running, and keeps the state the first one built up.
    const char *v2 =
        "use std\n"
        "let counter: i32 = 0\n"
        "let scale: f64 = 1.5\n"
        "fn bump(n: i32) i32 {\n"
        "    counter = counter + n * 100\n"
        "    return counter\n"
        "}\n"
        "fn twice() i32 { return counter * 2 }\n"
        "fn main() i32 { return 0 }\n";
    u8 *image2 = ns_cpu_test_image(v2, "<ns_cpu_test_v2>");
    ns_expect(image2 != ns_null, "the updated source lowers to an image.");
    if (!image2) return 1;
    ns_return_ptr loaded2 = ns_cpu_load(image2, ns_array_length(image2), ns_null, m);
    ns_expect(!ns_return_is_error(loaded2), "the updated image loads beside the running one.");
    if (ns_return_is_error(loaded2)) return 1;
    ns_cpu_module *m2 = loaded2.r;
    ns_cpu_unload(m);
    ns_expect(ns_cpu_call(m2, "twice", ns_null, 0, &out).s == NS_OK && out == 14,
              "globals carry over into the updated image.");
    ns_expect(ns_cpu_test_call(m2, "bump", 1, 0, &out) && out == 107, "the updated code runs.");
    ns_cpu_unload(m2);

    ns_array_free(image);
    ns_array_free(image2);

    ns_cpu_test_native_calls();
    return 0;
}
