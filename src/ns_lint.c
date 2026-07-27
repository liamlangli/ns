#include "ns_lint.h"
#include "ns_token.h"

#include <stdarg.h>

#define NS_LINT_INDENT_DEFAULT 4
#define NS_LINT_NESTED_NAME_DEFAULT 8
#define NS_LINT_INDENT_MAX 64

static const char *ns_lint_rule_names[NS_LINT_RULE_COUNT] = {
    "binary_op_space", "comma_space", "colon_space", "trailing_space", "tab_indent", "struct_label", "nested_name",
};

// Findings the fixer can rewrite. `nested_name` asks for a rename, which is a
// semantic edit, so it stays a report.
static const ns_bool ns_lint_rule_fixables[NS_LINT_RULE_COUNT] = {true, true, true, true, true, true, false};

static const ns_lint_severity ns_lint_rule_defaults[NS_LINT_RULE_COUNT] = {
    NS_LINT_ERROR, NS_LINT_ERROR, NS_LINT_ERROR, NS_LINT_ERROR, NS_LINT_ERROR, NS_LINT_WARN, NS_LINT_WARN,
};

// Indentation is rewritten from this buffer, so it caps how wide one fixed
// indent run can get (128 columns).
static const char ns_lint_spaces[] = "                                                                "
                                     "                                                                ";
#define NS_LINT_SPACES_MAX ((i32)sizeof(ns_lint_spaces) - 1)

ns_str ns_lint_rule_name(ns_lint_rule rule) {
    if (rule < 0 || rule >= NS_LINT_RULE_COUNT) return ns_str_cstr("unknown");
    return ns_str_cstr(ns_lint_rule_names[rule]);
}

ns_str ns_lint_severity_name(ns_lint_severity severity) {
    switch (severity) {
    case NS_LINT_ERROR: return ns_str_cstr("error");
    case NS_LINT_WARN: return ns_str_cstr("warning");
    default: return ns_str_cstr("off");
    }
}

ns_bool ns_lint_rule_fixable(ns_lint_rule rule) {
    if (rule < 0 || rule >= NS_LINT_RULE_COUNT) return false;
    return ns_lint_rule_fixables[rule];
}

ns_bool ns_lint_rule_from_str(ns_str name, ns_lint_rule *out) {
    for (i32 i = 0; i < NS_LINT_RULE_COUNT; i++) {
        if (ns_str_equals(name, ns_str_cstr(ns_lint_rule_names[i]))) {
            if (out) *out = (ns_lint_rule)i;
            return true;
        }
    }
    return false;
}

ns_bool ns_lint_severity_from_str(ns_str value, ns_lint_severity *out) {
    ns_lint_severity severity;
    if (ns_str_equals(value, ns_str_cstr("error")) || ns_str_equals(value, ns_str_cstr("deny"))) {
        severity = NS_LINT_ERROR;
    } else if (ns_str_equals(value, ns_str_cstr("warn")) || ns_str_equals(value, ns_str_cstr("warning"))) {
        severity = NS_LINT_WARN;
    } else if (ns_str_equals(value, ns_str_cstr("off")) || ns_str_equals(value, ns_str_cstr("none")) ||
               ns_str_equals(value, ns_str_cstr("allow")) || ns_str_equals(value, ns_str_false)) {
        severity = NS_LINT_OFF;
    } else if (ns_str_equals(value, ns_str_true)) {
        severity = NS_LINT_ERROR;
    } else {
        return false;
    }
    if (out) *out = severity;
    return true;
}

void ns_lint_config_default(ns_lint_config *cfg) {
    for (i32 i = 0; i < NS_LINT_RULE_COUNT; i++) cfg->severity[i] = ns_lint_rule_defaults[i];
    cfg->indent = NS_LINT_INDENT_DEFAULT;
    cfg->nested_name_max = NS_LINT_NESTED_NAME_DEFAULT;
}

// ---------------------------------------------------------------------------
// `[lint]` manifest table
// ---------------------------------------------------------------------------

static ns_str ns_lint_trim(ns_str s) {
    i32 b = 0, e = s.len;
    while (b < e && (s.data[b] == ' ' || s.data[b] == '\t' || s.data[b] == '\r')) b++;
    while (e > b && (s.data[e - 1] == ' ' || s.data[e - 1] == '\t' || s.data[e - 1] == '\r')) e--;
    return (ns_str){.data = s.data + b, .len = e - b};
}

// Strip a TOML string's quotes and any trailing comment. Values in the `[lint]`
// table are short scalars, so this stays deliberately literal.
static ns_str ns_lint_scalar(ns_str value) {
    value = ns_lint_trim(value);
    if (value.len >= 2 && (value.data[0] == '"' || value.data[0] == '\'')) {
        i8 quote = value.data[0];
        for (i32 i = 1; i < value.len; i++) {
            if (value.data[i] == quote) return (ns_str){.data = value.data + 1, .len = i - 1};
        }
    }
    for (i32 i = 0; i < value.len; i++) {
        if (value.data[i] == '#') return ns_lint_trim((ns_str){.data = value.data, .len = i});
    }
    return value;
}

