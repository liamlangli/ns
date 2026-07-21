#include "ns_test.h"
#include "ns_token.h"

// Lex a whole source string, dropping NS_TOKEN_SPACE, and return how many
// tokens were produced. Source text is utf8 by default, so these tests feed
// multibyte identifiers and literals straight through the byte-level API.
static i32 ns_token_lex(const char *src, ns_token_t *out, i32 max) {
    ns_str s = ns_str_cstr((i8 *)src);
    ns_token_t t = {0};
    t.line = 1;
    i32 i = 0, n = 0;
    do {
        i = ns_next_token(&t, s, ns_str_cstr("<ns_token_test>"), i);
        if (t.type == NS_TOKEN_EOF) break;
        if (t.type != NS_TOKEN_SPACE && n < max) out[n++] = t;
        t.type = NS_TOKEN_UNKNOWN;
    } while (i < s.len);
    return n;
}

static ns_bool ns_token_is(ns_token_t t, ns_token_type type, const char *val) {
    return t.type == type && t.val.len == (i32)strlen(val) && strncmp((char *)t.val.data, val, t.val.len) == 0;
}

static ns_bool ns_token_num_is(ns_token_t t, ns_token_type type, const char *val, ns_num_suffix suffix) {
    return ns_token_is(t, type, val) && t.suffix == suffix;
}

