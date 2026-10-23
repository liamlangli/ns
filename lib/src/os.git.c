// Sandboxed Git backend. The desktop continues to use its installed Git.
// NS_GIT_EMBEDDED is set by ns project only for apps opting in with
// os_git_embedded(). Never fall back to executing a program on iOS.
#ifdef NS_GIT_EMBEDDED
#include "os.h"
#include <git2.h>
#include <git2/sys/errors.h>
#include <git2/sys/repository.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>

// Apple adapter owns Keychain access and the first-use host trust prompt.
extern char *os_git_secret_copy(const char *name);
extern int os_git_trust_host(const char *endpoint, const char *sha256);
static pthread_once_t git_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t git_write_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local i32 git_status_code = -1;
static _Thread_local char *git_text;
static _Thread_local size_t git_used, git_limit;
static i32 git_percent = -1;
static void git_start(void) {
    git_libgit2_init();
    // The app's own global config, beside its other private state; the
    // sandbox $HOME itself is not where users expect settings to live.
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/Library/Application Support", home);
        mkdir(path, 0700); // absent on a fresh install
        strncat(path, "/Git", sizeof(path) - strlen(path) - 1);
        mkdir(path, 0700);
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, path);
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, "");
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, "");
    }
    git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, 15000);
    git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, 60000);
}
ns_bool os_git_embedded(void) { return true; }
i32 os_git_status(void) { return git_status_code; }
i32 os_git_progress(void) { return __atomic_load_n(&git_percent, __ATOMIC_RELAXED); }

// The result buffer lives until the thread's next call. Lens runs Git on
// worker tasks, a thread each, so the key frees a buffer its thread left.
static pthread_key_t git_text_key;
static pthread_once_t git_text_once = PTHREAD_ONCE_INIT;
static void git_text_key_init(void) { pthread_key_create(&git_text_key, free); }
static char *git_text_keep(char *text) {
    pthread_once(&git_text_once, git_text_key_init);
    pthread_setspecific(git_text_key, text);
    return text;
}
static int git_output_start(i32 limit) {
    pthread_once(&git_once, git_start);
    free(git_text); git_text = git_text_keep(NULL); git_used = 0; git_limit = 0; git_status_code = -1;
    if (limit <= 0 || limit > 1048576) return -1;
    git_text = git_text_keep(calloc((size_t)limit + 1, 1));
    if (!git_text) return -1;
    git_limit = (size_t)limit;
    return 0;
}
static int git_output(const char *text, size_t size) {
    if (!size) return 0;
    if (!text) return -1;
    if (size > git_limit - git_used) { git_error_set_str(GIT_ERROR_NOMEMORY, "Git output exceeds the detail budget"); git_status_code = -1; return -1; }
    memcpy(git_text + git_used, text, size); git_used += size; git_text[git_used] = 0;
    return 0;
}
static int git_printf(const char *format, ...) {
    va_list args; va_start(args, format);
    int n = vsnprintf(git_text + git_used, git_limit - git_used + 1, format, args);
    va_end(args);
    if (n < 0 || (size_t)n > git_limit - git_used) { git_error_set_str(GIT_ERROR_NOMEMORY, "Git output exceeds the detail budget"); return -1; }
    git_used += (size_t)n;
    return 0;
}
static const char *git_finish(int result) {
    if (result < 0) {
        const git_error *error = git_error_last();
        // Replace partial machine-readable output with an explicit failure.
        if (git_text) snprintf(git_text, git_limit + 1, "%s", error && error->message ? error->message : "Git operation failed or output exceeds the limit");
        git_status_code = -1;
    } else git_status_code = 0;
    return git_text ? git_text : "";
}
static const char *git_quiet_finish(int result) {
    const char *text = git_finish(result);
    return result < 0 ? "" : text;
}
static int git_patch_line(const git_diff_delta *delta, const git_diff_hunk *hunk, const git_diff_line *line, void *payload) {
    (void)delta; (void)hunk; (void)payload;
    if (line->origin == '+' || line->origin == '-' || line->origin == ' ') {
        if (git_output(&line->origin, 1) < 0) return -1;
    }
    return git_output(line->content, line->content_len);
}
static int git_open(git_repository **repo, const char *folder) {
    return git_repository_open_ext(repo, folder, 0, NULL);
}
static char *git_subdirectory(git_repository *repo, const char *folder) {
    const char *root = git_repository_workdir(repo);
    char *absolute = realpath(folder, NULL);
    if (!absolute || !root) { free(absolute); return NULL; }
    size_t n = strlen(root), f = strlen(absolute);
    char *sub = NULL;
    if (f + 1 == n && !strncmp(root, absolute, f)) sub = strdup("");
    else if (f >= n && !strncmp(root, absolute, n)) sub = strdup(absolute + n);
    free(absolute); return sub;
}
static int git_file_repo(git_repository **repo, const char *path, char **relative) {
    if (!path || !path[0]) return -1;
    char *parent = strdup(path);
    if (!parent) return -1;
    char *slash = strrchr(parent, '/');
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    if (slash) *slash = 0; else strcpy(parent, ".");
    if (!parent[0]) { free(parent); parent = strdup("/"); if (!parent) return -1; }
    int err = git_open(repo, parent);
    char *sub = !err ? git_subdirectory(*repo, parent) : NULL;
    free(parent);
    if (err < 0 || !sub) { free(sub); return -1; }
    size_t size = strlen(sub) + strlen(name) + 2;
    *relative = malloc(size);
    if (*relative) snprintf(*relative, size, "%s%s%s", sub, sub[0] ? "/" : "", name);
    free(sub); return *relative ? 0 : -1;
}
const char *os_git_revision(const char *path) {
    if (git_output_start(128) < 0) return "";
    git_repository *repo = NULL; git_object *head = NULL; char *rel = NULL;
    int err = git_file_repo(&repo, path, &rel);
    if (!err) err = git_revparse_single(&head, repo, "HEAD");
    if (!err) err = git_printf("%s", git_oid_tostr_s(git_object_id(head)));
    free(rel); git_object_free(head); git_repository_free(repo);
    return git_quiet_finish(err);
}
const char *os_git_base(const char *path, i32 max_bytes) {
    if (git_output_start(max_bytes) < 0) return "";
    git_repository *repo = NULL; git_object *head = NULL; git_tree *tree = NULL;
    git_tree_entry *entry = NULL; git_blob *blob = NULL; char *rel = NULL;
    int err = git_file_repo(&repo, path, &rel);
    if (!err) err = git_revparse_single(&head, repo, "HEAD^{tree}");
    tree = (git_tree *)head;
    if (!err) err = git_tree_entry_bypath(&entry, tree, rel);
    if (err == GIT_ENOTFOUND) { err = 0; git_error_clear(); }
    else if (!err) {
        err = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
        if (!err) {
            git_object_size_t size = git_blob_rawsize(blob);
            err = size > git_limit ? -1 : git_output(git_blob_rawcontent(blob), (size_t)size);
        }
    }
    free(rel); git_blob_free(blob); git_tree_entry_free(entry); git_object_free(head); git_repository_free(repo);
    return git_quiet_finish(err);
}
const char *os_git_diff_text(const char *before, const char *after, i32 max_bytes) {
    if (git_output_start(max_bytes) < 0) return "";
    git_patch *patch = NULL;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = 0; opts.interhunk_lines = 0;
    int err = git_patch_from_buffers(&patch, before, strlen(before), "before", after, strlen(after), "after", &opts);
    git_buf text = GIT_BUF_INIT;
    if (!err && patch) err = git_patch_to_buf(&text, patch);
    if (!err) err = git_output(text.ptr, text.size);
    git_buf_dispose(&text); git_patch_free(patch);
    return git_quiet_finish(err);
}
static const char *git_change_list(const char *folder, int kinds) {
    if (git_output_start(1048576) < 0) return "";
    git_repository *repo = NULL; git_status_list *list = NULL;
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    int err = git_open(&repo, folder);
    if (!err) err = git_status_list_new(&list, repo, &opts);
    char *sub = !err ? git_subdirectory(repo, folder) : NULL;
    if (!err && !sub) err = -1;
    size_t prefix = sub ? strlen(sub) : 0;
    for (size_t i = 0; !err && i < git_status_list_entrycount(list); ++i) {
        const git_status_entry *e = git_status_byindex(list, i);
        const git_diff_delta *d = e->index_to_workdir ? e->index_to_workdir : e->head_to_index;
        if (!d) continue;
        const char *p = d->new_file.path ? d->new_file.path : d->old_file.path;
        if (prefix && (strncmp(p, sub, prefix) || p[prefix] != '/')) continue;
        if (prefix) p += prefix + 1;
        char kind = (e->status & (GIT_STATUS_WT_DELETED | GIT_STATUS_INDEX_DELETED)) ? 'D' :
            (e->status & (GIT_STATUS_WT_NEW | GIT_STATUS_INDEX_NEW)) ? 'A' : 'M';
        err = kinds ? git_printf("%c %s\n", kind, p) : git_printf("%s\n", p);
    }
    free(sub); git_status_list_free(list); git_repository_free(repo);
    return git_quiet_finish(err);
}
const char *os_git_changes(const char *path) { return git_change_list(path, 0); }
const char *os_git_change_kinds(const char *path) { return git_change_list(path, 1); }

