#pragma once

#include "ns_type.h"

// Function-scope profiler for the interpreter. When enabled (`ns run
// --profile`), every function call is recorded as a Chrome Trace Event
// Format complete event and written to a JSON trace file at process exit,
// viewable with `ns run nscode/profile <file>` (or Perfetto/about:tracing).

// Cheap guard read by the evaluator hooks; all other calls are no-ops
// while this is false.
extern ns_bool ns_profile_on;

// Enable profiling: capture the time origin, remember the output path and
// register the save handler with atexit().
void ns_profile_begin(ns_str out_path);

// Record a function-call enter/exit pair. `name`/`cat` are kept by value;
// their bytes must stay alive until save (fn symbol names point into
// source buffers that live for the whole process).
void ns_profile_enter(ns_str name, ns_str cat);
void ns_profile_exit(void);

// Write the trace file. Idempotent; frames still open (evaluator error
// paths) are closed at the current time.
void ns_profile_save(void);
