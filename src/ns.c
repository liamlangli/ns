#include "ns.h"
#include "ns_bitcode.h"
#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_DS_IMPLEMENTATION

static ns_vm vm = {0};
static ns_ast_ctx ctx = {0};

ns_str ns_read_file(ns_str path) {
    FILE *file = fopen(path.data, "rb");
    if (!file) {
        return ns_str_null;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = (char *)malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    ns_str data = ns_str_range(buffer, size);
    data.dynamic = true;
    return data;
}

ns_str ns_str_slice(ns_str s, int start, int end) {
    char *buffer = (char *)malloc(end - start + 1);
    memcpy(buffer, s.data + start, end - start);
    buffer[end - start] = '\0';
    ns_str data = ns_str_range(buffer, end - start);
    data.dynamic = true;
    return data;
}

typedef struct ns_compile_option_t {
    bool tokenize_only;
    bool ast_only;
    bool bitcode_only;
    bool show_version;
    bool show_help;
    ns_str output;
    ns_str filename;
} ns_compile_option_t;

ns_compile_option_t parse_options(int argc, char **argv) {
    ns_compile_option_t option = {0};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tokenize") == 0) {
            option.tokenize_only = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--ast") == 0) {
            option.ast_only = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            option.show_version = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            option.show_help = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-b") == 0 ||strcmp(argv[i], "--bc") == 0) {
            option.bitcode_only = true;
        } else {
            option.filename = ns_str_cstr(argv[i]); // unmatched argument is treated as filename
        }
    }
    return option;
}

void ns_help() {
    ns_info("usage", "ns [option] [file.ns]\n");
    printf("  -t --tokenize     tokenize only\n");
    printf("  -a --ast          parse ast only\n");
    printf("  -b --bitcode      generate llvm bitcode\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  -o --output       output path\n");
}

void ns_version() {
    ns_info("nano script", "v%d.%d\n", (int)VERSION_MAJOR, (int)VERSION_MINOR);
}

void ns_exec_tokenize(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_read_file(filename);
    ns_token(source, filename);
}

void ns_exec_ast(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_read_file(filename);
    ns_ast_parse(&ctx, source, filename);
    ns_ast_ctx_dump(&ctx);
}

void ns_exec_bitcode(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    if (output.len == 0) ns_error("ns", "no output file.\n");

#ifndef NS_BITCODE
    ns_exit(1, "ns", "bitcode is not enabled\n");
#else
    ns_str source = ns_read_file(filename);
    ns_ast_parse(&ctx, source, filename);
    ns_vm_parse(&vm, &ctx);
    ctx.output = output;
    if (output.data == NULL) {
        ns_warn("ns", "output file is not specified.");
        return;
    }
    ns_bc_gen(&vm, &ctx);
#endif
}

void ns_exec_eval(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_read_file(filename);
    if (source.len == 0) ns_exit(1, "ns", "invalid input file %.*s.\n", filename.len, filename.data);
    ns_eval(&vm, source, filename);
}

void ns_exec_repl() {
    ns_repl(&vm);
}

int main(int argc, char **argv) {
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
