#pragma once

#include "ns_type.h"

// Runtime and compiler profiler, toggled by `--profile` or the `ns profile`
// command.
//
// Collection covers nested VM functions, build phases, and FFI calls. This
// module keeps:
//
//   - a per-symbol table with inclusive (total) and exclusive (self) time
//     for both interpreted functions and native calls
//   - a call-tree of unique stacks used to emit a folded flamechart
//   - a ring buffer of recent complete events for a time-axis flamechart
//
// The flame tree is complete for the whole run. Timeline events past the
// ring capacity drop the oldest samples so a long interactive session still
// has a recent window to inspect.

#define NS_PROFILE_MAX_FNS 4096
#define NS_PROFILE_FN_HASH 8192
#define NS_PROFILE_MAX_EVENTS 65536
#define NS_PROFILE_MAX_STACK 128
#define NS_PROFILE_MAX_FLAME 4096
#define NS_PROFILE_FLAME_HASH 8192

#define NS_PROFILE_EVENT_FFI 1
#define NS_PROFILE_EVENT_SCOPE 2

typedef struct ns_profile_fn_stat {
    ns_str name;   // function or foreign symbol name
    ns_str lib;    // owning module / native library (may be empty)
    u8 kind;       // NS_PROFILE_EVENT_SCOPE or NS_PROFILE_EVENT_FFI
    u64 calls;     // number of invocations
    f64 total_ms;  // cumulative inclusive wall-clock time
    f64 self_ms;   // cumulative exclusive wall-clock time
    f64 min_ms;    // fastest single inclusive call
    f64 max_ms;    // slowest single inclusive call
} ns_profile_fn_stat;

typedef struct ns_profile_event {
    u8 kind;        // NS_PROFILE_EVENT_*
    i32 depth;      // stack depth of this frame (0 = root)
    f64 start_ms;   // milliseconds since profile start
    f64 elapsed_ms; // inclusive wall-clock time
    f64 self_ms;    // exclusive wall-clock time
    ns_str name;    // symbol name
    ns_str lib;     // owning module / native library (may be empty)
} ns_profile_event;

typedef struct ns_profile_flame {
    i32 parent;    // parent flame index, or -1 for a root
    i32 fn_index;  // into fns[]
    u64 calls;
    f64 total_ms;
    f64 self_ms;
} ns_profile_flame;

typedef struct ns_profile_open {
    i32 fn_index;
    i32 flame_index;
    f64 child_ms;
} ns_profile_open;

typedef struct ns_profile_state {
    ns_bool enabled;
    f64 start_ms;

    u64 ffi_calls;
    f64 ffi_total_ms;

    u64 scope_calls;
    f64 scope_self_ms;

    ns_profile_fn_stat fns[NS_PROFILE_MAX_FNS];
    i32 fn_count;
    u64 fns_dropped;
    i32 fn_hash[NS_PROFILE_FN_HASH];

    // Ring buffer of completed events. `event_head` is the next write slot.
    // When the ring is full, new events overwrite the oldest and
    // `events_dropped` counts the overwritten samples.
    ns_profile_event events[NS_PROFILE_MAX_EVENTS];
    i32 event_count;
    i32 event_head;
    u64 events_dropped;

    ns_profile_open open[NS_PROFILE_MAX_STACK];
    i32 open_len;
    u64 stack_overflows;

    ns_profile_flame flames[NS_PROFILE_MAX_FLAME];
    i32 flame_count;
    u64 flames_dropped;
    i32 flame_hash[NS_PROFILE_FLAME_HASH];
} ns_profile_state;

extern ns_profile_state ns_profile;

// Monotonic wall clock in milliseconds.
f64 ns_profile_now_ms(void);

// Clear every collected sample. Collection stays disabled until enable.
void ns_profile_reset(void);

// Enable collection. Recorders below are no-ops until this is called.
void ns_profile_enable(f64 start_ms);

// Push a VM function or compiler phase onto the open stack so nested exclusive
// time and the flame tree can be attributed to the live call path.
void ns_profile_scope_enter(ns_str name, ns_str lib);

// Record one completed FFI call. `start_ms` is the monotonic clock value taken
// immediately before the native call, and `elapsed_ms` is the wall-clock time
// spent inside it. No-op unless profiling is enabled.
void ns_profile_record_ffi(ns_str name, ns_str lib, f64 start_ms, f64 elapsed_ms);

// Record one completed VM function or compiler phase and pop the matching
// enter. If no enter is on the stack, the scope is still counted as a root.
void ns_profile_record_scope(ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms);

// Write the ns-profile-v4 text report, including the folded flame stacks.
void ns_profile_write_text(FILE *f, f64 elapsed_ms, i32 argc, i8 **argv);

// Print a terminal hot-path summary. Rows are colored by self-time share.
void ns_profile_print_summary(FILE *out, f64 elapsed_ms);
