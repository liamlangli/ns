#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_path.h"

void ns_vm_parse_import_stmt(ns_vm *vm, ns_ast_ctx *ctx);

void ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx);
void ns_vm_parse_struct_def_ref(ns_vm *vm);

void ns_vm_parse_ops_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx);
void ns_vm_parse_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx);
void ns_vm_parse_fn_def_type(ns_vm *vm, ns_ast_ctx *ctx);
void ns_vm_parse_fn_def_body(ns_vm *vm, ns_ast_ctx *ctx);

void ns_vm_parse_var_def(ns_vm *vm, ns_ast_ctx *ctx);

ns_type ns_vm_parse_record_type(ns_vm *vm, ns_str n, bool infer);
ns_type ns_vm_parse_type(ns_vm *vm, ns_token_t t, bool infer);
ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_str_fmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_gen_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_type ns_vm_parse_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

void ns_vm_parse_import_stmt(ns_vm *vm, ns_ast_ctx *ctx);
void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
void ns_vm_parse_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
void ns_vm_parse_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
void ns_vm_parse_loop_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
void ns_vm_parse_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

ns_type ns_vm_number_type_upgrade(ns_type l, ns_type r) {
    ns_number_type ln = ns_vm_number_type(l);
    ns_number_type rn = ns_vm_number_type(r);
    if (ln == rn) return l;
    switch (ln | rn)
    {
    case NS_NUMBER_FLT_AND_I:
    case NS_NUMBER_FLT_AND_U:
        if (ns_min(l.type, r.type) >= NS_TYPE_I64) return ns_type_unknown;
        return ns_type_f64;
    default: break;
    }
    return ns_type_unknown;
}

u64 ns_type_size(ns_vm *vm, ns_type t) {
    switch (t.type)
    {
    case NS_TYPE_BOOL:
    case NS_TYPE_I8:
    case NS_TYPE_U8: return 1;
    case NS_TYPE_I16:
    case NS_TYPE_U16: return 2;
    case NS_TYPE_I32:
    case NS_TYPE_U32:
    case NS_TYPE_F32: return 4;
    case NS_TYPE_I64:
    case NS_TYPE_U64:
    case NS_TYPE_F64: return 8;
    case NS_TYPE_ARRAY:
    case NS_TYPE_FN:
    case NS_TYPE_STRING: return ns_ptr_size;
    case NS_TYPE_STRUCT: {
        u64 ti = ns_type_index(t);
        ns_symbol *s = &vm->symbols[ti];
        if (ns_null == s) ns_error("eval error", "missing struct %lu\n", ti);
        return s->st.stride;
    }
    default:
        break;
    }
    ns_error("eval error", "unknown type %d\n", (i32)t.type);
    return 0;
}

i32 ns_vm_push_symbol_global(ns_vm *vm, ns_symbol r) {
    i32 i = ns_array_length(vm->symbols);
    ns_array_push(vm->symbols, r);
    return i;
}

i32 ns_vm_push_symbol_local(ns_vm *vm, ns_symbol r) {
    i32 i = ns_array_length(vm->symbol_stack);
    ns_array_push(vm->symbol_stack, r);
    return i;
}

i32 ns_vm_push_string(ns_vm *vm, ns_str s) {
    i32 i = ns_array_length(vm->str_list);
    ns_str str = ns_str_slice(s, 0, s.len);
    ns_array_push(vm->str_list, str);
    return i;
}

i32 ns_vm_push_data(ns_vm *vm, ns_data d) {
    i32 i = ns_array_length(vm->data_list);
    ns_array_push(vm->data_list, d);
    return i;
}

