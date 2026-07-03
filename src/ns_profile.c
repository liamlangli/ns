#include "ns_profile.h"

#include <signal.h>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <time.h>
#endif

typedef struct ns_profile_frame {
    ns_str name;
    ns_str cat;
    u64 start;
} ns_profile_frame;

typedef struct ns_profile_event {
    ns_str name;
    ns_str cat;
    u64 start;
    u64 dur;
    i32 depth;
} ns_profile_event;

ns_bool ns_profile_on = false;

static ns_str ns_profile_path = {0};
static u64 ns_profile_t0 = 0;
static ns_bool ns_profile_saved = false;
static ns_profile_frame *ns_profile_open = ns_null;
static ns_profile_event *ns_profile_events = ns_null;

// Monotonic clock for durations; the script-facing os_time* fns use the
// wall clock and are unsuitable for profiling.
static u64 ns_profile_now(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)(t.QuadPart * 1000000000ull / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

// Debug builds abort on eval errors (ns_error asserts), which skips atexit;
// flush the trace on SIGABRT too so an erroring run still leaves a file.
static void ns_profile_on_abort(int sig) {
    ns_unused(sig);
    ns_profile_save();
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void ns_profile_begin(ns_str out_path) {
    if (ns_profile_on) return;
    ns_profile_on = true;
    ns_profile_path = out_path;
    ns_profile_t0 = ns_profile_now();
    atexit(ns_profile_save);
    signal(SIGABRT, ns_profile_on_abort);
}

void ns_profile_enter(ns_str name, ns_str cat) {
    if (!ns_profile_on) return;
    ns_profile_frame frame = (ns_profile_frame){.name = name, .cat = cat, .start = ns_profile_now()};
    ns_array_push(ns_profile_open, frame);
}

void ns_profile_exit(void) {
    if (!ns_profile_on || ns_array_length(ns_profile_open) == 0) return;
    u64 now = ns_profile_now();
    ns_profile_frame frame = ns_array_pop(ns_profile_open);
    ns_profile_event e = (ns_profile_event){.name = frame.name, .cat = frame.cat, .start = frame.start, .dur = now - frame.start, .depth = (i32)ns_array_length(ns_profile_open)};
    ns_array_push(ns_profile_events, e);
}

static void ns_profile_write_escaped(FILE *f, ns_str s) {
    for (i32 i = 0; i < s.len; ++i) {
        u8 c = (u8)s.data[i];
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

void ns_profile_save(void) {
    if (!ns_profile_on || ns_profile_saved) return;
    ns_profile_saved = true;

    // Close frames the evaluator abandoned on error paths so every enter
    // still yields a complete event.
    u64 now = ns_profile_now();
    while (ns_array_length(ns_profile_open) > 0) {
        ns_profile_frame frame = ns_array_pop(ns_profile_open);
        ns_profile_event e = (ns_profile_event){.name = frame.name, .cat = frame.cat, .start = frame.start, .dur = now - frame.start, .depth = (i32)ns_array_length(ns_profile_open)};
        ns_array_push(ns_profile_events, e);
    }

    FILE *f = fopen(ns_profile_path.data, "wb");
    if (!f) {
        ns_error("profile", "failed to open %.*s for writing.\n", ns_profile_path.len, ns_profile_path.data);
        return;
    }

    i32 count = (i32)ns_array_length(ns_profile_events);
    fprintf(f, "{\"traceEvents\":[\n");
    for (i32 i = 0; i < count; ++i) {
        ns_profile_event *e = &ns_profile_events[i];
        fprintf(f, "{\"name\":\"");
        ns_profile_write_escaped(f, e->name);
        fprintf(f, "\",\"cat\":\"");
        ns_profile_write_escaped(f, e->cat.len > 0 ? e->cat : ns_str_cstr("main"));
        fprintf(f, "\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":1,\"args\":{\"depth\":%d}}%s\n",
            (f64)(e->start - ns_profile_t0) / 1000.0, (f64)e->dur / 1000.0, e->depth, i + 1 < count ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);

    ns_info("profile", "wrote %.*s (%d events)\n", ns_profile_path.len, ns_profile_path.data, count);
}
