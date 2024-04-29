#include "ns_code_gen.h"
#include "ns_type.h"

bool ns_code_gen_arm64(ns_parse_context_t *ctx) {
    ns_dump_error(ctx, "ARM64 code generation is not implemented yet.")

    // warn, filename will be changed
    ns_str output = ns_str_cstr(ctx->filename);

    return true;
}