typedef struct git_connection {
    char endpoint[1024];
    int attempts;
} git_connection;
static void git_wipe(char *text) {
    if (text) { volatile char *p = text; size_t n = strlen(text); while (n--) *p++ = 0; free(text); }
}
static int git_credentials(git_credential **out, const char *url, const char *user, unsigned allowed, void *payload) {
    (void)url;
    git_connection *ctx = payload;
    if (allowed & GIT_CREDENTIAL_USERNAME) return git_credential_username_new(out, user ? user : "git");
    if (++ctx->attempts > 2 || !(allowed & GIT_CREDENTIAL_SSH_MEMORY)) {
        git_error_set_str(GIT_ERROR_SSH, "Import a supported SSH private key before connecting"); return GIT_EAUTH;
    }
    char *key = os_git_secret_copy("private-key");
    char *pass = os_git_secret_copy("passphrase");
    int err = key ? git_credential_ssh_key_memory_new(out, user ? user : "git", NULL, key, pass ? pass : "") : GIT_EAUTH;
    git_wipe(key); git_wipe(pass);
    return err;
}
static int git_certificate(git_cert *cert, int valid, const char *host, void *payload) {
    git_connection *ctx = payload;
    if (cert->cert_type == GIT_CERT_HOSTKEY_LIBSSH2) {
        git_cert_hostkey *key = (git_cert_hostkey *)cert;
        if (!(key->type & GIT_CERT_SSH_SHA256)) return GIT_ECERTIFICATE;
        char hash[52] = "SHA256:";
        EVP_EncodeBlock((unsigned char *)hash + 7, key->hash_sha256, 32);
        hash[50] = 0; // OpenSSH fingerprint notation omits Base64 padding.
        if (os_git_trust_host(ctx->endpoint[0] ? ctx->endpoint : host, hash)) return 0;
        git_error_set_str(GIT_ERROR_SSH, "SSH host key was not trusted or has changed");
        return GIT_ECERTIFICATE;
    }
    return valid ? 0 : GIT_ECERTIFICATE;
}
static int git_transfer(const git_indexer_progress *p, void *payload) {
    (void)payload;
    int percent = p->total_objects ? (int)(100ULL * p->received_objects / p->total_objects) : -1;
    __atomic_store_n(&git_percent, percent, __ATOMIC_RELAXED); return 0;
}
static int git_push_transfer(unsigned current, unsigned total, size_t bytes, void *payload) {
    (void)bytes; (void)payload;
    __atomic_store_n(&git_percent, total ? (int)(100ULL * current / total) : -1, __ATOMIC_RELAXED); return 0;
}
static int git_push_result(const char *ref, const char *status, void *payload) {
    (void)ref; (void)payload;
    if (!status) return 0;
    git_error_set_str(GIT_ERROR_NET, status); return -1;
}
static void git_callbacks(git_remote_callbacks *cb, git_connection *ctx, const char *url) {
    git_remote_init_callbacks(cb, GIT_REMOTE_CALLBACKS_VERSION);
    memset(ctx, 0, sizeof(*ctx));
    // Include the explicit port in the pinned endpoint. SCP-style URLs default
    // to port 22; libgit2 supplies the actual host to certificate_check.
    if (url) {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        const char *at = strchr(p, '@'); if (at) p = at + 1;
        const char *end = strchr(p, '/');
        if (!strstr(url, "://")) end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n < sizeof(ctx->endpoint) - 4) {
            memcpy(ctx->endpoint, p, n); ctx->endpoint[n] = 0;
            if (!strchr(ctx->endpoint, ':')) strcat(ctx->endpoint, ":22");
        }
    }
    cb->credentials = git_credentials; cb->certificate_check = git_certificate;
    cb->transfer_progress = git_transfer; cb->push_transfer_progress = git_push_transfer;
    cb->push_update_reference = git_push_result; cb->payload = ctx;
}

