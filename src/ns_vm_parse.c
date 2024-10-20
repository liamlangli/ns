#include "ns_ast.h"
#include "ns_tokenize.h"
#include "ns_type.h"
#include "ns_vm.h"

#define ns_vm_warn(n, t, m, ...) ns_warn("[%s]")
#define ns_vm_error(f, s, t, m, ...) ns_error(t, "\n[%.*s:%d:%d]: " m "\n", f.len, f.data, s.l, s.o, ##__VA_ARGS__)

ns_type ns_vm_parse_record_type(ns_vm *vm, ns_str n, bool infer);
ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t, bool infer);
ns_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_t n);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);
ns_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n);

void ns_vm_parse_import_stmt(ns_vm *vm, ns_ast_ctx *ctx);
void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

i32 ns_vm_push_symbol(ns_vm *vm, ns_symbol r) {
    r.index = ns_array_length(vm->symbols);
    ns_array_push(vm->symbols, r);
    return r.index;
}

i32 ns_vm_push_string(ns_vm *vm, ns_str s) {
    i32 i = ns_array_length(vm->str_list);
    ns_array_push(vm->str_list, s);
    return i;
}

i32 ns_vm_push_data(ns_vm *vm, ns_data d) {
    i32 i = ns_array_length(vm->data_list);
    ns_array_push(vm->data_list, d);
    return i;
}

ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t) {
    switch (t.type)
    {
    case NS_TYPE_I8: return ns_str_cstr("i8");
    case NS_TYPE_U8: return ns_str_cstr("u8");
    case NS_TYPE_I16: return ns_str_cstr("i16");
    case NS_TYPE_U16: return ns_str_cstr("u16");
    case NS_TYPE_I32: return ns_str_cstr("i32");
    case NS_TYPE_U32: return ns_str_cstr("u32");
    case NS_TYPE_I64: return ns_str_cstr("i64");
    case NS_TYPE_U64: return ns_str_cstr("u64");
    case NS_TYPE_F32: return ns_str_cstr("f32");
    case NS_TYPE_F64: return ns_str_cstr("f64");
    case NS_TYPE_BOOL: return ns_str_cstr("bool");
    case NS_TYPE_STRING: return ns_str_cstr("str");
    case NS_TYPE_FN:
    case NS_TYPE_STRUCT: {
        ns_symbol *r = &vm->symbols[t.i];
        if (!r) ns_error("syntax error", "missing type %d\n", t.i);
        return r->name;
    } break;
    default:
        break;
    }
    return ns_str_null;
}

ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s) {
    size_t l = ns_array_length(vm->call_symbols);
    if (l > 0) {
        ns_fn_call_record *call = &vm->call_symbols[l - 1].call;
        ns_fn_record *fn = &call->fn->fn;
        for (i32 i = 0, l = ns_array_length(fn->args); i < l; ++i) {
            if (ns_str_equals(fn->args[i].name, s)) {
                return &fn->args[i];
            }
        }

        for (i32 i = 0, l = ns_array_length(call->locals); i < l; ++i) {
            if (ns_str_equals(call->locals[i].name, s)) {
                return &call->locals[i];
            }
        }
    }

    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; i++) {
        if (ns_str_equals(vm->symbols[i].name, s)) {
            return &vm->symbols[i];
        }
    }
    return NULL;
}

ns_type ns_vm_parse_record_type(ns_vm *vm, ns_str n, bool infer) {
    ns_symbol *r = ns_vm_find_symbol(vm, n);
    if (!r) {
        if (infer) return ns_type_infer;
        ns_error("syntax error", "missing type %.*s\n", n.len, n.data);
    }

    switch (r->type) {
    case NS_SYMBOL_VALUE:
        return r->val.type;
    case NS_SYMBOL_FN:
        return (ns_type){.type = NS_TYPE_FN, .i = r->index};
    case NS_SYMBOL_STRUCT:
        return (ns_type){.type = NS_TYPE_STRUCT, .i = r->index};
    default:
        ns_error("syntax error", "unknown type [%.*s]\n", n.len, n.data);
        break;
    }
}

ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t, bool infer) {
    switch (t.type) {
    case NS_TOKEN_TYPE_I8: return ns_type_i8;
    case NS_TOKEN_TYPE_U8: return ns_type_u8;
    case NS_TOKEN_TYPE_I16: return ns_type_i16;
    case NS_TOKEN_TYPE_U16: return ns_type_u16;
    case NS_TOKEN_TYPE_I32: return ns_type_i32;
    case NS_TOKEN_TYPE_U32: return ns_type_u32;
    case NS_TOKEN_TYPE_I64: return ns_type_i64;
    case NS_TOKEN_TYPE_U64: return ns_type_u64;
    case NS_TOKEN_TYPE_F32: return ns_type_f32;
    case NS_TOKEN_TYPE_F64: return ns_type_f64;
    default:
        break;
    }
    return ns_vm_parse_record_type(vm, t.val, infer);
}

