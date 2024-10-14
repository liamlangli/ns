#ifdef NS_BITCODE

#include "ns_bitcode.h"
#include "ns_type.h"
#include "ns_vm.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

// types
#define ns_bc_module LLVMModuleRef
#define ns_bc_builder LLVMBuilderRef
#define ns_bc_block LLVMBasicBlockRef
#define ns_bc_type LLVMTypeRef
#define ns_bc_value LLVMValueRef

typedef struct ns_llvm_value_record {
    ns_str name;
    ns_bc_value ll_value;
    ns_bc_type ll_type;
    NS_VALUE_TYPE type;
    int i;
} ns_llvm_value_record;

typedef struct ns_llvm_fn_record {
    ns_str name;
    ns_bc_type type;
    ns_bc_value fn;
    ns_llvm_value_record *args;
} ns_llvm_fn_record;

typedef struct ns_llvm_struct_record {
    ns_str name;
    ns_bc_type type;
    ns_bc_value st;
    ns_llvm_value_record *fields;
} ns_llvm_struct_record;

typedef struct ns_llvm_call_record {
    ns_llvm_record *fn;
    ns_llvm_record *locals;
    ns_llvm_record ret;
} ns_llvm_call_record;

typedef enum ns_llvm_record_type {
    NS_LLVM_RECORD_TYPE_INVALID,
    NS_LLVM_RECORD_TYPE_VALUE,
    NS_LLVM_RECORD_TYPE_FN,
    NS_LLVM_RECORD_TYPE_STRUCT,
} ns_llvm_record_type;
#define NS_LLVM_RECORD_INVALID ((ns_llvm_record){.type = NS_LLVM_RECORD_TYPE_INVALID})
#define NS_LLVM_VALUE_RECORD_INVALID ((ns_llvm_value_record){.name = ns_str_null, .ll_value = NULL, .ll_type = NULL, .type = NS_TYPE_INFER, .i = -1})

typedef struct ns_llvm_record {
    ns_llvm_record_type type;
    union {
        ns_llvm_value_record val;
        ns_llvm_fn_record fn;
        ns_llvm_struct_record st;
    };
}  ns_llvm_record;

typedef struct ns_llvm_ctx {
    ns_str path;
    ns_ast_ctx *ctx;
    ns_bc_module mod;
    ns_bc_builder builder;
    ns_llvm_record *fn;

    // variables
    ns_llvm_record *locals;
    ns_llvm_record *globals;
    int local_stack_index;
    int local_stack[NS_MAX_CALL_STACK];
} ns_llvm_ctx;

// util
const char* ns_llvm_str(ns_str s);
ns_str ns_tmp_var_indexed(int i);
ns_str ns_tmp_var(ns_llvm_ctx *ctx);
ns_bc_type ns_llvm_type(ns_llvm_ctx *ctx, ns_type t);
ns_bc_value ns_llvm_find_var(ns_llvm_ctx *llvm_ctx, ns_str name);
ns_llvm_record ns_llvm_find_fn(ns_llvm_ctx *llvm_ctx, ns_str name);
ns_llvm_record ns_llvm_find_struct(ns_llvm_ctx *llvm_ctx, ns_str name);
ns_llvm_record ns_llvm_struct_find_member(ns_llvm_record s, ns_str name);
int ns_llvm_push_local(ns_llvm_ctx *llvm_ctx, ns_llvm_record r);
int ns_llvm_push_global(ns_llvm_ctx *llvm_ctx, ns_llvm_record r);

// expr
ns_bc_value ns_llvm_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n);
ns_bc_value ns_llvm_call_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n);
ns_bc_value ns_llvm_primary_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n);
ns_bc_value ns_llvm_binary_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n);

// stmt
int ns_llvm_fn_def(ns_llvm_ctx *llvm_ctx, ns_ast_t n);
int ns_llvm_struct_def(ns_llvm_ctx *llvm_ctx, ns_ast_t n);
ns_bc_value ns_llvm_jump_stmt(ns_llvm_ctx *llvm_ctx, ns_ast_t n);
void ns_llvm_compound_stmt(ns_llvm_ctx *llvm_ctx, ns_ast_t n);

