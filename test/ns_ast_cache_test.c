#include "ns_ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define setenv(key, value, overwrite) _putenv_s(key, value)
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "ns_ast_cache_test:%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void release(ns_ast_ctx *ctx) {
    ns_array_free(ctx->nodes); ns_array_free(ctx->sections); ns_array_free(ctx->stack);
    ns_array_free(ctx->op_stack); ns_array_free(ctx->expr_stack); ns_array_free(ctx->scopes);
}

static void compare(const char *source) {
    ns_ast_ctx a = {0}, b = {0};
    ns_str text = ns_str_cstr((char *)source), file = ns_str_cstr("<token cache>");
    setenv("NS_PARSE_TOKEN_CACHE", "0", 1);
    ns_return_bool x = ns_ast_parse(&a, text, file);
    setenv("NS_PARSE_TOKEN_CACHE", "1", 1);
    ns_return_bool y = ns_ast_parse(&b, text, file);
    CHECK(ns_return_is_error(x) == ns_return_is_error(y));
    CHECK(a.token_cache == NULL && b.token_cache == NULL);
    CHECK(a.f == b.f);
    CHECK(a.token.line == b.token.line && a.token.line_start == b.token.line_start);
    CHECK(ns_array_length(a.nodes) == ns_array_length(b.nodes));
    CHECK(ns_array_length(a.sections) == ns_array_length(b.sections));
    // Sources here contain no synthesized interpolation strings: every token
    // slice points at the same input, allowing exact AST and location parity.
    CHECK(memcmp(a.nodes, b.nodes, ns_array_length(a.nodes) * sizeof(ns_ast_t)) == 0);
    if (ns_array_length(a.sections))
        CHECK(memcmp(a.sections, b.sections, ns_array_length(a.sections) * sizeof(i32)) == 0);
    release(&a); release(&b);
}

int main(void) {
    compare("lit later = base + 2\n lit base = 40\n fn main() { assert later == 42 }\n");
    compare("// UTF-8\r\nstruct point { x: f32, y: f32 }\r\n"
            "fn main() { let 点 = point { x: 1.25, y: -2.0 }\r\n"
            "let a = [i32](4)\r\nfor i in 0 to 4 { a[i] = (i + 3) * 7 }\r\n}\r\n");
    compare("type op = (i32, i32) -> i32\n"
            "fn main() { let f: op = { a, b in return a + b }\n"
            "let text = \"fn { } // \\\" ;\"\n let n = 0x1f + 0b101 + 0o12\n"
            "if true { let x = f(n, 2) } else { let y = 0 } }\n");
    // Reusing one context after success/error must never retain stack cache.
    ns_ast_ctx ctx = {0};
    CHECK(!ns_return_is_error(ns_ast_parse(&ctx, ns_str_cstr("fn a() {}"), ns_str_cstr("a.ns"))));
    CHECK(ctx.token_cache == NULL);
    CHECK(!ns_return_is_error(ns_ast_parse(&ctx, ns_str_cstr("fn b() {}"), ns_str_cstr("b.ns"))));
    CHECK(ctx.token_cache == NULL && ctx.section_end == 2);
    release(&ctx);
    puts("ns_ast_cache_test: passed (AST/location parity and context reuse)");
    return 0;
}
