#include "ns_vm.h"

#include <math.h>

void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n);
ns_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

ns_value ns_vm_find_value(ns_vm *vm, ns_str name) {
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call* call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        for (i32 i = 0, l = ns_array_length(call->fn->fn.args); i < l; ++i) {
            if (ns_str_equals(call->fn->fn.args[i].name, name)) {
                return call->args[i];
            }
        }

        ns_scope* scope = &call->scopes[ns_array_length(call->scopes) - 1];
        for (i32 i = 0, l = ns_array_length(scope->vars); i < l; ++i) {
            if (ns_str_equals(scope->vars[i].name, name)) {
                return scope->vars[i].val.val;
            }
        }
    }

    ns_symbol *r = ns_vm_find_symbol(vm, name);
    if (!r) return ns_nil;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *r = &vm->symbols[i];
        if (ns_str_equals(r->name, name)) {
            switch (r->type)
            {
            case NS_SYMBOL_VALUE: return r->val.val;
            case NS_SYMBOL_FN: return r->fn.fn;
            default:
                break;
            }
        }
    }

    return ns_nil;
}

ns_value ns_eval_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_value callee = ns_eval_expr(vm, ctx, ctx->nodes[n.call_expr.callee]);
    if (ns_is_nil(callee)) {
        ns_error("eval error", "unknown callee\n");
    }

    ns_symbol *fn = &vm->symbols[callee.p];
    ns_call call = (ns_call){.fn = fn, .args = NULL, .scopes = NULL };
    ns_array_set_length(call.args, ns_array_length(fn->fn.args));

    ns_scope scope = (ns_scope){.vars = NULL, .stack_top = ns_array_length(vm->stack)};
    ns_array_push(call.scopes, scope);

    i32 next = n.call_expr.arg;
    for (i32 i = 0, l = n.call_expr.arg_count; i < l; ++i) {
        ns_ast_t arg = ctx->nodes[next];
        next = arg.next;
        ns_value v = ns_eval_expr(vm, ctx, arg);
        call.args[i] = v;
    }

    ns_array_push(vm->call_stack, call);
    if (ns_str_equals_STR(fn->lib, "std")) {
        ns_vm_eval_std(vm);
    } else {
        ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.fn_def.body]);
    }
    call = ns_array_pop(vm->call_stack);
    ns_array_set_length(vm->stack, scope.stack_top);
    return call.ret;
}

ns_value ns_eval_return_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = ctx->nodes[n.jump_stmt.expr];
    ns_value ret = ns_eval_expr(vm, ctx, expr);
    if (ns_array_length(vm->call_stack) > 0) {
        ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
        if (call->fn->fn.ret.type != ret.type.type) {
            // TODO: try type cast, emit error if failed
            ns_error("eval error", "return type mismatch\n");
        }
        call->ret = ret;
    }
    return ret;
}

void ns_eval_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.jump_stmt.label.type) {
    case NS_TOKEN_RETURN: ns_eval_return_stmt(vm, ctx, n); break;
    default: {
        ns_str l = n.jump_stmt.label.val;
        ns_error("eval error", "unknown jump stmt type %.*s\n", l.len, l.data);
    } break;
    }
}

ns_value ns_eval_binary_ops_number(ns_value l, ns_value r, ns_token_t op) {
    bool f = ns_type_is_float(l.type);
    ns_value ret = (ns_value){.type = l.type};
    switch (op.type) {
    case NS_TOKEN_ADD_OP: {
        if (ns_str_equals_STR(op.val, "+")) 
            if (f) { ret.f = l.f + r.f; } else { ret.i = l.i + r.i; }
        else 
            if (f) { ret.f = l.f - r.f; } else { ret.i = l.i - r.i; }
    } break;
    case NS_TOKEN_MUL_OP: {
        if (ns_str_equals_STR(op.val, "*"))
            if (f) { ret.f = l.f * r.f; } else { ret.i = l.i * r.i; }
        else if (ns_str_equals(op.val, ns_str_cstr("/")))
            if (f) { ret.f = l.f / r.f; } else { ret.i = l.i / r.i; }
        else
            if (f) { ret.f = fmod(l.f, r.f); } else { ret.i = l.i % r.i; }
    } break;
    case NS_TOKEN_SHIFT_OP: {
        if (f) ns_error("eval error", "shift op not support float\n");
        if (ns_str_equals_STR(op.val, "<<"))
            ret.i = l.i << r.i;
        else
            ret.i = l.i >> r.i;
    } break;
    case NS_TOKEN_LOGIC_OP: {
        if (ns_str_equals_STR(op.val, "&&"))
            return f ? ns_true : (ns_value){.type = ns_type_bool, .i = l.i && r.i};
        else
            return f ? ns_true : (ns_value){.type = ns_type_bool, .i = l.i || r.i};
    }
    case NS_TOKEN_CMP_OP: {
        if (ns_str_equals_STR(op.val, "=="))
            return f ? (ns_value){.type = ns_type_bool, .i = l.f == r.f} : (ns_value){.type = ns_type_bool, .i = l.i == r.i};
        else if (ns_str_equals_STR(op.val, "!="))
            return f ? (ns_value){.type = ns_type_bool, .i = l.f != r.f} : (ns_value){.type = ns_type_bool, .i = l.i != r.i};
        else if (ns_str_equals_STR(op.val, "<"))
            return f ? (ns_value){.type = ns_type_bool, .i = l.f < r.f} : (ns_value){.type = ns_type_bool, .i = l.i < r.i};
        else if (ns_str_equals_STR(op.val, "<="))
            return f ? (ns_value){.type = ns_type_bool, .i = l.f <= r.f} : (ns_value){.type = ns_type_bool, .i = l.i <= r.i};
        else if (ns_str_equals_STR(op.val, ">"))
            return f ? (ns_value){.type = ns_type_bool, .i = l.f > r.f} : (ns_value){.type = ns_type_bool, .i = l.i > r.i};
        else if (ns_str_equals_STR(op.val, ">="))
            return f ? (ns_value){.type = ns_type_bool, .i = l.f >= r.f} : (ns_value){.type = ns_type_bool, .i = l.i >= r.i};
    } break;
        default:
        ns_error("eval error", "unimplemented binary ops\n");
        break;
    }
    return ret;
}