// The existing ns_lens commands use one literal argument per line. This is a
// deliberately bounded adapter, not a shell or a general Git CLI emulator.
typedef struct git_args { char *storage; char *v[8192]; int n; } git_args;
static int git_args_parse(git_args *a, const char *input) {
    memset(a, 0, sizeof(*a));
    if (!input || strlen(input) > 1048576) return -1;
    a->storage = strdup(input); if (!a->storage) return -1;
    char *save = NULL;
    for (char *s = strtok_r(a->storage, "\n", &save); s; s = strtok_r(NULL, "\n", &save)) {
        if (a->n == 8192) return -1;
        a->v[a->n++] = s;
    }
    return a->n ? 0 : -1;
}
static int git_has(git_args *a, const char *option) {
    for (int i = 1; i < a->n; ++i) if (!strcmp(a->v[i], option)) return 1;
    return 0;
}
static const char *git_option(git_args *a, const char *option) {
    for (int i = 1; i + 1 < a->n; ++i) if (!strcmp(a->v[i], option)) return a->v[i + 1];
    return NULL;
}
static const char *git_last(git_args *a) { return a->n > 1 ? a->v[a->n - 1] : "HEAD"; }
static int git_paths(git_args *a) {
    for (int i = 1; i < a->n; ++i) if (!strcmp(a->v[i], "--")) return i + 1;
    return a->n;
}
static int git_head(git_repository *repo, git_reference **head, git_commit **commit) {
    int err = git_repository_head(head, repo);
    if (!err) err = git_commit_lookup(commit, repo, git_reference_target(*head));
    return err;
}
static int git_list_refs(git_repository *repo, git_args *a) {
    const char *prefix = git_last(a);
    git_reference_iterator *it = NULL; git_reference *ref = NULL;
    int err = git_reference_iterator_new(&it, repo); if (err < 0) return err;
    int count = 0;
    while (!(err = git_reference_next(&ref, it))) {
        const char *name = git_reference_name(ref);
        if (!strncmp(name, prefix, strlen(prefix)) && count++ < 1024) {
            const char *target = git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC ? git_reference_symbolic_target(ref) : "";
            int remote = !strcmp(prefix, "refs/remotes");
            err = git_printf(remote ? "%s %s\n" : "%s\n", name + strlen(prefix) + 1,
                target[0] && !strncmp(target, "refs/remotes/", 13) ? target + 13 : target);
        }
        git_reference_free(ref); ref = NULL;
        if (err < 0) break;
    }
    if (err == GIT_ITEROVER) err = 0;
    git_reference_free(ref); git_reference_iterator_free(it); return err;
}
static int git_counts(git_repository *repo, git_args *a) {
    git_reference *head = NULL, *up = NULL; git_commit *commit = NULL;
    int err = git_head(repo, &head, &commit);
    if (!err && git_has(a, "--left-right")) {
        err = git_branch_upstream(&up, head);
        size_t ahead = 0, behind = 0;
        if (!err) err = git_graph_ahead_behind(&ahead, &behind, repo, git_commit_id(commit), git_reference_target(up));
        if (!err) err = git_printf("%zu\t%zu\n", ahead, behind);
    } else if (!err) {
        git_revwalk *walk = NULL; git_oid oid;
        err = git_revwalk_new(&walk, repo);
        if (!err) err = git_revwalk_push_head(walk);
        if (!err) err = git_revwalk_hide_glob(walk, "refs/remotes/*");
        size_t count = 0;
        if (!err) { while (!(err = git_revwalk_next(&oid, walk))) ++count; if (err == GIT_ITEROVER) err = git_printf("%zu\n", count); }
        git_revwalk_free(walk);
    }
    git_commit_free(commit); git_reference_free(head); git_reference_free(up); return err;
}
static int git_fetch_one(git_repository *repo, const char *name) {
    git_remote *remote = NULL; git_connection ctx;
    int err = git_remote_lookup(&remote, repo, name);
    git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
    if (!err) {
        git_callbacks(&opts.callbacks, &ctx, git_remote_url(remote));
        opts.prune = GIT_FETCH_PRUNE;
        err = git_remote_fetch(remote, NULL, &opts, NULL);
    }
    git_remote_free(remote); return err;
}
static int git_fetch_all(git_repository *repo, git_args *a) {
    if (!git_has(a, "--all")) return git_fetch_one(repo, "origin");
    git_strarray names = {0}; int err = git_remote_list(&names, repo);
    for (size_t i = 0; !err && i < names.count; ++i) err = git_fetch_one(repo, names.strings[i]);
    git_strarray_dispose(&names); return err;
}
static int git_push_head(git_repository *repo) {
    git_reference *head = NULL; git_commit *commit = NULL; git_remote *remote = NULL;
    git_config *config = NULL; git_buf remote_name = GIT_BUF_INIT;
    int err = git_head(repo, &head, &commit);
    const char *name = "origin";
    if (!err && !git_branch_upstream_remote(&remote_name, repo, git_reference_name(head))) name = remote_name.ptr;
    else git_error_clear();
    if (!err) err = git_remote_lookup(&remote, repo, name);
    char refspec[2048];
    if (!err && snprintf(refspec, sizeof(refspec), "%s:%s", git_reference_name(head), git_reference_name(head)) >= (int)sizeof(refspec)) err = -1;
    git_push_options opts = GIT_PUSH_OPTIONS_INIT; git_connection ctx;
    if (!err) {
        git_callbacks(&opts.callbacks, &ctx, git_remote_url(remote));
        char *specs[] = {refspec}; git_strarray specs_array = {specs, 1};
        err = git_remote_push(remote, &specs_array, &opts);
    }
    if (!err) {
        err = git_repository_config(&config, repo);
        char key[2048];
        if (!err) { snprintf(key, sizeof(key), "branch.%s.remote", git_reference_shorthand(head)); err = git_config_set_string(config, key, name); }
        if (!err) { snprintf(key, sizeof(key), "branch.%s.merge", git_reference_shorthand(head)); err = git_config_set_string(config, key, git_reference_name(head)); }
    }
    git_config_free(config); git_buf_dispose(&remote_name); git_commit_free(commit); git_reference_free(head); git_remote_free(remote); return err;
}
static int git_switch_branch(git_repository *repo, git_args *a) {
    const char *name = git_last(a);
    git_reference *ref = NULL, *head = NULL; git_commit *commit = NULL; git_object *target = NULL;
    int err;
    if (git_has(a, "-c")) {
        err = git_head(repo, &head, &commit);
        if (!err) err = git_branch_create(&ref, repo, name, commit, 0);
    } else if (git_has(a, "--track")) {
        git_reference *remote = NULL;
        err = git_branch_lookup(&remote, repo, name, GIT_BRANCH_REMOTE);
        if (!err) err = git_commit_lookup(&commit, repo, git_reference_target(remote));
        const char *short_name = strchr(name, '/'); short_name = short_name ? short_name + 1 : name;
        if (!err) err = git_branch_create(&ref, repo, short_name, commit, 0);
        if (!err) err = git_branch_set_upstream(ref, name);
        git_reference_free(remote);
    } else err = git_branch_lookup(&ref, repo, name, GIT_BRANCH_LOCAL);
    if (!err) err = git_reference_peel(&target, ref, GIT_OBJECT_COMMIT);
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = GIT_CHECKOUT_SAFE;
    if (!err) err = git_checkout_tree(repo, target, &opts);
    if (!err) err = git_repository_set_head(repo, git_reference_name(ref));
    git_object_free(target); git_commit_free(commit); git_reference_free(head); git_reference_free(ref); return err;
}
static int git_fast_forward(git_repository *repo, const char *name) {
    git_reference *head = NULL, *up = NULL, *updated = NULL; git_commit *commit = NULL;
    git_object *target = NULL;
    int err = git_head(repo, &head, &commit);
    if (!err) err = name ? git_revparse_single(&target, repo, name) : git_branch_upstream(&up, head);
    if (!err && up) err = git_reference_peel(&target, up, GIT_OBJECT_COMMIT);
    if (!err) {
        const git_oid *oid = git_object_id(target);
        if (git_oid_equal(oid, git_commit_id(commit))) goto done;
        int descendant = git_graph_descendant_of(repo, oid, git_commit_id(commit));
        if (descendant != 1) {
            git_error_set_str(GIT_ERROR_MERGE, "Branches diverged; resolve the rebase on a desktop before pulling"); err = -1; goto done;
        }
        git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
        opts.checkout_strategy = GIT_CHECKOUT_SAFE;
        err = git_checkout_tree(repo, target, &opts);
        if (!err) err = git_reference_set_target(&updated, head, oid, "ns_lens: fast-forward");
    }
done:
    git_reference_free(updated); git_object_free(target); git_commit_free(commit); git_reference_free(head); git_reference_free(up); return err;
}
static int git_delete_branch(git_repository *repo, const char *name) {
    git_reference *ref = NULL, *head = NULL; git_commit *commit = NULL;
    int err = git_branch_lookup(&ref, repo, name, GIT_BRANCH_LOCAL);
    if (!err) err = git_head(repo, &head, &commit);
    if (!err) {
        int merged = git_oid_equal(git_reference_target(ref), git_commit_id(commit)) || git_graph_descendant_of(repo, git_commit_id(commit), git_reference_target(ref)) == 1;
        if (!merged) { git_error_set_str(GIT_ERROR_REFERENCE, "Branch has unmerged changes"); err = -1; }
        else err = git_branch_delete(ref);
    }
    git_reference_free(ref); git_reference_free(head); git_commit_free(commit); return err;
}
static char *git_relative(git_repository *repo, const char *folder, const char *path) {
    char *sub = git_subdirectory(repo, folder);
    if (!sub) return NULL;
    size_t size = strlen(sub) + strlen(path) + 2;
    char *out = malloc(size);
    if (out) snprintf(out, size, "%s%s%s", sub, sub[0] ? "/" : "", path);
    free(sub); return out;
}
static int git_make_paths(git_strarray *paths, git_repository *repo, const char *folder, git_args *args, int literal) {
    int first = git_paths(args);
    size_t count = (size_t)(args->n - first);
    if (!count) count = 1;
    paths->strings = calloc(count, sizeof(char *)); paths->count = count;
    if (!paths->strings) return -1;
    for (size_t i = 0; i < count; ++i) {
        const char *p = first < args->n ? args->v[first + (int)i] : ".";
        char *relative = git_relative(repo, folder, p);
        if (!relative) return -1;
        if (!strcmp(relative, ".")) relative[0] = 0;
        size_t n = strlen(relative);
        if (n >= 2 && !strcmp(relative + n - 2, "/.")) relative[n - 2] = 0;
        if (!literal) {
            // Git index APIs accept patterns. Escape metacharacters in literal
            // ns_lens filenames; a directory selects its descendants.
            char *escaped = calloc(strlen(relative) * 2 + 2, 1);
            if (!escaped) { free(relative); return -1; }
            size_t used = 0;
            for (size_t k = 0; relative[k]; ++k) {
                if (strchr("*?[]\\", relative[k])) escaped[used++] = '\\';
                escaped[used++] = relative[k];
            }
            if (!relative[0]) escaped[used++] = '*';
            free(relative); relative = escaped;
        }
        paths->strings[i] = relative;
    }
    return 0;
}
static int git_stage(git_index *index, git_strarray *paths) {
    int err = git_index_update_all(index, paths, NULL, NULL);
    if (!err) err = git_index_add_all(index, paths, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
    return err;
}
static int git_commit_changes(git_repository *repo, const char *folder, git_args *a) {
    git_index *original = NULL, *index = NULL;
    git_tree *base = NULL, *tree = NULL; git_commit *head = NULL;
    git_signature *signature = NULL; git_oid tree_oid, commit_oid;
    git_strarray paths = {0}; git_pathspec *match = NULL;
    int amend = git_has(a, "--amend"), selected = git_paths(a) < a->n || git_has(a, "--only");
    int err = git_repository_index(&original, repo);
    if (!err) {
        int found = git_revparse_single((git_object **)&head, repo, "HEAD");
        if (found < 0 && found != GIT_ENOTFOUND && found != GIT_EUNBORNBRANCH) err = found;
        if (found < 0) git_error_clear();
    }
    if (!err && head) err = git_commit_tree(&base, head);
    if (!err && selected) {
        err = git_index_new(&index);
        if (!err) err = git_repository_set_index(repo, index);
        if (!err && base) err = git_index_read_tree(index, base);
        if (!err) err = git_make_paths(&paths, repo, folder, a, 0);
        if (!err && git_paths(a) < a->n) err = git_stage(index, &paths);
    } else if (!err) { index = original; }
    if (!err && git_index_has_conflicts(index)) { git_error_set_str(GIT_ERROR_INDEX, "Resolve index conflicts before committing"); err = -1; }
    if (!err) err = git_index_write_tree_to(&tree_oid, index, repo);
    if (!err && !amend && base && git_oid_equal(&tree_oid, git_tree_id(base))) {
        git_error_set_str(GIT_ERROR_INDEX, "Nothing to commit"); err = -1;
    }
    if (!err) err = git_tree_lookup(&tree, repo, &tree_oid);
    if (!err) err = git_signature_default(&signature, repo);
    const char *message = git_option(a, "-m");
    if (!err && amend && head) {
        err = git_commit_amend(&commit_oid, head, "HEAD", NULL, signature, NULL, message, tree);
    } else if (!err && message) {
        const git_commit *parents[] = {head};
        err = git_commit_create(&commit_oid, repo, "HEAD", signature, signature, NULL, message, tree, head ? 1 : 0, head ? parents : NULL);
    } else if (!err) { git_error_set_str(GIT_ERROR_INVALID, "Commit message is required"); err = -1; }
    if (selected && original) {
        // Restore the actual index, retaining unrelated staged entries.
        git_repository_set_index(repo, original);
        if (!err && git_paths(a) < a->n) {
            err = git_pathspec_new(&match, &paths);
            for (size_t i = git_index_entrycount(original); !err && i > 0; --i) {
                const git_index_entry *entry = git_index_get_byindex(original, i - 1);
                if (git_pathspec_matches_path(match, 0, entry->path)) {
                    char *path = strdup(entry->path);
                    err = path ? git_index_remove_bypath(original, path) : -1;
                    free(path);
                }
            }
            for (size_t i = 0; !err && i < git_index_entrycount(index); ++i) {
                const git_index_entry *entry = git_index_get_byindex(index, i);
                if (git_pathspec_matches_path(match, 0, entry->path)) err = git_index_add(original, entry);
            }
            if (!err) err = git_index_write(original);
        }
    } else if (!err) err = git_index_write(original);
    git_pathspec_free(match); git_strarray_dispose(&paths); git_signature_free(signature);
    git_tree_free(base); git_tree_free(tree); git_commit_free(head);
    if (selected) git_index_free(index);
    git_index_free(original); return err;
}
static void git_date(char *text, size_t size, const git_signature *sig, int detailed) {
    time_t seconds = (time_t)sig->when.time + sig->when.offset * 60;
    struct tm tm; gmtime_r(&seconds, &tm);
    strftime(text, size, detailed ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M", &tm);
    if (detailed) {
        int offset = sig->when.offset, abs_offset = offset < 0 ? -offset : offset;
        size_t used = strlen(text);
        snprintf(text + used, size - used, " %c%02d%02d", offset < 0 ? '-' : '+', abs_offset / 60, abs_offset % 60);
    }
}
static int git_commit_diff(git_diff **diff, git_repository *repo, git_commit *commit, const char *folder, git_args *a) {
    git_tree *tree = NULL, *parent_tree = NULL; git_commit *parent = NULL;
    int err = git_commit_tree(&tree, commit);
    if (!err && git_commit_parentcount(commit)) {
        err = git_commit_parent(&parent, commit, 0);
        if (!err) err = git_commit_tree(&parent_tree, parent);
    }
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = 3;
    opts.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH;
    git_strarray paths = {0};
    if (!err) err = git_make_paths(&paths, repo, folder, a, 1);
    if (!err) {
        opts.pathspec = paths;
        if (paths.count == 1 && !paths.strings[0][0]) opts.pathspec.count = 0;
        err = git_diff_tree_to_tree(diff, repo, parent_tree, tree, &opts);
    }
    git_strarray_dispose(&paths); git_tree_free(tree); git_tree_free(parent_tree); git_commit_free(parent); return err;
}
static const char *git_scope_path(git_repository *repo, const char *folder, const char *path) {
    const char *root = git_repository_workdir(repo);
    size_t r = strlen(root), f = strlen(folder);
    if (f >= r && !strncmp(root, folder, r)) {
        size_t n = f - r;
        if (n && !strncmp(path, folder + r, n) && path[n] == '/') return path + n + 1;
    }
    return path;
}
static int git_show_commit(git_repository *repo, const char *folder, git_args *a) {
    int first = git_paths(a);
    const char *hash = first < a->n ? a->v[first - 2] : git_last(a);
    git_commit *commit = NULL; git_diff *diff = NULL;
    int err = git_revparse_single((git_object **)&commit, repo, hash);
    if (!err && git_has(a, "-s")) {
        const git_signature *author = git_commit_author(commit), *committer = git_commit_committer(commit);
        char date[64], cdate[64]; git_date(date, sizeof(date), author, 1); git_date(cdate, sizeof(cdate), committer, 1);
        err = git_printf("%s\n%s <%s>\n%s\n%s <%s>\n%s\n", git_oid_tostr_s(git_commit_id(commit)), author->name, author->email, date, committer->name, committer->email, cdate);
        for (unsigned i = 0; !err && i < git_commit_parentcount(commit); ++i)
            err = git_printf("%s%s", i ? " " : "", git_oid_tostr_s(git_commit_parent_id(commit, i)));
        if (!err) err = git_printf("\n%s\n", git_commit_message(commit));
    } else if (!err) {
        err = git_commit_diff(&diff, repo, commit, folder, a);
        if (!err && (git_has(a, "--numstat") || git_has(a, "--name-status"))) {
            for (size_t i = 0; !err && i < git_diff_num_deltas(diff); ++i) {
                const git_diff_delta *d = git_diff_get_delta(diff, i);
                const char *path = git_scope_path(repo, folder, d->new_file.path);
                if (git_has(a, "--name-status")) {
                    char kind = d->status == GIT_DELTA_ADDED ? 'A' : d->status == GIT_DELTA_DELETED ? 'D' : 'M';
                    err = git_printf("%c\t%s\n", kind, path);
                } else {
                    git_patch *patch = NULL; size_t added = 0, deleted = 0, context = 0;
                    err = git_patch_from_diff(&patch, diff, i);
                    if (!err && patch) err = git_patch_line_stats(&context, &added, &deleted, patch);
                    if (!err) err = git_printf("%zu\t%zu\t%s\n", added, deleted, path);
                    git_patch_free(patch);
                }
            }
        } else if (!err) err = git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, git_patch_line, NULL);
    }
    git_diff_free(diff); git_commit_free(commit); return err;
}
static int git_log_commits(git_repository *repo, const char *folder, git_args *a) {
    git_revwalk *walk = NULL; git_oid oid;
    int err = git_revwalk_new(&walk, repo);
    if (!err) { git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME); err = git_revwalk_push_head(walk); }
    int skip = 0, max = 128, emitted = 0;
    const char *limit = git_option(a, "-n"); if (limit) max = atoi(limit);
    for (int i = 1; i < a->n; ++i) if (!strncmp(a->v[i], "--skip=", 7)) skip = atoi(a->v[i] + 7);
    while (!err && emitted < max && !(err = git_revwalk_next(&oid, walk))) {
        git_commit *commit = NULL; git_diff *diff = NULL;
        err = git_commit_lookup(&commit, repo, &oid);
        if (!err) err = git_commit_diff(&diff, repo, commit, folder, a);
        if (!err && git_diff_num_deltas(diff)) {
            if (skip > 0) --skip;
            else {
                const git_signature *sig = git_commit_author(commit);
                char date[64], hash[GIT_OID_HEXSZ + 1];
                git_date(date, sizeof(date), sig, 0); git_oid_tostr(hash, sizeof(hash), &oid);
                err = git_printf("%s\x1f%.7s\x1f%s\x1f%s\x1f%s\n", hash, hash, sig->name, date, git_commit_summary(commit)); ++emitted;
            }
        }
        git_diff_free(diff); git_commit_free(commit);
    }
    if (err == GIT_ITEROVER || err == GIT_EUNBORNBRANCH || err == GIT_ENOTFOUND) { err = 0; git_error_clear(); }
    git_revwalk_free(walk); return err;
}
static int git_execute(const char *folder, git_args *a) {
    // Strip only the global options emitted by ns_lens. Unknown commands fail.
    int first = 0;
    while (first < a->n) {
        if (!strcmp(a->v[first], "--literal-pathspecs")) ++first;
        else if (!strcmp(a->v[first], "-c") && first + 1 < a->n && !strcmp(a->v[first + 1], "core.quotePath=false")) first += 2;
        else break;
    }
    if (first) { memmove(a->v, a->v + first, (size_t)(a->n - first) * sizeof(char *)); a->n -= first; }
    if (!a->n) return -1;
    const char *command = a->v[0];
    git_repository *repo = NULL; int err = 0;
    // The commit identity shared by every repository: set it, or --get it
    // (status -1 when unset). Only the author keys are accepted.
    if (!strcmp(command, "config") && a->n >= 3 && !strcmp(a->v[1], "--global")) {
        int get = a->n == 4 && !strcmp(a->v[2], "--get");
        const char *key = get ? a->v[3] : a->v[2];
        if ((!get && a->n != 4) || (strcmp(key, "user.name") && strcmp(key, "user.email"))) {
            git_error_set_str(GIT_ERROR_INVALID, "Unsupported embedded Git command"); return -1;
        }
        git_config *config = NULL, *global = NULL; git_buf value = GIT_BUF_INIT;
        err = git_config_open_default(&config);
        if (!err) err = git_config_open_level(&global, config, GIT_CONFIG_LEVEL_GLOBAL);
        if (err == GIT_ENOTFOUND && !get) {
            // No global file yet: create it in the configured search path.
            git_buf path = GIT_BUF_INIT; err = git_config_find_global(&path);
            if (err == GIT_ENOTFOUND) {
                git_buf dir = GIT_BUF_INIT; char file[PATH_MAX];
                err = git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &dir);
                if (!err) { snprintf(file, sizeof(file), "%s/.gitconfig", dir.ptr); err = git_config_open_ondisk(&global, file); }
                git_buf_dispose(&dir);
            } else if (!err) err = git_config_open_ondisk(&global, path.ptr);
            git_buf_dispose(&path);
        }
        if (!err && get) err = git_config_get_string_buf(&value, global, key);
        else if (!err) err = git_config_set_string(global, key, a->v[3]);
        if (!err && get) err = git_printf("%s\n", value.ptr);
        git_buf_dispose(&value); git_config_free(global); git_config_free(config);
        return err;
    }
    if (!strcmp(command, "clone")) {
        if (a->n < 3) return -1;
        const char *url = a->v[a->n - 2], *destination = a->v[a->n - 1];
        // Never clone over an existing directory, including an empty one.
        struct stat existing;
        if (!lstat(destination, &existing)) { git_error_set_str(GIT_ERROR_FILESYSTEM, "Destination already exists; choose another folder name"); return -1; }
        git_clone_options opts = GIT_CLONE_OPTIONS_INIT; git_connection ctx;
        git_callbacks(&opts.fetch_opts.callbacks, &ctx, url);
        err = git_clone(&repo, url, destination, &opts);
    } else {
        err = git_open(&repo, folder);
        if (err < 0) goto done;
        if (!strcmp(command, "symbolic-ref")) {
            git_reference *head = NULL; err = git_repository_head(&head, repo);
            if (!err && git_repository_head_detached(repo)) err = -1;
            if (!err) err = git_printf("%s\n", git_reference_shorthand(head));
            git_reference_free(head);
        } else if (!strcmp(command, "rev-parse")) {
            git_object *object = NULL; err = git_revparse_single(&object, repo, "HEAD");
            if (!err) err = git_printf(git_has(a, "--short") ? "%.7s\n" : "%s\n", git_oid_tostr_s(git_object_id(object)));
            git_object_free(object);
        } else if (!strcmp(command, "rev-list")) err = git_counts(repo, a);
        else if (!strcmp(command, "for-each-ref")) err = git_list_refs(repo, a);
        else if (!strcmp(command, "fetch")) err = git_fetch_all(repo, a);
        else if (!strcmp(command, "push")) err = git_push_head(repo);
        else if (!strcmp(command, "pull")) err = git_fast_forward(repo, NULL);
        else if (!strcmp(command, "merge")) err = git_fast_forward(repo, git_last(a));
        else if (!strcmp(command, "switch")) err = git_switch_branch(repo, a);
        else if (!strcmp(command, "branch") && git_has(a, "-d")) err = git_delete_branch(repo, git_last(a));
        else if (!strcmp(command, "commit")) err = git_commit_changes(repo, folder, a);
        else if (!strcmp(command, "add")) {
            git_index *index = NULL; git_strarray paths = {0};
            err = git_repository_index(&index, repo);
            if (!err) err = git_make_paths(&paths, repo, folder, a, 0);
            if (!err) err = git_stage(index, &paths);
            if (!err) err = git_index_write(index);
            git_strarray_dispose(&paths); git_index_free(index);
        } else if (!strcmp(command, "show")) err = git_show_commit(repo, folder, a);
        else if (!strcmp(command, "log")) err = git_log_commits(repo, folder, a);
        else if (!strcmp(command, "config") && a->n == 4 && !strcmp(a->v[1], "--local") &&
            (!strcmp(a->v[2], "user.name") || !strcmp(a->v[2], "user.email") ||
             (!strcmp(a->v[2], "--unset") && (!strcmp(a->v[3], "user.name") || !strcmp(a->v[3], "user.email"))))) {
            git_config *config = NULL, *local = NULL; err = git_repository_config(&config, repo);
            if (!err) err = git_config_open_level(&local, config, GIT_CONFIG_LEVEL_LOCAL);
            if (!err && !strcmp(a->v[2], "--unset")) {
                err = git_config_delete_entry(local, a->v[3]);
                if (err == GIT_ENOTFOUND) { git_error_clear(); err = 0; } // already unset
            } else if (!err) err = git_config_set_string(local, a->v[2], a->v[3]);
            git_config_free(local); git_config_free(config);
        } else { git_error_set_str(GIT_ERROR_INVALID, "Unsupported embedded Git command"); err = -1; }
    }
