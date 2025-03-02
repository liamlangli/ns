#ifdef NS_IR

#include "ns_ir.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_os.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>

#define ns_ir_cache ".build/cache"

// types
#define ns_ir_module LLVMModuleRef
#define ns_ir_builder LLVMBuilderRef
#define ns_ir_block LLVMBasicBlockRef
#define ns_ir_type_ref LLVMTypeRef
#define ns_ir_value_ref LLVMValueRef

typedef struct ns_ir_type {
    ns_type raw;
    ns_ir_type_ref type;
} ns_ir_type;

#define ns_ir_type_unknown ((ns_ir_type){.type = ns_null, .raw = ns_type_unknown})
#define ns_ir_type_infer ((ns_ir_type){.type = ns_null, .raw = ns_type_infer})
#define ns_ir_type_nil ((ns_ir_type){.type = LLVMVoidType(), .raw = ns_type_nil})
#define ns_ir_type_bool ((ns_ir_type){.type = LLVMInt32Type(), .raw = ns_type_bool})
#define ns_ir_type_str ((ns_ir_type){.type = LLVMPointerType(LLVMInt8Type(), 0), .raw = ns_type_str})
#define ns_ir_type_i64 ((ns_ir_type){.type = LLVMInt64Type(), .raw = ns_type_i64})
#define ns_ir_type_i8 ((ns_ir_type){.type = LLVMInt8Type(), .raw = ns_type_i8})
#define ns_ir_type_i16 ((ns_ir_type){.type = LLVMInt16Type(), .raw = ns_type_i16})
#define ns_ir_type_i32 ((ns_ir_type){.type = LLVMInt32Type(), .raw = ns_type_i32})
#define ns_ir_type_u8 ns_ir_type_i8
#define ns_ir_type_u16 ns_ir_type_i16
#define ns_ir_type_u32 ns_ir_type_i32
#define ns_ir_type_u64 ns_ir_type_i64
#define ns_ir_type_f32 ((ns_ir_type){.type = LLVMFloatType(), .raw = ns_type_f32})
#define ns_ir_type_f64 ((ns_ir_type){.type = LLVMDoubleType(), .raw = ns_type_f64})
#define ns_ir_type_is_float(t) ((t).raw.type == NS_TYPE_F64 || (t).raw.type == NS_TYPE_F64)

typedef struct ns_ir_value {
    ns_ir_type type;
    ns_ir_value_ref val;
    i32 p;
} ns_ir_value;

#define ns_ir_nil ((ns_ir_value){.val = ns_null, .type = ns_ir_type_nil, .p = -1})
#define ns_ir_true ((ns_ir_value){.val = LLVM, .type = ns_ir_type_bool, .p = -1})
#define ns_ir_false ((ns_ir_value){.val = ns_null, .type = ns_ir_type_bool, .p = -1})

typedef struct ns_ir_symbol ns_ir_symbol;

typedef struct ns_ir_fn_symbol {
    ns_ir_type ret;
    ns_ir_value fn;
} ns_ir_fn_symbol;

typedef struct ns_ir_st_symbol {
    ns_ir_value st;
    ns_ir_symbol *fields;
} ns_ir_st_symbol;

typedef enum ns_ir_symbol_type {
    NS_IR_INVALID,
    NS_IR_VALUE,
    NS_IR_FN,
    NS_IR_STRUCT
} ns_ir_symbol_type;

typedef struct ns_ir_symbol {
    ns_ir_symbol_type type;
    ns_str name;
    ns_str lib;
    i32 index;
    union {
        ns_ir_value val;
        ns_ir_fn_symbol fn;
        ns_ir_st_symbol st;
    };
}  ns_ir_symbol;

#define ns_ir_nil_symbol ((ns_ir_symbol){.type = NS_IR_INVALID})

typedef struct ns_ir_call {
    ns_ir_symbol *fn;
    i32 arg_offset, arg_count;
    u32 scope_top;
} ns_ir_call;

typedef struct ns_ir_ctx {
    ns_ir_module mod;
    ns_ir_builder bdr;

    ns_vm *vm;
    i32 fn;
    ns_ir_symbol *symbols;
    ns_ir_symbol *symbol_stack;
} ns_ir_ctx;

// expr
ns_ir_value ns_ir_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i);
void ns_ir_compound_stmt(ns_ir_ctx *ir_ctx, ns_ast_ctx* ctx, i32 i);

