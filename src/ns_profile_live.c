#include "ns_profile_live.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ns_live_socket;
#define NS_LIVE_INVALID INVALID_SOCKET
#define ns_live_closesocket closesocket
#define ns_live_would_block() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int ns_live_socket;
#define NS_LIVE_INVALID (-1)
#define ns_live_closesocket close
#define ns_live_would_block() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

#if defined(MSG_NOSIGNAL)
#define NS_LIVE_SEND_FLAGS MSG_NOSIGNAL
#else
#define NS_LIVE_SEND_FLAGS 0
#endif

typedef struct ns_profile_live_state {
    ns_bool connected;
    ns_bool paused;
    ns_live_socket fd;

    // Outgoing byte queue. The socket is nonblocking, so a slow viewer parks
    // bytes here instead of stalling the profiled program.
    u8 *out;
    szt out_len;
    szt out_head;
    szt out_cap;
    u64 dropped_bytes;
    u64 dropped_frames;

    // Incremental table cursors: only symbols and lanes the viewer has not
    // seen are re-sent before a frame that uses them.
    i32 sent_fns;
    i32 sent_threads;

    // Command reassembly. Commands are small and never carry a payload today.
    u8 in[256];
    szt in_len;
} ns_profile_live_state;

static ns_profile_live_state ns_live = {0};

// ---------------------------------------------------------------------------
// little-endian writers into the outgoing queue

static void ns_live_reserve(szt need) {
    if (ns_live.out_len + need <= ns_live.out_cap) return;
    szt cap = ns_live.out_cap ? ns_live.out_cap : 65536;
    while (cap < ns_live.out_len + need) cap *= 2;
    u8 *next = (u8 *)ns_malloc(cap);
    if (!next) return;
    if (ns_live.out) {
        memcpy(next, ns_live.out, ns_live.out_len);
        ns_free(ns_live.out);
    }
    ns_live.out = next;
    ns_live.out_cap = cap;
}

static void ns_live_put(const void *data, szt len) {
    if (!ns_live.connected || len == 0) return;
    ns_live_reserve(len);
    if (ns_live.out_len + len > ns_live.out_cap) return;
    memcpy(ns_live.out + ns_live.out_len, data, len);
    ns_live.out_len += len;
}

static void ns_live_put_u8(u8 v) { ns_live_put(&v, 1); }

static void ns_live_put_u16(u16 v) {
    u8 b[2] = {(u8)(v & 0xff), (u8)((v >> 8) & 0xff)};
    ns_live_put(b, 2);
}

static void ns_live_put_u32(u32 v) {
    u8 b[4] = {(u8)(v & 0xff), (u8)((v >> 8) & 0xff), (u8)((v >> 16) & 0xff), (u8)((v >> 24) & 0xff)};
    ns_live_put(b, 4);
}

static void ns_live_put_i32(i32 v) { ns_live_put_u32((u32)v); }

// Message headers are back-patched: the payload length is only known once the
// body is written, and buffering the body twice would cost a copy per frame.
static szt ns_live_begin_message(u32 type) {
    ns_live_put_u32(type);
    ns_live_put_u32(0);
    return ns_live.out_len; // payload start
}

static void ns_live_end_message(szt payload_start) {
    if (!ns_live.connected) return;
    if (payload_start < 4 || payload_start > ns_live.out_len) return;
    u32 len = (u32)(ns_live.out_len - payload_start);
    u8 *p = ns_live.out + payload_start - 4;
    p[0] = (u8)(len & 0xff);
    p[1] = (u8)((len >> 8) & 0xff);
    p[2] = (u8)((len >> 16) & 0xff);
    p[3] = (u8)((len >> 24) & 0xff);
}

// ---------------------------------------------------------------------------
// socket plumbing

static void ns_live_drop(void) {
    if (ns_live.fd != NS_LIVE_INVALID) ns_live_closesocket(ns_live.fd);
    ns_live.fd = NS_LIVE_INVALID;
    ns_live.connected = false;
    ns_live.out_len = 0;
    ns_live.out_head = 0;
    ns_profile.frame_sink = ns_null;
}

