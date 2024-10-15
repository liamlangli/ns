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
#define ns_bc_type_ref LLVMTypeRef
#define ns_bc_value_ref LLVMValueRef

typedef struct ns_bc_type {
    ns_type raw;
    ns_bc_type_ref type;
} ns_bc_type;

#define ns_bc_type_unknown ((ns_bc_type){.type = NULL, .raw = ns_type_unknown})
#define ns_bc_type_infer ((ns_bc_type){.type = NULL, .raw = ns_type_infer})
#define ns_bc_type_nil ((ns_bc_type){.type = LLVMVoidType(), .raw = ns_type_nil})
#define ns_bc_type_bool ((ns_bc_type){.type = LLVMInt32Type(), .raw = ns_type_bool})
#define ns_bc_type_string ((ns_bc_type){.type = LLVMPointerType(LLVMInt8Type(), 0), .raw = ns_type_string})
#define ns_bc_type_i64 ((ns_bc_type){.type = LLVMInt64Type(), .raw = ns_type_i64})
#define ns_bc_type_i8 ((ns_bc_type){.type = LLVMInt8Type(), .raw = ns_type_i8})
#define ns_bc_type_i16 ((ns_bc_type){.type = LLVMInt16Type(), .raw = ns_type_i16})
#define ns_bc_type_i32 ((ns_bc_type){.type = LLVMInt32Type(), .raw = ns_type_i32})
#define ns_bc_type_u8 ns_bc_type_i8
#define ns_bc_type_u16 ns_bc_type_i16
#define ns_bc_type_u32 ns_bc_type_i32
#define ns_bc_type_u64 ns_bc_type_i64
#define ns_bc_type_f32 ((ns_bc_type){.type = LLVMFloatType(), .raw = ns_type_f32})
#define ns_bc_type_f64 ((ns_bc_type){.type = LLVMDoubleType(), .raw = ns_type_f64})
#define ns_bc_type_is_float(t) ((t).raw.type == NS_TYPE_F64 || (t).raw.type == NS_TYPE_F64)

typedef struct ns_bc_value {
    ns_bc_type type;
    ns_bc_value_ref val;
    i32 p;
} ns_bc_value;

#define ns_bc_nil ((ns_bc_value){.val = NULL, .type = ns_bc_type_nil, .p = -1})

typedef struct ns_bc_record ns_bc_record;

typedef struct ns_bc_value_record {
    ns_bc_value val;
} ns_bc_value_record;

typedef struct ns_bc_fn_record {
    ns_bc_record *args;
    ns_bc_type ret;
    ns_bc_value fn;
} ns_bc_fn_record;

typedef struct ns_bc_struct_record {
    ns_bc_value st;
    ns_bc_record *fields;
} ns_bc_struct_record;

typedef enum ns_bc_record_type {
    NS_BC_INVALID,
    NS_BC_VALUE,
    NS_BC_FN,
    NS_BC_STRUCT
} ns_bc_record_type;

typedef struct ns_bc_record {
    ns_bc_record_type type;
    ns_str name;
    i32 index;
    union {
        ns_bc_value_record val;
        ns_bc_fn_record fn;
        ns_bc_struct_record st;
    };
}  ns_bc_record;

#define ns_bc_nil_record ((ns_bc_record){.type = NS_BC_INVALID})

typedef struct ns_bc_call {
    ns_bc_record *fn;
    ns_bc_record *locals;
    ns_bc_value *args;
    ns_bc_value ret;
} ns_bc_call;

typedef struct ns_bc_ctx {
    ns_ast_ctx *ctx;
    ns_vm *vm;

    ns_bc_module mod;
    ns_bc_builder builder;

    ns_bc_record *records;
    ns_bc_call *call_stack;
} ns_bc_ctx;

// util
const char* ns_bc_str(ns_str s);
ns_bc_type ns_bc_parse_type(ns_bc_ctx *bc_ctx, ns_type t);
ns_bc_value ns_bc_find_value(ns_bc_ctx *bc_ctx, ns_str name);

