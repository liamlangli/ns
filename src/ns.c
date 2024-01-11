#include "ns.h"
#include "ns_tokenize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: ns [file.ns]\n");
        return 1;
    }

    const char *filename = argv[1];
    const char *source = io_read_file(filename);
    if (source == NULL) {
        printf("Failed to read file: %s\n", argv[1]);
        return 1;
    }

    ns_value val = ns_eval(source, filename);
    return 0;
}
