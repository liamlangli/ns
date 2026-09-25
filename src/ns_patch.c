#include "ns_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Everything here uses plain C strings and malloc rather than ns_str and the
// ns allocator: the client runs in hosts that embed only part of the runtime,
// on worker threads, before any program state exists.

#define NS_PATCH_PATH_MAX 1024
#define NS_PATCH_BUNDLE_MAGIC "NSBUNDLE"
#define NS_PATCH_INDEX_MAGIC "NSAPP\0\0\0"
#define NS_PATCH_BUNDLE_HEADER_SIZE 16
#define NS_PATCH_IO_CHUNK (64 * 1024)
#define NS_PATCH_HTTP_HEAD_MAX (16 * 1024)

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

static const u32 ns_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define NS_SHA256_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ns_sha256_block(ns_sha256_ctx *ctx, const u8 *p) {
    u32 w[64];
    for (i32 i = 0; i < 16; i++) {
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    }
    for (i32 i = 16; i < 64; i++) {
        u32 s0 = NS_SHA256_ROR(w[i - 15], 7) ^ NS_SHA256_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = NS_SHA256_ROR(w[i - 2], 17) ^ NS_SHA256_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u32 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (i32 i = 0; i < 64; i++) {
        u32 s1 = NS_SHA256_ROR(e, 6) ^ NS_SHA256_ROR(e, 11) ^ NS_SHA256_ROR(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + ch + ns_sha256_k[i] + w[i];
        u32 s0 = NS_SHA256_ROR(a, 2) ^ NS_SHA256_ROR(a, 13) ^ NS_SHA256_ROR(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void ns_sha256_init(ns_sha256_ctx *ctx) {
    static const u32 init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(ctx->state, init, sizeof(init));
    ctx->bytes = 0;
    ctx->used = 0;
}

void ns_sha256_update(ns_sha256_ctx *ctx, const void *data, szt size) {
    const u8 *p = (const u8 *)data;
    ctx->bytes += size;
    if (ctx->used > 0) {
        szt take = 64 - ctx->used;
        if (take > size) take = size;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += (u32)take;
        p += take;
        size -= take;
        if (ctx->used < 64) return;
        ns_sha256_block(ctx, ctx->block);
        ctx->used = 0;
    }
    while (size >= 64) {
        ns_sha256_block(ctx, p);
        p += 64;
        size -= 64;
    }
    if (size > 0) {
        memcpy(ctx->block, p, size);
        ctx->used = (u32)size;
    }
}

void ns_sha256_final(ns_sha256_ctx *ctx, u8 out[NS_PATCH_HASH_SIZE]) {
    u64 bits = ctx->bytes * 8;
    u8 pad = 0x80;
    ns_sha256_update(ctx, &pad, 1);
    u8 zero = 0;
    while (ctx->used != 56) ns_sha256_update(ctx, &zero, 1);
    u8 length[8];
    for (i32 i = 0; i < 8; i++) length[i] = (u8)(bits >> (56 - i * 8));
    ns_sha256_update(ctx, length, 8);
    for (i32 i = 0; i < 8; i++) {
        out[i * 4] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)ctx->state[i];
    }
}

void ns_sha256(const void *data, szt size, u8 out[NS_PATCH_HASH_SIZE]) {
    ns_sha256_ctx ctx;
    ns_sha256_init(&ctx);
    ns_sha256_update(&ctx, data, size);
    ns_sha256_final(&ctx, out);
}

void ns_sha256_hex(const u8 hash[NS_PATCH_HASH_SIZE], char out[NS_PATCH_HASH_SIZE * 2 + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (i32 i = 0; i < NS_PATCH_HASH_SIZE; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 15];
    }
    out[NS_PATCH_HASH_SIZE * 2] = 0;
}

// ---------------------------------------------------------------------------
// Little-endian byte buffers
// ---------------------------------------------------------------------------

typedef struct ns_patch_buf {
    u8 *data;
    szt len, cap;
    ns_bool failed;
} ns_patch_buf;

static void ns_patch_buf_put(ns_patch_buf *b, const void *data, szt size) {
    if (b->failed) return;
    if (b->len + size > b->cap) {
        szt cap = b->cap ? b->cap : 256;
        while (cap < b->len + size) cap *= 2;
        u8 *grown = (u8 *)realloc(b->data, cap);
        if (!grown) { b->failed = true; return; }
        b->data = grown;
        b->cap = cap;
    }
    if (size) memcpy(b->data + b->len, data, size);
    b->len += size;
}

static void ns_patch_buf_u16(ns_patch_buf *b, u16 v) {
    u8 p[2] = {(u8)v, (u8)(v >> 8)};
    ns_patch_buf_put(b, p, 2);
}

static void ns_patch_buf_u32(ns_patch_buf *b, u32 v) {
    u8 p[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    ns_patch_buf_put(b, p, 4);
}

static void ns_patch_buf_u64(ns_patch_buf *b, u64 v) {
    ns_patch_buf_u32(b, (u32)v);
    ns_patch_buf_u32(b, (u32)(v >> 32));
}

static void ns_patch_buf_str(ns_patch_buf *b, const char *s) {
    szt len = s ? strlen(s) : 0;
    ns_patch_buf_u16(b, (u16)len);
    ns_patch_buf_put(b, s, len);
}

static u16 ns_patch_rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 ns_patch_rd_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 ns_patch_rd_u64(const u8 *p) { return (u64)ns_patch_rd_u32(p) | ((u64)ns_patch_rd_u32(p + 4) << 32); }

typedef struct ns_patch_reader {
    const u8 *data;
    szt len, at;
    ns_bool failed;
} ns_patch_reader;

static const u8 *ns_patch_take(ns_patch_reader *r, szt size) {
    if (r->failed || r->len - r->at < size) { r->failed = true; return ns_null; }
    const u8 *p = r->data + r->at;
    r->at += size;
    return p;
}

static u32 ns_patch_read_u32(ns_patch_reader *r) { const u8 *p = ns_patch_take(r, 4); return p ? ns_patch_rd_u32(p) : 0; }
static u64 ns_patch_read_u64(ns_patch_reader *r) { const u8 *p = ns_patch_take(r, 8); return p ? ns_patch_rd_u64(p) : 0; }

static char *ns_patch_read_str(ns_patch_reader *r) {
    const u8 *p = ns_patch_take(r, 2);
    if (!p) return ns_null;
    u16 len = ns_patch_rd_u16(p);
    const u8 *s = ns_patch_take(r, len);
    if (!s) return ns_null;
    char *out = (char *)malloc((szt)len + 1);
    if (!out) { r->failed = true; return ns_null; }
    memcpy(out, s, len);
    out[len] = 0;
    // A NUL inside a name would let two different names compare equal.
    if (strlen(out) != len) { free(out); r->failed = true; return ns_null; }
    return out;
}

static char *ns_patch_strdup(const char *s) {
    szt len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (out) memcpy(out, s, len + 1);
    return out;
}

static void ns_patch_msg(char *out, szt size, const char *fmt, const char *a, const char *b) {
    if (!out || size == 0) return;
    snprintf(out, size, fmt, a ? a : "", b ? b : "");
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static ns_bool ns_patch_join(char *out, const char *a, const char *b) {
    szt la = strlen(a);
    ns_bool sep = la > 0 && a[la - 1] != '/' && a[la - 1] != '\\';
    i32 n = snprintf(out, NS_PATCH_PATH_MAX, "%s%s%s", a, sep ? "/" : "", b);
    return n > 0 && n < NS_PATCH_PATH_MAX;
}

static ns_bool ns_patch_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static i64 ns_patch_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) return -1;
    return (i64)st.st_size;
}

static void ns_patch_mkdir_one(const char *path) {
#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static ns_bool ns_patch_mkdir_p(const char *path) {
    char buf[NS_PATCH_PATH_MAX];
    szt len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, path, len + 1);
    for (szt i = 1; i < len; i++) {
        if (buf[i] != '/' && buf[i] != '\\') continue;
        char c = buf[i];
        buf[i] = 0;
        ns_patch_mkdir_one(buf);
        buf[i] = c;
    }
    ns_patch_mkdir_one(buf);
    return ns_patch_is_dir(buf);
}

static ns_bool ns_patch_mkdir_parent(const char *file) {
    char buf[NS_PATCH_PATH_MAX];
    szt len = strlen(file);
    if (len >= sizeof(buf)) return false;
    memcpy(buf, file, len + 1);
    char *slash = strrchr(buf, '/');
    char *back = strrchr(buf, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (!slash || slash == buf) return true;
    *slash = 0;
    return ns_patch_mkdir_p(buf);
}

typedef void (*ns_patch_dir_fn)(void *user, const char *name, ns_bool is_dir);

static void ns_patch_dir_each(const char *path, ns_patch_dir_fn fn, void *user) {
#if defined(_WIN32)
    char pattern[NS_PATCH_PATH_MAX];
    if (!ns_patch_join(pattern, path, "*")) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        fn(user, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != ns_null) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[NS_PATCH_PATH_MAX];
        if (!ns_patch_join(child, path, entry->d_name)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        fn(user, entry->d_name, S_ISDIR(st.st_mode));
    }
    closedir(dir);
#endif
}

static void ns_patch_remove_tree(const char *path);

static void ns_patch_remove_child(void *user, const char *name, ns_bool is_dir) {
    (void)is_dir;
    char child[NS_PATCH_PATH_MAX];
    if (ns_patch_join(child, (const char *)user, name)) ns_patch_remove_tree(child);
}

static void ns_patch_remove_tree(const char *path) {
    struct stat st;
#if defined(_WIN32)
    if (stat(path, &st) != 0) return;
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        ns_patch_dir_each(path, ns_patch_remove_child, (void *)path);
        RemoveDirectoryA(path);
    } else {
        DeleteFileA(path);
    }
#else
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        ns_patch_dir_each(path, ns_patch_remove_child, (void *)path);
        rmdir(path);
    } else {
        unlink(path);
    }
#endif
}

// Replace `to` with `from`. Directories are moved aside first on Windows,
// whose rename never replaces an existing target.
static ns_bool ns_patch_rename(const char *from, const char *to) {
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}

static u8 *ns_patch_read_all(const char *path, szt *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return ns_null;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ns_null; }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ns_null; }
    u8 *data = (u8 *)malloc(end > 0 ? (szt)end : 1);
    if (!data) { fclose(f); return ns_null; }
    szt got = end > 0 ? fread(data, 1, (szt)end, f) : 0;
    fclose(f);
    if (got != (szt)end) { free(data); return ns_null; }
    *size = got;
    return data;
}

