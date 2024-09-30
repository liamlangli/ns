#include "ns_code_gen.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

bool ns_code_gen_llvm_bc(ns_parse_context_t *ctx) {
    ns_str output = ctx->output;
    if (output.data == NULL) {
        fprintf(stderr, "output file is not specified\n");
        return false;
    }

    ns_str bc_path = ns_str_cstr(output.data);
    printf("generate arm64 assemble file: %s\n", bc_path.data);

    FILE *fd = fopen(bc_path.data, "w");
    if (!fd) {
        fprintf(stderr, "failed to open file %s\n", bc_path.data);
        return false;
    }

    ns_str module_name = ns_path_filename(ctx->filename);
    LLVMModuleRef mod = LLVMModuleCreateWithName(module_name.data);

    LLVMTypeRef param_types[] = { LLVMInt32Type(), LLVMInt32Type() };
    LLVMTypeRef ret_type = LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
    LLVMValueRef sum = LLVMAddFunction(mod, "sum", ret_type);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(sum, "entry");

    LLVMBuilderRef builder = LLVMCreateBuilder();
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef tmp = LLVMBuildAdd(builder, LLVMGetParam(sum, 0), LLVMGetParam(sum, 1), "tmp");
    LLVMBuildRet(builder, tmp);

    char *error = NULL;
    LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    LLVMExecutionEngineRef engine;
    error = NULL;
    LLVMLinkInMCJIT();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    if (LLVMCreateExecutionEngineForModule(&engine, mod, &error) != 0) {
        fprintf(stderr, "failed to create execution engine\n");
        exit(1);
    }
    if (error) {
        fprintf(stderr, "error: %s\n", error);
        LLVMDisposeMessage(error);
        exit(1);
    }

    // Write out bitcode to file
    if (LLVMWriteBitcodeToFile(mod, bc_path.data) != 0) {
        fprintf(stderr, "error writing bitcode to file, skipping\n");
    }

    LLVMDisposeBuilder(builder);
    LLVMDisposeExecutionEngine(engine);

    return true;
}