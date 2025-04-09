#include "ns.h"
#include "ns_ir.h"
#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"

#define STB_DS_IMPLEMENTATION

static ns_vm vm = {0};
static ns_ast_ctx ctx = {0};

typedef struct ns_compile_option_t {
    ns_bool tokenize_only: 2;
    ns_bool ast_only: 2;
    ns_bool symbol_only: 2;
    ns_bool bitcode_only: 2;
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
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--symbol") == 0) {
            option.symbol_only = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            option.show_version = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            option.show_help = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-ir") == 0 ||strcmp(argv[i], "--ir") == 0) {
            option.bitcode_only = true;
        } else {
            option.filename = ns_str_cstr(argv[i]); // unmatched argument is treated as filename
        }
    }
    return option;
}

void ns_help() {
    ns_info("usage", "ns [option] [file.ns]\n");
    printf("  -t --token        tokenize only\n");
    printf("  -a --ast          parse ast only\n");
    printf("  -s --symbol       print symbol table\n");
    printf("  -b --bitcode      generate llvm bitcode\n");
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

void ns_exec_bitcode(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");

#ifndef NS_IR
    ns_exit(1, "ns", "bitcode is not enabled\n");
#else

    ns_return_bool ret = ns_ir_gen(filename, output);
    ns_return_assert(ret);
#endif
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

void ns_exec_eval(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) ns_exit(1, "ns", "invalid input file %.*s.\n", filename.len, filename.data);
    ns_return_value ret_v = ns_eval(&vm, source, filename);
    if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
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
    } else if (option.symbol_only) {
        ns_exec_symbol(option.filename);
    } else if (option.bitcode_only) {
        ns_exec_bitcode(option.filename, option.output);
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