// impl
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

ns_str ns_tmp_var(ns_llvm_ctx *ctx) {
    int i = ns_array_length(ctx->locals);
    return ns_tmp_var_indexed(i);
}

ns_bc_type ns_llvm_type(ns_llvm_ctx *ctx, ns_type t) {
    switch (t.type) {
    case NS_TYPE_I8:
    case NS_TYPE_U8:
        return LLVMInt8Type();
    case NS_TYPE_I16:
    case NS_TYPE_U16:
        return LLVMInt16Type();
    case NS_TYPE_I32:
    case NS_TYPE_U32:
        return LLVMInt32Type();
    case NS_TYPE_I64:
    case NS_TYPE_U64:
        return LLVMInt64Type();
    case NS_TYPE_F32:
        return LLVMFloatType();
    case NS_TYPE_F64:
        return LLVMDoubleType();
    case NS_TOKEN_IDENTIFIER:
        return ns_llvm_find_fn(ctx, t.name).st.type;
    default:
        ns_error("bitcode error:", "unknown type %s\n", ns_llvm_str(t.name));
        break;
    }
    return NULL;
}

int ns_llvm_push_local(ns_llvm_ctx *llvm_ctx, ns_llvm_record r) {
    int i = ns_array_length(llvm_ctx->locals);
    ns_array_push(llvm_ctx->locals, r);
    return i;
}

int ns_llvm_push_global(ns_llvm_ctx *llvm_ctx, ns_llvm_record r) {
    int i = ns_array_length(llvm_ctx->globals);
    ns_array_push(llvm_ctx->globals, r);
    return i;
}

ns_bc_value ns_llvm_find_var(ns_llvm_ctx *llvm_ctx, ns_str name) {
    if (llvm_ctx->fn->type != NS_LLVM_RECORD_TYPE_INVALID) {
        ns_llvm_record *fn = llvm_ctx->fn;
        for (int i = 0, l = ns_array_length(fn->fn.args); i < l; i++) {
            if (ns_str_equals(fn->fn.args[i].name, name)) {
                return LLVMGetParam(fn->fn.fn, i);
            }
        }
    }

    for (int i = 0, l = ns_array_length(llvm_ctx->locals); i < l; i++) {
        if (ns_str_equals(llvm_ctx->locals[i].val.name, name)) {
            return llvm_ctx->locals[i].val.ll_value;
        }
    }

    for (int i = 0, l = ns_array_length(llvm_ctx->globals); i < l; i++) {
        if (ns_str_equals(llvm_ctx->globals[i].val.name, name)) {
            return llvm_ctx->globals[i].val.ll_value;
        }
    }
    return NULL;
}

ns_llvm_record ns_llvm_find_fn(ns_llvm_ctx *llvm_ctx, ns_str name) {
    for (int i = 0, l = ns_array_length(llvm_ctx->globals); i < l; i++) {
        if (ns_str_equals(llvm_ctx->globals[i].fn.name, name) && llvm_ctx->globals[i].type == NS_LLVM_RECORD_TYPE_FN) {
            return llvm_ctx->globals[i];
        }
    }
    return NS_LLVM_RECORD_INVALID;
}

ns_llvm_record ns_llvm_find_struct(ns_llvm_ctx *llvm_ctx, ns_str name) {
    for (int i = 0, l = ns_array_length(llvm_ctx->globals); i < l; i++) {
        if (ns_str_equals(llvm_ctx->globals[i].st.name, name) && llvm_ctx->globals[i].type == NS_LLVM_RECORD_TYPE_STRUCT) {
            return llvm_ctx->globals[i];
        }
    }

    ns_error("bitcode error", "unknown type %s\n", ns_llvm_str(name));
    return NS_LLVM_RECORD_INVALID;
}

