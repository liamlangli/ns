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

void ns_profile_enable(f64 start_ms) {
    ns_profile.start_ms = start_ms;
    ns_profile.enabled = true;
}

static ns_bool ns_str_eq(ns_str a, ns_str b) {
    if (a.len != b.len) return false;
    for (i32 i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) return false;
    }
    return true;
}

static void ns_profile_record_event(u8 kind, ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms) {
    if (ns_profile.event_count < NS_PROFILE_MAX_EVENTS) {
        ns_profile.events[ns_profile.event_count++] = (ns_profile_event){
            .kind = kind,
            .depth = depth,
            .start_ms = start_ms - ns_profile.start_ms,
            .elapsed_ms = elapsed_ms,
            .name = name,
            .lib = lib,
        };
    } else {
        ns_profile.events_dropped++;
    }
}

void ns_profile_record_ffi(ns_str name, ns_str lib, f64 start_ms, f64 elapsed_ms) {
    if (!ns_profile.enabled) return;

    ns_profile.ffi_calls++;
    ns_profile.ffi_total_ms += elapsed_ms;

    ns_profile_record_event(NS_PROFILE_EVENT_FFI, name, lib, -1, start_ms, elapsed_ms);

    ns_profile_fn_stat *slot = ns_null;
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ns_profile.fns[i];
        if (ns_str_eq(s->name, name) && ns_str_eq(s->lib, lib)) {
            slot = s;
            break;
        }
    }

    if (slot == ns_null) {
        if (ns_profile.fn_count >= NS_PROFILE_MAX_FNS) {
            ns_profile.fns_dropped++;
            return;
        }
        slot = &ns_profile.fns[ns_profile.fn_count++];
        slot->name = name;
        slot->lib = lib;
        slot->calls = 0;
        slot->total_ms = 0.0;
        slot->min_ms = elapsed_ms;
        slot->max_ms = elapsed_ms;
    }

    slot->calls++;
    slot->total_ms += elapsed_ms;
    if (elapsed_ms < slot->min_ms) slot->min_ms = elapsed_ms;
    if (elapsed_ms > slot->max_ms) slot->max_ms = elapsed_ms;
}

void ns_profile_record_scope(ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms) {
    if (!ns_profile.enabled) return;

    ns_profile.scope_calls++;
    ns_profile_record_event(NS_PROFILE_EVENT_SCOPE, name, lib, depth, start_ms, elapsed_ms);
}
