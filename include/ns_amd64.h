#pragma once

#include "ns_ssa.h"
#include "asm/ns_asm.h"

typedef struct ns_amd64_call_fixup {
    u32 off;       /* byte offset of CALL instruction (rel32 field) in fn->text */
    ns_str callee; /* name of the callee function */
} ns_amd64_call_fixup;

typedef struct ns_amd64_fn_bin {
    ns_str name;
    u8 *text;
    ns_amd64_call_fixup *call_fixups;
} ns_amd64_fn_bin;

typedef struct ns_amd64_module_bin {
    ns_amd64_fn_bin *fns;
} ns_amd64_module_bin;

ns_return_ptr ns_amd64_from_ssa(ns_ssa_module *ssa);
void ns_amd64_print(ns_amd64_module_bin *m);
void ns_amd64_free(ns_amd64_module_bin *m);
