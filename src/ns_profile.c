#include "ns_profile.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

ns_profile_state ns_profile = {0};

f64 ns_profile_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return ((f64)counter.QuadPart * 1000.0) / (f64)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((f64)ts.tv_sec * 1000.0) + ((f64)ts.tv_nsec / 1000000.0);
#endif
}

void ns_profile_reset(void) {
    memset(&ns_profile, 0, sizeof(ns_profile));
}

void ns_profile_enable(f64 start_ms) {
    ns_profile.start_ms = start_ms;
    ns_profile.enabled = true;
}

static u32 ns_profile_hash_str(ns_str s) {
    u32 h = 2166136261u;
    for (i32 i = 0; i < s.len; i++) {
        h ^= (u32)(u8)s.data[i];
        h *= 16777619u;
    }
    return h;
}

static ns_bool ns_profile_str_eq(ns_str a, ns_str b) {
    if (a.len != b.len) return false;
    if (a.data == b.data) return true;
    for (i32 i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) return false;
    }
    return true;
}

static i32 ns_profile_fn_get(ns_str name, ns_str lib, u8 kind) {
    u32 h = ns_profile_hash_str(name) ^ (ns_profile_hash_str(lib) * 0x9e3779b9u) ^ (u32)kind;
    for (i32 n = 0; n < NS_PROFILE_FN_HASH; n++) {
        i32 slot = (i32)((h + (u32)n) & (NS_PROFILE_FN_HASH - 1));
        i32 idx = ns_profile.fn_hash[slot];
        if (idx == 0) {
            if (ns_profile.fn_count >= NS_PROFILE_MAX_FNS) {
                ns_profile.fns_dropped++;
                return -1;
            }
            i32 fi = ns_profile.fn_count++;
            ns_profile.fns[fi] = (ns_profile_fn_stat){
                .name = name,
                .lib = lib,
                .kind = kind,
                .min_ms = 0.0,
                .max_ms = 0.0,
            };
            ns_profile.fn_hash[slot] = fi + 1;
            return fi;
        }
        i32 fi = idx - 1;
        ns_profile_fn_stat *s = &ns_profile.fns[fi];
        if (s->kind == kind && ns_profile_str_eq(s->name, name) && ns_profile_str_eq(s->lib, lib)) {
            return fi;
        }
    }
    ns_profile.fns_dropped++;
    return -1;
}

static i32 ns_profile_flame_get(i32 parent, i32 fn_index) {
    if (fn_index < 0) return -1;
    u32 h = ((u32)(parent + 2) * 0x9e3779b9u) ^ ((u32)(fn_index + 1) * 0x85ebca6bu);
    for (i32 n = 0; n < NS_PROFILE_FLAME_HASH; n++) {
        i32 slot = (i32)((h + (u32)n) & (NS_PROFILE_FLAME_HASH - 1));
        i32 idx = ns_profile.flame_hash[slot];
        if (idx == 0) {
            if (ns_profile.flame_count >= NS_PROFILE_MAX_FLAME) {
                ns_profile.flames_dropped++;
                return -1;
            }
            i32 fi = ns_profile.flame_count++;
            ns_profile.flames[fi] = (ns_profile_flame){
                .parent = parent,
                .fn_index = fn_index,
            };
            ns_profile.flame_hash[slot] = fi + 1;
            return fi;
        }
        i32 fi = idx - 1;
        if (ns_profile.flames[fi].parent == parent && ns_profile.flames[fi].fn_index == fn_index) {
            return fi;
        }
    }
    ns_profile.flames_dropped++;
    return -1;
}

static void ns_profile_fn_add(i32 fn_index, f64 elapsed_ms, f64 self_ms) {
    if (fn_index < 0) return;
    ns_profile_fn_stat *s = &ns_profile.fns[fn_index];
    if (s->calls == 0) {
        s->min_ms = elapsed_ms;
        s->max_ms = elapsed_ms;
    } else {
        if (elapsed_ms < s->min_ms) s->min_ms = elapsed_ms;
        if (elapsed_ms > s->max_ms) s->max_ms = elapsed_ms;
    }
    s->calls++;
    s->total_ms += elapsed_ms;
    s->self_ms += self_ms;
}

