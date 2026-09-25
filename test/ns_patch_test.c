#define _GNU_SOURCE
#include "ns_patch.h"
#include "ns_test.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void path(char *out, const char *root, const char *relative) {
    snprintf(out, PATH_MAX, "%s/%s", root, relative);
}

static ns_bool write_bytes(const char *file, const void *data, size_t size) {
    FILE *f = fopen(file, "wb");
    if (!f) return false;
    ns_bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static ns_bool write_text(const char *file, const char *text) { return write_bytes(file, text, strlen(text)); }

static char *read_text(const char *file, size_t *size) {
    FILE *f = fopen(file, "rb");
    if (!f) return ns_null;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)n + 1);
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    data[got] = 0;
    if (size) *size = got;
    return data;
}

static ns_bool file_is(const char *file, const char *text) {
    char *data = read_text(file, ns_null);
    ns_bool same = data && strcmp(data, text) == 0;
    free(data);
    return same;
}

static void copy_dir(const char *from, const char *to) {
    char command[PATH_MAX * 3];
    snprintf(command, sizeof(command), "mkdir -p '%s' && cp '%s'/* '%s'/", to, from, to);
    ns_expect(system(command) == 0, "test copies a published patch directory.");
}

static ns_bool hex_is(const char *text, const char *expect) {
    u8 hash[NS_PATCH_HASH_SIZE];
    char hex[NS_PATCH_HASH_SIZE * 2 + 1];
    ns_sha256(text, strlen(text), hash);
    ns_sha256_hex(hash, hex);
    return strcmp(hex, expect) == 0;
}

// ---- a static file server that honors Range: bytes=0-N, and counts requests

typedef struct server {
    i32 fd;
    i32 port;
    char root[PATH_MAX];
    i32 requests;
    i32 bundle_requests;
    ns_bool range_seen;
    pthread_mutex_t lock;
} server;

static void *serve(void *arg) {
    server *s = arg;
    for (;;) {
        i32 client = accept(s->fd, ns_null, ns_null);
        if (client < 0) return ns_null;
        char request[4096];
        size_t got = 0;
        while (got < sizeof(request) - 1) {
            ssize_t n = recv(client, request + got, sizeof(request) - 1 - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
            request[got] = 0;
            if (strstr(request, "\r\n\r\n")) break;
        }
        request[got] = 0;
        char target[1024] = "";
        sscanf(request, "GET %1023s", target);
        long range_end = -1;
        const char *range = strstr(request, "Range: bytes=0-");
        if (range) range_end = strtol(range + 15, ns_null, 10);
        pthread_mutex_lock(&s->lock);
        s->requests++;
        if (strstr(target, ".nsbundle")) s->bundle_requests++;
        if (range) s->range_seen = true;
        pthread_mutex_unlock(&s->lock);
        char file[PATH_MAX * 2];
        snprintf(file, sizeof(file), "%s%s", s->root, target);
        size_t size = 0;
        char *body = strstr(target, "..") ? ns_null : read_text(file, &size);
        char head[256];
        if (!body) {
            i32 n = snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            send(client, head, (size_t)n, 0);
        } else {
            size_t send_size = size;
            ns_bool partial = range_end >= 0 && (size_t)range_end + 1 < size;
            if (partial) send_size = (size_t)range_end + 1;
            i32 n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                             partial ? "206 Partial Content" : "200 OK", send_size);
            send(client, head, (size_t)n, 0);
            for (size_t sent = 0; sent < send_size;) {
                ssize_t w = send(client, body + sent, send_size - sent, 0);
                if (w <= 0) break;
                sent += (size_t)w;
            }
            free(body);
        }
        close(client);
    }
}

static void server_start(server *s, const char *root) {
    memset(s, 0, sizeof(*s));
    snprintf(s->root, sizeof(s->root), "%s", root);
    pthread_mutex_init(&s->lock, ns_null);
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    ns_expect(bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(s->fd, 16) == 0,
              "test server listens on loopback.");
    socklen_t len = sizeof(addr);
    getsockname(s->fd, (struct sockaddr *)&addr, &len);
    s->port = ntohs(addr.sin_port);
    pthread_t thread;
    pthread_create(&thread, ns_null, serve, s);
    pthread_detach(thread);
}

static void server_reset(server *s) {
    pthread_mutex_lock(&s->lock);
    s->requests = 0;
    s->bundle_requests = 0;
    pthread_mutex_unlock(&s->lock);
}

// ---- a project with code and assets, written as a patch -------------------

