#include "ns_test.h"
#include "ns_os.h"
#include "ns_ssa.h"
#include "ns_wasm.h"

int main(void) {
    const char *source =
        "enum byte_code: u8 { zero = 0, one, }\n"
        "enum wide_mask: u64 { none = 0, all = 18446744073709551615, }\n"
        "fn byte_value() u8 { return byte_code.one }\n"
        "fn wide_value() u64 { return wide_mask.all }\n"
        "fn main() i32 {\n"
        "    let byte_restored = byte_value() as byte_code\n"
        "    let wide_restored = wide_value() as wide_mask\n"
        "    if byte_restored == byte_code.one && wide_restored == wide_mask.all { return 0 }\n"
        "    return 1\n"
        "}\n";

    ns_ast_ctx ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&ctx, ns_str_cstr((i8 *)source), ns_str_cstr("<ns_ssa_enum_test>"));
    ns_expect(!ns_return_is_error(parsed) && parsed.r, "enum backend source parses.");

    ns_return_ptr built = ns_ssa_build(&ctx);
    ns_expect(!ns_return_is_error(built), "enum backend source lowers to SSA.");
    if (ns_return_is_error(built)) return 1;

    ns_ssa_module *module = built.r;
    ns_bool byte_constant = false;
    ns_bool wide_constant = false;
    ns_bool byte_cast = false;
    ns_bool wide_cast = false;
    for (i32 fi = 0, fl = (i32)ns_array_length(module->fns); fi < fl; ++fi) {
        ns_ssa_fn *fn = &module->fns[fi];
        for (i32 ii = 0, il = (i32)ns_array_length(fn->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[ii];
            if (inst->op == NS_SSA_OP_CONST && ns_type_is(inst->type, NS_TYPE_U8) &&
                ns_str_equals(inst->name, ns_str_cstr("1"))) byte_constant = true;
            if (inst->op == NS_SSA_OP_CONST && ns_type_is(inst->type, NS_TYPE_U64) &&
                ns_str_equals(inst->name, ns_str_cstr("18446744073709551615"))) wide_constant = true;
            if (inst->op == NS_SSA_OP_CAST && ns_type_is(inst->type, NS_TYPE_U8)) byte_cast = true;
            if (inst->op == NS_SSA_OP_CAST && ns_type_is(inst->type, NS_TYPE_U64)) wide_cast = true;
        }
    }
    ns_expect(byte_constant && wide_constant, "enum members lower to exact u8 and u64 SSA constants.");
    ns_expect(byte_cast && wide_cast, "integer-to-enum casts lower to their underlying SSA types.");

    ns_str output = ns_str_cstr("/tmp/ns_ssa_enum_test.wasm");
    ns_return_bool emitted = ns_wasm_emit(module, output);
    ns_expect(!ns_return_is_error(emitted) && emitted.r, "enum SSA emits a WebAssembly module.");
    ns_str wasm = ns_os_read_file(output);
    ns_expect(wasm.len >= 8 && wasm.data[0] == 0 && wasm.data[1] == 'a' && wasm.data[2] == 's' && wasm.data[3] == 'm',
              "emitted enum WebAssembly has the expected module header.");
    ns_str_free(wasm);
    remove(output.data);
    ns_ssa_module_free(module);
    return 0;
}
