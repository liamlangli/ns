#pragma once

#include "ns_aarch.h"

ns_return_bool ns_macho_emit(ns_ssa_module *ssa, ns_str output_path);
ns_return_bool ns_macho_emit_object(ns_ssa_module *ssa, ns_str output_path);
// `embed_main` exports `fn main` as `_ns_program_main` so an IDE host can keep
// its own process entry and call the program after it has entered Resources.
// `platform` is an Xcode PLATFORM_NAME (`macosx`, `iphoneos`, `iphonesimulator`,
// `xros`, `xrsimulator`) used for LC_BUILD_VERSION; empty keeps macOS.
ns_return_bool ns_macho_emit_object_ex(ns_ssa_module *ssa, ns_str output_path, ns_bool embed_main, ns_str platform);