static void ns_profile_record_event(u8 kind, ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms, f64 self_ms) {
    ns_profile_event ev = {
        .kind = kind,
        .depth = depth,
        .start_ms = start_ms - ns_profile.start_ms,
        .elapsed_ms = elapsed_ms,
        .self_ms = self_ms,
        .name = name,
        .lib = lib,
    };
    ns_profile.events[ns_profile.event_head] = ev;
    ns_profile.event_head = (ns_profile.event_head + 1) % NS_PROFILE_MAX_EVENTS;
    if (ns_profile.event_count < NS_PROFILE_MAX_EVENTS) {
        ns_profile.event_count++;
    } else {
        ns_profile.events_dropped++;
    }
}

void ns_profile_scope_enter(ns_str name, ns_str lib) {
    if (!ns_profile.enabled) return;
    if (ns_profile.open_len >= NS_PROFILE_MAX_STACK) {
        ns_profile.stack_overflows++;
        return;
    }
    i32 fn_index = ns_profile_fn_get(name, lib, NS_PROFILE_EVENT_SCOPE);
    i32 parent = ns_profile.open_len > 0 ? ns_profile.open[ns_profile.open_len - 1].flame_index : -1;
    i32 flame_index = ns_profile_flame_get(parent, fn_index);
    ns_profile.open[ns_profile.open_len++] = (ns_profile_open){
        .fn_index = fn_index,
        .flame_index = flame_index,
        .child_ms = 0.0,
    };
}

void ns_profile_record_ffi(ns_str name, ns_str lib, f64 start_ms, f64 elapsed_ms) {
    if (!ns_profile.enabled) return;

    ns_profile.ffi_calls++;
    ns_profile.ffi_total_ms += elapsed_ms;

    i32 fn_index = ns_profile_fn_get(name, lib, NS_PROFILE_EVENT_FFI);
    i32 parent = ns_profile.open_len > 0 ? ns_profile.open[ns_profile.open_len - 1].flame_index : -1;
    i32 flame_index = ns_profile_flame_get(parent, fn_index);
    i32 depth = ns_profile.open_len;
    if (ns_profile.open_len > 0) {
        ns_profile.open[ns_profile.open_len - 1].child_ms += elapsed_ms;
    }
    ns_profile_fn_add(fn_index, elapsed_ms, elapsed_ms);
    if (flame_index >= 0) {
        ns_profile.flames[flame_index].calls++;
        ns_profile.flames[flame_index].total_ms += elapsed_ms;
        ns_profile.flames[flame_index].self_ms += elapsed_ms;
    }
    ns_str ev_name = fn_index >= 0 ? ns_profile.fns[fn_index].name : name;
    ns_str ev_lib = fn_index >= 0 ? ns_profile.fns[fn_index].lib : lib;
    ns_profile_record_event(NS_PROFILE_EVENT_FFI, ev_name, ev_lib, depth, start_ms, elapsed_ms, elapsed_ms);
}

void ns_profile_record_scope(ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms) {
    if (!ns_profile.enabled) return;

    ns_profile.scope_calls++;

    i32 fn_index = -1;
    i32 flame_index = -1;
    f64 self_ms = elapsed_ms;

    if (ns_profile.open_len > 0) {
        ns_profile_open o = ns_profile.open[--ns_profile.open_len];
        fn_index = o.fn_index;
        flame_index = o.flame_index;
        self_ms = elapsed_ms - o.child_ms;
        if (self_ms < 0.0) self_ms = 0.0;
        if (ns_profile.open_len > 0) {
            ns_profile.open[ns_profile.open_len - 1].child_ms += elapsed_ms;
        }
        depth = ns_profile.open_len;
    } else {
        fn_index = ns_profile_fn_get(name, lib, NS_PROFILE_EVENT_SCOPE);
        flame_index = ns_profile_flame_get(-1, fn_index);
    }

    ns_profile.scope_self_ms += self_ms;
    ns_profile_fn_add(fn_index, elapsed_ms, self_ms);
    if (flame_index >= 0) {
        ns_profile.flames[flame_index].calls++;
        ns_profile.flames[flame_index].total_ms += elapsed_ms;
        ns_profile.flames[flame_index].self_ms += self_ms;
    }

    ns_str ev_name = fn_index >= 0 ? ns_profile.fns[fn_index].name : name;
    ns_str ev_lib = fn_index >= 0 ? ns_profile.fns[fn_index].lib : lib;
    ns_profile_record_event(NS_PROFILE_EVENT_SCOPE, ev_name, ev_lib, depth, start_ms, elapsed_ms, self_ms);
}

