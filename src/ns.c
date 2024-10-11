#include "ns.h"
#include "ns_bitcode.h"
#include "ns_ast.h"
#include "ns_tokenize.h"
#include "ns_type.h"
#include "ns_vm.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_DS_IMPLEMENTATION

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

typedef enum ns_asm_arch { llvm_bc, arm_64, x86_64, risc } ns_asm_arch;

typedef struct ns_compile_option_t {
    bool tokenize_only;
    bool parse_only;
    bool code_gen_only;
    ns_asm_arch arch;
    bool show_version;
    bool show_help;
    bool repl;
    ns_str output;
} ns_compile_option_t;

ns_compile_option_t parse_options(int argc, char **argv) {
    ns_compile_option_t option = {0};
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tokenize") == 0) {
            option.tokenize_only = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--parse") == 0) {
            option.parse_only = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            option.show_version = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            option.show_help = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-b") == 0 ||strcmp(argv[i], "--bc") == 0) {
            option.code_gen_only = true;
            option.arch = llvm_bc;
        }  else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--repl") == 0) {
            option.repl = true;
        }
    }
    return option;
}

void help() {
    ns_info("Usage", "ns [option] [file.ns]\n");
    printf("  -t --tokenize     tokenize only\n");
    printf("  -p --parse        parse only\n");
    printf("  -b --bitcode      generate llvm bitcode\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  -r --repl         read eval print loop mode\n");
    printf("  -o --output       output path\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        help();
        return 0;
    }

    ns_compile_option_t option = parse_options(argc, argv);

    if (option.show_help) {
        help(); return 0;
    }

    if (option.show_version) {
        ns_info("nano script", "v%d.%d\n", (int)VERSION_MAJOR, (int)VERSION_MINOR);
        if (argc == 2) return 0; // only show version
    }

    ns_str filename = ns_str_cstr(argv[argc - 1]);
    ns_str source = ns_read_file(filename);

    if (source.data == NULL) {
        ns_exit(1, "ns", "failed to read file: %s\n", argv[1]);
        return 1;
    }

    ns_ast_ctx ctx = {0};
    ns_vm vm = {0};
    if (option.tokenize_only) {
        ns_tokenize(source, filename);
    } else if (option.parse_only) {
        ns_parse(&ctx, source, filename);
        ns_parse_context_dump(&ctx);
    } else if (option.code_gen_only) {
#ifdef NS_BITCODE
        ns_parse(&ctx, source, filename);
        ns_vm_parse(&vm, &ctx);
        ctx.output = option.output;
        if (option.output.data == NULL) {
            ns_warn("ns", "output file is not specified.");
            return false;
        }
        switch (option.arch) {
        case llvm_bc:
            ns_bitcode_gen(&vm, &ctx);
            break;
        case arm_64:
        case x86_64:
        default:
            ns_exit(1, "ns", "invalid arch %d\n", option.arch);
        }
#else
        ns_exit(1, "ns", "bitcode is not enabled\n");
#endif
    } else if (option.repl) {
        ns_eval(&vm, source, filename);
    }
    return 0;
}
