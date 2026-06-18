#include "ns_toml.h"

static ns_bool ns_toml_is_space(i8 c) { return c == ' ' || c == '\t' || c == '\r'; }

static ns_str ns_toml_trim(ns_str s) {
    i32 start = 0, end = s.len;
    while (start < end && ns_toml_is_space(s.data[start])) start++;
    while (end > start && ns_toml_is_space(s.data[end - 1])) end--;
    return ns_str_slice(s, start, end);
}

// strip matching surrounding quotes (single or double) from a value
static ns_str ns_toml_unquote(ns_str s) {
    if (s.len >= 2 && (s.data[0] == '"' || s.data[0] == '\'') && s.data[s.len - 1] == s.data[0]) {
        return ns_str_slice(s, 1, s.len - 1);
    }
    return s;
}

ns_toml ns_toml_parse(ns_str src) {
    ns_toml t = {0};
    ns_str section = ns_str_null;

    i32 i = 0;
    while (i < src.len) {
        // find end of line
        i32 j = i;
        while (j < src.len && src.data[j] != '\n') j++;
        ns_str line = ns_toml_trim(ns_str_slice(src, i, j));
        i = j + 1;

        if (line.len == 0 || line.data[0] == '#') continue;

        if (line.data[0] == '[') {
            i32 close = 1;
            while (close < line.len && line.data[close] != ']') close++;
            section = ns_toml_trim(ns_str_slice(line, 1, close));
            continue;
        }

        // key = value
        i32 eq = ns_str_index_of(line, ns_str_cstr("="));
        if (eq < 0) continue;
        ns_str key = ns_toml_trim(ns_str_slice(line, 0, eq));
        ns_str value = ns_toml_trim(ns_str_slice(line, eq + 1, line.len));

        // drop trailing inline comment on unquoted values
        if (value.len > 0 && value.data[0] != '"' && value.data[0] != '\'') {
            i32 hash = ns_str_index_of(value, ns_str_cstr("#"));
            if (hash >= 0) value = ns_toml_trim(ns_str_slice(value, 0, hash));
        }
        value = ns_toml_unquote(value);

        if (key.len == 0) continue;
        ns_array_push(t.entries, ((ns_toml_entry){.section = section, .key = key, .value = value}));
    }

    return t;
}

ns_str ns_toml_get(ns_toml *t, ns_str section, ns_str key) {
    for (i32 i = 0, l = (i32)ns_array_length(t->entries); i < l; i++) {
        ns_toml_entry *e = &t->entries[i];
        if (ns_str_equals(e->section, section) && ns_str_equals(e->key, key)) {
            return e->value;
        }
    }
    return ns_str_null;
}

ns_bool ns_toml_get_bool(ns_toml *t, ns_str section, ns_str key, ns_bool fallback) {
    ns_str v = ns_toml_get(t, section, key);
    if (v.len == 0) return fallback;
    return ns_str_equals(v, ns_str_cstr("true"));
}

void ns_toml_free(ns_toml *t) {
    ns_array_free(t->entries);
}