ns_str ns_ops_name(ns_token_t op) {
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        if (ns_str_equals_STR(op.val, "+"))
            return ns_str_cstr("add");
        else
            return ns_str_cstr("sub");
    case NS_TOKEN_MUL_OP:
        if (ns_str_equals_STR(op.val, "*"))
            return ns_str_cstr("mul");
        else if (ns_str_equals_STR(op.val, "/"))
            return ns_str_cstr("div");
        else
            return ns_str_cstr("mod");
    case NS_TOKEN_LOGIC_OP:
        if (ns_str_equals_STR(op.val, "&&"))
            return ns_str_cstr("and");
        else
            return ns_str_cstr("or");
    case NS_TOKEN_SHIFT_OP:
        if (ns_str_equals_STR(op.val, "<<"))
            return ns_str_cstr("shl");
        else
            return ns_str_cstr("shr");
    case NS_TOKEN_CMP_OP:
        if (ns_str_equals_STR(op.val, "=="))
            return ns_str_cstr("eq");
        else if (ns_str_equals_STR(op.val, "!="))
            return ns_str_cstr("ne");
        else if (ns_str_equals_STR(op.val, "<"))
            return ns_str_cstr("lt");
        else if (ns_str_equals_STR(op.val, "<="))
            return ns_str_cstr("le");
        else if (ns_str_equals_STR(op.val, ">"))
            return ns_str_cstr("gt");
        else if (ns_str_equals_STR(op.val, ">="))
            return ns_str_cstr("ge");
    default:
        ns_error("eval error", "unsupported ops override %.*s\n", op.val.len, op.val.data);
    }
}

ns_str ns_ops_override_name(ns_str l, ns_str r, ns_token_t op) {
    ns_str op_name = ns_ops_name(op);
    size_t len = l.len + r.len + op_name.len + 3;
    i8* data = (i8*)malloc(len);
    snprintf(data, len, "%.*s_%.*s_%.*s", l.len, l.data, op_name.len, op_name.data, r.len, r.data);
    data[len - 1] = '\0';
    return (ns_str){.data = data, .len = len - 1, .dynamic = 1};
}

ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t) {
    bool is_ref = ns_type_is_ref(t);
    switch (t.type)
    {
    case NS_TYPE_I8: return is_ref ? ns_str_cstr("ref_i8") : ns_str_cstr("i8");
    case NS_TYPE_U8: return is_ref ? ns_str_cstr("ref_u8") : ns_str_cstr("u8");
    case NS_TYPE_I16: return is_ref ? ns_str_cstr("ref_i16") : ns_str_cstr("i16");
    case NS_TYPE_U16: return is_ref ? ns_str_cstr("ref_u16") : ns_str_cstr("u16");
    case NS_TYPE_I32: return is_ref ? ns_str_cstr("ref_i32") : ns_str_cstr("i32");
    case NS_TYPE_U32: return is_ref ? ns_str_cstr("ref_u32") : ns_str_cstr("u32");
    case NS_TYPE_I64: return is_ref ? ns_str_cstr("ref_i64") : ns_str_cstr("i64");
    case NS_TYPE_U64: return is_ref ? ns_str_cstr("ref_u64") : ns_str_cstr("u64");
    case NS_TYPE_F32: return is_ref ? ns_str_cstr("ref_f32") : ns_str_cstr("f32");
    case NS_TYPE_F64: return is_ref ? ns_str_cstr("ref_f64") : ns_str_cstr("f64");
    case NS_TYPE_BOOL: return is_ref ? ns_str_cstr("ref_bool") : ns_str_cstr("bool");
    case NS_TYPE_STRING: return ns_str_cstr("str");
    case NS_TYPE_FN:
    case NS_TYPE_STRUCT: {
        u64 ti = ns_type_index(t);
        if (ti > ns_array_length(vm->symbols)) {
            ns_error("eval error", "missing type %lu\n", ti);
        }

        ns_symbol *r = &vm->symbols[ti];
        if (!r) ns_error("syntax error", "missing type %lu\n", ti);
        return r->name;
    } break;
    default:
        return ns_str_cstr("unknown");
        break;
    }
    return ns_str_null;
}

ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s) {
    size_t l = ns_array_length(vm->call_stack);
    if (l > 0) {
        ns_call *call = ns_array_last(vm->call_stack);
        i32 scope_top = call->scope_top;
        i32 symbol_top = vm->scope_stack[scope_top].symbol_top;
        i32 symbol_count = ns_array_length(vm->symbol_stack);

        for (i32 j = symbol_count - 1; j >= symbol_top; --j) {
            if (ns_str_equals(vm->symbol_stack[j].name, s)) {
                return &vm->symbol_stack[j];
            }
        }
    }

    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; i++) {
        if (ns_str_equals(vm->symbols[i].name, s)) {
            return &vm->symbols[i];
        }
    }
    return ns_null;
}

