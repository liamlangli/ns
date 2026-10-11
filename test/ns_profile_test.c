#include "ns_test.h"
#include "ns_profile.h"
#include "ns_profile_live.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static ns_str S(const char *s) { return ns_str_cstr((char *)s); }

static ns_bool ns_test_near(f64 a, f64 b) { return a > b - 0.001 && a < b + 0.001; }

// Interned ids, as ns_cpu records them: no name lookup per call.
static void ns_profile_test_ids(void) {
    ns_profile_reset();
    ns_profile_enable(0.0);
    i32 outer = ns_profile_symbol(S("outer"), ns_str_null, NS_PROFILE_EVENT_SCOPE);
    i32 inner = ns_profile_symbol(S("inner"), ns_str_null, NS_PROFILE_EVENT_SCOPE);
    i32 draw = ns_profile_symbol(S("gpu_draw"), S("gpu"), NS_PROFILE_EVENT_FFI);
    ns_expect(outer >= 0 && inner >= 0 && draw >= 0 && outer != inner, "symbols intern to distinct ids");
    ns_expect(ns_profile_symbol(S("outer"), ns_str_null, NS_PROFILE_EVENT_SCOPE) == outer, "interning is stable");
    ns_profile_enter(outer, 0.0);
    ns_profile_enter(inner, 10.0);
    ns_profile_record_ffi_id(draw, 12.0, 3.0);
    ns_profile_leave(20.0);
    ns_profile_leave(40.0);
    ns_expect(ns_profile.open_len == 0, "id scopes drain the open stack");
    ns_expect(ns_test_near(ns_profile.fns[outer].total_ms, 40.0) && ns_test_near(ns_profile.fns[outer].self_ms, 30.0),
              "outer: 40ms inclusive, 30ms exclusive");
    ns_expect(ns_test_near(ns_profile.fns[inner].total_ms, 10.0) && ns_test_near(ns_profile.fns[inner].self_ms, 7.0),
              "inner: 10ms inclusive, 7ms exclusive of the native call");
    ns_expect(ns_profile.ffi_calls == 1 && ns_test_near(ns_profile.fns[draw].self_ms, 3.0), "native call recorded by id");

    // Past the open-stack limit enters are skipped, and so are their leaves.
    for (i32 i = 0; i < NS_PROFILE_MAX_STACK + 5; i++) ns_profile_enter(inner, (f64)i);
    for (i32 i = 0; i < NS_PROFILE_MAX_STACK + 5; i++) ns_profile_leave(1000.0);
    ns_expect(ns_profile.open_len == 0 && ns_profile.open_skipped == 0, "deep recursion stays balanced");

    u64 calls = ns_profile.scope_calls;
    ns_profile_enter(outer, 0.0);
    ns_profile_disable();
    ns_expect(!ns_profile.enabled && ns_profile.open_len == 0, "disable drops the open stack");
    ns_profile_leave(5.0);
    ns_expect(ns_profile.scope_calls == calls, "nothing records while disabled");
    ns_profile_reset();
}

static u32 ns_test_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

// Read whole messages until one of `type` arrives; false on timeout.
static ns_bool ns_test_read_until(int fd, u8 *buf, szt cap, szt *len, u32 type) {
    for (i32 tries = 0; tries < 200; tries++) {
        szt at = 4; // stream magic
        while (*len >= at + 8) {
            u32 kind = ns_test_u32(buf + at);
            u32 size = ns_test_u32(buf + at + 4);
            if (*len < at + 8 + size) break;
            if (kind == type) return true;
            at += 8 + size;
        }
        ssize_t n = recv(fd, buf + *len, cap - *len, MSG_DONTWAIT);
        if (n > 0) *len += (szt)n;
        else usleep(5000);
    }
    return false;
}