static ns_bool ns_patch_write_all(const char *path, const void *data, szt size) {
    if (!ns_patch_mkdir_parent(path)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    ns_bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

// Write through a sibling temporary file so a reader never sees half of it.
static ns_bool ns_patch_write_atomic(const char *path, const void *data, szt size) {
    char tmp[NS_PATCH_PATH_MAX];
#if defined(_WIN32)
    i32 pid = (i32)_getpid();
#else
    i32 pid = (i32)getpid();
#endif
    if (snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, pid) >= (i32)sizeof(tmp)) return false;
    if (!ns_patch_write_all(tmp, data, size)) return false;
    if (!ns_patch_rename(tmp, path)) { remove(tmp); return false; }
    return true;
}

static ns_bool ns_patch_hash_file(const char *path, u8 out[NS_PATCH_HASH_SIZE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    ns_sha256_ctx ctx;
    ns_sha256_init(&ctx);
    u8 *buf = (u8 *)malloc(NS_PATCH_IO_CHUNK);
    if (!buf) { fclose(f); return false; }
    szt n;
    while ((n = fread(buf, 1, NS_PATCH_IO_CHUNK, f)) > 0) ns_sha256_update(&ctx, buf, n);
    ns_bool ok = !ferror(f);
    fclose(f);
    free(buf);
    ns_sha256_final(&ctx, out);
    return ok;
}

// Copy `from` to `to`; a hard link when both sit on one volume, which is the
// case for files an update keeps from the previous snapshot.
static ns_bool ns_patch_copy_file(const char *from, const char *to) {
    if (!ns_patch_mkdir_parent(to)) return false;
    remove(to);
#if !defined(_WIN32)
    if (link(from, to) == 0) return true;
#endif
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    u8 *buf = (u8 *)malloc(NS_PATCH_IO_CHUNK);
    ns_bool ok = buf != ns_null;
    szt n;
    while (ok && (n = fread(buf, 1, NS_PATCH_IO_CHUNK, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) ok = false;
    }
    if (ferror(in)) ok = false;
    free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(to);
    return ok;
}

// A path an index may name: relative, `/`-separated, no `.` or `..` part, no
// drive or backslash. Everything written below a snapshot goes through this.
static ns_bool ns_patch_path_safe(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strlen(path) > 512) return false;
    const char *part = path;
    for (const char *p = path;; p++) {
        if (*p == '\\' || *p == ':' || (*p > 0 && *p < 0x20)) return false;
        if (*p == '/' || *p == 0) {
            szt len = (szt)(p - part);
            if (len == 0) return false;
            if (len == 1 && part[0] == '.') return false;
            if (len == 2 && part[0] == '.' && part[1] == '.') return false;
            if (*p == 0) return true;
            part = p + 1;
        }
    }
}

// A bundle file name travels in a URL and names a local file.
static ns_bool ns_patch_bundle_name_safe(const char *name) {
    if (!name || !name[0] || strlen(name) > 128 || name[0] == '.') return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The index
// ---------------------------------------------------------------------------

ns_bool ns_patch_header_decode(const u8 *data, szt size, ns_patch_header *out) {
    memset(out, 0, sizeof(*out));
    if (!data || size < NS_PATCH_HEADER_SIZE) return false;
    if (memcmp(data, NS_PATCH_INDEX_MAGIC, 8) != 0) return false;
    out->format = ns_patch_rd_u16(data + 8);
    if (out->format != NS_PATCH_FORMAT) return false;
    out->mode = data[10];
    out->version = ns_patch_rd_u32(data + 12);
    out->body_size = ns_patch_rd_u32(data + 16);
    out->bundle_count = ns_patch_rd_u32(data + 20);
    out->file_count = ns_patch_rd_u32(data + 24);
    memcpy(out->body_hash, data + 32, NS_PATCH_HASH_SIZE);
    return out->mode == NS_PATCH_MODE_EVAL || out->mode == NS_PATCH_MODE_EMU;
}

void ns_patch_index_free(ns_patch_index *index) {
    if (!index) return;
    free(index->name);
    free(index->app_version);
    free(index->code);
    for (u32 i = 0; index->bundles && i < index->header.bundle_count; i++) free(index->bundles[i].file);
    for (u32 i = 0; index->files && i < index->header.file_count; i++) free(index->files[i].path);
    free(index->bundles);
    free(index->files);
    memset(index, 0, sizeof(*index));
}

ns_bool ns_patch_index_decode(const u8 *data, szt size, ns_patch_index *out, char *error, szt error_size) {
    memset(out, 0, sizeof(*out));
    ns_patch_header h;
    if (!ns_patch_header_decode(data, size, &h)) {
        ns_patch_msg(error, error_size, "not a format %s .nsapp index%s", "1", ns_null);
        return false;
    }
    if (size - NS_PATCH_HEADER_SIZE < h.body_size) {
        ns_patch_msg(error, error_size, "truncated index%s%s", ns_null, ns_null);
        return false;
    }
    u8 digest[NS_PATCH_HASH_SIZE];
    ns_sha256(data + NS_PATCH_HEADER_SIZE, h.body_size, digest);
    if (memcmp(digest, h.body_hash, NS_PATCH_HASH_SIZE) != 0) {
        ns_patch_msg(error, error_size, "index digest mismatch%s%s", ns_null, ns_null);
        return false;
    }
    // Every bundle and file entry takes at least this many bytes, which bounds
    // the counts before anything is allocated for them.
    if (h.bundle_count == 0 || h.file_count == 0 || h.bundle_count > h.body_size / 50 || h.file_count > h.body_size / 42) {
        ns_patch_msg(error, error_size, "index counts out of range%s%s", ns_null, ns_null);
        return false;
    }
    out->header = h;
    out->bundles = (ns_patch_bundle *)calloc(h.bundle_count, sizeof(ns_patch_bundle));
    out->files = (ns_patch_file *)calloc(h.file_count, sizeof(ns_patch_file));
    ns_patch_reader r = {.data = data + NS_PATCH_HEADER_SIZE, .len = h.body_size};
    if (!out->bundles || !out->files) r.failed = true;
    out->name = ns_patch_read_str(&r);
    out->app_version = ns_patch_read_str(&r);
    out->code = ns_patch_read_str(&r);
    out->created = ns_patch_read_u64(&r);
    for (u32 i = 0; !r.failed && i < h.bundle_count; i++) {
        ns_patch_bundle *b = &out->bundles[i];
        b->file = ns_patch_read_str(&r);
        b->size = ns_patch_read_u64(&r);
        const u8 *hash = ns_patch_take(&r, NS_PATCH_HASH_SIZE);
        if (hash) memcpy(b->hash, hash, NS_PATCH_HASH_SIZE);
        b->first_file = ns_patch_read_u32(&r);
        b->file_count = ns_patch_read_u32(&r);
    }
    for (u32 i = 0; !r.failed && i < h.file_count; i++) {
        ns_patch_file *f = &out->files[i];
        f->path = ns_patch_read_str(&r);
        f->size = ns_patch_read_u64(&r);
        const u8 *hash = ns_patch_take(&r, NS_PATCH_HASH_SIZE);
        if (hash) memcpy(f->hash, hash, NS_PATCH_HASH_SIZE);
    }
    const char *why = ns_null;
    if (r.failed || r.at != r.len) why = "malformed index body";
    // The bundles partition the files in order, every name is safe, and the
    // program file is one of them.
    u32 next = 0;
    for (u32 i = 0; !why && i < h.bundle_count; i++) {
        ns_patch_bundle *b = &out->bundles[i];
        if (!ns_patch_bundle_name_safe(b->file)) why = "unsafe bundle name";
        else if (b->first_file != next || b->file_count == 0 || b->file_count > h.file_count - next) why = "bundles do not cover the files";
        else next += b->file_count;
    }
    if (!why && next != h.file_count) why = "bundles do not cover the files";
    ns_bool has_code = false;
    for (u32 i = 0; !why && i < h.file_count; i++) {
        if (!ns_patch_path_safe(out->files[i].path)) why = "unsafe file path";
        else if (out->code && strcmp(out->files[i].path, out->code) == 0) has_code = true;
    }
    if (!why && (!out->name || !has_code)) why = "index names no program file";
    if (why) {
        ns_patch_msg(error, error_size, "%s%s", why, ns_null);
        ns_patch_index_free(out);
        return false;
    }
    return true;
}

static const ns_patch_file *ns_patch_index_find(const ns_patch_index *index, const char *path) {
    for (u32 i = 0; i < index->header.file_count; i++) {
        if (strcmp(index->files[i].path, path) == 0) return &index->files[i];
    }
    return ns_null;
}

static void ns_patch_index_encode(ns_patch_buf *out, const ns_patch_index *index) {
    ns_patch_buf body = {0};
    ns_patch_buf_str(&body, index->name);
    ns_patch_buf_str(&body, index->app_version);
    ns_patch_buf_str(&body, index->code);
    ns_patch_buf_u64(&body, index->created);
    for (u32 i = 0; i < index->header.bundle_count; i++) {
        const ns_patch_bundle *b = &index->bundles[i];
        ns_patch_buf_str(&body, b->file);
        ns_patch_buf_u64(&body, b->size);
        ns_patch_buf_put(&body, b->hash, NS_PATCH_HASH_SIZE);
        ns_patch_buf_u32(&body, b->first_file);
        ns_patch_buf_u32(&body, b->file_count);
    }
    for (u32 i = 0; i < index->header.file_count; i++) {
        const ns_patch_file *f = &index->files[i];
        ns_patch_buf_str(&body, f->path);
        ns_patch_buf_u64(&body, f->size);
        ns_patch_buf_put(&body, f->hash, NS_PATCH_HASH_SIZE);
    }
    u8 digest[NS_PATCH_HASH_SIZE];
    ns_sha256(body.data, body.len, digest);
    ns_patch_buf_put(out, NS_PATCH_INDEX_MAGIC, 8);
    ns_patch_buf_u16(out, NS_PATCH_FORMAT);
    u8 mode_flags[2] = {index->header.mode, 0};
    ns_patch_buf_put(out, mode_flags, 2);
    ns_patch_buf_u32(out, index->header.version);
    ns_patch_buf_u32(out, (u32)body.len);
    ns_patch_buf_u32(out, index->header.bundle_count);
    ns_patch_buf_u32(out, index->header.file_count);
    ns_patch_buf_u32(out, 0);
    ns_patch_buf_put(out, digest, NS_PATCH_HASH_SIZE);
    ns_patch_buf_put(out, body.data, body.len);
    if (body.failed) out->failed = true;
    free(body.data);
}

// ---------------------------------------------------------------------------
// Writing a patch
// ---------------------------------------------------------------------------

typedef struct ns_patch_src {
    char *path;          // relative path in the snapshot
    char *disk;          // where the bytes are, NULL for the in-memory program
} ns_patch_src;

typedef struct ns_patch_scan {
    ns_patch_src *items;
    u32 count, cap;
    const char *disk_dir;
    const char *rel_dir;
} ns_patch_scan;

static void ns_patch_scan_add(ns_patch_scan *s, const char *rel, const char *disk) {
    if (s->count == s->cap) {
        u32 cap = s->cap ? s->cap * 2 : 64;
        ns_patch_src *grown = (ns_patch_src *)realloc(s->items, cap * sizeof(ns_patch_src));
        if (!grown) return;
        s->items = grown;
        s->cap = cap;
    }
    s->items[s->count].path = ns_patch_strdup(rel);
    s->items[s->count].disk = disk ? ns_patch_strdup(disk) : ns_null;
    s->count++;
}

static void ns_patch_scan_path(ns_patch_scan *s, const char *disk, const char *rel);

static void ns_patch_scan_child(void *user, const char *name, ns_bool is_dir) {
    (void)is_dir;
    ns_patch_scan *s = (ns_patch_scan *)user;
    // Finder and Explorer metadata never belong to a program.
    if (strcmp(name, ".DS_Store") == 0 || strcmp(name, "Thumbs.db") == 0) return;
    char disk[NS_PATCH_PATH_MAX], rel[NS_PATCH_PATH_MAX];
    if (!ns_patch_join(disk, s->disk_dir, name) || !ns_patch_join(rel, s->rel_dir, name)) return;
    const char *saved_disk = s->disk_dir, *saved_rel = s->rel_dir;
    ns_patch_scan_path(s, disk, rel);
    s->disk_dir = saved_disk;
    s->rel_dir = saved_rel;
}

static void ns_patch_scan_path(ns_patch_scan *s, const char *disk, const char *rel) {
    if (ns_patch_is_dir(disk)) {
        s->disk_dir = disk;
        s->rel_dir = rel;
        ns_patch_dir_each(disk, ns_patch_scan_child, s);
    } else if (ns_patch_file_size(disk) >= 0) {
        ns_patch_scan_add(s, rel, disk);
    }
}

static int ns_patch_src_cmp(const void *a, const void *b) {
    return strcmp(((const ns_patch_src *)a)->path, ((const ns_patch_src *)b)->path);
}

static void ns_patch_scan_free(ns_patch_scan *s) {
    for (u32 i = 0; i < s->count; i++) {
        free(s->items[i].path);
        free(s->items[i].disk);
    }
    free(s->items);
}

// Pack files [first, first + count) into one bundle and write it to `out_dir`
// under its content hash.
static ns_bool ns_patch_write_bundle(const ns_patch_input *in, const ns_patch_src *src, ns_patch_file *files,
                                     u32 first, u32 count, ns_patch_bundle *bundle, char *error, szt error_size) {
    ns_patch_buf b = {0};
    ns_patch_buf_put(&b, NS_PATCH_BUNDLE_MAGIC, 8);
    ns_patch_buf_u16(&b, NS_PATCH_FORMAT);
    ns_patch_buf_u16(&b, 0);
    ns_patch_buf_u32(&b, count);
    for (u32 i = first; i < first + count; i++) {
        ns_patch_buf_str(&b, files[i].path);
        ns_patch_buf_u64(&b, files[i].size);
    }
    for (u32 i = first; i < first + count; i++) {
        if (!src[i].disk) {
            ns_patch_buf_put(&b, in->code, in->code_size);
            continue;
        }
        szt size = 0;
        u8 *data = ns_patch_read_all(src[i].disk, &size);
        if (!data || size != files[i].size) {
            free(data);
            free(b.data);
            ns_patch_msg(error, error_size, "failed to read %s%s", src[i].disk, ns_null);
            return false;
        }
        ns_patch_buf_put(&b, data, size);
        free(data);
    }
    if (b.failed) {
        free(b.data);
        ns_patch_msg(error, error_size, "out of memory packing a bundle%s%s", ns_null, ns_null);
        return false;
    }
    ns_sha256(b.data, b.len, bundle->hash);
    char hex[NS_PATCH_HASH_SIZE * 2 + 1];
    ns_sha256_hex(bundle->hash, hex);
    char name[64];
    snprintf(name, sizeof(name), "%.16s.nsbundle", hex);
    bundle->file = ns_patch_strdup(name);
    bundle->size = b.len;
    bundle->first_file = first;
    bundle->file_count = count;
    char path[NS_PATCH_PATH_MAX];
    ns_bool ok = ns_patch_join(path, in->out_dir, name);
    // Content addressed: an existing bundle of that name already holds these bytes.
    if (ok && ns_patch_file_size(path) != (i64)b.len) ok = ns_patch_write_atomic(path, b.data, b.len);
    free(b.data);
    if (!ok) ns_patch_msg(error, error_size, "failed to write %s%s", path, ns_null);
    return ok;
}

typedef struct ns_patch_sweep {
    const char *dir;
    const ns_patch_index *keep;
} ns_patch_sweep;

static void ns_patch_sweep_bundle(void *user, const char *name, ns_bool is_dir) {
    ns_patch_sweep *s = (ns_patch_sweep *)user;
    szt len = strlen(name);
    if (is_dir || len < 9 || strcmp(name + len - 9, ".nsbundle") != 0) return;
    for (u32 i = 0; i < s->keep->header.bundle_count; i++) {
        if (strcmp(s->keep->bundles[i].file, name) == 0) return;
    }
    char path[NS_PATCH_PATH_MAX];
    if (ns_patch_join(path, s->dir, name)) remove(path);
}

ns_bool ns_patch_write(const ns_patch_input *in, ns_patch_summary *out) {
    memset(out, 0, sizeof(*out));
    u64 chunk = in->chunk_size ? in->chunk_size : NS_PATCH_CHUNK_SIZE;
    const char *code_name = in->mode == NS_PATCH_MODE_EMU ? NS_PATCH_CODE_EMU : NS_PATCH_CODE_EVAL;
    if (!ns_patch_mkdir_p(in->out_dir)) {
        ns_patch_msg(out->error, sizeof(out->error), "failed to create %s%s", in->out_dir, ns_null);
        return false;
    }

    // The program first, alone in the first bundle: it changes with almost
    // every patch, and keeping it apart leaves the asset bundles untouched.
    ns_patch_scan scan = {0};
    ns_patch_scan_add(&scan, code_name, ns_null);
    for (i32 i = 0; i < in->asset_count; i++) {
        char disk[NS_PATCH_PATH_MAX];
        if (!ns_patch_path_safe(in->assets[i]) || !ns_patch_join(disk, in->root, in->assets[i])) continue;
        ns_patch_scan_path(&scan, disk, in->assets[i]);
    }
    if (scan.count > 1) qsort(scan.items + 1, scan.count - 1, sizeof(ns_patch_src), ns_patch_src_cmp);
    // An asset listed twice, or a directory and a file inside it, packs once.
    u32 unique = 1;
    for (u32 i = 1; i < scan.count; i++) {
        if (strcmp(scan.items[i].path, code_name) == 0 ||
            (unique > 1 && strcmp(scan.items[i].path, scan.items[unique - 1].path) == 0)) {
            free(scan.items[i].path);
            free(scan.items[i].disk);
            continue;
        }
        scan.items[unique++] = scan.items[i];
    }
    scan.count = unique;

    ns_patch_index index = {0};
    index.name = ns_patch_strdup(in->name);
    index.app_version = ns_patch_strdup(in->app_version ? in->app_version : "");
    index.code = ns_patch_strdup(code_name);
    index.created = (u64)time(ns_null);
    index.header.format = NS_PATCH_FORMAT;
    index.header.mode = (u8)in->mode;
    index.header.file_count = scan.count;
    index.files = (ns_patch_file *)calloc(scan.count, sizeof(ns_patch_file));
    index.bundles = (ns_patch_bundle *)calloc(scan.count, sizeof(ns_patch_bundle));
    ns_bool ok = index.files && index.bundles;
    for (u32 i = 0; ok && i < scan.count; i++) {
        ns_patch_file *f = &index.files[i];
        f->path = ns_patch_strdup(scan.items[i].path);
        if (!scan.items[i].disk) {
            f->size = in->code_size;
            ns_sha256(in->code, in->code_size, f->hash);
        } else {
            i64 size = ns_patch_file_size(scan.items[i].disk);
            if (size < 0 || !ns_patch_hash_file(scan.items[i].disk, f->hash)) {
                ns_patch_msg(out->error, sizeof(out->error), "failed to read %s%s", scan.items[i].disk, ns_null);
                ok = false;
            }
            f->size = (u64)size;
        }
    }

    // Cut the files into bundles in path order, a new one whenever the next
    // file would take the current one past the chunk size.
    u32 bundles = 0;
    for (u32 first = 0; ok && first < scan.count;) {
        u32 count = 1;
        u64 bytes = index.files[first].size;
        if (first > 0) {
            while (first + count < scan.count && bytes + index.files[first + count].size <= chunk) {
                bytes += index.files[first + count].size;
                count++;
            }
        }
        ok = ns_patch_write_bundle(in, scan.items, index.files, first, count, &index.bundles[bundles],
                                   out->error, sizeof(out->error));
        if (ok) out->bytes += index.bundles[bundles].size;
        bundles++;
        first += count;
    }
    index.header.bundle_count = bundles;

    char index_path[NS_PATCH_PATH_MAX], index_name[NS_PATCH_PATH_MAX];
    snprintf(index_name, sizeof(index_name), "%s.nsapp", in->name);
    if (ok && !ns_patch_join(index_path, in->out_dir, index_name)) ok = false;

    // The version: pinned by the caller, or one past the index already here.
    // An identical patch keeps its version, so clients see nothing new.
    ns_patch_index previous = {0};
    ns_bool has_previous = false;
    if (ok) {
        szt size = 0;
        u8 *data = ns_patch_read_all(index_path, &size);
        has_previous = data && ns_patch_index_decode(data, size, &previous, ns_null, 0);
        free(data);
    }
    ns_bool same = has_previous && previous.header.mode == index.header.mode &&
                   previous.header.bundle_count == index.header.bundle_count &&
                   strcmp(previous.code, index.code) == 0;
    for (u32 i = 0; same && i < bundles; i++) {
        same = memcmp(previous.bundles[i].hash, index.bundles[i].hash, NS_PATCH_HASH_SIZE) == 0;
    }
    out->previous = has_previous ? previous.header.version : 0;
    if (in->version) index.header.version = in->version;
    else index.header.version = same ? out->previous : out->previous + 1;
    out->unchanged = same && index.header.version == out->previous;

    if (ok && !out->unchanged) {
        ns_patch_buf encoded = {0};
        ns_patch_index_encode(&encoded, &index);
        ok = !encoded.failed && ns_patch_write_atomic(index_path, encoded.data, encoded.len);
        if (!ok) ns_patch_msg(out->error, sizeof(out->error), "failed to write %s%s", index_path, ns_null);
        free(encoded.data);
    }
    if (ok) {
        ns_patch_sweep sweep = {.dir = in->out_dir, .keep = &index};
        ns_patch_dir_each(in->out_dir, ns_patch_sweep_bundle, &sweep);
        out->version = index.header.version;
        out->bundle_count = bundles;
        out->file_count = scan.count;
        snprintf(out->index_path, sizeof(out->index_path), "%s", index_path);
    }
    ns_patch_index_free(&previous);
    ns_patch_index_free(&index);
    ns_patch_scan_free(&scan);
    return ok;
}

// ---------------------------------------------------------------------------
// HTTP/1.1 client
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef SOCKET ns_patch_socket;
#define NS_PATCH_BAD_SOCKET INVALID_SOCKET
#define ns_patch_closesocket closesocket
#else
typedef i32 ns_patch_socket;
#define NS_PATCH_BAD_SOCKET (-1)
#define ns_patch_closesocket close
#endif

typedef struct ns_patch_url {
    char host[256];
    char port[8];
    char path[NS_PATCH_PATH_MAX]; // starts with '/'
    char dir[NS_PATCH_PATH_MAX];  // `path` up to and including its last '/'
} ns_patch_url;

static ns_bool ns_patch_url_parse(const char *url, ns_patch_url *out, char *error, szt error_size) {
    memset(out, 0, sizeof(*out));
    if (!url || strncmp(url, "http://", 7) != 0) {
        ns_patch_msg(error, error_size, "only http:// patch URLs are supported: %s%s", url, ns_null);
        return false;
    }
    const char *host = url + 7;
    const char *slash = strchr(host, '/');
    const char *end = slash ? slash : host + strlen(host);
    const char *colon = ns_null;
    if (*host == '[') {
        const char *close = memchr(host, ']', (szt)(end - host));
        if (!close) return false;
        if ((szt)(close - host - 1) >= sizeof(out->host)) return false;
        memcpy(out->host, host + 1, (szt)(close - host - 1));
        if (close + 1 < end && close[1] == ':') colon = close + 1;
    } else {
        colon = memchr(host, ':', (szt)(end - host));
        const char *host_end = colon ? colon : end;
        if (host_end == host || (szt)(host_end - host) >= sizeof(out->host)) return false;
        memcpy(out->host, host, (szt)(host_end - host));
    }
    if (colon) {
        szt len = (szt)(end - colon - 1);
        if (len == 0 || len >= sizeof(out->port)) return false;
        memcpy(out->port, colon + 1, len);
    } else {
        strcpy(out->port, "80");
    }
    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    snprintf(out->dir, sizeof(out->dir), "%s", out->path);
    char *last = strrchr(out->dir, '/');
    if (last) last[1] = 0;
    return out->host[0] != 0;
}

static void ns_patch_net_init(void) {
#if defined(_WIN32)
    static ns_bool started = false;
    if (!started) {
        WSADATA wsa;
        started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
#endif
}

static ns_patch_socket ns_patch_connect(const ns_patch_url *u, i32 timeout_ms) {
    struct addrinfo hints, *list = ns_null;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u->host, u->port, &hints, &list) != 0) return NS_PATCH_BAD_SOCKET;
    ns_patch_socket s = NS_PATCH_BAD_SOCKET;
    for (struct addrinfo *a = list; a && s == NS_PATCH_BAD_SOCKET; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == NS_PATCH_BAD_SOCKET) continue;
        // Connect without blocking so an unreachable server costs the timeout,
        // not the system's minute-long default.
#if defined(_WIN32)
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
#else
        i32 flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
        i32 r = connect(s, a->ai_addr, (i32)a->ai_addrlen);
        ns_bool connected = r == 0;
        if (!connected) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select((i32)s + 1, ns_null, &w, ns_null, &tv) == 1) {
                i32 err = 0;
                socklen_t len = sizeof(err);
                connected = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0;
            }
        }
        if (!connected) {
            ns_patch_closesocket(s);
            s = NS_PATCH_BAD_SOCKET;
            continue;
        }
#if defined(_WIN32)
        u_long off = 0;
        ioctlsocket(s, FIONBIO, &off);
        DWORD tv = (DWORD)timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
        fcntl(s, F_SETFL, flags);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
#if defined(SO_NOSIGPIPE)
        i32 one = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    freeaddrinfo(list);
    return s;
}

