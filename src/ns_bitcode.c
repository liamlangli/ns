#ifdef NS_BITCODE

#include "ns_bitcode.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_fmt.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

#define NS_BC_CACHE ".build/cache"

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

#define ns_bc_type_unknown ((ns_bc_type){.type = ns_null, .raw = ns_type_unknown})
#define ns_bc_type_infer ((ns_bc_type){.type = ns_null, .raw = ns_type_infer})
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

#define ns_bc_nil ((ns_bc_value){.val = ns_null, .type = ns_bc_type_nil, .p = -1})
#define ns_bc_true ((ns_bc_value){.val = LLVM, .type = ns_bc_type_bool, .p = -1})
#define ns_bc_false ((ns_bc_value){.val = ns_null, .type = ns_bc_type_bool, .p = -1})

typedef struct ns_bc_symbol ns_bc_symbol;

typedef struct ns_bc_fn_symbol {
    ns_bc_symbol *args;
    ns_bc_type ret;
    ns_bc_value fn;
} ns_bc_fn_symbol;

typedef struct ns_bc_st_symbol {
    ns_bc_value st;
    ns_bc_symbol *fields;
} ns_bc_st_symbol;

typedef enum ns_bc_symbol_type {
    NS_BC_INVALID,
    NS_BC_VALUE,
    NS_BC_FN,
    NS_BC_STRUCT
} ns_bc_symbol_type;

typedef struct ns_bc_symbol {
    ns_bc_symbol_type type;
    ns_str name;
    i32 index;
    union {
        ns_bc_value val;
        ns_bc_fn_symbol fn;
        ns_bc_st_symbol st;
    };
}  ns_bc_symbol;

#define ns_bc_nil_symbol ((ns_bc_symbol){.type = NS_BC_INVALID})

typedef struct ns_bc_call {
    ns_bc_symbol *fn;
    i32 arg_offset, arg_count;
    ns_bc_value ret;
    u32 scope_top;
} ns_bc_call;

typedef struct ns_bc_ctx {
    ns_bc_module mod;
    ns_bc_builder builder;

    ns_bc_symbol *symbols;

    ns_ast_ctx *ctx;
    ns_vm *vm;

    ns_bc_call *call_stack;
    ns_bc_symbol *symbol_stack;
} ns_bc_ctx;

// util
const char* ns_bc_str(ns_str s);
ns_bc_type ns_bc_parse_type(ns_bc_ctx *bc_ctx, ns_type t);
ns_bc_value ns_bc_find_value(ns_bc_ctx *bc_ctx, ns_str name);
void ns_bc_set_symbol(ns_bc_ctx *bc_ctx, ns_bc_symbol r, i32 i);

// expr
ns_bc_value ns_bc_expr(ns_bc_ctx *bc_ctx, i32 i);
ns_bc_value ns_bc_call_expr(ns_bc_ctx *bc_ctx, i32 i);
ns_bc_value ns_bc_primary_expr(ns_bc_ctx *bc_ctx, i32 i);
ns_bc_value ns_bc_binary_expr(ns_bc_ctx *bc_ctx, i32 i);
ns_bc_value ns_bc_call_std(ns_bc_ctx *bc_ctx);

// stmt
void ns_bc_jump_stmt(ns_bc_ctx *bc_ctx, i32 i);
void ns_bc_compound_stmt(ns_bc_ctx *bc_ctx, i32 i);

// def
void ns_bc_parse_def(ns_bc_ctx *bc_ctx);


// impl
#define MAX_STR_LENGTH 512
static i8 _str_buff[MAX_STR_LENGTH];
const char* ns_bc_str(ns_str s) {
    if (s.len >= MAX_STR_LENGTH) {
        assert(false);
    }
    ns_str ret = {.data = _str_buff, .len = 0};
    for (i32 i = 0; i < s.len; i++) {
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
        return bc_ctx->symbols[t.index].fn.ret;
    default:
        ns_error("ns_bc", "unimplemented type\n");
        break;
    }
    return ns_bc_type_nil;
}

