#include "storage.internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NS_WIN
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STORAGE_KV_CAPACITY 256
#define STORAGE_ERROR_CAPACITY 512

enum storage_kv_type {
    STORAGE_KV_STRING = 1,
    STORAGE_KV_I64,
    STORAGE_KV_F64,
    STORAGE_KV_BOOL,
};

typedef struct storage_kv_entry {
    char *key;
    char *string_value;
    i64 i64_value;
    f64 f64_value;
    i32 bool_value;
    i32 type;
} storage_kv_entry;

static storage_kv_entry storage_entries[STORAGE_KV_CAPACITY];
static i32 storage_entry_count;
static char storage_error[STORAGE_ERROR_CAPACITY];
static char storage_root[STORAGE_PATH_CAPACITY];
static char storage_json_path[STORAGE_PATH_CAPACITY];
static i32 storage_ready;

static char *storage_copy_string(const char *value) {
    if (!value) value = "";
    size_t size = strlen(value) + 1;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

void storage_clear_error(void) {
    storage_error[0] = '\0';
}

void storage_set_error(const char *message) {
    snprintf(storage_error, sizeof(storage_error), "%s", message ? message : "storage error");
}

void storage_set_errorf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(storage_error, sizeof(storage_error), format, args);
    va_end(args);
}

const char *storage_last_error(void) {
    return storage_error;
}

i32 storage_make_dirs(const char *path) {
    if (!path || !path[0]) return 0;
    char copy[STORAGE_PATH_CAPACITY];
    i32 count = snprintf(copy, sizeof(copy), "%s", path);
    if (count < 0 || (size_t)count >= sizeof(copy)) return 0;
#ifdef NS_WIN
    size_t start = strlen(copy) >= 3 && copy[1] == ':' ? 3 : 0;
#else
    size_t start = copy[0] == '/' ? 1 : 0;
#endif
    for (size_t i = start; copy[i]; ++i) {
        if (copy[i] != '/' && copy[i] != '\\') continue;
        char separator = copy[i];
        copy[i] = '\0';
#ifdef NS_WIN
        if (_mkdir(copy) != 0 && errno != EEXIST) return 0;
#else
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) return 0;
#endif
        copy[i] = separator;
    }
#ifdef NS_WIN
    return _mkdir(copy) == 0 || errno == EEXIST;
#else
    return mkdir(copy, 0755) == 0 || errno == EEXIST;
#endif
}

const char *storage_app_data_dir(void) {
    if (!storage_ready) storage_set_error("storage_init must be called first");
    return storage_ready ? storage_root : "";
}

static void storage_free_entries(void) {
    for (i32 i = 0; i < storage_entry_count; ++i) {
        free(storage_entries[i].key);
        free(storage_entries[i].string_value);
        storage_entries[i] = (storage_kv_entry){0};
    }
    storage_entry_count = 0;
}

static i32 storage_find(const char *key) {
    if (!key) return -1;
    for (i32 i = 0; i < storage_entry_count; ++i) {
        if (strcmp(storage_entries[i].key, key) == 0) return i;
    }
    return -1;
}

static storage_kv_entry *storage_upsert(const char *key) {
    if (!key || !key[0]) {
        storage_set_error("storage key must not be empty");
        return NULL;
    }
    i32 index = storage_find(key);
    if (index >= 0) return &storage_entries[index];
    if (storage_entry_count >= STORAGE_KV_CAPACITY) {
        storage_set_error("storage KV capacity reached");
        return NULL;
    }
    char *copy = storage_copy_string(key);
    if (!copy) {
        storage_set_error("out of memory storing key");
        return NULL;
    }
    storage_kv_entry *entry = &storage_entries[storage_entry_count++];
    *entry = (storage_kv_entry){ .key = copy };
    return entry;
}