static int ns_profile_fn_cmp(const void *a, const void *b) {
    const ns_profile_fn_stat *x = a;
    const ns_profile_fn_stat *y = b;
    if (x->self_ms < y->self_ms) return 1;
    if (x->self_ms > y->self_ms) return -1;
    if (x->total_ms < y->total_ms) return 1;
    if (x->total_ms > y->total_ms) return -1;
    if (x->calls < y->calls) return 1;
    if (x->calls > y->calls) return -1;
    return 0;
}

static int ns_profile_event_cmp(const void *a, const void *b) {
    const ns_profile_event *x = a;
    const ns_profile_event *y = b;
    if (x->start_ms < y->start_ms) return -1;
    if (x->start_ms > y->start_ms) return 1;
    if (x->kind == NS_PROFILE_EVENT_SCOPE && y->kind != NS_PROFILE_EVENT_SCOPE) return -1;
    if (x->kind != NS_PROFILE_EVENT_SCOPE && y->kind == NS_PROFILE_EVENT_SCOPE) return 1;
    if (x->depth < y->depth) return -1;
    if (x->depth > y->depth) return 1;
    return 0;
}

static int ns_profile_flame_self_cmp(const void *a, const void *b) {
    const i32 *ia = a;
    const i32 *ib = b;
    const ns_profile_flame *x = &ns_profile.flames[*ia];
    const ns_profile_flame *y = &ns_profile.flames[*ib];
    if (x->self_ms < y->self_ms) return 1;
    if (x->self_ms > y->self_ms) return -1;
    if (x->total_ms < y->total_ms) return 1;
    if (x->total_ms > y->total_ms) return -1;
    return 0;
}

static void ns_profile_write_symbol(FILE *f, ns_str lib, ns_str name) {
    if (lib.len > 0) fprintf(f, "%.*s::", lib.len, lib.data);
    fprintf(f, "%.*s", name.len, name.data);
}

static void ns_profile_write_stack(FILE *f, i32 flame_index) {
    i32 chain[NS_PROFILE_MAX_STACK];
    i32 n = 0;
    for (i32 i = flame_index; i >= 0 && n < NS_PROFILE_MAX_STACK; i = ns_profile.flames[i].parent) {
        chain[n++] = i;
    }
    for (i32 i = n - 1; i >= 0; i--) {
        if (i < n - 1) fputc(';', f);
        ns_profile_fn_stat *s = &ns_profile.fns[ns_profile.flames[chain[i]].fn_index];
        ns_profile_write_symbol(f, s->lib, s->name);
    }
}

static void ns_profile_sort_events(void) {
    if (ns_profile.event_count <= 1) return;
    if (ns_profile.events_dropped == 0) {
        qsort(ns_profile.events, (size_t)ns_profile.event_count, sizeof(ns_profile_event), ns_profile_event_cmp);
        ns_profile.event_head = ns_profile.event_count % NS_PROFILE_MAX_EVENTS;
        return;
    }
    // The ring is full and wrapped. Rotate so index 0 is the oldest sample,
    // then sort by start time for a stable dump.
    if (ns_profile.event_head != 0) {
        ns_profile_event *tmp = (ns_profile_event *)malloc(sizeof(ns_profile_event) * (size_t)NS_PROFILE_MAX_EVENTS);
        if (tmp) {
            i32 n = NS_PROFILE_MAX_EVENTS - ns_profile.event_head;
            memcpy(tmp, ns_profile.events + ns_profile.event_head, sizeof(ns_profile_event) * (size_t)n);
            memcpy(tmp + n, ns_profile.events, sizeof(ns_profile_event) * (size_t)ns_profile.event_head);
            memcpy(ns_profile.events, tmp, sizeof(ns_profile_event) * (size_t)NS_PROFILE_MAX_EVENTS);
            free(tmp);
            ns_profile.event_head = 0;
        }
    }
    qsort(ns_profile.events, (size_t)ns_profile.event_count, sizeof(ns_profile_event), ns_profile_event_cmp);
}