ns_llvm_record ns_llvm_struct_find_member(ns_llvm_record s, ns_str name) {
    // for (int i = 0, l = ns_array_length(s.st.fields); i < l; i++) {
    //     if (ns_str_equals(s.st.fields[i].name, name)) {
    //         return s.st.fields[i];
    //     }
    // }
    return NS_LLVM_RECORD_INVALID;
}

ns_bc_value ns_llvm_primary_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n) {
    switch (n.primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
    case NS_TOKEN_FLT_LITERAL:
        return LLVMConstReal(LLVMDoubleType(), n.primary_expr.token.val.len);
    case NS_TOKEN_STR_LITERAL:
        return LLVMBuildGlobalStringPtr(llvm_ctx->builder, ns_str_unescape(n.primary_expr.token.val).data, "");
    case NS_TOKEN_IDENTIFIER:
        return ns_llvm_find_var(llvm_ctx, n.primary_expr.token.val);
    default:
        break;
    }
    return NULL;
}

ns_bc_value ns_llvm_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n) {
    ns_ast_ctx *ctx = llvm_ctx->ctx;
    switch (n.type) {
    case NS_AST_EXPR:
        return ns_llvm_expr(llvm_ctx, ctx->nodes[n.expr.body]);
    case NS_AST_BINARY_EXPR:
        return ns_llvm_binary_expr(llvm_ctx, n);
    case NS_AST_PRIMARY_EXPR:
        return ns_llvm_primary_expr(llvm_ctx, n);
    case NS_AST_CALL_EXPR:
        return ns_llvm_call_expr(llvm_ctx, n);
    default:
        break;
    }
    return NULL;
}

void ns_bitcode_struct_def(ns_llvm_ctx *llvm_ctx, ns_vm *vm) {
    ns_bc_module mod = llvm_ctx->mod;
    ns_bc_builder builder = llvm_ctx->builder;

    for (int i = 0, l = ns_array_length(vm->records); i < l; i++) {
        ns_record r = vm->records[i];
        if (r.type != NS_RECORD_STRUCT) continue;
        ns_bc_type *fields = NULL;
        i32 field_count = ns_array_length(r.st.fields);
        ns_array_set_length(fields, field_count);
        for (int j = 0; j < field_count; j++) {
            fields[j] = ns_llvm_type(vm, r.st.fields[j].val.type);
        }
        ns_bc_type struct_type = LLVMStructType(fields, field_count, 0);
        ns_bc_value st = LLVMAddStruct(mod, ns_llvm_str(r.name), struct_type);
        ns_llvm_record struct_record = {.type = NS_LLVM_RECORD_TYPE_STRUCT, .st = {.name = r.name}};
        struct_record.st.type = struct_type;
        struct_record.st.st = st;
        ns_llvm_push_global(llvm_ctx, struct_record);
    }
}

void ns_bitcode_fn_def(ns_llvm_ctx *llvm_ctx, ns_vm *vm, ns_ast_ctx *ctx) {
    ns_bc_module mod = llvm_ctx->mod;
    ns_bc_builder builder = llvm_ctx->builder;

    for (int i = 0, l = ns_array_length(vm->records); i < l; i++) {
        ns_record r = vm->records[i];
        if (r.type != NS_RECORD_FN) continue;
        ns_bc_type *args = NULL;
        i32 arg_count = ns_array_length(r.fn.args);
        ns_array_set_length(args, arg_count);
        for (int j = 0; j < arg_count; j++) {
            args[j] = ns_llvm_type(vm, r.fn.args[j].val.type);
        }
        ns_bc_type ret = ns_llvm_type(vm, r.fn.ret);
        ns_bc_type fn_type = LLVMFunctionType(ret, args, arg_count, 0);
        ns_bc_value fn = LLVMAddFunction(mod, ns_llvm_str(r.name), fn_type);
        ns_llvm_record fn_record = {.type = NS_LLVM_RECORD_TYPE_FN, .fn = {.name = r.name}};
        fn_record.fn.type = fn_type;
        fn_record.fn.fn = fn;
        ns_llvm_push_global(llvm_ctx, fn_record);

        ns_bc_block entry = LLVMAppendBasicBlock(fn, "entry");
        LLVMPositionBuilderAtEnd(builder, entry);
        ns_llvm_compound_stmt(llvm_ctx, ctx->nodes[r.fn.ast]);
    }
}


