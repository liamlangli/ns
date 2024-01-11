#include "ns_ast.h"
#include "ns_tokenize.h"

#include <stdlib.h>

ns_ast_t* ns_parse(const char *source, const char *filename) {
    ns_ast_t *root = (ns_ast_t*)malloc(sizeof(ns_ast_t));

    int f = 0;
    ns_token_t t;
    f = ns_tokenize(&t, source, filename, f);

    return root;
}