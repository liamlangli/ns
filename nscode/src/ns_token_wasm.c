/**
 * ns_token_wasm.c
 * Self-contained WASM build of the NS tokenizer.
 * No libc dependencies — all helpers are provided inline.
 *
 * Compile:
 *   clang --target=wasm32-unknown-unknown -O2 -nostdlib \
 *     -Wl,--no-entry -Wl,--export-dynamic \
 *     -o public/ns_token.wasm src/ns_token_wasm.c
 *
 * Memory layout (JS side):
 *   src_buf  [0 .. SRC_MAX)   — JS writes source bytes here
 *   tok_buf  [SRC_MAX .. )    — tokenize() writes [type, offset, len] i32 triples
 *
 * Exports:
 *   i32 get_src_buf()         — pointer to source buffer
 *   i32 get_tok_buf()         — pointer to token buffer
 *   i32 tokenize(src_len)     — tokenize src_len bytes, return token count
 */

/* ---------- minimal type aliases --------------------------------- */
typedef int       i32;
typedef char      i8;
typedef struct { i8 *data; i32 len; i32 owned; } ns_str;

#define ns_str_range(s, n) ((ns_str){(i8 *)(s), (n), 1})
#define ns_unused(x)       ((void)(x))

/* ---------- minimal strncmp (no libc) ---------------------------- */
static int ns_strncmp(const char *a, const char *b, i32 n) {
    while (n-- > 0) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
    return 0;
}
#define strncmp ns_strncmp

/* ---------- token type enum (mirrors ns_type.h) ------------------ */
typedef enum {
    NS_TOKEN_UNKNOWN      = -1,
    NS_TOKEN_INVALID      = 0,
    NS_TOKEN_AS           = 1,
    NS_TOKEN_ANY,
    NS_TOKEN_ASYNC,
    NS_TOKEN_ASSERT,
    NS_TOKEN_AWAIT,
    NS_TOKEN_BREAK,

    NS_TOKEN_CONST,
    NS_TOKEN_CONTINUE,
    NS_TOKEN_COMMENT,
    NS_TOKEN_DO,
    NS_TOKEN_LOOP,
    NS_TOKEN_ELSE,
    NS_TOKEN_FALSE,
    NS_TOKEN_FOR,
    NS_TOKEN_TO,
    NS_TOKEN_IF,
    NS_TOKEN_USE,
    NS_TOKEN_IN,
    NS_TOKEN_LET,
    NS_TOKEN_NIL,
    NS_TOKEN_MATCH,
    NS_TOKEN_MODULE,
    NS_TOKEN_RETURN,
    NS_TOKEN_REF,
    NS_TOKEN_STRUCT,
    NS_TOKEN_TRUE,
    NS_TOKEN_TYPE,
    NS_TOKEN_VERTEX,
    NS_TOKEN_FRAGMENT,
    NS_TOKEN_COMPUTE,
    NS_TOKEN_KERNEL,
    NS_TOKEN_OPS,

    NS_TOKEN_TYPE_I8,
    NS_TOKEN_TYPE_I16,
    NS_TOKEN_TYPE_I32,
    NS_TOKEN_TYPE_I64,
    NS_TOKEN_TYPE_U8,
    NS_TOKEN_TYPE_U16,
    NS_TOKEN_TYPE_U32,
    NS_TOKEN_TYPE_U64,
    NS_TOKEN_TYPE_F32,
    NS_TOKEN_TYPE_F64,
    NS_TOKEN_TYPE_BOOL,
    NS_TOKEN_TYPE_STR,
    NS_TOKEN_TYPE_ANY,
    NS_TOKEN_TYPE_VOID,

    NS_TOKEN_INT_LITERAL,
    NS_TOKEN_FLT_LITERAL,
    NS_TOKEN_STR_LITERAL,
    NS_TOKEN_STR_FORMAT,
    NS_TOKEN_FN,
    NS_TOKEN_SPACE,
    NS_TOKEN_IDENTIFIER,

    NS_TOKEN_COMMA,
    NS_TOKEN_DOT,
    NS_TOKEN_ASSIGN,
    NS_TOKEN_COLON,
    NS_TOKEN_QUESTION_MARK,

    NS_TOKEN_LOGIC_OP,
    NS_TOKEN_EQ_OP,
    NS_TOKEN_REL_OP,

    NS_TOKEN_SHIFT_OP,
    NS_TOKEN_ADD_OP,
    NS_TOKEN_MUL_OP,

    NS_TOKEN_CMP_OP,
    NS_TOKEN_BIT_INVERT_OP,

    NS_TOKEN_BITWISE_OP,
    NS_TOKEN_ASSIGN_OP,

    NS_TOKEN_OPEN_BRACE,
    NS_TOKEN_CLOSE_BRACE,
    NS_TOKEN_OPEN_PAREN,
    NS_TOKEN_CLOSE_PAREN,
    NS_TOKEN_OPEN_BRACKET,
    NS_TOKEN_CLOSE_BRACKET,

    NS_TOKEN_RETURN_TYPE,

    NS_TOKEN_EOL,
    NS_TOKEN_EOF,
} ns_token_type;