static i32 storage_write_json_string(FILE *file, const char *value) {
    if (fputc('"', file) == EOF) return 0;
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p) {
        switch (*p) {
            case '"': if (fputs("\\\"", file) == EOF) return 0; break;
            case '\\': if (fputs("\\\\", file) == EOF) return 0; break;
            case '\b': if (fputs("\\b", file) == EOF) return 0; break;
            case '\f': if (fputs("\\f", file) == EOF) return 0; break;
            case '\n': if (fputs("\\n", file) == EOF) return 0; break;
            case '\r': if (fputs("\\r", file) == EOF) return 0; break;
            case '\t': if (fputs("\\t", file) == EOF) return 0; break;
            default:
                if (*p < 0x20) {
                    if (fprintf(file, "\\u%04x", *p) < 0) return 0;
                } else if (fputc(*p, file) == EOF) return 0;
        }
    }
    return fputc('"', file) != EOF;
}

static i32 storage_write_json(void) {
    if (!storage_ready) {
        storage_set_error("storage_init must be called first");
        return 0;
    }
    char temporary[STORAGE_PATH_CAPACITY];
    i32 count = snprintf(temporary, sizeof(temporary), "%s.tmp", storage_json_path);
    if (count < 0 || (size_t)count >= sizeof(temporary)) {
        storage_set_error("storage path is too long");
        return 0;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        storage_set_errorf("could not write storage JSON: %s", strerror(errno));
        return 0;
    }
    i32 ok = fputs("{\n  \"version\": 1,\n  \"values\": [", file) != EOF;
    for (i32 i = 0; ok && i < storage_entry_count; ++i) {
        storage_kv_entry *entry = &storage_entries[i];
        ok = fputs(i == 0 ? "\n    {\"key\": " : ",\n    {\"key\": ", file) != EOF &&
             storage_write_json_string(file, entry->key) && fputs(", \"type\": ", file) != EOF;
        const char *type = entry->type == STORAGE_KV_STRING ? "string" : entry->type == STORAGE_KV_I64 ? "i64" :
                           entry->type == STORAGE_KV_F64 ? "f64" : "bool";
        ok = ok && storage_write_json_string(file, type) && fputs(", \"value\": ", file) != EOF;
        if (entry->type == STORAGE_KV_STRING) ok = ok && storage_write_json_string(file, entry->string_value);
        else if (entry->type == STORAGE_KV_I64) ok = ok && fprintf(file, "%lld", (long long)entry->i64_value) >= 0;
        else if (entry->type == STORAGE_KV_F64) ok = ok && fprintf(file, "%.17g", entry->f64_value) >= 0;
        else ok = ok && fputs(entry->bool_value ? "true" : "false", file) != EOF;
        ok = ok && fputc('}', file) != EOF;
    }
    ok = ok && fputs(storage_entry_count ? "\n  ]\n}\n" : "]\n}\n", file) != EOF && fflush(file) == 0;
#ifndef NS_WIN
    if (ok && fsync(fileno(file)) != 0) ok = 0;
#endif
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        remove(temporary);
        storage_set_error("could not finish storage JSON write");
        return 0;
    }
#ifdef NS_WIN
    if (!MoveFileExA(temporary, storage_json_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(temporary);
        storage_set_error("could not replace storage JSON");
        return 0;
    }
#else
    if (rename(temporary, storage_json_path) != 0) {
        remove(temporary);
        storage_set_errorf("could not replace storage JSON: %s", strerror(errno));
        return 0;
    }
#endif
    return 1;
}

static const char *storage_skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static const char *storage_parse_string(const char *p, char **result) {
    p = storage_skip_space(p);
    if (*p != '"') return NULL;
    ++p;
    size_t capacity = strlen(p) + 1;
    char *text = (char *)malloc(capacity);
    if (!text) return NULL;
    size_t length = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (!c) { free(text); return NULL; }
            if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'u') {
                // Writer only emits JSON unicode escapes for control bytes.
                if (p[0] != '0' || p[1] != '0' || !isxdigit((unsigned char)p[2]) || !isxdigit((unsigned char)p[3])) {
                    free(text); return NULL;
                }
                char hex[3] = { p[2], p[3], 0 };
                c = (unsigned char)strtoul(hex, NULL, 16);
                p += 4;
            }
        }
        text[length++] = (char)c;
    }
    if (*p != '"') { free(text); return NULL; }
    text[length] = '\0';
    *result = text;
    return p + 1;
}

static const char *storage_expect(const char *p, char expected) {
    p = storage_skip_space(p);
    return *p == expected ? p + 1 : NULL;
}

