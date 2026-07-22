#pragma once

#include "ns_ssa.h"

ns_return_bool ns_wasm_emit(ns_ssa_module *ssa, ns_str output_path);
ns_return_bool ns_wasm_emit_source_mapped(ns_ssa_module *ssa, ns_str output_path,
                                          ns_str map_path, ns_str map_url,
                                          ns_str source_root);