static ns_bool ns_lint_positive_int(ns_str value, i32 *out) {
    if (value.len == 0) return false;
    i32 n = 0;
    for (i32 i = 0; i < value.len; i++) {
        if (value.data[i] < '0' || value.data[i] > '9') return false;
        n = n * 10 + (value.data[i] - '0');
        if (n > 1024) return false;
    }
    *out = n;
    return true;
}

void ns_lint_config_from_manifest(ns_lint_config *cfg, ns_str manifest) {
    if (manifest.data == ns_null || manifest.len == 0) return;

    ns_bool in_table = false;
    for (i32 i = 0; i < manifest.len;) {
        i32 begin = i;
        while (i < manifest.len && manifest.data[i] != '\n') i++;
        ns_str line = ns_lint_trim((ns_str){.data = manifest.data + begin, .len = i - begin});
        if (i < manifest.len) i++;
        if (line.len == 0 || line.data[0] == '#') continue;

        if (line.data[0] == '[') {
            in_table = ns_str_equals(line, ns_str_cstr("[lint]"));
            continue;
        }
        if (!in_table) continue;

        i32 eq = -1;
        for (i32 j = 0; j < line.len; j++) {
            if (line.data[j] == '=') { eq = j; break; }
        }
        if (eq <= 0) continue;
        ns_str key = ns_lint_trim((ns_str){.data = line.data, .len = eq});
        ns_str value = ns_lint_scalar((ns_str){.data = line.data + eq + 1, .len = line.len - eq - 1});

        i32 number = 0;
        ns_lint_rule rule;
        if (ns_str_equals(key, ns_str_cstr("indent"))) {
            if (ns_lint_positive_int(value, &number) && number > 0 && number <= NS_LINT_INDENT_MAX) cfg->indent = number;
            else ns_warn("lint", "[lint] indent must be 1..%d, kept %d.\n", NS_LINT_INDENT_MAX, cfg->indent);
        } else if (ns_str_equals(key, ns_str_cstr("nested_name_max"))) {
            if (ns_lint_positive_int(value, &number) && number > 0) cfg->nested_name_max = number;
            else ns_warn("lint", "[lint] nested_name_max must be a positive integer, kept %d.\n", cfg->nested_name_max);
        } else if (ns_lint_rule_from_str(key, &rule)) {
            ns_lint_severity severity;
            if (ns_lint_severity_from_str(value, &severity)) cfg->severity[rule] = severity;
            else ns_warn("lint", "[lint] %.*s = \"%.*s\" is not error | warn | off.\n", key.len, key.data, value.len, value.data);
        } else {
            ns_warn("lint", "[lint] unknown key %.*s.\n", key.len, key.data);
        }
    }
}

// ---------------------------------------------------------------------------
// Token stream
// ---------------------------------------------------------------------------

typedef struct ns_lint_token {
    ns_token_type type;
    ns_str val;
    i32 start, end; // byte span of the token text in the source
    i32 line;       // 1-based line of `start`
} ns_lint_token;

// One source rewrite: replace [start, end) with `text`. `text` always points at
// static storage, so edits never own memory.
typedef struct ns_lint_edit {
    i32 start, end;
    ns_str text;
} ns_lint_edit;

typedef struct ns_lint_struct {
    ns_str name;
    ns_str *fields; // declaration order
} ns_lint_struct;

typedef struct ns_lint_ctx {
    const ns_lint_config *cfg;
    ns_str src;
    ns_str file;
    ns_lint_token *tokens;
    i32 count;
    i32 *line_start; // byte offset of line n at index n - 1
    i32 line_count;
    ns_lint_struct *structs;
    ns_lint_edit *edits;
    ns_lint_result *out;
    i32 tail_start; // start of the trailing whitespace run at end of file
    ns_bool fix;
} ns_lint_ctx;

// The tokenizer reports keyword spans through `val` but returns an index that
// has already consumed the following separators, and it reports string literals
// without their quotes. Recover the exact token span from both.
static i32 ns_lint_token_end(ns_token_t *t, i32 from, i32 next, i32 len) {
    i32 end = from + (t->val.len > 0 ? t->val.len : 0);
    switch (t->type) {
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT:
    case NS_TOKEN_EOL:
        end = next;
        break;
    default:
        break;
    }
    if (end <= from) end = from + 1;
    return end > len ? len : end;
}