// impl
#define MAX_STR_LENGTH 512
static i8 _str_buff[MAX_STR_LENGTH];
const char* ns_ir_str(ns_str s) {
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
#define ns_ir_MAX_ARGS 32
static ns_ir_value_ref _args[ns_ir_MAX_ARGS];

ns_ir_type ns_ir_parse_type(ns_ir_ctx *ir_ctx, ns_type t) {
    switch (t.type) {
    case NS_TYPE_I8:
    case NS_TYPE_U8: return ns_ir_type_i8;
    case NS_TYPE_I16:
    case NS_TYPE_U16: return ns_ir_type_i16;
    case NS_TYPE_BOOL:
    case NS_TYPE_I32:
    case NS_TYPE_U32: return ns_ir_type_i32;
    case NS_TYPE_I64:
    case NS_TYPE_U64: return ns_ir_type_i64;
    case NS_TYPE_F32: return ns_ir_type_f32;
    case NS_TYPE_F64: return ns_ir_type_f64;
    case NS_TYPE_STRING: return ns_ir_type_str;
    case NS_TYPE_INFER:
    case NS_TYPE_NIL: return ns_ir_type_nil;
    case NS_TYPE_FN: return ir_ctx->symbols[t.index].fn.fn.type;
    case NS_TYPE_STRUCT: return ir_ctx->symbols[t.index].st.st.type;
    default:
        ns_error("ns_bc", "unimplemented type\n");
        break;
    }
    return ns_ir_type_nil;
}

void ns_ir_set_symbol(ns_ir_ctx *ir_ctx, ns_ir_symbol r, i32 i) {
    switch (r.type)
    {
    case NS_IR_FN: r.fn.fn.p = i; break;
    case NS_IR_VALUE: r.val.p = i; break;
    default: break;
    }
    r.index = i;
    ir_ctx->symbols[i] = r;
}

ns_ir_symbol* ns_ir_find_symbol(ns_ir_ctx *ir_ctx, ns_str name) {
    i32 symbol_count = ns_array_length(ir_ctx->symbol_stack);
    for (i32 j = symbol_count - 1; j >= 0; --j) {
        if (ns_str_equals(ir_ctx->symbol_stack[j].name, name)) {
            return &ir_ctx->symbol_stack[j];
        }
    }

    for (i32 i = 0, l = ns_array_length(ir_ctx->symbols); i < l; i++) {
        if (ns_str_equals(ir_ctx->symbols[i].name, name)) {
            return &ir_ctx->symbols[i];
        }
    }
    return ns_null;
}

ns_ir_value ns_ir_std_printf(ns_ir_ctx *ir_ctx) {
    static ns_bool _ir_printf_init = false;
    static ns_ir_value _ir_printf;
    if (_ir_printf_init) return _ir_printf;

    ns_ir_type_ref _types[1] = {LLVMPointerType(LLVMInt8Type(), 0)};
    _ir_printf.type.type = LLVMFunctionType(LLVMInt32Type(), _types, 1, 1);
    _ir_printf.val = LLVMAddFunction(ir_ctx->mod, "printf", _ir_printf.type.type);
    _ir_printf_init = true;
    return _ir_printf;
}

ns_ir_value ns_ir_std_sqrt(ns_ir_ctx *ir_ctx) {
    static ns_bool _ir_sqrt_init = false;
    static ns_ir_value _ir_sqrt;
    if (_ir_sqrt_init) return _ir_sqrt;

    ns_ir_type_ref sqrtArgs[] = {LLVMDoubleType()};
    LLVMTypeRef sqrtFuncType = LLVMFunctionType(LLVMDoubleType(), sqrtArgs, 1, 0);
    LLVMValueRef sqrtFunc = LLVMAddFunction(ir_ctx->mod, "llvm.sqrt.f64", sqrtFuncType);
    _ir_sqrt = (ns_ir_value){.val = sqrtFunc, .type = (ns_ir_type){.type = sqrtFuncType, .raw = ns_type_f64}};
    _ir_sqrt_init = true;
    return _ir_sqrt;
}

ns_ir_value ns_ir_call_std(ns_ir_ctx *ir_ctx, ns_ast_ctx* ctx, ns_symbol *fn) {
    ns_unused(ctx);

    ns_ir_value ret = ns_ir_nil;
    if (ns_str_equals_STR(fn->name, "print")) {
        ns_ir_value ir_printf = ns_ir_std_printf(ir_ctx);
        LLVMBuildCall2(ir_ctx->bdr, ir_printf.type.type, ir_printf.val, _args, 1, "");
    } else if (ns_str_equals_STR(fn->name, "sqrt")) {
        ns_ir_value ir_sqrt = ns_ir_std_sqrt(ir_ctx);
        ret.val = LLVMBuildCall2(ir_ctx->bdr, ir_sqrt.type.type, ir_sqrt.val, _args, 1, "");
        ret.type = ir_sqrt.type;
    } else {
        ns_error("ns_bc", "unimplemented std function.\n");
    }
    return ret;
}

ns_ir_value ns_ir_std_malloc(ns_ir_ctx *ir_ctx) {
    static ns_bool _ir_malloc_init = false;
    static ns_ir_value _ir_malloc;
    if (_ir_malloc_init) return _ir_malloc;

    ns_ir_type_ref _types[1] = {LLVMInt64Type()};
    _ir_malloc.type.type = LLVMFunctionType(LLVMPointerType(LLVMInt8Type(), 0), _types, 1, 0);
    _ir_malloc.val = LLVMAddFunction(ir_ctx->mod, "malloc", _ir_malloc.type.type);
    _ir_malloc_init = true;
    return _ir_malloc;
}

ns_ir_value ns_ir_std_free(ns_ir_ctx *ir_ctx) {
    static ns_bool _ir_free_init = false;
    static ns_ir_value _ir_free;
    if (_ir_free_init) return _ir_free;

    ns_ir_type_ref _types[1] = {LLVMPointerType(LLVMInt8Type(), 0)};
    _ir_free.type.type = LLVMFunctionType(LLVMVoidType(), _types, 1, 0);
    _ir_free.val = LLVMAddFunction(ir_ctx->mod, "free", _ir_free.type.type);
    _ir_free_init = true;
    return _ir_free;
}

ns_ir_value ns_ir_std_snprinf(ns_ir_ctx *ir_ctx) {
    static ns_bool _ir_snprinf_init = false;
    static ns_ir_value _ir_snprinf;
    if (_ir_snprinf_init) return _ir_snprinf;

    ns_ir_type_ref _types[3] = {LLVMPointerType(LLVMInt8Type(), 0), LLVMInt64Type(), LLVMPointerType(LLVMInt8Type(), 0)};
    _ir_snprinf.type.type = LLVMFunctionType(LLVMInt32Type(), _types, 3, 1);
    _ir_snprinf.val = LLVMAddFunction(ir_ctx->mod, "snprintf", _ir_snprinf.type.type);
    _ir_snprinf_init = true;
    return _ir_snprinf;
}

ns_ir_value ns_ir_find_value(ns_ir_ctx *ir_ctx, ns_str name) {
    ns_ir_symbol *r = ns_ir_find_symbol(ir_ctx, name);
    if (r) {
        switch (r->type) {
        case NS_IR_VALUE: return r->val;
        case NS_IR_FN: return r->fn.fn;
        case NS_IR_STRUCT: return r->st.st;
        default: break;
        }
    }
    return ns_ir_nil;
}

ns_scope *ns_ir_enter_scope(ns_ir_ctx *ir_ctx) {
    ns_scope scope = (ns_scope){.symbol_top = ns_array_length(ir_ctx->symbol_stack)};
    ns_array_push(ir_ctx->vm->scope_stack, scope);
    return ns_array_last(ir_ctx->vm->scope_stack);
}

ns_scope *ns_ir_exit_scope(ns_ir_ctx *ir_ctx) {
    ns_scope *scope = ns_array_last(ir_ctx->vm->scope_stack);
    ns_array_set_length(ir_ctx->vm->symbol_stack, scope->symbol_top);
    ns_array_pop(ir_ctx->vm->scope_stack);
    return scope;
}

ns_ir_value ns_ir_primary_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t t = n->primary_expr.token;
    switch (n->primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return (ns_ir_value){.val = LLVMConstInt(LLVMInt32Type(), ns_str_to_i32(t.val), 0), .type = ns_ir_type_i32};
    case NS_TOKEN_FLT_LITERAL:
        return (ns_ir_value){.val = LLVMConstReal(LLVMDoubleType(), ns_str_to_f64(t.val)), .type = ns_ir_type_f64};
    case NS_TOKEN_STR_LITERAL: {
        i32 p = ns_array_length(ir_ctx->vm->str_list);
        ns_array_push(ir_ctx->vm->str_list, t.val);
        return (ns_ir_value){.val = LLVMBuildGlobalStringPtr(ir_ctx->bdr, ns_str_unescape(n->primary_expr.token.val).data, ""), .type = ns_ir_type_str, .p = p};
    } break;
    case NS_TOKEN_TRUE:
        return (ns_ir_value){.val = LLVMConstInt(LLVMInt32Type(), 1, 0), .type = ns_ir_type_bool};
    case NS_TOKEN_FALSE:
        return (ns_ir_value){.val = LLVMConstInt(LLVMInt32Type(), 0, 0), .type = ns_ir_type_bool};
    case NS_TOKEN_IDENTIFIER:
        return ns_ir_find_value(ir_ctx, n->primary_expr.token.val);
    default:
        break;
    }
    return ns_ir_nil;
}

