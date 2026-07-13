#pragma once

#include "ns_type.h"

typedef enum ns_project_kind {
    NS_PROJECT_APP,
    NS_PROJECT_LIBRARY,
} ns_project_kind;

// Fully resolved inputs shared by the platform project generators. Paths are
// absolute and remain owned by the caller for the duration of generation.
typedef struct ns_project_spec {
    ns_project_kind kind;
    // Use IDE utility targets that delegate to `ns build`/`ns test`. This is
    // required when an app imports native FFI modules that cannot be embedded
    // in the portable Apple runtime target.
    ns_bool host_build;
    ns_str root;
    ns_str manifest;
    ns_str source_dir;
    ns_str name;
    ns_str safe_name;
    ns_str version;
    ns_str linked_source;
    ns_str ns_executable;
    // Directory containing src/, include/, and ref/ for the embeddable,
    // language-only runtime SDK installed with ns.
    ns_str runtime_root;
} ns_project_spec;

// Normalize a manifest display name for filenames, IDE identifiers, and the
// default bundle identifier component. The result is process-lifetime storage.
ns_str ns_project_safe_name(ns_str name);
ns_bool ns_project_generate_xcode(const ns_project_spec *spec);
ns_bool ns_project_generate_visual_studio(const ns_project_spec *spec);