void ns_vm_parse_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        if (n.type != NS_AST_FN_DEF)
            continue;
        ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = n}};
        fn.name = n.fn_def.name.val;
        ns_vm_push_symbol(vm, fn);
    }
}

void ns_vm_parse_fn_def_type(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *fn = &vm->symbols[i];
        if (fn->type != NS_SYMBOL_FN || fn->parsed)
            continue;
        ns_ast_t n = fn->fn.ast;
        fn->fn.ret = ns_vm_parse_type(vm, n.fn_def.return_type, true);
        ns_array_set_length(fn->fn.args, n.fn_def.arg_count);
        ns_ast_t *arg = &n;
        for (i32 i = 0; i < n.fn_def.arg_count; i++) {
            arg = &ctx->nodes[arg->next];
            ns_symbol arg_record = (ns_symbol){.type = NS_SYMBOL_VALUE, .index = i};
            arg_record.name = arg->arg.name.val;
            arg_record.val.type = ns_vm_parse_type(vm, arg->arg.type, false);
            fn->fn.args[i] = arg_record;
        }
    }
}

void ns_vm_parse_fn_def_body(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *fn = &vm->symbols[i];
        if (fn->type != NS_SYMBOL_FN || fn->parsed)
            continue;
        ns_ast_t n = fn->fn.ast;
        ns_symbol call = (ns_symbol){.type = NS_SYMBOL_FN_CALL};
        call.call.fn = fn;
        ns_array_push(vm->call_symbols, call);
        ns_vm_parse_compound_stmt(vm, ctx, n.fn_def.body);
        ns_array_pop(vm->call_symbols);
        fn->fn.fn = (ns_value){.p = i, .type = (ns_type){.type = NS_TYPE_FN, .i = i}};
        fn->parsed = true;
    }
}

void ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_STRUCT_DEF)
            continue;
        ns_symbol st = (ns_symbol){.type = NS_SYMBOL_STRUCT, .st = {.ast = n}};
        st.name = n.struct_def.name.val;
        ns_array_set_length(st.st.fields, n.struct_def.count);
        ns_ast_t *field = &n;
        for (i32 i = 0; i < n.struct_def.count; i++) {
            field = &ctx->nodes[field->next];
            ns_symbol f = (ns_symbol){.type = NS_SYMBOL_VALUE, .index = i, .val = {.type = ns_type_unknown}};
            f.name = field->arg.name.val;
            f.val.is_ref = field->arg.is_ref;
            f.val.is_const = false;
            f.val.scope = NS_SCOPE_FIELD;
            st.st.fields[i] = f;
            // if (field->arg.is_ref)
                // f.val.type = ns_vm_parse_type(vm, field->arg.type, false);
        }
        ns_vm_push_symbol(vm, st);
    }
}

void ns_vm_parse_struct_def_ref(ns_vm *vm) {
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *st = &vm->symbols[i];
        if (st->type != NS_SYMBOL_STRUCT || st->parsed)
            continue;
        for (i32 j = 0, l = ns_array_length(st->st.fields); j < l; ++j) {
            ns_symbol *f = &st->st.fields[j];
            if (!f->val.is_ref)
                continue;
            ns_str n = ns_vm_get_type_name(vm, f->val.type);
            ns_symbol *t = ns_vm_find_symbol(vm, n);
            if (t->type == NS_SYMBOL_INVALID) {
                ns_error("syntax error", "unknow ref type %.*s.\n", n.len, n.data);
            }
        }
        st->parsed = true;
    }
}

void ns_vm_parse_var_def(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_VAR_DEF)
            continue;
        ns_symbol r = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = { .scope = NS_SCOPE_GLOBAL }, .parsed = true};
        r.name = n.var_def.name.val;
        r.val.type = ns_vm_parse_type(vm, n.var_def.type, true);
        ns_vm_push_symbol(vm, r);
    }
}

