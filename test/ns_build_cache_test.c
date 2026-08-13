#include "ns_build_cache.h"
#include "ns_test.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void path(char *out, const char *root, const char *relative) {
    snprintf(out, PATH_MAX, "%s/%s", root, relative);
}

static ns_bool write_text(const char *file, const char *text) {
    FILE *f = fopen(file, "wb");
    if (!f) return false;
    ns_bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

// A build that records `inputs` for `artifact` under `config`, as `ns build`
// does once an artifact has been emitted.
static void record(const char *artifact, const char *config, const char **inputs, i32 count) {
    ns_build_cache cache;
    ns_build_cache_open(&cache, ns_str_cstr((char *)artifact), ns_str_cstr((char *)config));
    for (i32 i = 0; i < count; i++) ns_build_cache_add(&cache, ns_str_cstr((char *)inputs[i]));
    ns_expect(ns_build_cache_write(&cache), "a build writes its input stamps.");
    ns_build_cache_free(&cache);
}

// Freshness of a build that collects `inputs` for `artifact` under `config`.
static ns_bool fresh(const char *artifact, const char *config, const char **inputs, i32 count) {
    ns_build_cache cache;
    ns_build_cache_open(&cache, ns_str_cstr((char *)artifact), ns_str_cstr((char *)config));
    for (i32 i = 0; i < count; i++) ns_build_cache_add(&cache, ns_str_cstr((char *)inputs[i]));
    ns_bool result = ns_build_cache_fresh(&cache);
    ns_build_cache_free(&cache);
    return result;
}

int main(void) {
    char root[] = "/tmp/ns-build-cache-XXXXXX";
    ns_expect(mkdtemp(root) != ns_null, "build cache test creates a fixture directory.");

    char main_ns[PATH_MAX], other_ns[PATH_MAX], artifact[PATH_MAX], cache_file[PATH_MAX];
    char assets[PATH_MAX], asset_file[PATH_MAX];
    path(main_ns, root, "main.ns");
    path(other_ns, root, "other.ns");
    path(artifact, root, "demo");
    path(cache_file, root, ".ns-build/demo.cache");
    path(assets, root, "assets");
    path(asset_file, root, "assets/data.txt");
    ns_expect(write_text(main_ns, "fn main() {}\n") && write_text(other_ns, "fn other() {}\n"),
              "build cache test creates source fixtures.");

    ns_str expected_cache = ns_build_cache_path(ns_str_cstr(artifact));
    ns_expect(ns_str_equals(expected_cache, ns_str_cstr(cache_file)),
              "the cache of an artifact lives in .ns-build beside it.");
    ns_str_free(expected_cache);

    const char *inputs[] = {main_ns, other_ns};
    const char *config = "kind 1 output demo";
    ns_expect(!fresh(artifact, config, inputs, 2),
              "a build with no recorded inputs is never fresh.");

    ns_expect(write_text(artifact, "emitted\n"), "build cache test emits an artifact fixture.");
    record(artifact, config, inputs, 2);
    ns_expect(access(cache_file, R_OK) == 0, "the cache file is written beside the artifact.");
    ns_expect(fresh(artifact, config, inputs, 2),
              "unchanged inputs keep a recorded artifact up to date.");

    struct timespec touched[2] = {{time(ns_null) + 5, 0}, {time(ns_null) + 5, 0}};
    ns_expect(utimensat(AT_FDCWD, main_ns, touched, 0) == 0, "build cache test touches a source.");
    ns_expect(fresh(artifact, config, inputs, 2),
              "a touched source with unchanged contents does not force a rebuild.");

    ns_expect(write_text(main_ns, "fn main() { }\n"), "build cache test edits a source.");
    ns_expect(!fresh(artifact, config, inputs, 2), "an edited source forces a rebuild.");
    record(artifact, config, inputs, 2);
    ns_expect(fresh(artifact, config, inputs, 2), "the rebuild records the edited source.");

    char added_ns[PATH_MAX];
    path(added_ns, root, "added.ns");
    ns_expect(write_text(added_ns, "fn added() {}\n"), "build cache test adds a source.");
    const char *grown[] = {main_ns, other_ns, added_ns};
    ns_expect(!fresh(artifact, config, grown, 3), "a source added to the build forces a rebuild.");
    record(artifact, config, grown, 3);
    ns_expect(fresh(artifact, config, grown, 3), "the rebuild records the added source.");

    ns_expect(remove(added_ns) == 0, "build cache test removes a source.");
    ns_expect(!fresh(artifact, config, inputs, 2), "a deleted input forces a rebuild.");
    record(artifact, config, inputs, 2);

    // Inputs the linker discovers are recorded without being collected again,
    // so they are checked against the files on disk instead.
    ns_expect(fresh(artifact, config, inputs, 1),
              "an unchanged input that is not collected again keeps the artifact up to date.");
    ns_expect(write_text(other_ns, "fn other() { }\n"), "build cache test edits a discovered input.");
    ns_expect(!fresh(artifact, config, inputs, 1), "an edited discovered input forces a rebuild.");
    record(artifact, config, inputs, 2);

    ns_expect(!fresh(artifact, "kind 2 output demo", inputs, 2),
              "a different build configuration forces a rebuild.");
    ns_expect(fresh(artifact, config, inputs, 2),
              "the recorded configuration keeps matching builds up to date.");

    ns_expect(remove(artifact) == 0, "build cache test removes the artifact.");
    ns_expect(!fresh(artifact, config, inputs, 2), "a missing artifact forces a rebuild.");
    ns_expect(write_text(artifact, "emitted\n"), "build cache test re-emits the artifact.");

    ns_expect(mkdir(assets, 0755) == 0 && write_text(asset_file, "one\n"),
              "build cache test creates a packaged asset.");
    ns_build_cache cache;
    ns_build_cache_open(&cache, ns_str_cstr(artifact), ns_str_cstr((char *)config));
    ns_build_cache_add(&cache, ns_str_cstr(main_ns));
    ns_build_cache_add(&cache, ns_str_cstr(other_ns));
    ns_build_cache_add_tree(&cache, ns_str_cstr(assets));
    ns_expect(!ns_build_cache_fresh(&cache), "an asset added to a bundle forces a rebuild.");
    ns_expect(ns_build_cache_write(&cache), "the rebuild records the packaged asset.");
    ns_build_cache_free(&cache);

    ns_build_cache_open(&cache, ns_str_cstr(artifact), ns_str_cstr((char *)config));
    ns_build_cache_add(&cache, ns_str_cstr(main_ns));
    ns_build_cache_add(&cache, ns_str_cstr(other_ns));
    ns_build_cache_add_tree(&cache, ns_str_cstr(assets));
    ns_expect(ns_build_cache_fresh(&cache), "unchanged assets keep a bundle up to date.");
    ns_build_cache_free(&cache);

    ns_expect(write_text(asset_file, "two\n"), "build cache test edits a packaged asset.");
    ns_build_cache_open(&cache, ns_str_cstr(artifact), ns_str_cstr((char *)config));
    ns_build_cache_add(&cache, ns_str_cstr(main_ns));
    ns_build_cache_add(&cache, ns_str_cstr(other_ns));
    ns_build_cache_add_tree(&cache, ns_str_cstr(assets));
    ns_expect(!ns_build_cache_fresh(&cache), "an edited asset forces a rebuild.");
    ns_build_cache_free(&cache);

    ns_expect(write_text(cache_file, "ns-build-cache/v0\nstamp bogus\n"),
              "build cache test corrupts the cache file.");
    ns_expect(!fresh(artifact, config, inputs, 2),
              "a cache file from another version is ignored instead of trusted.");

    return 0;
}