// Receives the body. Returns false to stop reading early.
typedef ns_bool (*ns_patch_sink)(void *user, const u8 *data, szt size);

typedef struct ns_patch_conn {
    ns_patch_socket s;
    u8 buf[NS_PATCH_IO_CHUNK];
    szt at, len;
    ns_bool closed;
} ns_patch_conn;

static ns_bool ns_patch_conn_fill(ns_patch_conn *c) {
    if (c->at < c->len) return true;
    if (c->closed) return false;
#if defined(MSG_NOSIGNAL)
    i32 flags = MSG_NOSIGNAL;
#else
    i32 flags = 0;
#endif
    i32 n = (i32)recv(c->s, (char *)c->buf, sizeof(c->buf), flags);
    if (n <= 0) { c->closed = true; return false; }
    c->at = 0;
    c->len = (szt)n;
    return true;
}

// One CRLF-terminated line, without the terminator.
static ns_bool ns_patch_conn_line(ns_patch_conn *c, char *out, szt size) {
    szt n = 0;
    for (;;) {
        if (!ns_patch_conn_fill(c)) return false;
        char ch = (char)c->buf[c->at++];
        if (ch == '\n') break;
        if (n + 1 >= size) return false;
        out[n++] = ch;
    }
    if (n > 0 && out[n - 1] == '\r') n--;
    out[n] = 0;
    return true;
}