typedef struct {
    ns_token_type type;
    ns_str        val;
    i32           line, line_start;
} ns_token_t;

/* ---------- static memory buffers -------------------------------- */
#define SRC_MAX  65536
#define TOK_MAX  4096  /* max tokens per call */

static char src_mem[SRC_MAX];
static i32  tok_mem[TOK_MAX * 3]; /* [type, offset, length] per token */

/* ---------- ns_next_token (adapted from ns_token.c) -------------- */

static i32 ns_token_float_literal(ns_token_t *t, char *s, i32 i) {
    i32 j = i;
    while (s[j] >= '0' && s[j] <= '9') j++;
    if (s[j] == '.') {
        j++;
        while (s[j] >= '0' && s[j] <= '9') j++;
    }
    t->type = NS_TOKEN_FLT_LITERAL;
    t->val  = ns_str_range(s + i, j - i);
    return j;
}

static i32 ns_token_separator(char *s, i32 i) {
    i32 c = s[i], to = i, sep;
    do {
        sep = c == ' ' || c == '\t' || c == '\v' || c == ';';
        to++;
        c = s[to];
    } while (sep);
    return to - i - 1;
}

static i32 ns_identifier_follow(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$'   ||
           (c >= '0' && c <= '9');
}

#define ns_range_token(token, length)                                   \
    sep = ns_token_separator(s, i + (length));                          \
    if (sep == 0 && ns_identifier_follow(s[i + (length)]))             \
        goto identifier;                                                 \
    t->type = (token);                                                   \
    t->val  = ns_str_range(s + f, (length));                            \
    to = i + (length) + sep;