// Push as much of the queue as the socket accepts. Never blocks.
static void ns_live_flush(void) {
    if (!ns_live.connected) return;
    while (ns_live.out_head < ns_live.out_len) {
        szt remain = ns_live.out_len - ns_live.out_head;
        int n = (int)send(ns_live.fd, (const char *)(ns_live.out + ns_live.out_head), (int)(remain > 262144 ? 262144 : remain), NS_LIVE_SEND_FLAGS);
        if (n > 0) {
            ns_live.out_head += (szt)n;
            continue;
        }
        if (n < 0 && ns_live_would_block()) break;
        // The viewer closed or the socket failed: capture keeps running local.
        ns_live_drop();
        return;
    }
    if (ns_live.out_head == ns_live.out_len) {
        ns_live.out_head = 0;
        ns_live.out_len = 0;
    } else if (ns_live.out_head > 0) {
        memmove(ns_live.out, ns_live.out + ns_live.out_head, ns_live.out_len - ns_live.out_head);
        ns_live.out_len -= ns_live.out_head;
        ns_live.out_head = 0;
    }
}

static ns_bool ns_live_set_nonblocking(ns_live_socket fd) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// ---------------------------------------------------------------------------
// table + frame publishing

static void ns_live_send_tables(void) {
    if (ns_profile.thread_count > ns_live.sent_threads) {
        i32 first = ns_live.sent_threads;
        i32 count = ns_profile.thread_count - first;
        szt at = ns_live_begin_message(NS_PROFILE_LIVE_MSG_THREADS);
        ns_live_put_u32((u32)first);
        ns_live_put_u32((u32)count);
        for (i32 i = first; i < ns_profile.thread_count; i++) {
            ns_str t = ns_profile.threads[i];
            u16 len = t.len > 0 && t.len <= 0xffff ? (u16)t.len : 0;
            ns_live_put_u16(len);
            if (len > 0) ns_live_put(t.data, len);
        }
        ns_live_end_message(at);
        ns_live.sent_threads = ns_profile.thread_count;
    }
    if (ns_profile.fn_count > ns_live.sent_fns) {
        i32 first = ns_live.sent_fns;
        i32 count = ns_profile.fn_count - first;
        szt at = ns_live_begin_message(NS_PROFILE_LIVE_MSG_SYMBOLS);
        ns_live_put_u32((u32)first);
        ns_live_put_u32((u32)count);
        for (i32 i = first; i < ns_profile.fn_count; i++) {
            ns_profile_fn_stat *s = &ns_profile.fns[i];
            u16 lib_len = s->lib.len > 0 && s->lib.len <= 0xffff ? (u16)s->lib.len : 0;
            u16 name_len = s->name.len > 0 && s->name.len <= 0xffff ? (u16)s->name.len : 0;
            ns_live_put_u8(s->kind);
            ns_live_put_u16(lib_len);
            ns_live_put_u16(name_len);
            if (lib_len > 0) ns_live_put(s->lib.data, lib_len);
            if (name_len > 0) ns_live_put(s->name.data, name_len);
        }
        ns_live_end_message(at);
        ns_live.sent_fns = ns_profile.fn_count;
    }
}

static i32 ns_live_ms_to_us(f64 ms) {
    if (ms <= 0.0) return 0;
    f64 us = ms * 1000.0;
    if (us > 2147483647.0) return 2147483647;
    return (i32)(us + 0.5);
}

static void ns_live_put_frame(const ns_profile_frame *frame) {
    i32 count = (i32)ns_array_length(frame->events);
    szt at = ns_live_begin_message(NS_PROFILE_LIVE_MSG_FRAME);
    ns_live_put_u32(frame->index);
    ns_live_put_i32(ns_live_ms_to_us(frame->start_ms));
    ns_live_put_i32(ns_live_ms_to_us(frame->end_ms));
    ns_live_put_u32((u32)count);
    for (i32 i = 0; i < count; i++) {
        ns_profile_event *e = &frame->events[i];
        i32 thread = e->thread;
        if (thread < 0 || thread >= ns_profile.thread_count) thread = 0;
        i32 sym = e->fn_index;
        if (sym < 0 || sym >= ns_profile.fn_count) sym = 0;
        if (sym > 0xffff) sym = 0xffff;
        i32 depth = e->depth;
        if (depth < 0) depth = 0;
        if (depth > 255) depth = 255;
        ns_live_put_u8(e->kind);
        ns_live_put_u8((u8)depth);
        ns_live_put_u16((u16)thread);
        ns_live_put_u16((u16)sym);
        ns_live_put_u16(0);
        ns_live_put_i32(ns_live_ms_to_us(e->start_ms));
        ns_live_put_i32(ns_live_ms_to_us(e->elapsed_ms));
        ns_live_put_i32(ns_live_ms_to_us(e->self_ms));
    }
    ns_live_end_message(at);
}

