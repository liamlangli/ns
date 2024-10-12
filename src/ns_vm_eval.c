#include "ns_vm.h"

ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

bool ns_type_is_number(ns_type t) {
    switch (t.type) {
    case NS_TYPE_I8:
    case NS_TYPE_I16:
    case NS_TYPE_I32:
    case NS_TYPE_I64:
    case NS_TYPE_U8:
    case NS_TYPE_U16:
    case NS_TYPE_U32:
    case NS_TYPE_U64:
    case NS_TYPE_F32:
    case NS_TYPE_F64:
        return true;
    default:
        return false;
    }
}

ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value callee = ns_eval_expr(vm, ctx, ctx->nodes[n.call_expr.callee]);
    if (ns_is_nil(callee)) {
        ns_error("eval error", "unknown callee\n");
    }

    ns_record *fn = &vm->records[callee.p];
    ns_call call = (ns_call){.fn = fn, .args = NULL };
    ns_array_set_length(call.args, ns_array_length(fn->fn.args));

    ns_ast_t arg = n;
    for (int i = 0, l = n.call_expr.arg_count; i < l; ++i) {
        arg = ctx->nodes[arg.next];
        ns_value v = ns_eval_expr(vm, ctx, arg);
        call.args[i] = v;
    }
    ns_array_push(vm->call_stack, call);
    ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast]);
    ns_array_pop(vm->call_stack);
    return call.ret;
}

ns_value ns_eval_return_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = ctx->nodes[n.jump_stmt.expr];
    ns_value v = ns_eval_expr(vm, ctx, expr);
    return v;
}

ns_value ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.jump_stmt.label.type) {
    case NS_TOKEN_RETURN: return ns_eval_return_stmt(vm, ctx, n);
    default: {
        ns_str l = n.jump_stmt.label.val;
        ns_error("eval error", "unknown jump stmt type %.*s\n", l.len, l.data);
    } break;
    }
}

ns_value ns_eval_binary_ops_number(ns_value lhs, ns_value rhs, ns_token_t op) {
    bool f = ns_type_is_float(lhs.type);
    switch (op.type) {
    case NS_TOKEN_ADD_OP: {
        if (ns_str_equals_STR(op.val, "+"))
            return f ? (ns_value){.type = NS_TYPE_F64, .f = lhs.f + rhs.f} : (ns_value){.type = NS_TYPE_I64, .i = lhs.i + rhs.i};
        else 
            return f ? (ns_value){.type = NS_TYPE_F64, .f = lhs.f - rhs.f} : (ns_value){.type = NS_TYPE_I64, .i = lhs.i - rhs.i};
    } break;
    case NS_TOKEN_MUL_OP: {
        if (ns_str_equals_STR(op.val, "*"))
            return f ? (ns_value){.type = NS_TYPE_F64, .f = lhs.f * rhs.f} : (ns_value){.type = NS_TYPE_I64, .i = lhs.i * rhs.i};
        else if (ns_str_equals(op.val, ns_str_cstr("/")))
            return f ? (ns_value){.type = NS_TYPE_F64, .f = lhs.f / rhs.f} : (ns_value){.type = NS_TYPE_I64, .i = lhs.i / rhs.i};
        else
            return f ? (ns_value){.type = NS_TYPE_F64, .f = fmod(lhs.f, rhs.f)} : (ns_value){.type = NS_TYPE_I64, .i = lhs.i % rhs.i};
    } break;
    case NS_TOKEN_BOOL_OP: {
        if (ns_str_equals_STR(op.val, "&&"))
            return f ? ns_true : (ns_value){.type = ns_type_bool, .i = lhs.i && rhs.i};
        else
        return f ? ns_true : (ns_value){.type = ns_type_bool, .i = lhs.i && rhs.i};
    } break;
    default: ns_error("eval error", "unimplemented binary ops\n");
    }
    return ns_nil;
}

