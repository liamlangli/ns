#include "ns.h"
#include "ns_parse.h"
#include "ns_tokenize.h"
#include "ns_vm.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

char *io_read_file(const char *path) {
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

typedef struct ns_compile_option_t {
    int tokenize_only;
    int parse_only;
    int show_version;
    int show_help;
} ns_compile_option_t;

ns_compile_option_t parse_options(int argc, char ** argv) {
    ns_compile_option_t option = {0};
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tokenize") == 0) {
            option.tokenize_only = 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--parse") == 0) {
            option.parse_only = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            option.show_version = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            option.show_help = 1;
        }
    }
    return option;
}

void help() {
    printf("Usage: ns [option] [file.ns]\n");
    printf("  -t --tokenize     do tokenize only\n");
    printf("  -p --parse        do parse only\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  -i --interactive  enter interactive mode\n");
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
    const char *source = io_read_file(filename);

    if (source == NULL) {
        printf("Failed to read file: %s\n", argv[1]);
        return 1;
    }

    if (option.tokenize_only) {
        ns_tokenize(source, filename);
        return 0;
    } else if (option.parse_only) {
        ns_parse_context_dump(ns_parse(source, filename));
    }

    ns_vm_t *vm = ns_create_vm();
    ns_eval(vm, source, filename);
    return 0;
}
