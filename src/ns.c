#include "ns.h"

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

ns_value ns_eval(const char *source) {
    int len = strlen(source);
    int i = 0;
    ns_token_t token = {0};
    do {
        i = ns_tokenize(&token, (char *)source, i);
        if (token.type == NS_TOKEN_SPACE) {
            continue;
        } else {
            printf("%s %s\n", ns_token_to_string(token.type), token.val);
        }
    } while (token.type != NS_TOKEN_EOF && i < len);
    return NS_NIL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: ns [file.ns]\n");
        return 1;
    }

    const char *source = io_read_file(argv[1]);
    if (source == NULL) {
        printf("Failed to read file: %s\n", argv[1]);
        return 1;
    }

    ns_value val = ns_eval(source);
    return 0;
}