#define ns_ir_MAX_FIELD_COUNT 128
static ns_ir_type_ref _types[ns_ir_MAX_FIELD_COUNT];

void ns_ir_struct_def(ns_ir_ctx *ir_ctx, ns_symbol *st, i32 i) {
    ns_ir_symbol ir_st = (ns_ir_symbol){.type = NS_IR_STRUCT, .name = st->name};
    ns_ir_st_symbol st_symbol = {.st = ns_ir_nil, .fields = ns_null};

    i32 field_count = ns_array_length(st->st.fields);
    ns_array_set_length(st_symbol.fields, field_count);
    for (i32 j = 0, l = field_count; j < l; j++) {
        ns_struct_field *field = &st->st.fields[j];
        ns_ir_type t = ns_ir_parse_type(ir_ctx, field->t);
        if (field->t.ref) t.type = LLVMPointerType(t.type, 0);
        st_symbol.fields[j] = (ns_ir_symbol){.type = NS_IR_VALUE, .name = field->name, .val = (ns_ir_value){.type = t, .val = ns_null, .p = j}};
        _types[j] = t.type;
    }
    ns_ir_type_ref st_type = LLVMStructType(_types, field_count, 0);
    st_symbol.st = (ns_ir_value){.val = ns_null, .type = (ns_ir_type){.raw = st->st.st.t, .type = st_type}, .p = i};
    ir_st.st = st_symbol;
    ns_ir_set_symbol(ir_ctx, ir_st, i);
}

