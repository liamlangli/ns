#include "ns_test.h"
#include "ns_os.h"
#include "ns_ssa.h"
#include "ns_wasm.h"
#include "ns_native_rt.h"

static void test_native_pins(void) {
    ns_rt_reset();
    int64_t mark = ns_rt_scope_enter();
    int64_t scratch = ns_rt_alloc(32);
    int native_handle = 0;
    ns_rt_pin_ref((int64_t)(uintptr_t)&native_handle);
    ns_rt_scope_leave(mark, 0, 0);
    ns_expect(ns_rt_alloc(32) == scratch, "a native handle does not pin script heap temporaries.");
    mark = ns_rt_scope_enter();
    int64_t kept = ns_rt_alloc(32);
    ns_rt_store(kept, 0, 42, 8);
    ns_rt_pin_ref(kept);
    ns_rt_scope_leave(mark, 0, 0);
    ns_expect(ns_rt_alloc(32) > kept && ns_rt_load(kept, 0, 8) == 42,
              "a complex script ref still pins its reachable heap.");
    int64_t text = ns_rt_itos(123456);
    ns_rt_pin_str(text);
    ns_rt_pin_str(text);
    mark = ns_rt_scope_enter();
    scratch = ns_rt_alloc(32);
    ns_rt_pin_str(text);
    ns_rt_scope_leave(mark, 0, 0);
    ns_expect(ns_rt_alloc(32) == scratch, "re-pinning a published string does not pin unrelated temporaries.");
    ns_rt_reset();
}

static void test_allocation_free_scopes(void) {
    const char *source =
        "fn leaf(n: i32) i32 { return n + 1 }\n"
        "fn chain(n: i32) i32 { return leaf(n) * 2 }\n"
        "fn scratch(n: i32) i32 { let a = [i32](8)\n a[0] = n\n return a[0] }\n"
        "fn allocating(n: i32) i32 { return scratch(n) }\n"
        "fn main() i32 { return chain(1) + allocating(2) }\n";
    ns_ast_ctx ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&ctx, ns_str_cstr((i8 *)source), ns_str_cstr("<scope_effect_test>"));
    ns_expect(!ns_return_is_error(parsed), "scope effect source parses.");
    ns_return_ptr built = ns_ssa_build_with_runtime_paths_options(&ctx, ns_str_null, ns_str_null, ns_str_null, false);
    ns_expect(!ns_return_is_error(built), "scope effect source lowers.");
    if (ns_return_is_error(built)) return;
    ns_ssa_module *m = built.r;
    for (i32 i = 0; i < (i32)ns_array_length(m->fns); ++i) {
        ns_ssa_fn *fn = &m->fns[i];
        ns_bool has_scope = false;
        for (i32 j = 0; j < (i32)ns_array_length(fn->insts); ++j)
            if (fn->insts[j].op == NS_SSA_OP_SCOPE_ENTER) has_scope = true;
        if (ns_str_equals_STR(fn->name, "chain")) {
            ns_expect(!has_scope, "a scalar helper chain needs no heap scope.");
        }
        if (ns_str_equals_STR(fn->name, "allocating")) {
            ns_expect(has_scope, "a caller of an allocating function keeps its heap scope.");
        }
    }
    ns_ssa_module_free(m);
}

/* A native `str` parameter receives a C copy, so building a label every frame
 * and handing it to a library must not pin it (and the frame below it). A
 * string stored into a container is still published and keeps its pin. */
static void test_native_str_args_unpinned(void) {
    const char *source =
        "use os\n"
        "fn measure(name: str) i64 { let text = \"n=\" + name\n return os_file_size(text) }\n"
        "fn keep(name: str) [str] { let a = [str](1)\n a[0] = \"x\" + name\n return a }\n"
        "fn main() i32 { let k = keep(\"a\")\n return measure(\"b\") as i32 }\n";
    ns_ast_ctx ctx = {0};
    ns_return_bool parsed = ns_ast_parse(&ctx, ns_str_cstr((i8 *)source), ns_str_cstr("<native_str_pin_test>"));
    ns_expect(!ns_return_is_error(parsed), "native str argument source parses.");
    ns_str lib = ns_str_cstr("lib");
    ns_return_ptr built = ns_ssa_build_with_runtime_paths_options(&ctx, lib, lib, ns_str_null, false);
    ns_expect(!ns_return_is_error(built), "native str argument source lowers.");
    if (ns_return_is_error(built)) return;
    ns_ssa_module *m = built.r;
    ns_bool measure_seen = false, measure_pins = false, keep_pins = false;
    for (i32 i = 0; i < (i32)ns_array_length(m->fns); ++i) {
        ns_ssa_fn *fn = &m->fns[i];
        ns_bool pins_str = false;
        for (i32 j = 0; j < (i32)ns_array_length(fn->insts); ++j) {
            ns_ssa_inst *inst = &fn->insts[j];
            if (inst->op == NS_SSA_OP_PIN && (inst->target0 == 1 || inst->target0 == 3)) pins_str = true;
        }
        if (ns_str_equals_STR(fn->name, "measure")) {
            measure_seen = true;
            measure_pins = pins_str;
        }
        if (ns_str_equals_STR(fn->name, "keep")) {
            for (i32 j = 0; j < (i32)ns_array_length(fn->insts); ++j)
                if (fn->insts[j].op == NS_SSA_OP_PIN) keep_pins = true;
        }
    }
    ns_expect(measure_seen && !measure_pins, "a str handed to a native str parameter is not pinned.");
    ns_expect(keep_pins, "a str stored into an array is still pinned.");
    ns_ssa_module_free(m);
}

