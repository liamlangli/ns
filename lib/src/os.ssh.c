// Remote SSH terminals for sandboxed apps that cannot start a local shell.
// Shares the embedded libssh2/OpenSSL with os.git.c, together with its
// Keychain identity and its SHA-256 host pins. One detached worker thread per
// session owns every libssh2 call; the app thread only exchanges bytes with it
// through two bounded buffers, so no network wait ever blocks a UI frame.
#ifdef NS_GIT_EMBEDDED
#include "os.h"
#include <libssh2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

extern char *os_git_secret_copy(const char *name);
extern int os_git_trust_host(const char *endpoint, const char *sha256);

#define OS_SSH_SLOTS 8
#define OS_SSH_IN_MAX (256 * 1024)
#define OS_SSH_OUT_MAX (1024 * 1024)
#define OS_SSH_READ_MAX 65536
#define OS_SSH_CHUNK 16384
#define OS_SSH_CONNECT_SECONDS 15
#define OS_SSH_LOGIN_SECONDS 30
#define OS_SSH_CANCELLED -1000
#define OS_SSH_TIMEOUT -1001

enum { SSH_CLOSED = 0, SSH_CONNECTING = 1, SSH_VERIFYING = 2, SSH_OPEN = 3, SSH_FAILED = -1 };

typedef struct os_ssh {
    int used, app_open, worker_live, cancel, resize;
    unsigned generation;
    int wake[2];
    i32 state;
    char error[256];
    char host[256], user[128], folder[1024];
    int port, cols, rows;
    unsigned char *in, *out;
    size_t in_used, out_used;
} os_ssh;

static os_ssh ssh_slots[OS_SSH_SLOTS];
static pthread_mutex_t ssh_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t ssh_once = PTHREAD_ONCE_INIT;
// Returned to the app thread; valid until its next os_ssh_read/os_ssh_error.
static char ssh_read_text[OS_SSH_READ_MAX + 1];
static char ssh_error_text[256];

static void ssh_start(void) { libssh2_init(0); }

static f64 ssh_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (f64)t.tv_sec + (f64)t.tv_nsec * 1e-9;
}

static void ssh_wake(os_ssh *s) {
    char byte = 1;
    if (s->wake[1] >= 0) (void)!write(s->wake[1], &byte, 1);
}

// Caller holds ssh_lock. A slot is reused only after both the app and the
// worker have let go of it.
static void ssh_release(os_ssh *s) {
    if (s->wake[0] >= 0) close(s->wake[0]);
    if (s->wake[1] >= 0) close(s->wake[1]);
    if (s->in) OPENSSL_cleanse(s->in, OS_SSH_IN_MAX);
    free(s->in); free(s->out);
    unsigned generation = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = generation + 1;
    s->wake[0] = s->wake[1] = -1;
}

static os_ssh *ssh_find(i32 handle) {
    if (handle <= 0) return NULL;
    unsigned index = (unsigned)(handle - 1) % OS_SSH_SLOTS;
    unsigned generation = (unsigned)(handle - 1) / OS_SSH_SLOTS;
    os_ssh *s = &ssh_slots[index];
    return s->used && s->app_open && s->generation == generation ? s : NULL;
}

static int ssh_cancelled(os_ssh *s) {
    pthread_mutex_lock(&ssh_lock);
    int cancel = s->cancel;
    pthread_mutex_unlock(&ssh_lock);
    return cancel;
}

static void ssh_set_state(os_ssh *s, i32 state) {
    pthread_mutex_lock(&ssh_lock);
    if (s->state > 0) s->state = state;
    pthread_mutex_unlock(&ssh_lock);
}

// The first failure wins; pending input is dropped so nothing typed for a dead
// connection is sent to a later one.
static void ssh_fail(os_ssh *s, LIBSSH2_SESSION *session, const char *what) {
    char *detail = NULL;
    if (session) libssh2_session_last_error(session, &detail, NULL, 0);
    pthread_mutex_lock(&ssh_lock);
    if (s->state > 0) {
        if (detail && detail[0]) snprintf(s->error, sizeof(s->error), "%s: %s", what, detail);
        else snprintf(s->error, sizeof(s->error), "%s", what);
        s->state = SSH_FAILED;
    }
    s->in_used = 0;
    pthread_mutex_unlock(&ssh_lock);
}