// expr
ns_bc_value ns_bc_expr(ns_bc_ctx *bc_ctx, int i);
ns_bc_value ns_bc_call_expr(ns_bc_ctx *bc_ctx, int i);
ns_bc_value ns_bc_primary_expr(ns_bc_ctx *bc_ctx, int i);
ns_bc_value ns_bc_binary_expr(ns_bc_ctx *bc_ctx, int i);

ns_bc_value ns_bc_jump_stmt(ns_bc_ctx *bc_ctx, int i);
void ns_bc_compound_stmt(ns_bc_ctx *bc_ctx, int i);

// impl
#define MAX_STR_LENGTH 128
static i8 _str_buff[MAX_STR_LENGTH];
const char* ns_bc_str(ns_str s) {
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

ns_bc_type ns_bc_parse_type(ns_bc_ctx *bc_ctx, ns_type t) {
    switch (t.type) {
    case NS_TYPE_I8:
    case NS_TYPE_U8:
        return ns_bc_type_i8;
    case NS_TYPE_I16:
    case NS_TYPE_U16:
        return ns_bc_type_i16;
    case NS_TYPE_I32:
    case NS_TYPE_U32:
        return ns_bc_type_i32;
    case NS_TYPE_I64:
    case NS_TYPE_U64:
        return ns_bc_type_i64;
    case NS_TYPE_F32:
        return ns_bc_type_f32;
    case NS_TYPE_F64:
        return ns_bc_type_f64;
    case NS_TOKEN_IDENTIFIER:
        return ns_bc_find_value(bc_ctx, ns_vm_get_type_name(bc_ctx->vm, t)).type;
    default:
        ns_str n = ns_vm_get_type_name(bc_ctx->vm, t);
        ns_error("bitcode error:", "unknown type %.*s", n.len, n.data);
        break;
    }
    return ns_bc_type_nil;
}

int ns_bc_push_record(ns_bc_ctx *bc_ctx, ns_bc_record r) {
    r.index = ns_array_length(bc_ctx->records);
    switch (r.type)
    {
    case NS_BC_FN: r.fn.fn.p = r.index; break;
    case NS_BC_VALUE: r.val.val.p = r.index; break;
    default: break;
    }
    ns_array_push(bc_ctx->records, r);
    return r.index;
}

ns_bc_value ns_bc_find_value(ns_bc_ctx *bc_ctx, ns_str name) {
    if (ns_array_length(bc_ctx->call_stack) > 0) {
        ns_bc_call *call = &bc_ctx->call_stack[ns_array_length(bc_ctx->call_stack) - 1];
        ns_bc_record *fn = call->fn;
        for (int i = 0, l = ns_array_length(fn->fn.args); i < l; i++) {
            if (ns_str_equals(fn->fn.args[i].name, name)) {
                return call->args[i];
            }
        }

        for (int i = 0, l = ns_array_length(call->locals); i < l; i++) {
            if (ns_str_equals(call->locals[i].name, name)) {
                return call->locals[i].val.val;
            }
        }
    }

    for (int i = 0, l = ns_array_length(bc_ctx->records); i < l; i++) {
        if (ns_str_equals(bc_ctx->records[i].name, name)) {
            ns_bc_record *r = &bc_ctx->records[i];
            switch (r->type)
            {
            case NS_BC_VALUE: return r->val.val;
            case NS_BC_FN: return r->fn.fn;
            default: break;
            }
        }
    }
    return ns_bc_nil;
}

ns_bc_value ns_bc_primary_expr(ns_bc_ctx *bc_ctx, int i) {
    ns_ast_t n = bc_ctx->ctx->nodes[i];
    ns_token_t t = n.primary_expr.token;
    switch (n.primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return (ns_bc_value){.val = LLVMConstInt(LLVMInt32Type(), ns_str_to_i32(t.val), 0), .type = ns_bc_type_i32};
    case NS_TOKEN_FLT_LITERAL:
        return (ns_bc_value){.val = LLVMConstReal(LLVMDoubleType(), ns_str_to_f64(t.val)), .type = ns_bc_type_f64};
    case NS_TOKEN_STR_LITERAL:
        return (ns_bc_value){ .val = LLVMBuildGlobalStringPtr(bc_ctx->builder, ns_str_unescape(n.primary_expr.token.val).data, ""), .type = ns_bc_type_string };
    case NS_TOKEN_IDENTIFIER:
        return ns_bc_find_value(bc_ctx, n.primary_expr.token.val);
    default:
        break;
    }
    return ns_bc_nil;
}

ns_bc_value ns_bc_expr(ns_bc_ctx *bc_ctx, int i) {
    ns_ast_t n = bc_ctx->ctx->nodes[i];
    switch (n.type) {
    case NS_AST_EXPR:
        return ns_bc_expr(bc_ctx, n.expr.body);
    case NS_AST_BINARY_EXPR:
        return ns_bc_binary_expr(bc_ctx, i);
    case NS_AST_PRIMARY_EXPR:
        return ns_bc_primary_expr(bc_ctx, i);
    case NS_AST_CALL_EXPR:
        return ns_bc_call_expr(bc_ctx, i);
    default:
        break;
    }
    return ns_bc_nil;
}

void ns_bc_struct_def(ns_bc_ctx *bc_ctx, ns_vm *vm) {
    ns_bc_module mod = bc_ctx->mod;
    ns_bc_builder bdr = bc_ctx->builder;

    // for (int i = 0, l = ns_array_length(vm->records); i < l; i++) {
    //     ns_record r = vm->records[i];
    //     if (r.type != NS_RECORD_STRUCT) continue;

    //     ns_bc_record struct_record = {.type = NS_BC_STRUCT, .st = };
    //     ns_bc_type_ref *fields = NULL;
    //     i32 field_count = ns_array_length(r.st.fields);
    //     ns_array_set_length(fields, field_count);
    //     for (int j = 0; j < field_count; j++) {
    //         ns_bc_type t = ns_bc_parse_type(vm, r.st.fields[j].val.type);
    //         fields[j] = t.type;
    //         struct_record.st.fields[j].val
    //     }
    //     ns_bc_type struct_type = LLVMStructType(fields, field_count, 0);
    //     ns_bc_value st = LLVMAddStruct(mod, ns_bc_str(r.name), struct_type);
        
    //     struct_record.st.type = struct_type;
    //     struct_record.st.st = st;
    //     ns_bc_push_global(bc_ctx, struct_record);
    // }
}

void ns_bitcode_fn_def(ns_bc_ctx *bc_ctx, ns_vm *vm, ns_ast_ctx *ctx) {
    ns_bc_module mod = bc_ctx->mod;
    ns_bc_builder bdr = bc_ctx->builder;

    for (int i = 0, l = ns_array_length(vm->records); i < l; i++) {
        ns_record r = vm->records[i];
        if (r.type != NS_RECORD_FN) continue;
        ns_bc_type *args = NULL;
        i32 arg_count = ns_array_length(r.fn.args);
        ns_array_set_length(args, arg_count);
        for (int j = 0; j < arg_count; j++) {
            args[j] = ns_bc_parse_type(vm, r.fn.args[j].val.type);
        }
        ns_bc_type ret = ns_bc_parse_type(vm, r.fn.ret);
        ns_bc_type_ref fn_type = LLVMFunctionType(ret.type, args, arg_count, 0);
        ns_bc_value_ref fn = LLVMAddFunction(mod, ns_bc_str(r.name), fn_type);
        ns_bc_record fn_record = (ns_bc_record){.type = NS_BC_FN, .name = r.name};
        ns_bc_value fn_val = (ns_bc_value){.p = -1, .type = (ns_bc_type){.raw = {.i = i, .type = NS_TYPE_FN}, .type = fn_type}, .val = fn };
        fn_record.fn = (ns_bc_fn_record){.fn = fn_val, .args = NULL, .ret = ret};
        ns_bc_push_record(bc_ctx, fn_record);
        ns_bc_block entry = LLVMAppendBasicBlock(fn, "entry");
        LLVMPositionBuilderAtEnd(bdr, entry);
        ns_bc_compound_stmt(bc_ctx, r.fn.ast);
    }
}

ns_bc_value ns_bc_binary_ops_number(ns_bc_value lhs, ns_bc_value rhs, ns_token_t op) {
    return ns_bc_nil;
}

ns_bc_value ns_bc_binary_ops(ns_bc_ctx *bc_ctx, ns_bc_value l, ns_bc_value r, ns_token_t op) {
    if (ns_type_is_number(l.type.raw)) return ns_bc_binary_ops_number(l, r, op);
    return ns_bc_nil;
}

ns_bc_value ns_bc_binary_expr(ns_bc_ctx *bc_ctx, int i) {
    ns_ast_t n = bc_ctx->ctx->nodes[i];
    ns_bc_builder bdr = bc_ctx->builder;
    ns_bc_value l = ns_bc_expr(bc_ctx, n.binary_expr.left);
    ns_bc_value r = ns_bc_expr(bc_ctx, n.binary_expr.right);
    if (l.type.raw.type == r.type.raw.type) {
        return ns_bc_binary_ops(bc_ctx, l, r, n.binary_expr.op);
    }

    ns_bc_value ret = (ns_bc_value){.val = NULL, .type = l.type, .p = -1};
    switch (n.binary_expr.op.type) {
    case NS_TOKEN_ADD_OP:
        if (ns_str_equals_STR(n.binary_expr.op.val, "+")) {
            ret.val = LLVMBuildFAdd(bdr, l.val, r.val, "");
        } else if (ns_str_equals_STR(n.binary_expr.op.val, "-")) {
            ret.val = LLVMBuildFSub(bdr, l.val, r.val, "");
        }
        break;
    case NS_TOKEN_MUL_OP:
        if (ns_str_equals_STR(n.binary_expr.op.val, "*")) {
            ret.val = LLVMBuildFMul(bdr, l.val, r.val, "");
        } else if (ns_str_equals_STR(n.binary_expr.op.val, "/")) {
            ret.val = LLVMBuildFDiv(bdr, l.val, r.val, "");
        }
        break;
    case NS_TOKEN_ASSIGN_OP:
        ret.val = LLVMBuildStore(bdr, r.val, l.val);
        break;
    default:
        assert(false); // unexpected operator
        break;
    }
    return ret;
}

ns_bc_value ns_bc_jump_stmt(ns_bc_ctx *bc_ctx, int i) {
    ns_bc_builder bdr = bc_ctx->builder;
    ns_ast_t n = bc_ctx->ctx->nodes[i];
    ns_token_t t = n.jump_stmt.label;
    if (ns_str_equals_STR(t.val, "return")) {
        if (n.jump_stmt.expr != -1) {
            ns_bc_value ret = ns_bc_expr(bc_ctx, n.jump_stmt.expr);
            LLVMBuildRet(bdr, ret.val);
        } else {
            LLVMBuildRetVoid(bdr);
        }
    }
    return ns_bc_nil;
}

void ns_bc_compound_stmt(ns_bc_ctx *bc_ctx, int i) {
    ns_ast_t n = bc_ctx->ctx->nodes[i];
    ns_ast_t stmt = n;
    for (int i = 0; i < n.compound_stmt.count; i++) {
        switch (stmt.type) {
        case NS_AST_JUMP_STMT:
            ns_bc_jump_stmt(bc_ctx, stmt.next);
            break;
        default:
            break;
        }
        stmt = bc_ctx->ctx->nodes[stmt.next];
    }
}

ns_bc_value ns_bc_call_expr(ns_bc_ctx *bc_ctx, int i) {
    ns_ast_ctx *ctx = bc_ctx->ctx;
    ns_bc_builder bdr = bc_ctx->builder;

    ns_ast_t n = ctx->nodes[i];
    ns_ast_t arg = n;
    ns_bc_value *args = malloc(n.call_expr.arg_count * sizeof(ns_bc_value));
    for (int i = 0; i < n.call_expr.arg_count; i++) {
        args[i] = ns_bc_expr(bc_ctx, arg.next);
        arg = ctx->nodes[arg.next];
    }

    ns_bc_value fn = ns_bc_expr(bc_ctx, n.call_expr.callee);
    if (fn.type.raw.type != NS_TYPE_FN || fn.p == -1) {
        ns_error("bitcode error", "invalid callee");
    }

    ns_bc_record fn_record = bc_ctx->records[fn.p];
    ns_bc_value fn_val = fn_record.fn.fn;

    ns_bc_value ret = (ns_bc_value){.p = -1, .val = NULL, .type = fn_record.fn.ret };
    ret.val = LLVMBuildCall2(bdr, fn_val.val, fn_val.type.type, args, n.call_expr.arg_count, "");
    return ret;
}

void ns_bc_std(ns_bc_ctx *bc_ctx) {
    ns_bc_module module = bc_ctx->mod;

    // register printf
    LLVMTypeRef printf_args[] = { LLVMPointerType(LLVMInt8Type(), 0) }; // char* type
    LLVMTypeRef printf_type = LLVMFunctionType(LLVMInt32Type(), printf_args, 1, 1); // variadic function
    LLVMValueRef printf_func = LLVMAddFunction(module, "printf", printf_type);
    // ns_bc_record printf_record = {.type = NS_BC_FN, .name =  .fn = {.name = ns_str_cstr("print"), .args = 0}};
    // printf_record.fn.fn = printf_func;
    // printf_record.fn.type = printf_type;
    // ns_bc_push_global(bc_ctx, printf_record);
}

bool ns_bc_gen(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_str output_path = ns_str_cstr(ctx->output.data);
    ns_info("bitcode", "generate llvm bitcode file %s\n", output_path.data);

    ns_str module_name = ns_path_filename(ctx->filename);
    ns_bc_module mod = LLVMModuleCreateWithName(module_name.data);
    ns_bc_builder bdr = LLVMCreateBuilder();
    ns_bc_ctx bc_ctx = {0};
    bc_ctx.ctx = ctx;
    bc_ctx.vm = vm;
    bc_ctx.mod = mod;
    bc_ctx.builder = bdr;

    ns_bc_std(&bc_ctx);
    ns_bitcode_fn_def(&bc_ctx, vm, ctx);
    ns_bc_struct_def(&bc_ctx, vm);
    
    ns_bc_type main_fn_type = (ns_bc_type){.type = NULL, .raw = (ns_type){.type = NS_TYPE_FN, .i = -1}};
    main_fn_type.type = LLVMFunctionType(LLVMInt32Type(), NULL, 0, 0);
    ns_bc_value main_fn = (ns_bc_value){.p = -1, .val = NULL, .type = main_fn_type};
    main_fn.val = LLVMAddFunction(mod, "main", main_fn_type.type);
    ns_bc_block entry_main = LLVMAppendBasicBlock(main_fn.val, "entry");
    LLVMPositionBuilderAtEnd(bc_ctx.builder, entry_main);

    // parse deferred nodes as main fn body
    for (int i = ctx->section_begin; i < ctx->section_end; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        switch (n.type) {
        case NS_AST_VAR_DEF: {
            // ns_bc_value ret = ns_bc_expr(&bc_ctx, n.var_def.expr);
            // ns_bc_type ll_type = ns_bc_type(&bc_ctx, n.var_def.type);
            // if (ll_type == NULL) ll_type = LLVMTypeOf(ret);
            // ns_str name = n.var_def.name.val;
            // ns_bc_value var = LLVMBuildAlloca(bdr, ll_type, ns_bc_str(name));
            // LLVMBuildStore(builder, ret, var);
            // ns_bc_record r = {.type = NS_BC_VALUE, .val = {.name = name, .ll_value = var, .ll_type = ll_type }};
            // ns_bc_push_global(&bc_ctx, r);
        } break;
        case NS_AST_CALL_EXPR:
            ns_bc_call_expr(&bc_ctx, i);
            break;
        default:
            break;
        }
    }

    LLVMBuildRet(bdr, LLVMConstInt(LLVMInt32Type(), 0, 0));

    char *error = NULL;
    LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    // Write out bitcode to file
    if (LLVMWriteBitcodeToFile(mod, output_path.data) != 0) {
        ns_error("bitcode error", "fail writing bitcode to file.");
        return false;
    }

    LLVMDisposeBuilder(bdr);
    LLVMDisposeModule(mod);
    return true;
}

#endif // NS_BITCODE