static ns_bool ns_patch_conn_body(ns_patch_conn *c, u64 size, ns_bool until_close, ns_patch_sink sink, void *user,
                                  ns_bool *stopped) {
    while (until_close || size > 0) {
        if (!ns_patch_conn_fill(c)) return until_close;
        szt n = c->len - c->at;
        if (!until_close && n > size) n = (szt)size;
        if (!sink(user, c->buf + c->at, n)) { *stopped = true; return true; }
        c->at += n;
        if (!until_close) size -= n;
    }
    return true;
}

static ns_bool ns_patch_ieq_prefix(const char *line, const char *name) {
    szt n = strlen(name);
    for (szt i = 0; i < n; i++) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// GET `path` from the server `u` names and stream the body into `sink`. A
// non-negative `range_end` asks for bytes [0, range_end] only; a server that
// ignores the Range header answers with the whole body, and the sink stops it.
static ns_bool ns_patch_http_get(const ns_patch_url *u, const char *path, i64 range_end, i32 timeout_ms,
                                 ns_patch_sink sink, void *user, char *error, szt error_size) {
    ns_patch_net_init();
    ns_patch_socket s = ns_patch_connect(u, timeout_ms);
    if (s == NS_PATCH_BAD_SOCKET) {
        ns_patch_msg(error, error_size, "cannot reach %s:%s", u->host, u->port);
        return false;
    }
    char request[2048];
    char range[64] = "";
    if (range_end >= 0) snprintf(range, sizeof(range), "Range: bytes=0-%lld\r\n", (long long)range_end);
    i32 len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.1\r\nHost: %s\r\n%sUser-Agent: ns-patch/1\r\nAccept-Encoding: identity\r\n"
                       "Connection: close\r\n\r\n",
                       path, u->host, range);
    ns_bool ok = len > 0 && len < (i32)sizeof(request);
    for (i32 sent = 0; ok && sent < len;) {
#if defined(MSG_NOSIGNAL)
        i32 n = (i32)send(s, request + sent, (szt)(len - sent), MSG_NOSIGNAL);
#else
        i32 n = (i32)send(s, request + sent, len - sent, 0);
#endif
        if (n <= 0) ok = false;
        else sent += n;
    }
    ns_patch_conn *c = ok ? (ns_patch_conn *)calloc(1, sizeof(ns_patch_conn)) : ns_null;
    if (!c) {
        ns_patch_closesocket(s);
        ns_patch_msg(error, error_size, "request to %s failed%s", u->host, ns_null);
        return false;
    }
    c->s = s;
    char line[1024];
    i32 status = 0;
    ok = ns_patch_conn_line(c, line, sizeof(line)) && sscanf(line, "HTTP/%*d.%*d %d", &status) == 1;
    i64 content_length = -1;
    ns_bool chunked = false;
    szt head = 0;
    while (ok) {
        if (!ns_patch_conn_line(c, line, sizeof(line))) { ok = false; break; }
        head += strlen(line);
        if (head > NS_PATCH_HTTP_HEAD_MAX) { ok = false; break; }
        if (line[0] == 0) break;
        if (ns_patch_ieq_prefix(line, "content-length:")) content_length = strtoll(line + 15, ns_null, 10);
        if (ns_patch_ieq_prefix(line, "transfer-encoding:") && strstr(line, "chunked")) chunked = true;
    }
    if (ok && status != 200 && status != 206) {
        char code[16];
        snprintf(code, sizeof(code), "%d", status);
        ns_patch_msg(error, error_size, "HTTP %s for %s", code, path);
        ok = false;
    } else if (!ok) {
        ns_patch_msg(error, error_size, "malformed response for %s%s", path, ns_null);
    }
    ns_bool stopped = false;
    if (ok && chunked) {
        while (ok && !stopped) {
            if (!ns_patch_conn_line(c, line, sizeof(line))) { ok = false; break; }
            u64 size = strtoull(line, ns_null, 16);
            if (size == 0) break;
            ok = ns_patch_conn_body(c, size, false, sink, user, &stopped) && (stopped || ns_patch_conn_line(c, line, sizeof(line)));
        }
    } else if (ok) {
        ok = ns_patch_conn_body(c, content_length >= 0 ? (u64)content_length : 0, content_length < 0, sink, user, &stopped);
    }
    if (!ok && error && error[0] == 0) ns_patch_msg(error, error_size, "connection dropped reading %s%s", path, ns_null);
    ns_patch_closesocket(s);
    free(c);
    return ok;
}

