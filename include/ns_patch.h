#pragma once

// Over-the-air patches for interpreted programs (`target = "eval"` and
// `target = "emu"`). See doc/patch.md.
//
// `ns patch` writes a patch directory: one `<name>.nsapp` index and the
// content-addressed `<hash>.nsbundle` files it names. A host that runs the
// program's code as data (the AST interpreter or ns_cpu) calls
// ns_patch_update() before the program starts: it reads the index's fixed
// header to learn the published version, and only when that is newer than what
// it runs downloads the rest, fetches the bundles it cannot rebuild from files
// it already has in parallel, verifies every byte against the SHA-256 digests
// the index lists, and installs the result as a snapshot directory the program
// then runs from.
//
// This file has no dependency on the parser or the VM, so the generated Apple
// apps compile it next to the embedded runtime.

#include "ns_type.h"

#define NS_PATCH_FORMAT 1
// Size of the fixed `.nsapp` header a client requests first.
#define NS_PATCH_HEADER_SIZE 64
#define NS_PATCH_HASH_SIZE 32
// Bundles are cut at this many bytes of files, so an update transfers only the
// chunks whose files changed and the chunks download side by side.
#define NS_PATCH_CHUNK_SIZE (4u * 1024u * 1024u)
// The program's code travels as this file at the root of a snapshot.
#define NS_PATCH_CODE_EVAL "LinkedProject.ns"
#define NS_PATCH_CODE_EMU "LinkedProject.nsc"
// The environment variable a host publishes the running patch version in.
#define NS_PATCH_VERSION_ENV "NS_PATCH_VERSION"

typedef enum ns_patch_mode {
    NS_PATCH_MODE_NONE = 0,
    NS_PATCH_MODE_EVAL = 1, // code is linked source for the AST interpreter
    NS_PATCH_MODE_EMU = 2,  // code is an ns_cpu image
} ns_patch_mode;

// ---- SHA-256 --------------------------------------------------------------

typedef struct ns_sha256_ctx {
    u32 state[8];
    u64 bytes;
    u8 block[64];
    u32 used;
} ns_sha256_ctx;

void ns_sha256_init(ns_sha256_ctx *ctx);
void ns_sha256_update(ns_sha256_ctx *ctx, const void *data, szt size);
void ns_sha256_final(ns_sha256_ctx *ctx, u8 out[NS_PATCH_HASH_SIZE]);
void ns_sha256(const void *data, szt size, u8 out[NS_PATCH_HASH_SIZE]);
// `out` receives 64 lowercase hex digits and a terminating NUL.
void ns_sha256_hex(const u8 hash[NS_PATCH_HASH_SIZE], char out[NS_PATCH_HASH_SIZE * 2 + 1]);

// ---- the index ------------------------------------------------------------

// The fixed header at the front of every `.nsapp`.
typedef struct ns_patch_header {
    u16 format;
    u8 mode;             // ns_patch_mode
    u32 version;         // patch version, 1 and up
    u32 body_size;       // bytes after the header
    u32 bundle_count;
    u32 file_count;
    u8 body_hash[NS_PATCH_HASH_SIZE];
} ns_patch_header;

typedef struct ns_patch_bundle {
    char *file;          // `<hash>.nsbundle`, relative to the index URL
    u64 size;
    u8 hash[NS_PATCH_HASH_SIZE];
    u32 first_file;      // the files it packs are files[first_file, +file_count)
    u32 file_count;
} ns_patch_bundle;

typedef struct ns_patch_file {
    char *path;          // relative, `/`-separated
    u64 size;
    u8 hash[NS_PATCH_HASH_SIZE];
} ns_patch_file;

typedef struct ns_patch_index {
    ns_patch_header header;
    char *name;          // manifest target name
    char *app_version;   // manifest `version` when the patch was made
    char *code;          // the file in the snapshot that holds the program
    u64 created;         // unix seconds
    ns_patch_bundle *bundles;
    ns_patch_file *files;
} ns_patch_index;

// Decode the fixed header. False when `data` is not a supported `.nsapp`.
ns_bool ns_patch_header_decode(const u8 *data, szt size, ns_patch_header *out);
// Decode a whole index and check its body against the header digest. On
// failure `error` (when given) says why and the index is left empty.
ns_bool ns_patch_index_decode(const u8 *data, szt size, ns_patch_index *out, char *error, szt error_size);
void ns_patch_index_free(ns_patch_index *index);

// ---- writing a patch (`ns patch`) ------------------------------------------

typedef struct ns_patch_input {
    const char *name;
    const char *app_version;
    ns_patch_mode mode;
    const u8 *code;       // the program: linked source (eval) or ns_cpu image (emu)
    szt code_size;
    const char *root;     // asset paths are relative to this directory
    const char *const *assets; // files or directories below `root`
    i32 asset_count;
    const char *out_dir;  // created; stale bundles in it are removed
    u32 version;          // 0: one past the index already in `out_dir`
    u64 chunk_size;       // 0: NS_PATCH_CHUNK_SIZE
} ns_patch_input;

typedef struct ns_patch_summary {
    u32 version;
    u32 previous;         // version found in `out_dir`, 0 when none
    ns_bool unchanged;    // identical to `previous`, nothing rewritten
    u32 bundle_count;
    u32 file_count;
    u64 bytes;            // total bundle bytes
    char index_path[1024];
    char error[256];
} ns_patch_summary;

ns_bool ns_patch_write(const ns_patch_input *in, ns_patch_summary *out);

// ---- applying a patch (hosts) ----------------------------------------------

typedef struct ns_patch_config {
    const char *url;      // http://host[:port]/path/<name>.nsapp
    const char *name;     // must match the index
    ns_patch_mode mode;   // must match the index
    const char *base_dir; // files the host shipped with, reused when unchanged
    u32 base_version;     // patch the shipped files already are, 0 for none
    const char *cache_dir; // NULL: NS_PATCH_DIR, else the per-user cache
    i32 timeout_ms;       // per connection step; 0: 3000
    i32 jobs;             // parallel bundle downloads; 0: 4
    ns_bool quiet;        // no progress lines on stdout
} ns_patch_config;

typedef struct ns_patch_state {
    u32 version;          // the patch to run: base_version when none applies
    ns_bool patched;      // `root` is an installed snapshot
    ns_bool updated;      // this call downloaded and installed it
    char root[1024];      // directory the program runs in (patched only)
    char code[1024];      // the program file inside `root` (patched only)
    char message[256];    // why nothing was applied, or what was
} ns_patch_state;

// Check `cfg->url` and install a newer patch. Never fails the host: on any
// network or verification problem it keeps the newest snapshot already
// installed (or none), and `state->message` says what happened.
void ns_patch_update(const ns_patch_config *cfg, ns_patch_state *state);

// Forget the installed snapshot of `name`, for a host whose patched program
// could not load, so the next start runs the shipped files again.
void ns_patch_discard(const char *cache_dir, const char *name, u32 version);

// Publish `version` to the program as NS_PATCH_VERSION.
void ns_patch_publish_version(u32 version);
