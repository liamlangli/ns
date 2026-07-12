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
    {
        ns_vm vm = {0};
        ns_vm_set_ref_path(&vm, ns_str_cstr("test/ref"));
        const char *src =
            "use std\n"
            "fn main() bool { return sqrt(9.0d) == 3.0d }\n";
        ns_return_value r = ns_eval(&vm, ns_str_cstr((i8 *)src), ns_str_cstr("<ref-path-test>"));
        ns_expect(!ns_return_is_error(r) && ns_eval_bool(&vm, r.r),
                  "VM resolves modules from a configured reference path.");
    }

    {
        const char *src =
            "fn main() bool {\n"
            "    let arr = [1, 2, 3]\n"
            "    return arr.len == 3 && arr[0] == 1 && arr[1] == 2 && arr[2] == 3\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "array literal infers element type and supports indexing.");
    }

    {
        const char *src =
            "fn main() bool {\n"
            "    let arr: [f32] = [0, 1, 2]\n"
            "    return arr.len == 3 && arr[0] == 0.0 && arr[1] == 1.0 && arr[2] == 2.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "array literal adopts declared array element type.");
    }

    {
        const char *src =
            "fn main() bool {\n"
            "    let arr = [f32](3)\n"
            "    arr = [0.0, 1.0, 2.0]\n"
            "    return arr.len == 3 && arr[0] == 0.0 && arr[1] == 1.0 && arr[2] == 2.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "array literal works as an assignment right-hand side.");
    }

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
            "fn takes_i32(x: i32) bool { return x == 1 }\n"
            "fn takes_f32(x: f32) bool { return x == 1.5 }\n"
            "fn main() bool {\n"
            "    let i = 1\n"
            "    let f = 1.5\n"
            "    return takes_i32(i) && takes_f32(f)\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unsuffixed literals infer to i32 and f32 by default.");
    }
    {
        const char *src =
            "fn takes_i8(x: i8) bool { return x == 1b }\n"
            "fn takes_u8(x: u8) bool { return x == 255ub }\n"
            "fn takes_i16(x: i16) bool { return x == 123s }\n"
            "fn takes_u16(x: u16) bool { return x == 456us }\n"
            "fn takes_u32(x: u32) bool { return x == 789u }\n"
            "fn takes_i64(x: i64) bool { return x == 100000l }\n"
            "fn takes_u64(x: u64) bool { return x == 100000ul }\n"
            "fn takes_f64(x: f64) bool { return x == 1.25d }\n"
            "fn main() bool {\n"
            "    let a = 1b\n"
            "    let b = 255ub\n"
            "    let c = 123s\n"
            "    let d = 456us\n"
            "    let e = 789u\n"
            "    let f = 100000l\n"
            "    let g = 100000ul\n"
            "    let h = 1.25d\n"
            "    return takes_i8(a) && takes_u8(b) && takes_i16(c) && takes_u16(d) && takes_u32(e) && takes_i64(f) && takes_u64(g) && takes_f64(h)\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "numeric literal suffixes infer explicit widths.");
    }
    {
        const char *src =
            "fn takes_f32(x: f32) bool { return x == 1.5 }\n"
            "fn main() bool {\n"
            "    let h = 1.5h\n"
            "    let hb = 1.5hb\n"
            "    return takes_f32(h) && takes_f32(hb)\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "half and brain-float literals fall back to f32 on CPU.");
    }
    {
        const char *src =
            "fn main() bool {\n"
            "    return 2.0 + 3.0 == 5.0 && 7.0 / 2.0 == 3.5 && 7.0 % 2.0 == 1.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "float add div mod operators (mod via fmod).");
    }

    // --- multiplicative binds tighter than additive, parens override ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return 2 + 3 * 4 == 14 && 20 - 6 / 2 == 17 && (2 + 3) * 4 == 20 && 2 * (3 + 4) == 14\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "multiplicative over additive precedence, parens override.");
    }

    // --- left associativity of - / % chains ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return 10 - 3 - 2 == 5 && 100 / 5 / 2 == 10 && 7 / 2 * 2 == 6 && 2 - 3 + 4 == 3 && 20 % 7 % 4 == 2\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "left-associative subtraction, division and modulo chains.");
    }
    {
        const char *src =
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
            "fn main() bool {\n"
            "    return rev_digits(102) == 201\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "loop body locals are scoped per iteration.");
    }

    // --- integer division truncates toward zero; remainder keeps the dividend's
    // sign, so (a / b) * b + a % b == a holds for negative operands too. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return -7 / 2 == -3 && 7 / -2 == -3 && -7 % 2 == -1 && -7 / 2 * 2 + -7 % 2 == -7\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "negative integer division truncates toward zero.");
    }

    // --- unary minus as the right operand of a binary operator ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return 2 * -3 == -6 && 5 - -2 == 7 && -2 - -3 == 1 && -(2 * 3) == -6\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "unary minus as a right operand and on a product.");
    }

    // --- float precedence, negative factors, fmod sign follows the dividend ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return 1.0 / 4.0 + 0.25 == 0.5 && -1.5 * -2.0 == 3.0 && 2.0 * 3.0 + 1.5 == 7.5 && -7.5 % 2.0 == -1.5\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "float precedence, negative factors and fmod sign.");
    }

    // --- arithmetic over variables: (a + b) * (a - b) == a*a - b*b ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let a = 6\n"
            "    let b = 4\n"
            "    return (a + b) * (a - b) == a * a - b * b\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "difference-of-squares identity over variables.");
    }

    // --- parenthesized shifts composed with arithmetic ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return (1 << 4) - 1 == 15 && (255 >> 4) + 1 == 16\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "parenthesized shifts composed with arithmetic.");
    }

    // --- cast results as arithmetic operands ---
    {
        const char *src =
            "fn main() bool {\n"
            "    return (7 as f64) / 2.0 == 3.5 && (7.9 as i32) * 2 == 14\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "cast results as arithmetic operands.");
    }

    // Regression: casting a stack-backed inferred i64 local to an i32 return
    // type must produce an immediate i32 value, not an immediate value still
    // tagged as stack-backed.
    {
        const char *src =
            "fn one() i32 {\n"
            "    let x = 1\n"
            "    return x\n"
            "}\n"
            "fn inc() i32 {\n"
            "    let x = 1\n"
            "    x = x + 1\n"
            "    return x\n"
            "}\n"
            "fn main() bool {\n"
            "    return one() == 1 && inc() == 2\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "numeric return cast from inferred local.");
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

    // --- single-line function body containing an assignment statement
    // (regression): a body written on one line with the open brace on the same
    // line ({ v.x = 1.0 }) used to fail to parse with "expected function body",
    // because the expression-statement parser required a newline/EOF terminator
    // and rejected the closing brace. It must now parse, compile and run like its
    // multiline equivalent. ---
    {
        const char *src =
            "struct point { x: f64, y: f64 }\n"
            "fn set_x(v: ref point) void { v.x = 1.0 }\n"
            "fn main() bool {\n"
            "    let p = point { x: 0.0, y: 0.0 }\n"
            "    set_x(ref p)\n"
            "    return p.x == 1.0\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "single-line function body with an assignment statement.");
    }

    // --- utf8 source text: str is utf8-encoded by default, so multibyte
    // identifiers and string literals flow through tokenize/parse/eval
    // unchanged, including as format-string interpolation inputs. ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let 答案 = 40\n"
            "    let 名字 = \"世界\"\n"
            "    let s = `你好 {名字} {答案 + 2}`\n"
            "    return 答案 + 2 == 42 && s == \"你好 世界 42\"\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "utf8 identifiers and string literals through parse and eval.");
    }

    // --- escaped quotes inside string literals (tokenizer regression: the
    // literal used to end at the first quote even when escaped, which made
    // everything after `say \` tokenize as code and fail to parse). ---
    {
        const char *src =
            "fn main() bool {\n"
            "    let s = \"say \\\"hi\\\"\"\n"
            "    let t = \"ok\"\n"
            "    return t == \"ok\"\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "escaped quote stays inside a string literal.");
    }

    // --- std.utf8_len counts codepoints while byte indexing stays byte-based ---
    {
        const char *src =
            "use std\n"
            "fn main() bool {\n"
            "    return utf8_len(\"你好ns\") == 4 && utf8_len(\"ns\") == 2\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "std.utf8_len counts utf8 codepoints.");
    }

    // --- std file handles survive storage in locals. close(fd) must read the
    // u64 handle value, not the stack slot offset of the local variable. ---
    {
        FILE *f = fopen("bin/ns_std_file_test.txt", "wb");
        ns_expect(f != NULL, "test fixture file opens for writing.");
        if (f) {
            fwrite("hello profile", 1, 13, f);
            fclose(f);
        }

        const char *src =
            "use std\n"
            "fn main() bool {\n"
            "    let fd = open(\"bin/ns_std_file_test.txt\", \"rb\")\n"
            "    if fd == 0 {\n"
            "        return false\n"
            "    }\n"
            "    let data = read(fd)\n"
            "    close(fd)\n"
            "    return data == \"hello profile\"\n"
            "}\n";
        ns_expect(ns_expr_eval_bool(src), "std open/read/close handles stored in locals.");
    }

    return 0;
}