void ns_bc_set_symbol(ns_bc_ctx *bc_ctx, ns_bc_symbol r, i32 i) {
    switch (r.type)
    {
    case NS_BC_FN: r.fn.fn.p = i; break;
    case NS_BC_VALUE: r.val.p = i; break;
    default: break;
    }
    r.index = i;
    bc_ctx->symbols[i] = r;
}

ns_bc_value ns_bc_find_value(ns_bc_ctx *bc_ctx, ns_str name) {
    ns_vm *vm = bc_ctx->vm;
    size_t l = ns_array_length(vm->call_stack);
    if (l > 0) {
        ns_bc_call *call = &vm->call_stack[l - 1];
        i32 scope_top = call->scope_top;
        i32 symbol_top = vm->scope_stack[scope_top].symbol_top;
        i32 symbol_count = ns_array_length(vm->symbol_stack);

        for (i32 j = symbol_count - 1; j >= symbol_top; --j) {
            if (ns_str_equals(bc_ctx->symbol_stack[j].name, name)) {
                return bc_ctx->symbol_stack[j].val;
            }
        }
    }

    for (i32 i = 0, l = ns_array_length(bc_ctx->symbols); i < l; i++) {
        if (ns_str_equals(bc_ctx->symbols[i].name, name)) {
            ns_bc_symbol *r = &bc_ctx->symbols[i];
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

ns_bc_value ns_bc_primary_expr(ns_bc_ctx *bc_ctx, i32 i) {
    ns_ast_t *n = &bc_ctx->ctx->nodes[i];
    ns_token_t t = n->primary_expr.token;
    switch (n->primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return (ns_bc_value){.val = LLVMConstInt(LLVMInt32Type(), ns_str_to_i32(t.val), 0), .type = ns_bc_type_i32};
    case NS_TOKEN_FLT_LITERAL:
        return (ns_bc_value){.val = LLVMConstReal(LLVMDoubleType(), ns_str_to_f64(t.val)), .type = ns_bc_type_f64};
    case NS_TOKEN_STR_LITERAL: {
        i32 p = ns_array_length(bc_ctx->vm->str_list);
        ns_array_push(bc_ctx->vm->str_list, t.val);
        return (ns_bc_value){.val = LLVMBuildGlobalStringPtr(bc_ctx->builder, ns_str_unescape(n->primary_expr.token.val).data, ""), .type = ns_bc_type_str, .p = p};
    } break;
    case NS_TOKEN_TRUE:
        return (ns_bc_value){.val = LLVMConstInt(LLVMInt32Type(), 1, 0), .type = ns_bc_type_bool};
    case NS_TOKEN_FALSE:
        return (ns_bc_value){.val = LLVMConstInt(LLVMInt32Type(), 0, 0), .type = ns_bc_type_bool};
    case NS_TOKEN_IDENTIFIER:
        return ns_bc_find_value(bc_ctx, n->primary_expr.token.val);
    default:
        break;
    }
    return ns_bc_nil;
}

ns_bc_value ns_bc_expr(ns_bc_ctx *bc_ctx, i32 i) {
    ns_ast_t *n = &bc_ctx->ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_bc_expr(bc_ctx, n->expr.body);
    case NS_AST_BINARY_EXPR: return ns_bc_binary_expr(bc_ctx, i);
    case NS_AST_PRIMARY_EXPR: return ns_bc_primary_expr(bc_ctx, i);
    case NS_AST_CALL_EXPR: return ns_bc_call_expr(bc_ctx, i);
    default:
        break;
    }
    return ns_bc_nil;
}

void ns_bc_struct_def(ns_bc_ctx *bc_ctx, ns_symbol *st, i32 i) {
    ns_vm *vm = bc_ctx->vm;
    ns_bc_module mod = bc_ctx->mod;
    ns_bc_builder bdr = bc_ctx->builder;

    ns_bc_symbol bc_st = (ns_bc_symbol){.type = NS_BC_STRUCT, .name = st->name};
    ns_bc_st_symbol st_symbol = {.st = ns_bc_nil, .fields = ns_null};
    ns_array_set_length(st_symbol.fields, ns_array_length(st->st.fields));
    for (i32 j = 0, l = ns_array_length(st->st.fields); j < l; j++) {
        ns_symbol *field = &st->st.fields[j];
        ns_bc_type t = ns_bc_parse_type(bc_ctx, field->val.t);
        st_symbol.fields[j] = (ns_bc_symbol){.type = NS_BC_VALUE, .name = field->name, .val = (ns_bc_value){.type = t, .val = ns_null, .p = j}};
    }
    ns_bc_type_ref st_type = LLVMStructType(st_symbol.fields, ns_array_length(st_symbol.fields), 0);
    st_symbol.st = (ns_bc_value){.val = ns_null, .type = (ns_bc_type){.raw = st->st.st.t, .type = st_type}, .p = i};
    bc_st.st = st_symbol;
    ns_bc_set_symbol(bc_ctx, bc_st, i);
}

ns_return_void ns_bc_fn_def(ns_bc_ctx *bc_ctx, ns_symbol *fn, i32 i) {
    ns_bc_module mod = bc_ctx->mod;
    ns_bc_builder bdr = bc_ctx->builder;

    i32 arg_count = ns_array_length(fn->fn.args);
    ns_bc_symbol bc_fn = (ns_bc_symbol){.type = NS_BC_FN, .name = fn->name};
    // parse argument types
    ns_bc_type_ref *args = (ns_bc_type_ref *)malloc(sizeof(ns_bc_type_ref) * arg_count);
    ns_array_set_length(bc_fn.fn.args, arg_count);
    for (i32 j = 0; j < arg_count; j++) {
        ns_str arg_name = fn->fn.args[j].name;
        ns_bc_type t = ns_bc_parse_type(bc_ctx, fn->fn.args[j].val.t);
        if (t.raw.type == NS_TYPE_UNKNOWN) {
            ns_error("ns_bc", "unknown type\n");
        }

        args[j] = t.type;
        bc_fn.fn.args[j] = (ns_bc_symbol){.type = NS_BC_VALUE, .name = arg_name, .val = { .type = t, .val = ns_null, .p = -1 }};
    }

    // make and save fn symbol
    ns_bc_type ret = ns_bc_parse_type(bc_ctx, fn->fn.ret);
    ns_bc_type_ref bc_fn_type = LLVMFunctionType(ret.type, args, arg_count, 0);
    ns_bc_value_ref bc_fn_val = LLVMAddFunction(mod, ns_bc_str(fn->name), bc_fn_type);
    ns_bc_value fn_val = (ns_bc_value){.p = -1, .type = (ns_bc_type){.raw = fn->fn.fn.t, .type = bc_fn_type}, .val = bc_fn_val };
    bc_fn.fn.fn = fn_val;
    bc_fn.fn.ret = ret;
    ns_bc_set_symbol(bc_ctx, bc_fn, i);
    return ns_return_ok_void;
}

ns_bc_value ns_bc_fn_body(ns_bc_ctx *bc_ctx, ns_symbol *fn, i32 i) {
    ns_bc_module mod = bc_ctx->mod;
    ns_bc_builder bdr = bc_ctx->builder;
    if (fn->lib.len != 0) return ns_bc_nil; // skip lib functions

    ns_bc_value ret = ns_bc_nil;
    ns_bc_symbol *bc_fn = &bc_ctx->symbols[i];
    ns_bc_block entry = LLVMAppendBasicBlock(bc_fn->fn.fn.val, "entry");
    LLVMPositionBuilderAtEnd(bdr, entry);

    // load argument values
    ns_bc_call call = (ns_bc_call){
        .fn = fn,
        .ret = ns_bc_nil,
        .scope_top = ns_array_length(bc_ctx->vm->scope_stack),
        .arg_offset = ns_array_length(bc_ctx->symbol_stack), .arg_count = ns_array_length(fn->fn.args)};

    ns_array_push(bc_ctx->call_stack, call);
    ns_ast_t *fn_ast = &bc_ctx->ctx->nodes[fn->fn.ast];
    ns_bc_compound_stmt(bc_ctx, fn_ast->fn_def.body);
    ns_array_pop(bc_ctx->call_stack);

    return ret;
}

ns_bc_value ns_bc_binary_ops_number(ns_bc_ctx *bc_ctx, ns_bc_value l, ns_bc_value r, ns_token_t op) {
    ns_bc_builder bdr = bc_ctx->builder;
   ns_bool f = ns_type_is_float(l.type.raw);
   ns_bool s = ns_type_signed(l.type.raw);
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
        if (f) ns_error("ns_bc", "shift operator is not supported for float and double.\n");
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
        else ns_error("ns_bc", "unimplemented compare ops\n");
    } break;
    default:
        ns_error("ns_bc", "unimplemented binary ops\n");
        break;
    }
    return ret;
}