void ns_profile_write_text(FILE *f, f64 elapsed_ms, i32 argc, i8 **argv) {
    f64 ffi_ms = ns_profile.ffi_total_ms;
    f64 ffi_pct = elapsed_ms > 0.0 ? (ffi_ms / elapsed_ms) * 100.0 : 0.0;
    i32 ffi_event_count = 0;
    i32 scope_event_count = 0;
    for (i32 i = 0; i < ns_profile.event_count; i++) {
        if (ns_profile.events[i].kind == NS_PROFILE_EVENT_SCOPE) scope_event_count++;
        else ffi_event_count++;
    }

    i32 scope_symbols = 0;
    i32 ffi_symbols = 0;
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        if (ns_profile.fns[i].kind == NS_PROFILE_EVENT_SCOPE) scope_symbols++;
        else ffi_symbols++;
    }

    fprintf(f, "format: ns-profile-v4\n");
    fprintf(f, "elapsed_ms: %.3f\n", elapsed_ms);
    fprintf(f, "ffi_calls: %llu\n", (unsigned long long)ns_profile.ffi_calls);
    fprintf(f, "ffi_ms: %.3f\n", ffi_ms);
    fprintf(f, "ffi_pct: %.1f\n", ffi_pct);
    fprintf(f, "ffi_symbols: %d\n", ffi_symbols);
    fprintf(f, "ffi_events: %d\n", ffi_event_count);
    fprintf(f, "scope_calls: %llu\n", (unsigned long long)ns_profile.scope_calls);
    fprintf(f, "scope_self_ms: %.3f\n", ns_profile.scope_self_ms);
    fprintf(f, "scope_symbols: %d\n", scope_symbols);
    fprintf(f, "scope_events: %d\n", scope_event_count);
    fprintf(f, "timeline_events: %d\n", ns_profile.event_count);
    fprintf(f, "flame_frames: %d\n", ns_profile.flame_count);
    fprintf(f, "argv:");
    for (i32 i = 0; i < argc; i++) fprintf(f, " %s", argv[i]);
    fprintf(f, "\n");

    ns_profile_sort_events();

    fprintf(f, "timeline: kind depth start_ms duration_ms self_ms symbol\n");
    for (i32 i = 0; i < ns_profile.event_count; i++) {
        ns_profile_event *e = &ns_profile.events[i];
        if (e->kind == NS_PROFILE_EVENT_SCOPE) {
            fprintf(f, "scope_event: %d %.3f %.3f %.3f ", e->depth, e->start_ms, e->elapsed_ms, e->self_ms);
        } else {
            fprintf(f, "ffi_event: %d %.3f %.3f %.3f ", e->depth, e->start_ms, e->elapsed_ms, e->self_ms);
        }
        ns_profile_write_symbol(f, e->lib, e->name);
        fputc('\n', f);
    }
    if (ns_profile.events_dropped > 0) {
        fprintf(f, "timeline_events_dropped: %llu\n", (unsigned long long)ns_profile.events_dropped);
    }
    if (ns_profile.stack_overflows > 0) {
        fprintf(f, "stack_overflows: %llu\n", (unsigned long long)ns_profile.stack_overflows);
    }

    ns_profile_fn_stat ordered[NS_PROFILE_MAX_FNS];
    memcpy(ordered, ns_profile.fns, sizeof(ns_profile_fn_stat) * (size_t)ns_profile.fn_count);
    qsort(ordered, (size_t)ns_profile.fn_count, sizeof(ns_profile_fn_stat), ns_profile_fn_cmp);
    fprintf(f, "fn_table: kind calls total_ms self_ms avg_ms min_ms max_ms symbol\n");
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ordered[i];
        f64 avg_ms = s->calls ? s->total_ms / (f64)s->calls : 0.0;
        const char *kind = s->kind == NS_PROFILE_EVENT_SCOPE ? "scope" : "ffi";
        fprintf(f, "fn: %s %llu %.3f %.3f %.4f %.4f %.4f ", kind, (unsigned long long)s->calls, s->total_ms, s->self_ms,
                avg_ms, s->min_ms, s->max_ms);
        ns_profile_write_symbol(f, s->lib, s->name);
        fputc('\n', f);
    }
    if (ns_profile.fns_dropped > 0) {
        fprintf(f, "fn_dropped: %llu\n", (unsigned long long)ns_profile.fns_dropped);
    }
    // Keep a v3-compatible alias so older viewers still see an FFI table.
    fprintf(f, "ffi_table: calls total_ms avg_ms min_ms max_ms symbol\n");
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ordered[i];
        if (s->kind != NS_PROFILE_EVENT_FFI) continue;
        f64 avg_ms = s->calls ? s->total_ms / (f64)s->calls : 0.0;
        fprintf(f, "ffi: %llu %.3f %.4f %.4f %.4f ", (unsigned long long)s->calls, s->total_ms, avg_ms, s->min_ms, s->max_ms);
        ns_profile_write_symbol(f, s->lib, s->name);
        fputc('\n', f);
    }

    fprintf(f, "flame: calls total_ms self_ms stack\n");
    i32 order[NS_PROFILE_MAX_FLAME];
    for (i32 i = 0; i < ns_profile.flame_count; i++) order[i] = i;
    qsort(order, (size_t)ns_profile.flame_count, sizeof(i32), ns_profile_flame_self_cmp);
    for (i32 i = 0; i < ns_profile.flame_count; i++) {
        ns_profile_flame *fl = &ns_profile.flames[order[i]];
        fprintf(f, "flame: %llu %.3f %.3f ", (unsigned long long)fl->calls, fl->total_ms, fl->self_ms);
        ns_profile_write_stack(f, order[i]);
        fputc('\n', f);
    }
    if (ns_profile.flames_dropped > 0) {
        fprintf(f, "flame_dropped: %llu\n", (unsigned long long)ns_profile.flames_dropped);
    }
}

