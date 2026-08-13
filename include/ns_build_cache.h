#pragma once

#include "ns_type.h"

// Incremental `ns build` bookkeeping. A build stamps every file it reads with
// that file's last modify time, size, and content hash, and stores the stamps
// beside the artifact. The next build re-stats the same inputs: an unchanged
// time and size reuses the stored hash without reading the file, a touched
// file is hashed again, and only a different hash, a different input set, a
// different build configuration, or a missing artifact forces a recompile.

typedef struct ns_build_stamp {
    ns_str path;     // input file, owned by the stamp
    i64 mtime;       // last modify time in whole seconds, 0 when the file is missing
    i32 mtime_nsec;  // nanoseconds within `mtime`, 0 when the filesystem has no sub-second time
    i64 size;        // file size in bytes, -1 when the file is missing
    u64 hash;        // FNV-1a 64 content hash, 0 when the file is missing
} ns_build_stamp;

typedef struct ns_build_cache {
    ns_str path;              // cache file backing this artifact
    ns_str artifact;          // build output whose existence is required
    ns_str config;            // build configuration of the current build
    ns_str recorded_config;   // build configuration of the recorded build
    ns_build_stamp *inputs;   // stamps collected for the current build
    ns_build_stamp *recorded; // stamps read back from `path`
    ns_bool loaded;           // a previous cache file was read
} ns_build_cache;

// Cache file that belongs to `artifact`: `<dir>/.ns-build/<name>.cache`. The
// result is heap-owned. Keeping it below the output directory lets `ns clean`
// and the scaffolded `.gitignore` treat it as ordinary build output.
ns_str ns_build_cache_path(ns_str artifact);

// Begin a build against `artifact` and read back its recorded stamps. `config`
// describes everything outside the input files that changes the artifact, such
// as the artifact kind and the output path; a different value forces a
// rebuild. The cache copies both strings.
void ns_build_cache_open(ns_build_cache *cache, ns_str artifact, ns_str config);

// Stamp one input file. Repeated paths are recorded once, and a missing file
// is stamped as missing so that creating it later invalidates the cache.
void ns_build_cache_add(ns_build_cache *cache, ns_str path);

// Stamp every regular file below `dir`. A missing directory adds nothing.
void ns_build_cache_add_tree(ns_build_cache *cache, ns_str dir);

// True when the artifact exists, the configuration matches, every collected
// input is recorded with the same content hash, and every recorded input still
// hashes to its recorded value. Recorded inputs that were not collected are
// re-stamped here, so files discovered while linking a previous build (sibling
// modules, installed module declarations) still guard the artifact.
ns_bool ns_build_cache_fresh(ns_build_cache *cache);

// Write the collected stamps. Returns false when the file cannot be written.
ns_bool ns_build_cache_write(ns_build_cache *cache);

void ns_build_cache_free(ns_build_cache *cache);