ns_return_void ns_ir_fn_def(ns_ir_ctx *ir_ctx, ns_symbol *fn, i32 i) {
    ns_ir_symbol ir_fn = (ns_ir_symbol){.type = NS_IR_FN, .name = fn->name, .lib = fn->lib};
    i32 arg_count = ns_array_length(fn->fn.args);
    
    // parse argument types
    for (i32 j = 0; j < arg_count; j++) {
        ns_ir_type t = ns_ir_parse_type(ir_ctx, fn->fn.args[j].val.t);
        _types[j] = t.type;
    }

    // make and save fn symbol
    ns_ir_type ret = ns_ir_parse_type(ir_ctx, fn->fn.ret);
    ns_ir_type_ref ir_fn_type = LLVMFunctionType(ret.type, _types, arg_count, 0);
    ns_ir_value_ref ir_fn_val = LLVMAddFunction(ir_ctx->mod, ns_ir_str(fn->name), ir_fn_type);
    ns_ir_value fn_val = (ns_ir_value){.p = -1, .type = (ns_ir_type){.raw = fn->fn.fn.t, .type = ir_fn_type}, .val = ir_fn_val };
    ir_fn.fn.fn = fn_val;
    ir_fn.fn.ret = ret;
    ns_ir_set_symbol(ir_ctx, ir_fn, i);

    // parse function body
    return ns_return_ok_void;
}

void ns_ir_fn_body(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, ns_symbol *fn, i32 i) {
    if (fn->lib.len != 0 || fn->fn.fn.t.ref) return; // skip lib functions

    ns_ir_symbol *ir_fn = &ir_ctx->symbols[i];
    ns_ir_block entry = LLVMAppendBasicBlock(ir_fn->fn.fn.val, "entry");
    LLVMPositionBuilderAtEnd(ir_ctx->bdr, entry);

    i32 arg_count = ns_array_length(fn->fn.args);
    ns_ir_enter_scope(ir_ctx);
    for (i32 j = 0; j < arg_count; j++) {
        ns_ir_symbol arg = (ns_ir_symbol){.type = NS_IR_VALUE, .name = fn->fn.args[j].name};
        arg.val = (ns_ir_value){.val = LLVMGetParam(ir_fn->fn.fn.val, j), .type = ns_ir_parse_type(ir_ctx, fn->fn.args[j].val.t)};
        ns_array_push(ir_ctx->symbol_stack, arg);
    }

    ir_ctx->fn = fn->fn.fn.t.index;
    ns_ast_t *fn_ast = &ctx->nodes[fn->fn.ast];
    ns_ir_compound_stmt(ir_ctx, ctx, fn_ast->type == NS_AST_FN_DEF ? fn_ast->fn_def.body: fn_ast->ops_fn_def.body);
    ir_ctx->fn = -1;

    ns_ir_exit_scope(ir_ctx);
}

ns_ir_value ns_ir_binary_ops_number(ns_ir_ctx *ir_ctx, ns_ir_value l, ns_ir_value r, ns_token_t op) {
    ns_ir_builder bdr = ir_ctx->bdr;
    ns_bool f = ns_type_is_float(l.type.raw);
    ns_bool s = ns_type_signed(l.type.raw);
    ns_ir_value ret = (ns_ir_value){.type = l.type};
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
        else ns_error("ns_bc", "unimplemented compare ops.");
    } break;
    default:
        ns_error("ns_bc", "unimplemented binary ops.");
        break;
    }
    return ret;
}

ns_ir_value ns_ir_call_ops_fn(ns_ir_ctx *ir_ctx, ns_ir_symbol *fn, ns_ir_value l, ns_ir_value r) {
    ns_ir_builder bdr = ir_ctx->bdr;
    ns_ir_value ret = (ns_ir_value){.val = ns_null, .type = fn->fn.ret};
    ns_ir_value_ref args[2] = {l.val, r.val};
    ret.val = LLVMBuildCall2(bdr, fn->fn.fn.type.type, fn->fn.fn.val, args, 2, "");
    return ret;
}