static void ns_profile_write_json_name(FILE *f, ns_str lib, ns_str name) {
    fputc('"', f);
    if (lib.len > 0) {
        for (i32 i = 0; i < lib.len; i++) {
            unsigned char c = (unsigned char)lib.data[i];
            if (c == '"' || c == '\\') {
                fputc('\\', f);
                fputc((int)c, f);
            } else if (c < 32) {
                fprintf(f, "\\u%04x", c);
            } else {
                fputc((int)c, f);
            }
        }
        fputs("::", f);
    }
    for (i32 i = 0; i < name.len; i++) {
        unsigned char c = (unsigned char)name.data[i];
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c == '\n') {
            fputs("\\n", f);
        } else if (c < 32) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc((int)c, f);
        }
    }
    fputc('"', f);
}

ns_bool ns_profile_write_chrome_path(const char *path, f64 elapsed_ms) {
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"displayTimeUnit\": \"ms\",\n");
    fprintf(f, "  \"otherData\": {\n");
    fprintf(f, "    \"format\": \"ns-profile-v4\",\n");
    fprintf(f, "    \"elapsed_ms\": %.3f\n", elapsed_ms);
    fprintf(f, "  },\n");
    fprintf(f, "  \"traceEvents\": [\n");
    fprintf(f, "    {\"name\": \"process_name\", \"ph\": \"M\", \"pid\": 1, \"args\": {\"name\": \"ns\"}},\n");
    fprintf(f, "    {\"name\": \"thread_name\", \"ph\": \"M\", \"pid\": 1, \"tid\": 1, \"args\": {\"name\": \"interpreter\"}}");

    for (i32 i = 0; i < ns_profile.event_count; i++) {
        ns_profile_event *e = &ns_profile.events[i];
        fputs(",\n    {\"name\": ", f);
        ns_profile_write_json_name(f, e->lib, e->name);
        fprintf(f, ", \"cat\": \"%s\", \"ph\": \"X\", \"ts\": %.3f, \"dur\": %.3f, \"pid\": 1, \"tid\": 1, \"args\": {\"self_ms\": %.3f, \"depth\": %d}}",
                e->kind == NS_PROFILE_EVENT_SCOPE ? "scope" : "ffi",
                e->start_ms * 1000.0, e->elapsed_ms * 1000.0, e->self_ms, e->depth);
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    return true;
}