done:
    git_repository_free(repo); return err;
}
const char *os_git_run(const char *folder, const char *args) {
    if (git_output_start(65536) < 0) return "";
    git_error_clear();
    __atomic_store_n(&git_percent, -1, __ATOMIC_RELAXED);
    git_args parsed;
    int err = git_args_parse(&parsed, args);
    pthread_mutex_lock(&git_write_lock);
    char *absolute = folder ? realpath(folder, NULL) : NULL;
    if (!err) err = absolute ? git_execute(absolute, &parsed) : -1;
    free(absolute);
    const char *result = git_finish(err);
    pthread_mutex_unlock(&git_write_lock);
    free(parsed.storage); return result;
}
#endif
#ifdef NS_GIT_EMBEDDED
#include <openssl/pem.h>
#include <openssl/evp.h>
static int git_pem_password(char *buffer, int size, int writing, void *payload) {
    (void)writing; const char *pass = payload; size_t n = strlen(pass);
    if (n > (size_t)size) return 0;
    memcpy(buffer, pass, n); return (int)n;
}
int os_git_key_valid(const char *key, const char *passphrase) {
    const char *begin = "-----BEGIN OPENSSH PRIVATE KEY-----";
    if (!strncmp(key, begin, strlen(begin))) {
        const char *end = strstr(key, "-----END OPENSSH PRIVATE KEY-----");
        if (!end) return 0;
        size_t n = (size_t)(end - key), used = 0;
        char *base64 = malloc(n + 1); unsigned char *decoded = malloc(n + 1);
        if (!base64 || !decoded) { free(base64); free(decoded); return 0; }
        for (const char *p = key + strlen(begin); p < end; ++p) {
            if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') continue;
            base64[used++] = *p;
        }
        base64[used] = 0;
        int size = used % 4 ? -1 : EVP_DecodeBlock(decoded, (unsigned char *)base64, (int)used);
        // OpenSSH encrypted keys are fully checked by libssh2 at connect time.
        int valid = size >= 32 && !memcmp(decoded, "openssh-key-v1\0", 15);
        OPENSSL_cleanse(decoded, n + 1); free(decoded); git_wipe(base64); return valid;
    }
    BIO *bio = BIO_new_mem_buf(key, -1);
    EVP_PKEY *pkey = bio ? PEM_read_bio_PrivateKey(bio, NULL, git_pem_password, (void *)passphrase) : NULL;
    int valid = pkey != NULL;
    EVP_PKEY_free(pkey); BIO_free(bio); return valid;
}

