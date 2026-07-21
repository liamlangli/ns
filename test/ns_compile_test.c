#include "ns_test.h"
#include "ns_ssa.h"

// Compile-mode tests. Where the interpreter tests (ns_expr_test.c) exercise the
// tree-walking evaluator, these drive the other half of the toolchain: the
// SSA → native code path. Each program is lowered to SSA, translated to host
// machine code, linked in memory, and executed, and its `main` return value is
// checked. Only the integer/bool/enum/control-flow subset the native backends
// support is covered; floats, strings, arrays, structs, closures and tasks are
// interpreter-only and are exercised by ns_expr_test.c.

#if defined(__x86_64__) || defined(__aarch64__)
#define NS_COMPILE_TEST_NATIVE 1
#endif

#ifdef NS_COMPILE_TEST_NATIVE
#include <sys/mman.h>

#if defined(__x86_64__)
#include "ns_amd64.h"
typedef ns_amd64_module_bin ns_native_module;
typedef ns_amd64_fn_bin ns_native_fn;
static ns_return_ptr ns_native_from_ssa(ns_ssa_module *m) { return ns_amd64_from_ssa(m); }
static void ns_native_free(ns_native_module *m) { ns_amd64_free(m); }
#else
#include "ns_aarch.h"
typedef ns_aarch_module_bin ns_native_module;
typedef ns_aarch_fn_bin ns_native_fn;
static ns_return_ptr ns_native_from_ssa(ns_ssa_module *m) { return ns_aarch_from_ssa(m); }
static void ns_native_free(ns_native_module *m) { ns_aarch_free(m); }
#endif

typedef i64 (*ns_compiled_main)(void);

// Patch a same-module relative call at `site` (byte offset of the call/branch)
// so it targets the function at byte offset `target`.
static void ns_compile_patch_call(u8 *buf, u64 site, u64 target) {
#if defined(__x86_64__)
    // CALL rel32: rel = target - (site + 4), the field sits right after 0xE8.
    i32 rel = (i32)((i64)target - (i64)(site + 4));
    buf[site + 0] = (u8)(rel & 0xFF);
    buf[site + 1] = (u8)((rel >> 8) & 0xFF);
    buf[site + 2] = (u8)((rel >> 16) & 0xFF);
    buf[site + 3] = (u8)((rel >> 24) & 0xFF);
#else
    // BL imm26: imm = (target - site) / 4.
    i32 imm26 = (i32)(((i64)target - (i64)site) / 4);
    u32 bl = 0x94000000u | ((u32)imm26 & 0x3FFFFFFu);
    buf[site + 0] = (u8)(bl & 0xFF);
    buf[site + 1] = (u8)((bl >> 8) & 0xFF);
    buf[site + 2] = (u8)((bl >> 16) & 0xFF);
    buf[site + 3] = (u8)((bl >> 24) & 0xFF);
#endif
}

