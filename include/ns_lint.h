#pragma once

#include "ns_type.h"

// Nano Script linter.
//
// The linter works on the token stream instead of the AST: style is about the
// bytes between tokens, and a file that does not parse should still be
// tidied. Every rule reports a diagnostic; the fixable ones also produce a
// source edit, so `ns lint` and `ns lint_fix` share one analysis pass.
//
// Rules are customized by the `[lint]` table of a project's ns.mod:
//
//     [lint]
//     indent = 4
//     nested_name_max = 8
//     binary_op_space = "error"
//     struct_label = "warn"
//     nested_name = "off"
//     snake_case = "off"

typedef enum ns_lint_severity {
    NS_LINT_OFF = 0,
    NS_LINT_WARN,
    NS_LINT_ERROR,
} ns_lint_severity;

typedef enum ns_lint_rule {
    NS_LINT_BINARY_OP_SPACE = 0, // a binary operator takes one space on both sides
    NS_LINT_COMMA_SPACE,         // `,` binds to the left and is followed by one space
    NS_LINT_COLON_SPACE,         // `:` binds to the left and is followed by one space
    NS_LINT_TRAILING_SPACE,      // no whitespace at end of line, one newline at end of file
    NS_LINT_TAB_INDENT,          // indent with spaces, never with tabs
    NS_LINT_STRUCT_LABEL,        // a struct literal listing every field in order needs no labels
    NS_LINT_NESTED_NAME,         // a nested fn/block keeps its own names short
    NS_LINT_SNAKE_CASE,          // identifiers use snake_case; camelCase/PascalCase are rewritten
    NS_LINT_RULE_COUNT,
} ns_lint_rule;

typedef struct ns_lint_config {
    ns_lint_severity severity[NS_LINT_RULE_COUNT];
    i32 indent;          // spaces per indentation level
    i32 nested_name_max; // longest name allowed inside a nested fn/block
} ns_lint_config;

typedef struct ns_lint_diagnostic {
    ns_lint_rule rule;
    ns_lint_severity severity;
    ns_str file;      // borrowed from the caller
    ns_str message;   // ns_array-backed, released by ns_lint_result_free
    i32 line;         // 1-based
    i32 column;       // 1-based, counted in bytes
    ns_bool fixable;  // the fixer knows how to rewrite this one
    ns_bool fixed;    // and `ns lint_fix` did
} ns_lint_diagnostic;

typedef struct ns_lint_result {
    ns_lint_diagnostic *diagnostics; // ns_array
    i32 errors;
    i32 warnings;
    i32 fixable;
    i32 fixed;
    i32 files;
} ns_lint_result;

ns_str ns_lint_rule_name(ns_lint_rule rule);
ns_str ns_lint_severity_name(ns_lint_severity severity);
// Parse "error" | "warn" | "warning" | "off" | "none" | "false", or return
// false when the value names no severity.
ns_bool ns_lint_severity_from_str(ns_str value, ns_lint_severity *out);
ns_bool ns_lint_rule_from_str(ns_str name, ns_lint_rule *out);
// True when the fixer can rewrite this rule's findings.
ns_bool ns_lint_rule_fixable(ns_lint_rule rule);

void ns_lint_config_default(ns_lint_config *cfg);
// Apply the `[lint]` table of an ns.mod manifest on top of `cfg`. Unknown keys
// and unknown values warn and are otherwise ignored.
void ns_lint_config_from_manifest(ns_lint_config *cfg, ns_str manifest);

// Append the diagnostics of one source buffer to `out`. `file` is borrowed and
// only used for reporting, so it must outlive `out`.
void ns_lint_source(const ns_lint_config *cfg, ns_str source, ns_str file, ns_lint_result *out);

// Same analysis, plus the rewritten source. Returns ns_str_null when nothing
// changed; otherwise the result is ns_array-backed (free with
// ns_array_free(fixed.data)) and null-terminated. Diagnostics the rewrite
// resolved are appended with `fixed` set.
ns_str ns_lint_fix(const ns_lint_config *cfg, ns_str source, ns_str file, ns_lint_result *out);

void ns_lint_diagnostic_print(const ns_lint_diagnostic *diagnostic);
void ns_lint_result_free(ns_lint_result *out);