static i32 storage_load_json(void) {
    FILE *file = fopen(storage_json_path, "rb");
    if (!file) return errno == ENOENT;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    char *source = (char *)malloc((size_t)size + 1);
    if (!source) { fclose(file); return 0; }
    i32 ok = fread(source, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    source[size] = '\0';
    const char *p = ok ? strstr(source, "\"values\"") : NULL;
    p = p ? strchr(p, '[') : NULL;
    if (p) ++p;
    while (p && *(p = storage_skip_space(p)) && *p != ']') {
        if (*p == ',') p = storage_skip_space(p + 1);
        p = storage_expect(p, '{');
        char *label = NULL;
        char *key = NULL;
        char *type = NULL;
        char *string_value = NULL;
        p = p ? storage_parse_string(p, &label) : NULL;
        p = p && strcmp(label, "key") == 0 ? storage_expect(p, ':') : NULL;
        p = p ? storage_parse_string(p, &key) : NULL;
        p = p ? storage_expect(p, ',') : NULL;
        free(label); label = NULL;
        p = p ? storage_parse_string(p, &label) : NULL;
        p = p && strcmp(label, "type") == 0 ? storage_expect(p, ':') : NULL;
        p = p ? storage_parse_string(p, &type) : NULL;
        p = p ? storage_expect(p, ',') : NULL;
        free(label); label = NULL;
        p = p ? storage_parse_string(p, &label) : NULL;
        p = p && strcmp(label, "value") == 0 ? storage_expect(p, ':') : NULL;
        if (!p || !key || !type || storage_entry_count >= STORAGE_KV_CAPACITY) ok = 0;
        storage_kv_entry entry = { .key = key };
        if (ok && strcmp(type, "string") == 0) {
            p = storage_parse_string(p, &string_value);
            entry.type = STORAGE_KV_STRING;
            entry.string_value = string_value;
        } else if (ok && strcmp(type, "i64") == 0) {
            char *end = NULL;
            entry.i64_value = (i64)strtoll(p, &end, 10);
            if (end == p) ok = 0; else p = end;
            entry.type = STORAGE_KV_I64;
        } else if (ok && strcmp(type, "f64") == 0) {
            char *end = NULL;
            entry.f64_value = strtod(p, &end);
            if (end == p) ok = 0; else p = end;
            entry.type = STORAGE_KV_F64;
        } else if (ok) {
            // Unlike the string and number readers above, strncmp does not skip
            // the space the writer puts after the colon, so this one has to.
            const char *literal = storage_skip_space(p);
            if (strncmp(literal, "true", 4) == 0) {
                p = literal + 4; entry.type = STORAGE_KV_BOOL; entry.bool_value = 1;
            } else if (strncmp(literal, "false", 5) == 0) {
                p = literal + 5; entry.type = STORAGE_KV_BOOL;
            } else ok = 0;
        } else ok = 0;
        p = ok ? storage_expect(p, '}') : NULL;
        if (!p) ok = 0;
        if (ok) storage_entries[storage_entry_count++] = entry;
        else { free(key); free(string_value); }
        free(label);
        free(type);
        if (!ok) break;
    }
    free(source);
    if (!ok || !p) {
        storage_free_entries();
        storage_set_error("storage JSON is invalid");
        return 0;
    }
    return 1;
}

i32 storage_init(const char *app_name) {
    storage_clear_error();
    storage_free_entries();
    storage_ready = 0;
    const char *input = app_name && app_name[0] ? app_name : "ns";
    char safe[128];
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && length + 1 < sizeof(safe); ++p) {
        safe[length++] = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' ? (char)*p : '_';
    }
    safe[length] = '\0';
    if (strcmp(safe, ".") == 0 || strcmp(safe, "..") == 0) snprintf(safe, sizeof(safe), "ns");
#ifdef NS_WIN
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("USERPROFILE");
    i32 root_count = snprintf(storage_root, sizeof(storage_root), "%s\\%s", base && base[0] ? base : ".", safe);
    i32 json_count = snprintf(storage_json_path, sizeof(storage_json_path), "%s\\storage.json", storage_root);
#else
    const char *base = getenv("XDG_DATA_HOME");
    i32 root_count = base && base[0]
        ? snprintf(storage_root, sizeof(storage_root), "%s/%s", base, safe)
        : snprintf(storage_root, sizeof(storage_root), "%s/.local/share/%s", getenv("HOME") ? getenv("HOME") : ".", safe);
    i32 json_count = snprintf(storage_json_path, sizeof(storage_json_path), "%s/storage.json", storage_root);
#endif
    if (root_count < 0 || json_count < 0 || (size_t)root_count >= sizeof(storage_root) ||
        (size_t)json_count >= sizeof(storage_json_path) || !storage_make_dirs(storage_root)) {
        storage_set_error("could not create application storage directory");
        return 0;
    }
    storage_ready = 1;
    return storage_load_json();
}

