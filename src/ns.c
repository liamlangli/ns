#include "ns.h"
#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"
#include "ns_asm.h"

#define STB_DS_IMPLEMENTATION

static ns_vm vm = {0};
static ns_ast_ctx ctx = {0};

typedef struct ns_compile_option_t {
    ns_bool tokenize_only: 2;
    ns_bool ast_only: 2;
    ns_bool ssa_only: 2;
    ns_bool aarch_only: 2;
    ns_bool macho_only: 2;
    ns_bool macho_obj_only: 2;
    ns_bool symbol_only: 2;
    ns_bool show_version: 2;
    ns_bool show_help: 2;
    ns_str output;
    ns_str filename;
} ns_compile_option_t;

ns_compile_option_t parse_options(i32 argc, i8** argv) {
    ns_compile_option_t option = {0};
    for (i32 i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--token") == 0) {
            option.tokenize_only = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--ast") == 0) {
            option.ast_only = true;
        } else if (strcmp(argv[i], "--ssa") == 0) {
            option.ssa_only = true;
        } else if (strcmp(argv[i], "--aarch") == 0) {
            option.aarch_only = true;
        } else if (strcmp(argv[i], "--macho") == 0) {
            option.macho_only = true;
        } else if (strcmp(argv[i], "--macho-o") == 0 || strcmp(argv[i], "--macho-obj") == 0) {
            option.macho_obj_only = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--symbol") == 0) {
            option.symbol_only = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            option.show_version = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            option.show_help = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else {
            option.filename = ns_str_cstr(argv[i]); // unmatched argument is treated as filename
        }
    }
    return option;
}

void ns_help() {
    ns_asm_target target;
    ns_asm_get_current_target(&target);
    ns_str arch = ns_arch_str(target.arch);
    ns_str os = ns_os_str(target.os);

    ns_info("usage", "ns [option] [file.ns]\n");
    printf("target: %.*s-%.*s\n", arch.len, arch.data, os.len, os.data);
    printf("  -t --token        tokenize only\n");
    printf("  -a --ast          parse ast only\n");
    printf("  --ssa             lower ast to ssa blocks\n");
    printf("  --aarch           lower ssa to aarch64 machine words\n");
    printf("  --macho           emit mach-o executable (arm64)\n");
    printf("  --macho-o         emit mach-o object file (.o, arm64)\n");
    printf("  -s --symbol       print symbol table\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  -o --output       output path\n");

}

void ns_version() {
    ns_info("nanoscript", "v%d.%d\n", (int)VERSION_MAJOR, (int)VERSION_MINOR);
}

void ns_exec_tokenize(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_token(source, filename);
}

void ns_exec_ast(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);
    ns_ast_ctx_print(&ctx, true);
}

void ns_exec_symbol(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    if (ns_return_is_error(ret)) ns_return_assert(ret);

    ret = ns_vm_parse(&vm, &ctx);
    if (ns_return_is_error(ret)) ns_return_assert(ret);
    ns_vm_symbol_print(&vm);
}

void ns_exec_ssa(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *m = ssa_ret.r;
    ns_ssa_print(m);
    ns_ssa_module_free(m);
}

void ns_exec_aarch(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_ptr bin_ret = ns_aarch_from_ssa(ssa);
    if (ns_return_is_error(bin_ret)) {
        ns_ssa_module_free(ssa);
        ns_return_assert(bin_ret);
    }

    ns_aarch_module_bin *bin = bin_ret.r;
    ns_aarch_print(bin);
    ns_aarch_free(bin);
    ns_ssa_module_free(ssa);
}

void ns_exec_macho(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.out");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_macho_emit(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("macho", "output %.*s\n", output.len, output.data);
}

void ns_exec_macho_object(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.out.o");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_macho_emit_object(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("macho", "object %.*s\n", output.len, output.data);
}

void ns_exec_eval(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) ns_exit(1, "ns", "invalid input file %.*s.\n", filename.len, filename.data);
    ns_return_value ret_v = ns_eval(&vm, source, filename);
    if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
    ns_str ret = ns_fmt_value(&vm, ret_v.r);
    printf("%.*s\n", ret.len, ret.data);
    ns_str_free(ret);
}

void ns_exec_repl() {
    ns_repl(&vm);
}

i32 main(i32 argc, i8** argv) {
    ns_compile_option_t option = parse_options(argc, argv);

    if (option.show_help) {
        ns_help(); return 0;
    }

    if (option.show_version) {
        ns_version(); return 0;
    }

    if (option.tokenize_only) {
        ns_exec_tokenize(option.filename);
    } else if (option.ast_only) {
        ns_exec_ast(option.filename);
        ns_mem_status();
    } else if (option.ssa_only) {
        ns_exec_ssa(option.filename);
    } else if (option.aarch_only) {
        ns_exec_aarch(option.filename);
    } else if (option.macho_only) {
        ns_exec_macho(option.filename, option.output);
    } else if (option.macho_obj_only) {
        ns_exec_macho_object(option.filename, option.output);
    } else if (option.symbol_only) {
        ns_exec_symbol(option.filename);
    } else {
        if (option.filename.len == 0) {
            ns_version();
            ns_exec_repl();
        } else {
            ns_exec_eval(option.filename);
        }
    }
    return 0;
}
