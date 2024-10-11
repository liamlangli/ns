#include "ns_ast.h"
#include "ns_tokenize.h"
#include "ns_type.h"
#include "ns_vm.h"

ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t, bool infer);
ns_record ns_vm_find_record(ns_vm *vm, ns_str s);

ns_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_t n);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

ns_type ns_vm_parse_record_type(ns_vm *vm, ns_str n, bool infer) {
    ns_record r = ns_vm_find_record(vm, n);
    switch (r.type) {
    case NS_RECORD_VALUE:
        return r.val.type;
    case NS_RECORD_FN:
        return (ns_type){.type = NS_TYPE_FN, .name = r.name};
    case NS_RECORD_STRUCT:
        return (ns_type){.type = NS_TYPE_STRUCT, .name = r.name};
    default:
        if (infer)
            return ns_type_infer;
        if (n.len == 0) {
            ns_error("syntax error", "missing type\n");
        } else {
            ns_error("syntax error", "unknown type [%.*s]\n", n.len, n.data);
        }
        break;
    }
}

ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t, bool infer) {
    switch (t.type) {
    case NS_TOKEN_TYPE_INT8:
        return (ns_type){.type = NS_TYPE_I8, .name = ns_str_cstr("i8")};
    case NS_TOKEN_TYPE_UINT8:
        return (ns_type){.type = NS_TYPE_U8, .name = ns_str_cstr("u8")};
    case NS_TOKEN_TYPE_INT16:
        return (ns_type){.type = NS_TYPE_I16, .name = ns_str_cstr("i16")};
    case NS_TOKEN_TYPE_UINT16:
        return (ns_type){.type = NS_TYPE_U16, .name = ns_str_cstr("u16")};
    case NS_TOKEN_TYPE_INT32:
        return (ns_type){.type = NS_TYPE_I32, .name = ns_str_cstr("i32")};
    case NS_TOKEN_TYPE_UINT32:
        return (ns_type){.type = NS_TYPE_U32, .name = ns_str_cstr("u32")};
    case NS_TOKEN_TYPE_INT64:
        return (ns_type){.type = NS_TYPE_I64, .name = ns_str_cstr("i64")};
    case NS_TOKEN_TYPE_UINT64:
        return (ns_type){.type = NS_TYPE_U64, .name = ns_str_cstr("u64")};
    case NS_TOKEN_TYPE_F32:
        return (ns_type){.type = NS_TYPE_F32, .name = ns_str_cstr("f32")};
    case NS_TOKEN_TYPE_F64:
        return (ns_type){.type = NS_TYPE_F64, .name = ns_str_cstr("f64")};
    default:
        break;
    }
    return ns_vm_parse_record_type(vm, t.val, infer);
}

ns_record ns_vm_find_record(ns_vm *vm, ns_str s) {
    if (vm->fn) {
        for (int i = 0, l = ns_array_length(vm->fn->fn.args); i < l; ++i) {
            if (ns_str_equals(vm->fn->fn.args[i].name, s)) {
                return vm->fn->fn.args[i];
            }
        }
    }

    for (int i = 0, l = ns_array_length(vm->records); i < l; i++) {
        if (ns_str_equals(vm->records[i].name, s)) {
            return vm->records[i];
        }
    }
    return (ns_record){.type = NS_RECORD_INVALID};
}

int ns_vm_push_record(ns_vm *vm, ns_record r) {
    r.index = ns_array_length(vm->records);
    ns_array_push(vm->records, r);
    return r.index;
}

void ns_vm_parse_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx) {
    for (int i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        int s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        if (n.type != NS_AST_FN_DEF)
            continue;
        ns_record fn = (ns_record){.type = NS_RECORD_FN};
        fn.name = n.fn_def.name.val;
        fn.fn.ast = s;
        ns_vm_push_record(vm, fn);
    }
}

void ns_vm_parse_fn_def_type(ns_vm *vm, ns_ast_ctx *ctx) {
    for (int i = 0, l = ns_array_length(vm->records); i < l; ++i) {
        ns_record *fn = &vm->records[i];
        if (fn->type != NS_RECORD_FN)
            continue;
        fn->fn.ret = ns_vm_parse_type(vm, ctx->nodes[fn->fn.ast].fn_def.return_type, false);
        ns_ast_t n = ctx->nodes[fn->fn.ast];
        ns_array_set_length(fn->fn.args, n.fn_def.arg_count);
        ns_ast_t *arg = &n;
        for (int i = 0; i < n.fn_def.arg_count; i++) {
            arg = &ctx->nodes[arg->next];
            ns_record arg_record = (ns_record){.type = NS_RECORD_VALUE, .index = i};
            arg_record.name = arg->arg.name.val;
            arg_record.val.type = ns_vm_parse_type(vm, arg->arg.type, false);
            fn->fn.args[i] = arg_record;
        }
    }
}

ns_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_t n) {
    switch (n.primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
    case NS_TOKEN_FLT_LITERAL:
        return (ns_type){.type = NS_TYPE_F64};
    case NS_TOKEN_STR_LITERAL:
        return (ns_type){.type = NS_TYPE_STRING};
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
        return (ns_type){.type = NS_TYPE_BOOL};
    case NS_TOKEN_IDENTIFIER:
        return ns_vm_parse_record_type(vm, n.primary_expr.token.val, true);
    default:
        break;
    }
    return ns_type_unknown;
}