// Device mode: profiling stays off until a viewer connects, streams frames
// while it is there and switches off when it leaves.
static void ns_profile_test_serve(void) {
    ns_profile_reset();
    const i32 port = 47640;
    ns_expect(ns_profile_live_serve(port, "profile-test"), "device mode listens");
    ns_expect(!ns_profile.enabled, "no profiling before a viewer connects");
    ns_profile_callback_return();
    ns_expect(!ns_profile.enabled, "a callback without a viewer changes nothing");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u16)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ns_expect(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0, "viewer connects to the device port");
    for (i32 i = 0; i < 400 && !ns_profile.enabled; i++) {
        usleep(5000);
        ns_profile_callback_return();
    }
    ns_expect(ns_profile.enabled, "the next callback picks up the viewer and enables profiling");

    static u8 buf[1 << 16];
    szt len = 0;
    ns_expect(ns_test_read_until(fd, buf, sizeof(buf), &len, NS_PROFILE_LIVE_MSG_HELLO), "viewer receives HELLO");
    ns_expect(len >= 4 && ns_test_u32(buf) == NS_PROFILE_LIVE_MAGIC, "stream starts with the magic");

    i32 work = ns_profile_symbol(S("frame_work"), ns_str_null, NS_PROFILE_EVENT_SCOPE);
    f64 t = ns_profile_now_ms();
    ns_profile_enter(work, t);
    ns_profile_leave(t + 2.0);
    ns_profile_callback_return();
    ns_expect(ns_test_read_until(fd, buf, sizeof(buf), &len, NS_PROFILE_LIVE_MSG_FRAME), "a closed frame is streamed");

    close(fd);
    for (i32 i = 0; i < 400 && ns_profile.enabled; i++) {
        usleep(5000);
        t = ns_profile_now_ms();
        ns_profile_enter(work, t);
        ns_profile_leave(t + 1.0);
        ns_profile_callback_return();
    }
    ns_expect(!ns_profile.enabled, "profiling stops when the viewer leaves");
    ns_profile.service = ns_null;
    ns_profile_reset();
}