ns_bc_value ns_llvm_binary_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n) {
    ns_ast_ctx *ctx = llvm_ctx->ctx;
    ns_bc_builder builder = llvm_ctx->builder;

    ns_bc_value left = ns_llvm_expr(llvm_ctx, ctx->nodes[n.binary_expr.left]);
    ns_bc_value right = ns_llvm_expr(llvm_ctx, ctx->nodes[n.binary_expr.right]);
    ns_bc_value ret = NULL;
    switch (n.binary_expr.op.type) {
    case NS_TOKEN_ADD_OP:
        if (ns_str_equals_STR(n.binary_expr.op.val, "+")) {
            ret = LLVMBuildFAdd(builder, left, right, "");
        } else if (ns_str_equals_STR(n.binary_expr.op.val, "-")) {
            ret = LLVMBuildFSub(builder, left, right, "");
        }
        break;
    case NS_TOKEN_MUL_OP:
        if (ns_str_equals_STR(n.binary_expr.op.val, "*")) {
            ret = LLVMBuildFMul(builder, left, right, "");
        } else if (ns_str_equals_STR(n.binary_expr.op.val, "/")) {
            ret = LLVMBuildFDiv(builder, left, right, "");
        }
        break;
    case NS_TOKEN_ASSIGN_OP:
        ret = LLVMBuildStore(builder, right, left);
        break;
    default:
        assert(false); // unexpected operator
        break;
    }
    return ret;
}

ns_bc_value ns_llvm_jump_stmt(ns_llvm_ctx *llvm_ctx, ns_ast_t n) {
    ns_ast_ctx *ctx = llvm_ctx->ctx;
    ns_bc_builder builder = llvm_ctx->builder;

    ns_token_t t = n.jump_stmt.label;
    if (ns_str_equals_STR(t.val, "return")) {
        if (n.jump_stmt.expr != -1) {
            ns_bc_value expr = ns_llvm_expr(llvm_ctx, ctx->nodes[n.jump_stmt.expr]);
            return LLVMBuildRet(builder, expr);
        } else {
            return LLVMBuildRetVoid(builder);
        }
    }
    return NULL;
}

void ns_llvm_compound_stmt(ns_llvm_ctx *llvm_ctx, ns_ast_t n) {
    ns_ast_ctx *ctx = llvm_ctx->ctx;

    ns_ast_t *stmt = &n;
    for (int i = 0; i < n.compound_stmt.count; i++) {
        stmt = &ctx->nodes[stmt->next];
        switch (stmt->type) {
        case NS_AST_JUMP_STMT:
            ns_llvm_jump_stmt(llvm_ctx, *stmt);
            break;
        default:
            break;
        }
    }
}

ns_bc_value ns_llvm_call_expr(ns_llvm_ctx *llvm_ctx, ns_ast_t n) {
    ns_ast_ctx *ctx = llvm_ctx->ctx;
    ns_bc_builder builder = llvm_ctx->builder;

    ns_ast_t *arg = &n;
    ns_bc_value *args = malloc(n.call_expr.arg_count * sizeof(ns_bc_value));
    for (int i = 0; i < n.call_expr.arg_count; i++) {
        arg = &ctx->nodes[arg->next];
        args[i] = ns_llvm_expr(llvm_ctx, *arg);
    }
    ns_str fn_name = ctx->nodes[n.call_expr.callee].primary_expr.token.val;
    ns_llvm_record fn = ns_llvm_find_fn(llvm_ctx, fn_name);
    if (fn.type == NS_LLVM_RECORD_TYPE_INVALID) {
        ns_error("bitcode error", "unknown function %s\n", ns_llvm_str(fn_name));
    }

    ns_bc_value ret = LLVMBuildCall2(builder, fn.fn.type, fn.fn.fn, args, n.call_expr.arg_count, "");
    return ret;
}