// Compile `src`, execute its `main`, and report the returned value. On any
// parse/lower/link failure *ok is left false.
static i64 ns_compile_run(const char *src, ns_bool *ok) {
    *ok = false;
    ns_ast_ctx ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&ctx, ns_str_cstr((i8 *)src), ns_str_cstr("<ns_compile_test>"));
    if (ns_return_is_error(parsed) || !parsed.r) {
        ns_warn("ns_compile_test", "parse failed\n");
        return 0;
    }
    ns_return_ptr built = ns_ssa_build(&ctx);
    if (ns_return_is_error(built)) {
        ns_warn("ns_compile_test", "ssa build failed: %.*s\n", built.e.msg.len, built.e.msg.data);
        return 0;
    }
    ns_ssa_module *ssa = built.r;
    ns_return_ptr native = ns_native_from_ssa(ssa);
    if (ns_return_is_error(native)) {
        ns_ssa_module_free(ssa);
        ns_warn("ns_compile_test", "native lowering failed\n");
        return 0;
    }
    ns_native_module *m = native.r;

    i32 nfns = (i32)ns_array_length(m->fns);
    u64 *off = ns_malloc(sizeof(u64) * (nfns > 0 ? nfns : 1));
    u64 total = 0;
    for (i32 i = 0; i < nfns; ++i) {
        off[i] = total;
        total += ns_array_length(m->fns[i].text);
        total = (total + 15) & ~15ull; // keep each function 16-byte aligned
    }
    if (total == 0) total = 16;

    // Map writable first, fill and patch, then flip to read+execute so the test
    // works on hosts that forbid simultaneously writable+executable pages.
    u8 *buf = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        ns_free(off);
        ns_native_free(m);
        ns_ssa_module_free(ssa);
        ns_warn("ns_compile_test", "mmap failed\n");
        return 0;
    }
    for (i32 i = 0; i < nfns; ++i) {
        memcpy(buf + off[i], m->fns[i].text, ns_array_length(m->fns[i].text));
    }
    for (i32 fi = 0; fi < nfns; ++fi) {
        ns_native_fn *fn = &m->fns[fi];
        for (i32 ci = 0, cl = (i32)ns_array_length(fn->call_fixups); ci < cl; ++ci) {
            ns_str callee = fn->call_fixups[ci].callee;
            i32 cidx = -1;
            for (i32 k = 0; k < nfns; ++k) {
                if (ns_str_equals(m->fns[k].name, callee)) { cidx = k; break; }
            }
            if (cidx < 0) continue;
            ns_compile_patch_call(buf, off[fi] + fn->call_fixups[ci].off, off[cidx]);
        }
    }

    i64 main_off = -1;
    for (i32 i = 0; i < nfns; ++i) {
        if (ns_str_equals(m->fns[i].name, ns_str_cstr("main"))) { main_off = (i64)off[i]; break; }
    }

    i64 result = 0;
    if (main_off >= 0 && mprotect(buf, total, PROT_READ | PROT_EXEC) == 0) {
        __builtin___clear_cache((char *)buf, (char *)buf + total);
        ns_compiled_main fp = (ns_compiled_main)(buf + main_off);
        result = fp();
        *ok = true;
    } else {
        ns_warn("ns_compile_test", "no main / mprotect failed\n");
    }

    munmap(buf, total);
    ns_free(off);
    ns_native_free(m);
    ns_ssa_module_free(ssa);
    return result;
}

// Convenience for programs written as `fn main() bool`: the backend returns the
// boolean as 0/1 in the native return register.
static ns_bool ns_compile_true(const char *src) {
    ns_bool ok = false;
    i64 r = ns_compile_run(src, &ok);
    return ok && r == 1;
}

// Convenience for programs written as `fn main() i32`.
static ns_bool ns_compile_returns(const char *src, i64 expected) {
    ns_bool ok = false;
    i64 r = ns_compile_run(src, &ok);
    return ok && r == expected;
}

