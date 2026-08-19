# Storage

`storage` supplies two application-local persistence layers:

- Typed KV values (`str`, `i64`, `f64`, and `bool`). macOS, iOS, and visionOS
  use `NSUserDefaults`; Windows and Linux use an atomically replaced JSON file.
- Structured data through the SQLite library shipped by the current platform.

Call `storage_init(app_name)` once at startup. On Windows and Linux this creates
the platform app-data directory and loads `storage.json`. On Apple platforms it
creates an application-support directory for databases and namespaces keys in
the app's standard user defaults. `storage_last_error()` reports the latest
diagnostic.

```ns
use storage

fn main() {
    assert storage_init("my-game")
    assert storage_kv_set_i64("high-score", 42)
    let score = storage_kv_get_i64("high-score", 0)

    let db = storage_db_open("world") // world.sqlite in app data
    assert storage_db_exec(db, "create table if not exists chunk (id integer primary key, payload blob)")
    storage_db_close(db)
}
```

KV setters persist immediately on the JSON backend. `storage_kv_sync()` can be
used at lifecycle boundaries and explicitly synchronizes `NSUserDefaults` on
Apple. Getters return their fallback when a key is missing or stores another
type. `storage_kv_clear()` removes only the namespace selected by
`storage_init`, not unrelated application defaults.

## Blob cache

`storage_cache_*` keeps binary artifacts that are expensive to produce and cheap
to verify. Entries are files below `cache/` in the app data directory rather
than KV values, because their consumers are platform APIs that read and write a
path: a Metal binary archive serializes to a URL, and an image atlas is loaded
by filename.

A key is a `name` plus a `hash` of whatever the entry was derived from. The name
says what is cached and makes the file recognizable; the hash decides whether
the entry is still valid, so changed input misses instead of returning something
stale. Writing or adopting one generation of a name retires the others, so a
rebuilt entry replaces the old one rather than accumulating beside it.

```ns
let hash = storage_cache_hash_str(shader_source)
if storage_cache_has("vs_main-fs_main", hash) {
    let size = storage_cache_size("vs_main-fs_main", hash)
    let blob = [u8](size)
    assert storage_cache_read("vs_main-fs_main", hash, blob, size) == size
}
```

`storage_cache_read` returns -1 when the entry is missing or longer than the
destination, so size it with `storage_cache_size` first rather than reading into
a guess. Writes are atomic: the bytes land in a scratch file that replaces the
entry only once it is complete, so an interrupted write cannot leave a truncated
entry that a later read would trust.

For a producer that only emits to a path, write to `storage_cache_path` and hand
the result to `storage_cache_adopt`; it moves the file onto the entry and retires
the superseded generations. `storage_cache_remove` drops every generation of one
name and `storage_cache_clear` empties the directory.

The cache is a cache: nothing in it is required to exist, and a caller that
misses must be able to produce the entry again. `gpu` relies on that when it
caches compiled Metal pipelines, and it is why a cache miss is never an error.

## SQLite statements

`storage_db_exec` is suitable for schema changes and transactions. Parameterized
queries use the prepare/bind/step/finalize lifecycle. Bind indices are 1-based;
result columns are 0-based. `storage_stmt_step` returns `STORAGE_STEP_ROW`,
`STORAGE_STEP_DONE`, or `STORAGE_STEP_ERROR`.

```ns
let stmt = storage_db_prepare(db, "insert into chunk(id, payload) values (?, ?)")
let payload = [u8](1024)
assert storage_stmt_bind_i64(stmt, 1, 7)
assert storage_stmt_bind_blob(stmt, 2, payload, 1024)
assert storage_stmt_step(stmt) == STORAGE_STEP_DONE
storage_stmt_finalize(stmt)
```

Relative database names stay inside the application-support directory and gain
a `.sqlite` suffix unless they already end in `.sqlite` or `.db`. Directory
separators and `..` are rejected. `:memory:` remains available for temporary
databases and tests. SQLite connections use serialized mode and a five-second
busy timeout.