// Sleep until the socket is ready in the directions libssh2 waits for (plus
// `events`), the app wakes the worker, or `timeout_ms` passes. Returns 0 once
// the app has closed the session.
static int ssh_wait(os_ssh *s, int fd, LIBSSH2_SESSION *session, short events, int timeout_ms) {
    struct pollfd p[2] = {{fd, events, 0}, {s->wake[0], POLLIN, 0}};
    int directions = session ? libssh2_session_block_directions(session) : 0;
    if (directions & LIBSSH2_SESSION_BLOCK_INBOUND) p[0].events |= POLLIN;
    if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) p[0].events |= POLLOUT;
    poll(p, 2, timeout_ms);
    if (p[1].revents & POLLIN) {
        char drain[64];
        while (read(s->wake[0], drain, sizeof(drain)) > 0) {}
    }
    return !ssh_cancelled(s);
}

// Retry a nonblocking libssh2 call until it stops asking to be called again.
#define SSH_RETRY(rc, call)                                                   \
    for (;;) {                                                                \
        rc = (call);                                                          \
        if (rc != LIBSSH2_ERROR_EAGAIN) break;                                \
        if (!ssh_wait(s, fd, session, 0, 100)) { rc = OS_SSH_CANCELLED; break; } \
        if (ssh_now() > deadline) { rc = OS_SSH_TIMEOUT; break; }             \
    }

