#include "ns_code_gen.h"

// risc-v instruction set architecture [32 bits]
enum isa_type {
    // [---7---] [--5--] [--5--] [-3-] [--5--] [---7---]
    // [     fn] [  rs2] [  rs1] [ fn] [   rd] [     op]
    isa_type_reg,

    // [-----11-----] [--5--] [-3-] [--5--] [---7---]
    // [         imm] [  rs1] [ fn] [   rd] [     op]
    isa_type_imm,

    // [---7---] [--5--] [--5--] [-3-] [--5--] [---7---]
    // [    imm] [  rs2] [  rs1] [   fn] [  imm] [     op]
    isa_type_store,

    // [-] [---6---] [--5--] [--5--] [-3-] [--4--] [-] [---7---]
    // [ ] [    imm] [  rs2] [  rs1] [ fn] [   rd] [ ] [     op]
    isa_type_br,

    // [----------20----------] [--5--] [---7---]
    // [                   imm] [   xrd] [     op]
    isa_type_upper,

    // [-] [-----10-----] [-] []
    isa_type_jmp,
};

// risc_v instruction set

// integer arithmetic

// immediate representation

// load and store

// control transfer

// system

// floating point

// TODO: atomic
// TODO: vector
// TODO: compressed

bool ns_code_gen_risc(ns_ast_ctx *ctx) {
    ns_str output_path = ns_str_cstr(ctx->output.data);
    printf("generate risc object file: %s\n", output_path.data);

    FILE *fd = fopen(output_path.data, "w");
    if (!fd) {
        fprintf(stderr, "failed to open file %s\n", output_path.data);
        return false;
    }

    return true;
}