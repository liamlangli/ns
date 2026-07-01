#include "ns_test.h"
#include "ns_vm.h"

// Evaluate a self-contained ns program whose `main` returns a bool, and report
// that boolean back to the C harness. A parse/eval error counts as a failure.
static ns_bool ns_expr_eval_bool(const char *src) {
    ns_vm vm = {0};
    ns_return_value r = ns_eval(&vm, ns_str_cstr((i8 *)src), ns_str_cstr("<ns_expr_test>"));
    if (ns_return_is_error(r)) {
        ns_warn("ns_expr_test", "eval error: %.*s\n", r.e.msg.len, r.e.msg.data);
        return false;
    }
    return ns_eval_bool(&vm, r.r);
}

int main() {
    // Regression: a call argument that is a bare identifier must resolve to the
    // caller's binding, not to a parameter of the same name that an earlier
    // argument of the same call already bound. Before the fix, `pick(a + 100, a)`
    // bound parameter `a` to 105 while evaluating the second argument, so the
    // bare `a` read 105 instead of the caller's 5.
    {
        const char *src =
            "fn pick(a: i32, b: i32) i32 { return b }\n"
            "fn main() bool {\n"
            "    let a = 5\n"
            "    return pick(a + 100, a) == 5\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "bare-identifier arg resolves to caller binding.");
    }

    // Same defect across three colliding parameters: every argument is evaluated
    // in the caller's scope, independent of the parameter names being bound.
    {
        const char *src =
            "fn three(a: i32, b: i32, c: i32) i32 { return a * 100 + b * 10 + c }\n"
            "fn main() bool {\n"
            "    let a = 1\n"
            "    let b = 2\n"
            "    return three(a + b, a, b) == 312\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "multiple colliding args resolve to caller bindings.");
    }

    // The original nscode/render.ns symptom: a struct-literal field initializer
    // that references a parameter (`y`) after an earlier argument bound a
    // same-named parameter. `center_v(x + 86.0, x, ...)` must place y at 11, not 97.
    {
        const char *src =
            "struct rect { x: f64, y: f64 }\n"
            "fn center_v(x: f64, y: f64, h: f64, ch: f64) rect {\n"
            "    return rect { x: x, y: y + (h - ch) * 0.5 }\n"
            "}\n"
            "fn main() bool {\n"
            "    let x = 0.0\n"
            "    let box = center_v(x + 86.0, x, 38.0, 16.0)\n"
            "    return box.y == 11.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "struct-literal field initializer uses caller value.");
    }

    // Sanity: ordinary left-associative float arithmetic through call arguments
    // keeps working (`x + w - 1.0`).
    {
        const char *src =
            "fn last(x: f64, w: f64) f64 { return x + w - 1.0 }\n"
            "fn main() bool {\n"
            "    return last(40.0, 200.0) == 239.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "chained additive float arithmetic in args.");
    }

    // --- additive / multiplicative operators, int and float ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return 2 + 3 == 5 && 2 - 3 == -1 && 2 * 3 == 6 && 7 / 2 == 3 && 7 % 2 == 1\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "integer add sub mul div mod operators.");
    }
    {
        const char *src =
            "fn main() bool {\n"
            "    return 2.0 + 3.0 == 5.0 && 7.0 / 2.0 == 3.5 && 7.0 % 2.0 == 1.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "float add div mod operators (mod via fmod).");
    }

    // --- bitwise and shift operators ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return (5 & 3) == 1 && (5 | 2) == 7 && (5 ^ 1) == 4 && (1 << 3) == 8 && (8 >> 2) == 2\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "bitwise & | ^ and shift << >> operators.");
    }

    // --- relational / equality operators, numbers ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return 3 < 5 && 5 <= 5 && 5 > 3 && 5 >= 5 && 5 == 5 && 5 != 3\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "numeric relational and equality operators.");
    }

    // --- bool equality/relational regression: eval's cmp-op macro had no
    // NS_TYPE_BOOL case, so every bool comparison silently returned false. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return (true == true) && (false == false) && (true == false) == false && (true != false)\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "bool equality operators (regression: always returned false).");
    }

    // --- logical operators ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return (true && true) && (true && false) == false && (false || true) && (false || false) == false\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "logical && || operators.");
    }

    // --- unary minus ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let n = 5\n"
            "    let f = 2.5\n"
            "    return -n == -5 && -f == -2.5\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary minus on int and float.");
    }

    // --- unary logical not (bool variable): reads the operand through the
    // stack, not the immediate union field. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let a = true\n"
            "    let b = false\n"
            "    return (!a == false) && !b\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary not on a bool variable.");
    }

    // --- unary not on a parenthesized compound expression: `!(a == b)`. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return !(1 == 2) && !(3 < 1) && (!(1 == 1) == false)\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary not on a parenthesized expr.");
    }

    // --- unary not on call and member operands, plus double negation. ---
    {
        const char *src =
            "struct flags { on: bool }\n"
            "fn ready() bool { return false }\n"
            "fn main() bool {\n"
            "    let f = flags { on: false }\n"
            "    return !ready() && !f.on && !!true\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary not on call/member operands and double negation.");
    }

    // --- unary minus on a parenthesized expression (fixed alongside not). ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return -(1 + 2) == -3\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary minus on a parenthesized expr.");
    }

    // --- unary bitwise not `~` on integers: ~x == -x - 1, and it composes with
    // other bitwise ops, nesting, and parenthesised operands. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let a = 5\n"
            "    return ~a == -6 && ~0 == -1 && (a & ~1) == 4 && ~~a == 5 && ~(2 + 3) == -6\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary bitwise not on integers.");
    }


    // --- cast expr ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let i = 7\n"
            "    let f = i as f64\n"
            "    let f2 = 7.9\n"
            "    let i2 = f2 as i32\n"
            "    return f == 7.0 && i2 == 7\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "cast expr between int and float.");
    }

    // --- desig (struct literal) + member expr ---
    {
        const char *src =
            "struct point { x: f64, y: f64 }\n"
            "fn main() bool {\n"
            "    let p = point { x: 1.0, y: 2.0 }\n"
            "    return p.x == 1.0 && p.y == 2.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "struct literal (desig expr) and member access.");
    }

    // --- index expr on array, plus array .len member ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let arr = [i32](3)\n"
            "    arr[0] = 10\n"
            "    arr[1] = 20\n"
            "    arr[2] = 30\n"
            "    return arr[1] == 20 && arr.len == 3\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "array index expr and .len member.");
    }

    // --- ref param mutation ---
    {
        const char *src =
            "struct point { x: f64, y: f64 }\n"
            "fn add_x(v: ref point, dx: f64) void {\n"
            "    v.x = v.x + dx\n"
            "}\n"
            "fn main() bool {\n"
            "    let p = point { x: 5.0, y: 0.0 }\n"
            "    add_x(p, 3.0)\n"
            "    return p.x == 8.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "ref param mutates caller's struct.");
    }

    // --- block/closure expr: no captures, stored via typed var-def, then called.
    // Regression: ns_eval_local_var_def only accepted an exact type-index match,
    // but a closure's own synthesized NS_TYPE_FN symbol never equals a declared
    // `type X = (...) -> ...` alias's index even when signatures match, so this
    // always failed with "local var def type mismatch." ---
    {
        const char *src =
            "type unary_op = (i32) -> i32\n"
            "fn main() bool {\n"
            "    let double: unary_op = { x in\n"
            "        return x * 2\n"
            "    }\n"
            "    return double(21) == 42\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "no-capture closure stored via typed var-def (regression).");
    }

    // --- block/closure expr passed directly as a call argument ---
    {
        const char *src =
            "type op_fn = (i32, i32) -> i32\n"
            "fn do_op(op: op_fn, a: i32, b: i32) i32 { return op(a, b) }\n"
            "fn main() bool {\n"
            "    return do_op({ a, b in\n"
            "        return a - b\n"
            "    }, 5, 3) == 2\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "closure passed directly as a call argument.");
    }

    // --- block/closure expr that captures an outer local, passed as a call arg.
    // Regression: ns_eval_block_expr read the capture-struct size from the wrong
    // union member (sym->st.stride instead of sym->bc.st.stride), which is never
    // populated for a block symbol, handing ns_eval_alloc a garbage size and
    // corrupting the interpreter's stack/heap. ---
    {
        const char *src =
            "type unary_op = (i32) -> i32\n"
            "fn apply(v: i32, op: unary_op) i32 { return op(v) }\n"
            "fn main() bool {\n"
            "    let bias = 100\n"
            "    let r = apply(5, { a in\n"
            "        return a + bias\n"
            "    })\n"
            "    return r == 105\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "capturing closure as call arg (regression: wrong union stride).");
    }

    // --- capturing closure stored via typed var-def (combines both fixes) ---
    {
        const char *src =
            "type unary_op = (i32) -> i32\n"
            "fn main() bool {\n"
            "    let sum = 7\n"
            "    let bias = 1\n"
            "    let capture: unary_op = { a in\n"
            "        return a + sum + bias\n"
            "    }\n"
            "    return capture(5) == 13\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "capturing closure stored via typed var-def.");
    }

    // --- string interpolation / format expr ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let name = \"ns\"\n"
            "    let n = 2\n"
            "    let s = `hello {name} {n + 1}`\n"
            "    return s == \"hello ns 3\"\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "string format (interpolation) expr.");
    }

    // --- string relational ordering regression: eval treated every REL_OP
    // ('<','<=','>','>=') as a plain equality check, so "a" < "b" returned
    // false and "b" > "a" also returned false. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return (\"a\" < \"b\") && (\"b\" < \"a\") == false && (\"a\" <= \"a\") && (\"b\" > \"a\") && (\"a\" > \"b\") == false\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "string relational ordering (regression: always equality).");
    }

    return 0;
}