ns_ir_value ns_ir_binary_ops(ns_ir_ctx *ir_ctx, ns_ir_value l, ns_ir_value r, ns_token_t op) {
    if (ns_type_is_number(l.type.raw)) {
        return ns_ir_binary_ops_number(ir_ctx, l, r, op);
    } else {
        ns_vm *vm = ir_ctx->vm;
        ns_str l_t = ns_vm_get_type_name(vm, l.type.raw);
        ns_str r_t = ns_vm_get_type_name(vm, r.type.raw);
        ns_str fn_name = ns_ops_override_name(l_t, r_t, op);
        ns_ir_symbol *fn = ns_ir_find_symbol(ir_ctx, fn_name);
        if (fn) {
            return ns_ir_call_ops_fn(ir_ctx, fn, l, r);
        }

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
    return ns_ir_nil;
}

ns_ir_value ns_ir_binary_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ir_builder bdr = ir_ctx->bdr;
    ns_ast_t *n = &ctx->nodes[i];

    ns_ir_value l = ns_ir_expr(ir_ctx, ctx, n->binary_expr.left);
    ns_ir_value r = ns_ir_expr(ir_ctx, ctx, n->binary_expr.right);
    if (ns_type_equals(l.type.raw, r.type.raw)) {
        return ns_ir_binary_ops(ir_ctx, l, r, n->binary_expr.op);
    }

    ns_ir_value ret = (ns_ir_value){.val = ns_null, .type = l.type, .p = -1};
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

void ns_ir_jump_stmt(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ir_builder bdr = ir_ctx->bdr;
    ns_ast_t *n = &ctx->nodes[i];

    ns_token_t t = n->jump_stmt.label;
    if (ns_str_equals_STR(t.val, "return")) {
        if (n->jump_stmt.expr) {
            ns_ir_value ret = ns_ir_expr(ir_ctx, ctx, n->jump_stmt.expr);
            LLVMBuildRet(bdr, ret.val);
        } else {
            LLVMBuildRetVoid(bdr);
        }
    }
}

ns_ir_value ns_ir_call_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ir_builder bdr = ir_ctx->bdr;
    ns_ast_t *n = &ctx->nodes[i];

    ns_ir_value ir_fn = ns_ir_expr(ir_ctx, ctx, n->call_expr.callee);
    ns_ir_symbol *fn_symbol = &ir_ctx->symbols[ir_fn.p];
    i32 arg_count = n->call_expr.arg_count;
    ns_symbol *fn = &ir_ctx->vm->symbols[ir_fn.p];

    i32 next = n->call_expr.arg;
    for (i32 i = 0; i < n->call_expr.arg_count; i++) {
        ns_ast_t arg = ctx->nodes[next];
        _args[i] = ns_ir_expr(ir_ctx, ctx, next).val;
        next = arg.next;
    }

    if (fn->lib.len != 0 && ns_str_equals_STR(fn->lib, "std")) {
        return ns_ir_call_std(ir_ctx, ctx, fn);
    }

    ns_ir_value ret = (ns_ir_value){.val = ns_null, .type = fn_symbol->fn.ret};
    ns_ir_value fn_val = fn_symbol->fn.fn;
    ret.val = LLVMBuildCall2(bdr, fn_val.type.type, fn_val.val, _args, arg_count, "");
    return ret;
}

