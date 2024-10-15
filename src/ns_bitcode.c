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
#define ns_bc_type_str ((ns_bc_type){.type = LLVMPointerType(LLVMInt8Type(), 0), .raw = ns_type_str})
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
        ns_bc_value val;
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
void ns_bc_call_std(ns_bc_ctx *bc_ctx);

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
    case NS_TYPE_STRING:
        return ns_bc_type_str;
    case NS_TYPE_FN:
    case NS_TYPE_STRUCT:
        return bc_ctx->records[t.i].fn.ret;
    default:
        ns_error("bitcode error", "unimplemented type\n");
        break;
    }
    return ns_bc_type_nil;
}

int ns_bc_push_record(ns_bc_ctx *bc_ctx, ns_bc_record r) {
    r.index = ns_array_length(bc_ctx->records);
    switch (r.type)
    {
    case NS_BC_FN: r.fn.fn.p = r.index; break;
    case NS_BC_VALUE: r.val.p = r.index; break;
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
                return call->locals[i].val;
            }
        }
    }

    for (int i = 0, l = ns_array_length(bc_ctx->records); i < l; i++) {
        if (ns_str_equals(bc_ctx->records[i].name, name)) {
            ns_bc_record *r = &bc_ctx->records[i];
            switch (r->type)
            {
            case NS_BC_VALUE: return r->val;
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
        return (ns_bc_value){.val = LLVMBuildGlobalStringPtr(bc_ctx->builder, ns_str_unescape(n.primary_expr.token.val).data, ""), .type = ns_bc_type_str };
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
    // ns_bc_module mod = bc_ctx->mod;
    // ns_bc_builder bdr = bc_ctx->builder;

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

void ns_bc_fn_def(ns_bc_ctx *bc_ctx, ns_vm *vm, ns_ast_ctx *ctx) {
    ns_bc_module mod = bc_ctx->mod;
    ns_bc_builder bdr = bc_ctx->builder;

    for (int i = 0, l = ns_array_length(vm->records); i < l; i++) {
        ns_record r = vm->records[i];
        if (r.type != NS_RECORD_FN) continue;
        if (r.lib.len > 0) continue;
        i32 arg_count = ns_array_length(r.fn.args);

        ns_bc_record fn_record = (ns_bc_record){.type = NS_BC_FN, .name = r.name};
        ns_bc_type_ref *args = (ns_bc_type_ref *)malloc(sizeof(ns_bc_type_ref) * arg_count);
        ns_array_set_length(fn_record.fn.args, arg_count);
        for (int j = 0; j < arg_count; j++) {
            ns_str arg_name = r.fn.args[j].name;
            ns_bc_type t = ns_bc_parse_type(bc_ctx, r.fn.args[j].val.type);
            args[j] = t.type;
            fn_record.fn.args[j] = (ns_bc_record){.type = NS_BC_VALUE, .name = arg_name, .val = { .type = t, .val = NULL, .p = -1 }};
        }
        ns_bc_type ret = ns_bc_parse_type(bc_ctx, r.fn.ret);
        ns_bc_type_ref fn_type = LLVMFunctionType(ret.type, args, arg_count, 0);
        ns_bc_value_ref fn = LLVMAddFunction(mod, ns_bc_str(r.name), fn_type);
        
        ns_bc_value fn_val = (ns_bc_value){.p = -1, .type = (ns_bc_type){.raw = {.i = i, .type = NS_TYPE_FN}, .type = fn_type}, .val = fn };
        fn_record.fn = (ns_bc_fn_record){.fn = fn_val, .args = NULL, .ret = ret};
        ns_bc_push_record(bc_ctx, fn_record);
        ns_bc_block entry = LLVMAppendBasicBlock(fn, "entry");
        LLVMPositionBuilderAtEnd(bdr, entry);

        ns_bc_call call = (ns_bc_call){.fn = &fn_record, .locals = NULL, .args = NULL, .ret = ns_bc_nil};
        ns_array_push(bc_ctx->call_stack, call);
        ns_bc_compound_stmt(bc_ctx, r.fn.ast);
        ns_array_pop(bc_ctx->call_stack);
    }
}

ns_bc_value ns_bc_binary_ops_number(ns_bc_ctx *bc_ctx, ns_bc_value l, ns_bc_value r, ns_token_t op) {
    ns_bc_builder bdr = bc_ctx->builder;
    bool f = ns_type_is_float(l.type.raw);
    bool s = ns_type_signed(l.type.raw);
    ns_bc_value ret = (ns_bc_value){.type = l.type};
    switch (op.type)
    {
    case NS_TOKEN_ADD_OP: {
        if (ns_str_equals_STR(op.val, "+"))
            if (f) { ret.val = LLVMBuildFAdd(bdr, l.val, r.val, ""); } else { ret.val = LLVMBuildAdd(bdr, l.val, r.val, ""); }
        else
            if (f) { ret.val = LLVMBuildFSub(bdr, l.val, r.val, ""); } else { ret.val = LLVMBuildSub(bdr, l.val, r.val, ""); }
    } break;
    case NS_TOKEN_MUL_OP: {
        if (ns_str_equals_STR(op.val, "*"))
            if (f) { ret.val = LLVMBuildFMul(bdr, l.val, r.val, ""); } else { ret.val = LLVMBuildMul(bdr, l.val, r.val, ""); }
        else if (ns_str_equals(op.val, ns_str_cstr("/")))
            if (f) { ret.val = LLVMBuildFDiv(bdr, l.val, r.val, ""); } else { ret.val = s ? LLVMBuildSDiv(bdr, l.val, r.val, "") : LLVMBuildUDiv(bdr, l.val, r.val, ""); }
        else
            if (f) { ret.val = LLVMBuildFRem(bdr, l.val, r.val, ""); } else { ret.val = s ? LLVMBuildSRem(bdr, l.val, r.val, "") : LLVMBuildURem(bdr, l.val, r.val, ""); }
    } break;
    case NS_TOKEN_SHIFT_OP:
        if (f) ns_error("bitcode error", "shift operator is not supported for float\n");
        if (ns_str_equals_STR(op.val, "<<")) {
            ret.val = LLVMBuildShl(bdr, l.val, r.val, "");
        } else {
            ret.val = s ? LLVMBuildAShr(bdr, l.val, r.val, "") : LLVMBuildLShr(bdr, l.val, r.val, "");
        }
        break;
    case NS_TOKEN_LOGIC_OP:
        if (f) return r;
        if (ns_str_equals_STR(op.val, "&&")) {
            ret.val = LLVMBuildAnd(bdr, l.val, r.val, "");
        } else {
            ret.val = LLVMBuildOr(bdr, l.val, r.val, "");
        }
        break;
    case NS_TOKEN_CMP_OP:{
        if (ns_str_equals_STR(op.val, "=="))
            if (f) ret.val = LLVMBuildFCmp(bdr, LLVMRealOEQ, l.val, r.val, "");
            else ret.val = LLVMBuildICmp(bdr, LLVMIntEQ, l.val, r.val, "");
        else if (ns_str_equals_STR(op.val, "!="))
            if (f) ret.val = LLVMBuildFCmp(bdr, LLVMRealONE, l.val, r.val, "");
            else ret.val = LLVMBuildICmp(bdr, LLVMIntNE, l.val, r.val, "");
        else if (ns_str_equals_STR(op.val, "<"))
            if (f) ret.val = LLVMBuildFCmp(bdr, LLVMRealOLT, l.val, r.val, "");
            else ret.val = LLVMBuildICmp(bdr, s ? LLVMIntSLT : LLVMIntULT, l.val, r.val, "");
        else if (ns_str_equals_STR(op.val, "<="))
            if (f) ret.val = LLVMBuildFCmp(bdr, LLVMRealOLE, l.val, r.val, "");
            else ret.val = LLVMBuildICmp(bdr, s ? LLVMIntSLE : LLVMIntULE, l.val, r.val, "");
        else if (ns_str_equals_STR(op.val, ">"))
            if (f) ret.val = LLVMBuildFCmp(bdr, LLVMRealOGT, l.val, r.val, "");
            else ret.val = LLVMBuildICmp(bdr, s ? LLVMIntSGT : LLVMIntUGT, l.val, r.val, "");
        else if (ns_str_equals_STR(op.val, ">="))
            if (f) ret.val = LLVMBuildFCmp(bdr, LLVMRealOGE, l.val, r.val, "");
            else ret.val = LLVMBuildICmp(bdr, s ? LLVMIntSGE : LLVMIntUGE, l.val, r.val, "");
        else ns_error("bitcode error", "unimplemented compare ops\n");
    } break;
    default:
        ns_error("bitcode error", "unimplemented binary ops\n");
        break;
    }
    return ns_bc_nil;
}

ns_bc_value ns_bc_binary_ops(ns_bc_ctx *bc_ctx, ns_bc_value l, ns_bc_value r, ns_token_t op) {
    if (ns_type_is_number(l.type.raw)) return ns_bc_binary_ops_number(bc_ctx, l, r, op);
    else {
        switch (l.type.raw.type)
        {
        case NS_TYPE_STRING:
            ns_error("bitcode error", "unimplemented string ops\n");
        case NS_TYPE_BOOL: {
            ns_bc_value_ref b = LLVMBuildAnd(bc_ctx->builder, l.val, r.val, "");
            return (ns_bc_value){ .type = ns_bc_type_bool, .val = b };
        } break;
        default:
            break;
        }
        ns_error("bitcode error", "unimplemented binary ops\n");
    }
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
    for (int i = 0; i < n.compound_stmt.count; i++) {
        int n_i = n.next;
        n = bc_ctx->ctx->nodes[n_i];
        switch (n.type) {
        case NS_AST_JUMP_STMT:
            ns_bc_jump_stmt(bc_ctx, n_i);
            break;
        default:
            break;
        }
    }
}

ns_bc_value ns_bc_call_expr(ns_bc_ctx *bc_ctx, int i) {
    ns_ast_ctx *ctx = bc_ctx->ctx;
    ns_ast_t n = ctx->nodes[i];
    ns_bc_builder bdr = bc_ctx->builder;

    ns_bc_value fn = ns_bc_expr(bc_ctx, n.call_expr.callee);
    if (fn.type.raw.type != NS_TYPE_FN || fn.p == -1) {
        ns_error("bitcode error", "invalid callee\n");
    }
    ns_bc_record fn_record = bc_ctx->records[fn.p];

    ns_ast_t arg = n;
    ns_bc_value_ref *args = NULL;
    ns_array_set_length(args, n.call_expr.arg_count);
    if (n.call_expr.arg_count != (i32)ns_array_length(fn_record.fn.args)) {
        ns_error("bitcode error", "argument count mismatched\n");
    }

    for (int i = 0; i < n.call_expr.arg_count; i++) {
        ns_bc_value v = ns_bc_expr(bc_ctx, arg.next);
        if (v.type.raw.type != fn_record.fn.args[i].val.type.raw.type) {
            ns_error("bitcode error", "invalid argument type\n");
        }
        args[i] = v.val;
        arg = ctx->nodes[arg.next];
    }

    ns_bc_value fn_val = fn_record.fn.fn;
    ns_bc_value ret = (ns_bc_value){.p = -1, .val = NULL, .type = fn_record.fn.ret };
    ret.val = LLVMBuildCall2(bdr, fn_val.type.type, fn_val.val, args, n.call_expr.arg_count, "");
    return ret;
}

void ns_bc_std(ns_bc_ctx *bc_ctx) {
    ns_bc_module mod = bc_ctx->mod;

    // register printf
    ns_bc_type_ref print_args[] = { LLVMPointerType(LLVMInt8Type(), 0) }; // char* type
    ns_bc_type_ref print_type = LLVMFunctionType(LLVMInt32Type(), print_args, 1, 1); // variadic function
    ns_bc_value_ref print_fn = LLVMAddFunction(mod, "printf", print_type);
    ns_bc_type print_ret_type = (ns_bc_type){.type = LLVMInt32Type(), .raw = ns_type_i32};
    ns_bc_record print_record = {.type = NS_BC_FN, .name = ns_str_cstr("print") };
    ns_bc_value print_val = (ns_bc_value){.p = 0, .type = (ns_bc_type){.raw = (ns_type){.type = NS_TYPE_FN, .i = 0 }, .type = print_type }, .val = print_fn};
    print_record.fn = (ns_bc_fn_record){ .fn = print_val, .ret = print_ret_type, .args = NULL };
    ns_array_set_length(print_record.fn.args, 1);
    print_record.fn.args[0] = (ns_bc_record){.type = NS_BC_VALUE, .val = {.type = ns_bc_type_str, .val = NULL, .p = -1} };
    ns_bc_push_record(bc_ctx, print_record);
}

void ns_bc_call_std(ns_bc_ctx *bc_ctx) {
    
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
    ns_bc_fn_def(&bc_ctx, vm, ctx);
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
            ns_bc_call_expr(&bc_ctx, ctx->sections[i]);
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