#pragma once

#include "ns_aarch.h"

ns_return_bool ns_macho_emit(ns_ssa_module *ssa, ns_str output_path);
ns_return_bool ns_macho_emit_object(ns_ssa_module *ssa, ns_str output_path);