ns_ir_value ns_ir_str_fmt_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 n_i) {
    ns_ast_t *n = &ctx->nodes[n_i];
    i32 expr_count = n->str_fmt.expr_count;
    ns_str fmt = n->str_fmt.fmt;

    if (expr_count == 0) return (ns_ir_value){.val = LLVMConstString(fmt.data, fmt.len, 0), .type = ns_ir_type_str, .p = -1};
    
    ns_str ret = (ns_str){.data = ns_null, .len = 0, .dynamic = 1};
    ns_array_set_capacity(ret.data, fmt.len);
    ns_array_set_length(ir_ctx->symbol_stack, expr_count + 3);

    i32 i = 0;
    i32 expr_i = 0;
    i32 next = n_i;
    while (i < fmt.len) {
        if (fmt.data[i] == '{' && (i == 0 || (i > 0 && fmt.data[i - 1] != '\\'))) {
            while (i < fmt.len && fmt.data[i] != '}') {
                i++;
            }
            if (i == fmt.len) {
                ns_error("fmt error", "missing '}'.");
            }
            i++;

            n = &ctx->nodes[next = n->next];
            ns_type t = n->expr.type;
            ns_str p = ns_fmt_type_str(t);
            ns_str_append(&ret, p);

            ns_ir_value val = ns_ir_expr(ir_ctx, ctx, next);
            _args[expr_i + 3] = val.val;

            expr_i++;
            if (expr_i > expr_count) {
                ns_error("fmt error", "too many arguments.\n");
            }
        } else {
            ns_array_push(ret.data, fmt.data[i++]);
        }
    }
    ret.len = ns_array_length(ret.data);

    // snprintf get length
    ns_ir_value ir_snprintf = ns_ir_std_snprinf(ir_ctx);

    _args[0] = LLVMConstNull(LLVMPointerType(LLVMInt8Type(), 0));
    _args[1] = LLVMConstNull(LLVMInt64Type());

    ns_ir_value_ref fmt_str = LLVMBuildGlobalStringPtr(ir_ctx->bdr, ret.data, "");
    _args[2] = fmt_str;
    ns_ir_value_ref len = LLVMBuildCall2(ir_ctx->bdr, ir_snprintf.type.type, ir_snprintf.val, _args, 3 + expr_count, "");
    ns_ir_value_ref add_one = LLVMBuildAdd(ir_ctx->bdr, len, LLVMConstInt(LLVMInt32Type(), 1, 0), "");

    _args[0] = len;
    ns_ir_value_ref buffer = LLVMBuildArrayMalloc(ir_ctx->bdr, LLVMInt8Type(), add_one, ""); 

    _args[0] = buffer;
    _args[1] = LLVMBuildSExt(ir_ctx->bdr, len, LLVMInt64Type(), "");
    LLVMBuildCall2(ir_ctx->bdr, ir_snprintf.type.type, ir_snprintf.val, _args, 3 + expr_count, ""); // sprintf to buffer

    // set end of string as '\0'
    ns_ir_value_ref end = LLVMBuildGEP2(ir_ctx->bdr, LLVMInt8Type(), buffer, &add_one, 1, "");
    LLVMBuildStore(ir_ctx->bdr, LLVMConstInt(LLVMInt8Type(), 0, 0), end);
    return (ns_ir_value){.val = buffer, .type = ns_ir_type_str, .p = -1};
}

ns_ir_value ns_ir_member_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx* ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ir_value ir_st = ns_ir_expr(ir_ctx, ctx, n->member_expr.left);

    ns_ast_t *field = &ctx->nodes[n->next];
    if (field->type == NS_AST_MEMBER_EXPR)
        return ns_ir_member_expr(ir_ctx, ctx, n->next);

    ns_str name = field->primary_expr.token.val;
    ns_symbol *st = &ir_ctx->vm->symbols[ns_type_index(ir_st.type.raw)];

    i32 index = ns_struct_field_index(st, name);
    if (index == -1) ns_error("ns_bc", "field %.*s not found.\n", name.len, name.data);

    ns_struct_field *st_field = &st->st.fields[index];
    ns_ir_value_ref st_index = LLVMConstInt(LLVMInt32Type(), index, 0);
    if (ns_type_is_ref(ir_st.type.raw)) {
        ns_ir_value_ref member = LLVMBuildGEP2(ir_ctx->bdr, LLVMPointerType(LLVMInt8Type(), 0), ir_st.val, &st_index, 1, "");
        ns_ir_type_ref t = LLVMPointerType(ns_ir_parse_type(ir_ctx, st_field->t).type, 0);
        return (ns_ir_value){.val = member, .type = (ns_ir_type){.type = t, .raw = st_field->t}};
    } else {
        ns_ir_value_ref member = LLVMBuildExtractValue(ir_ctx->bdr, ir_st.val, index, "");
        ns_ir_type t = ns_ir_parse_type(ir_ctx, st_field->t);
        return (ns_ir_value){.val = member, .type = t};
    }
}

ns_ir_value ns_ir_designated_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ir_symbol *st = ns_ir_find_symbol(ir_ctx, n->desig_expr.name.val);
    if (st == nil) ns_error("ns_bc", "struct %.*s not found.\n", n->desig_expr.name.val.len, n->desig_expr.name.val.data);

    ns_ast_t *field = n;
    i32 count = n->desig_expr.count;
    while (count > 0) {
        field = &ctx->nodes[field->next];
        // ns_type t = st->st.fields[field->desig_expr.index].t;
        count--;
    }

    return ns_ir_nil;
}

ns_ir_value ns_ir_unary_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ir_value ret = ns_ir_expr(ir_ctx, ctx, n->unary_expr.expr);

    ns_token_t op = n->unary_expr.op;
    switch (op.type)
    {
    case NS_TOKEN_ADD_OP:
        if (ns_str_equals_STR(op.val, "+")) return ret;
        if (ns_str_equals_STR(op.val, "-")) {
            if (ns_type_is_float(ret.type.raw)) {
                ret.val = LLVMBuildFNeg(ir_ctx->bdr, ret.val, "");
            } else {
                ret.val = LLVMBuildNeg(ir_ctx->bdr, ret.val, "");
            }
        }
        break;
    case NS_TOKEN_BIT_INVERT_OP:
        ret.val = LLVMBuildNot(ir_ctx->bdr, ret.val, "");
        break;
    default:
        ns_error("ns_bc", "unimplemented unary ops\n");
        break;
    }
    return ret;
}