static const char *ns_profile_pct_color(f64 pct) {
    if (pct >= 25.0) return ns_color_err;
    if (pct >= 10.0) return ns_color_wrn;
    if (pct >= 3.0) return ns_color_log;
    return ns_color_ign;
}

void ns_profile_print_summary(FILE *out, f64 elapsed_ms) {
    f64 ffi_ms = ns_profile.ffi_total_ms;
    f64 ffi_pct = elapsed_ms > 0.0 ? (ffi_ms / elapsed_ms) * 100.0 : 0.0;
    fprintf(out, "\n");
    fprintf(out, "profile  %.3f ms total\n", elapsed_ms);
    fprintf(out, "  vm scopes   %llu   self %.3f ms\n", (unsigned long long)ns_profile.scope_calls, ns_profile.scope_self_ms);
    fprintf(out, "  ffi calls   %llu   %s%.3f ms / %.1f%%%s\n", (unsigned long long)ns_profile.ffi_calls,
            ns_profile_pct_color(ffi_pct), ffi_ms, ffi_pct, ns_color_nil);
    fprintf(out, "  timeline    %d events", ns_profile.event_count);
    if (ns_profile.events_dropped > 0) {
        fprintf(out, " (%llu older dropped, keeping latest window)", (unsigned long long)ns_profile.events_dropped);
    }
    fprintf(out, "\n");
    fprintf(out, "  stacks      %d unique\n", ns_profile.flame_count);
    fprintf(out, "  share       %s>=25%%%s  %s>=10%%%s  %s>=3%%%s  %s<3%%%s\n",
            ns_color_err, ns_color_nil, ns_color_wrn, ns_color_nil,
            ns_color_log, ns_color_nil, ns_color_ign, ns_color_nil);

    ns_profile_fn_stat ordered[NS_PROFILE_MAX_FNS];
    memcpy(ordered, ns_profile.fns, sizeof(ns_profile_fn_stat) * (size_t)ns_profile.fn_count);
    qsort(ordered, (size_t)ns_profile.fn_count, sizeof(ns_profile_fn_stat), ns_profile_fn_cmp);

    fprintf(out, "\nhot functions by self time:\n");
    i32 fn_show = ns_profile.fn_count < 12 ? ns_profile.fn_count : 12;
    for (i32 i = 0; i < fn_show; i++) {
        ns_profile_fn_stat *s = &ordered[i];
        f64 pct = elapsed_ms > 0.0 ? (s->self_ms / elapsed_ms) * 100.0 : 0.0;
        const char *kind = s->kind == NS_PROFILE_EVENT_SCOPE ? "scope" : "ffi  ";
        fprintf(out, "  %s%10.3f ms  %5.1f%%  %8llux  %s  ", ns_profile_pct_color(pct), s->self_ms, pct,
                (unsigned long long)s->calls, kind);
        ns_profile_write_symbol(out, s->lib, s->name);
        fprintf(out, "%s\n", ns_color_nil);
    }

    fprintf(out, "\nhot stacks by self time:\n");
    i32 order[NS_PROFILE_MAX_FLAME];
    for (i32 i = 0; i < ns_profile.flame_count; i++) order[i] = i;
    qsort(order, (size_t)ns_profile.flame_count, sizeof(i32), ns_profile_flame_self_cmp);
    i32 st_show = ns_profile.flame_count < 8 ? ns_profile.flame_count : 8;
    for (i32 i = 0; i < st_show; i++) {
        ns_profile_flame *fl = &ns_profile.flames[order[i]];
        f64 pct = elapsed_ms > 0.0 ? (fl->self_ms / elapsed_ms) * 100.0 : 0.0;
        fprintf(out, "  %s%10.3f ms  %5.1f%%  ", ns_profile_pct_color(pct), fl->self_ms, pct);
        ns_profile_write_stack(out, order[i]);
        fprintf(out, "%s\n", ns_color_nil);
    }

    fprintf(out, "\nopen ns.profile.json in https://ui.perfetto.dev\n");
    fprintf(out, "or run: ns profiler\n");
}