int main(void) {
    test_native_pins();
    test_allocation_free_scopes();
    test_native_str_args_unpinned();
    const char *source =
        "use std\n"
        "lit folded = 6 * 7\n"
        "lit unused = 99\n"
        "enum byte_code: u8 { zero = 0, one, }\n"
        "enum wide_mask: u64 { none = 0, all = 18446744073709551615, }\n"
        "fn byte_value() u8 { return byte_code.one }\n"
        "fn wide_value() u64 { return wide_mask.all }\n"
        "fn widths() i32 {\n"
        "    if 1b == 1b && 255ub == 255ub && 123s == 123s && 456us == 456us && 789u == 789u { return 1 }\n"
        "    return 0\n"
        "}\n"
        "fn signed_math() i64 { return (-100000l) / 3l }\n"
        "fn half(x: f64) f64 { return x / 2.0d }\n"
        "fn float_value() f64 { return half(5.0d) + 0.25d }\n"
        "fn portable_sin() f64 { return sin(0.0d) }\n"
        "fn folded_value() i32 { return folded }\n"
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
    ns_bool folded_lit = false;
    ns_bool unused_lit = false;
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
            if (inst->op == NS_SSA_OP_CONST && ns_type_is(inst->type, NS_TYPE_I32) &&
                ns_str_equals(inst->name, ns_str_cstr("42"))) folded_lit = true;
            if (inst->op == NS_SSA_OP_CONST && ns_type_is(inst->type, NS_TYPE_I32) &&
                ns_str_equals(inst->name, ns_str_cstr("99"))) unused_lit = true;
        }
    }
    ns_expect(byte_constant && wide_constant, "enum members lower to exact u8 and u64 SSA constants.");
    ns_expect(byte_cast && wide_cast, "integer-to-enum casts lower to their underlying SSA types.");
    ns_expect(folded_lit, "a global lit constant expression is folded into an SSA constant.");
    ns_expect(!unused_lit, "an unused global lit does not add an SSA constant to every function.");

    ns_str output = ns_str_cstr("/tmp/ns_ssa_enum_test.wasm");
    ns_return_bool emitted = ns_wasm_emit(module, output);
    ns_expect(!ns_return_is_error(emitted) && emitted.r, "enum SSA emits a WebAssembly module.");
    ns_str wasm = ns_os_read_file(output);
    ns_expect(wasm.len >= 8 && wasm.data[0] == 0 && wasm.data[1] == 'a' && wasm.data[2] == 's' && wasm.data[3] == 'm',
              "emitted enum WebAssembly has the expected module header.");
    int node_status = system("node -e \"const fs=require('fs');const b=fs.readFileSync('/tmp/ns_ssa_enum_test.wasm');"
                             "if(!WebAssembly.validate(b))process.exit(1);const m=new WebAssembly.Module(b);"
                             "const i=new WebAssembly.Instance(m,{std:{sin:Math.sin}});i.exports.__ns_init();"
                             "if(i.exports.main()!==0)process.exit(2);const before=i.exports.memory.buffer.byteLength;"
                             "if(i.exports.widths()!==1||i.exports.signed_math()!==-33333n||i.exports.float_value()!==2.75||i.exports.portable_sin()!==0)process.exit(3);"
                             "i.exports.__ns_alloc(before+64);if(i.exports.memory.buffer.byteLength<=before)process.exit(4);\"");
    ns_expect(node_status == 0, "emitted WebAssembly validates, instantiates, executes, and grows memory in Node.");
    ns_str_free(wasm);
    remove(output.data);
    ns_ssa_module_free(module);

    const char *shader_source =
        "use shader\n"
        "fn cs_fill() void { let x = shader_global_id_x() }\n"
        "fn main() {}\n";
    ns_ast_ctx shader_ctx = {0};
    parsed = ns_ast_parse(&shader_ctx, ns_str_cstr((i8 *)shader_source), ns_str_cstr("<ns_ssa_shader_test>"));
    ns_expect(!ns_return_is_error(parsed) && parsed.r, "shader metadata source parses.");
    built = ns_ssa_build(&shader_ctx);
    ns_expect(!ns_return_is_error(built), "default SSA lowering embeds Wasm shader metadata.");
    if (!ns_return_is_error(built)) {
        module = built.r;
        ns_expect(ns_array_length(module->shaders) == 1, "default SSA contains the compute shader WGSL.");
        ns_ssa_module_free(module);
    }

    ns_ast_ctx native_ctx = {0};
    parsed = ns_ast_parse(&native_ctx, ns_str_cstr((i8 *)shader_source), ns_str_cstr("<ns_ssa_native_shader_test>"));
    built = ns_ssa_build_with_runtime_paths_options(&native_ctx, ns_str_null, ns_str_null, ns_str_null, false);
    ns_expect(!ns_return_is_error(built), "native SSA lowering skips Wasm shader metadata.");
    if (!ns_return_is_error(built)) {
        module = built.r;
        ns_expect(ns_array_length(module->shaders) == 0, "native SSA contains no unused WGSL shader payload.");
        ns_ssa_module_free(module);
    }
    return 0;
}
