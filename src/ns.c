#include "ns.h"
#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_code_gen.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_DS_IMPLEMENTATION

char *ns_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = (char *)malloc(size + 1);
    fread(buffer, 1, size, file);
    fclose(file);
    buffer[size] = '\0';
    return buffer;
}

typedef enum ns_asm_arch {
    llvm_ir,
    arm_64,
    x86_64
} ns_asm_arch;

typedef struct ns_compile_option_t {
    bool tokenize_only;
    bool parse_only;
    bool code_gen_only;
    ns_asm_arch arch;
    bool show_version;
    bool show_help;
    bool repl;
} ns_compile_option_t;

ns_compile_option_t parse_options(int argc, char ** argv) {
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
        } else if (strcmp(argv[i], "-arm64")) {
            option.code_gen_only = true;
            option.arch = arm_64;
        } else if (strcmp(argv[i], "-ir")) {
            option.code_gen_only = true;
            option.arch = llvm_ir;
        } else if (strcmp(argv[i], "-x86")) {
            option.code_gen_only = true;
            option.arch = x86_64;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--repl") == 0) {
            option.repl = true;
        }
    }
    return option;
}

void help() {
    printf("Usage: ns [option] [file.ns]\n");
    printf("  -t --tokenize     tokenize only\n");
    printf("  -p --parse        parse only\n");
    printf("  -arm -ir -x86     code gen only (64bit arch only)\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  -r --repl         read eval print loop mode\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        help();
        return 0;
    }

    ns_compile_option_t option = parse_options(argc, argv);

    if (option.show_help) {
        help();
        return 0;
    }

    if (option.show_version) {
        printf("ns %d.%d\n", VERSION_MAJOR, VERSION_MINOR);
        if (argc == 2) return 0; // only show version
    }

    const char *filename = argv[argc - 1];
    const char *source = ns_read_file(filename);

    if (source == NULL) {
        printf("Failed to read file: %s\n", argv[1]);
        return 1;
    }

    if (option.tokenize_only) {
        ns_tokenize(source, filename);
    } else if (option.parse_only) {
        ns_parse_context_dump(ns_parse(source, filename));
    } else if (option.code_gen_only) {
        ns_parse_context_t *ctx = ns_parse(source, filename);
        if (ctx == NULL) {
            ns_dump_error(ctx, "parse error: failed to parse source\n")
        }
        switch (option.arch) {
            case llvm_ir:
                ns_code_gen_ir(ctx);
                break;
            case arm_64:
                ns_code_gen_arm64(ctx);
                break;
            case x86_64:
                ns_code_gen_x86_64(ctx);
                break;
            default:
                fprintf(stderr, "invalid arch %d\n", option.arch);
                exit(1);
        }
    } else if (option.repl) {
        ns_vm_t *vm = ns_create_vm();
        ns_eval(vm, source, filename);
    }
    return 0;
}