static void ns_lint_scan(ns_lint_ctx *c) {
    ns_str src = c->src;
    ns_array_push(c->line_start, 0);
    for (i32 i = 0; i < src.len; i++) {
        if (src.data[i] == '\n') ns_array_push(c->line_start, i + 1);
    }
    c->line_count = (i32)ns_array_length(c->line_start);

    ns_token_t t = {0};
    t.line = 1;
    i32 line = 1;
    i32 next_line_at = c->line_count > 1 ? c->line_start[1] : src.len + 1;
    i32 i = 0;
    while (i < src.len) {
        i32 from = i;
        t.type = NS_TOKEN_UNKNOWN;
        i = ns_next_token(&t, src, c->file, from);
        if (i <= from) i = from + 1; // a rejected byte must still advance
        if (t.type == NS_TOKEN_EOF) break;
        if (t.type == NS_TOKEN_SPACE) continue;
        while (from >= next_line_at && line < c->line_count) {
            line++;
            next_line_at = line < c->line_count ? c->line_start[line] : src.len + 1;
        }
        ns_lint_token token = {
            .type = t.type,
            .val = t.val,
            .start = from,
            .end = ns_lint_token_end(&t, from, i, src.len),
            .line = line,
        };
        ns_array_push(c->tokens, token);
    }
    c->count = (i32)ns_array_length(c->tokens);
}

static i32 ns_lint_line_of(ns_lint_ctx *c, i32 offset) {
    i32 lo = 0, hi = c->line_count - 1;
    while (lo < hi) {
        i32 mid = (lo + hi + 1) / 2;
        if (c->line_start[mid] <= offset) lo = mid;
        else hi = mid - 1;
    }
    return lo + 1;
}

static i32 ns_lint_column_of(ns_lint_ctx *c, i32 offset) {
    i32 line = ns_lint_line_of(c, offset);
    return offset - c->line_start[line - 1] + 1;
}

