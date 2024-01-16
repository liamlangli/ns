#include "ns.h"
#include "ns_tokenize.h"

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

ns_value ns_eval(const char *source, const char *filename) {
    int len = strlen(source);
    int i = 0;
    ns_token_t t = {0};
    do {
        i = ns_tokenize(&t, source, filename, i);
        if (t.type == NS_TOKEN_SPACE) {
            continue;
        } else {
            printf("[%s, line:%4d, offset:%4d] %-20s %.*s\n", filename, t.line, i - t.line_start, ns_token_to_string(t.type), macro_max(0, t.val.len), t.val.data);
            t.type = NS_TOKEN_UNKNOWN;
        }
    } while (t.type != NS_TOKEN_EOF && i < len);
    return NS_NIL;
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

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: [-t] ns [file.ns]\n");
        printf("  -t --tokenize do tokenize only\n");
        printf("  -p --parse    do parse only\n");
        printf("  -v --version  show version\n");
        printf("  -h --help     show this help\n");
        return 1;
    }

    ns_compile_option_t option = parse_options(argc, argv);

    const char *filename = argv[argc - 1];
    const char *source = io_read_file(filename);

    if (source == NULL) {
        printf("Failed to read file: %s\n", argv[1]);
        return 1;
    }

    ns_value val = ns_eval(source, filename);
    return 0;
}