static int ssh_connect(os_ssh *s) {
    char port[16];
    snprintf(port, sizeof(port), "%d", s->port);
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(s->host, port, &hints, &list);
    if (err) {
        pthread_mutex_lock(&ssh_lock);
        snprintf(s->error, sizeof(s->error), "Cannot resolve %s: %s", s->host, gai_strerror(err));
        s->state = SSH_FAILED;
        pthread_mutex_unlock(&ssh_lock);
        return -1;
    }
    int fd = -1, reason = ETIMEDOUT;
    f64 deadline = ssh_now() + OS_SSH_CONNECT_SECONDS;
    for (struct addrinfo *a = list; a && fd < 0 && !ssh_cancelled(s); a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { reason = errno; continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        reason = errno;
        if (errno == EINPROGRESS) {
            reason = ETIMEDOUT;
            while (ssh_now() < deadline) {
                struct pollfd p = {fd, POLLOUT, 0};
                if (!ssh_wait(s, fd, NULL, POLLOUT, 100)) break;
                if (poll(&p, 1, 0) > 0) {
                    socklen_t size = sizeof(reason);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &reason, &size);
                    break;
                }
            }
            if (reason == 0) break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0 && !ssh_cancelled(s)) {
        pthread_mutex_lock(&ssh_lock);
        snprintf(s->error, sizeof(s->error), "Cannot connect to %s:%d: %s", s->host, s->port, strerror(reason));
        s->state = SSH_FAILED;
        pthread_mutex_unlock(&ssh_lock);
    }
    return fd;
}

// Single quotes keep every byte literal for the remote POSIX shell; a leading
// `~/` stays relative to the remote $HOME.
static void ssh_shell_command(char *out, size_t size, const char *folder) {
    size_t n = 0;
    const char *path = folder;
    n += (size_t)snprintf(out + n, size - n, "cd -- ");
    if (!strncmp(path, "~/", 2)) { n += (size_t)snprintf(out + n, size - n, "\"$HOME\"/"); path += 2; }
    out[n++] = '\'';
    for (const char *p = path; *p && n + 8 < size; ++p) {
        if (*p == '\'') { memcpy(out + n, "'\\''", 4); n += 4; }
        else out[n++] = *p;
    }
    out[n++] = '\'';
    out[n] = 0;
    // A missing folder prints the shell's own error and still opens a shell.
    snprintf(out + n, size - n, "; exec \"${SHELL:-/bin/sh}\" -l");
}

static void ssh_session_loop(os_ssh *s, int fd, LIBSSH2_SESSION *session) {
    LIBSSH2_CHANNEL *channel = NULL;
    f64 deadline = ssh_now() + OS_SSH_LOGIN_SECONDS;
    int rc;
    libssh2_session_set_blocking(session, 0);
    SSH_RETRY(rc, libssh2_session_handshake(session, fd));
    if (rc) { if (rc != OS_SSH_CANCELLED) ssh_fail(s, rc == OS_SSH_TIMEOUT ? NULL : session, rc == OS_SSH_TIMEOUT ? "SSH handshake timed out" : "SSH handshake failed"); return; }

    ssh_set_state(s, SSH_VERIFYING);
    const unsigned char *digest = (const unsigned char *)libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!digest) { ssh_fail(s, session, "The server offered no SHA-256 host key"); return; }
    char hash[52] = "SHA256:", endpoint[300];
    EVP_EncodeBlock((unsigned char *)hash + 7, digest, 32);
    hash[50] = 0; // OpenSSH fingerprint notation omits Base64 padding.
    snprintf(endpoint, sizeof(endpoint), "%s:%d", s->host, s->port);
    // Same Keychain pins as Git: first use asks, a changed key is refused.
    if (!os_git_trust_host(endpoint, hash)) { ssh_fail(s, NULL, "SSH host key was not trusted or has changed"); return; }
    if (ssh_cancelled(s)) return;

    char *key = os_git_secret_copy("private-key");
    char *pass = os_git_secret_copy("passphrase");
    if (!key) {
        ssh_fail(s, NULL, "Import an SSH private key before connecting");
        free(pass);
        return;
    }
    deadline = ssh_now() + OS_SSH_LOGIN_SECONDS;
    SSH_RETRY(rc, libssh2_userauth_publickey_frommemory(session, s->user, strlen(s->user), NULL, 0,
                                                        key, strlen(key), pass ? pass : ""));
    OPENSSL_cleanse(key, strlen(key)); free(key);
    if (pass) { OPENSSL_cleanse(pass, strlen(pass)); free(pass); }
    if (rc) { if (rc != OS_SSH_CANCELLED) ssh_fail(s, rc == OS_SSH_TIMEOUT ? NULL : session, rc == OS_SSH_TIMEOUT ? "SSH login timed out" : "SSH key was not accepted"); return; }

    for (;;) {
        channel = libssh2_channel_open_session(session);
        if (channel || libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) break;
        if (!ssh_wait(s, fd, session, 0, 100) || ssh_now() > deadline) break;
    }
    if (!channel) { ssh_fail(s, session, "Cannot open an SSH channel"); return; }
    pthread_mutex_lock(&ssh_lock);
    int cols = s->cols, rows = s->rows;
    s->resize = 0;
    pthread_mutex_unlock(&ssh_lock);
    static const char term[] = "xterm-256color";
    SSH_RETRY(rc, libssh2_channel_request_pty_ex(channel, term, sizeof(term) - 1, NULL, 0, cols, rows, 0, 0));
    if (rc) { ssh_fail(s, session, "The server refused a terminal"); goto done; }
    if (s->folder[0]) {
        char command[4200];
        ssh_shell_command(command, sizeof(command), s->folder);
        SSH_RETRY(rc, libssh2_channel_exec(channel, command));
    } else {
        SSH_RETRY(rc, libssh2_channel_shell(channel));
    }
    if (rc) { ssh_fail(s, session, "The server refused a shell"); goto done; }
    libssh2_keepalive_config(session, 0, 30);
    ssh_set_state(s, SSH_OPEN);

    unsigned char pending[OS_SSH_CHUNK], incoming[OS_SSH_CHUNK];
    for (;;) {
        pthread_mutex_lock(&ssh_lock);
        int cancel = s->cancel, resize = s->resize;
        cols = s->cols; rows = s->rows;
        size_t want = s->in_used < sizeof(pending) ? s->in_used : sizeof(pending);
        memcpy(pending, s->in, want);
        size_t space = OS_SSH_OUT_MAX - s->out_used;
        pthread_mutex_unlock(&ssh_lock);
        if (cancel) break;
        short events = 0;
        if (resize) {
            rc = libssh2_channel_request_pty_size_ex(channel, cols, rows, 0, 0);
            // EAGAIN keeps the request for the next pass; a refusal is not fatal.
            if (rc != LIBSSH2_ERROR_EAGAIN) {
                pthread_mutex_lock(&ssh_lock);
                if (s->cols == cols && s->rows == rows) s->resize = 0;
                pthread_mutex_unlock(&ssh_lock);
            }
        }
        if (want) {
            ssize_t sent = libssh2_channel_write(channel, (const char *)pending, want);
            if (sent > 0) {
                // Bytes are dropped from the queue as soon as the channel takes
                // them, so a reconnect never replays them.
                pthread_mutex_lock(&ssh_lock);
                memmove(s->in, s->in + sent, s->in_used - (size_t)sent);
                s->in_used -= (size_t)sent;
                pthread_mutex_unlock(&ssh_lock);
            } else if (sent == LIBSSH2_ERROR_EAGAIN) {
                events |= POLLOUT;
            } else {
                ssh_fail(s, session, "SSH write failed");
                break;
            }
        }
        int failed = 0;
        for (int stream = 0; stream < 2 && space > 0; ++stream) {
            for (;;) {
                size_t ask = space < sizeof(incoming) ? space : sizeof(incoming);
                ssize_t got = libssh2_channel_read_ex(channel, stream ? SSH_EXTENDED_DATA_STDERR : 0, (char *)incoming, ask);
                if (got == LIBSSH2_ERROR_EAGAIN || got == 0) break;
                if (got < 0) { failed = 1; break; }
                pthread_mutex_lock(&ssh_lock);
                memcpy(s->out + s->out_used, incoming, (size_t)got);
                s->out_used += (size_t)got;
                space = OS_SSH_OUT_MAX - s->out_used;
                pthread_mutex_unlock(&ssh_lock);
                if (space == 0) break;
            }
        }
        if (failed) { ssh_fail(s, session, "SSH connection lost"); break; }
        // EOF counts only once everything before it has been taken.
        if (space > 0 && libssh2_channel_eof(channel)) {
            // The exit status follows EOF; it is known once the channel closes.
            deadline = ssh_now() + 2;
            SSH_RETRY(rc, libssh2_channel_close(channel));
            if (rc == 0) SSH_RETRY(rc, libssh2_channel_wait_closed(channel));
            int status = libssh2_channel_get_exit_status(channel);
            pthread_mutex_lock(&ssh_lock);
            snprintf(s->error, sizeof(s->error), "Remote shell exited with status %d", status);
            if (s->state > 0) s->state = SSH_CLOSED;
            s->in_used = 0;
            pthread_mutex_unlock(&ssh_lock);
            break;
        }
        int next = 30;
        if (libssh2_keepalive_send(session, &next) < 0 && libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
            ssh_fail(s, session, "SSH connection lost");
            break;
        }
        // A full output buffer stops reading; the app's next read wakes us.
        if (space > 0) events |= POLLIN;
        if (!ssh_wait(s, fd, space > 0 ? session : NULL, events, 1000)) break;
    }
done:
    // Bounded blocking teardown; the network may already be gone.
    libssh2_session_set_timeout(session, 2000);
    libssh2_session_set_blocking(session, 1);
    if (channel) {
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
}

static void *ssh_worker(void *payload) {
    os_ssh *s = payload;
    int fd = ssh_connect(s);
    if (fd >= 0) {
        LIBSSH2_SESSION *session = libssh2_session_init();
        if (!session) {
            ssh_fail(s, NULL, "Cannot create an SSH session");
        } else {
            ssh_session_loop(s, fd, session);
            libssh2_session_set_timeout(session, 2000);
            libssh2_session_set_blocking(session, 1);
            libssh2_session_disconnect(session, "Closed by user");
            libssh2_session_free(session);
        }
        close(fd);
    }
    pthread_mutex_lock(&ssh_lock);
    if (s->state > 0) {
        if (!s->error[0]) snprintf(s->error, sizeof(s->error), "SSH session closed");
        s->state = SSH_CLOSED;
    }
    s->worker_live = 0;
    if (!s->app_open) ssh_release(s);
    pthread_mutex_unlock(&ssh_lock);
    return NULL;
}

i32 os_ssh_open(const char *host, i32 port, const char *user, const char *folder, i32 cols, i32 rows) {
    pthread_once(&ssh_once, ssh_start);
    if (!host || !user || !host[0] || !user[0] || port <= 0 || port > 65535) return 0;
    if (strlen(host) >= sizeof(ssh_slots[0].host) || strlen(user) >= sizeof(ssh_slots[0].user)) return 0;
    if (folder && strlen(folder) >= sizeof(ssh_slots[0].folder)) return 0;
    for (const char *p = host; *p; ++p) if ((unsigned char)*p <= 32) return 0;
    for (const char *p = user; *p; ++p) if ((unsigned char)*p <= 32) return 0;
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = NULL;
    for (int i = 0; i < OS_SSH_SLOTS && !s; ++i) if (!ssh_slots[i].used) s = &ssh_slots[i];
    if (!s) { pthread_mutex_unlock(&ssh_lock); return 0; }
    unsigned generation = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->wake[0] = s->wake[1] = -1;
    s->in = malloc(OS_SSH_IN_MAX);
    s->out = malloc(OS_SSH_OUT_MAX);
    if (!s->in || !s->out || pipe(s->wake) != 0) {
        free(s->in); free(s->out);
        s->in = s->out = NULL;
        s->wake[0] = s->wake[1] = -1;
        pthread_mutex_unlock(&ssh_lock);
        return 0;
    }
    fcntl(s->wake[0], F_SETFL, O_NONBLOCK);
    fcntl(s->wake[1], F_SETFL, O_NONBLOCK);
    snprintf(s->host, sizeof(s->host), "%s", host);
    snprintf(s->user, sizeof(s->user), "%s", user);
    snprintf(s->folder, sizeof(s->folder), "%s", folder ? folder : "");
    s->port = port;
    s->cols = cols > 0 ? cols : 80;
    s->rows = rows > 0 ? rows : 24;
    s->used = s->app_open = s->worker_live = 1;
    s->state = SSH_CONNECTING;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // The loop keeps two 16 KiB chunks on its stack.
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int started = pthread_create(&thread, &attr, ssh_worker, s) == 0;
    pthread_attr_destroy(&attr);
    if (!started) {
        s->worker_live = s->app_open = 0;
        ssh_release(s);
        pthread_mutex_unlock(&ssh_lock);
        return 0;
    }
    i32 handle = (i32)(s->generation * OS_SSH_SLOTS + (unsigned)(s - ssh_slots) + 1);
    pthread_mutex_unlock(&ssh_lock);
    return handle;
}

i32 os_ssh_state(i32 handle) {
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = ssh_find(handle);
    i32 state = s ? s->state : SSH_FAILED;
    pthread_mutex_unlock(&ssh_lock);
    return state;
}

const char *os_ssh_error(i32 handle) {
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = ssh_find(handle);
    snprintf(ssh_error_text, sizeof(ssh_error_text), "%s", s ? s->error : "");
    pthread_mutex_unlock(&ssh_lock);
    return ssh_error_text;
}

// Output keeps arriving after the shell exits until it is drained. NUL bytes
// cannot travel in a str and are dropped; terminals ignore them anyway.
const char *os_ssh_read(i32 handle, i32 capacity) {
    size_t n = 0;
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = ssh_find(handle);
    if (s && s->out_used) {
        size_t limit = capacity > 0 && capacity < OS_SSH_READ_MAX ? (size_t)capacity : OS_SSH_READ_MAX;
        size_t take = s->out_used < limit ? s->out_used : limit;
        int was_full = s->out_used == OS_SSH_OUT_MAX;
        for (size_t i = 0; i < take; ++i) if (s->out[i]) ssh_read_text[n++] = (char)s->out[i];
        memmove(s->out, s->out + take, s->out_used - take);
        s->out_used -= take;
        if (was_full) ssh_wake(s);
    }
    ssh_read_text[n] = 0;
    pthread_mutex_unlock(&ssh_lock);
    return ssh_read_text;
}

// Queues input while connecting or open. Returns the bytes accepted, or -1
// when the session has ended or the queue cannot take all of `data`.
i32 os_ssh_write(i32 handle, const char *data) {
    if (!data) return -1;
    size_t n = strlen(data);
    i32 result = -1;
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = ssh_find(handle);
    if (s && s->state > 0 && s->in_used + n <= OS_SSH_IN_MAX) {
        memcpy(s->in + s->in_used, data, n);
        s->in_used += n;
        result = (i32)n;
        ssh_wake(s);
    }
    pthread_mutex_unlock(&ssh_lock);
    return result;
}

i32 os_ssh_resize(i32 handle, i32 cols, i32 rows) {
    if (cols <= 0 || rows <= 0) return 0;
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = ssh_find(handle);
    if (s && (s->cols != cols || s->rows != rows)) {
        s->cols = cols;
        s->rows = rows;
        s->resize = 1;
        ssh_wake(s);
    }
    pthread_mutex_unlock(&ssh_lock);
    return s ? 1 : 0;
}

void os_ssh_close(i32 handle) {
    pthread_mutex_lock(&ssh_lock);
    os_ssh *s = ssh_find(handle);
    if (s) {
        s->app_open = 0;
        s->cancel = 1;
        s->in_used = 0;
        ssh_wake(s);
        // A worker still waiting on the network or a trust prompt releases
        // the slot itself when it ends.
        if (!s->worker_live) ssh_release(s);
    }
    pthread_mutex_unlock(&ssh_lock);
}
#endif