static i32 ns_next_token(ns_token_t *t, ns_str src, i32 f) {
    i32   i  = f, to = f + 1, l, sep;
    char *s  = src.data;
    char  lead = s[i];

    switch (lead) {

    case '0' ... '9': {
        if (s[i + 1] == 'x') {
            i += 2;
            while ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')) i++;
            t->type = NS_TOKEN_INT_LITERAL;
            t->val  = ns_str_range(s + f, i - f);
            to = i;
        } else if (s[i + 1] == 'b') {
            i += 2;
            while (s[i] == '0' || s[i] == '1') i++;
            t->type = NS_TOKEN_INT_LITERAL;
            t->val  = ns_str_range(s + f, i - f);
            to = i;
        } else if (s[i + 1] == 'o') {
            i += 2;
            while (s[i] >= '0' && s[i] <= '7') i++;
            t->type = NS_TOKEN_INT_LITERAL;
            t->val  = ns_str_range(s + f, i - f);
            to = i;
        } else if (s[i + 1] == '.') {
            i = ns_token_float_literal(t, s, i);
            to = i;
        } else {
            while (s[i] >= '0' && s[i] <= '9') i++;
            if (s[i] == '.') {
                i = ns_token_float_literal(t, s, f);
                to = i;
            } else {
                t->type = NS_TOKEN_INT_LITERAL;
                t->val  = ns_str_range(s + f, i - f);
                to = i;
            }
        }
    } break;

    case '.': {
        if (s[i + 1] >= '0' && s[i + 1] <= '9') {
            i = ns_token_float_literal(t, s, i);
            to = i;
        } else {
            t->type = NS_TOKEN_DOT;
            t->val  = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;

    case 'a': {
        if (strncmp(s + f, "assert", 6) == 0)      { ns_range_token(NS_TOKEN_ASSERT, 6) }
        else if (strncmp(s + f, "async", 5) == 0)  { ns_range_token(NS_TOKEN_ASYNC,  5) }
        else if (strncmp(s + f, "await", 5) == 0)  { ns_range_token(NS_TOKEN_AWAIT,  5) }
        else if (strncmp(s + f, "any",   3) == 0)  { ns_range_token(NS_TOKEN_TYPE_ANY, 3) }
        else if (strncmp(s + f, "as",    2) == 0)  { ns_range_token(NS_TOKEN_AS,      2) }
        else goto identifier;
    } break;

    case 'b': {
        if (strncmp(s + f, "break", 5) == 0)       { ns_range_token(NS_TOKEN_BREAK, 5) }
        else if (strncmp(s + f, "bool", 4) == 0)   { ns_range_token(NS_TOKEN_TYPE_BOOL, 4) }
        else goto identifier;
    } break;

    case 'c': {
        if (strncmp(s + f, "const",    5) == 0)    { ns_range_token(NS_TOKEN_CONST,    5) }
        else if (strncmp(s + f, "compute",  7) == 0){ ns_range_token(NS_TOKEN_COMPUTE,  7) }
        else if (strncmp(s + f, "continue", 8) == 0){ ns_range_token(NS_TOKEN_CONTINUE, 8) }
        else goto identifier;
    } break;

    case 'd': {
        if (strncmp(s + f, "do", 2) == 0)          { ns_range_token(NS_TOKEN_DO, 2) }
        else goto identifier;
    } break;

    case 'e': {
        if (strncmp(s + f, "else", 4) == 0)        { ns_range_token(NS_TOKEN_ELSE, 4) }
        else goto identifier;
    } break;

    case 'f': {
        if (strncmp(s + f, "fn", 2) == 0)          { ns_range_token(NS_TOKEN_FN,  2) }
        else if (strncmp(s + f, "for", 3) == 0)    { ns_range_token(NS_TOKEN_FOR, 3) }
        else if (strncmp(s + f, "false", 5) == 0)  { ns_range_token(NS_TOKEN_FALSE, 5) }
        else if (strncmp(s + f, "fragment", 8) == 0){ ns_range_token(NS_TOKEN_FRAGMENT, 8) }
        else if (strncmp(s + f, "f32", 3) == 0 || strncmp(s + f, "f64", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3])) goto identifier;
            t->type = (strncmp(s + f, "f32", 3) == 0) ? NS_TOKEN_TYPE_F32 : NS_TOKEN_TYPE_F64;
            t->val  = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else goto identifier;
    } break;

    case 'i': {
        if (strncmp(s + f, "if", 2) == 0)          { ns_range_token(NS_TOKEN_IF, 2) }
        else if (strncmp(s + f, "in", 2) == 0)     { ns_range_token(NS_TOKEN_IN, 2) }
        else if (strncmp(s + f, "i8", 2) == 0)     { ns_range_token(NS_TOKEN_TYPE_I8, 2) }
        else if (strncmp(s + f, "i32", 3) == 0 || strncmp(s + f, "i16", 3) == 0 || strncmp(s + f, "i64", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3])) goto identifier;
            t->type = (strncmp(s + f, "i32", 3) == 0) ? NS_TOKEN_TYPE_I32
                    : (strncmp(s + f, "i64", 3) == 0) ? NS_TOKEN_TYPE_I64
                    : NS_TOKEN_TYPE_I16;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else goto identifier;
    } break;

    case 'k': {
        if (strncmp(s + f, "kernel", 6) == 0)      { ns_range_token(NS_TOKEN_KERNEL, 6) }
        else goto identifier;
    } break;

    case 'l': {
        if (strncmp(s + f, "let",  3) == 0)        { ns_range_token(NS_TOKEN_LET,  3) }
        else if (strncmp(s + f, "loop", 4) == 0)   { ns_range_token(NS_TOKEN_LOOP, 4) }
        else goto identifier;
    } break;

    case 'm': {
        if (strncmp(s + f, "match", 5) == 0)       { ns_range_token(NS_TOKEN_MATCH,  5) }
        else if (strncmp(s + f, "mod", 3) == 0)    { ns_range_token(NS_TOKEN_MODULE, 3) }
        else goto identifier;
    } break;

    case 'n': {
        if (strncmp(s + f, "nil", 3) == 0)         { ns_range_token(NS_TOKEN_NIL, 3) }
        else goto identifier;
    } break;

    case 'o': {
        if (strncmp(s + f, "ops", 3) == 0)         { ns_range_token(NS_TOKEN_OPS, 3) }
        else goto identifier;
    } break;

    case 'r': {
        if (strncmp(s + f, "return", 6) == 0)      { ns_range_token(NS_TOKEN_RETURN, 6) }
        else if (strncmp(s + f, "ref",  3) == 0)   { ns_range_token(NS_TOKEN_REF,    3) }
        else goto identifier;
    } break;

    case 's': {
        if (strncmp(s + f, "struct", 6) == 0)      { ns_range_token(NS_TOKEN_STRUCT,   6) }
        else if (strncmp(s + f, "str", 3) == 0)    { ns_range_token(NS_TOKEN_TYPE_STR, 3) }
        else goto identifier;
    } break;

    case 't': {
        if (strncmp(s + f, "type", 4) == 0)        { ns_range_token(NS_TOKEN_TYPE, 4) }
        else if (strncmp(s + f, "true", 4) == 0)   { ns_range_token(NS_TOKEN_TRUE, 4) }
        else if (strncmp(s + f, "to",   2) == 0)   { ns_range_token(NS_TOKEN_TO,   2) }
        else goto identifier;
    } break;

    case 'u': {
        if (strncmp(s + f, "use", 3) == 0)         { ns_range_token(NS_TOKEN_USE, 3) }
        else if (strncmp(s + f, "u8", 2) == 0)     { ns_range_token(NS_TOKEN_TYPE_U8, 2) }
        else if (strncmp(s + f, "u32", 3) == 0 || strncmp(s + f, "u64", 3) == 0 || strncmp(s + f, "u16", 3) == 0) {
            sep = ns_token_separator(s, i + 3);
            if (sep == 0 && ns_identifier_follow(s[i + 3])) goto identifier;
            t->type = (strncmp(s + f, "u32", 3) == 0) ? NS_TOKEN_TYPE_U32
                    : (strncmp(s + f, "u64", 3) == 0) ? NS_TOKEN_TYPE_U64
                    : NS_TOKEN_TYPE_U16;
            t->val = ns_str_range(s + f, 3);
            to = i + 3 + sep;
        } else goto identifier;
    } break;

    case 'v': {
        if (strncmp(s + f, "void",   4) == 0)      { ns_range_token(NS_TOKEN_TYPE_VOID, 4) }
        else if (strncmp(s + f, "vertex", 6) == 0) { ns_range_token(NS_TOKEN_VERTEX,    6) }
        else goto identifier;
    } break;

    case '(': { t->type = NS_TOKEN_OPEN_PAREN;    t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case ')': { t->type = NS_TOKEN_CLOSE_PAREN;   t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case '{': { t->type = NS_TOKEN_OPEN_BRACE;    t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case '}': { t->type = NS_TOKEN_CLOSE_BRACE;   t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case '[': { t->type = NS_TOKEN_OPEN_BRACKET;  t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case ']': { t->type = NS_TOKEN_CLOSE_BRACKET; t->val = ns_str_range(s + f, 1); to = i + 1; } break;

    case 39:  /* ' */
    case 34:  /* " */
    case 96:  /* ` */ {
        char quote = lead;
        i++;
        while (s[i] != quote && s[i] != '\0' && i < src.len) i++;
        t->type = (lead == 96) ? NS_TOKEN_STR_FORMAT : NS_TOKEN_STR_LITERAL;
        /* include the quotes in the token span */
        t->val  = ns_str_range(s + f, i - f + (s[i] == quote ? 1 : 0));
        to = (s[i] == quote) ? i + 1 : i;
    } break;

    case '!': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_EQ_OP;
            t->val  = ns_str_range(s + f, (s[i + 2] == '=') ? 3 : 2);
            to = i + ((s[i + 2] == '=') ? 3 : 2);
        } else {
            t->type = NS_TOKEN_CMP_OP;
            t->val  = ns_str_range(s + f, 1);
            to = i + 1;
        }
    } break;

    case '&':
    case '|': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_BITWISE_OP; t->val = ns_str_range(s + f, 2); to = i + 2;
        } else if (s[i + 1] == lead) {
            t->type = NS_TOKEN_LOGIC_OP;   t->val = ns_str_range(s + f, 2); to = i + 2;
        } else {
            t->type = NS_TOKEN_BITWISE_OP; t->val = ns_str_range(s + f, 1); to = i + 1;
        }
    } break;

    case '^': {
        if (s[i + 1] == '=') { t->type = NS_TOKEN_ASSIGN_OP;  t->val = ns_str_range(s + f, 2); to = i + 2; }
        else                  { t->type = NS_TOKEN_BITWISE_OP; t->val = ns_str_range(s + f, 1); to = i + 1; }
    } break;

    case '=': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_EQ_OP;
            i32 len = (s[i + 2] == '=') ? 3 : 2;
            t->val  = ns_str_range(s + f, len); to = i + len;
        } else {
            t->type = NS_TOKEN_ASSIGN; t->val = ns_str_range(s + f, 1); to = i + 1;
        }
    } break;

    case '+': {
        if (s[i + 1] == '=') { t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 2); to = i + 2; }
        else                  { t->type = NS_TOKEN_ADD_OP;    t->val = ns_str_range(s + f, 1); to = i + 1; }
    } break;

    case '-': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 2); to = i + 2;
        } else if (s[i + 1] == '>') {
            t->type = NS_TOKEN_RETURN_TYPE; t->val = ns_str_range(s + f, 2); to = i + 2;
        } else if (s[i + 1] >= '0' && s[i + 1] <= '9') {
            i++;
            while (s[i] >= '0' && s[i] <= '9') i++;
            if (s[i] == '.') {
                i = ns_token_float_literal(t, s, f + 1);
                t->val.data--;
                t->val.len++;
                to = i;
            } else {
                t->type = NS_TOKEN_INT_LITERAL;
                t->val  = ns_str_range(s + f, i - f);
                to = i;
            }
        } else {
            t->type = NS_TOKEN_ADD_OP; t->val = ns_str_range(s + f, 1); to = i + 1;
        }
    } break;

    case '*': {
        if (s[i + 1] == '=') { t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 2); to = i + 2; }
        else                  { t->type = NS_TOKEN_MUL_OP;    t->val = ns_str_range(s + f, 1); to = i + 1; }
    } break;

    case '/': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 2); to = i + 2;
        } else if (s[i + 1] == '/') {
            i += 2;
            while (s[i] != '\n' && s[i] != '\r' && s[i] != '\0' && i < src.len) i++;
            t->type = NS_TOKEN_COMMENT;
            t->val  = ns_str_range(s + f, i - f);
            to = i;
        } else {
            t->type = NS_TOKEN_MUL_OP; t->val = ns_str_range(s + f, 1); to = i + 1;
        }
    } break;

    case '%': {
        if (s[i + 1] == '=') { t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 2); to = i + 2; }
        else                  { t->type = NS_TOKEN_MUL_OP;    t->val = ns_str_range(s + f, 1); to = i + 1; }
    } break;

    case '>': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_CMP_OP; t->val = ns_str_range(s + f, 2); to = i + 2;
        } else if (s[i + 1] == '>') {
            if (s[i + 2] == '=') { t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 3); to = i + 3; }
            else                  { t->type = NS_TOKEN_SHIFT_OP;  t->val = ns_str_range(s + f, 2); to = i + 2; }
        } else {
            t->type = NS_TOKEN_CMP_OP; t->val = ns_str_range(s + f, 1); to = i + 1;
        }
    } break;

    case '<': {
        if (s[i + 1] == '=') {
            t->type = NS_TOKEN_CMP_OP; t->val = ns_str_range(s + f, 2); to = i + 2;
        } else if (s[i + 1] == '<') {
            if (s[i + 2] == '=') { t->type = NS_TOKEN_ASSIGN_OP; t->val = ns_str_range(s + f, 3); to = i + 3; }
            else                  { t->type = NS_TOKEN_SHIFT_OP;  t->val = ns_str_range(s + f, 2); to = i + 2; }
        } else {
            t->type = NS_TOKEN_CMP_OP; t->val = ns_str_range(s + f, 1); to = i + 1;
        }
    } break;

    case '?': { t->type = NS_TOKEN_QUESTION_MARK; t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case ':': { t->type = NS_TOKEN_COLON;         t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case ',': { t->type = NS_TOKEN_COMMA;         t->val = ns_str_range(s + f, 1); to = i + 1; } break;
    case '~': { t->type = NS_TOKEN_BIT_INVERT_OP; t->val = ns_str_range(s + f, 1); to = i + 1; } break;

    case ' ':
    case '\t':
    case '\v': {
        l = i;
        sep = ns_token_separator(s, i);
        if (sep == 0) goto identifier;
        t->type = NS_TOKEN_SPACE;
        t->val  = ns_str_range(s + l, sep);
        to = i + sep;
    } break;

    case '\r':
    case '\n':
    case ';': {
        if (s[i] == '\r' && s[i + 1] == '\n') i++;
        t->type = NS_TOKEN_EOL;
        t->val  = ns_str_range(s + f, 1);
        to = i + 1;
        t->line++;
        t->line_start = i;
    } break;

    case '\0': {
        t->type = NS_TOKEN_EOF;
        to = f;
    } break;

    identifier:
    default: {
        char lc = s[i];
        if (!((lc >= 'a' && lc <= 'z') || (lc >= 'A' && lc <= 'Z') || lc == '_')) {
            t->type = NS_TOKEN_INVALID;
            t->val  = ns_str_range(s + i, 1);
            to = i + 1;
            break;
        }
        i++;
        while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
               (s[i] >= '0' && s[i] <= '9') || s[i] == '_') i++;
        t->type = NS_TOKEN_IDENTIFIER;
        t->val  = ns_str_range(s + f, i - f);
        to = i;
    } break;
    }
    return to;
}

