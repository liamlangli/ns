#pragma once

#include "ns_amd64.h"

// ELF64 relocatable object for x86-64. `ns build` hands the result to the
// host's C toolchain, which links it against the native runtime and the
// feature modules the program imports, exactly as the mach-o path does on
// Darwin.
ns_return_bool ns_elf_emit_object(ns_ssa_module *ssa, ns_str output_path);
