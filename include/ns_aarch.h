#pragma once

#include "ns_ssa.h"
#include "asm/ns_asm.h"

typedef struct ns_aarch_call_fixup {
    u32 off;       /* byte offset of BL instruction in fn->text */
    ns_str callee; /* name of the callee function */
} ns_aarch_call_fixup;

typedef struct ns_aarch_fn_bin {
    ns_str name;
    u8 *text;
    ns_aarch_call_fixup *call_fixups;
} ns_aarch_fn_bin;

typedef struct ns_aarch_module_bin {
    ns_aarch_fn_bin *fns;
} ns_aarch_module_bin;

ns_return_ptr ns_aarch_from_ssa(ns_ssa_module *ssa);
void ns_aarch_print(ns_aarch_module_bin *m);
void ns_aarch_free(ns_aarch_module_bin *m);