ns_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_type fn = ns_vm_parse_primary_expr(vm, ctx->nodes[n.call_expr.callee]);
    if (fn.type!= NS_TYPE_UNKNOWN) {
        ns_parse_error(ctx, "syntax error", "unknown callee\n");
    }

    ns_record fn_record = ns_vm_find_record(vm, fn.name);
    ns_ast_t arg = ctx->nodes[n.call_expr.arg_count];
    for (int i = 0, l = n.call_expr.arg_count; i < l; ++i) {
        arg = ctx->nodes[arg.next];
        ns_type t = ns_vm_parse_expr(vm, ctx, n);
        if (t.type != fn_record.fn.args[i].val.type.type) {
            ns_parse_error(ctx, "type error", "call expr type mismatch\n");
        }
    }
    return fn_record.fn.ret;
}

ns_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_type left = ns_vm_parse_expr(vm, ctx, ctx->nodes[n.binary_expr.left]);
    ns_type right = ns_vm_parse_expr(vm, ctx, ctx->nodes[n.binary_expr.right]);
    if (left.type != right.type) {
        ns_parse_error(ctx, "type error", "binary expr type mismatch\n");
    }
    return left;
}

ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.type) {
    case NS_AST_BINARY_EXPR:
        return ns_vm_parse_binary_expr(vm, ctx, n);
    case NS_AST_PRIMARY_EXPR:
        return ns_vm_parse_primary_expr(vm, n);
    case NS_AST_CALL_EXPR:
        return ns_vm_parse_call_expr(vm, ctx, n);
    default: {
        ns_str type = ns_ast_type_to_string(n.type);
        ns_warn("vm parse", "unimplemented expr type %.*s\n", type.len, type.data);
    } break;
    }
    return ns_type_unknown;
}

void ns_vm_parse_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.jump_stmt.label.type) {
    case NS_TOKEN_RETURN: {
        ns_ast_t expr = ctx->nodes[n.jump_stmt.expr];
        ns_type t = ns_vm_parse_expr(vm, ctx, expr);
        if (vm->fn->fn.ret.type != t.type) {
            ns_parse_error(ctx, "type error", "return type mismatch\n");
        }
    } break;
    default: {
        ns_str l = n.jump_stmt.label.val;
        ns_warn("vm parse", "unknown jump stmt type %.*s\n", l.len, l.data);
    } break;
    }
}

void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, int i) {
    ns_ast_t expr = ctx->nodes[i];
    for (int i = 0, l = expr.compound_stmt.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        switch (expr.type) {
        case NS_AST_JUMP_STMT:
            ns_vm_parse_jump_stmt(vm, ctx, expr);
            break;
        default: {
            ns_str type = ns_ast_type_to_string(expr.type);
            ns_warn("vm parse", "unimplemented stmt type %.*s*\n", type.len, type.data);
        } break;
        }
    }
}

void ns_vm_parse_fn_def_body(ns_vm *vm, ns_ast_ctx *ctx) {
    for (int i = 0, l = ns_array_length(vm->records); i < l; ++i) {
        ns_record *fn = &vm->records[i];
        if (fn->type != NS_RECORD_FN)
            continue;
        ns_ast_t n = ctx->nodes[fn->fn.ast];
        fn->fn.ast = n.fn_def.body;
        vm->fn = fn;
        ns_vm_parse_compound_stmt(vm, ctx, fn->fn.ast);
        vm->fn = NULL;
    }
}

void ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx) {
    for (int i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_STRUCT_DEF)
            continue;
        ns_record st = (ns_record){.type = NS_RECORD_STRUCT, .st = {.ast = i}};
        st.name = n.struct_def.name.val;
        ns_array_set_length(st.st.fields, n.struct_def.count);
        ns_ast_t *field = &n;
        for (int i = 0; i < n.struct_def.count; i++) {
            field = &ctx->nodes[field->next];
            ns_record f = (ns_record){.type = NS_RECORD_VALUE, .index = i, .val = {.type = ns_type_unknown}};
            f.name = field->arg.name.val;
            f.val.is_ref = field->arg.is_ref;
            f.val.is_const = false;
            f.val.scope = NS_SCOPE_FIELD;
            st.st.fields[i] = f;
            if (field->arg.is_ref)
                f.val.type = ns_vm_parse_type(vm, field->arg.type, false);
        }
        ns_vm_push_record(vm, st);
    }
}

// parse struct def ref field, ref can be struct or fn or generic type
void ns_vm_parse_struct_def_ref(ns_vm *vm) {
    for (int i = 0, l = ns_array_length(vm->records); i < l; ++i) {
        ns_record *st = &vm->records[i];
        if (st->type != NS_RECORD_STRUCT)
            continue;
        for (int j = 0, l = ns_array_length(st->st.fields); j < l; ++j) {
            ns_record *f = &st->st.fields[j];
            if (!f->val.is_ref)
                continue;
            ns_record t = ns_vm_find_record(vm, f->val.type.name);
            if (t.type == NS_RECORD_INVALID) {
                ns_error("syntax error", "unknow ref type %.*s.\n", f->val.type.name.len, f->val.type.name.data);
            }
        }
    }
}

void ns_vm_parse_var_def(ns_vm *vm, ns_ast_ctx *ctx) {
    for (int i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_VAR_DEF)
            continue;
        ns_record r = (ns_record){.type = NS_RECORD_VALUE};
        r.name = n.var_def.name.val;
        r.val.type = ns_vm_parse_type(vm, n.var_def.type, true);
        ns_vm_push_record(vm, r);
    }
}

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_vm_parse_fn_def_name(vm, ctx);
    ns_vm_parse_struct_def(vm, ctx);
    ns_vm_parse_struct_def_ref(vm);
    ns_vm_parse_fn_def_type(vm, ctx);
    ns_vm_parse_var_def(vm, ctx);
    ns_vm_parse_fn_def_body(vm, ctx);
    for (int i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type == NS_AST_FN_DEF || n.type == NS_AST_STRUCT_DEF || n.type == NS_AST_VAR_DEF)
            continue;
        ns_vm_parse_expr(vm, ctx, n);
    }
    return true;
}