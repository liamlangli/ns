#include "ns_test.h"
#include "ns_profile.h"

static ns_str S(const char *s) { return ns_str_cstr((char *)s); }

int main() {
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
        ns_profile_write_text(tf, 50.0, 0, ns_null);
        fclose(tf);
    }

    FILE *in = fopen(text_path, "r");
    ns_expect(in != ns_null, "reread text profile");
    char line[512];
    ns_bool saw_v5 = false;
    ns_bool saw_fn = false;
    ns_bool saw_flame = false;
    ns_bool saw_stack = false;
    ns_bool saw_thread = false;
    ns_bool saw_thread_event = false;
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (strncmp(line, "format: ns-profile-v5", 21) == 0) saw_v5 = true;
            if (strncmp(line, "fn: scope", 9) == 0) saw_fn = true;
            if (strncmp(line, "flame:", 6) == 0) saw_flame = true;
            if (strstr(line, "main;mid;leaf")) saw_stack = true;
            if (strncmp(line, "thread: 0 main", 14) == 0) saw_thread = true;
            if (strncmp(line, "scope_event: main ", 18) == 0) saw_thread_event = true;
        }
        fclose(in);
    }
    ns_expect(saw_v5, "text format ns-profile-v5");
    ns_expect(saw_fn, "fn table rows");
    ns_expect(saw_flame, "folded flame rows");
    ns_expect(saw_stack, "folded stack main;mid;leaf");
    ns_expect(saw_thread, "thread table lists main");
    ns_expect(saw_thread_event, "scope events carry thread name");

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

    // Timeline is a linearly growing array: keep every sample past the old
    // fixed ring capacity instead of overwriting oldest entries.
    ns_profile_reset();
    ns_profile_enable(0.0);
    for (i32 i = 0; i < 300000; i++) {
        ns_profile_record_ffi(S("grow"), S("test"), (f64)i, 0.001);
    }
    ns_expect(ns_array_length(ns_profile.events) == 300000, "timeline keeps all samples");
    ns_expect(ns_profile.events[0].start_ms >= -0.001 && ns_profile.events[0].start_ms <= 0.001, "oldest sample retained");
    ns_expect(ns_profile.events[299999].start_ms > 299998.0, "newest sample retained");
    ns_profile_reset();

    return 0;
}
