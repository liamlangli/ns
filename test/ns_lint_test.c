#include "ns_test.h"
#include "ns_lint.h"

static i32 ns_lint_test_count(const ns_lint_config *cfg, const char *src, ns_lint_rule rule) {
    ns_lint_result result = {0};
    ns_lint_source(cfg, ns_str_cstr((i8 *)src), ns_str_cstr("<lint_test>"), &result);
    i32 count = 0;
    for (i32 i = 0, l = (i32)ns_array_length(result.diagnostics); i < l; i++) {
        if (result.diagnostics[i].rule == rule) count++;
    }
    ns_lint_result_free(&result);
    return count;
}

static i32 ns_lint_test_total(const ns_lint_config *cfg, const char *src) {
    ns_lint_result result = {0};
    ns_lint_source(cfg, ns_str_cstr((i8 *)src), ns_str_cstr("<lint_test>"), &result);
    i32 count = (i32)ns_array_length(result.diagnostics);
    ns_lint_result_free(&result);
    return count;
}

// Rewrite `src` and compare the result byte for byte. A rule that reports
// without rewriting leaves the source untouched, which reads as equal here.
static ns_bool ns_lint_test_fix_is(const ns_lint_config *cfg, const char *src, const char *expect) {
    ns_lint_result result = {0};
    ns_str fixed = ns_lint_fix(cfg, ns_str_cstr((i8 *)src), ns_str_cstr("<lint_test>"), &result);
    ns_str out = fixed.data != ns_null ? fixed : ns_str_cstr((i8 *)src);
    ns_bool same = out.len == (i32)strlen(expect) && strncmp(out.data, expect, out.len) == 0;
    if (!same) printf("  got: %.*s\n  want: %s\n", out.len, out.data, expect);
    ns_array_free(fixed.data);
    ns_lint_result_free(&result);
    return same;
}

// A fixed source must be a fixed point: the second pass finds nothing to do.
static ns_bool ns_lint_test_stable(const ns_lint_config *cfg, const char *src) {
    ns_lint_result first = {0};
    ns_str fixed = ns_lint_fix(cfg, ns_str_cstr((i8 *)src), ns_str_cstr("<lint_test>"), &first);
    ns_str out = fixed.data != ns_null ? fixed : ns_str_cstr((i8 *)src);
    ns_lint_result second = {0};
    ns_str again = ns_lint_fix(cfg, out, ns_str_cstr("<lint_test>"), &second);
    ns_bool stable = again.data == ns_null && second.fixed == 0;
    if (!stable) printf("  unstable: %.*s\n", again.data != ns_null ? again.len : 0, again.data);
    ns_array_free(again.data);
    ns_array_free(fixed.data);
    ns_lint_result_free(&first);
    ns_lint_result_free(&second);
    return stable;
}

