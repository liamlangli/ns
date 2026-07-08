#pragma once

#include "ns_type.h"

// Lightweight runtime profiler, toggled by the `--profile` CLI flag.
//
// The whole-run wall clock alone rarely explains where a native (nscode)
// program spends its time: the interpreter itself is cheap, and the real cost
// hides behind FFI calls into the platform libraries (view/gpu/ui/os/net...).
// This module keeps a per-symbol breakdown of those native calls so the
// emitted ns.profile can show call counts and time spent per foreign function,
// not just a single elapsed number.

#define NS_PROFILE_MAX_FNS 512
#define NS_PROFILE_MAX_EVENTS 20000

#define NS_PROFILE_EVENT_FFI 1
#define NS_PROFILE_EVENT_SCOPE 2

typedef struct ns_profile_fn_stat {
    ns_str name;   // foreign symbol name
    ns_str lib;    // owning native library (may be empty)
    u64 calls;     // number of invocations
    f64 total_ms;  // cumulative wall-clock time spent inside the call
    f64 min_ms;    // fastest single call
    f64 max_ms;    // slowest single call
} ns_profile_fn_stat;

typedef struct ns_profile_event {
    u8 kind;        // NS_PROFILE_EVENT_*
    i32 depth;      // VM call depth for scope events, -1 for FFI events
    f64 start_ms;   // milliseconds since profile start
    f64 elapsed_ms; // wall-clock time spent inside the call
    ns_str name;    // symbol name
    ns_str lib;     // owning native library (may be empty)
} ns_profile_event;

typedef struct ns_profile_state {
    ns_bool enabled;
    f64 start_ms;

    // FFI aggregate across every recorded native call.
    u64 ffi_calls;
    f64 ffi_total_ms;

    // VM function scope calls. These are timeline events only; they make nested
    // interpreted call frames visible in the profile viewer.
    u64 scope_calls;

    // Per-symbol table. Linear scan on record: a program's distinct FFI
    // surface is small, and the cap keeps the cost bounded even for pathological
    // input. Calls to distinct symbols past the cap fold into `fns_dropped`.
    ns_profile_fn_stat fns[NS_PROFILE_MAX_FNS];
    i32 fn_count;
    u64 fns_dropped;

    // Per-call timeline, used by the native profile viewer to render a flame
    // chart style time axis. Events past the cap are counted but not retained.
    ns_profile_event events[NS_PROFILE_MAX_EVENTS];
    i32 event_count;
    u64 events_dropped;
} ns_profile_state;

extern ns_profile_state ns_profile;

// Monotonic wall clock in milliseconds.
f64 ns_profile_now_ms(void);

// Enable collection. Recorders below are no-ops until this is called.
void ns_profile_enable(f64 start_ms);

// Record one completed FFI call. `start_ms` is the monotonic clock value taken
// immediately before the native call, and `elapsed_ms` is the wall-clock time
// spent inside it. No-op unless profiling is enabled.
void ns_profile_record_ffi(ns_str name, ns_str lib, f64 start_ms, f64 elapsed_ms);

// Record one completed interpreted VM function scope.
void ns_profile_record_scope(ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms);