ns_type ns_vm_parse_record_type(ns_vm *vm, ns_str n, bool infer) {
    ns_symbol *r = ns_vm_find_symbol(vm, n);
    if (!r) {
        if (infer) return ns_type_infer;
        ns_error("syntax error", "missing type %.*s\n", n.len, n.data);
    }

    switch (r->type) {
    case NS_SYMBOL_VALUE:
        return r->val.t;
    case NS_SYMBOL_FN:
        return r->fn.fn.t;
    case NS_SYMBOL_STRUCT:
        return r->st.st.t;
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
    case NS_TOKEN_TYPE_STR: return ns_type_str;
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
        ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = n}, .lib =  vm->lib };
        fn.name = n.fn_def.name.val;
        ns_vm_push_symbol_global(vm, fn);
    }
}

void ns_vm_parse_ops_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        if (n.type != NS_AST_OPS_FN_DEF)
            continue;
        ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = n}, .lib =  vm->lib};
        ns_ast_t l = ctx->nodes[n.ops_fn_def.left];
        ns_ast_t r = ctx->nodes[n.ops_fn_def.right];

        ns_str l_type = ctx->nodes[l.arg.type].type_label.name.val;
        ns_str r_type = ctx->nodes[r.arg.type].type_label.name.val;

        fn.name = ns_ops_override_name(l_type, r_type, n.ops_fn_def.ops);
        ns_vm_push_symbol_global(vm, fn);
    }
}

void ns_vm_parse_fn_def_type(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = vm->symbol_top, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *fn = &vm->symbols[i];
        if (fn->type != NS_SYMBOL_FN || fn->parsed)
            continue;
        ns_ast_t n = fn->fn.ast;
        if (n.type == NS_AST_FN_DEF) {
            ns_ast_t *ret_type = &ctx->nodes[n.fn_def.ret];
            fn->fn.ret = ns_vm_parse_type(vm, ret_type->type_label.name, true);
            ns_array_set_length(fn->fn.args, n.fn_def.arg_count);
            ns_ast_t *arg = &n;
            for (i32 i = 0; i < n.fn_def.arg_count; i++) {
                arg = &ctx->nodes[arg->next];
                ns_symbol arg_record = (ns_symbol){.type = NS_SYMBOL_VALUE};
                arg_record.name = arg->arg.name.val;

                ns_ast_t *arg_type = &ctx->nodes[arg->arg.type];
                arg_record.val.t = ns_vm_parse_type(vm, arg_type->type_label.name, false);
                fn->fn.args[i] = arg_record;
            }
        } else if (n.type == NS_AST_OPS_FN_DEF) {
            ns_ast_t l = ctx->nodes[n.ops_fn_def.left];
            ns_ast_t r = ctx->nodes[n.ops_fn_def.right];
            fn->fn.ret = ns_vm_parse_type(vm, ctx->nodes[l.arg.type].type_label.name, true);
            ns_array_set_length(fn->fn.args, 2);
            ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE};
            l_arg.name = l.arg.name.val;
            l_arg.val.t = ns_vm_parse_type(vm, ctx->nodes[l.arg.type].type_label.name, false);
            fn->fn.args[0] = l_arg;
            ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE};
            r_arg.name = r.arg.name.val;
            r_arg.val.t = ns_vm_parse_type(vm, ctx->nodes[r.arg.type].type_label.name, false);
            fn->fn.args[1] = r_arg;
        } else {
            ns_error("syntax error", "unknown fn def type\n");
        }
    }
}

void ns_vm_parse_fn_def_body(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *fn = &vm->symbols[i];
        if (fn->type != NS_SYMBOL_FN || fn->parsed)
            continue;
        ns_ast_t n = fn->fn.ast;
        i32 body = n.type == NS_AST_FN_DEF ? n.fn_def.body : n.ops_fn_def.body;
        bool is_ref = n.type == NS_AST_FN_DEF ? n.fn_def.is_ref : n.ops_fn_def.is_ref;
        ns_call call = (ns_call){.fn = fn, .scope_top = ns_array_length(vm->scope_stack)};

        ns_array_push(vm->call_stack, call);
        ns_enter_scope(vm);

        for (i32 j = 0, l = ns_array_length(fn->fn.args); j < l; ++j) {
            ns_symbol *arg = &fn->fn.args[j];
            ns_vm_push_symbol_local(vm, *arg);
        }

        ns_vm_parse_compound_stmt(vm, ctx, body);
        ns_exit_scope(vm);
        ns_array_pop(vm->call_stack);
        fn->fn.fn = (ns_value){.t = ns_type_encode(ns_type_fn, i, is_ref, 0) };
        fn->parsed = true;
    }
}

void ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx) {
    i32 size;
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_STRUCT_DEF)
            continue;
        i32 i = ns_array_length(vm->symbols);
        ns_symbol st = (ns_symbol){.type = NS_SYMBOL_STRUCT, .st = {.ast = n, .st.t = ns_type_encode(ns_type_struct, i, 0, 0)}, .lib =  vm->lib};
        st.name = n.struct_def.name.val;
        ns_array_set_length(st.st.fields, n.struct_def.count);
        ns_ast_t *field = &n;
        i32 offset = 0;
        for (i32 i = 0; i < n.struct_def.count; i++) {
            field = &ctx->nodes[field->next];
            ns_ast_t *type = &ctx->nodes[field->arg.type];
            ns_type t;
            if (type->type_label.is_array) {
                size = ns_ptr_size;
                ns_type item_type = ns_vm_parse_type(vm, type->type_label.name, true);
                t = (ns_type){.type = item_type.type, .ref = type->type_label.is_ref, .array = true, .store = NS_STORE_HEAP};
            } else {
                t = ns_vm_parse_type(vm, type->type_label.name, true);
                size = ns_type_size(vm, t);
            }
            
            // std layout
            offset = ns_align(offset, size);
            ns_struct_field f = (ns_struct_field){.name = field->arg.name.val, .t = t, .o = offset, .s = size};
            offset += size;
            st.st.fields[i] = f;
        }
        st.st.stride = offset; // 4 bytes align
        ns_vm_push_symbol_global(vm, st);
    }
}

void ns_vm_parse_struct_def_ref(ns_vm *vm) {
    for (i32 i = vm->symbol_top, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *st = &vm->symbols[i];
        if (st->type != NS_SYMBOL_STRUCT || st->parsed)
            continue;
        for (i32 j = 0, l = ns_array_length(st->st.fields); j < l; ++j) {
            ns_struct_field *f = &st->st.fields[j];
            if (!ns_type_is_ref(f->t) || ns_type_is_array(f->t))
                continue;
            ns_str n = ns_vm_get_type_name(vm, f->t);
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
        ns_ast_t *n = &ctx->nodes[ctx->sections[i]];
        if (n->type != NS_AST_VAR_DEF)
            continue;
        ns_symbol r = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true};
        r.name = n->var_def.name.val;
        r.val.t = ns_vm_parse_type(vm, ctx->nodes[n->var_def.type].type_label.name, true);
        n->var_def.type_size = ns_type_size(vm, r.val.t);
        ns_vm_push_symbol_global(vm, r);
    }
}

ns_type ns_vm_parse_str_fmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    for (i32 j = 0, l = n->str_fmt.expr_count; j < l; ++j) {
        ns_ast_t *expr = &ctx->nodes[n->next];
        ns_vm_parse_expr(vm, ctx, expr->next);
    }
    return ns_type_str;
}

ns_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return ns_type_i32;
    case NS_TOKEN_FLT_LITERAL:
        return ns_type_f64;
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT:
        return ns_type_str;
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
        return ns_type_bool;
    case NS_TOKEN_IDENTIFIER:
        return ns_vm_parse_record_type(vm, n->primary_expr.token.val, true);
    default:
        break;
    }
    return ns_type_unknown;
}

ns_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *callee_n = &ctx->nodes[n->call_expr.callee];
    ns_type fn = ns_vm_parse_primary_expr(vm, ctx, n->call_expr.callee);
    if (ns_type_is(fn, NS_TYPE_UNKNOWN)) {
        ns_vm_error(ctx->filename, callee_n->state, "syntax error", "unknown callee.");
    }

    ns_symbol *fn_record = &vm->symbols[ns_type_index(fn)];
    if (!fn_record || fn_record->type != NS_SYMBOL_FN) {
        ns_vm_error(ctx->filename, callee_n->state, "syntax error", "invalid callee.");
    }

    i32 next = n->call_expr.arg;
    for (i32 a_i = 0, l = n->call_expr.arg_count; a_i < l; ++a_i) {
        ns_ast_t arg = ctx->nodes[next];
        ns_type t = ns_vm_parse_expr(vm, ctx, next);
        next = arg.next;
        if (!ns_type_equals(t, fn_record->fn.args[a_i].val.t)) {
            ns_str arg_type = ns_vm_get_type_name(vm, t);
            ns_str fn_arg_type = ns_vm_get_type_name(vm, fn_record->fn.args[a_i].val.t);
            ns_str fn_name = fn_record->name;
            ns_vm_error(ctx->filename, n->state, "type error", "call expr type mismatch fn [%.*s] arg [%.*s], and input arg[%.*s].", fn_name.len, fn_name.data, fn_arg_type.len, fn_arg_type.data, arg_type.len, arg_type.data);
        }
    }
    return fn_record->fn.ret;
}