typedef struct ns_patch_mem_sink {
    ns_patch_buf buf;
    szt limit;
} ns_patch_mem_sink;

static ns_bool ns_patch_mem_write(void *user, const u8 *data, szt size) {
    ns_patch_mem_sink *m = (ns_patch_mem_sink *)user;
    szt room = m->limit - m->buf.len;
    ns_patch_buf_put(&m->buf, data, size < room ? size : room);
    return !m->buf.failed && m->buf.len < m->limit;
}

typedef struct ns_patch_file_sink {
    FILE *f;
    ns_sha256_ctx sha;
    u64 bytes, limit;
    ns_bool failed;
} ns_patch_file_sink;

static ns_bool ns_patch_file_write(void *user, const u8 *data, szt size) {
    ns_patch_file_sink *s = (ns_patch_file_sink *)user;
    if (s->bytes + size > s->limit) { s->failed = true; return false; }
    if (fwrite(data, 1, size, s->f) != size) { s->failed = true; return false; }
    ns_sha256_update(&s->sha, data, size);
    s->bytes += size;
    return true;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef CRITICAL_SECTION ns_patch_mutex;
static void ns_patch_mutex_init(ns_patch_mutex *m) { InitializeCriticalSection(m); }
static void ns_patch_mutex_lock(ns_patch_mutex *m) { EnterCriticalSection(m); }
static void ns_patch_mutex_unlock(ns_patch_mutex *m) { LeaveCriticalSection(m); }
static void ns_patch_mutex_free(ns_patch_mutex *m) { DeleteCriticalSection(m); }
#else
typedef pthread_mutex_t ns_patch_mutex;
static void ns_patch_mutex_init(ns_patch_mutex *m) { pthread_mutex_init(m, ns_null); }
static void ns_patch_mutex_lock(ns_patch_mutex *m) { pthread_mutex_lock(m); }
static void ns_patch_mutex_unlock(ns_patch_mutex *m) { pthread_mutex_unlock(m); }
static void ns_patch_mutex_free(ns_patch_mutex *m) { pthread_mutex_destroy(m); }
#endif

// ---------------------------------------------------------------------------
// Applying a patch
// ---------------------------------------------------------------------------

typedef struct ns_patch_job {
    const ns_patch_config *cfg;
    const ns_patch_url *url;
    const ns_patch_index *index;
    const char *stage;     // snapshot being assembled
    const char *scratch;   // downloaded bundles
    i32 timeout_ms;
    u32 *pending;          // bundle indices to fetch
    u32 pending_count;
    u32 next;
    ns_bool failed;
    char error[256];
    ns_patch_mutex lock;
} ns_patch_job;

static void ns_patch_job_fail(ns_patch_job *job, const char *error) {
    ns_patch_mutex_lock(&job->lock);
    if (!job->failed) snprintf(job->error, sizeof(job->error), "%s", error);
    job->failed = true;
    ns_patch_mutex_unlock(&job->lock);
}

// Unpack a verified bundle into the stage, checking each file against the
// size and digest the index lists for it.
static ns_bool ns_patch_extract(const ns_patch_job *job, const ns_patch_bundle *b, const char *path, char *error, szt error_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { ns_patch_msg(error, error_size, "cannot open %s%s", path, ns_null); return false; }
    u8 head[NS_PATCH_BUNDLE_HEADER_SIZE];
    ns_bool ok = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, NS_PATCH_BUNDLE_MAGIC, 8) == 0 &&
                 ns_patch_rd_u16(head + 8) == NS_PATCH_FORMAT && ns_patch_rd_u32(head + 12) == b->file_count;
    for (u32 i = 0; ok && i < b->file_count; i++) {
        const ns_patch_file *expect = &job->index->files[b->first_file + i];
        u8 lenb[2];
        ok = fread(lenb, 1, 2, f) == 2;
        u16 len = ok ? ns_patch_rd_u16(lenb) : 0;
        char name[520];
        ok = ok && len < sizeof(name) && fread(name, 1, len, f) == len;
        if (ok) name[len] = 0;
        u8 sizeb[8];
        ok = ok && fread(sizeb, 1, 8, f) == 8 && strcmp(name, expect->path) == 0 && ns_patch_rd_u64(sizeb) == expect->size;
    }
    u8 *buf = ok ? (u8 *)malloc(NS_PATCH_IO_CHUNK) : ns_null;
    ok = ok && buf;
    for (u32 i = 0; ok && i < b->file_count; i++) {
        const ns_patch_file *expect = &job->index->files[b->first_file + i];
        char out_path[NS_PATCH_PATH_MAX];
        ok = ns_patch_join(out_path, job->stage, expect->path) && ns_patch_mkdir_parent(out_path);
        FILE *out = ok ? fopen(out_path, "wb") : ns_null;
        ok = out != ns_null;
        ns_sha256_ctx sha;
        ns_sha256_init(&sha);
        for (u64 left = expect->size; ok && left > 0;) {
            szt n = left > NS_PATCH_IO_CHUNK ? NS_PATCH_IO_CHUNK : (szt)left;
            ok = fread(buf, 1, n, f) == n && fwrite(buf, 1, n, out) == n;
            ns_sha256_update(&sha, buf, n);
            left -= n;
        }
        if (out && fclose(out) != 0) ok = false;
        u8 digest[NS_PATCH_HASH_SIZE];
        ns_sha256_final(&sha, digest);
        if (ok && memcmp(digest, expect->hash, NS_PATCH_HASH_SIZE) != 0) ok = false;
        if (!ok) ns_patch_msg(error, error_size, "bad file %s in bundle %s", expect->path, b->file);
    }
    free(buf);
    fclose(f);
    if (!ok && error[0] == 0) ns_patch_msg(error, error_size, "malformed bundle %s%s", b->file, ns_null);
    return ok;
}

