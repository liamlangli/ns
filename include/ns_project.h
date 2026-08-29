#pragma once

#include "ns_type.h"

typedef enum ns_project_kind {
    NS_PROJECT_APP,
    NS_PROJECT_LIBRARY,
} ns_project_kind;

// The screen orientations a mobile application supports. The manifest names
// them with `orientation = ["portrait", "landscape_left"]`; a generated mobile
// application enables exactly the named ones and disables the rest.
typedef enum ns_project_orientation {
    NS_PROJECT_ORIENTATION_NONE = 0,
    NS_PROJECT_ORIENTATION_PORTRAIT = 1 << 0,
    NS_PROJECT_ORIENTATION_PORTRAIT_UPSIDE_DOWN = 1 << 1,
    NS_PROJECT_ORIENTATION_LANDSCAPE_LEFT = 1 << 2,
    NS_PROJECT_ORIENTATION_LANDSCAPE_RIGHT = 1 << 3,
    NS_PROJECT_ORIENTATION_ALL = NS_PROJECT_ORIENTATION_PORTRAIT | NS_PROJECT_ORIENTATION_PORTRAIT_UPSIDE_DOWN |
                                 NS_PROJECT_ORIENTATION_LANDSCAPE_LEFT | NS_PROJECT_ORIENTATION_LANDSCAPE_RIGHT,
} ns_project_orientation;

// Fully resolved inputs shared by the platform project generators. Paths are
// absolute and remain owned by the caller for the duration of generation.
typedef struct ns_project_spec {
    ns_project_kind kind;
    // Use IDE utility targets that delegate to `ns build`/`ns test`. This is
    // required when an app imports native FFI modules that cannot be embedded
    // in the portable Apple runtime target.
    ns_bool host_build;
    // Compile the linked program to a Mach-O object and call it from the
    // generated Apple app instead of interpreting LinkedProject.ns. Set when
    // the manifest has `link = true` and every imported module is embeddable.
    ns_bool link_native;
    ns_str root;
    ns_str manifest;
    ns_str source_dir;
    ns_str name;
    ns_str safe_name;
    ns_str version;
    // Absolute path to the manifest icon, or null when the app has no icon.
    ns_str icon;
    ns_str linked_source;
    // What the manifest `assets` key packages, as paths relative to `root`. The
    // generated app carries each one into its resource directory under the same
    // name, and the app enters that directory before the program runs, so a
    // relative path in the program reads the same file from the IDE's build as
    // it does from the project. An ns_array; may be empty.
    ns_str *assets;
    // Mobile orientations the generated application enables. A manifest that
    // declares none leaves this NS_PROJECT_ORIENTATION_NONE, and every
    // orientation stays enabled; any declared set disables the rest.
    u32 orientations;
    ns_str ns_executable;
    // Directory containing src/, include/, and ref/ for the embeddable,
    // language-only runtime SDK installed with ns.
    ns_str runtime_root;
} ns_project_spec;

// Normalize a manifest display name for filenames, IDE identifiers, and the
// default bundle identifier component. The result is process-lifetime storage.
ns_str ns_project_safe_name(ns_str name);

// The orientation a manifest name selects, or NS_PROJECT_ORIENTATION_NONE when
// the name is not one of `portrait`, `portrait_upside_down`, `landscape_left`,
// or `landscape_right`.
ns_project_orientation ns_project_orientation_from_name(ns_str name);
ns_bool ns_project_generate_xcode(const ns_project_spec *spec);
ns_bool ns_project_generate_visual_studio(const ns_project_spec *spec);
