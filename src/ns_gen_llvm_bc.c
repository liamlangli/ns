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

#define MAX_PARAM_COUNT 16
#define MAX_LOCAL_COUNT 32
#define MAX_GLOBAL_COUNT 64
#define MAX_STACK_DEPTH 16

typedef struct ns_llvm_value_record {
    ns_str name;
    ns_bc_value value;
    int i;
} ns_llvm_value_record;

typedef struct ns_llvm_ctx_t {
    ns_str path;
    ns_parse_context_t *ctx;
    ns_bc_module mod;
    ns_bc_builder builder;
    ns_bc_value fn;
    ns_llvm_value_record params[MAX_PARAM_COUNT];
    int param_count;
    ns_llvm_value_record locals[MAX_LOCAL_COUNT];
    int local_count;
    ns_llvm_value_record globals[MAX_GLOBAL_COUNT];
    int global_count;
} ns_llvm_ctx_t;

#define MAX_STR_LENGTH 128
static i8 _str_buff[MAX_STR_LENGTH];
const char* ns_llvm_str(ns_str s) {
    if (s.len >= MAX_STR_LENGTH) {
        assert(false);
    }
    ns_str ret = {.data = _str_buff, .len = 0};
    for (int i = 0; i < s.len; i++) {
        _str_buff[i] = s.data[i];
        ret.len++;
    }
    _str_buff[ret.len] = '\0';
    return _str_buff;
}

ns_bc_type ns_llvm_type(ns_token_t t);
ns_bc_value ns_llvm_find_variable(ns_llvm_ctx_t *code_gen_ctx, ns_str name);
int ns_llvm_push_param(ns_llvm_ctx_t *code_gen_ctx, ns_str name, int i);
int ns_llvm_push_local(ns_llvm_ctx_t *code_gen_ctx, ns_str name, ns_bc_value value);
int ns_llvm_push_global(ns_llvm_ctx_t *code_gen_ctx, ns_str name, ns_bc_value value);

ns_bc_value ns_llvm_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_fn_def(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_binary_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_jump_stmt(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
void ns_llvm_compound_stmt(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);
ns_bc_value ns_llvm_primary_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n);

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

int ns_llvm_push_param(ns_llvm_ctx_t *code_gen_ctx, ns_str name, int i) {
    if (code_gen_ctx->param_count >= MAX_PARAM_COUNT) {
        assert(false);
    }
    code_gen_ctx->params[code_gen_ctx->param_count++] = (ns_llvm_value_record){.name = name, .i = i};
    return code_gen_ctx->param_count;
}

int ns_llvm_push_local(ns_llvm_ctx_t *code_gen_ctx, ns_str name, ns_bc_value value) {
    if (code_gen_ctx->local_count >= MAX_LOCAL_COUNT) {
        assert(false);
    }
    code_gen_ctx->locals[code_gen_ctx->local_count++] = (ns_llvm_value_record){.name = name, .value = value};
    return code_gen_ctx->local_count;
}

int ns_llvm_push_global(ns_llvm_ctx_t *code_gen_ctx, ns_str name, ns_bc_value value) {
    if (code_gen_ctx->global_count >= MAX_GLOBAL_COUNT) {
        assert(false);
    }
    code_gen_ctx->globals[code_gen_ctx->global_count++] = (ns_llvm_value_record){.name = name, .value = value};
    return code_gen_ctx->global_count;
}

ns_bc_value ns_llvm_find_variable(ns_llvm_ctx_t *code_gen_ctx, ns_str name) {
    for (int i = 0; i < code_gen_ctx->param_count; i++) {
        if (ns_str_equals(code_gen_ctx->params[i].name, name)) {
            return LLVMGetParam(code_gen_ctx->fn, i);
        }
    }
    for (int i = 0; i < code_gen_ctx->local_count; i++) {
        if (ns_str_equals(code_gen_ctx->locals[i].name, name)) {
            return code_gen_ctx->locals[i].value;
        }
    }
    for (int i = 0; i < code_gen_ctx->global_count; i++) {
        if (ns_str_equals(code_gen_ctx->globals[i].name, name)) {
            return code_gen_ctx->globals[i].value;
        }
    }
    return NULL;
}

ns_bc_value ns_llvm_primary_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    switch (n.primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return LLVMConstInt(LLVMInt64Type(), n.primary_expr.token.val.len, 0);
    case NS_TOKEN_FLOAT_LITERAL:
        return LLVMConstReal(LLVMDoubleType(), n.primary_expr.token.val.len);
    case NS_TOKEN_STRING_LITERAL:
        return LLVMConstString(n.primary_expr.token.val.data, n.primary_expr.token.val.len, 0);
    case NS_TOKEN_IDENTIFIER:
        return ns_llvm_find_variable(code_gen_ctx, n.primary_expr.token.val);
    default:
        break;
    }
    return NULL;
}

ns_bc_value ns_llvm_expr(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    switch (n.type) {
    case NS_AST_EXPR:
        return ns_llvm_expr(code_gen_ctx, ctx->nodes[n.expr.body]);
    case NS_AST_BINARY_EXPR:
        return ns_llvm_binary_expr(code_gen_ctx, n);
    case NS_AST_PRIMARY_EXPR:
        return ns_llvm_primary_expr(code_gen_ctx, n);
    default:
        break;
    }
    return NULL;
}

ns_bc_value ns_llvm_fn_def(ns_llvm_ctx_t *code_gen_ctx, ns_ast_t n) {
    ns_parse_context_t *ctx = code_gen_ctx->ctx;
    ns_bc_builder builder = code_gen_ctx->builder;

    i32 param_count = n.fn_def.param_count;
    ns_bc_type param_types[MAX_PARAM_COUNT];
    for (int i = 0; i < n.fn_def.param_count; i++) {
        ns_ast_t p = ctx->nodes[n.fn_def.params[i]];
        if (p.param.is_ref) {
            // TODO: handle ref type
        }
        param_types[i] = ns_llvm_type(p.param.type);
        ns_llvm_push_param(code_gen_ctx, p.param.name.val, i);
    }
    ns_bc_type ret_type = ns_llvm_type(n.fn_def.return_type);
    ns_bc_type fn_type = LLVMFunctionType(ret_type, param_types, param_count, 0);
    ns_bc_value fn = LLVMAddFunction(code_gen_ctx->mod, ns_llvm_str(n.fn_def.name.val), fn_type);
    if (code_gen_ctx->fn) {
        assert(false); // nested function is not supported
    }
    code_gen_ctx->fn = fn;
    ns_bc_block entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    ns_llvm_compound_stmt(code_gen_ctx, ctx->nodes[n.fn_def.body]);
    code_gen_ctx->fn = NULL;
    code_gen_ctx->param_count = 0;
    code_gen_ctx->local_count = 0;
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
    ns_llvm_ctx_t code_gen_ctx = {0};
    code_gen_ctx.path = bc_path;
    code_gen_ctx.ctx = ctx;
    code_gen_ctx.mod = mod;
    code_gen_ctx.builder = builder;

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