// Next token index, skipping comments and (when `same_line` is false) newlines.
static i32 ns_lint_next(ns_lint_ctx *c, i32 index, ns_bool same_line) {
    for (i32 i = index + 1; i < c->count; i++) {
        if (c->tokens[i].type == NS_TOKEN_COMMENT) continue;
        if (c->tokens[i].type == NS_TOKEN_EOL) {
            if (same_line) return -1;
            continue;
        }
        return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Diagnostics and edits
// ---------------------------------------------------------------------------

static ns_str ns_lint_message(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    i32 len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) len = 0;
    if (len > (i32)sizeof(buf) - 1) len = (i32)sizeof(buf) - 1;
    ns_str message = ns_str_null;
    ns_str_append_len(&message, (const i8 *)buf, len);
    return message;
}

// Record an edit unless it overlaps one an earlier rule already claimed.
static ns_bool ns_lint_push_edit(ns_lint_ctx *c, i32 start, i32 end, ns_str text) {
    for (i32 i = 0, l = (i32)ns_array_length(c->edits); i < l; i++) {
        ns_lint_edit *e = &c->edits[i];
        if (start < e->end && e->start < end) return false;
        if (start == end && e->start <= start && start <= e->end) return false;
    }
    ns_array_push(c->edits, ((ns_lint_edit){.start = start, .end = end, .text = text}));
    return true;
}

static void ns_lint_report(ns_lint_ctx *c, ns_lint_rule rule, i32 offset, ns_bool fixable, ns_str message) {
    ns_lint_severity severity = c->cfg->severity[rule];
    if (severity == NS_LINT_OFF) {
        ns_array_free(message.data);
        return;
    }
    ns_lint_diagnostic diagnostic = {
        .rule = rule,
        .severity = severity,
        .file = c->file,
        .message = message,
        .line = ns_lint_line_of(c, offset),
        .column = ns_lint_column_of(c, offset),
        .fixable = fixable,
        .fixed = fixable && c->fix,
    };
    ns_array_push(c->out->diagnostics, diagnostic);
    if (severity == NS_LINT_ERROR) c->out->errors++;
    else c->out->warnings++;
    if (fixable) {
        c->out->fixable++;
        if (c->fix) c->out->fixed++;
    }
}

// Report a rule that also knows how to rewrite the source. In fix mode a
// dropped edit means an earlier rule already rewrote the same bytes, so the
// finding disappears with it and is not reported twice.
static void ns_lint_report_fix(ns_lint_ctx *c, ns_lint_rule rule, i32 offset, i32 start, i32 end, ns_str text, ns_str message) {
    if (c->cfg->severity[rule] == NS_LINT_OFF) { // a disabled rule never rewrites
        ns_array_free(message.data);
        return;
    }
    ns_bool applied = ns_lint_push_edit(c, start, end, text);
    if (c->fix && !applied) {
        ns_array_free(message.data);
        return;
    }
    ns_lint_report(c, rule, offset, applied, message);
}

static int ns_lint_edit_cmp(const void *a, const void *b) {
    const ns_lint_edit *x = a;
    const ns_lint_edit *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end < y->end ? -1 : 1;
    return 0;
}

static int ns_lint_diagnostic_cmp(const void *a, const void *b) {
    const ns_lint_diagnostic *x = a;
    const ns_lint_diagnostic *y = b;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    if (x->column != y->column) return x->column < y->column ? -1 : 1;
    return (i32)x->rule - (i32)y->rule;
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

static ns_bool ns_lint_enabled(ns_lint_ctx *c, ns_lint_rule rule) { return c->cfg->severity[rule] != NS_LINT_OFF; }

// True when the bytes in [start, end) are only spaces and tabs. A gap holding
// anything else (a `;` separator the tokenizer swallowed, for instance) is left
// alone by every spacing rule.
static ns_bool ns_lint_blank(ns_lint_ctx *c, i32 start, i32 end) {
    for (i32 i = start; i < end; i++) {
        if (c->src.data[i] != ' ' && c->src.data[i] != '\t') return false;
    }
    return true;
}

static ns_bool ns_lint_single_space(ns_lint_ctx *c, i32 start, i32 end) {
    return end - start == 1 && c->src.data[start] == ' ';
}

// Tokens that can end an operand. A `+`/`-` after anything else is unary.
static ns_bool ns_lint_ends_value(ns_token_type type) {
    switch (type) {
    case NS_TOKEN_IDENTIFIER:
    case NS_TOKEN_INT_LITERAL:
    case NS_TOKEN_FLT_LITERAL:
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT:
    case NS_TOKEN_CLOSE_PAREN:
    case NS_TOKEN_CLOSE_BRACKET:
    case NS_TOKEN_CLOSE_BRACE:
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
    case NS_TOKEN_NIL:
    case NS_TOKEN_ANY:
    case NS_TOKEN_TYPE_I8:
    case NS_TOKEN_TYPE_I16:
    case NS_TOKEN_TYPE_I32:
    case NS_TOKEN_TYPE_I64:
    case NS_TOKEN_TYPE_U8:
    case NS_TOKEN_TYPE_U16:
    case NS_TOKEN_TYPE_U32:
    case NS_TOKEN_TYPE_U64:
    case NS_TOKEN_TYPE_F32:
    case NS_TOKEN_TYPE_F64:
    case NS_TOKEN_TYPE_BOOL:
    case NS_TOKEN_TYPE_STR:
    case NS_TOKEN_TYPE_ANY:
    case NS_TOKEN_TYPE_VOID:
        return true;
    default:
        return false;
    }
}

static ns_bool ns_lint_is_binary_op(ns_token_type type) {
    switch (type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_REL_OP:
    case NS_TOKEN_EQ_OP:
    case NS_TOKEN_LOGIC_OP:
    case NS_TOKEN_SHIFT_OP:
    case NS_TOKEN_BITWISE_OP:
    case NS_TOKEN_ASSIGN:
    case NS_TOKEN_ASSIGN_OP:
    case NS_TOKEN_RETURN_TYPE:
        return true;
    default:
        return false;
    }
}

static ns_bool ns_lint_opens(ns_token_type type) {
    return type == NS_TOKEN_OPEN_BRACE || type == NS_TOKEN_OPEN_PAREN || type == NS_TOKEN_OPEN_BRACKET;
}

static ns_bool ns_lint_closes(ns_token_type type) {
    return type == NS_TOKEN_CLOSE_BRACE || type == NS_TOKEN_CLOSE_PAREN || type == NS_TOKEN_CLOSE_BRACKET;
}

// `a+b` -> `a + b`. A binary operator wants exactly one space on both sides. An
// operator that ends a line continues the expression, so only its left side is
// checked; a unary `-`/`+` is skipped entirely.
static void ns_lint_check_binary_op(ns_lint_ctx *c, i32 index) {
    ns_lint_token *op = &c->tokens[index];
    if (index == 0) return;
    ns_lint_token *prev = &c->tokens[index - 1];
    if (!ns_lint_ends_value(prev->type)) return; // unary, or not an expression

    if (prev->line == op->line && ns_lint_blank(c, prev->end, op->start) && !ns_lint_single_space(c, prev->end, op->start)) {
        ns_str message = prev->end == op->start
                             ? ns_lint_message("binary operator `%.*s` needs a space before it", op->val.len, op->val.data)
                             : ns_lint_message("binary operator `%.*s` wants a single space before it", op->val.len, op->val.data);
        ns_lint_report_fix(c, NS_LINT_BINARY_OP_SPACE, op->start, prev->end, op->start, ns_str_cstr(" "), message);
    }

    if (index + 1 >= c->count) return;
    ns_lint_token *next = &c->tokens[index + 1];
    if (next->type == NS_TOKEN_EOL || next->type == NS_TOKEN_COMMENT) return; // line continuation
    if (next->line != op->line || !ns_lint_blank(c, op->end, next->start)) return;
    if (ns_lint_single_space(c, op->end, next->start)) return;
    ns_str message = op->end == next->start
                         ? ns_lint_message("binary operator `%.*s` needs a space after it", op->val.len, op->val.data)
                         : ns_lint_message("binary operator `%.*s` wants a single space after it", op->val.len, op->val.data);
    ns_lint_report_fix(c, NS_LINT_BINARY_OP_SPACE, op->start, op->end, next->start, ns_str_cstr(" "), message);
}

// `,` and `:` bind to the token on their left and take one space on their
// right. A separator at end of line, or one closing an empty tail, is left as
// written.
static void ns_lint_check_glue(ns_lint_ctx *c, i32 index, ns_lint_rule rule) {
    ns_lint_token *tok = &c->tokens[index];
    if (index > 0) {
        ns_lint_token *prev = &c->tokens[index - 1];
        if (prev->line == tok->line && prev->end < tok->start && ns_lint_blank(c, prev->end, tok->start)) {
            ns_str message = ns_lint_message("`%.*s` takes no space before it", tok->val.len, tok->val.data);
            ns_lint_report_fix(c, rule, tok->start, prev->end, tok->start, ns_str_cstr(""), message);
        }
    }

    if (index + 1 >= c->count) return;
    ns_lint_token *next = &c->tokens[index + 1];
    if (next->type == NS_TOKEN_EOL || next->type == NS_TOKEN_COMMENT) return;
    if (ns_lint_closes(next->type)) return; // `[1, 2, ]` keeps its own shape
    if (next->line != tok->line || !ns_lint_blank(c, tok->end, next->start)) return;
    if (ns_lint_single_space(c, tok->end, next->start)) return;
    ns_str message = tok->end == next->start ? ns_lint_message("`%.*s` needs a space after it", tok->val.len, tok->val.data)
                                             : ns_lint_message("`%.*s` wants a single space after it", tok->val.len, tok->val.data);
    ns_lint_report_fix(c, rule, tok->start, tok->end, next->start, ns_str_cstr(" "), message);
}

static void ns_lint_check_trailing(ns_lint_ctx *c, i32 index) {
    ns_lint_token *eol = &c->tokens[index];
    i32 start = c->line_start[eol->line - 1];
    if (index > 0 && c->tokens[index - 1].end > start) start = c->tokens[index - 1].end;
    i32 end = eol->start;
    if (end <= start || start >= c->tail_start) return;
    if (!ns_lint_blank(c, start, end)) return;
    ns_str message = ns_lint_message("trailing whitespace");
    ns_lint_report_fix(c, NS_LINT_TRAILING_SPACE, start, start, end, ns_str_cstr(""), message);
}

// A source file ends with exactly one newline. This also owns the whitespace
// run at end of file so the last line is not reported twice.
static void ns_lint_check_eof(ns_lint_ctx *c) {
    i32 len = c->src.len;
    i32 tail = len;
    while (tail > 0) {
        i8 ch = c->src.data[tail - 1];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') break;
        tail--;
    }
    if (tail == 0) return; // an empty or whitespace-only file has nothing to end
    c->tail_start = tail;
    if (!ns_lint_enabled(c, NS_LINT_TRAILING_SPACE)) return;

    ns_bool crlf = false;
    for (i32 i = 1; i < len && !crlf; i++) crlf = c->src.data[i] == '\n' && c->src.data[i - 1] == '\r';
    ns_str newline = crlf ? ns_str_cstr("\r\n") : ns_str_cstr("\n");
    if (len - tail == newline.len && strncmp(c->src.data + tail, newline.data, newline.len) == 0) return;

    ns_str message = len == tail ? ns_lint_message("file does not end with a newline")
                                 : ns_lint_message("file ends with extra blank space");
    ns_lint_report_fix(c, NS_LINT_TRAILING_SPACE, tail, tail, len, newline, message);
}

// Indentation is spaces, never tabs: a tab advances to the next indent stop.
static void ns_lint_pass_indent(ns_lint_ctx *c) {
    if (!ns_lint_enabled(c, NS_LINT_TAB_INDENT)) return;
    i32 token = 0;
    for (i32 line = 1; line <= c->line_count; line++) {
        i32 begin = c->line_start[line - 1];
        i32 end = line < c->line_count ? c->line_start[line] : c->src.len;
        i32 p = begin;
        while (p < end && (c->src.data[p] == ' ' || c->src.data[p] == '\t')) p++;
        if (p == begin) continue;
        // A blank line has no indentation, only trailing whitespace.
        if (p == end || c->src.data[p] == '\n' || c->src.data[p] == '\r') continue;

        while (token < c->count && c->tokens[token].end <= begin) token++;
        // A token spanning the line start means this line lives inside a
        // multi-line string, where leading bytes are payload.
        if (token < c->count && c->tokens[token].start < begin) continue;

        i32 tabs = 0, width = 0;
        for (i32 i = begin; i < p; i++) {
            if (c->src.data[i] == '\t') {
                tabs++;
                width += c->cfg->indent - (width % c->cfg->indent);
            } else {
                width++;
            }
        }
        if (tabs == 0) continue;
        ns_str message = ns_lint_message("indentation uses %d tab%s; indent with %d spaces per level", tabs, tabs == 1 ? "" : "s", c->cfg->indent);
        if (width > NS_LINT_SPACES_MAX) {
            // Deeper than the fixer's space budget: report it, do not truncate.
            ns_lint_report(c, NS_LINT_TAB_INDENT, begin, false, message);
            continue;
        }
        ns_lint_report_fix(c, NS_LINT_TAB_INDENT, begin, begin, p, (ns_str){.data = (i8 *)ns_lint_spaces, .len = width}, message);
    }
}

// ---------------------------------------------------------------------------
// Struct literals
// ---------------------------------------------------------------------------

static const ns_lint_struct *ns_lint_find_struct(ns_lint_ctx *c, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(c->structs); i < l; i++) {
        if (ns_str_equals(c->structs[i].name, name)) return &c->structs[i];
    }
    return ns_null;
}

// Collect `struct name { field: type, ... }` declarations, keeping the field
// order a positional literal has to match.
static void ns_lint_collect_structs(ns_lint_ctx *c) {
    for (i32 k = 0; k < c->count; k++) {
        if (c->tokens[k].type != NS_TOKEN_STRUCT) continue;
        i32 name = ns_lint_next(c, k, false);
        if (name < 0 || c->tokens[name].type != NS_TOKEN_IDENTIFIER) continue;
        i32 open = ns_lint_next(c, name, false);
        if (open < 0 || c->tokens[open].type != NS_TOKEN_OPEN_BRACE) continue;

        ns_lint_struct decl = {.name = c->tokens[name].val};
        i32 depth = 0;
        ns_bool entry = false;
        for (i32 j = open; j < c->count; j++) {
            ns_token_type type = c->tokens[j].type;
            if (ns_lint_opens(type)) {
                depth++;
                entry = j == open;
                continue;
            }
            if (ns_lint_closes(type)) {
                depth--;
                if (depth <= 0) break;
                continue;
            }
            if (type == NS_TOKEN_COMMENT) continue;
            if (depth == 1 && (type == NS_TOKEN_COMMA || type == NS_TOKEN_EOL)) {
                entry = true;
                continue;
            }
            if (depth == 1 && entry) {
                entry = false;
                if (type == NS_TOKEN_IDENTIFIER && j + 1 < c->count && c->tokens[j + 1].type == NS_TOKEN_COLON) {
                    ns_array_push(decl.fields, c->tokens[j].val);
                }
            }
        }
        if (ns_array_length(decl.fields) > 0) ns_array_push(c->structs, decl);
        else ns_array_free(decl.fields);
    }
}

// Keywords that make a following `identifier {` a statement header rather than
// a struct literal.
static ns_bool ns_lint_literal_prev_ok(ns_token_type type) {
    switch (type) {
    case NS_TOKEN_STRUCT:
    case NS_TOKEN_FN:
    case NS_TOKEN_TYPE:
    case NS_TOKEN_ENUM:
    case NS_TOKEN_OPS:
    case NS_TOKEN_DOT:
    case NS_TOKEN_MODULE:
    case NS_TOKEN_USE:
    case NS_TOKEN_LET:
    case NS_TOKEN_LIT:
    case NS_TOKEN_REF:
    case NS_TOKEN_IF:
    case NS_TOKEN_FOR:
    case NS_TOKEN_IN:
    case NS_TOKEN_TO:
    case NS_TOKEN_LOOP:
    case NS_TOKEN_DO:
    case NS_TOKEN_ELSE:
        return false;
    default:
        return true;
    }
}

// `point { x: 0, y: 0 }` -> `point { 0, 0 }`. A literal that labels every field
// in declaration order carries no information in its labels.
static void ns_lint_check_struct_label(ns_lint_ctx *c, i32 index) {
    const ns_lint_struct *decl = ns_lint_find_struct(c, c->tokens[index].val);
    if (decl == ns_null) return;
    if (index > 0 && !ns_lint_literal_prev_ok(c->tokens[index - 1].type)) return;
    i32 open = index + 1;
    if (open >= c->count || c->tokens[open].type != NS_TOKEN_OPEN_BRACE) return;
    if (c->tokens[open].line != c->tokens[index].line) return;

    i32 *labels = ns_null;
    i32 *values = ns_null;
    i32 depth = 0;
    ns_bool entry = false;
    ns_bool ordered = true;
    ns_bool closed = false;
    for (i32 j = open; j < c->count && ordered; j++) {
        ns_token_type type = c->tokens[j].type;
        if (ns_lint_opens(type)) {
            depth++;
            entry = j == open;
            continue;
        }
        if (ns_lint_closes(type)) {
            depth--;
            if (depth <= 0) {
                closed = true;
                break;
            }
            continue;
        }
        if (type == NS_TOKEN_COMMENT) continue;
        if (depth == 1 && (type == NS_TOKEN_COMMA || type == NS_TOKEN_EOL)) {
            entry = true;
            continue;
        }
        if (depth != 1 || !entry) continue;

        entry = false;
        i32 field = (i32)ns_array_length(labels);
        i32 value = j + 2;
        ordered = type == NS_TOKEN_IDENTIFIER && j + 1 < c->count && c->tokens[j + 1].type == NS_TOKEN_COLON &&
                  value < c->count && field < (i32)ns_array_length(decl->fields) &&
                  ns_str_equals(c->tokens[j].val, decl->fields[field]) && c->tokens[value].line == c->tokens[j].line;
        if (!ordered) break;
        ns_array_push(labels, j);
        ns_array_push(values, value);
    }

    i32 count = (i32)ns_array_length(labels);
    if (ordered && closed && count > 0 && count == (i32)ns_array_length(decl->fields)) {
        ns_bool applied = true;
        for (i32 i = 0; i < count; i++) {
            i32 from = c->tokens[labels[i]].start;
            i32 to = c->tokens[values[i]].start;
            applied = ns_lint_push_edit(c, from, to, ns_str_cstr("")) && applied;
        }
        if (!c->fix || applied) {
            ns_str message = ns_lint_message("struct literal `%.*s` labels every field in declaration order; drop the labels",
                                             decl->name.len, decl->name.data);
            ns_lint_report(c, NS_LINT_STRUCT_LABEL, c->tokens[index].start, applied, message);
        }
    }
    ns_array_free(labels);
    ns_array_free(values);
}

// ---------------------------------------------------------------------------
// Nested scopes
// ---------------------------------------------------------------------------

// A `{` in expression position, or one opening a `{ arg, ... in ... }` block,
// starts a nested fn. Control-flow bodies and declarations do not.
static ns_bool ns_lint_is_block(ns_lint_ctx *c, i32 open) {
    if (open > 0) {
        switch (c->tokens[open - 1].type) {
        case NS_TOKEN_ASSIGN:
        case NS_TOKEN_ASSIGN_OP:
        case NS_TOKEN_OPEN_PAREN:
        case NS_TOKEN_OPEN_BRACKET:
        case NS_TOKEN_COMMA:
        case NS_TOKEN_COLON:
        case NS_TOKEN_RETURN:
            return true;
        default:
            break;
        }
    }
    // `{ a, b in ...`: an argument list followed by `in` on the header line.
    for (i32 j = open + 1; j < c->count; j++) {
        ns_token_type type = c->tokens[j].type;
        if (type == NS_TOKEN_IN) return true;
        if (type == NS_TOKEN_IDENTIFIER || type == NS_TOKEN_COMMA) continue;
        return false;
    }
    return false;
}

static void ns_lint_check_name(ns_lint_ctx *c, i32 index) {
    ns_lint_token *tok = &c->tokens[index];
    i32 length = ns_str_utf8_len(tok->val);
    if (length <= c->cfg->nested_name_max) return;
    ns_str message = ns_lint_message("`%.*s` is %d chars; a nested fn should use short names (max %d)", tok->val.len, tok->val.data, length,
                                     c->cfg->nested_name_max);
    ns_lint_report(c, NS_LINT_NESTED_NAME, tok->start, false, message);
}

static void ns_lint_pass_nested_name(ns_lint_ctx *c) {
    if (!ns_lint_enabled(c, NS_LINT_NESTED_NAME)) return;

    ns_bool *stack = ns_null;
    ns_bool nested = false;
    for (i32 k = 0; k < c->count; k++) {
        ns_token_type type = c->tokens[k].type;
        if (type == NS_TOKEN_OPEN_BRACE) {
            ns_array_push(stack, nested);
            ns_bool block = ns_lint_is_block(c, k);
            nested = nested || block;
            if (nested && block) {
                for (i32 j = k + 1; j < c->count; j++) { // `{ arg, ... in`
                    if (c->tokens[j].type == NS_TOKEN_IDENTIFIER) ns_lint_check_name(c, j);
                    else if (c->tokens[j].type != NS_TOKEN_COMMA) break;
                }
            }
            continue;
        }
        if (type == NS_TOKEN_CLOSE_BRACE) {
            nested = ns_array_length(stack) > 0 ? ns_array_pop(stack) : false;
            continue;
        }
        if (!nested) continue;
        if (type != NS_TOKEN_LET && type != NS_TOKEN_LIT && type != NS_TOKEN_FOR) continue;
        i32 name = ns_lint_next(c, k, true);
        if (name >= 0 && c->tokens[name].type == NS_TOKEN_IDENTIFIER) ns_lint_check_name(c, name);
    }
    ns_array_free(stack);
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static ns_str ns_lint_apply_edits(ns_lint_ctx *c) {
    i32 count = (i32)ns_array_length(c->edits);
    if (count == 0) return ns_str_null;
    qsort(c->edits, count, sizeof(ns_lint_edit), ns_lint_edit_cmp);

    ns_str out = ns_str_null;
    i32 cursor = 0;
    for (i32 i = 0; i < count; i++) {
        ns_lint_edit *e = &c->edits[i];
        if (e->start < cursor) continue; // defensive: overlapping edits never apply twice
        ns_str_append_len(&out, c->src.data + cursor, e->start - cursor);
        ns_str_append_len(&out, e->text.data, e->text.len);
        cursor = e->end;
    }
    ns_str_append_len(&out, c->src.data + cursor, c->src.len - cursor);
    ns_array_push(out.data, '\0');
    out.dynamic = false; // ns_array-backed, freed with ns_array_free
    return out;
}

static void ns_lint_run(ns_lint_ctx *c) {
    ns_lint_scan(c);
    c->tail_start = c->src.len;
    ns_lint_check_eof(c);

    if (ns_lint_enabled(c, NS_LINT_STRUCT_LABEL)) {
        ns_lint_collect_structs(c);
        for (i32 k = 0; k < c->count; k++) {
            if (c->tokens[k].type == NS_TOKEN_IDENTIFIER) ns_lint_check_struct_label(c, k);
        }
    }

    for (i32 k = 0; k < c->count; k++) {
        ns_token_type type = c->tokens[k].type;
        if (ns_lint_is_binary_op(type)) {
            if (ns_lint_enabled(c, NS_LINT_BINARY_OP_SPACE)) ns_lint_check_binary_op(c, k);
        } else if (type == NS_TOKEN_COMMA) {
            if (ns_lint_enabled(c, NS_LINT_COMMA_SPACE)) ns_lint_check_glue(c, k, NS_LINT_COMMA_SPACE);
        } else if (type == NS_TOKEN_COLON) {
            if (ns_lint_enabled(c, NS_LINT_COLON_SPACE)) ns_lint_check_glue(c, k, NS_LINT_COLON_SPACE);
        } else if (type == NS_TOKEN_EOL) {
            if (ns_lint_enabled(c, NS_LINT_TRAILING_SPACE)) ns_lint_check_trailing(c, k);
        }
    }

    ns_lint_pass_indent(c);
    ns_lint_pass_nested_name(c);
}

static void ns_lint_ctx_free(ns_lint_ctx *c) {
    for (i32 i = 0, l = (i32)ns_array_length(c->structs); i < l; i++) ns_array_free(c->structs[i].fields);
    ns_array_free(c->structs);
    ns_array_free(c->tokens);
    ns_array_free(c->line_start);
    ns_array_free(c->edits);
}

static void ns_lint_sort_from(ns_lint_result *out, i32 first) {
    i32 count = (i32)ns_array_length(out->diagnostics) - first;
    if (count > 1) qsort(&out->diagnostics[first], count, sizeof(ns_lint_diagnostic), ns_lint_diagnostic_cmp);
}

void ns_lint_source(const ns_lint_config *cfg, ns_str source, ns_str file, ns_lint_result *out) {
    if (source.data == ns_null) return;
    i32 first = (i32)ns_array_length(out->diagnostics);
    ns_lint_ctx c = {.cfg = cfg, .src = source, .file = file, .out = out};
    out->files++;
    ns_lint_run(&c);
    ns_lint_ctx_free(&c);
    ns_lint_sort_from(out, first);
}

ns_str ns_lint_fix(const ns_lint_config *cfg, ns_str source, ns_str file, ns_lint_result *out) {
    if (source.data == ns_null) return ns_str_null;
    i32 first = (i32)ns_array_length(out->diagnostics);
    ns_lint_ctx c = {.cfg = cfg, .src = source, .file = file, .out = out, .fix = true};
    out->files++;
    ns_lint_run(&c);
    ns_str fixed = ns_lint_apply_edits(&c);
    ns_lint_ctx_free(&c);
    ns_lint_sort_from(out, first);
    return fixed;
}

void ns_lint_diagnostic_print(const ns_lint_diagnostic *d) {
    ns_str rule = ns_lint_rule_name(d->rule);
    const char *color = d->severity == NS_LINT_ERROR ? ns_color_err : ns_color_wrn;
    ns_str label = ns_lint_severity_name(d->severity);
    const char *note = d->fixed ? " (fixed)" : d->fixable ? " (fixable)" : "";
    printf("%.*s:%d:%d: %s%.*s" ns_color_nil " %.*s: %.*s%s\n", d->file.len, d->file.data, d->line, d->column, color, label.len, label.data,
           rule.len, rule.data, d->message.len, d->message.data, note);
}

void ns_lint_result_free(ns_lint_result *out) {
    for (i32 i = 0, l = (i32)ns_array_length(out->diagnostics); i < l; i++) ns_array_free(out->diagnostics[i].message.data);
    ns_array_free(out->diagnostics);
    out->errors = 0;
    out->warnings = 0;
    out->fixable = 0;
    out->fixed = 0;
    out->files = 0;
}