static ns_bool ns_patch_fetch_bundle(ns_patch_job *job, const ns_patch_bundle *b, char *error, szt error_size) {
    char path[NS_PATCH_PATH_MAX], remote[NS_PATCH_PATH_MAX * 2];
    if (!ns_patch_join(path, job->scratch, b->file)) return false;
    snprintf(remote, sizeof(remote), "%s%s", job->url->dir, b->file);
    // A dropped connection gets one more try; a digest mismatch does not.
    for (i32 attempt = 0; attempt < 2; attempt++) {
        error[0] = 0;
        ns_patch_file_sink sink = {.limit = b->size};
        sink.f = fopen(path, "wb");
        if (!sink.f) { ns_patch_msg(error, error_size, "cannot write %s%s", path, ns_null); return false; }
        ns_sha256_init(&sink.sha);
        ns_bool got = ns_patch_http_get(job->url, remote, -1, job->timeout_ms, ns_patch_file_write, &sink, error, error_size);
        if (fclose(sink.f) != 0) sink.failed = true;
        if (got && !sink.failed && sink.bytes == b->size) {
            u8 digest[NS_PATCH_HASH_SIZE];
            ns_sha256_final(&sink.sha, digest);
            if (memcmp(digest, b->hash, NS_PATCH_HASH_SIZE) != 0) {
                ns_patch_msg(error, error_size, "digest mismatch for %s%s", b->file, ns_null);
                remove(path);
                return false;
            }
            ns_bool ok = ns_patch_extract(job, b, path, error, error_size);
            remove(path);
            return ok;
        }
        if (got) ns_patch_msg(error, error_size, "size mismatch for %s%s", b->file, ns_null);
        remove(path);
    }
    return false;
}