/* ---------- exported WASM API ------------------------------------ */

__attribute__((export_name("get_src_buf")))
i32 get_src_buf(void) { return (i32)(i8 *)src_mem; }

__attribute__((export_name("get_tok_buf")))
i32 get_tok_buf(void) { return (i32)(i8 *)tok_mem; }

/**
 * Tokenize src_len bytes from src_mem.
 * Writes token triples [type, byte_offset, byte_length] into tok_mem.
 * Returns token count. Skips EOL tokens.
 */
__attribute__((export_name("tokenize")))
i32 tokenize(i32 src_len) {
    if (src_len <= 0 || src_len >= SRC_MAX) return 0;
    src_mem[src_len] = '\0'; /* null-terminate for safety */

    ns_str  src    = ns_str_range(src_mem, src_len);
    ns_token_t tok = {0};
    tok.line = 1;

    i32 i = 0, count = 0;
    while (i < src_len && count < TOK_MAX) {
        i32 next = ns_next_token(&tok, src, i);
        if (tok.type == NS_TOKEN_EOF) break;
        if (tok.type != NS_TOKEN_EOL) {
            i32 offset = (i32)(tok.val.data - src_mem);
            i32 len    = tok.val.len;
            /* clamp to avoid out-of-range data */
            if (offset >= 0 && offset + len <= src_len && len > 0) {
                tok_mem[count * 3 + 0] = (i32)tok.type;
                tok_mem[count * 3 + 1] = offset;
                tok_mem[count * 3 + 2] = len;
                count++;
            }
        }
        if (next <= i) break; /* guard against infinite loop */
        i = next;
    }
    return count;
}