ns_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_t n) {
    switch (n.primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return (ns_type){.type = NS_TYPE_I64};
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
    ns_ast_t callee_n = ctx->nodes[n.call_expr.callee];
    ns_type fn = ns_vm_parse_primary_expr(vm, callee_n);
    if (fn.type == NS_TYPE_UNKNOWN) {
        ns_vm_error(ctx->filename, callee_n.state, "syntax error", "unknown callee");
    }

    ns_symbol *fn_record = ns_vm_find_symbol(vm, ns_vm_get_type_name(vm, fn));
    if (!fn_record || fn_record->type != NS_SYMBOL_FN) {
        ns_vm_error(ctx->filename, callee_n.state, "syntax error", "invalid callee");
    }

    ns_ast_t arg = n;
    for (i32 i = 0, l = n.call_expr.arg_count; i < l; ++i) {
        arg = ctx->nodes[arg.next];
        ns_type t = ns_vm_parse_expr(vm, ctx, arg);
        if (t.type != fn_record->fn.args[i].val.type.type) {
            ns_ast_error(ctx, "type error", "call expr type mismatch\n");
        }
    }
    return fn_record->fn.ret;
}

void ns_vm_parse_import_stmt(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_IMPORT_STMT)
            continue;
        ns_str lib = n.import_stmt.lib.val;
        if (ns_str_equals(lib, ns_str_cstr("std"))) {
            ns_vm_import_std_symbols(vm);
        } else {
            // find lib in lib path & then add all symbols to vm
            ns_error("syntax error", "unknown lib %.*s\n", lib.len, lib.data);
        }
    }
}

ns_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_type left = ns_vm_parse_expr(vm, ctx, ctx->nodes[n.binary_expr.left]);
    ns_type right = ns_vm_parse_expr(vm, ctx, ctx->nodes[n.binary_expr.right]);
    if (left.type != right.type) {
        ns_ast_error(ctx, "type error", "binary expr type mismatch\n");
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
        ns_vm_error(ctx->filename, n.state, "syntax error", "unimplemented expr type %.*s", type.len, type.data);
    } break;
    }
    return ns_type_unknown;
}

void ns_vm_parse_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    switch (n.jump_stmt.label.type) {
    case NS_TOKEN_RETURN: {
        ns_ast_t expr = ctx->nodes[n.jump_stmt.expr];
        ns_type t = ns_vm_parse_expr(vm, ctx, expr);
        size_t l = ns_array_length(vm->call_symbols);
        if (l == 0) {
            ns_ast_error(ctx, "syntax error", "return stmt not in fn\n");
        }
        ns_symbol *fn = vm->call_symbols[l - 1].call.fn;
        if (fn->fn.ret.type != t.type) {
            ns_ast_error(ctx, "type error", "return type mismatch\n");
        }
    } break;
    default: {
        ns_str l = n.jump_stmt.label.val;
        ns_error("vm parse", "unknown jump stmt type %.*s\n", l.len, l.data);
    } break;
    }
}

void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t expr = ctx->nodes[i];
    for (i32 i = 0, l = expr.compound_stmt.count; i < l; i++) {
        expr = ctx->nodes[expr.next];
        switch (expr.type) {
        case NS_AST_JUMP_STMT:
            ns_vm_parse_jump_stmt(vm, ctx, expr);
            break;
        default: {
            ns_str type = ns_ast_type_to_string(expr.type);
            ns_error("vm parse", "unimplemented stmt type %.*s*\n", type.len, type.data);
        } break;
        }
    }
}

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_vm_parse_import_stmt(vm, ctx);

    ns_vm_parse_fn_def_name(vm, ctx);

    ns_vm_parse_struct_def(vm, ctx);
    ns_vm_parse_struct_def_ref(vm);

    ns_vm_parse_fn_def_type(vm, ctx);
    ns_vm_parse_var_def(vm, ctx);
    ns_vm_parse_fn_def_body(vm, ctx);

    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        switch (n.type) {
        case NS_AST_EXPR:
        case NS_AST_CALL_EXPR:
            ns_vm_parse_expr(vm, ctx, n);
            break;
        case NS_AST_FN_DEF:
        case NS_AST_OPS_FN_DEF:
        case NS_AST_STRUCT_DEF:
        case NS_AST_VAR_DEF:
            break; // already parsed
        default: {
            ns_str type = ns_ast_type_to_string(n.type);
            if (vm->mode != NS_VM_MODE_REPL) ns_warn("vm parse", "unimplemented global ast parse %.*s\n", type.len, type.data);
        } break;
        }
    }
    return true;
}