ns_bc_value ns_bc_binary_ops(ns_bc_ctx *bc_ctx, ns_bc_value l, ns_bc_value r, ns_token_t op) {
    if (ns_type_is_number(l.type.raw)) {
        return ns_bc_binary_ops_number(bc_ctx, l, r, op);
    } else {
        switch (l.type.raw.type)
        {
        case NS_TYPE_STRING:
            ns_error("ns_bc", "unimplemented string ops\n");
            break;
        default:
            break;
        }
        ns_error("ns_bc", "unimplemented binary ops\n");
    }
    return ns_bc_nil;
}

ns_bc_value ns_bc_binary_expr(ns_bc_ctx *bc_ctx, i32 i) {
    ns_bc_builder bdr = bc_ctx->builder;
    ns_ast_t *n = &bc_ctx->ctx->nodes[i];

    ns_bc_value l = ns_bc_expr(bc_ctx, n->binary_expr.left);
    ns_bc_value r = ns_bc_expr(bc_ctx, n->binary_expr.right);
    if (ns_type_equals(l.type.raw, r.type.raw)) {
        return ns_bc_binary_ops(bc_ctx, l, r, n->binary_expr.op);
    }

    ns_bc_value ret = (ns_bc_value){.val = ns_null, .type = l.type, .p = -1};
    switch (n->binary_expr.op.type) {
    case NS_TOKEN_ADD_OP:
        if (ns_str_equals_STR(n->binary_expr.op.val, "+")) {
            ret.val = LLVMBuildFAdd(bdr, l.val, r.val, "");
        } else if (ns_str_equals_STR(n->binary_expr.op.val, "-")) {
            ret.val = LLVMBuildFSub(bdr, l.val, r.val, "");
        }
        break;
    case NS_TOKEN_MUL_OP:
        if (ns_str_equals_STR(n->binary_expr.op.val, "*")) {
            ret.val = LLVMBuildFMul(bdr, l.val, r.val, "");
        } else if (ns_str_equals_STR(n->binary_expr.op.val, "/")) {
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

void ns_bc_jump_stmt(ns_bc_ctx *bc_ctx, i32 i) {
    ns_bc_builder bdr = bc_ctx->builder;
    ns_ast_t *n = &bc_ctx->ctx->nodes[i];

    ns_token_t t = n->jump_stmt.label;
    if (ns_str_equals_STR(t.val, "return")) {
        if (n->jump_stmt.expr) {
            ns_bc_value ret = ns_bc_expr(bc_ctx, n->jump_stmt.expr);
            LLVMBuildRet(bdr, ret.val);
        } else {
            LLVMBuildRetVoid(bdr);
        }
    }
}

void ns_bc_compound_stmt(ns_bc_ctx *bc_ctx, i32 i) {
    ns_ast_t *n = &bc_ctx->ctx->nodes[i];
    i32 next;
    i32 l = n->compound_stmt.count;
    for (i32 i = 0; i < l; i++) {
        next = n->next;
        switch (n->type) {
        case NS_AST_JUMP_STMT:
            ns_bc_jump_stmt(bc_ctx, next);
            break;
        default:
            break;
        }
        n = &bc_ctx->ctx->nodes[next];
    }
}

ns_bc_value ns_bc_call_expr(ns_bc_ctx *bc_ctx, i32 i) {
    ns_ast_ctx *ctx = bc_ctx->ctx;
    ns_bc_builder bdr = bc_ctx->builder;
    ns_ast_t *n = &ctx->nodes[i];

    ns_bc_value fn = ns_bc_expr(bc_ctx, n->call_expr.callee);
    ns_bc_symbol *fn_symbol = &bc_ctx->symbols[fn.p];

    if (n->call_expr.arg_count != (i32)ns_array_length(fn_symbol->fn.args)) {
        ns_error("ns_bc", "argument count mismatched\n");
    }

    i32 next = n->call_expr.arg;
    for (i32 i = 0; i < n->call_expr.arg_count; i++) {
        // ns_ast_t arg = ctx->nodes[next];
        // call->args[i] = ns_bc_expr(bc_ctx, next);
        // next = arg.next;
        // if (call.args[i].type.type != fn_symbol->fn.args[i].val.type.type) {
        //     ns_error("ns_bc", "invalid argument type\n");
        // }
    }

    ns_bc_value ret = (ns_bc_value){.val = ns_null, .type = fn_symbol->fn.ret};
    // if (ns_str_equals_STR(bc_ctx->vm->symbols[fn_symbol->index].lib, "std")) {
    //     ret.val = ns_bc_call_std(bc_ctx).val;
    // } else {
    //     ns_bc_value fn_val = fn_symbol->fn.fn;
    //     ns_bc_value_ref *args = (ns_bc_value_ref *)malloc(sizeof(ns_bc_value_ref) * n.call_expr.arg_count);
    //     for (i32 i = 0; i < n.call_expr.arg_count; i++) {
    //         args[i] = call.args[i].val;
    //     }
    //     ret.val = LLVMBuildCall2(bdr, fn_val.type.type, fn_val.val, args, n.call_expr.arg_count, "");
    // }
    // ns_array_pop(bc_ctx->call_stack);
    return ret;
}

ns_bc_value ns_bc_var_def(ns_bc_ctx *bc_ctx, i32 i) {
    ns_ast_t *n = &bc_ctx->ctx->nodes[i];
    ns_bc_value val = ns_bc_expr(bc_ctx, n->var_def.expr);
    ns_str name = n->var_def.name.val;
    ns_bc_symbol val_symbol = (ns_bc_symbol){.type = NS_BC_VALUE, .name = name, .val = val};
    ns_bc_set_symbol(bc_ctx, val_symbol, 0);
    return bc_ctx->symbols[0].val;
}

ns_bc_value ns_bc_call_std_print(ns_bc_ctx *bc_ctx) {
    ns_bc_builder bdr = bc_ctx->builder;
    ns_bc_call *call = &bc_ctx->call_stack[ns_array_length(bc_ctx->call_stack) - 1];

    ns_bc_value_ref *args = ns_null;
    ns_array_push(args, ns_null); // fmt_str placeholder

    // ns_str fmt = bc_ctx->vm->str_list[call->args[0].p];
    ns_str fmt = ns_str_cstr("print: {0}");

    ns_str ptn = {.data = ns_null, .len = 0, .dynamic = 1};
    ns_array_set_capacity(ptn.data, fmt.len);
    ns_ast_ctx *raw_ctx = bc_ctx->ctx;

    i32 i = 0;
    while (i < fmt.len) {
        if (fmt.data[i] == '{' && (i == 0 || (i > 0 && fmt.data[i - 1] != '\\'))) {
            i32 start = ++i;
            while (i < fmt.len && fmt.data[i] != '}') {
                i++;
            }
            ns_ast_ctx ctx = {0};
            ns_str expr = ns_str_slice(fmt, start, i++);
            ctx.source = expr;
            ctx.filename = ns_str_cstr("fmt");

            ctx.token.line = 1; // start from 1
            ctx.current = -1;
            bc_ctx->ctx = &ctx;

            ns_parse_expr(&ctx);
            ns_bc_value v = ns_bc_expr(bc_ctx, ctx.current);
            ns_str s = ns_fmt_type_str(v.type.raw);
            ns_str_append(&ptn, s);
            ns_array_push(args, v.val);
        } else {
            ns_array_push(ptn.data, fmt.data[i++]);
        }
    }
    ptn.len = ns_array_length(ptn.data);
    ns_str unescaped = ns_str_unescape(ptn);

    ns_bc_value_ref fmt_str = LLVMBuildGlobalStringPtr(bdr, unescaped.data, "");
    args[0] = fmt_str;

    ns_bc_value fn = call->fn->fn.fn;
    LLVMBuildCall2(bdr, fn.type.type, fn.val, args, ns_array_length(args), "");
    bc_ctx->ctx = raw_ctx;

    return ns_bc_nil;
}

ns_bc_value ns_bc_call_std(ns_bc_ctx *bc_ctx) {
    ns_bc_call* call = &bc_ctx->call_stack[ns_array_length(bc_ctx->call_stack) - 1];
    ns_str fn_name = call->fn->name;
    if (ns_str_equals_STR(fn_name, "print")) {
        return ns_bc_call_std_print(bc_ctx);
    }
    return ns_bc_nil;
}

void ns_bc_parse_def(ns_bc_ctx *bc_ctx) {
    ns_vm *vm = bc_ctx->vm;
    // parse struct and fn
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; i++) {
        ns_symbol *s = &vm->symbols[i];
        switch (s->type)
        {
        case NS_SYMBOL_FN:
            ns_bc_fn_def(bc_ctx, s, i);
            break;
        case NS_SYMBOL_STRUCT:
            ns_bc_struct_def(bc_ctx, s, i);
            break;
        default:
            break;
        }
    }

    ns_symbol* main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main"));
    if (!main_fn) {

    }
}

ns_return_bool ns_bc_gen(ns_vm *vm, ns_str input, ns_str output) {
    ns_ast_ctx ctx = {0};

    ns_str source = ns_fs_read_file(input);
    if (source.len == 0) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_BITCODE, "empty file.\n");

    if (output.len == 0) {
        ns_str path = ns_path_filename(input);
        path = ns_str_concat(path, ns_str_cstr(".bc"));
        output = ns_path_join(ns_str_cstr(NS_BC_CACHE), path);
    }
    ctx.source = source;

    ns_return_bool ret = ns_ast_parse(&ctx, source, input);
    if (ns_return_is_error(ret)) return ret;

    ret = ns_vm_parse(vm, &ctx);
    if (ns_return_is_error(ret)) return ret;

    ns_bc_module mod = LLVMModuleCreateWithName(input.data);
    ns_bc_builder bdr = LLVMCreateBuilder();
    ns_bc_ctx bc_ctx = {0};
    bc_ctx.ctx = &ctx;
    bc_ctx.vm = vm;
    bc_ctx.mod =  mod;
    bc_ctx.builder = bdr;
    ns_array_set_length(bc_ctx.symbols, ns_array_length(vm->symbols));

    ns_bc_parse_def(&bc_ctx);

    ns_bc_type main_fn_type = (ns_bc_type){.type = ns_null, .raw = ns_type_encode(ns_type_fn, 0, true, 0)};
    main_fn_type.type = LLVMFunctionType(LLVMInt32Type(), ns_null, 0, 0);
    ns_bc_value main_fn = (ns_bc_value){.p = -1, .val = ns_null, .type = main_fn_type};
    main_fn.val = LLVMAddFunction(mod, "main", main_fn_type.type);
    ns_bc_block entry_main = LLVMAppendBasicBlock(main_fn.val, "entry");
    LLVMPositionBuilderAtEnd(bc_ctx.builder, entry_main);

    // parse deferred nodes as main fn body
    for (i32 i = ctx.section_begin; i < ctx.section_end; ++i) {
        i32 s = ctx.sections[i];
        ns_ast_t *n = &ctx.nodes[s];
        switch (n->type) {
        case NS_AST_VAR_DEF: ns_bc_var_def(&bc_ctx, s); break;
        case NS_AST_CALL_EXPR: ns_bc_call_expr(&bc_ctx, s); break;
        default:
            break;
        }
    }

    LLVMBuildRet(bdr, LLVMConstInt(LLVMInt32Type(), 0, 0));

    i8* error = ns_null;
    LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    // Write out bitcode to file
    if (LLVMWriteBitcodeToFile(mod, output.data) != 0) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_BITCODE, "fail writing bitcode to file.");

    LLVMDisposeBuilder(bdr);
    LLVMDisposeModule(mod);
    return ns_return_ok(bool, true);
}

#endif // NS_BITCODE