ns_type ns_vm_parse_member_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type t = ns_vm_parse_expr(vm, ctx, n->member_expr.left);
    if (!ns_type_is(t, NS_TYPE_STRUCT)) {
        ns_vm_error(ctx->filename, n->state, "type error", "member expr type mismatch.");
    }

    ns_ast_t field = ctx->nodes[n->next];
    if (field.type == NS_AST_PRIMARY_EXPR) {
        ns_str name = field.primary_expr.token.val;
        ns_struct_symbol *st = &vm->symbols[ns_type_index(t)].st;
        for (i32 f_i = 0, l = ns_array_length(st->fields); f_i < l; ++f_i) {
            ns_struct_field *f = &st->fields[f_i];
            if (ns_str_equals(f->name, name)) {
                return f->t;
            }
        }
        ns_ast_error(ctx, "syntax error", "unknown member %.*s\n", name.len, name.data);
        return ns_type_unknown;
    } else {
        // recursive member expr
        return ns_vm_parse_expr(vm, ctx, n->next);
    }
}

bool ns_vm_parse_type_generable(ns_type t) {
    return ns_type_is(t, NS_TYPE_STRING) || ns_type_is(t, NS_TYPE_ARRAY);
}

ns_type ns_vm_parse_gen_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->gen_expr.range) {
        ns_type from_t = ns_vm_parse_expr(vm, ctx, n->gen_expr.from);
        ns_type to_t = ns_vm_parse_expr(vm, ctx, n->gen_expr.to);
        if (!ns_type_is(from_t, NS_TYPE_I32) || !ns_type_is(to_t, NS_TYPE_I32)) {
            ns_ast_error(ctx, "type error", "gen expr type mismatch");
        }
        return ns_type_i32;
    } else {
        ns_type from_t = ns_vm_parse_expr(vm, ctx, n->gen_expr.from);
        if (!ns_vm_parse_type_generable(from_t)) {
            ns_ast_error(ctx, "type error", "gen expr type mismatch\n");
        }
        return from_t;
    }
}

ns_type ns_vm_parse_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *st = ns_vm_find_symbol(vm, n->desig_expr.name.val);
    if (!st || st->type != NS_SYMBOL_STRUCT) {
        ns_str name = n->desig_expr.name.val;
        ns_vm_error(ctx->filename, n->state, "syntax error", "unknown struct %.*s.", name.len, name.data);
    }

    ns_ast_t *field = n;
    for (i32 f_i = 0, l = n->desig_expr.count; f_i < l; ++f_i) {
        i32 next = field->next;
        field = &ctx->nodes[next];
        ns_str name = field->field_def.name.val;
        for (i32 j = 0, l = ns_array_length(st->st.fields); j < l; ++j) {
            ns_struct_field *f = &st->st.fields[j];
            if (ns_str_equals(f->name, name)) {
                ns_type t = ns_vm_parse_expr(vm, ctx, field->field_def.expr);
                if (!ns_type_equals(t, f->t)) {
                    ns_str f_type = ns_vm_get_type_name(vm, f->t);
                    ns_str t_type = ns_vm_get_type_name(vm, t);
                    ns_vm_error(ctx->filename, field->state, "type error", "designated expr type mismatch [%.*s = %.*s].", f_type.len, f_type.data, t_type.len, t_type.data);
                }
                break;
            }
        }
    }

    return st->st.st.t;
}

ns_type ns_vm_parse_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type t = ns_vm_parse_expr(vm, ctx, n->unary_expr.expr);
    ns_token_t op = n->unary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        if (!ns_type_is_number(t)) {
            ns_ast_error(ctx, "type error", "unary expr type mismatch\n");
        }
        return t;
    case NS_TOKEN_BIT_INVERT_OP:
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            ns_ast_error(ctx, "type error", "unary expr type mismatch\n");
        }
        return t;
    default:
        ns_vm_error(ctx->filename, n->state, "syntax error", "unknown unary ops %.*s.", op.val.len, op.val.data);
    }
    return ns_type_unknown;
}

