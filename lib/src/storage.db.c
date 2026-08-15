#include "storage.internal.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct storage_db {
    sqlite3 *handle;
};

struct storage_stmt {
    sqlite3_stmt *handle;
    storage_db *db;
};

static i32 storage_db_valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (strcmp(name, ":memory:") == 0) return 1;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    return 1;
}

static void storage_db_error(storage_db *db, const char *fallback) {
    if (db && db->handle) storage_set_error(sqlite3_errmsg(db->handle));
    else storage_set_error(fallback);
}

storage_db *storage_db_open(const char *name) {
    storage_clear_error();
    if (!storage_db_valid_name(name)) {
        storage_set_error("database name must be a local filename or :memory:");
        return NULL;
    }

    char path[STORAGE_PATH_CAPACITY];
    if (strcmp(name, ":memory:") == 0) {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        const char *root = storage_app_data_dir();
        if (!root || !root[0]) return NULL;
        size_t name_len = strlen(name);
        const char *suffix = name_len >= 3 && strcmp(name + name_len - 3, ".db") == 0 ? "" :
                             name_len >= 7 && strcmp(name + name_len - 7, ".sqlite") == 0 ? "" : ".sqlite";
#ifdef NS_WIN
        i32 count = snprintf(path, sizeof(path), "%s\\%s%s", root, name, suffix);
#else
        i32 count = snprintf(path, sizeof(path), "%s/%s%s", root, name, suffix);
#endif
        if (count < 0 || (size_t)count >= sizeof(path)) {
            storage_set_error("database path is too long");
            return NULL;
        }
    }

    storage_db *db = (storage_db *)calloc(1, sizeof(storage_db));
    if (!db) {
        storage_set_error("out of memory opening database");
        return NULL;
    }
    i32 result = sqlite3_open_v2(path, &db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) {
        storage_db_error(db, "could not open database");
        if (db->handle) sqlite3_close(db->handle);
        free(db);
        return NULL;
    }
    sqlite3_busy_timeout(db->handle, 5000);
    sqlite3_extended_result_codes(db->handle, 1);
    return db;
}

void storage_db_close(storage_db *db) {
    if (!db) return;
    storage_clear_error();
    i32 result = sqlite3_close(db->handle);
    if (result != SQLITE_OK) {
        storage_db_error(db, "could not close database");
        return;
    }
    db->handle = NULL;
    free(db);
}

i32 storage_db_exec(storage_db *db, const char *sql) {
    storage_clear_error();
    if (!db || !db->handle || !sql) {
        storage_set_error("invalid database or SQL");
        return 0;
    }
    char *message = NULL;
    i32 result = sqlite3_exec(db->handle, sql, NULL, NULL, &message);
    if (result != SQLITE_OK) {
        storage_set_error(message ? message : sqlite3_errmsg(db->handle));
        sqlite3_free(message);
        return 0;
    }
    return 1;
}

storage_stmt *storage_db_prepare(storage_db *db, const char *sql) {
    storage_clear_error();
    if (!db || !db->handle || !sql) {
        storage_set_error("invalid database or SQL");
        return NULL;
    }
    storage_stmt *stmt = (storage_stmt *)calloc(1, sizeof(storage_stmt));
    if (!stmt) {
        storage_set_error("out of memory preparing statement");
        return NULL;
    }
    i32 result = sqlite3_prepare_v2(db->handle, sql, -1, &stmt->handle, NULL);
    if (result != SQLITE_OK) {
        storage_db_error(db, "could not prepare statement");
        free(stmt);
        return NULL;
    }
    stmt->db = db;
    return stmt;
}

i32 storage_db_changes(storage_db *db) {
    return db && db->handle ? sqlite3_changes(db->handle) : 0;
}

i64 storage_db_last_insert_id(storage_db *db) {
    return db && db->handle ? sqlite3_last_insert_rowid(db->handle) : 0;
}

static i32 storage_bind_result(storage_stmt *stmt, i32 result) {
    if (result == SQLITE_OK) return 1;
    storage_db_error(stmt ? stmt->db : NULL, "could not bind statement value");
    return 0;
}

i32 storage_stmt_bind_null(storage_stmt *stmt, i32 index) {
    storage_clear_error();
    return stmt && stmt->handle ? storage_bind_result(stmt, sqlite3_bind_null(stmt->handle, index)) : 0;
}

i32 storage_stmt_bind_i64(storage_stmt *stmt, i32 index, i64 value) {
    storage_clear_error();
    return stmt && stmt->handle ? storage_bind_result(stmt, sqlite3_bind_int64(stmt->handle, index, value)) : 0;
}

i32 storage_stmt_bind_f64(storage_stmt *stmt, i32 index, f64 value) {
    storage_clear_error();
    return stmt && stmt->handle ? storage_bind_result(stmt, sqlite3_bind_double(stmt->handle, index, value)) : 0;
}