int main() {
    ns_lint_config cfg;
    ns_lint_config_default(&cfg);

    // ---- binary operators -------------------------------------------------
    {
        const char *src = "fn main() {\n    let a = 1+2\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_BINARY_OP_SPACE) == 2, "a binary operator reports both of its sides.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n    let a = 1 + 2\n}\n"), "the fixer spaces a binary operator.");
    }

    {
        const char *src = "fn main() {\n    let a  =   1\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_BINARY_OP_SPACE) == 2, "extra space around an operator reports too.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n    let a = 1\n}\n"), "the fixer collapses padding to one space.");
    }

    // A unary sign is not a binary operator, in every position that starts an
    // operand.
    {
        const char *src = "fn main() {\n    let a = -1\n    let b = f(-1, ~2)\n    let c = [-1]\n    let d = a - -1\n    return -a\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_BINARY_OP_SPACE) == 0, "a unary sign needs no surrounding space.");
    }

    // An operator ending a line continues the expression on the next one, so
    // only its left side is checked.
    {
        const char *src = "fn main() {\n    let a = 1 +\n        2\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_BINARY_OP_SPACE) == 0, "a line-continuing operator keeps its line break.");
    }

    // Operators inside a string or a comment are payload, not code.
    {
        const char *src = "fn main() {\n    print(\"a+b\") // c+d\n}\n";
        ns_expect(ns_lint_test_total(&cfg, src) == 0, "strings and comments are left alone.");
    }

    // ---- separators -------------------------------------------------------
    {
        const char *src = "fn main() {\n    f(1 ,2)\n    let p: point = q\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_COMMA_SPACE) == 2, "a comma binds left and takes one space right.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n    f(1, 2)\n    let p: point = q\n}\n"), "the fixer tidies a comma.");
    }

    {
        const char *src = "struct point {\n    x :f32,\n    y: f32\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_COLON_SPACE) == 2, "a type label colon binds left and takes one space right.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "struct point {\n    x: f32,\n    y: f32\n}\n"), "the fixer tidies a colon.");
    }

    // ---- whitespace -------------------------------------------------------
    {
        const char *src = "fn main() {   \n    let a = 1\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_TRAILING_SPACE) == 1, "trailing whitespace reports once per line.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n    let a = 1\n}\n"), "the fixer strips trailing whitespace.");
    }

    {
        ns_expect(ns_lint_test_count(&cfg, "fn main() {\n}", NS_LINT_TRAILING_SPACE) == 1, "a missing final newline reports.");
        ns_expect(ns_lint_test_fix_is(&cfg, "fn main() {\n}", "fn main() {\n}\n"), "the fixer terminates the last line.");
        ns_expect(ns_lint_test_fix_is(&cfg, "fn main() {\n}\n\n\n", "fn main() {\n}\n"), "the fixer drops blank lines at end of file.");
        ns_expect(ns_lint_test_count(&cfg, "fn main() {\n}\n", NS_LINT_TRAILING_SPACE) == 0, "one final newline is what a file wants.");
    }

    // A CRLF file keeps its line endings.
    {
        ns_expect(ns_lint_test_fix_is(&cfg, "fn main() {\r\n}", "fn main() {\r\n}\r\n"), "the fixer follows the file's line ending.");
    }

    {
        const char *src = "fn main() {\n\tlet a = 1\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_TAB_INDENT) == 1, "a tab indent reports.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n    let a = 1\n}\n"), "the fixer expands a tab to the indent width.");
        cfg.indent = 2;
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n  let a = 1\n}\n"), "the configured indent width sets the expansion.");
        cfg.indent = 4;
    }

    // ---- struct literal labels -------------------------------------------
    {
        const char *decl = "struct point {\n    x: f32,\n    y: f32\n}\n";
        char src[512];
        snprintf(src, sizeof(src), "%sfn main() {\n    let p = point { x: 1, y: 2 }\n}\n", decl);
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_STRUCT_LABEL) == 1, "a fully ordered struct literal reports its labels.");

        char expect[512];
        snprintf(expect, sizeof(expect), "%sfn main() {\n    let p = point { 1, 2 }\n}\n", decl);
        ns_expect(ns_lint_test_fix_is(&cfg, src, expect), "the fixer drops the labels of an ordered literal.");

        snprintf(src, sizeof(src), "%sfn main() {\n    let p = point { y: 2, x: 1 }\n}\n", decl);
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_STRUCT_LABEL) == 0, "an out-of-order literal keeps its labels.");

        snprintf(src, sizeof(src), "%sfn main() {\n    let p = point { x: 1 }\n}\n", decl);
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_STRUCT_LABEL) == 0, "a partial literal keeps its labels.");

        snprintf(src, sizeof(src), "%sfn main() {\n    let p = point { 1, 2 }\n}\n", decl);
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_STRUCT_LABEL) == 0, "a positional literal has nothing to drop.");

        // Only declared structs are literals; `if flag { ... }` is a statement.
        snprintf(src, sizeof(src), "%sfn main() {\n    if flag { x: 1, y: 2 }\n}\n", decl);
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_STRUCT_LABEL) == 0, "an unknown name before a brace is not a literal.");
    }

    // A nested literal is rewritten with its parent.
    {
        const char *src = "struct point {\n    x: f32,\n    y: f32\n}\nstruct line {\n    a: point,\n    b: point\n}\n"
                          "fn main() {\n    let l = line { a: point { x: 0, y: 1 }, b: point { x: 2, y: 3 } }\n}\n";
        const char *expect = "struct point {\n    x: f32,\n    y: f32\n}\nstruct line {\n    a: point,\n    b: point\n}\n"
                             "fn main() {\n    let l = line { point { 0, 1 }, point { 2, 3 } }\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_STRUCT_LABEL) == 3, "every ordered literal in an expression reports.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, expect), "the fixer drops labels through nesting.");
    }

    // ---- nested names -----------------------------------------------------
    {
        const char *src = "fn main() {\n    let scale_factor = 2\n    let f = { input_value in\n        let doubled_value = input_value\n"
                          "        return doubled_value * scale_factor\n    }\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_NESTED_NAME) == 2, "a nested block reports its own long names only.");
        cfg.nested_name_max = 32;
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_NESTED_NAME) == 0, "nested_name_max sets the budget.");
        cfg.nested_name_max = 8;
    }

    {
        const char *src = "fn helper(first_argument: i32) i32 {\n    let running_total = first_argument\n    for element_index in 0 to 4 {\n"
                          "        let inner_value = element_index\n        running_total = running_total + inner_value\n    }\n"
                          "    return running_total\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_NESTED_NAME) == 0, "a top-level fn and its loops may spell names out.");
    }

    // ---- snake_case -------------------------------------------------------
    {
        const char *src = "fn main() {\n    let fooBar = 1\n    return fooBar\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) == 2, "each camelCase identifier reports.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, "fn main() {\n    let foo_bar = 1\n    return foo_bar\n}\n"),
                  "the fixer rewrites every occurrence of a camelCase name.");
    }

    {
        const char *src = "struct Point {\n    posX: f32,\n    posY: f32\n}\nfn makePoint(posX: f32) Point {\n    return Point { posX: posX, posY: 0 }\n}\n";
        const char *expect = "struct point {\n    pos_x: f32,\n    pos_y: f32\n}\nfn make_point(pos_x: f32) point {\n    return point { pos_x, 0 }\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) > 0, "PascalCase types and camelCase fields report.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, expect), "the fixer converts types, fields and fn names together.");
    }

    {
        ns_expect(ns_lint_test_fix_is(&cfg, "fn main() {\n    let x = XMLParser\n    let y = getHTTPResponse\n}\n",
                                     "fn main() {\n    let x = xml_parser\n    let y = get_http_response\n}\n"),
                  "acronyms split before the last capital of a run.");
    }

    {
        const char *src = "fn main() {\n    let foo_bar = 1\n    let FOO_BAR = 2\n    let x = 3\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) == 0, "snake_case and SCREAMING_SNAKE_CASE are already valid.");
    }

    {
        const char *src = "fn main() {\n    let fooBar = 1\n    let foo_bar = 2\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) == 1, "a colliding rename still reports.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, src), "a colliding rename is not rewritten.");
    }

    {
        const char *src = "fn main() {\n    let Let = 1\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) == 1, "a name that would become a keyword reports.");
        ns_expect(ns_lint_test_fix_is(&cfg, src, src), "a name that would become a keyword is not rewritten.");
    }

    {
        const char *src = "use myLib\nref fn createWindow()\nfn main() {\n    createWindow()\n    let fooBar = 1\n}\n";
        const char *expect = "use myLib\nref fn createWindow()\nfn main() {\n    createWindow()\n    let foo_bar = 1\n}\n";
        ns_expect(ns_lint_test_fix_is(&cfg, src, expect), "use and ref fn names, including their call sites, stay put.");
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) == 1, "only the local binding is reported.");
    }

    {
        const char *src = "fn main() {\n    print(\"fooBar\") // fooBar\n}\n";
        ns_expect(ns_lint_test_count(&cfg, src, NS_LINT_SNAKE_CASE) == 0, "camelCase inside strings and comments is payload.");
        ns_expect(ns_lint_test_stable(&cfg, "fn main() {\n    let fooBar = 1\n    return fooBar + XMLParser\n}\n"),
                  "a snake_case rewrite is a fixed point.");
    }

    // ---- configuration ----------------------------------------------------
    {
        ns_lint_config off;
        ns_lint_config_default(&off);
        ns_str manifest = ns_str_cstr("name = \"demo\"\nindent = 8\n\n[lint]\nindent = 2\nnested_name_max = 3\n"
                                      "binary_op_space = \"off\"\nstruct_label = \"error\"\nsnake_case = \"off\"\n\n[other]\nindent = 16\n");
        ns_lint_config_from_manifest(&off, manifest);
        ns_expect(off.indent == 2 && off.nested_name_max == 3, "[lint] keys outside the table are ignored.");
        ns_expect(off.severity[NS_LINT_BINARY_OP_SPACE] == NS_LINT_OFF, "a rule set to off is disabled.");
        ns_expect(off.severity[NS_LINT_STRUCT_LABEL] == NS_LINT_ERROR, "a rule severity can be raised.");
        ns_expect(off.severity[NS_LINT_SNAKE_CASE] == NS_LINT_OFF, "snake_case can be disabled.");
        ns_expect(ns_lint_test_count(&off, "fn main() {\n    let a = 1+2\n}\n", NS_LINT_BINARY_OP_SPACE) == 0, "a disabled rule reports nothing.");
        ns_expect(ns_lint_test_fix_is(&off, "fn main() {\n    let a = 1+2\n}\n", "fn main() {\n    let a = 1+2\n}\n"),
                  "a disabled rule rewrites nothing.");
        ns_expect(ns_lint_test_count(&off, "fn main() {\n    let fooBar = 1\n}\n", NS_LINT_SNAKE_CASE) == 0,
                  "a disabled snake_case rule reports nothing.");
        ns_expect(ns_lint_test_fix_is(&off, "fn main() {\n    let fooBar = 1\n}\n", "fn main() {\n    let fooBar = 1\n}\n"),
                  "a disabled snake_case rule rewrites nothing.");
    }

    {
        ns_lint_severity severity = NS_LINT_ERROR;
        ns_expect(ns_lint_severity_from_str(ns_str_cstr("warn"), &severity) && severity == NS_LINT_WARN, "warn parses as a severity.");
        ns_expect(!ns_lint_severity_from_str(ns_str_cstr("loud"), &severity), "an unknown severity is rejected.");
        ns_lint_rule rule = NS_LINT_RULE_COUNT;
        ns_expect(ns_lint_rule_from_str(ns_str_cstr("tab_indent"), &rule) && rule == NS_LINT_TAB_INDENT, "a rule name resolves.");
        ns_expect(ns_lint_rule_from_str(ns_str_cstr("snake_case"), &rule) && rule == NS_LINT_SNAKE_CASE, "snake_case resolves.");
        ns_expect(!ns_lint_rule_from_str(ns_str_cstr("no_such_rule"), &rule), "an unknown rule name is rejected.");
        ns_expect(ns_lint_rule_fixable(NS_LINT_BINARY_OP_SPACE) && !ns_lint_rule_fixable(NS_LINT_NESTED_NAME),
                  "nested renaming is reported, spacing is fixed.");
        ns_expect(ns_lint_rule_fixable(NS_LINT_SNAKE_CASE), "snake_case is rewritten.");
    }

    // ---- the fixer is idempotent -----------------------------------------
    {
        const char *messy = "use std\n\nstruct point {\n    x :f32,\n    y: f32\n}\n\ntype op = (i32,i32)->i32\n\n"
                            "fn main() {\n\tlet p = point { x: 1, y: 2 }   \n    let nVal = -p.x*2\n    let f: op = { a,b in return a+b }\n"
                            "    print(`{n} {f(1, 2)}\\n`)\n}";
        ns_expect(ns_lint_test_stable(&cfg, messy), "fixing a messy file twice changes nothing the second time.");
        ns_expect(ns_lint_test_fix_is(&cfg, "fn main() {\n    let a = 1\n}\n", "fn main() {\n    let a = 1\n}\n"),
                  "a clean file is not rewritten.");
    }

    // An empty or whitespace-only buffer has nothing to report.
    {
        ns_expect(ns_lint_test_total(&cfg, "") == 0, "an empty file is clean.");
        ns_expect(ns_lint_test_total(&cfg, "\n\n") == 0, "a blank file is clean.");
    }

    return 0;
}