ns_type ns_vm_parse_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *type = &ctx->nodes[n->array_expr.type];
    ns_type t = ns_vm_parse_type(vm, type->type_label.name, false);
    return (ns_type){.type = t.type, .ref = t.ref, .array = true, .store = NS_STORE_HEAP};
}

ns_type ns_vm_parse_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type l = ns_vm_parse_expr(vm, ctx, n->index_expr.table);
    ns_type r = ns_vm_parse_expr(vm, ctx, n->index_expr.expr);
    if (!ns_type_is_array(l)) {
        ns_vm_error(ctx->filename, n->state, "type error", "index expr type mismatch.");
    }
    if (!ns_type_is(r, NS_TYPE_I32)) {
        ns_vm_error(ctx->filename, n->state, "type error", "index expr type mismatch.");
    }
    return (ns_type){.type = l.type, .ref = l.ref, .array = false, .store = l.ref ? NS_STORE_HEAP : NS_STORE_CONST, .index = l.index };
}

ns_type ns_vm_parse_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type t = ns_vm_parse_expr(vm, ctx, n->cast_expr.expr);
    ns_type cast = ns_vm_parse_type(vm, n->cast_expr.type, false);
    if (ns_type_equals(t, cast)) {
        return t;
    }
    if (ns_type_is_number(t) && ns_type_is_number(cast)) {
        return cast;
    }
    ns_str t_name = ns_vm_get_type_name(vm, t);
    ns_str cast_name = ns_vm_get_type_name(vm, cast);
    ns_vm_error(ctx->filename, n->state, "type error", "cast expr type mismatch [%.*s -> %.*s].", t_name.len, t_name.data, cast_name.len, cast_name.data);
    return ns_type_unknown;
}

void ns_vm_parse_import_stmt(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_IMPORT_STMT)
            continue;
        ns_str lib = n.import_stmt.lib.val;
        ns_lib_import(vm, lib);
    }
}

ns_type ns_vm_parse_binary_override(ns_vm *vm, ns_type l, ns_type r, ns_token_t op) {
    ns_str l_name = ns_vm_get_type_name(vm, l);
    ns_str r_name = ns_vm_get_type_name(vm, r);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name);
    return fn ? fn->fn.ret : ns_type_unknown;
}

ns_type ns_vm_parse_binary_ops_number(ns_ast_ctx *ctx, ns_type t, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t op = n->binary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_SHIFT_OP:
        return t;
    case NS_TOKEN_LOGIC_OP:
    case NS_TOKEN_CMP_OP:
        return ns_type_bool;
    default:
        ns_vm_error(ctx->filename, n->state, "syntax error", "unknown binary ops %.*s.", op.val.len, op.val.data);
        break;
    }
    return ns_type_unknown;
}

ns_type ns_vm_parse_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_type t, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (ns_type_is_number(t)) {
        return ns_vm_parse_binary_ops_number(ctx, t, i);
    } else {
        ns_token_t op = n->binary_expr.op;
        ns_type ret = ns_vm_parse_binary_override(vm, t, t, op);
        if (!ns_type_is_unknown(ret)) {
            return ret;
        }
        ns_vm_error(ctx->filename, n->state, "syntax error", "unknown binary ops %.*s.", op.val.len, op.val.data);
    }
    return ns_type_unknown;
}

ns_type ns_vm_parse_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type l = ns_vm_parse_expr(vm, ctx, n->binary_expr.left);
    ns_type r = ns_vm_parse_expr(vm, ctx, n->binary_expr.right);
    if (!ns_type_equals(l, r)) {
        ns_str l_name = ns_vm_get_type_name(vm, l);
        ns_str r_name = ns_vm_get_type_name(vm, r);
        ns_vm_error(ctx->filename, n->state, "type error", "assign expr type mismatch [%.*s = %.*s].", l_name.len, l_name.data, r_name.len, r_name.data);
    }
    return l;
}