ns_value ns_eval_call_ops_fn(ns_vm *vm, ns_ast_ctx *ctx, ns_value l, ns_value r, ns_token_t op) {
    ns_str l_name = ns_vm_get_type_name(vm, l.type);
    ns_str r_name = ns_vm_get_type_name(vm, r.type);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
    if (fn == NULL) {
        ns_error("eval error", "override fn not found %.*s %.*s %.*s\n", l_name.len, l_name.data, op.val.len, op.val.data, r_name.len, r_name.data);
    }

    ns_call call = (ns_call){.fn = fn, .args = NULL };
    ns_array_set_length(call.args, 2);
    call.args[0] = l;
    call.args[1] = r;

    ns_array_push(vm->call_stack, call);
    ns_eval_compound_stmt(vm, ctx, ctx->nodes[fn->fn.ast.fn_def.body]);
    ns_array_pop(vm->call_stack);

    return call.ret;
}

ns_value ns_eval_binary_ops(ns_value l, ns_value r, ns_token_t op) {
    if (ns_type_is_number(l.type)) {
        return ns_eval_binary_ops_number(l, r, op);
    } else {
        switch (l.type.type)
        {
        case NS_TYPE_STRING:
            ns_error("eval error", "unimplemented string ops\n");
        case NS_TYPE_BOOL:
            return (ns_value){.type = ns_type_bool, .i = l.i && r.i};
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
        return ns_eval_binary_ops(left, right, n.binary_expr.op); // same type apply binary operator
    } else {
        // TODO
        // step 1: if type not the same, try to find override function for binary operator
        // step 2: if override function not found, try to string cast and apply binary operator
        // step 3: if string cast not found, upcast number type to f64 and i64 and apply binary operator
        // step 4: emit error if not override function found
        ns_error("eval error", "binary expr type mismatch\n");
        return ns_nil;
    }
}

ns_value ns_eval_primary_expr(ns_vm *vm, ns_ast_t n) {
    ns_token_t t = n.primary_expr.token;
    switch (t.type) {
    case NS_TOKEN_INT_LITERAL:
        return (ns_value){.type = ns_type_i64, .i = ns_str_to_i32(t.val)};
    case NS_TOKEN_FLT_LITERAL:
        return (ns_value){.type = ns_type_f64, .f = ns_str_to_f64(t.val)};
    case NS_TOKEN_STR_LITERAL:
        return (ns_value){.type = ns_type_str, .p = ns_vm_push_string(vm, t.val)};
    case NS_TOKEN_TRUE:
        return ns_true;
    case NS_TOKEN_FALSE:
        return ns_false;
    case NS_TOKEN_IDENTIFIER:
        return ns_vm_find_value(vm, t.val);
    default: {
        ns_str type = ns_token_type_to_string(t.type);
        ns_error("eval error", "unimplemented primary expr type %.*s\n", type.len, type.data);
    } break;
    }
    return ns_nil;
}

ns_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.type) {
    case NS_AST_EXPR:
        return ns_eval_expr(vm, ctx, ctx->nodes[n.expr.body]);
    case NS_AST_CALL_EXPR:
        return ns_eval_call_expr(vm, ctx, n);
    case NS_AST_BINARY_EXPR:
        return ns_eval_binary_expr(vm, ctx, n);
    case NS_AST_PRIMARY_EXPR:
        return ns_eval_primary_expr(vm, n);
    case NS_AST_DESIG_EXPR:
        return ns_eval_desig_expr(vm, ctx, n);
    default: {
        ns_str type = ns_ast_type_to_string(n.type);
        ns_error("eval error", "unimplemented expr type %.*s\n", type.len, type.data);
    } break;
    }

    return ns_nil;
}