int main() {
    // ── additive / multiplicative operators, int ──────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return 2 + 3 == 5 && 2 - 3 == -1 && 2 * 3 == 6 && 7 / 2 == 3 && 7 % 2 == 1\n"
        "}\n"), "integer add sub mul div mod operators compile and run.");

    // ── precedence and parenthesisation ───────────────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return 2 + 3 * 4 == 14 && 20 - 6 / 2 == 17 && (2 + 3) * 4 == 20 && 2 * (3 + 4) == 14\n"
        "}\n"), "multiplicative over additive precedence, parens override.");

    // ── left associativity of - / % chains ────────────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return 10 - 3 - 2 == 5 && 100 / 5 / 2 == 10 && 7 / 2 * 2 == 6 && 2 - 3 + 4 == 3 && 20 % 7 % 4 == 2\n"
        "}\n"), "left-associative subtraction, division and modulo chains.");

    // ── integer division truncates toward zero; remainder keeps sign ──────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return -7 / 2 == -3 && 7 / -2 == -3 && -7 % 2 == -1 && -7 / 2 * 2 + -7 % 2 == -7\n"
        "}\n"), "negative integer division truncates toward zero.");

    // ── unary minus, including as a right operand and on a product ─────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return 2 * -3 == -6 && 5 - -2 == 7 && -2 - -3 == 1 && -(2 * 3) == -6\n"
        "}\n"), "unary minus as a right operand and on a product.");

    // ── arithmetic over variables: (a+b)*(a-b) == a*a - b*b ───────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    let a = 6\n"
        "    let b = 4\n"
        "    return (a + b) * (a - b) == a * a - b * b\n"
        "}\n"), "difference-of-squares identity over variables.");

    // ── bitwise and shift operators ───────────────────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return (5 & 3) == 1 && (5 | 2) == 7 && (5 ^ 1) == 4 && (1 << 3) == 8 && (8 >> 2) == 2\n"
        "}\n"), "bitwise & | ^ and shift << >> operators.");

    // ── parenthesized shifts composed with arithmetic ─────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return (1 << 4) - 1 == 15 && (255 >> 4) + 1 == 16\n"
        "}\n"), "parenthesized shifts composed with arithmetic.");

    // ── relational / equality operators, numbers ──────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return 3 < 5 && 5 <= 5 && 5 > 3 && 5 >= 5 && 5 == 5 && 5 != 3\n"
        "}\n"), "numeric relational and equality operators.");

    // ── bool equality operators ───────────────────────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return (true == true) && (false == false) && (true == false) == false && (true != false)\n"
        "}\n"), "bool equality operators.");

    // ── logical operators (both operands are pure, so no short-circuit) ────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    return (true && true) && (true && false) == false && (false || true) && (false || false) == false\n"
        "}\n"), "logical && || operators.");

    // ── unary logical not on a bool variable and a parenthesized expr ─────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    let a = true\n"
        "    let b = false\n"
        "    return (!a == false) && !b && !(1 == 2) && (!(1 == 1) == false)\n"
        "}\n"), "unary not on bool variables and parenthesized expressions.");

    // ── cast results as arithmetic operands (integer casts) ───────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    let i = 7\n"
        "    let w = i as i64\n"
        "    return (w as i32) == 7 && (300 as u8 as i32) == 44\n"
        "}\n"), "integer casts flow through arithmetic.");

    // ── function calls, including nested numeric calls ────────────────────
    ns_expect(ns_compile_true(
        "fn center_y(height: i32) i32 { return height - 54 }\n"
        "fn side(width: i32, height: i32) i32 { return center_y(height) + width }\n"
        "fn menu(width: i32, height: i32) i32 { return side(width, height) }\n"
        "fn main() bool { return menu(402, 874) == 1222 }\n"),
        "nested numeric function calls compile and run.");

    // ── argument evaluation resolves to caller bindings, not just-bound
    // parameters of the same name (the render.ns regression, integer form). ──
    ns_expect(ns_compile_true(
        "fn pick(a: i32, b: i32) i32 { return b }\n"
        "fn main() bool {\n"
        "    let a = 5\n"
        "    return pick(a + 100, a) == 5\n"
        "}\n"), "bare-identifier arg resolves to the caller binding.");

    ns_expect(ns_compile_true(
        "fn three(a: i32, b: i32, c: i32) i32 { return a * 100 + b * 10 + c }\n"
        "fn main() bool {\n"
        "    let a = 1\n"
        "    let b = 2\n"
        "    return three(a + b, a, b) == 312\n"
        "}\n"), "multiple colliding args resolve to caller bindings.");

    // ── recursion ─────────────────────────────────────────────────────────
    ns_expect(ns_compile_true(
        "fn fib(n: i32) i32 {\n"
        "    if n < 2 { return n }\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "}\n"
        "fn main() bool { return fib(10) == 55 }\n"),
        "recursive functions compile and run.");

    ns_expect(ns_compile_true(
        "fn fac(n: i32) i32 {\n"
        "    if n < 2 { return 1 }\n"
        "    return n * fac(n - 1)\n"
        "}\n"
        "fn main() bool { return fac(6) == 720 }\n"),
        "recursion with many live temporaries compiles and runs.");

    // ── if / else, including a value that differs per branch (phi) ─────────
    ns_expect(ns_compile_true(
        "fn classify(n: i32) i32 {\n"
        "    let r = 0\n"
        "    if n < 0 { r = -1 } else { if n > 0 { r = 1 } else { r = 0 } }\n"
        "    return r\n"
        "}\n"
        "fn main() bool { return classify(-8) == -1 && classify(0) == 0 && classify(8) == 1 }\n"),
        "if/else branches merge divergent values through phis.");

    // ── pre-test loop, with per-iteration locals and back-edge updates ────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    let x = 0\n"
        "    let i = 0\n"
        "    loop i < 10 {\n"
        "        x = x + i\n"
        "        i = i + 1\n"
        "    }\n"
        "    return x == 45\n"
        "}\n"), "pre-test loop accumulates across iterations.");

    ns_expect(ns_compile_true(
        "fn rev_digits(n: i32) i32 {\n"
        "    let out = 0\n"
        "    let v = n\n"
        "    loop v > 0 {\n"
        "        let d = v % 10\n"
        "        out = out * 10 + d\n"
        "        v = v / 10\n"
        "    }\n"
        "    return out\n"
        "}\n"
        "fn main() bool { return rev_digits(102) == 201 }\n"),
        "loop body locals and reductions compile and run.");

    // ── for range loop, with an accumulator carried by a header phi ───────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    let x = 0\n"
        "    for i in 0 to 10 { x = x + i }\n"
        "    return x == 45\n"
        "}\n"), "for-range loop accumulates through a header phi.");

    ns_expect(ns_compile_true(
        "fn base() i32 { return 0 }\n"
        "fn limit() i32 { return 2 }\n"
        "fn main() bool {\n"
        "    let sum = 0\n"
        "    for i in base() to limit() { sum = sum + i + 1 }\n"
        "    return sum == 3\n"
        "}\n"), "for-range bounds may themselves be calls.");

    // ── nested loops ──────────────────────────────────────────────────────
    ns_expect(ns_compile_true(
        "fn main() bool {\n"
        "    let total = 0\n"
        "    for i in 0 to 3 {\n"
        "        for j in 0 to 3 { total = total + i * j }\n"
        "    }\n"
        "    return total == 9\n"
        "}\n"), "nested for-range loops compile and run.");

    // ── enums: members, integer lowering, casts, constant folding ─────────
    ns_expect(ns_compile_true(
        "enum os_platform: u8 { unknown = 0, macos, linux = macos + 4, other, }\n"
        "enum signed_code { low = -2, next, mask = (1 << 4) | 3, }\n"
        "fn identity(value: os_platform) os_platform { return value }\n"
        "fn main() bool {\n"
        "    let platform = identity(os_platform.macos)\n"
        "    let raw: u8 = platform\n"
        "    let restored = raw as os_platform\n"
        "    return raw == 1 && restored == os_platform.macos && os_platform.other == 6 &&\n"
        "           signed_code.next == -1 && signed_code.mask == 19 &&\n"
        "           (os_platform.linux | 2) == 7 && os_platform.linux > platform\n"
        "}\n"), "enum members lower to integers, cast, fold and compare.");

    // ── wide (u64) enum constant crosses the imm64 path ───────────────────
    ns_expect(ns_compile_true(
        "enum wide_mask: u64 { none = 0, all = 18446744073709551615, }\n"
        "fn value() u64 { return wide_mask.all }\n"
        "fn main() bool {\n"
        "    let restored = value() as wide_mask\n"
        "    return restored == wide_mask.all && wide_mask.none == 0\n"
        "}\n"), "64-bit enum constants survive the wide-immediate path.");

    ns_expect(ns_compile_true(
        "use os\n"
        "lit base = 6 * 7\n"
        "fn main() bool {\n"
        "    lit answer = base\n"
        "    return answer == 42 && OS_PLATFORM_MACOS == 1\n"
        "}\n"), "global, local, and imported lit constants compile and run.");

    // ── a small end-to-end program returning a non-boolean status ─────────
    ns_expect(ns_compile_returns(
        "fn gcd(a: i32, b: i32) i32 {\n"
        "    let x = a\n"
        "    let y = b\n"
        "    loop y != 0 {\n"
        "        let t = x % y\n"
        "        x = y\n"
        "        y = t\n"
        "    }\n"
        "    return x\n"
        "}\n"
        "fn main() i32 { return gcd(48, 36) }\n", 12),
        "iterative gcd compiles and returns its integer result.");

    return 0;
}

#else // !NS_COMPILE_TEST_NATIVE

int main() {
    ns_info("SKIP", "compile-mode execution tests require an x86-64 or aarch64 host\n");
    return 0;
}

#endif
