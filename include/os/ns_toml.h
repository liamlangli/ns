#pragma once

#include "ns_type.h"

/**
 *   DOC: ns toml lib.
 *   A deliberately minimal TOML reader, just enough to describe a project's
 * build/target specification in `ns.toml`. It supports:
 *     - table headers:   [section]
 *     - key/value pairs: key = "string" | key = bare | key = 123 | key = true
 *     - line comments starting with '#'
 *   Nested tables are flattened by their literal header text (e.g. "target").
 * Quotes around string values are stripped. Everything is stored as an ns_str
 * slice into the source buffer, so the source must outlive the ns_toml.
 */

typedef struct ns_toml_entry {
    ns_str section; // table name, or empty for top-level keys
    ns_str key;
    ns_str value;   // raw value with surrounding quotes stripped
} ns_toml_entry;

typedef struct ns_toml {
    ns_toml_entry *entries; // ns_array
} ns_toml;

ns_toml ns_toml_parse(ns_str src);

// lookup a value by table + key. returns ns_str_null when absent.
ns_str ns_toml_get(ns_toml *t, ns_str section, ns_str key);
ns_bool ns_toml_get_bool(ns_toml *t, ns_str section, ns_str key, ns_bool fallback);

void ns_toml_free(ns_toml *t);