void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_ast_t expr = n;
    for (i32 i = 0, l = n.compound_stmt.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        switch (expr.type) {
        case NS_AST_JUMP_STMT:
            ns_eval_jump_stmt(vm, ctx, expr);
            break;
        case NS_AST_VAR_DEF:
            ns_eval_local_var_def(vm, ctx, expr);
            break;
        default: {
            ns_str type = ns_ast_type_to_string(expr.type);
            ns_error("eval error", "unimplemented stmt type %.*s\n", type.len, type.data);
        } break;
        }
    }
}

ns_value ns_eval_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_symbol *st = ns_vm_find_symbol(vm, n.desig_expr.name.val);
    if (NULL == st) ns_vm_error(ctx->filename, n.state, "eval error", "unknown struct %.*s\n", n.desig_expr.name.val.len, n.desig_expr.name.val.data);

    i32 offset = ns_array_length(vm->stack);
    // align in 4 bytes;
    offset = (offset + 3) & ~3;
    i32 stride = st->st.stride;

    ns_array_set_length(vm->stack, offset + stride);
    i8* data = &vm->stack[offset];
    memset(data, 0, stride);

    ns_ast_t expr = n;
    for (i32 i = 0, l = n.desig_expr.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        ns_str name = expr.field_def.name.val;

        ns_symbol *field = NULL;
        for (i32 j = 0, jl = ns_array_length(st->st.fields); j < jl; ++j) {
            field = &st->st.fields[j];
            if (ns_str_equals(field->name, name)) {
                break;
            }
        }

        if (field == NULL) {
            ns_vm_error(ctx->filename, expr.state, "eval error", "unknown field %.*s\n", name.len, name.data);
        }

        ns_value val = ns_eval_expr(vm, ctx, ctx->nodes[expr.field_def.expr]);
        if (field->val.type.type != val.type.type) { // type mismatch
            ns_str f_type = ns_vm_get_type_name(vm, field->val.type);
            ns_str v_type = ns_vm_get_type_name(vm, val.type);
            ns_vm_error(ctx->filename, expr.state, "eval error", "field type mismatch [%.*s = %.*s]\n", f_type.len, f_type.data, v_type.len, v_type.data);
        }

        ns_type t = field->val.type;
        if (ns_type_is_number(t)) {
            switch (t.type)
            {
            case NS_TYPE_U8: *(u8*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_U16: *(u16*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_U32: *(u32*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_U64: *(u64*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_I8: *(i8*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_I16: *(i16*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_I32: *(i32*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_I64: *(i64*)(data + field->val.offset) = val.i; break;
            case NS_TYPE_F32: *(f32*)(data + field->val.offset) = val.f; break;
            case NS_TYPE_F64: *(f64*)(data + field->val.offset) = val.f; break;
            default:
                break;
            }
        } else if (t.type == NS_TYPE_STRUCT) {
            memcpy(data, vm->stack + val.p, stride);
        } else if (t.type == NS_TYPE_STRING) {
            ns_error("eval error", "unimplemented string field\n");
        } else {
            ns_error("eval error", "unknown field type\n");
        }
    }

    return (ns_value){.type = {.type = NS_TYPE_STRUCT, .i = st->index}, .p = offset};
}

ns_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_symbol *val = ns_vm_find_symbol(vm, n.var_def.name.val);
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.var_def.expr]);
    val->val.val.p = val->index;
    val->val.val = v;
    return v;
}

ns_value ns_eval_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_call *call = &vm->call_stack[ns_array_length(vm->call_stack) - 1];
    ns_scope *scope = &call->scopes[ns_array_length(call->scopes) - 1];
    if (NULL == scope) ns_vm_error(ctx->filename, n.state, "vm error", "invalid local var def");
    ns_value v = ns_eval_expr(vm, ctx, ctx->nodes[n.var_def.expr]);
    ns_symbol symbol = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = n.var_def.name.val,  .val = {.val = v } };
    ns_array_push(scope->vars, symbol);
    return v;
}

ns_value ns_eval(ns_vm *vm, ns_str source, ns_str filename) {
    ns_ast_ctx ctx = {0};
    ns_ast_parse(&ctx, source, filename);
    ns_vm_parse(vm, &ctx);

    // eval global value
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol r = vm->symbols[i];
        if (r.type != NS_SYMBOL_VALUE)
            continue;
        ns_value v = r.val.val;
        if (r.val.is_const) {
            ns_symbol *record = &vm->symbols[i];
            record->val.val = v;
        }
    }

    ns_symbol* main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main"));
    if (NULL != main_fn) {
        ns_call call = (ns_call){.fn = main_fn, .args = NULL };
        ns_array_set_length(call.args, 0);
        ns_array_push(vm->call_stack, call);
        ns_eval_compound_stmt(vm, &ctx, ctx.nodes[main_fn->fn.ast.fn_def.body]);
        ns_array_pop(vm->call_stack);
        return ns_nil;
    }

    ns_value ret = ns_nil;
    for (i32 i = ctx.section_begin, l = ctx.section_end; i < l; ++i) {
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
