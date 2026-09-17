#pragma once

#include "ns_ssa.h"
#include "asm/ns_asm.h"

typedef struct ns_amd64_call_fixup {
    u32 off;       /* CALL: byte offset of the rel32 field; LEA: of the disp32 field */
    ns_str callee; /* name of the callee function */
    u8 kind;       /* 0 = CALL rel32, 1 = RIP-relative address of the symbol */
} ns_amd64_call_fixup;

typedef struct ns_amd64_fn_bin {
    ns_str name;
    u8 *text;
    ns_amd64_call_fixup *call_fixups;
} ns_amd64_fn_bin;

typedef struct ns_amd64_module_bin {
    ns_amd64_fn_bin *fns;
    char **strtab;
    i32 *strlens;
    i32 nstr;
} ns_amd64_module_bin;

ns_return_ptr ns_amd64_from_ssa(ns_ssa_module *ssa);
void ns_amd64_print(ns_amd64_module_bin *m);
void ns_amd64_free(ns_amd64_module_bin *m);
