#include "ns_test.h"
#include "ns_cpu.h"
#include "ns_ssa.h"

// ns_cpu: lower source to an image, run it, reject damaged images, and carry
// program state across a hot update.

static u8 *ns_cpu_test_image(const char *source, const char *name) {
    ns_ast_ctx *ctx = calloc(1, sizeof(ns_ast_ctx));
    ns_return_bool parsed = ns_ast_parse(ctx, ns_str_cstr((i8 *)source), ns_str_cstr((i8 *)name));
    if (ns_return_is_error(parsed)) return ns_null;
    // The native lowering, as `ns run --cpu` uses; ns_ssa_build is the Wasm one.
    ns_return_ptr built = ns_ssa_build_with_runtime_paths_options(ctx, ns_str_null, ns_str_null, ns_str_null, false);
    if (ns_return_is_error(built)) return ns_null;
    ns_return_ptr image = ns_cpu_image_from_ssa(built.r);
    ns_ssa_module_free(built.r);
    return ns_return_is_error(image) ? ns_null : image.r;
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
    return 0;
}