void ns_llvm_std(ns_llvm_ctx *llvm_ctx) {
    ns_bc_module module = llvm_ctx->mod;

    // register printf
    LLVMTypeRef printf_args[] = { LLVMPointerType(LLVMInt8Type(), 0) }; // char* type
    LLVMTypeRef printf_type = LLVMFunctionType(LLVMInt32Type(), printf_args, 1, 1); // variadic function
    LLVMValueRef printf_func = LLVMAddFunction(module, "printf", printf_type);
    ns_llvm_record printf_record = {.type = NS_LLVM_RECORD_TYPE_FN, .fn = {.name = ns_str_cstr("print"), .args = 0}};
    printf_record.fn.fn = printf_func;
    printf_record.fn.type = printf_type;
    ns_llvm_push_global(llvm_ctx, printf_record);
}

bool ns_bitcode_gen(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_str output_path = ns_str_cstr(ctx->output.data);
    ns_info("bitcode", "generate llvm bitcode file %s\n", output_path.data);

    ns_str module_name = ns_path_filename(ctx->filename);
    ns_bc_module mod = LLVMModuleCreateWithName(module_name.data);
    ns_bc_builder builder = LLVMCreateBuilder();
    ns_llvm_ctx llvm_ctx = {0};
    llvm_ctx.path = output_path;
    llvm_ctx.ctx = ctx;
    llvm_ctx.mod = mod;
    llvm_ctx.builder = builder;
    llvm_ctx.fn = NULL;

    ns_llvm_std(&llvm_ctx);
    ns_bitcode_fn_def(&llvm_ctx, vm, ctx);
    ns_bitcode_struct_def(&llvm_ctx, vm);

    ns_bc_type main_func_type = LLVMFunctionType(LLVMInt32Type(), NULL, 0, 0);
    ns_bc_value main_func = LLVMAddFunction(mod, "main", main_func_type);
    ns_bc_block entry_main = LLVMAppendBasicBlock(main_func, "entry");
    LLVMPositionBuilderAtEnd(builder, entry_main);

    // parse deferred nodes as main fn body
    for (int i = ctx->section_begin; i < ctx->section_end; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i++]];
        switch (n.type) {
        case NS_AST_VAR_DEF: {
            ns_bc_value ret = ns_llvm_expr(&llvm_ctx, ctx->nodes[n.var_def.expr]);
            ns_bc_type ll_type = ns_llvm_type(&llvm_ctx, n.var_def.type);
            if (ll_type == NULL) ll_type = LLVMTypeOf(ret);
            ns_str name = n.var_def.name.val;
            ns_bc_value var = LLVMBuildAlloca(builder, ll_type, ns_llvm_str(name));
            LLVMBuildStore(builder, ret, var);
            ns_llvm_record r = {.type = NS_LLVM_RECORD_TYPE_VALUE, .val = {.name = name, .ll_value = var, .ll_type = ll_type }};
            ns_llvm_push_global(&llvm_ctx, r);
        } break;
        case NS_AST_CALL_EXPR:
            ns_llvm_call_expr(&llvm_ctx, n);
            break;
        default:
            break;
        }
    }

    LLVMBuildRet(builder, LLVMConstInt(LLVMInt32Type(), 0, 0));

    char *error = NULL;
    LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    // Write out bitcode to file
    if (LLVMWriteBitcodeToFile(mod, output_path.data) != 0) {
        ns_error("bitcode error", "fail writing bitcode to file.");
        return false;
    }

    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(mod);
    return true;
}

#endif // NS_BITCODE