ns_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP || n->binary_expr.op.type == NS_TOKEN_ASSIGN) {
        return ns_vm_parse_assign_expr(vm, ctx, i);
    }

    ns_type l = ns_vm_parse_expr(vm, ctx, n->binary_expr.left);
    ns_type r = ns_vm_parse_expr(vm, ctx, n->binary_expr.right);
    if (ns_type_equals(l, r)) {
        return ns_vm_parse_binary_ops(vm, ctx, l, i);
    }

    // try to find override fn
    ns_type ret = ns_vm_parse_binary_override(vm, l, r, n->binary_expr.op);
    if (!ns_type_is_unknown(ret)) {
        return ret;
    }

    // try upgrade type
    if (ns_type_is_number(l) && ns_type_is_number(r)) {
        ns_type t = ns_vm_number_type_upgrade(l, r);
        if (!ns_type_is_unknown(t))
            return t;
    }

    ns_str l_name = ns_vm_get_type_name(vm, l);
    ns_str r_name = ns_vm_get_type_name(vm, r);
    ns_str op = n->binary_expr.op.val;
    ns_vm_error(ctx->filename, n->state, "type error", "binary expr type mismatch [%.*s %.*s %.*s].", l_name.len, l_name.data, op.len, op.data, r_name.len, r_name.data);
    return ns_type_unknown;
}

ns_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_vm_parse_expr(vm, ctx, n->expr.body);
    case NS_AST_BINARY_EXPR: return ns_vm_parse_binary_expr(vm, ctx, i);
    case NS_AST_PRIMARY_EXPR: return ns_vm_parse_primary_expr(vm, ctx, i);
    case NS_AST_CALL_EXPR: return ns_vm_parse_call_expr(vm, ctx, i);
    case NS_AST_MEMBER_EXPR: return ns_vm_parse_member_expr(vm, ctx, i);
    case NS_AST_GEN_EXPR: return ns_vm_parse_gen_expr(vm, ctx, i);
    case NS_AST_DESIG_EXPR: return ns_vm_parse_desig_expr(vm, ctx, i);
    case NS_AST_UNARY_EXPR: return ns_vm_parse_unary_expr(vm, ctx, i);
    case NS_AST_CAST_EXPR: return ns_vm_parse_cast_expr(vm, ctx, i);
    case NS_AST_ARRAY_EXPR: return ns_vm_parse_array_expr(vm, ctx, i);
    case NS_AST_INDEX_EXPR: return ns_vm_parse_index_expr(vm, ctx, i);
    case NS_AST_STR_FMT: return ns_type_str;
    default: {
        ns_str type = ns_ast_type_to_string(n->type);
        ns_vm_error(ctx->filename, n->state, "syntax error", "unimplemented expr type %.*s.", type.len, type.data);
    } break;
    }
    return ns_type_unknown;
}

void ns_vm_parse_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->jump_stmt.label.type) {
    case NS_TOKEN_RETURN: {
        ns_type t = ns_vm_parse_expr(vm, ctx, n->jump_stmt.expr);
        size_t l = ns_array_length(vm->call_stack);
        if (l == 0) {
            ns_ast_error(ctx, "syntax error", "return stmt not in fn\n");
        }
        ns_symbol *fn = vm->call_stack[l - 1].fn;
        if (!ns_type_equals(fn->fn.ret, t)) {
            ns_ast_error(ctx, "type error", "return type mismatch\n");
        }
    } break;
    default: {
        ns_str l = n->jump_stmt.label.val;
        ns_error("vm parse", "unknown jump stmt type %.*s\n", l.len, l.data);
    } break;
    }
}

void ns_vm_parse_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type t = ns_vm_parse_expr(vm, ctx, n->if_stmt.condition);
    if (!ns_type_is(t, NS_TYPE_BOOL)) {
        ns_ast_t *cond = &ctx->nodes[n->if_stmt.condition];
        ns_vm_error(ctx->filename, cond->state, "type error", "if stmt expr type mismatch.");
    }

    ns_vm_parse_compound_stmt(vm, ctx, n->if_stmt.body);
    if (n->if_stmt.else_body) {
        ns_vm_parse_compound_stmt(vm, ctx, n->if_stmt.else_body);
    }
}

void ns_vm_parse_loop_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    i32 cond = n->loop_stmt.condition;
    i32 body = n->loop_stmt.body;
    if (n->loop_stmt.do_first) {
        ns_vm_parse_compound_stmt(vm, ctx, body);
        ns_type t = ns_vm_parse_expr(vm, ctx, cond);
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            ns_ast_error(ctx, "type error", "loop stmt expr type mismatch\n");
        }
    } else {
        ns_type t = ns_vm_parse_expr(vm, ctx, cond);
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            ns_ast_error(ctx, "type error", "loop stmt expr type mismatch\n");
        }
        ns_vm_parse_compound_stmt(vm, ctx, body);
    }
}