ns_ir_value ns_ir_local_var_def(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ir_value val = ns_ir_expr(ir_ctx, ctx, n->var_def.expr);
    ns_str name = n->var_def.name.val;
    ns_ir_symbol val_symbol = (ns_ir_symbol){.type = NS_IR_VALUE, .name = name, .val = val};
    ns_array_push(ir_ctx->symbol_stack, val_symbol);
    return val;
}

void ns_ir_var_def(ns_ir_ctx *ir_ctx, ns_symbol *s, i32 i) {
    ns_ir_value val = (ns_ir_value){.p = i, .type = ns_ir_parse_type(ir_ctx, s->val.t)};
    ns_ir_symbol r = (ns_ir_symbol){.type = NS_IR_VALUE, .name = s->name, .val = val};
    ns_ir_set_symbol(ir_ctx, r, i);
}

ns_ir_value ns_ir_expr(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_ir_expr(ir_ctx, ctx, n->expr.body);
    case NS_AST_BINARY_EXPR: return ns_ir_binary_expr(ir_ctx, ctx, i);
    case NS_AST_PRIMARY_EXPR: return ns_ir_primary_expr(ir_ctx, ctx, i);
    case NS_AST_CALL_EXPR: return ns_ir_call_expr(ir_ctx, ctx, i);
    case NS_AST_STR_FMT_EXPR: return ns_ir_str_fmt_expr(ir_ctx, ctx, i);
    case NS_AST_MEMBER_EXPR: return ns_ir_member_expr(ir_ctx, ctx, i);
    case NS_AST_DESIG_EXPR: return ns_ir_designated_expr(ir_ctx, ctx, i);
    case NS_AST_UNARY_EXPR: return ns_ir_unary_expr(ir_ctx, ctx, i);
    default: {
        ns_str type = ns_ast_type_to_string(n->type);
        ns_error("ns_bc", "unimplemented expr type %.*s\n", type.len, type.data);
    } break;
    }
    return ns_ir_nil;
}

void ns_ir_if_stmt(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    ns_ir_builder bdr = ir_ctx->bdr;
    ns_ast_t *n = &ctx->nodes[i];
    ns_ir_value cond = ns_ir_expr(ir_ctx, ctx, n->if_stmt.condition);

    ns_ir_symbol *ir_fn = &ir_ctx->symbols[ir_ctx->fn];
    assert(ir_fn->type == NS_IR_FN);

    ns_ir_value_ref fn = ir_fn->fn.fn.val;
    ns_ir_block then = LLVMAppendBasicBlock(fn, "");
    ns_ir_block els = LLVMAppendBasicBlock(fn, "");
    ns_ir_block merge = LLVMAppendBasicBlock(fn, "");

    LLVMBuildCondBr(bdr, cond.val, then, els);
    LLVMPositionBuilderAtEnd(bdr, then);
    ns_ir_compound_stmt(ir_ctx, ctx, n->if_stmt.body);
    LLVMBuildBr(bdr, merge);

    LLVMPositionBuilderAtEnd(bdr, els);
    ns_ir_compound_stmt(ir_ctx, ctx, n->if_stmt.else_body);
    LLVMBuildBr(bdr, merge);

    LLVMPositionBuilderAtEnd(bdr, merge);
}

void ns_ir_for_stmt(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    
}

void ns_ir_compound_stmt(ns_ir_ctx *ir_ctx, ns_ast_ctx *ctx, i32 i) {
    i32 next;
    ns_ast_t *n = &ctx->nodes[i];
    i32 l = n->compound_stmt.count;
    for (i32 i = 0; i < l; i++) {
        next = n->next;
        n = &ctx->nodes[next];
        switch (n->type) {
        case NS_AST_JUMP_STMT: ns_ir_jump_stmt(ir_ctx, ctx, next); break;
        case NS_AST_VAR_DEF: ns_ir_local_var_def(ir_ctx, ctx, next); break;
        case NS_AST_IF_STMT: ns_ir_if_stmt(ir_ctx, ctx, next); break;
        case NS_AST_FOR_STMT: ns_ir_for_stmt(ir_ctx, ctx, next); break;
        default:
            ns_error("ns_bc", "unimplemented compound stmt\n");
        break;
        }
    }
}

