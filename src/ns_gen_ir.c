#include "ns_code_gen.h"

bool ns_code_gen_ir(ns_parse_context_t *ctx) {
    ns_str ir_path = ns_str_cstr("bin/ir.ll");
    printf("generate arm64 assemble file: %s\n", ir_path.data);

    FILE *fd = fopen(ir_path.data, "w");
    if (!fd) {
        fprintf(stderr, "failed to open file %s\n", ir_path.data);
        return false;
    }

    fprintf(fd, "; ModuleID = %s\n", ctx->filename);
    fprintf(fd, "source_filename = \"%s\"\n", ctx->filename);

    ns_ast_t stack[128];
    int top = 0;

    return true;
}