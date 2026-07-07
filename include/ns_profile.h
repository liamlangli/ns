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

typedef struct ns_profile_fn_stat {
    ns_str name;   // foreign symbol name
    ns_str lib;    // owning native library (may be empty)
    u64 calls;     // number of invocations
    f64 total_ms;  // cumulative wall-clock time spent inside the call
    f64 min_ms;    // fastest single call
    f64 max_ms;    // slowest single call
} ns_profile_fn_stat;

typedef struct ns_profile_state {
    ns_bool enabled;

    // FFI aggregate across every recorded native call.
    u64 ffi_calls;
    f64 ffi_total_ms;

    // Per-symbol table. Linear scan on record: a program's distinct FFI
    // surface is small, and the cap keeps the cost bounded even for pathological
    // input. Calls to distinct symbols past the cap fold into `fns_dropped`.
    ns_profile_fn_stat fns[NS_PROFILE_MAX_FNS];
    i32 fn_count;
    u64 fns_dropped;
} ns_profile_state;

extern ns_profile_state ns_profile;

// Monotonic wall clock in milliseconds.
f64 ns_profile_now_ms(void);

// Enable collection. Recorders below are no-ops until this is called.
void ns_profile_enable(void);

// Record one completed FFI call. `elapsed_ms` is the wall-clock time spent in
// the native call. No-op unless profiling is enabled.
void ns_profile_record_ffi(ns_str name, ns_str lib, f64 elapsed_ms);