i32 storage_stmt_bind_str(storage_stmt *stmt, i32 index, const char *value) {
    storage_clear_error();
    return stmt && stmt->handle && value
        ? storage_bind_result(stmt, sqlite3_bind_text(stmt->handle, index, value, -1, SQLITE_TRANSIENT)) : 0;
}

i32 storage_stmt_bind_blob(storage_stmt *stmt, i32 index, const u8 *data, i32 size) {
    storage_clear_error();
    if (!stmt || !stmt->handle || size < 0 || (size > 0 && !data)) {
        storage_set_error("invalid blob binding");
        return 0;
    }
    return storage_bind_result(stmt, sqlite3_bind_blob(stmt->handle, index, data, size, SQLITE_TRANSIENT));
}

i32 storage_stmt_step(storage_stmt *stmt) {
    storage_clear_error();
    if (!stmt || !stmt->handle) {
        storage_set_error("invalid statement");
        return STORAGE_STEP_ERROR;
    }
    i32 result = sqlite3_step(stmt->handle);
    if (result == SQLITE_ROW) return STORAGE_STEP_ROW;
    if (result == SQLITE_DONE) return STORAGE_STEP_DONE;
    storage_db_error(stmt->db, "could not step statement");
    return STORAGE_STEP_ERROR;
}

i32 storage_stmt_reset(storage_stmt *stmt) {
    storage_clear_error();
    if (!stmt || !stmt->handle) return 0;
    i32 result = sqlite3_reset(stmt->handle);
    if (result == SQLITE_OK) return 1;
    storage_db_error(stmt->db, "could not reset statement");
    return 0;
}

i32 storage_stmt_clear_bindings(storage_stmt *stmt) {
    storage_clear_error();
    if (!stmt || !stmt->handle) return 0;
    return storage_bind_result(stmt, sqlite3_clear_bindings(stmt->handle));
}

void storage_stmt_finalize(storage_stmt *stmt) {
    if (!stmt) return;
    storage_clear_error();
    i32 result = sqlite3_finalize(stmt->handle);
    if (result != SQLITE_OK) storage_db_error(stmt->db, "could not finalize statement");
    stmt->handle = NULL;
    free(stmt);
}

static i32 storage_column_valid(storage_stmt *stmt, i32 column) {
    return stmt && stmt->handle && column >= 0 && column < sqlite3_column_count(stmt->handle);
}

i32 storage_stmt_column_count(storage_stmt *stmt) {
    return stmt && stmt->handle ? sqlite3_column_count(stmt->handle) : 0;
}

const char *storage_stmt_column_name(storage_stmt *stmt, i32 column) {
    return storage_column_valid(stmt, column) ? sqlite3_column_name(stmt->handle, column) : "";
}

i32 storage_stmt_column_type(storage_stmt *stmt, i32 column) {
    if (!storage_column_valid(stmt, column)) return STORAGE_VALUE_NULL;
    switch (sqlite3_column_type(stmt->handle, column)) {
        case SQLITE_INTEGER: return STORAGE_VALUE_I64;
        case SQLITE_FLOAT: return STORAGE_VALUE_F64;
        case SQLITE_TEXT: return STORAGE_VALUE_TEXT;
        case SQLITE_BLOB: return STORAGE_VALUE_BLOB;
        default: return STORAGE_VALUE_NULL;
    }
}

i64 storage_stmt_column_i64(storage_stmt *stmt, i32 column) {
    return storage_column_valid(stmt, column) ? sqlite3_column_int64(stmt->handle, column) : 0;
}

f64 storage_stmt_column_f64(storage_stmt *stmt, i32 column) {
    return storage_column_valid(stmt, column) ? sqlite3_column_double(stmt->handle, column) : 0.0;
}

const char *storage_stmt_column_str(storage_stmt *stmt, i32 column) {
    if (!storage_column_valid(stmt, column)) return "";
    const unsigned char *text = sqlite3_column_text(stmt->handle, column);
    return text ? (const char *)text : "";
}

i32 storage_stmt_column_blob_size(storage_stmt *stmt, i32 column) {
    return storage_column_valid(stmt, column) ? sqlite3_column_bytes(stmt->handle, column) : 0;
}

i32 storage_stmt_column_blob(storage_stmt *stmt, i32 column, u8 *data, i32 capacity) {
    storage_clear_error();
    if (!storage_column_valid(stmt, column) || capacity < 0 || (capacity > 0 && !data)) {
        storage_set_error("invalid blob column read");
        return -1;
    }
    i32 size = sqlite3_column_bytes(stmt->handle, column);
    if (size > capacity) {
        storage_set_error("blob destination is too small");
        return -1;
    }
    const void *blob = sqlite3_column_blob(stmt->handle, column);
    if (size > 0 && blob) memcpy(data, blob, (size_t)size);
    return size;
}