// Resend the whole retained window, oldest first, so a viewer that asks for a
// replay sees the same 128 frames the ring holds.
static void ns_live_replay(void) {
    if (!ns_live.connected) return;
    i32 slot = ns_profile.frame_head - ns_profile.frame_fill;
    while (slot < 0) slot += NS_PROFILE_LIVE_FRAMES;
    ns_live_send_tables();
    for (i32 i = 0; i < ns_profile.frame_fill; i++) {
        ns_profile_frame *frame = &ns_profile.frames[slot];
        if (ns_array_length(frame->events) > 0) ns_live_put_frame(frame);
        slot = (slot + 1) % NS_PROFILE_LIVE_FRAMES;
    }
    ns_live_flush();
}

// ---------------------------------------------------------------------------
// viewer commands

static void ns_live_handle_command(u32 type) {
    if (type == NS_PROFILE_LIVE_CMD_PAUSE) {
        ns_live.paused = true;
    } else if (type == NS_PROFILE_LIVE_CMD_RESUME) {
        ns_live.paused = false;
    } else if (type == NS_PROFILE_LIVE_CMD_REPLAY) {
        ns_live.paused = false;
        ns_live_replay();
    } else if (type == NS_PROFILE_LIVE_CMD_QUIT) {
        ns_profile_live_close(NS_PROFILE_LIVE_BYE_STOPPED);
        // The atexit hook still writes bin/ns.profile for the run so far.
        exit(0);
    }
}

static void ns_live_poll_commands(void) {
    if (!ns_live.connected) return;
    for (;;) {
        if (ns_live.in_len >= sizeof(ns_live.in)) ns_live.in_len = 0;
        int n = (int)recv(ns_live.fd, (char *)(ns_live.in + ns_live.in_len), (int)(sizeof(ns_live.in) - ns_live.in_len), 0);
        if (n > 0) {
            ns_live.in_len += (szt)n;
        } else if (n == 0) {
            ns_live_drop();
            return;
        } else {
            if (!ns_live_would_block()) ns_live_drop();
            break;
        }
    }
    szt at = 0;
    while (ns_live.in_len - at >= 8) {
        const u8 *p = ns_live.in + at;
        u32 type = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        u32 len = (u32)p[4] | ((u32)p[5] << 8) | ((u32)p[6] << 16) | ((u32)p[7] << 24);
        if (len > sizeof(ns_live.in)) { // unreadable framing: resync by dropping
            ns_live.in_len = 0;
            return;
        }
        if (ns_live.in_len - at < 8 + (szt)len) break;
        at += 8 + (szt)len;
        ns_live_handle_command(type);
        if (!ns_live.connected) return;
    }
    if (at > 0) {
        memmove(ns_live.in, ns_live.in + at, ns_live.in_len - at);
        ns_live.in_len -= at;
    }
}

// ---------------------------------------------------------------------------
// frame sink

static void ns_profile_live_sink(const ns_profile_frame *frame) {
    if (!ns_live.connected) return;
    ns_live_poll_commands();
    if (!ns_live.connected) return;
    if (!ns_live.paused) {
        // A viewer that cannot keep up loses whole frames rather than delaying
        // the program; the gap is visible as a jump in the frame index.
        if (ns_live.out_len - ns_live.out_head > NS_PROFILE_LIVE_OUT_MAX) {
            ns_live.dropped_bytes += ns_live.out_len - ns_live.out_head;
            ns_live.dropped_frames++;
            ns_live.out_len = 0;
            ns_live.out_head = 0;
            // Tables are re-sent on demand; the viewer already has them.
        } else {
            ns_live_send_tables();
            ns_live_put_frame(frame);
        }
    }
    ns_live_flush();
}

