// Run with scripts/test_embedded_git.sh. Uses the actual mobile backend on the
// host; all credentials/trust are denied and all repositories are temporary.
#include "os.h"
#include <git2.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
char *os_git_secret_copy(const char *name) { (void)name; return NULL; }
int os_git_trust_host(const char *endpoint, const char *hash) { (void)endpoint; (void)hash; return 0; }
ns_bool os_git_identity_import(const char *key, const char *passphrase) { (void)key; (void)passphrase; return false; }
static void write_text(const char *root, const char *name, const char *text) {
    char path[2048]; snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(text, 1, strlen(text), f) == strlen(text)); fclose(f);
}
static void check_index(const char *root, const char *path, const char *expected) {
    git_repository *repo = NULL; git_index *index = NULL; git_blob *blob = NULL;
    assert(git_repository_open(&repo, root) == 0);
    assert(git_repository_index(&index, repo) == 0);
    const git_index_entry *entry = git_index_get_bypath(index, path, 0); assert(entry);
    assert(git_blob_lookup(&blob, repo, &entry->id) == 0);
    assert(git_blob_rawsize(blob) == strlen(expected));
    assert(!memcmp(git_blob_rawcontent(blob), expected, strlen(expected)));
    git_blob_free(blob); git_index_free(index); git_repository_free(repo);
}
static const char *run(const char *root, const char *args) {
    const char *text = os_git_run(root, args);
    if (os_git_status()) fprintf(stderr, "failed %s: %s\n", args, text);
    assert(os_git_status() == 0); return text;
}
int main(void) {
    char root[] = "/tmp/ns-git-test-XXXXXX"; assert(mkdtemp(root));
    // A private $HOME holds the global config under Library/Application Support/Git.
    char home[2048]; snprintf(home, sizeof(home), "%s-home", root);
    assert(mkdir(home, 0700) == 0); setenv("HOME", home, 1);
    char library[2048]; snprintf(library, sizeof(library), "%s/Library", home); assert(mkdir(library, 0700) == 0);
    snprintf(library, sizeof(library), "%s/Library/Application Support", home); assert(mkdir(library, 0700) == 0);
    git_libgit2_init(); git_repository *repo = NULL;
    assert(git_libgit2_features() & GIT_FEATURE_SSH);
    git_credential *credential = NULL;
    assert(git_credential_ssh_key_memory_new(&credential, "git", NULL, "test", "") == 0);
    git_credential_free(credential);
    assert(!strcmp(os_git_diff_text("same", "same", 1024), ""));
    assert(os_git_status() == 0);
    assert(git_repository_init(&repo, root, 0) == 0); git_repository_free(repo);
    os_git_run(root, "config\n--global\n--get\nuser.name"); assert(os_git_status() < 0);
    run(root, "config\n--global\nuser.name\nLens Test");
    run(root, "config\n--global\nuser.email\nlens@example.test");
    assert(!strcmp(run(root, "config\n--global\n--get\nuser.email"), "lens@example.test\n"));
    os_git_run(root, "config\n--global\ncore.editor\nvi"); assert(os_git_status() < 0);
    char global[2048]; snprintf(global, sizeof(global), "%s/Library/Application Support/Git/.gitconfig", home);
    struct stat global_stat; assert(stat(global, &global_stat) == 0);
    // A local identity overrides the global one until it is unset.
    run(root, "config\n--local\nuser.name\nOld Local");
    run(root, "config\n--local\n--unset\nuser.name");
    run(root, "config\n--local\n--unset\nuser.name");
    write_text(root, "hello.ns", "old\n"); write_text(root, "other.ns", "other\n");
    assert(strstr(os_git_change_kinds(root), "A hello.ns"));
    run(root, "add\n-A\n--\n."); run(root, "commit\n-q\n-m\ninitial\n--\n.");
    assert(!strcmp(os_git_changes(root), "")); assert(os_git_status() == 0);
    char path[2048]; snprintf(path, sizeof(path), "%s/hello.ns", root);
    assert(strlen(os_git_revision(path)) == 40);
    assert(!strcmp(os_git_base(path, 1024), "old\n"));
    assert(strstr(os_git_diff_text("old\n", "new\n", 1024), "@@ -1 +1 @@"));
    write_text(root, "hello.ns", "new\n"); write_text(root, "other.ns", "staged\n");
    run(root, "add\n-A\n--\nother.ns");
    write_text(root, "other.ns", "unstaged on top of staging\n");
    run(root, "--literal-pathspecs\ncommit\n-q\n-m\npartial\n--\nhello.ns");
    assert(!strcmp(os_git_base(path, 1024), "new\n"));
    check_index(root, "other.ns", "staged\n");
    assert(strstr(os_git_change_kinds(root), "M other.ns"));
    assert(!strstr(os_git_change_kinds(root), "hello.ns"));
    const char *log = run(root, "log\n-n\n10\n--skip=0\n--\n."); assert(strstr(log, "partial")); assert(strstr(log, "initial"));
    assert(strstr(run(root, "show\n-s\nHEAD"), "Lens Test <lens@example.test>"));
    assert(strstr(run(root, "show\n--numstat\nHEAD"), "1\t1\thello.ns"));
    assert(strstr(run(root, "show\n--name-status\nHEAD"), "M\thello.ns"));
    assert(strstr(run(root, "show\nHEAD\n--\nhello.ns"), "+new"));
    run(root, "switch\n-c\nfeature/test");
    assert(!strcmp(run(root, "symbolic-ref\n--short\n-q\nHEAD"), "feature/test\n"));
    assert(strstr(run(root, "for-each-ref\nrefs/heads"), "feature/test"));
    run(root, "commit\n-q\n--amend\n--only\n-m\nreworded");
    assert(strstr(run(root, "show\n-s\nHEAD"), "reworded"));
    // Staged unrelated content must survive both partial commit and amend.
    assert(strstr(os_git_change_kinds(root), "M other.ns"));
    check_index(root, "other.ns", "staged\n");
    assert(!strcmp(os_git_base(path, 2), "") || os_git_status() < 0);
    os_git_run(root, "not-a-command"); assert(os_git_status() < 0);
    os_git_run(root, "push\n--progress"); assert(os_git_status() < 0);
    char clone[2048], args[4096]; snprintf(clone, sizeof(clone), "%s-clone", root);
    snprintf(args, sizeof(args), "clone\n--progress\n--\n%s\n%s", root, clone);
    run(root, args);
    snprintf(path, sizeof(path), "%s/hello.ns", clone);
    assert(!strcmp(os_git_base(path, 1024), "new\n"));
    assert(os_git_embedded());
    char sub[2048]; snprintf(sub, sizeof(sub), "%s/sub", root); assert(mkdir(sub, 0700) == 0);
    write_text(sub, "nested.ns", "nested\n");
    run(root, "add\n-A\n--\nsub");
    run(root, "commit\n-m\nsubdir commit\n--\nsub");
    assert(strstr(run(sub, "log\n-n\n10\n--\n."), "subdir commit"));
    assert(!strstr(run(sub, "log\n-n\n10\n--\n."), "initial"));
    write_text(sub, "nested.ns", "changed\n");
    assert(strstr(os_git_change_kinds(sub), "M nested.ns"));
    assert(!strstr(os_git_change_kinds(sub), "other.ns"));
    write_text(root, "[draft].ns", "literal\n");
    run(root, "--literal-pathspecs\nadd\n-A\n--\n[draft].ns");
    run(root, "--literal-pathspecs\ncommit\n-m\nliteral filename\n--\n[draft].ns");
    check_index(root, "other.ns", "staged\n");
    // Refuse an existing destination, even when empty.
    os_git_run(root, args); assert(os_git_status() < 0);
    char missing[2048]; snprintf(missing, sizeof(missing), "%s/not-tracked.ns", root);
    assert(!strcmp(os_git_base(missing, 1024), "")); assert(os_git_status() == 0);
    puts("embedded Git tests passed");
    git_libgit2_shutdown(); return 0;
}
