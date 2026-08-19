#ifndef NS_STORAGE_H
#define NS_STORAGE_H

#include "ns_type.h"

typedef struct storage_db storage_db;
typedef struct storage_stmt storage_stmt;

enum {
    STORAGE_VALUE_NULL = 0,
    STORAGE_VALUE_I64 = 1,
    STORAGE_VALUE_F64 = 2,
    STORAGE_VALUE_TEXT = 3,
    STORAGE_VALUE_BLOB = 4,
};

enum {
    STORAGE_STEP_ERROR = -1,
    STORAGE_STEP_DONE = 0,
    STORAGE_STEP_ROW = 1,
};

i32 storage_init(const char *app_name);
const char *storage_last_error(void);

i32 storage_kv_has(const char *key);
i32 storage_kv_set_str(const char *key, const char *value);
i32 storage_kv_set_i64(const char *key, i64 value);
i32 storage_kv_set_f64(const char *key, f64 value);
i32 storage_kv_set_bool(const char *key, i32 value);
const char *storage_kv_get_str(const char *key, const char *fallback);
i64 storage_kv_get_i64(const char *key, i64 fallback);
f64 storage_kv_get_f64(const char *key, f64 fallback);
i32 storage_kv_get_bool(const char *key, i32 fallback);
i32 storage_kv_remove(const char *key);
i32 storage_kv_clear(void);
i32 storage_kv_sync(void);

// Content-addressed blob cache below the app data directory. `name` says what
// is cached and `hash` identifies the bytes it was derived from, so a changed
// input misses rather than returning a stale entry. Writing or committing one
// generation of `name` retires the others.
u64 storage_cache_hash(const u8 *data, i32 size);
u64 storage_cache_hash_str(const char *text);
const char *storage_cache_path(const char *name, u64 hash);
i32 storage_cache_has(const char *name, u64 hash);
i32 storage_cache_size(const char *name, u64 hash);
i32 storage_cache_read(const char *name, u64 hash, u8 *data, i32 capacity);
i32 storage_cache_write(const char *name, u64 hash, const u8 *data, i32 size);
i32 storage_cache_adopt(const char *name, u64 hash, const char *path);
i32 storage_cache_retire(const char *name, u64 hash);
i32 storage_cache_remove(const char *name);
i32 storage_cache_clear(void);

storage_db *storage_db_open(const char *name);
void storage_db_close(storage_db *db);
i32 storage_db_exec(storage_db *db, const char *sql);
storage_stmt *storage_db_prepare(storage_db *db, const char *sql);
i32 storage_db_changes(storage_db *db);
i64 storage_db_last_insert_id(storage_db *db);

i32 storage_stmt_bind_null(storage_stmt *stmt, i32 index);
i32 storage_stmt_bind_i64(storage_stmt *stmt, i32 index, i64 value);
i32 storage_stmt_bind_f64(storage_stmt *stmt, i32 index, f64 value);
i32 storage_stmt_bind_str(storage_stmt *stmt, i32 index, const char *value);
i32 storage_stmt_bind_blob(storage_stmt *stmt, i32 index, const u8 *data, i32 size);
i32 storage_stmt_step(storage_stmt *stmt);
i32 storage_stmt_reset(storage_stmt *stmt);
i32 storage_stmt_clear_bindings(storage_stmt *stmt);
void storage_stmt_finalize(storage_stmt *stmt);
i32 storage_stmt_column_count(storage_stmt *stmt);
const char *storage_stmt_column_name(storage_stmt *stmt, i32 column);
i32 storage_stmt_column_type(storage_stmt *stmt, i32 column);
i64 storage_stmt_column_i64(storage_stmt *stmt, i32 column);
f64 storage_stmt_column_f64(storage_stmt *stmt, i32 column);
const char *storage_stmt_column_str(storage_stmt *stmt, i32 column);
i32 storage_stmt_column_blob_size(storage_stmt *stmt, i32 column);
i32 storage_stmt_column_blob(storage_stmt *stmt, i32 column, u8 *data, i32 capacity);

#endif
