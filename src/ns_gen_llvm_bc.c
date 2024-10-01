#include "ns_code_gen.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

#define ns_bc_module LLVMModuleRef
#define ns_bc_builder LLVMBuilderRef
#define ns_bc_block LLVMBasicBlockRef
#define ns_bc_type LLVMTypeRef
#define ns_bc_value LLVMValueRef

typedef struct ns_llvm_ctx_t {
    ns_str path;
    ns_parse_context_t *ctx;
    ns_bc_module mod;
    ns_bc_builder builder;
    ns_bc_value fn;
} ns_llvm_ctx_t;

ns_bc_type ns_llvm_type(ns_token_t t);
ns_bc_value ns_llvm_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_fn_def(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_binary_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_jump_stmt(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
void ns_llvm_compound_stmt(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);

ns_bc_type ns_llvm_type(ns_token_t t) {
    switch (t.type) {
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

ns_bc_value ns_llvm_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    switch (n.type) {
    case NS_AST_BINARY_EXPR:
        return ns_llvm_binary_expr(code_gen_ctx, n);
    default:
        break;
    }
    return NULL;
}

ns_bc_value ns_llvm_fn_def(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    i32 param_count = n.fn_def.param_count;
    ns_bc_type *param_types = (ns_bc_type *)malloc(sizeof(ns_bc_type) * param_count);
    for (int i = 0; i < n.fn_def.param_count; i++) {
        ns_ast_t p = ctx->nodes[n.fn_def.params[i]];
        if (p.param.is_ref) {
            // TODO: handle ref type
        }
        param_types[i] = ns_llvm_type(p.param.type);
    }
    ns_bc_type ret_type = ns_llvm_type(n.fn_def.return_type);
    ns_bc_type fn_type = LLVMFunctionType(ret_type, param_types, param_count, 0);
    ns_bc_value fn = LLVMAddFunction(code_gen_ctx->mod, n.fn_def.name.val.data, fn_type);
    if (code_gen_ctx->fn) {
        assert(false); // nested function is not supported
    }
    code_gen_ctx->fn = fn;
    ns_bc_block entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    ns_llvm_compound_stmt(code_gen_ctx, ctx->nodes[n.fn_def.body]);
    return fn;
}

ns_bc_value ns_llvm_binary_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    ns_bc_value left = ns_llvm_expr(code_gen_ctx, ctx->nodes[n.binary_expr.left]);
    ns_bc_value right = ns_llvm_expr(code_gen_ctx, ctx->nodes[n.binary_expr.right]);
    ns_bc_value ret = NULL;
    switch (n.binary_expr.op.type) {
    case NS_TOKEN_ADDITIVE_OPERATOR:
        if (ns_str_equals_STR(n.binary_expr.op.val, "+")) {
            ret = LLVMBuildAdd(builder, left, right, "add");
        } else if (ns_str_equals_STR(n.binary_expr.op.val, "-")) {
            ret = LLVMBuildSub(builder, left, right, "sub");
        }
        break;
    case NS_TOKEN_MULTIPLICATIVE_OPERATOR:
        if (ns_str_equals_STR(n.binary_expr.op.val, "*")) {
            ret = LLVMBuildMul(builder, left, right, "mul");
        } else if (ns_str_equals_STR(n.binary_expr.op.val, "/")) {
            ret = LLVMBuildSDiv(builder, left, right, "div");
        }
        break;
    case NS_TOKEN_ASSIGN_OPERATOR:
        ret = LLVMBuildStore(builder, right, left);
        break;
    default:
        assert(false); // unexpected operator
        break;
    }
    return ret;
}

ns_bc_value ns_llvm_jump_stmt(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    ns_token_t t = n.jump_stmt.label;
    if (ns_str_equals_STR(t.val, "return")) {
        if (n.jump_stmt.expr != -1) {
            ns_bc_value expr = ns_llvm_expr(code_gen_ctx, ctx->nodes[n.jump_stmt.expr]);
            return LLVMBuildRet(builder, expr);
        } else {
            return LLVMBuildRetVoid(builder);
        }
    }
    return NULL;
}

void ns_llvm_compound_stmt(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    ns_ast_t *last = &n;
    for (int i = 0; i < n.compound_stmt.count; i++) {
        ns_ast_t *stmt = &ctx->nodes[last->next];
        switch (stmt->type) {
        case NS_AST_JUMP_STMT:
            ns_llvm_jump_stmt(code_gen_ctx, *stmt);
            break;
        default:
            break;
        }
    }
}

bool ns_code_gen_llvm_bc(ns_parse_context_t *ctx) {
    ns_str output = ctx->output;
    if (output.data == NULL) {
        fprintf(stderr, "output file is not specified\n");
        return false;
    }

    ns_str bc_path = ns_str_cstr(output.data);
    printf("generate llvm bitcode file: %s\n", bc_path.data);

    ns_str module_name = ns_path_filename(ctx->filename);
    ns_bc_module mod = LLVMModuleCreateWithName(module_name.data);
    ns_bc_builder builder = LLVMCreateBuilder();
    ns_llvm_ctx_t code_gen_ctx = {.path = bc_path, .ctx = ctx, .mod = mod, .builder = builder};

    int i = 0;
    int l = ctx->section_count;
    while (i < l) {
        int s = ctx->sections[i++];
        ns_ast_t n = ctx->nodes[s];

        switch (n.type) {
        case NS_AST_FN_DEF:
            ns_llvm_fn_def(&code_gen_ctx, n);
            break;
        case NS_AST_COMPOUND_STMT:
            ns_llvm_compound_stmt(&code_gen_ctx, n);
            break;
        default:
            break;
        }
    }

    ns_bc_type main_func_type = LLVMFunctionType(LLVMInt32Type(), NULL, 0, 0);
    ns_bc_value main_func = LLVMAddFunction(mod, "main", main_func_type);
    ns_bc_block entry_main = LLVMAppendBasicBlock(main_func, "entry");
    LLVMPositionBuilderAtEnd(builder, entry_main);
    ns_bc_value main_ret_val = LLVMConstInt(LLVMInt32Type(), 0, 0);
    LLVMBuildRet(builder, main_ret_val);

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