void ns_vm_parse_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];

    ns_ast_t *gen = &ctx->nodes[n->for_stmt.generator];
    ns_type t = ns_vm_parse_gen_expr(vm, ctx, n->for_stmt.generator);
    ns_symbol var = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = { .t = t }, .parsed = true};
    var.name = gen->gen_expr.name.val;
    ns_vm_push_symbol_local(vm, var);

    ns_enter_scope(vm);
    ns_vm_parse_compound_stmt(vm, ctx, n->for_stmt.body);
    ns_exit_scope(vm);
}

void ns_vm_parse_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol s = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true};
    s.name = n->var_def.name.val;
    ns_type l = ns_vm_parse_type(vm, ctx->nodes[n->var_def.type].type_label.name, true);

    if (n->var_def.expr != 0) {
        ns_type t = ns_vm_parse_expr(vm, ctx, n->var_def.expr);
        if (!ns_type_is(l, NS_TYPE_INFER) && !ns_type_equals(l, t)) {
            ns_str type = ns_vm_get_type_name(vm, t);
            ns_vm_error(ctx->filename, n->state, "type error", "local var def type mismatch %.*s.", type.len, type.data);
        }
        s.val.t = t;
        n->var_def.type_size = ns_type_size(vm, t);
    }
    ns_array_push(vm->symbol_stack, s);
}

void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *expr = &ctx->nodes[i];
    for (i32 e_i = 0, l = expr->compound_stmt.count; e_i < l; e_i++) {
        i32 expr_i = expr->next;
        expr = &ctx->nodes[expr_i];
        switch (expr->type) {
        case NS_AST_JUMP_STMT:
            ns_vm_parse_jump_stmt(vm, ctx, expr_i);
            break;
        case NS_AST_VAR_DEF:
            ns_vm_parse_local_var_def(vm, ctx, expr_i);
            break;
        case NS_AST_IF_STMT:
            ns_vm_parse_if_stmt(vm, ctx, expr_i);
            break;
        case NS_AST_FOR_STMT:
            ns_vm_parse_for_stmt(vm, ctx, expr_i);
            break;
        case NS_AST_CALL_EXPR:
        case NS_AST_BINARY_EXPR:
        case NS_AST_PRIMARY_EXPR:
        case NS_AST_MEMBER_EXPR:
        case NS_AST_GEN_EXPR:
        case NS_AST_DESIG_EXPR:
        case NS_AST_UNARY_EXPR:
            ns_vm_parse_expr(vm, ctx, expr_i);
            break;
        default: {
            ns_str type = ns_ast_type_to_string(expr->type);
            ns_vm_error(ctx->filename, expr->state, "vm parse", "unimplemented stmt type %.*s*.", type.len, type.data);
        } break;
        }
    }
}

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_vm_parse_import_stmt(vm, ctx);
    ns_vm_parse_fn_def_name(vm, ctx);
    ns_vm_parse_ops_fn_def_name(vm, ctx);
    ns_vm_parse_struct_def(vm, ctx);
    ns_vm_parse_struct_def_ref(vm);
    ns_vm_parse_fn_def_type(vm, ctx);
    ns_vm_parse_var_def(vm, ctx);
    ns_vm_parse_fn_def_body(vm, ctx);

    vm->symbol_top = ns_array_length(vm->symbols);

    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        switch (n->type) {
        case NS_AST_EXPR:
        case NS_AST_CALL_EXPR:
            ns_vm_parse_expr(vm, ctx, s_i);
            break;
        case NS_AST_IMPORT_STMT:
        case NS_AST_MODULE_STMT:
        case NS_AST_FN_DEF:
        case NS_AST_OPS_FN_DEF:
        case NS_AST_STRUCT_DEF:
        case NS_AST_VAR_DEF:
        case NS_AST_PROGRAM:
            break; // already parsed
        default: {
            ns_str type = ns_ast_type_to_string(n->type);
            if (!vm->repl) ns_warn("vm parse", "unimplemented global ast parse %.*s\n", type.len, type.data);
        } break;
        }
    }
    return true;
}