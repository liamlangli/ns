// Run with scripts/test_embedded_git.sh. Drives the mobile SSH terminal
// backend against a throwaway sshd on loopback: NS_SSH_TEST_PORT, the client
// key NS_SSH_TEST_KEY and the expected host fingerprint NS_SSH_TEST_HOST.
#include "os.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static char *test_key;
static int test_trust = 1;
static char test_seen[64];
char *os_git_secret_copy(const char *name) {
    if (!strcmp(name, "private-key")) return test_key ? strdup(test_key) : NULL;
    return strdup("");
}
ns_bool os_git_identity_import(const char *key, const char *passphrase) {
    (void)passphrase;
    free(test_key); test_key = strdup(key); return true;
}
int os_git_trust_host(const char *endpoint, const char *hash) {
    assert(strstr(endpoint, "127.0.0.1:"));
    snprintf(test_seen, sizeof(test_seen), "%s", hash);
    return test_trust;
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    char *text = calloc(1, 65536); size_t n = fread(text, 1, 65535, f); fclose(f);
    text[n] = 0; return text;
}
static void pause_ms(int ms) { struct timespec t = {0, ms * 1000000L}; nanosleep(&t, NULL); }
// Collect output until `want` appears or the session ends.
static char output[1 << 20];
static int wait_for(i32 h, const char *want, int seconds) {
    size_t used = strlen(output);
    for (int i = 0; i < seconds * 100; ++i) {
        const char *chunk = os_ssh_read(h, 65536);
        size_t n = strlen(chunk);
        if (n && used + n < sizeof(output)) { memcpy(output + used, chunk, n + 1); used += n; }
        if (want && strstr(output, want)) return 1;
        if (!n && os_ssh_state(h) <= 0) return want == NULL;
        pause_ms(10);
    }
    return 0;
}
static int wait_state(i32 h, i32 state, int seconds) {
    for (int i = 0; i < seconds * 100; ++i) {
        if (os_ssh_state(h) == state) return 1;
        if (os_ssh_state(h) < 0 && state >= 0) return 0;
        os_ssh_read(h, 65536);
        pause_ms(10);
    }
    return 0;
}
int main(void) {
    const char *port_text = getenv("NS_SSH_TEST_PORT");
    if (!port_text) { puts("embedded SSH tests skipped"); return 0; }
    int port = atoi(port_text);
    test_key = read_file(getenv("NS_SSH_TEST_KEY"));
    const char *host_hash = getenv("NS_SSH_TEST_HOST");
    char folder[] = "/tmp/ns ssh 'dir'-XXXXXX"; assert(mkdtemp(folder));

    // Shell in a quoted folder, input, resize, then a clean exit status.
    output[0] = 0;
    i32 h = os_ssh_open("127.0.0.1", port, getenv("USER"), folder, 100, 30);
    assert(h > 0);
    assert(os_ssh_write(h, "echo ready-$((40+2))\n") > 0); // queued while connecting
    assert(wait_state(h, 3, 20));
    assert(!strcmp(test_seen, host_hash));
    assert(wait_for(h, "ready-42", 20));
    output[0] = 0;
    os_ssh_write(h, "pwd; stty size\n");
    assert(wait_for(h, "30 100", 20));
    assert(strstr(output, "ssh 'dir'-"));
    os_ssh_resize(h, 132, 43);
    output[0] = 0;
    os_ssh_write(h, "stty size\n");
    assert(wait_for(h, "43 132", 20));
    // A burst bigger than one 16 KiB chunk arrives whole and in order.
    output[0] = 0;
    os_ssh_write(h, "seq 1 30000; echo done-$((1))\n");
    assert(wait_for(h, "done-1", 20));
    assert(strstr(output, "\r\n29999\r\n30000\r\n"));
    output[0] = 0;
    os_ssh_write(h, "exit 7\n");
    assert(wait_for(h, NULL, 20));
    assert(os_ssh_state(h) == 0);
    assert(strstr(os_ssh_error(h), "status 7"));
    assert(os_ssh_write(h, "late\n") < 0);
    os_ssh_close(h);
    assert(os_ssh_state(h) < 0); // a closed handle is gone

    // Refused host trust and a missing key fail without opening a shell.
    test_trust = 0;
    h = os_ssh_open("127.0.0.1", port, getenv("USER"), "", 80, 24);
    assert(h > 0 && !wait_state(h, 3, 20));
    assert(strstr(os_ssh_error(h), "not trusted"));
    os_ssh_close(h);
    test_trust = 1;
    char *key = test_key; test_key = NULL;
    h = os_ssh_open("127.0.0.1", port, getenv("USER"), "", 80, 24);
    assert(h > 0 && !wait_state(h, 3, 20));
    assert(strstr(os_ssh_error(h), "Import an SSH private key"));
    os_ssh_close(h);
    test_key = key;
    h = os_ssh_open("127.0.0.1", port, "ns-no-such-user", "", 80, 24);
    assert(h > 0 && !wait_state(h, 3, 20));
    assert(strstr(os_ssh_error(h), "not accepted"));
    os_ssh_close(h);
    // Nothing listens on port 1.
    h = os_ssh_open("127.0.0.1", 1, getenv("USER"), "", 80, 24);
    assert(h > 0 && !wait_state(h, 3, 20));
    assert(strstr(os_ssh_error(h), "Cannot connect"));
    os_ssh_close(h);
    assert(os_ssh_open("", port, "u", "", 80, 24) == 0);
    assert(os_ssh_open("bad host", port, "u", "", 80, 24) == 0);

    // A generated key: ssh-keygen reads the same public key back, and the
    // server accepts it once its line is authorized.
    free(key); test_key = NULL;
    assert(!os_git_identity_exists());
    assert(os_git_identity_generate("ns-lens-test"));
    assert(os_git_identity_exists());
    assert(!os_git_identity_generate("again")); // never replaces a key
    char line[512];
    snprintf(line, sizeof(line), "%s", os_git_identity_public());
    assert(!strncmp(line, "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5", 32) && strstr(line, " ns-lens"));
    char path[] = "/tmp/ns-ssh-key-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    assert(write(fd, test_key, strlen(test_key)) == (ssize_t)strlen(test_key)); close(fd);
    char command[256], derived[512] = {0};
    snprintf(command, sizeof(command), "ssh-keygen -y -f %s", path);
    FILE *p = popen(command, "r"); assert(p);
    assert(fgets(derived, sizeof(derived), p)); pclose(p); unlink(path);
    size_t blob = strchr(line + 12, ' ') - line;
    assert(!strncmp(derived, line, blob));
    FILE *auth = fopen(getenv("NS_SSH_TEST_AUTH"), "a"); assert(auth);
    fprintf(auth, "%s\n", line); fclose(auth);
    output[0] = 0;
    h = os_ssh_open("127.0.0.1", port, getenv("USER"), "", 80, 24);
    assert(h > 0 && wait_state(h, 3, 20));
    os_ssh_write(h, "echo generated-$((6*7)); exit\n");
    assert(wait_for(h, "generated-42", 20));
    os_ssh_close(h);

    // Closing mid-session releases the slot; every slot can be reused.
    for (int i = 0; i < 12; ++i) {
        h = 0;
        for (int tries = 0; tries < 500 && h <= 0; ++tries) {
            h = os_ssh_open("127.0.0.1", port, getenv("USER"), "", 80, 24);
            if (h <= 0) pause_ms(10);
        }
        assert(h > 0);
        if (i == 0) assert(wait_state(h, 3, 20));
        os_ssh_close(h);
        pause_ms(50);
    }
    pause_ms(500);
    rmdir(folder);
    free(test_key);
    puts("embedded SSH tests passed");
    return 0;
}