// ---- generated identity ------------------------------------------------------
#include <openssl/rand.h>
typedef struct git_bytes { unsigned char *p; size_t n, cap; } git_bytes;
static int git_put(git_bytes *b, const void *data, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n) cap *= 2;
        unsigned char *grown = malloc(cap);
        if (!grown) return 0;
        if (b->p) { memcpy(grown, b->p, b->n); OPENSSL_cleanse(b->p, b->cap); free(b->p); }
        b->p = grown; b->cap = cap;
    }
    memcpy(b->p + b->n, data, n); b->n += n; return 1;
}
static int git_put_u32(git_bytes *b, uint32_t v) {
    unsigned char be[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
    return git_put(b, be, 4);
}
static int git_put_string(git_bytes *b, const void *data, size_t n) { return git_put_u32(b, (uint32_t)n) && git_put(b, data, n); }
static void git_bytes_free(git_bytes *b) { if (b->p) { OPENSSL_cleanse(b->p, b->cap); free(b->p); } memset(b, 0, sizeof(*b)); }
static char *git_base64(const unsigned char *data, size_t n) {
    char *out = malloc(4 * ((n + 2) / 3) + 1);
    if (out) EVP_EncodeBlock((unsigned char *)out, data, (int)n);
    return out;
}
static char *git_public_line(const unsigned char *blob, size_t n, const char *comment) {
    // The blob starts with its own key type string.
    if (n < 4) return NULL;
    uint32_t type_n = (uint32_t)blob[0] << 24 | (uint32_t)blob[1] << 16 | (uint32_t)blob[2] << 8 | blob[3];
    if (type_n > n - 4 || type_n > 64) return NULL;
    char *encoded = git_base64(blob, n);
    if (!encoded) return NULL;
    size_t size = type_n + strlen(encoded) + strlen(comment) + 4;
    char *line = malloc(size);
    if (line) snprintf(line, size, "%.*s %s%s%s", (int)type_n, (const char *)blob + 4, encoded, comment[0] ? " " : "", comment);
    free(encoded); return line;
}

// A new unencrypted ed25519 key in OpenSSH's own format, the one ssh-keygen
// writes. Returns the private key text (wipe and free it); *public_line gets
// the authorized_keys line.
char *os_git_key_generate(const char *comment, char **public_line) {
    *public_line = NULL;
    if (!comment || strlen(comment) > 200 || strchr(comment, '\n')) return NULL;
    unsigned char seed[32], pub[32], check[4];
    size_t seed_n = sizeof(seed), pub_n = sizeof(pub);
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    int ok = ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &pkey) == 1 &&
             EVP_PKEY_get_raw_private_key(pkey, seed, &seed_n) == 1 && EVP_PKEY_get_raw_public_key(pkey, pub, &pub_n) == 1 &&
             seed_n == 32 && pub_n == 32 && RAND_bytes(check, sizeof(check)) == 1;
    EVP_PKEY_free(pkey); EVP_PKEY_CTX_free(ctx);
    git_bytes blob = {0}, secret = {0}, file = {0};
    static const char type[] = "ssh-ed25519";
    ok = ok && git_put_string(&blob, type, sizeof(type) - 1) && git_put_string(&blob, pub, 32);
    // The private section repeats a random check word and pads to the
    // 8-byte block size of the "none" cipher with 1, 2, 3, ...
    unsigned char sk[64];
    memcpy(sk, seed, 32); memcpy(sk + 32, pub, 32);
    ok = ok && git_put(&secret, check, 4) && git_put(&secret, check, 4) && git_put_string(&secret, type, sizeof(type) - 1) &&
         git_put_string(&secret, pub, 32) && git_put_string(&secret, sk, 64) && git_put_string(&secret, comment, strlen(comment));
    for (unsigned char pad = 1; ok && secret.n % 8; ++pad) ok = git_put(&secret, &pad, 1);
    ok = ok && git_put(&file, "openssh-key-v1", 15) && git_put_string(&file, "none", 4) && git_put_string(&file, "none", 4) &&
         git_put_string(&file, "", 0) && git_put_u32(&file, 1) && git_put_string(&file, blob.p, blob.n) &&
         git_put_string(&file, secret.p, secret.n);
    OPENSSL_cleanse(seed, sizeof(seed)); OPENSSL_cleanse(sk, sizeof(sk));
    char *text = NULL;
    char *encoded = ok ? git_base64(file.p, file.n) : NULL;
    if (encoded) {
        size_t n = strlen(encoded), size = n + n / 70 + 80, used = 0;
        text = malloc(size);
        if (text) {
            used += (size_t)snprintf(text, size, "-----BEGIN OPENSSH PRIVATE KEY-----\n");
            for (size_t i = 0; i < n; i += 70) used += (size_t)snprintf(text + used, size - used, "%.70s\n", encoded + i);
            snprintf(text + used, size - used, "-----END OPENSSH PRIVATE KEY-----\n");
        }
        git_wipe(encoded);
    }
    if (text) *public_line = git_public_line(blob.p, blob.n, comment);
    if (text && !*public_line) { git_wipe(text); text = NULL; }
    git_bytes_free(&blob); git_bytes_free(&secret); git_bytes_free(&file);
    return text;
}