static ns_bool make_patch(const char *root, const char *out, const char *code, u32 version, ns_patch_summary *summary) {
    const char *assets[] = {"res", "missing"};
    ns_patch_input in = {
        .name = "demo",
        .app_version = "1.0",
        .mode = NS_PATCH_MODE_EVAL,
        .code = (const u8 *)code,
        .code_size = strlen(code),
        .root = root,
        .assets = assets,
        .asset_count = 2,
        .out_dir = out,
        .version = version,
        .chunk_size = 64 * 1024,
    };
    return ns_patch_write(&in, summary);
}

static ns_patch_state update(const char *url, const char *base, const char *cache, u32 base_version) {
    ns_patch_config cfg = {
        .url = url, .name = "demo", .mode = NS_PATCH_MODE_EVAL, .base_dir = base,
        .base_version = base_version, .cache_dir = cache, .timeout_ms = 2000, .jobs = 4, .quiet = true,
    };
    ns_patch_state state;
    ns_patch_update(&cfg, &state);
    return state;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    ns_expect(hex_is("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") &&
                  hex_is("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") &&
                  hex_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
              "SHA-256 matches the FIPS 180-4 test vectors.");
    {
        // Streaming across odd chunk sizes equals hashing at once.
        u8 data[1000], a[NS_PATCH_HASH_SIZE], b[NS_PATCH_HASH_SIZE];
        for (i32 i = 0; i < 1000; i++) data[i] = (u8)(i * 7);
        ns_sha256(data, sizeof(data), a);
        ns_sha256_ctx ctx;
        ns_sha256_init(&ctx);
        for (i32 at = 0; at < 1000;) {
            i32 n = at % 3 == 0 ? 63 : 1;
            if (at + n > 1000) n = 1000 - at;
            ns_sha256_update(&ctx, data + at, (szt)n);
            at += n;
        }
        ns_sha256_final(&ctx, b);
        ns_expect(memcmp(a, b, sizeof(a)) == 0, "SHA-256 streams across block boundaries.");
    }

    char root[] = "/tmp/ns-patch-test-XXXXXX";
    ns_expect(mkdtemp(root) != ns_null, "patch test creates a fixture directory.");
    char project[PATH_MAX], res[PATH_MAX], res_sub[PATH_MAX], out[PATH_MAX], site[PATH_MAX], cache[PATH_MAX];
    path(project, root, "project");
    path(res, root, "project/res");
    path(res_sub, root, "project/res/sub");
    path(out, root, "project/bin/demo_patch");
    path(site, root, "site");
    path(cache, root, "cache");
    ns_expect(mkdir(project, 0755) == 0 && mkdir(res, 0755) == 0 && mkdir(res_sub, 0755) == 0,
              "patch test creates the project tree.");

    char file_a[PATH_MAX], file_b[PATH_MAX], file_c[PATH_MAX], ds_store[PATH_MAX];
    path(file_a, project, "res/a.txt");
    path(file_b, project, "res/sub/b.bin");
    path(file_c, project, "res/sub/c.bin");
    path(ds_store, project, "res/.DS_Store");
    static u8 blob[48 * 1024];
    for (size_t i = 0; i < sizeof(blob); i++) blob[i] = (u8)(i * 31 + 7);
    ns_expect(write_text(file_a, "alpha\n") && write_bytes(file_b, blob, sizeof(blob)) &&
                  write_bytes(file_c, blob, sizeof(blob) - 5) && write_text(ds_store, "finder"),
              "patch test writes the project assets.");

    // ---- writing ----------------------------------------------------------
    ns_patch_summary sum;
    ns_expect(make_patch(project, out, "fn main() { print(`one`) }\n", 0, &sum) && sum.version == 1 &&
                  sum.previous == 0 && !sum.unchanged && sum.file_count == 4,
              "the first patch is version 1 and packs the program and every asset but Finder metadata.");
    // program | a.txt + b.bin (under 64 KiB) | c.bin
    ns_expect(sum.bundle_count == 3, "files are cut into bundles at the chunk size, the program alone.");

    char index_path[PATH_MAX];
    path(index_path, out, "demo.nsapp");
    size_t index_size = 0;
    char *index_data = read_text(index_path, &index_size);
    ns_patch_index index;
    char error[256] = "";
    ns_expect(index_data && ns_patch_index_decode((const u8 *)index_data, index_size, &index, error, sizeof(error)),
              "the written index decodes.");
    ns_expect(index.header.version == 1 && index.header.mode == NS_PATCH_MODE_EVAL && strcmp(index.name, "demo") == 0 &&
                  strcmp(index.code, NS_PATCH_CODE_EVAL) == 0 && strcmp(index.files[0].path, NS_PATCH_CODE_EVAL) == 0 &&
                  strcmp(index.files[1].path, "res/a.txt") == 0 && strcmp(index.files[3].path, "res/sub/c.bin") == 0,
              "the index lists the program first, then the assets by path.");
    ns_patch_header header;
    ns_expect(ns_patch_header_decode((const u8 *)index_data, NS_PATCH_HEADER_SIZE, &header) && header.version == 1,
              "the fixed header alone says which version is published.");
    for (u32 i = 0; i < index.header.bundle_count; i++) {
        char bundle[PATH_MAX];
        path(bundle, out, index.bundles[i].file);
        size_t size = 0;
        char *data = read_text(bundle, &size);
        u8 digest[NS_PATCH_HASH_SIZE];
        ns_sha256(data, size, digest);
        ns_expect(data && size == index.bundles[i].size && memcmp(digest, index.bundles[i].hash, sizeof(digest)) == 0,
                  "each bundle on disk matches the size and digest its index lists.");
        free(data);
    }
    char first_program_bundle[64];
    snprintf(first_program_bundle, sizeof(first_program_bundle), "%s", index.bundles[0].file);
    char first_asset_bundle[64];
    snprintf(first_asset_bundle, sizeof(first_asset_bundle), "%s", index.bundles[1].file);
    ns_patch_index_free(&index);

    {
        // A flipped byte in the body, a truncated index, and a path that climbs
        // out of the snapshot are all rejected.
        char *bad = malloc(index_size);
        memcpy(bad, index_data, index_size);
        bad[index_size - 1] ^= 1;
        ns_expect(!ns_patch_index_decode((const u8 *)bad, index_size, &index, error, sizeof(error)) &&
                      strstr(error, "digest"),
                  "an index whose body does not match its digest is rejected.");
        ns_expect(!ns_patch_index_decode((const u8 *)index_data, index_size - 10, &index, ns_null, 0),
                  "a truncated index is rejected.");
        memcpy(bad, index_data, index_size);
        char *where = memmem(bad, index_size, "res/a.txt", 9);
        memcpy(where, "../../etc", 9);
        ns_sha256(bad + NS_PATCH_HEADER_SIZE, index_size - NS_PATCH_HEADER_SIZE, (u8 *)bad + 32);
        ns_expect(!ns_patch_index_decode((const u8 *)bad, index_size, &index, error, sizeof(error)) &&
                      strstr(error, "unsafe"),
                  "an index naming a path outside its snapshot is rejected even with a valid digest.");
        free(bad);
    }
    free(index_data);

    ns_expect(make_patch(project, out, "fn main() { print(`one`) }\n", 0, &sum) && sum.version == 1 && sum.unchanged,
              "an identical patch keeps its version.");
    ns_expect(make_patch(project, out, "fn main() { print(`two`) }\n", 0, &sum) && sum.version == 2 &&
                  sum.previous == 1 && !sum.unchanged,
              "a changed program counts the version on.");
    char stale[PATH_MAX], kept[PATH_MAX];
    path(stale, out, first_program_bundle);
    path(kept, out, first_asset_bundle);
    ns_expect(access(stale, F_OK) != 0 && access(kept, F_OK) == 0,
              "the old program bundle is swept and the unchanged asset bundle is kept by name.");
    ns_expect(make_patch(project, out, "fn main() { print(`two`) }\n", 9, &sum) && sum.version == 9 && !sum.unchanged,
              "a pinned patch_version is written as given.");

    // ---- applying ---------------------------------------------------------
    copy_dir(out, site);
    server srv;
    server_start(&srv, root);
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/site/demo.nsapp", srv.port);

    // A client with no local files downloads every bundle.
    char empty_base[PATH_MAX];
    path(empty_base, root, "nothing-here");
    ns_patch_state st = update(url, empty_base, cache, 0);
    char program[PATH_MAX], asset[PATH_MAX];
    snprintf(program, sizeof(program), "%s", st.code);
    path(asset, st.root, "res/sub/c.bin");
    ns_expect(st.patched && st.updated && st.version == 9 && file_is(program, "fn main() { print(`two`) }\n") &&
                  access(asset, R_OK) == 0,
              "a client installs the published patch into a snapshot it runs from.");
    ns_expect(srv.bundle_requests == 3 && srv.range_seen, "the client asked for the fixed header first, then every bundle.");

    // Asking again only reads the header.
    server_reset(&srv);
    st = update(url, empty_base, cache, 0);
    ns_expect(st.patched && !st.updated && st.version == 9 && srv.requests == 1,
              "an up-to-date client makes one header request and keeps its snapshot.");

    // The next patch changes only the program: the assets come from the
    // installed snapshot, not the network.
    ns_expect(make_patch(project, out, "fn main() { print(`three`) }\n", 10, &sum), "patch 10 is written.");
    copy_dir(out, site);
    server_reset(&srv);
    st = update(url, empty_base, cache, 0);
    ns_expect(st.patched && st.updated && st.version == 10 && srv.bundle_requests == 1 &&
                  file_is(st.code, "fn main() { print(`three`) }\n"),
              "an update downloads only the bundles whose files the client does not already have.");
    char old_snapshot[PATH_MAX];
    path(old_snapshot, cache, "demo/9");
    ns_expect(access(old_snapshot, F_OK) != 0, "the previous snapshot is removed once the new one is installed.");

    // A fresh client whose shipped files already hold the assets reuses them.
    char cache2[PATH_MAX];
    path(cache2, root, "cache2");
    server_reset(&srv);
    st = update(url, project, cache2, 0);
    ns_expect(st.patched && st.version == 10 && srv.bundle_requests == 1,
              "a fresh client copies unchanged files from its base directory instead of downloading them.");

    // A shipped app that already is patch 10 does nothing.
    char cache3[PATH_MAX];
    path(cache3, root, "cache3");
    server_reset(&srv);
    st = update(url, project, cache3, 10);
    ns_expect(!st.patched && st.version == 10 && srv.requests == 1, "a base at the published version stays on its files.");

    // A tampered bundle is rejected, and the installed snapshot keeps running.
    ns_expect(make_patch(project, out, "fn main() { print(`four`) }\n", 11, &sum), "patch 11 is written.");
    copy_dir(out, site);
    char code_bundle[PATH_MAX];
    {
        size_t n = 0;
        char site_index[PATH_MAX];
        path(site_index, site, "demo.nsapp");
        char *data = read_text(site_index, &n);
        ns_patch_index idx;
        ns_expect(ns_patch_index_decode((const u8 *)data, n, &idx, ns_null, 0), "patch 11 index decodes.");
        path(code_bundle, site, idx.bundles[0].file);
        ns_patch_index_free(&idx);
        free(data);
        size_t size = 0;
        char *bundle = read_text(code_bundle, &size);
        bundle[size - 2] ^= 0x20;
        write_bytes(code_bundle, bundle, size);
        free(bundle);
    }
    st = update(url, empty_base, cache, 0);
    ns_expect(st.patched && !st.updated && st.version == 10 && strstr(st.message, "digest") &&
                  file_is(st.code, "fn main() { print(`three`) }\n"),
              "a bundle that does not match its digest is never installed; the previous patch keeps running.");

    // An unreachable server leaves the installed patch in charge.
    st = update("http://127.0.0.1:1/demo.nsapp", empty_base, cache, 0);
    ns_expect(st.patched && st.version == 10 && strstr(st.message, "cannot reach"),
              "an offline client runs the patch it installed last.");

    // A patch for another program or mode is ignored.
    ns_patch_config other = {.url = url, .name = "other", .mode = NS_PATCH_MODE_EVAL, .cache_dir = cache, .quiet = true};
    ns_patch_update(&other, &st);
    ns_expect(!st.patched && strstr(st.message, "not `other`"), "a client ignores an index published for another program.");
    ns_patch_config emu = {.url = url, .name = "demo", .mode = NS_PATCH_MODE_EMU, .cache_dir = cache, .quiet = true};
    ns_patch_update(&emu, &st);
    ns_expect(!st.patched && strstr(st.message, "run mode"), "a client ignores an index for another run mode.");

    // Discarding forgets the snapshot.
    ns_patch_discard(cache, "demo", 10);
    st = update("http://127.0.0.1:1/demo.nsapp", empty_base, cache, 0);
    ns_expect(!st.patched && st.version == 0, "a discarded patch no longer runs.");

    ns_patch_publish_version(42);
    ns_expect(strcmp(getenv(NS_PATCH_VERSION_ENV), "42") == 0, "the running patch is published as NS_PATCH_VERSION.");

    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) return 1;
    return 0;
}