int main() {
    ns_token_t ts[16];

    {
        i32 n = ns_token_lex("lit answer = 42 literal", ts, 16);
        ns_expect(n == 5 && ns_token_is(ts[0], NS_TOKEN_LIT, "lit") &&
                      ns_token_is(ts[1], NS_TOKEN_IDENTIFIER, "answer") &&
                      ns_token_is(ts[4], NS_TOKEN_IDENTIFIER, "literal"),
                  "lit is a keyword only at an identifier boundary.");
    }

    {
        i32 n = ns_token_lex("enum enum_value enumerable", ts, 16);
        ns_expect(n == 3 && ns_token_is(ts[0], NS_TOKEN_ENUM, "enum") &&
                      ns_token_is(ts[1], NS_TOKEN_IDENTIFIER, "enum_value") &&
                      ns_token_is(ts[2], NS_TOKEN_IDENTIFIER, "enumerable"),
                  "enum is a keyword only at an identifier boundary.");
    }

    // utf8 identifiers tokenize as single NS_TOKEN_IDENTIFIER tokens
    {
        i32 n = ns_token_lex("let 名字 = 1", ts, 16);
        ns_expect(n == 4 && ns_token_is(ts[0], NS_TOKEN_LET, "let") && ns_token_is(ts[1], NS_TOKEN_IDENTIFIER, "名字") &&
                      ns_token_is(ts[2], NS_TOKEN_ASSIGN, "=") && ns_token_is(ts[3], NS_TOKEN_INT_LITERAL, "1"),
                  "utf8 identifier lexes as one identifier token.");
    }

    // ascii and utf8 codepoints mix freely inside one identifier
    {
        i32 n = ns_token_lex("héllo_1", ts, 16);
        ns_expect(n == 1 && ns_token_is(ts[0], NS_TOKEN_IDENTIFIER, "héllo_1"), "mixed ascii/utf8 identifier stays one token.");
    }

    // a utf8 char glued to a keyword extends it into an identifier,
    // matching the ascii rule that `letx` is not `let` + `x`
    {
        i32 n = ns_token_lex("let名", ts, 16);
        ns_expect(n == 1 && ns_token_is(ts[0], NS_TOKEN_IDENTIFIER, "let名"), "keyword followed by utf8 char is an identifier.");
    }

    // utf8 payload passes through string literals byte-for-byte
    {
        i32 n = ns_token_lex("\"你好, 世界\"", ts, 16);
        ns_expect(n == 1 && ns_token_is(ts[0], NS_TOKEN_STR_LITERAL, "你好, 世界"), "utf8 string literal keeps its bytes.");
    }

    // an escaped quote no longer terminates the literal
    {
        i32 n = ns_token_lex("\"a\\\"b\" 1", ts, 16);
        ns_expect(n == 2 && ns_token_is(ts[0], NS_TOKEN_STR_LITERAL, "a\\\"b") && ns_token_is(ts[1], NS_TOKEN_INT_LITERAL, "1"),
                  "escaped quote stays inside the string literal.");
    }

    // format strings get the same escape and utf8 handling
    {
        i32 n = ns_token_lex("`名 = {x}`", ts, 16);
        ns_expect(n == 1 && ns_token_is(ts[0], NS_TOKEN_STR_FORMAT, "名 = {x}"), "utf8 format string literal.");
    }

    // a leading utf8 BOM is treated as whitespace, not an identifier
    {
        i32 n = ns_token_lex("\xEF\xBB\xBFlet", ts, 16);
        ns_expect(n == 1 && ns_token_is(ts[0], NS_TOKEN_LET, "let"), "utf8 BOM is skipped as whitespace.");
    }

    // malformed utf8 surfaces as NS_TOKEN_INVALID instead of being absorbed
    {
        i32 n = ns_token_lex("\xFF", ts, 16);
        ns_expect(n == 1 && ts[0].type == NS_TOKEN_INVALID, "invalid utf8 lead byte is an invalid token.");
        n = ns_token_lex("\xE4\xBD", ts, 16); // first two bytes of a 3-byte sequence
        ns_expect(n == 2 && ts[0].type == NS_TOKEN_INVALID && ts[1].type == NS_TOKEN_INVALID, "truncated utf8 sequence is invalid.");
    }

    // encode/decode roundtrip across all sequence lengths
    {
        u32 cps[4] = {0x24, 0xE9, 0x4F60, 0x1F600};
        ns_bool ok = true;
        for (i32 k = 0; k < 4; k++) {
            i8 buf[4];
            i32 n = ns_utf8_encode(cps[k], buf);
            u32 cp = 0;
            ok = ok && n == k + 1 && ns_utf8_decode(ns_str_range(buf, n), 0, &cp) == n && cp == cps[k];
        }
        ns_expect(ok, "utf8 encode/decode roundtrip for 1..4 byte sequences.");
    }

    // codepoint length vs byte length, and validation
    {
        ns_str s = ns_str_cstr("héllo");
        ns_expect(ns_str_utf8_len(s) == 5 && s.len == 6 && ns_str_utf8_valid(s), "utf8_len counts codepoints, len counts bytes.");
        ns_expect(ns_str_utf8_len(ns_str_cstr("你好")) == 2, "utf8_len of two cjk chars is 2.");
        ns_expect(!ns_str_utf8_valid(ns_str_cstr("\xC0\x80")), "overlong utf8 encoding is rejected.");
        ns_expect(!ns_str_utf8_valid(ns_str_cstr("\xED\xA0\x80")), "utf8-encoded surrogate is rejected.");
    }

    // \u{...} escapes unescape into utf8 bytes; malformed ones stay verbatim
    {
        ns_str u = ns_str_unescape(ns_str_cstr("\\u{4F60}\\u{597D}!"));
        ns_expect(ns_str_equals(u, ns_str_cstr("你好!")), "\\u{...} escapes decode to utf8 bytes.");
        ns_str_free(u);
        ns_str m = ns_str_unescape(ns_str_cstr("\\u{}\\uZZ"));
        ns_expect(ns_str_equals(m, ns_str_cstr("u{}uZZ")), "malformed \\u escapes keep their spelling.");
        ns_str_free(m);
    }

    // numeric literal suffixes select explicit widths; old i/f suffixes are
    // no longer part of the language.
    {
        i32 n = ns_token_lex("1s 1us 1b 1ub 1l 1ul 1u 1d 1.0d 1h 1.0h 1hb 1.0hb", ts, 16);
        ns_expect(n == 13 &&
                      ns_token_num_is(ts[0], NS_TOKEN_INT_LITERAL, "1s", NS_NUM_SUFFIX_I16) &&
                      ns_token_num_is(ts[1], NS_TOKEN_INT_LITERAL, "1us", NS_NUM_SUFFIX_U16) &&
                      ns_token_num_is(ts[2], NS_TOKEN_INT_LITERAL, "1b", NS_NUM_SUFFIX_I8) &&
                      ns_token_num_is(ts[3], NS_TOKEN_INT_LITERAL, "1ub", NS_NUM_SUFFIX_U8) &&
                      ns_token_num_is(ts[4], NS_TOKEN_INT_LITERAL, "1l", NS_NUM_SUFFIX_I64) &&
                      ns_token_num_is(ts[5], NS_TOKEN_INT_LITERAL, "1ul", NS_NUM_SUFFIX_U64) &&
                      ns_token_num_is(ts[6], NS_TOKEN_INT_LITERAL, "1u", NS_NUM_SUFFIX_U32) &&
                      ns_token_num_is(ts[7], NS_TOKEN_FLT_LITERAL, "1d", NS_NUM_SUFFIX_F64) &&
                      ns_token_num_is(ts[8], NS_TOKEN_FLT_LITERAL, "1.0d", NS_NUM_SUFFIX_F64) &&
                      ns_token_num_is(ts[9], NS_TOKEN_FLT_LITERAL, "1h", NS_NUM_SUFFIX_F16) &&
                      ns_token_num_is(ts[10], NS_TOKEN_FLT_LITERAL, "1.0h", NS_NUM_SUFFIX_F16) &&
                      ns_token_num_is(ts[11], NS_TOKEN_FLT_LITERAL, "1hb", NS_NUM_SUFFIX_BF16) &&
                      ns_token_num_is(ts[12], NS_TOKEN_FLT_LITERAL, "1.0hb", NS_NUM_SUFFIX_BF16),
                  "numeric literal suffix grammar.");
    }
    {
        i32 n = ns_token_lex("32bit 1i 1f 1bh", ts, 16);
        ns_expect(n == 8 &&
                      ns_token_num_is(ts[0], NS_TOKEN_INT_LITERAL, "32", NS_NUM_SUFFIX_NONE) &&
                      ns_token_is(ts[1], NS_TOKEN_IDENTIFIER, "bit") &&
                      ns_token_num_is(ts[2], NS_TOKEN_INT_LITERAL, "1", NS_NUM_SUFFIX_NONE) &&
                      ns_token_is(ts[3], NS_TOKEN_IDENTIFIER, "i") &&
                      ns_token_num_is(ts[4], NS_TOKEN_INT_LITERAL, "1", NS_NUM_SUFFIX_NONE) &&
                      ns_token_is(ts[5], NS_TOKEN_IDENTIFIER, "f") &&
                      ns_token_num_is(ts[6], NS_TOKEN_INT_LITERAL, "1", NS_NUM_SUFFIX_NONE) &&
                      ns_token_is(ts[7], NS_TOKEN_IDENTIFIER, "bh"),
                  "old numeric suffixes are identifiers and suffixes do not split identifiers.");
    }

    // A minus immediately followed by a number remains an operator. Negative
    // values are represented by the parser as unary expressions, which keeps
    // subtraction unambiguous without requiring whitespace (`width-54.0`).
    {
        i32 n = ns_token_lex("width-54.0 -2 5--3", ts, 16);
        ns_expect(n == 9 &&
                      ns_token_is(ts[0], NS_TOKEN_IDENTIFIER, "width") &&
                      ns_token_is(ts[1], NS_TOKEN_ADD_OP, "-") &&
                      ns_token_is(ts[2], NS_TOKEN_FLT_LITERAL, "54.0") &&
                      ns_token_is(ts[3], NS_TOKEN_ADD_OP, "-") &&
                      ns_token_is(ts[4], NS_TOKEN_INT_LITERAL, "2") &&
                      ns_token_is(ts[5], NS_TOKEN_INT_LITERAL, "5") &&
                      ns_token_is(ts[6], NS_TOKEN_ADD_OP, "-") &&
                      ns_token_is(ts[7], NS_TOKEN_ADD_OP, "-") &&
                      ns_token_is(ts[8], NS_TOKEN_INT_LITERAL, "3"),
                  "minus before numeric literals always lexes as an operator.");
    }

    return 0;
}