// ---------------------------------------------------------------------------
// lifecycle

ns_bool ns_profile_live_connect(const char *host, i32 port, const char *title) {
    if (port <= 0 || port > 65535) return false;
    if (ns_live.connected) return true;
#if defined(_WIN32)
    static ns_bool wsa_ready = false;
    if (!wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_ready = true;
    }
#endif
    const char *addr = (host && host[0]) ? host : "127.0.0.1";
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u16)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) return false;

    // The viewer listens before it launches the target, so the first attempt
    // normally succeeds; the retries only cover a hand-started target racing a
    // viewer that is still coming up.
    ns_live_socket fd = NS_LIVE_INVALID;
    for (i32 attempt = 0; attempt < 10; attempt++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == NS_LIVE_INVALID) return false;
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
        ns_live_closesocket(fd);
        fd = NS_LIVE_INVALID;
#if defined(_WIN32)
        Sleep(50);
#else
        usleep(50000);
#endif
    }
    if (fd == NS_LIVE_INVALID) {
        ns_warn("profile", "live viewer not listening on %s:%d; capture stays local.\n", addr, port);
        return false;
    }

    int yes = 1;
#if defined(SO_NOSIGPIPE)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof(yes));
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
    ns_live_set_nonblocking(fd);

    ns_live.fd = fd;
    ns_live.connected = true;
    ns_live.paused = false;
    ns_live.sent_fns = 0;
    ns_live.sent_threads = 0;
    ns_live.in_len = 0;
    ns_live.out_len = 0;
    ns_live.out_head = 0;

    ns_live_put_u32(NS_PROFILE_LIVE_MAGIC);
    szt at = ns_live_begin_message(NS_PROFILE_LIVE_MSG_HELLO);
    ns_live_put_u32(NS_PROFILE_LIVE_VERSION);
#if defined(_WIN32)
    ns_live_put_u32((u32)GetCurrentProcessId());
#else
    ns_live_put_u32((u32)getpid());
#endif
    ns_live_put_u32((u32)NS_PROFILE_LIVE_FRAMES);
    const char *name = (title && title[0]) ? title : "ns";
    szt name_len = strlen(name);
    if (name_len > 0xffff) name_len = 0xffff;
    ns_live_put_u16((u16)name_len);
    ns_live_put(name, name_len);
    ns_live_end_message(at);
    ns_live_flush();
    if (!ns_live.connected) return false;

    ns_profile_ring_enable();
    ns_profile.frame_sink = ns_profile_live_sink;
    return true;
}

ns_bool ns_profile_live_active(void) { return ns_live.connected; }

void ns_profile_live_close(u32 reason) {
    if (!ns_live.connected) return;
    ns_profile.frame_sink = ns_null;
    // Publish whatever the open frame collected before the program ends.
    ns_profile_frame *frame = &ns_profile.frames[ns_profile.frame_head];
    if (ns_array_length(frame->events) > 0) {
        frame->index = ns_profile.frame_seq;
        frame->end_ms = ns_profile_now_ms() - ns_profile.start_ms;
        ns_live_send_tables();
        ns_live_put_frame(frame);
    }
    szt at = ns_live_begin_message(NS_PROFILE_LIVE_MSG_BYE);
    ns_live_put_u32(reason);
    ns_live_end_message(at);

    // A short blocking drain: the viewer should see the last frames, but a
    // stuck viewer must not hold the exit path open.
    for (i32 i = 0; i < 100 && ns_live.connected && ns_live.out_head < ns_live.out_len; i++) {
        ns_live_flush();
        if (ns_live.out_head >= ns_live.out_len) break;
#if defined(_WIN32)
        Sleep(2);
#else
        usleep(2000);
#endif
    }
    if (ns_live.dropped_frames > 0) {
        ns_warn("profile", "live viewer fell behind; dropped %llu frame batches.\n",
                (unsigned long long)ns_live.dropped_frames);
    }
    ns_live_drop();
}
