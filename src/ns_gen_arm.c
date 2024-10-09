#include "ns_code_gen.h"
#include "ns_ast.h"
#include "ns_type.h"

#include <stdio.h>

bool ns_code_gen_arm64(ns_ast_ctx *ctx) {
    ns_str output_path = ns_str_cstr(ctx->output.data);
    printf("generate arm64 object file: %s\n", output_path.data);
    return true;
}