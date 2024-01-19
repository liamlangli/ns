#include "ns_vm.h"
#include "ns_parse.h"
#include "ns_type.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

ns_vm_t *ns_create_vm() {
    ns_vm_t *vm = (ns_vm_t *)malloc(sizeof(ns_vm_t));
    vm->ast = NULL;
    vm->stack_depth = 0;
    vm->global = NULL;
    return vm;
}

ns_value ns_call(ns_vm_t *vm, ns_value fn, ns_value *args, int argc) {
    return NS_NIL;
}

ns_value ns_eval_expr(ns_parse_context_t *ctx, int i) {
    ns_ast_t *nodes = ctx->nodes;
    switch (nodes[i].type)
    {
    case NS_AST_BINARY_EXPR:
        break;
    case NS_AST_CALL_EXPR:
        break;
    case NS_AST_CAST_EXPR:
        break;
    case NS_AST_MEMBER_EXPR:
        break;
    case NS_AST_PRIMARY_EXPR:
        break;
    case NS_AST_GENERATOR_EXPR:
        break;
    default:
        fprintf(stderr, "eval error: unknown ast type\n");
        assert(false);
        break;
    }
    return NS_NIL;
}

ns_value ns_eval(ns_vm_t *vm, const char* source, const char *filename) {
    ns_parse_context_t *ctx = ns_parse(source, filename);

    if (ctx == NULL) {
        fprintf(stderr, "eval error: ast parse failed\n");
        return NS_NIL;
    }

    vm->ast = ctx->nodes;

    int i = 0;
    int l = arrlen(ctx->sections);
    while (i < l) {
        int section = ctx->sections[i++];
        ns_ast_t n = ctx->nodes[section];
        switch (section)
        {
        case NS_AST_FN_DEF:

            break;
        default:
            fprintf(stderr, "eval error: unknown section type\n");
            assert(false);
            break;
        }
    }

    return NS_NIL;
}