int main() {
    ns_profile_test_ids();
    ns_profile_test_serve();
    ns_profile_reset();
    ns_profile_enable(0.0);

    ns_profile_scope_enter(S("main"), ns_str_null);
    ns_profile_scope_enter(S("mid"), ns_str_null);
    ns_profile_scope_enter(S("leaf"), ns_str_null);
    ns_profile_record_ffi(S("os_time_ms"), S("os"), 100.0, 10.0);
    ns_profile_record_scope(S("leaf"), ns_str_null, 2, 90.0, 20.0);
    ns_profile_record_scope(S("mid"), ns_str_null, 1, 80.0, 30.0);
    ns_profile_record_scope(S("main"), ns_str_null, 0, 0.0, 50.0);

    ns_expect(ns_profile.scope_calls == 3, "three vm scopes recorded");
    ns_expect(ns_profile.ffi_calls == 1, "one ffi call recorded");
    ns_expect(ns_profile.flame_count == 4, "main/mid/leaf/ffi flame nodes");
    ns_expect(ns_array_length(ns_profile.events) == 4, "four timeline events");
    ns_expect(ns_profile.open_len == 0, "open stack drained");
    ns_expect(ns_profile.thread_count == 1, "default main thread");
    ns_expect(ns_profile.events[0].thread == 0, "events on main thread");

    i32 main_i = -1;
    i32 mid_i = -1;
    i32 leaf_i = -1;
    i32 ffi_i = -1;
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ns_profile.fns[i];
        if (ns_str_equals_STR(s->name, "main")) main_i = i;
        if (ns_str_equals_STR(s->name, "mid")) mid_i = i;
        if (ns_str_equals_STR(s->name, "leaf")) leaf_i = i;
        if (ns_str_equals_STR(s->name, "os_time_ms")) ffi_i = i;
    }
    ns_expect(main_i >= 0 && mid_i >= 0 && leaf_i >= 0 && ffi_i >= 0, "all symbols present");
    ns_expect(ns_profile.fns[main_i].total_ms > 49.0 && ns_profile.fns[main_i].total_ms < 51.0, "main inclusive 50ms");
    ns_expect(ns_profile.fns[main_i].self_ms > 19.0 && ns_profile.fns[main_i].self_ms < 21.0, "main exclusive 20ms");
    ns_expect(ns_profile.fns[mid_i].self_ms > 9.0 && ns_profile.fns[mid_i].self_ms < 11.0, "mid exclusive 10ms");
    ns_expect(ns_profile.fns[leaf_i].self_ms > 9.0 && ns_profile.fns[leaf_i].self_ms < 11.0, "leaf exclusive 10ms");
    ns_expect(ns_profile.fns[ffi_i].self_ms > 9.0 && ns_profile.fns[ffi_i].self_ms < 11.0, "ffi exclusive 10ms");
    ns_expect(ns_profile.fns[ffi_i].kind == NS_PROFILE_EVENT_FFI, "ffi kind");
    ns_expect(ns_profile.fns[main_i].kind == NS_PROFILE_EVENT_SCOPE, "scope kind");

    i32 leaf_flame = -1;
    for (i32 i = 0; i < ns_profile.flame_count; i++) {
        if (ns_profile.flames[i].fn_index == leaf_i) leaf_flame = i;
    }
    ns_expect(leaf_flame >= 0, "leaf flame node");
    i32 mid_flame = ns_profile.flames[leaf_flame].parent;
    ns_expect(mid_flame >= 0, "leaf has parent");
    ns_expect(ns_profile.flames[mid_flame].fn_index == mid_i, "leaf parent is mid");

    char text_path[] = "bin/ns_profile_test.profile";
    FILE *tf = fopen(text_path, "w");
    ns_expect(tf != ns_null, "open text profile");
    if (tf) {
        ns_profile_write_report(tf, text_path, 50.0, 0, ns_null);
        fclose(tf);
    }

    FILE *in = fopen(text_path, "r");
    ns_expect(in != ns_null, "reread text profile");
    char line[512];
    ns_bool saw_v6 = false;
    ns_bool saw_fn = false;
    ns_bool saw_flame = false;
    ns_bool saw_stack = false;
    ns_bool saw_thread = false;
    ns_bool saw_blob = false;
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (strncmp(line, "format: ns-profile-v6", 21) == 0) saw_v6 = true;
            if (strncmp(line, "fn: scope", 9) == 0) saw_fn = true;
            if (strncmp(line, "flame:", 6) == 0) saw_flame = true;
            if (strstr(line, "main;mid;leaf")) saw_stack = true;
            if (strncmp(line, "thread: 0 main", 14) == 0) saw_thread = true;
            if (strncmp(line, "timeline_blob:", 14) == 0) saw_blob = true;
        }
        fclose(in);
    }
    ns_expect(saw_v6, "text format ns-profile-v6");
    ns_expect(saw_fn, "fn table rows");
    ns_expect(saw_flame, "folded flame rows");
    ns_expect(saw_stack, "folded stack main;mid;leaf");
    ns_expect(saw_thread, "thread table lists main");
    ns_expect(saw_blob, "timeline blob referenced");
    FILE *blob = fopen("bin/ns_profile_test.profile.tl", "rb");
    if (!blob) blob = fopen("bin/ns_profile_test.profile.tl.z", "rb");
    ns_expect(blob != ns_null, "compact timeline blob written");
    if (blob) fclose(blob);

    // Per-thread open stacks: park main mid-scope, record on a worker lane,
    // then restore main so exclusive time stays correct across handoffs.
    ns_profile_reset();
    ns_profile_enable(0.0);
    ns_profile_thread_ctx main_ctx = {0};
    ns_profile_thread_ctx worker_ctx = {0};
    ns_profile_bind_thread(S("main"), &main_ctx);
    ns_profile_scope_enter(S("root"), ns_str_null);
    ns_profile_park_thread(&main_ctx);
    ns_expect(ns_profile.open_len == 0, "parked open stack clears live frames");
    ns_profile_bind_thread(S("worker"), &worker_ctx);
    ns_profile_scope_enter(S("job"), ns_str_null);
    ns_profile_record_scope(S("job"), ns_str_null, 0, 10.0, 5.0);
    ns_expect(ns_profile.thread_count == 2, "main and worker threads interned");
    ns_expect(ns_array_length(ns_profile.events) == 1, "worker event recorded");
    ns_expect(ns_profile.events[0].thread == 1, "worker event uses worker thread");
    ns_profile_park_thread(&worker_ctx);
    ns_profile_bind_thread(S("main"), &main_ctx);
    ns_expect(ns_profile.open_len == 1, "main open stack restored");
    ns_profile_record_scope(S("root"), ns_str_null, 0, 0.0, 20.0);
    ns_expect(ns_profile.open_len == 0, "main stack drained after restore");
    ns_expect(ns_array_length(ns_profile.events) == 2, "main event after restore");
    ns_expect(ns_profile.events[1].thread == 0, "restored event stays on main");

    // Nested micro-scopes stay out of the timeline; depth-0 roots are kept so
    // empty async workers still show a lane. FFI samples stay up to the hard cap.
    ns_profile_reset();
    ns_profile_enable(0.0);
    ns_profile_scope_enter(S("holder"), ns_str_null);
    for (i32 i = 0; i < 1000; i++) {
        ns_profile_scope_enter(S("tiny"), ns_str_null);
        // Nested under holder: after pop, depth is 1 and the short span is dropped.
        ns_profile_record_scope(S("tiny"), ns_str_null, 1, (f64)i, 0.001);
    }
    ns_expect(ns_array_length(ns_profile.events) == 0, "nested micro scopes skipped from timeline");
    ns_expect(ns_profile.timeline_skipped == 1000, "micro scopes counted as skipped");
    ns_expect(ns_profile.scope_calls == 1000, "micro scopes still aggregate");
    ns_profile_record_scope(S("holder"), ns_str_null, 0, 0.0, 50.0);
    ns_expect(ns_array_length(ns_profile.events) == 1, "holder root retained");
    ns_profile_reset();
    ns_profile_enable(0.0);
    ns_profile_scope_enter(S("root"), ns_str_null);
    ns_profile_record_scope(S("root"), ns_str_null, 0, 0.0, 0.001);
    ns_expect(ns_array_length(ns_profile.events) == 1, "depth-0 root kept even when short");

    ns_profile_reset();
    ns_profile_enable(0.0);
    for (i32 i = 0; i < 300000; i++) {
        ns_profile_record_ffi(S("grow"), S("test"), (f64)i, 0.001);
    }
    ns_expect(ns_array_length(ns_profile.events) == 300000, "timeline keeps ffi samples");
    ns_expect(ns_profile.events[0].start_ms >= -0.001 && ns_profile.events[0].start_ms <= 0.001, "oldest sample retained");
    ns_expect(ns_profile.events[299999].start_ms > 299998.0, "newest sample retained");
    ns_profile_reset();

    // Live mode: the timeline becomes a ring of whole frames. Capture keeps
    // running for as long as the program does, so only the last
    // NS_PROFILE_LIVE_FRAMES frames are retained.
    ns_profile_enable(0.0);
    ns_profile_ring_enable();
    ns_expect(ns_profile.ring, "ring enabled");
    for (i32 frame = 0; frame < 400; frame++) {
        for (i32 e = 0; e < 3; e++) {
            ns_profile_record_ffi(S("draw"), S("gpu"), (f64)(frame * 10 + e), 1.0);
        }
        ns_profile_frame_boundary();
    }
    ns_expect(ns_array_length(ns_profile.events) == 0, "ring mode keeps the linear timeline empty");
    ns_expect(ns_profile.frame_seq == 400, "every frame published");
    ns_expect(ns_profile.frame_fill == NS_PROFILE_LIVE_FRAMES - 1, "ring holds its full window");
    ns_expect(ns_profile.ffi_calls == 1200, "aggregates still cover the whole run");
    i32 oldest_slot = ns_profile.frame_head - ns_profile.frame_fill;
    while (oldest_slot < 0) oldest_slot += NS_PROFILE_LIVE_FRAMES;
    // 127 closed frames behind the open one: 128 frames of data in all.
    ns_expect(ns_profile.frames[oldest_slot].index == 400 - (NS_PROFILE_LIVE_FRAMES - 1), "oldest retained frame is the window start");
    ns_expect(ns_profile.frames_retired == 0, "no frame dropped early under the event budget");

    // The exit report covers exactly the window the ring still holds. Frames
    // coalesce their three identical back-to-back spans into one.
    i32 flat = ns_profile_ring_flatten();
    ns_expect(flat == NS_PROFILE_LIVE_FRAMES - 1, "flatten yields one span per retained frame");
    ns_expect(ns_profile.events[0].start_ms >= (f64)((400 - (NS_PROFILE_LIVE_FRAMES - 1)) * 10), "flattened window starts at the oldest frame");

    // An empty frame is not published: the boundary fires on every callback,
    // including ones that record nothing.
    u32 before = ns_profile.frame_seq;
    ns_profile_frame_boundary();
    ns_expect(ns_profile.frame_seq == before, "empty frames are not published");
    ns_profile_reset();

    return 0;
}