ns_value ns_eval_binary_ops(ns_vm *vm, ns_value lhs, ns_value rhs, ns_token_t op) {
    if (ns_type_is_int_number(lhs.type)) {
        return ns_eval_binary_ops_number(lhs, rhs, op);
    } else {
        switch (lhs.type.type)
        {
        case NS_TYPE_STRING:
            ns_error("eval error", "unimplemented string ops\n");
        case NS_TYPE_BOOL:
            return (ns_value){.type = ns_type_bool, .i = lhs.i && rhs.i};
            break;
        default:
            break;
        }
        ns_error("eval error", "unimplemented binary ops\n");
    }
}

ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value left = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.left]);
    ns_value right = ns_eval_expr(vm, ctx, ctx->nodes[n.binary_expr.right]);
    if (left.type.type == right.type.type) {
        return ns_eval_binary_ops(vm, left, right, n.binary_expr.op); // same type apply binary operator
    } else {
        // type mismatch, try to cast
        ns_error("eval error", "binary expr type mismatch\n");
        return ns_nil;
    }
}

ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    return ns_nil;
}

ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.type) {
    case NS_AST_EXPR:
        return ns_eval_expr(vm, ctx, ctx->nodes[n.expr.body]);
    case NS_AST_CALL_EXPR:
        return ns_eval_call_expr(vm, ctx, n);
    case NS_AST_JUMP_STMT:
        return ns_eval_jump_stmt(vm, ctx, n);
    case NS_AST_BINARY_EXPR:
        return ns_eval_binary_expr(vm, ctx, n);
    default: {
        ns_str type = ns_ast_type_to_string(n.type);
        ns_error("eval error", "unimplemented expr type %.*s\n", type.len, type.data);
    } break;
    }

    return ns_nil;
}

ns_value ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = n;
    for (int i = 0, l = n.compound_stmt.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        switch (expr.type) {
        case NS_AST_JUMP_STMT:
            ns_eval_jump_stmt(vm, ctx, expr);
            break;
        case NS_AST_VAR_DEF:
            ns_eval_var_def(vm, ctx, expr);
            break;
        default: {
            ns_str type = ns_ast_type_to_string(expr.type);
            ns_error("eval error", "unimplemented stmt type %.*s\n", type.len, type.data);
        } break;
        }
    }
    return ns_nil;
}

ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_record *val = ns_vm_find_record(vm, n.var_def.name.val);
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.var_def.expr]);
    val->val.val.p = val->index;
    val->val.val = v;
    return v;
}

ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename) {
    ns_ast_ctx ctx = {0};
    ns_ast_parse(&ctx, source, filename);
    ns_vm_parse(vm, &ctx);

    // eval global value
    for (int i = 0, l = ns_array_length(vm->records); i < l; ++i) {
        ns_record r = vm->records[i];
        if (r.type != NS_RECORD_VALUE)
            continue;
        ns_value v = r.val.val;
        if (r.val.is_const) {
            ns_record *record = &vm->records[i];
            record->val.val = v;
        }
    }

    ns_value ret = ns_nil;
    for (int i = ctx.section_begin, l = ctx.section_end; i < l; ++i) {
        ns_ast_t n = ctx.nodes[ctx.sections[i]];
        switch (n.type) {
        case NS_AST_EXPR:
        case NS_AST_CALL_EXPR:
            ns_eval_expr(vm, &ctx, n);
            break;
        case NS_AST_VAR_DEF:
            ns_eval_var_def(vm, &ctx, n);
            break;
        case NS_AST_IMPORT_STMT:
        case NS_AST_FN_DEF:
        case NS_AST_OPS_FN_DEF:
        case NS_AST_STRUCT_DEF:
            break; // already parsed
        default: {
            ns_str type = ns_ast_type_to_string(n.type);
            ns_warn("eval error", "unimplemented global ast %.*s\n", type.len, type.data);
        } break;
        }
    }

    return ret;
}