i32 storage_kv_has(const char *key) {
    return storage_find(key) >= 0;
}

static i32 storage_set_entry(storage_kv_entry *entry, i32 type) {
    if (!entry) return 0;
    if (type != STORAGE_KV_STRING) { free(entry->string_value); entry->string_value = NULL; }
    entry->type = type;
    return storage_write_json();
}

i32 storage_kv_set_str(const char *key, const char *value) {
    storage_clear_error();
    storage_kv_entry *entry = storage_upsert(key);
    char *copy = storage_copy_string(value);
    if (!entry || !copy) { free(copy); if (!storage_error[0]) storage_set_error("out of memory storing value"); return 0; }
    free(entry->string_value);
    entry->string_value = copy;
    return storage_set_entry(entry, STORAGE_KV_STRING);
}

i32 storage_kv_set_i64(const char *key, i64 value) {
    storage_clear_error();
    storage_kv_entry *entry = storage_upsert(key);
    if (entry) entry->i64_value = value;
    return storage_set_entry(entry, STORAGE_KV_I64);
}

i32 storage_kv_set_f64(const char *key, f64 value) {
    storage_clear_error();
    if (!isfinite(value)) {
        storage_set_error("JSON storage cannot represent a non-finite number");
        return 0;
    }
    storage_kv_entry *entry = storage_upsert(key);
    if (entry) entry->f64_value = value;
    return storage_set_entry(entry, STORAGE_KV_F64);
}

i32 storage_kv_set_bool(const char *key, i32 value) {
    storage_clear_error();
    storage_kv_entry *entry = storage_upsert(key);
    if (entry) entry->bool_value = value != 0;
    return storage_set_entry(entry, STORAGE_KV_BOOL);
}

const char *storage_kv_get_str(const char *key, const char *fallback) {
    i32 index = storage_find(key);
    return index >= 0 && storage_entries[index].type == STORAGE_KV_STRING ? storage_entries[index].string_value : fallback;
}

i64 storage_kv_get_i64(const char *key, i64 fallback) {
    i32 index = storage_find(key);
    return index >= 0 && storage_entries[index].type == STORAGE_KV_I64 ? storage_entries[index].i64_value : fallback;
}

f64 storage_kv_get_f64(const char *key, f64 fallback) {
    i32 index = storage_find(key);
    return index >= 0 && storage_entries[index].type == STORAGE_KV_F64 ? storage_entries[index].f64_value : fallback;
}

i32 storage_kv_get_bool(const char *key, i32 fallback) {
    i32 index = storage_find(key);
    return index >= 0 && storage_entries[index].type == STORAGE_KV_BOOL ? storage_entries[index].bool_value : fallback;
}

i32 storage_kv_remove(const char *key) {
    storage_clear_error();
    i32 index = storage_find(key);
    if (index < 0) return 1;
    free(storage_entries[index].key);
    free(storage_entries[index].string_value);
    for (i32 i = index; i + 1 < storage_entry_count; ++i) storage_entries[i] = storage_entries[i + 1];
    storage_entries[--storage_entry_count] = (storage_kv_entry){0};
    return storage_write_json();
}

i32 storage_kv_clear(void) {
    storage_clear_error();
    storage_free_entries();
    return storage_write_json();
}

i32 storage_kv_sync(void) {
    storage_clear_error();
    return storage_write_json();
}