// The public half of an OpenSSH private key, read from its unencrypted header
// (encrypted keys keep it in the clear too). PEM keys return NULL.
char *os_git_key_public(const char *key) {
    const char *begin = "-----BEGIN OPENSSH PRIVATE KEY-----";
    if (!key || strncmp(key, begin, strlen(begin))) return NULL;
    const char *end = strstr(key, "-----END OPENSSH PRIVATE KEY-----");
    if (!end) return NULL;
    size_t n = (size_t)(end - key), used = 0;
    char *base64 = malloc(n + 1); unsigned char *decoded = malloc(n + 1);
    char *line = NULL;
    if (base64 && decoded) {
        for (const char *p = key + strlen(begin); p < end; ++p) if (*p > ' ') base64[used++] = *p;
        base64[used] = 0;
        int size = used % 4 ? -1 : EVP_DecodeBlock(decoded, (unsigned char *)base64, (int)used);
        // magic, cipher, kdf, kdf options, key count, then the public blob.
        size_t at = 15;
        int fields = 0;
        if (size >= 15 && !memcmp(decoded, "openssh-key-v1\0", 15)) {
            for (; fields < 3 && at + 4 <= (size_t)size; ++fields) {
                uint32_t len = (uint32_t)decoded[at] << 24 | (uint32_t)decoded[at + 1] << 16 | (uint32_t)decoded[at + 2] << 8 | decoded[at + 3];
                if (len > (size_t)size - at - 4) break;
                at += 4 + len;
            }
            if (fields == 3 && at + 8 <= (size_t)size) {
                at += 4;
                uint32_t len = (uint32_t)decoded[at] << 24 | (uint32_t)decoded[at + 1] << 16 | (uint32_t)decoded[at + 2] << 8 | decoded[at + 3];
                if (len <= (size_t)size - at - 4) line = git_public_line(decoded + at + 4, len, "ns-lens");
            }
        }
    }
    if (decoded) { OPENSSL_cleanse(decoded, n + 1); free(decoded); }
    git_wipe(base64);
    return line;
}

static char *git_public_text;
ns_bool os_git_identity_exists(void) {
    char *key = os_git_secret_copy("private-key");
    ns_bool exists = key && key[0];
    git_wipe(key); return exists;
}
ns_bool os_git_identity_generate(const char *comment) {
    if (os_git_identity_exists()) return false; // never replace a key silently
    char *public_line = NULL;
    char *key = os_git_key_generate(comment && comment[0] ? comment : "ns-lens", &public_line);
    ns_bool ok = key && os_git_identity_import(key, "");
    git_wipe(key); free(public_line); return ok;
}
const char *os_git_identity_public(void) {
    char *key = os_git_secret_copy("private-key");
    free(git_public_text);
    git_public_text = os_git_key_public(key);
    git_wipe(key);
    return git_public_text ? git_public_text : "";
}
#endif