void ns_ir_parse_ast(ns_ir_ctx* ir_ctx, ns_ast_ctx *ctx) {
    ns_vm *vm = ir_ctx->vm;
    ns_ir_module mod = ir_ctx->mod;
    ns_ir_builder bdr = ir_ctx->bdr;
    i32 symbol_count = ns_array_length(vm->symbols);

    ns_array_set_length(ir_ctx->symbols, symbol_count);
    i32 main_i = 0;

    // parse struct and fn
    for (i32 i = 0, l = symbol_count; i < l; i++) {
        ns_symbol *s = &vm->symbols[i];
        switch (s->type)
        {
        case NS_SYMBOL_FN:
            if (ns_str_equals(s->name, ns_str_cstr("main"))) {
                if (main_i != 0) ns_error("ns_bc", "multiple main functions.\n");
                main_i = i; break;
            }
            ns_ir_fn_def(ir_ctx, s, i); break;
        case NS_SYMBOL_STRUCT: ns_ir_struct_def(ir_ctx, s, i); break;
        default:
            break;
        }
    }

    if (main_i == 0) {
        for (i32 i = 0, l = symbol_count; i < l; i++) {
            ns_symbol *s = &vm->symbols[i];
            if (s->type != NS_SYMBOL_VALUE) continue;
            ns_ir_var_def(ir_ctx, s, i);
        }
    }

    // parse fn body
    for (i32 i = 0, l = symbol_count; i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        if (s->type != NS_SYMBOL_FN) continue;
        if (ns_str_equals(s->name, ns_str_cstr("main"))) continue;
        ns_ir_fn_body(ir_ctx, ctx, s, i);
    }

    // parse main fn
    if (main_i != 0) {
        ns_symbol *main_fn = &vm->symbols[main_i];
        ns_ir_fn_def(ir_ctx, main_fn, main_i);
        ns_ir_fn_body(ir_ctx, ctx, main_fn, main_i);
    } else {
        ns_ir_type main_fn_type = (ns_ir_type){.type = ns_null, .raw = ns_type_encode(ns_type_fn, 0, true, 0)};
        main_fn_type.type = LLVMFunctionType(LLVMInt32Type(), ns_null, 0, 0);
        ns_ir_value main_fn = (ns_ir_value){.p = -1, .val = ns_null, .type = main_fn_type};
        main_i = ns_array_length(ir_ctx->symbols);
        ns_ir_symbol r = (ns_ir_symbol){.type = NS_IR_FN, .name = ns_str_cstr("main"), .fn = (ns_ir_fn_symbol){.fn = main_fn, .ret = ns_ir_type_i32}};
        ns_ir_set_symbol(ir_ctx, r, main_i);

        main_fn.val = LLVMAddFunction(mod, "main", main_fn_type.type);
        ns_ir_block entry_main = LLVMAppendBasicBlock(main_fn.val, "entry");
        LLVMPositionBuilderAtEnd(bdr, entry_main);

        ns_ir_enter_scope(ir_ctx);
        // parse deferred nodes as main fn body
        for (i32 i = ctx->section_begin; i < ctx->section_end; ++i) {
            i32 s = ctx->sections[i];
            ns_ast_t *n = &ctx->nodes[s];
            switch (n->type) {
            case NS_AST_VAR_DEF: ns_ir_local_var_def(ir_ctx, ctx, s); break;
            case NS_AST_CALL_EXPR: ns_ir_call_expr(ir_ctx, ctx, s); break;
            default:
                break;
            }
        }
        LLVMBuildRet(bdr, LLVMConstInt(LLVMInt32Type(), 0, 0));
        ns_ir_exit_scope(ir_ctx);
    }
}

ns_return_bool ns_ir_gen(ns_str input, ns_str output) {
    ns_vm vm = {0};
    ns_ast_ctx ctx = {0};
    ns_ir_ctx ir_ctx = {.vm = &vm};

    ns_str source = ns_fs_read_file(input);
    if (source.len == 0) return ns_return_error(bool, ns_code_loc_nil, NS_ERR_BITCODE, "empty file.\n");

    if (output.len == 0) {
        ns_str path = ns_path_filename(input);
        path = ns_str_concat(path, ns_str_cstr(".bc"));
        output = ns_path_join(ns_str_cstr(ns_ir_cache), path);
    }
    ctx.source = source;

    ns_return_bool ret = ns_ast_parse(&ctx, source, input);
    if (ns_return_is_error(ret)) return ret;

    ret = ns_vm_parse(&vm, &ctx);
    if (ns_return_is_error(ret)) return ret;

    ns_ir_module mod = LLVMModuleCreateWithName(input.data);
    ns_ir_builder bdr = LLVMCreateBuilder();
    ir_ctx.mod = mod;
    ir_ctx.bdr = bdr;

    ns_ir_parse_ast(&ir_ctx, &ctx);

    i8* error = ns_null;
    LLVMVerifyModule(mod, LLVMAbortProcessAction, &error);
    LLVMDisposeMessage(error);

    // Write out bitcode to file
    // if (LLVMWriteBitcodeToFile(mod, output.data) != 0)
    if (LLVMPrintModuleToFile(mod, output.data, &error) != 0)
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_BITCODE, "fail writing bitcode to file.");

    LLVMDisposeBuilder(bdr);
    LLVMDisposeModule(mod);
    return ns_return_ok(bool, true);
}

#endif // NS_IR