#if defined(_WIN32)
static unsigned __stdcall ns_patch_worker(void *arg) {
#else
static void *ns_patch_worker(void *arg) {
#endif
    ns_patch_job *job = (ns_patch_job *)arg;
    for (;;) {
        ns_patch_mutex_lock(&job->lock);
        ns_bool stop = job->failed || job->next >= job->pending_count;
        u32 slot = stop ? 0 : job->pending[job->next++];
        ns_patch_mutex_unlock(&job->lock);
        if (stop) break;
        char error[256] = "";
        if (!ns_patch_fetch_bundle(job, &job->index->bundles[slot], error, sizeof(error))) {
            ns_patch_job_fail(job, error[0] ? error : "bundle download failed");
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return ns_null;
#endif
}

static void ns_patch_run_workers(ns_patch_job *job, i32 jobs) {
    if (jobs > (i32)job->pending_count) jobs = (i32)job->pending_count;
    if (jobs < 1) return;
#if defined(_WIN32)
    HANDLE threads[16];
    i32 started = 0;
    for (i32 i = 1; i < jobs && i < 16; i++) {
        HANDLE t = (HANDLE)_beginthreadex(ns_null, 0, ns_patch_worker, job, 0, ns_null);
        if (t) threads[started++] = t;
    }
    ns_patch_worker(job);
    for (i32 i = 0; i < started; i++) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
#else
    pthread_t threads[16];
    i32 started = 0;
    for (i32 i = 1; i < jobs && i < 16; i++) {
        if (pthread_create(&threads[started], ns_null, ns_patch_worker, job) == 0) started++;
    }
    ns_patch_worker(job);
    for (i32 i = 0; i < started; i++) pthread_join(threads[i], ns_null);
#endif
}

static void ns_patch_default_cache(char *out) {
    const char *env = getenv("NS_PATCH_DIR");
    if (env && env[0]) { snprintf(out, NS_PATCH_PATH_MAX, "%s", env); return; }
#if defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");
    snprintf(out, NS_PATCH_PATH_MAX, "%s/ns-patch", base ? base : ".");
#elif defined(__APPLE__)
    // Inside an iOS sandbox HOME is the app container, so this is the app's
    // own Caches directory there too.
    const char *home = getenv("HOME");
    snprintf(out, NS_PATCH_PATH_MAX, "%s/Library/Caches/ns-patch", home ? home : ".");
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(out, NS_PATCH_PATH_MAX, "%s/ns-patch", xdg);
    else snprintf(out, NS_PATCH_PATH_MAX, "%s/.cache/ns-patch", home ? home : ".");
#endif
}

// `<cache>/<name>`: every patch of one program lives below it.
static ns_bool ns_patch_app_dir(const char *cache_dir, const char *name, char *out) {
    char root[NS_PATCH_PATH_MAX], safe[256];
    if (cache_dir && cache_dir[0]) snprintf(root, sizeof(root), "%s", cache_dir);
    else ns_patch_default_cache(root);
    szt n = 0;
    for (const char *p = name; *p && n + 1 < sizeof(safe); p++) {
        char c = *p;
        ns_bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        safe[n++] = ok ? c : '_';
    }
    safe[n] = 0;
    if (n == 0 || safe[0] == '.') return false;
    return ns_patch_join(out, root, safe);
}

// The snapshot a previous run installed, with its index. False when there is
// none, or it is not for this program.
static ns_bool ns_patch_installed(const char *app_dir, const ns_patch_config *cfg, ns_patch_index *index, char *snapshot) {
    char current[NS_PATCH_PATH_MAX], index_path[NS_PATCH_PATH_MAX];
    if (!ns_patch_join(current, app_dir, "current")) return false;
    szt size = 0;
    u8 *text = ns_patch_read_all(current, &size);
    if (!text) return false;
    char digits[16] = "";
    memcpy(digits, text, size < sizeof(digits) - 1 ? size : sizeof(digits) - 1);
    free(text);
    u32 version = (u32)strtoul(digits, ns_null, 10);
    if (version == 0) return false;
    char name[16];
    snprintf(name, sizeof(name), "%u", version);
    if (!ns_patch_join(snapshot, app_dir, name) || !ns_patch_join(index_path, snapshot, ".nsapp")) return false;
    u8 *data = ns_patch_read_all(index_path, &size);
    ns_bool ok = data && ns_patch_index_decode(data, size, index, ns_null, 0);
    free(data);
    if (ok && (index->header.version != version || index->header.mode != (u8)cfg->mode || strcmp(index->name, cfg->name) != 0)) {
        ns_patch_index_free(index);
        ok = false;
    }
    // A missing program file means the snapshot was damaged after it was
    // installed; the shipped files run instead.
    char code[NS_PATCH_PATH_MAX];
    const ns_patch_file *program = ok ? ns_patch_index_find(index, index->code) : ns_null;
    if (ok && (!program || !ns_patch_join(code, snapshot, index->code) || ns_patch_file_size(code) != (i64)program->size)) {
        ns_patch_index_free(index);
        ok = false;
    }
    return ok;
}

static void ns_patch_use(ns_patch_state *state, const char *snapshot, const ns_patch_index *index) {
    state->patched = true;
    state->version = index->header.version;
    snprintf(state->root, sizeof(state->root), "%s", snapshot);
    ns_patch_join(state->code, snapshot, index->code);
}

// Find `f` among files the host already has: the installed snapshot (whose
// index says what each of its files is) or the shipped base directory.
static ns_bool ns_patch_reuse(const ns_patch_file *f, const char *snapshot, const ns_patch_index *installed,
                              const char *base_dir, const char *stage) {
    char from[NS_PATCH_PATH_MAX], to[NS_PATCH_PATH_MAX];
    if (!ns_patch_join(to, stage, f->path)) return false;
    if (installed) {
        for (u32 i = 0; i < installed->header.file_count; i++) {
            const ns_patch_file *old = &installed->files[i];
            if (old->size != f->size || memcmp(old->hash, f->hash, NS_PATCH_HASH_SIZE) != 0) continue;
            if (ns_patch_join(from, snapshot, old->path) && ns_patch_file_size(from) == (i64)f->size &&
                ns_patch_copy_file(from, to)) {
                return true;
            }
        }
    }
    if (base_dir && base_dir[0] && ns_patch_join(from, base_dir, f->path) && ns_patch_file_size(from) == (i64)f->size) {
        u8 digest[NS_PATCH_HASH_SIZE];
        if (ns_patch_hash_file(from, digest) && memcmp(digest, f->hash, NS_PATCH_HASH_SIZE) == 0) {
            return ns_patch_copy_file(from, to);
        }
    }
    return false;
}

typedef struct ns_patch_prune {
    const char *dir;
    const char *keep;
} ns_patch_prune;

static void ns_patch_prune_entry(void *user, const char *name, ns_bool is_dir) {
    (void)is_dir;
    ns_patch_prune *p = (ns_patch_prune *)user;
    if (strcmp(name, "current") == 0 || (p->keep && strcmp(name, p->keep) == 0)) return;
    char path[NS_PATCH_PATH_MAX];
    if (ns_patch_join(path, p->dir, name)) ns_patch_remove_tree(path);
}

static void ns_patch_log(const ns_patch_config *cfg, const char *text) {
    if (cfg->quiet) return;
    printf("patch: %s\n", text);
    fflush(stdout);
}

void ns_patch_update(const ns_patch_config *cfg, ns_patch_state *state) {
    memset(state, 0, sizeof(*state));
    state->version = cfg->base_version;
    char app_dir[NS_PATCH_PATH_MAX];
    if (!cfg->name || !ns_patch_app_dir(cfg->cache_dir, cfg->name, app_dir)) {
        snprintf(state->message, sizeof(state->message), "no usable patch cache directory");
        return;
    }

    // What runs when nothing newer can be installed: a snapshot a previous run
    // installed, unless the shipped files are at least as new (an app store
    // update replaced the host since).
    ns_patch_index installed = {0};
    char snapshot[NS_PATCH_PATH_MAX] = "";
    ns_bool has_installed = ns_patch_installed(app_dir, cfg, &installed, snapshot);
    if (has_installed && installed.header.version <= cfg->base_version) {
        ns_patch_index_free(&installed);
        has_installed = false;
    }
    if (has_installed) ns_patch_use(state, snapshot, &installed);
    u32 running = state->version;

    ns_patch_url url;
    char error[256] = "";
    i32 timeout = cfg->timeout_ms > 0 ? cfg->timeout_ms : 3000;
    if (!cfg->url || !cfg->url[0] || !ns_patch_url_parse(cfg->url, &url, error, sizeof(error))) {
        snprintf(state->message, sizeof(state->message), "%s", error[0] ? error : "no patch URL");
        ns_patch_index_free(&installed);
        return;
    }

    // 1. The fixed header alone says whether there is anything to do.
    ns_patch_mem_sink head = {.limit = NS_PATCH_HEADER_SIZE};
    ns_patch_header remote;
    if (!ns_patch_http_get(&url, url.path, NS_PATCH_HEADER_SIZE - 1, timeout, ns_patch_mem_write, &head, error, sizeof(error)) ||
        !ns_patch_header_decode(head.buf.data, head.buf.len, &remote)) {
        snprintf(state->message, sizeof(state->message), "patch check failed: %.200s", error[0] ? error : "not an .nsapp index");
        free(head.buf.data);
        ns_patch_index_free(&installed);
        return;
    }
    free(head.buf.data);
    if (remote.mode != (u8)cfg->mode) {
        snprintf(state->message, sizeof(state->message), "published patch is for another run mode");
        ns_patch_index_free(&installed);
        return;
    }
    if (remote.version <= running) {
        snprintf(state->message, sizeof(state->message), "up to date at patch %u", running);
        ns_patch_index_free(&installed);
        return;
    }

    // 2. The whole index: every bundle and file with its digest.
    ns_patch_mem_sink body = {.limit = (szt)NS_PATCH_HEADER_SIZE + remote.body_size};
    ns_patch_index index = {0};
    error[0] = 0;
    ns_bool ok = remote.body_size <= 64u * 1024u * 1024u &&
                 ns_patch_http_get(&url, url.path, -1, timeout, ns_patch_mem_write, &body, error, sizeof(error)) &&
                 ns_patch_index_decode(body.buf.data, body.buf.len, &index, error, sizeof(error));
    if (ok && (index.header.version != remote.version || strcmp(index.name, cfg->name) != 0)) {
        snprintf(error, sizeof(error), "published index is for `%s`, not `%s`", index.name, cfg->name);
        ok = false;
    }
    if (!ok) {
        snprintf(state->message, sizeof(state->message), "patch %u rejected: %.200s", remote.version, error[0] ? error : "bad index");
        free(body.buf.data);
        ns_patch_index_free(&index);
        ns_patch_index_free(&installed);
        return;
    }

    // 3. Assemble the new snapshot in a private stage: files the host already
    // has are copied, bundles holding anything else are downloaded.
#if defined(_WIN32)
    i32 pid = (i32)_getpid();
#else
    i32 pid = (i32)getpid();
#endif
    char stage_name[64], scratch_name[64], stage[NS_PATCH_PATH_MAX], scratch[NS_PATCH_PATH_MAX];
    snprintf(stage_name, sizeof(stage_name), ".stage-%u-%d", index.header.version, pid);
    snprintf(scratch_name, sizeof(scratch_name), ".bundles-%d", pid);
    ok = ns_patch_join(stage, app_dir, stage_name) && ns_patch_join(scratch, app_dir, scratch_name);
    if (ok) {
        ns_patch_remove_tree(stage);
        ok = ns_patch_mkdir_p(stage) && ns_patch_mkdir_p(scratch);
    }
    ns_patch_job job = {.cfg = cfg, .url = &url, .index = &index, .stage = stage, .scratch = scratch, .timeout_ms = timeout};
    ns_patch_mutex_init(&job.lock);
    job.pending = ok ? (u32 *)calloc(index.header.bundle_count, sizeof(u32)) : ns_null;
    ok = ok && job.pending;
    u64 download = 0;
    for (u32 i = 0; ok && i < index.header.bundle_count; i++) {
        const ns_patch_bundle *b = &index.bundles[i];
        ns_bool local = true;
        for (u32 f = b->first_file; local && f < b->first_file + b->file_count; f++) {
            local = ns_patch_reuse(&index.files[f], snapshot, has_installed ? &installed : ns_null, cfg->base_dir, stage);
        }
        if (!local) {
            job.pending[job.pending_count++] = i;
            download += b->size;
        }
    }
    if (ok) {
        char line[128];
        snprintf(line, sizeof(line), "installing patch %u: %u of %u bundle(s) to download, %.1f MB", index.header.version,
                 job.pending_count, index.header.bundle_count, (double)download / (1024.0 * 1024.0));
        ns_patch_log(cfg, line);
        ns_patch_run_workers(&job, cfg->jobs > 0 ? cfg->jobs : 4);
        if (job.failed) {
            snprintf(error, sizeof(error), "%s", job.error);
            ok = false;
        }
    }
    ns_patch_mutex_free(&job.lock);
    free(job.pending);
    ns_patch_remove_tree(scratch);

    // 4. Install: the index goes into the snapshot, the stage becomes
    // `<version>`, and `current` names it. A crash before the last step leaves
    // the previous snapshot in charge.
    char final_dir[NS_PATCH_PATH_MAX], index_path[NS_PATCH_PATH_MAX], current[NS_PATCH_PATH_MAX], version_name[16];
    snprintf(version_name, sizeof(version_name), "%u", index.header.version);
    if (ok) {
        ok = ns_patch_join(index_path, stage, ".nsapp") && ns_patch_write_all(index_path, body.buf.data, body.buf.len) &&
             ns_patch_join(final_dir, app_dir, version_name) && ns_patch_join(current, app_dir, "current");
        if (ok) {
            ns_patch_remove_tree(final_dir);
            ok = ns_patch_rename(stage, final_dir);
        }
        char text[24];
        i32 len = snprintf(text, sizeof(text), "%u\n", index.header.version);
        ok = ok && ns_patch_write_atomic(current, text, (szt)len);
        if (!ok) snprintf(error, sizeof(error), "cannot install into %.200s", app_dir);
    }
    free(body.buf.data);
    if (!ok) {
        ns_patch_remove_tree(stage);
        snprintf(state->message, sizeof(state->message), "patch %u not installed: %.200s", index.header.version, error);
        // The earlier `state` stands: the previous snapshot (or the base) runs.
    } else {
        ns_patch_index_free(&installed);
        has_installed = false;
        ns_patch_use(state, final_dir, &index);
        state->updated = true;
        snprintf(state->message, sizeof(state->message), "installed patch %u", index.header.version);
        ns_patch_prune prune = {.dir = app_dir, .keep = version_name};
        ns_patch_dir_each(app_dir, ns_patch_prune_entry, &prune);
    }
    ns_patch_index_free(&index);
    ns_patch_index_free(&installed);
}

void ns_patch_discard(const char *cache_dir, const char *name, u32 version) {
    char app_dir[NS_PATCH_PATH_MAX], current[NS_PATCH_PATH_MAX], snapshot[NS_PATCH_PATH_MAX], version_name[16];
    if (!name || !ns_patch_app_dir(cache_dir, name, app_dir)) return;
    snprintf(version_name, sizeof(version_name), "%u", version);
    if (ns_patch_join(current, app_dir, "current")) remove(current);
    if (ns_patch_join(snapshot, app_dir, version_name)) ns_patch_remove_tree(snapshot);
}

void ns_patch_publish_version(u32 version) {
    char text[16];
    snprintf(text, sizeof(text), "%u", version);
#if defined(_WIN32)
    _putenv_s(NS_PATCH_VERSION_ENV, text);
#else
    setenv(NS_PATCH_VERSION_ENV, text, 1);
#endif
}
