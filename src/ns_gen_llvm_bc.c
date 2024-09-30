#include "ns_code_gen.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

typedef struct ns_llvm_ctx_t {
    ns_str path;
    ns_parse_context_t *ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef builder;
} ns_llvm_ctx_t;

LLVMTypeRef ns_llvm_type(ns_token_t t) {
    switch (t.type)
    {
        case NS_TOKEN_TYPE_INT8:
        case NS_TOKEN_TYPE_UINT8:
            return LLVMInt8Type();
        case NS_TOKEN_TYPE_INT16:
        case NS_TOKEN_TYPE_UINT16:
            return LLVMInt16Type();
        case NS_TOKEN_TYPE_INT32:
        case NS_TOKEN_TYPE_UINT32:
            return LLVMInt32Type();
        case NS_TOKEN_TYPE_INT64:
        case NS_TOKEN_TYPE_UINT64:
            return LLVMInt64Type();
        case NS_TOKEN_TYPE_FLOAT32:
            return LLVMFloatType();
        case NS_TOKEN_TYPE_FLOAT64:
            return LLVMDoubleType();
    default:
        break;
    }
    return LLVMVoidType();
}

bool ns_llvm_fn_def(ns_llvm_ctx_t *code_gen_ctx, int s) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_ast_t n = ctx->nodes[s];
    LLVMBuilderRef builder = code_gen_ctx->builder;

    i32 param_count = n.fn_def.param_count;
    LLVMTypeRef* param_types = (LLVMTypeRef*)malloc(sizeof(LLVMTypeRef) * param_count);
    for (int i = 0; i < n.fn_def.param_count; i++) {
        ns_ast_t p = ctx->nodes[n.fn_def.params[i]];
        if (p.param.is_ref) {
            // TODO: handle ref type
        }
        param_types[i] = ns_llvm_type(p.param.type);
    }
    LLVMTypeRef ret_type = ns_llvm_type(n.fn_def.return_type);
    LLVMTypeRef fn_type = LLVMFunctionType(ret_type, param_types, param_count, 0);
    LLVMValueRef fn = LLVMAddFunction(code_gen_ctx->mod, n.fn_def.name.val.data, fn_type); 
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");

    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMBuildRet(builder, LLVMConstInt(LLVMInt32Type(), 0, 0));
    return true;
}

bool ns_code_gen_llvm_bc(ns_parse_context_t *ctx) {
    ns_str output = ctx->output;
    if (output.data == NULL) {
        fprintf(stderr, "output file is not specified\n");
        return false;
    }

    ns_str bc_path = ns_str_cstr(output.data);
    printf("generate arm64 assemble file: %s\n", bc_path.data);

    ns_str module_name = ns_path_filename(ctx->filename);
    LLVMModuleRef mod = LLVMModuleCreateWithName(module_name.data);
    LLVMBuilderRef builder = LLVMCreateBuilder();
    ns_llvm_ctx_t code_gen_ctx = {.path=bc_path, .ctx=ctx, .mod=mod, .builder=builder};

    int i = 0;
    int l = ctx->section_count;
    while (i < l) {
        int s = ctx->sections[i++];
        ns_ast_t n = ctx->nodes[s];

        switch (n.type) {
            case NS_AST_FN_DEF:
                ns_llvm_fn_def(&code_gen_ctx, s);
                break;
            default:
                break;
        }
    }
    LLVMDisposeBuilder(builder);

    char *error = NULL;
    LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    // Write out bitcode to file
    if (LLVMWriteBitcodeToFile(mod, bc_path.data) != 0) {
        fprintf(stderr, "error writing bitcode to file, skipping\n");
        return false;
    }

    return true;
}