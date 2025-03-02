#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"

ns_return_void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

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

i32 ns_type_size(ns_vm *vm, ns_type t) {
    if (ns_type_is_ref(t)) return ns_ptr_size;

    switch (t.type)
    {
    case NS_TYPE_NIL: return 0;
    case NS_TYPE_I8:
    case NS_TYPE_U8: return 1;
    case NS_TYPE_I16:
    case NS_TYPE_U16: return 2;
    case NS_TYPE_BOOL:
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
        if (ns_null == s) return -1;
        return s->st.stride;
    }
    default:
        break;
    }
    return -1;
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
        return ns_str_null;
    }
}

i32 ns_struct_field_index(ns_symbol *st, ns_str s) {
    assert(st->type == NS_SYMBOL_STRUCT);
    ns_struct_field *fields = st->st.fields;
    for (i32 i = 0, l = ns_array_length(fields); i < l; i++) {
        if (ns_str_equals(fields[i].name, s)) {
            return i;
        }
    }
    return -1;
}

ns_str ns_ops_override_name(ns_str l, ns_str r, ns_token_t op) {
    ns_str op_name = ns_ops_name(op);
    if (ns_str_empty(op_name)) return ns_str_null;

    size_t len = l.len + r.len + op_name.len + 3;
    i8* data = (i8*)malloc(len);
    snprintf(data, len, "%.*s_%.*s_%.*s", l.len, l.data, op_name.len, op_name.data, r.len, r.data);
    data[len - 1] = '\0';
    return (ns_str){.data = data, .len = len - 1, .dynamic = 1};
}

ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t) {
    ns_bool is_ref = ns_type_is_ref(t);
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
            return ns_str_null;
        }

        ns_symbol *r = &vm->symbols[ti];
        if (!r) return ns_str_null;
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

ns_return_type ns_vm_parse_symbol_type(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_str n, ns_bool infer) {
    ns_symbol *r = ns_vm_find_symbol(vm, n);
    if (!r) {
        if (infer) return ns_return_ok(type, ns_type_infer);
        ns_ast_state s = ctx->nodes[i].state;
        return ns_return_error(type, ns_ast_state_loc(ctx, s), NS_ERR_SYNTAX, "unknown type.");
    }

    switch (r->type) {
    case NS_SYMBOL_VALUE:
        return ns_return_ok(type, r->val.t);
    case NS_SYMBOL_FN:
        return ns_return_ok(type, r->fn.fn.t);
    case NS_SYMBOL_STRUCT:
        return ns_return_ok(type, r->st.st.t);
    default: {
        ns_ast_state s = ctx->nodes[i].state;
        return ns_return_error(type, ns_ast_state_loc(ctx, s), NS_ERR_SYNTAX, "unknown symbol.");
    } break;
    }
}

ns_return_type ns_vm_parse_type(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_token_t t, ns_bool infer) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_type ret = ns_type_encode(NS_TYPE_UNKNOWN, 0, n->type_label.is_ref, NS_STORE_CONST);
    switch (t.type) {
    case NS_TOKEN_TYPE_I8: ret.type = NS_TYPE_I8; break;
    case NS_TOKEN_TYPE_U8: ret.type = NS_TYPE_U8; break;
    case NS_TOKEN_TYPE_I16: ret.type = NS_TYPE_I16; break;
    case NS_TOKEN_TYPE_U16: ret.type = NS_TYPE_U16; break;
    case NS_TOKEN_TYPE_BOOL: ret.type = NS_TYPE_BOOL; break;
    case NS_TOKEN_TYPE_I32: ret.type = NS_TYPE_I32; break;
    case NS_TOKEN_TYPE_U32: ret.type = NS_TYPE_U32; break;
    case NS_TOKEN_TYPE_I64: ret.type = NS_TYPE_I64; break;
    case NS_TOKEN_TYPE_U64: ret.type = NS_TYPE_U64; break;
    case NS_TOKEN_TYPE_F32: ret.type = NS_TYPE_F32; break;
    case NS_TOKEN_TYPE_F64: ret.type = NS_TYPE_F64; break;
    case NS_TOKEN_TYPE_STR: ret.type = NS_TYPE_STRING; break;
    case NS_TOKEN_NIL: ret.type = NS_TYPE_NIL; break;
    default:
        break;
    }
    if (ret.type != NS_TYPE_UNKNOWN) return ns_return_ok(type, ret);

    ns_return_type ret_t = ns_vm_parse_symbol_type(vm, ctx, i, t.val, infer);
    if (ns_return_is_error(ret_t)) return ns_return_change_type(type, ret_t);
    return ns_return_ok(type, ns_type_set_ref(ret_t.r, ret.ref));
}

void ns_vm_parse_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals(vm->lib, ns_str_cstr("main"));
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s];
        if (n->type != NS_AST_FN_DEF) continue;
        if (!main_mod && ns_str_equals_STR(n->fn_def.name.val, "main")) continue; // skip main in lib

        ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = s}, .lib =  vm->lib };
        fn.name = n->fn_def.name.val;
        ns_vm_push_symbol_global(vm, fn);
    }
}

ns_return_void ns_vm_parse_ops_fn_def_name(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s];
        if (n->type != NS_AST_OPS_FN_DEF) continue;

        ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = s}, .lib =  vm->lib};
        ns_ast_t l = ctx->nodes[n->ops_fn_def.left];
        ns_ast_t r = ctx->nodes[n->ops_fn_def.right];

        ns_str l_type = ctx->nodes[l.arg.type].type_label.name.val;
        ns_str r_type = ctx->nodes[r.arg.type].type_label.name.val;

        fn.name = ns_ops_override_name(l_type, r_type, n->ops_fn_def.ops);
        if (ns_str_empty(fn.name)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unknown ops override.");
        }

        ns_vm_push_symbol_global(vm, fn);
    }

    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_fn_def_type(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals(vm->lib, ns_str_cstr("main"));
    for (i32 i = vm->symbol_top, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *fn = &vm->symbols[i];
        if (fn->type != NS_SYMBOL_FN || fn->parsed) continue;
        ns_ast_t *n = &ctx->nodes[fn->fn.ast];
        if (!main_mod && ns_str_equals_STR(n->fn_def.name.val, "main")) continue; // skip main in lib

        fn->fn.fn.t = ns_type_encode(NS_TYPE_FN, i, 0, NS_STORE_CONST);
        if (n->type == NS_AST_FN_DEF) {
            ns_ast_t *ret_type = &ctx->nodes[n->fn_def.ret];
            ns_return_type ret = ns_vm_parse_type(vm, ctx, n->fn_def.ret, ret_type->type_label.name, true);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            fn->fn.ret = ret.r;

            ns_array_set_length(fn->fn.args, n->fn_def.arg_count);
            ns_ast_t *arg = n;
            for (i32 i = 0, l = n->fn_def.arg_count; i < l; i++) {
                arg = &ctx->nodes[arg->next];
                ns_symbol arg_record = (ns_symbol){.type = NS_SYMBOL_VALUE};
                arg_record.name = arg->arg.name.val;

                ns_ast_t *arg_type = &ctx->nodes[arg->arg.type];
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, arg->arg.type, arg_type->type_label.name, false);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);

                arg_record.val.t = ret_t.r;
                fn->fn.args[i] = arg_record;
            }
        } else if (n->type == NS_AST_OPS_FN_DEF) {
            ns_ast_t *l = &ctx->nodes[n->ops_fn_def.left];
            ns_ast_t *r = &ctx->nodes[n->ops_fn_def.right];
            ns_return_type ret = ns_vm_parse_type(vm, ctx, n->ops_fn_def.ret, ctx->nodes[n->ops_fn_def.ret].type_label.name, true);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            fn->fn.ret = ret.r;

            ns_array_set_length(fn->fn.args, 2);
            ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE};
            l_arg.name = l->arg.name.val;
            ret = ns_vm_parse_type(vm, ctx, l->arg.type, ctx->nodes[l->arg.type].type_label.name, false);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            l_arg.val.t = ret.r;
            fn->fn.args[0] = l_arg;

            ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE};
            r_arg.name = r->arg.name.val;

            ret = ns_vm_parse_type(vm, ctx, r->arg.type, ctx->nodes[r->arg.type].type_label.name, false);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            r_arg.val.t = ret.r;
            fn->fn.args[1] = r_arg;
        } else {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unknown fn def type");
        }
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_fn_def_body(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_return_void ret;
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals(vm->lib, ns_str_cstr("main"));
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *fn = &vm->symbols[i];
        if (fn->type != NS_SYMBOL_FN || fn->parsed) continue;
        ns_ast_t *n = &ctx->nodes[fn->fn.ast];
        if (!main_mod && ns_str_equals_STR(n->fn_def.name.val, "main")) continue; // skip main in lib

        i32 body = n->type == NS_AST_FN_DEF ? n->fn_def.body : n->ops_fn_def.body;
        ns_bool is_ref = n->type == NS_AST_FN_DEF ? n->fn_def.is_ref : n->ops_fn_def.is_ref;
        ns_call call = (ns_call){.fn = fn, .scope_top = ns_array_length(vm->scope_stack)};

        ns_array_push(vm->call_stack, call);
        ns_enter_scope(vm);

        for (i32 j = 0, l = ns_array_length(fn->fn.args); j < l; ++j) {
            ns_symbol *arg = &fn->fn.args[j];
            ns_vm_push_symbol_local(vm, *arg);
        }

        ret = ns_vm_parse_compound_stmt(vm, ctx, body);
        if (ns_return_is_error(ret)) return ret;
        
        ns_exit_scope(vm);
        ns_array_pop(vm->call_stack);
        fn->fn.fn = (ns_value){.t = ns_type_encode(ns_type_fn, i, is_ref, 0) };
        fn->parsed = true;
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx) {
    i32 size;
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t n = ctx->nodes[s];
        if (n.type != NS_AST_STRUCT_DEF)
            continue;
        i32 i = ns_array_length(vm->symbols);
        ns_symbol st = (ns_symbol){.type = NS_SYMBOL_STRUCT, .st = {.ast = s, .st.t = ns_type_encode(ns_type_struct, i, 0, 0)}, .lib =  vm->lib};
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
                ns_return_type ret_item_type = ns_vm_parse_type(vm, ctx, field->arg.type, type->type_label.name, true);
                if (ns_return_is_error(ret_item_type)) return ns_return_change_type(void, ret_item_type);

                ns_type item_type = ret_item_type.r;
                t = (ns_type){.type = item_type.type, .ref = type->type_label.is_ref, .array = true, .store = NS_STORE_HEAP};
            } else {
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, field->arg.type, type->type_label.name, true);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
                t = ret_t.r;

                size = ns_type_size(vm, t);
                if (size == -1) {
                    return ns_return_error(void, ns_ast_state_loc(ctx, type->state), NS_ERR_SYNTAX, "unknown type.");
                }
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
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_struct_def_ref(ns_vm *vm, ns_ast_ctx *ctx) {
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
                ns_ast_t *field = &ctx->nodes[st->st.ast];
                return ns_return_error(void, ns_ast_state_loc(ctx, field->state), NS_ERR_SYNTAX, "unknown ref type");
            }
        }
        st->parsed = true;
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_var_def(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t *n = &ctx->nodes[ctx->sections[i]];
        if (n->type != NS_AST_VAR_DEF)
            continue;
        ns_symbol r = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true};
        r.name = n->var_def.name.val;

        ns_return_type ret = ns_vm_parse_type(vm, ctx, n->var_def.type, ctx->nodes[n->var_def.type].type_label.name, true);
        if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);

        if (ret.r.type == NS_TYPE_UNKNOWN) return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "var def with unknown type.");

        if (ret.r.type == NS_TYPE_INFER) {
            if (n->var_def.expr == -1) {
                return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "implicit var def.");
            }
            ns_return_type ret_expr = ns_vm_parse_expr(vm, ctx, n->var_def.expr);
            if (ns_return_is_error(ret_expr)) return ns_return_change_type(void, ret_expr);
            r.val.t = ret_expr.r;
            n->var_def.type_size = ns_type_size(vm, r.val.t);
        } else {
            r.val.t = ret.r;
            n->var_def.type_size = ns_type_size(vm, r.val.t);
        }

        if (n->var_def.type_size == -1) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "var def with invalid type.");
        }

        ns_vm_push_symbol_global(vm, r);
    }
    return ns_return_ok_void;
}

ns_return_type ns_vm_parse_str_fmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_return_type ret;
    ns_ast_t *n = &ctx->nodes[i];
    i32 next = i;
    for (i32 j = 0, l = n->str_fmt.expr_count; j < l; ++j) {
        n = &ctx->nodes[next = n->next];
        ret = ns_vm_parse_expr(vm, ctx, next);
        n->expr.type = ret.r;
        if (ns_return_is_error(ret)) return ret;
    }
    return ns_return_ok(type, ns_type_str);
}

ns_return_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL:
        return ns_return_ok(type, ns_type_i32);
    case NS_TOKEN_FLT_LITERAL:
        return ns_return_ok(type, ns_type_f64);
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT:
        return ns_return_ok(type, ns_type_str);
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
        return ns_return_ok(type, ns_type_bool);
    case NS_TOKEN_IDENTIFIER:
        return ns_vm_parse_symbol_type(vm, ctx, i, n->primary_expr.token.val, true);
    default:
        break;
    }
    ns_ast_state s = ctx->nodes[i].state;
    return ns_return_error(type, ns_ast_state_loc(ctx, s), NS_ERR_SYNTAX, "invalid primary expr.");
}

ns_return_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *callee_n = &ctx->nodes[n->call_expr.callee];
    ns_return_type ret_callee = ns_vm_parse_primary_expr(vm, ctx, n->call_expr.callee);
    if (ns_return_is_error(ret_callee)) return ret_callee;

    ns_type fn = ret_callee.r;
    if (ns_type_is(fn, NS_TYPE_UNKNOWN)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, callee_n->state), NS_ERR_EVAL, "invalid callee.");
    }

    ns_symbol *fn_record = &vm->symbols[ns_type_index(fn)];
    if (!fn_record || fn_record->type != NS_SYMBOL_FN) {
        return ns_return_error(type, ns_ast_state_loc(ctx, callee_n->state), NS_ERR_EVAL, "unknown callee.");
    }

    i32 next = n->call_expr.arg;
    for (i32 a_i = 0, l = n->call_expr.arg_count; a_i < l; ++a_i) {
        ns_ast_t arg = ctx->nodes[next];
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, next);
        if (ns_return_is_error(ret_t)) return ret_t;

        ns_type t = ret_t.r;
        next = arg.next;
        if (!ns_type_equals(t, fn_record->fn.args[a_i].val.t)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, arg.state), NS_ERR_EVAL, "call expr type mismatch fn.");
        }
    }
    return ns_return_ok(type, fn_record->fn.ret);
}

ns_return_type ns_vm_parse_member_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->member_expr.left);
    if (ns_return_is_error(ret_t)) return ret_t;

    ns_type t = ret_t.r;
    if (!ns_type_is(t, NS_TYPE_STRUCT)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "member expr type mismatch.");
    }

    ns_ast_t field = ctx->nodes[n->next];
    if (field.type == NS_AST_PRIMARY_EXPR) {
        ns_str name = field.primary_expr.token.val;
        ns_struct_symbol *st = &vm->symbols[ns_type_index(t)].st;
        for (i32 f_i = 0, l = ns_array_length(st->fields); f_i < l; ++f_i) {
            ns_struct_field *f = &st->fields[f_i];
            if (ns_str_equals(f->name, name)) {
                return ns_return_ok(type, f->t);
            }
        }
        return ns_return_error(type, ns_ast_state_loc(ctx, field.state), NS_ERR_EVAL, "unknown member.");
    }
    return ns_vm_parse_expr(vm, ctx, n->next);
}

ns_bool ns_vm_parse_type_generable(ns_type t) {
    return ns_type_is(t, NS_TYPE_STRING) || ns_type_is(t, NS_TYPE_ARRAY);
}

ns_return_type ns_vm_parse_gen_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->gen_expr.range) {
        ns_return_type from_ret = ns_vm_parse_expr(vm, ctx, n->gen_expr.from);
        if (ns_return_is_error(from_ret)) return from_ret;
        ns_return_type to_ret = ns_vm_parse_expr(vm, ctx, n->gen_expr.to);
        if (ns_return_is_error(to_ret)) return to_ret;

        ns_type from_t = from_ret.r;
        ns_type to_t = to_ret.r;
        if (!ns_type_is(from_t, NS_TYPE_I32) || !ns_type_is(to_t, NS_TYPE_I32)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "gen expr type mismatch.");
        }
        return ns_return_ok(type, ns_type_i32);
    } else {
        ns_return_type from_ret = ns_vm_parse_expr(vm, ctx, n->gen_expr.from);
        if (ns_return_is_error(from_ret)) return from_ret;
        ns_type from_t = from_ret.r;
        if (!ns_vm_parse_type_generable(from_t)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "gen expr type mismatch.");
        }
        return ns_return_ok(type, from_t);
    }
}

ns_return_type ns_vm_parse_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *st = ns_vm_find_symbol(vm, n->desig_expr.name.val);
    if (!st || st->type != NS_SYMBOL_STRUCT) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown struct.");
    }

    ns_ast_t *field = n;
    for (i32 f_i = 0, l = n->desig_expr.count; f_i < l; ++f_i) {
        i32 next = field->next;
        field = &ctx->nodes[next];
        ns_str name = field->field_def.name.val;
        i32 field_i = ns_struct_field_index(st, name);
        if (field_i == -1) {
            return ns_return_error(type, ns_ast_state_loc(ctx, field->state), NS_ERR_EVAL, "unknown field.");
        }

        ns_struct_field *f = &st->st.fields[field_i];
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, field->field_def.expr);
        if (ns_return_is_error(ret_t)) return ret_t;

        ns_type t = ret_t.r;
        if (!ns_type_equals(t, f->t)) return ns_return_error(type, ns_ast_state_loc(ctx, field->state), NS_ERR_EVAL, "designated expr type mismatch.");

        field->field_def.index = field_i;
    }

    return ns_return_ok(type, st->st.st.t);
}

ns_return_type ns_vm_parse_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->unary_expr.expr);
    if (ns_return_is_error(ret_t)) return ret_t;

    ns_type t = ret_t.r;
    ns_token_t op = n->unary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        if (!ns_type_is_number(t)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
        return ns_return_ok(type, t);
    case NS_TOKEN_BIT_INVERT_OP:
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unary expr type mismatch.");
        }
        return ns_return_ok(type, t);
    default:
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown unary ops.");
    }
    return ns_return_ok(type, ns_type_unknown);
}

ns_return_type ns_vm_parse_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *type = &ctx->nodes[n->array_expr.type];
    ns_return_type ret_t = ns_vm_parse_type(vm, ctx, i, type->type_label.name, false);
    if (ns_return_is_error(ret_t)) return ret_t;
    ns_type t = ret_t.r;
    ns_type arr_t = (ns_type){.type = t.type, .ref = t.ref, .array = true, .store = NS_STORE_HEAP};
    return ns_return_ok(type, arr_t);
}

ns_return_type ns_vm_parse_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_l = ns_vm_parse_expr(vm, ctx, n->index_expr.table);
    if (ns_return_is_error(ret_l)) return ret_l;
    ns_return_type ret_r = ns_vm_parse_expr(vm, ctx, n->index_expr.expr);
    if (ns_return_is_error(ret_r)) return ret_r;

    ns_type l = ret_l.r;
    ns_type r = ret_r.r;
    if (!ns_type_is_array(l)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "index expr type mismatch.");
    }
    if (!ns_type_is(r, NS_TYPE_I32)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "index expr type mismatch.");
    }
    ns_type t = (ns_type){.type = l.type, .ref = l.ref, .array = false, .store = l.ref ? NS_STORE_HEAP : NS_STORE_CONST, .index = l.index };
    return ns_return_ok(type, t);
}

ns_return_type ns_vm_parse_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->cast_expr.expr);
    if (ns_return_is_error(ret_t)) return ret_t;
    ns_type t = ret_t.r;

    ns_return_type ret_cast = ns_vm_parse_type(vm, ctx, i, n->cast_expr.type, false);
    if (ns_return_is_error(ret_cast)) return ret_cast;

    ns_type cast = ret_cast.r;
    if (ns_type_equals(t, cast)) {
        return ns_return_ok(type, t);
    }

    if (ns_type_is_number(t) && ns_type_is_number(cast)) {
        return ns_return_ok(type, cast);
    }
    // ns_str t_name = ns_vm_get_type_name(vm, t);
    // ns_str cast_name = ns_vm_get_type_name(vm, cast);
    return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "cast expr type mismatch.");
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

ns_return_type ns_vm_parse_binary_ops_number(ns_ast_ctx *ctx, ns_type t, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t op = n->binary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_SHIFT_OP:
        return ns_return_ok(type, t);
    case NS_TOKEN_LOGIC_OP:
    case NS_TOKEN_CMP_OP:
        return ns_return_ok(type, ns_type_bool);
    default:
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state) , NS_ERR_EVAL, "unknown binary ops");
        break;
    }
    return ns_return_ok(type, ns_type_unknown);
}

ns_return_type ns_vm_parse_binary_ops(ns_vm *vm, ns_ast_ctx *ctx, ns_type t, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (ns_type_is_number(t)) {
        return ns_vm_parse_binary_ops_number(ctx, t, i);
    } else {
        ns_token_t op = n->binary_expr.op;
        ns_type ret = ns_vm_parse_binary_override(vm, t, t, op);
        if (!ns_type_is_unknown(ret)) {
            return ns_return_ok(type, ret);
        }
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown binary ops");
    }
    return ns_return_ok(type, ns_type_unknown);
}

ns_return_type ns_vm_parse_assign_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_l = ns_vm_parse_expr(vm, ctx, n->binary_expr.left);
    if (ns_return_is_error(ret_l)) return ret_l;
    ns_return_type ret_r = ns_vm_parse_expr(vm, ctx, n->binary_expr.right);
    if (ns_return_is_error(ret_r)) return ret_r;
    ns_type l = ret_l.r;
    ns_type r = ret_r.r;
    if (!ns_type_equals(l, r)) {
        // ns_str l_name = ns_vm_get_type_name(vm, l);
        // ns_str r_name = ns_vm_get_type_name(vm, r);
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "assign expr type mismatch.");
    }
    return ns_return_ok(type, l);
}

ns_return_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP || n->binary_expr.op.type == NS_TOKEN_ASSIGN) {
        return ns_vm_parse_assign_expr(vm, ctx, i);
    }

    ns_return_type ret_l = ns_vm_parse_expr(vm, ctx, n->binary_expr.left);
    if (ns_return_is_error(ret_l)) return ret_l;
    ns_return_type ret_r = ns_vm_parse_expr(vm, ctx, n->binary_expr.right);
    if (ns_return_is_error(ret_r)) return ret_r;

    ns_type l = ret_l.r;
    ns_type r = ret_r.r;

    if (ns_type_equals(l, r)) {
        return ns_vm_parse_binary_ops(vm, ctx, l, i);
    }

    // try to find override fn
    ns_type ret = ns_vm_parse_binary_override(vm, l, r, n->binary_expr.op);
    if (!ns_type_is_unknown(ret)) {
        return ns_return_ok(type, ret);
    }

    // try upgrade type
    if (ns_type_is_number(l) && ns_type_is_number(r)) {
        ns_type t = ns_vm_number_type_upgrade(l, r);
        if (!ns_type_is_unknown(t))
            return ns_return_ok(type, t);
    }

    // ns_str l_name = ns_vm_get_type_name(vm, l);
    // ns_str r_name = ns_vm_get_type_name(vm, r);
    // ns_str op = n->binary_expr.op.val;
    return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "binary expr type mismatch.");
}

ns_return_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
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
    case NS_AST_STR_FMT_EXPR: return ns_vm_parse_str_fmt(vm, ctx, i);
    default: {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unimplemented expr type.");
    } break;
    }
    return ns_return_ok(type, ns_type_unknown);
}

ns_return_void ns_vm_parse_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->jump_stmt.label.type) {
    case NS_TOKEN_RETURN: {
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->jump_stmt.expr);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};
        ns_type t = ret_t.r;

        size_t l = ns_array_length(vm->call_stack);
        if (l == 0) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "return stmt not in fn.");
        }
        ns_symbol *fn = vm->call_stack[l - 1].fn;
        if (!ns_type_equals(fn->fn.ret, t)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "return stmt type mismatch.");
        }
    } break;
    default:
        return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unknown jump stmt type.");
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->if_stmt.condition);
    if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

    ns_type t = ret_t.r;
    if (!ns_type_is(t, NS_TYPE_BOOL)) {
        ns_ast_t *cond = &ctx->nodes[n->if_stmt.condition];
        return ns_return_error(void, ns_ast_state_loc(ctx, cond->state), NS_ERR_SYNTAX, "if stmt expr type mismatch.");
    }

    ns_return_void ret = ns_vm_parse_compound_stmt(vm, ctx, n->if_stmt.body);
    if (ns_return_is_error(ret)) return ret;

    if (n->if_stmt.else_body) {
        ret = ns_vm_parse_compound_stmt(vm, ctx, n->if_stmt.else_body);
        if (ns_return_is_error(ret)) return ret;
    }

    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_loop_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_return_void ret;
    ns_ast_t *n = &ctx->nodes[i];
    i32 cond = n->loop_stmt.condition;
    i32 body = n->loop_stmt.body;
    if (n->loop_stmt.do_first) {
        ret = ns_vm_parse_compound_stmt(vm, ctx, body);
        if (ns_return_is_error(ret)) return ret;

        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, cond);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

        ns_type t = ret_t.r;
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "loop stmt expr type mismatch.");
        }
    } else {
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, cond);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

        ns_type t = ret_t.r;
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "loop stmt expr type mismatch.");
        }
        ret = ns_vm_parse_compound_stmt(vm, ctx, body);
        if (ns_return_is_error(ret)) return ret;
    }

    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_for_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];

    ns_ast_t *gen = &ctx->nodes[n->for_stmt.generator];
    ns_return_type ret_t = ns_vm_parse_gen_expr(vm, ctx, n->for_stmt.generator);
    if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

    ns_type t = ret_t.r;
    ns_symbol var = (ns_symbol){.type = NS_SYMBOL_VALUE, .val = { .t = t }, .parsed = true};
    var.name = gen->gen_expr.name.val;
    ns_vm_push_symbol_local(vm, var);

    ns_enter_scope(vm);
    ns_return_void ret = ns_vm_parse_compound_stmt(vm, ctx, n->for_stmt.body);
    if (ns_return_is_error(ret)) return ret;
    ns_exit_scope(vm);

    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol s = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true};
    s.name = n->var_def.name.val;
    ns_return_type ret_l = ns_vm_parse_type(vm, ctx, n->var_def.type, ctx->nodes[n->var_def.type].type_label.name, true);
    if (ns_return_is_error(ret_l)) return ns_return_change_type(void, ret_l);

    ns_type l = ret_l.r;
    if (n->var_def.expr != 0) {
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->var_def.expr);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

        ns_type t = ret_t.r;
        if (!ns_type_is(l, NS_TYPE_INFER) && !ns_type_equals(l, t)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "local var def type mismatch.");
        }
        s.val.t = t;
        n->var_def.type_size = ns_type_size(vm, t);
    }
    ns_array_push(vm->symbol_stack, s);
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_return_void ret;
    ns_ast_t *expr = &ctx->nodes[i];
    for (i32 e_i = 0, l = expr->compound_stmt.count; e_i < l; e_i++) {
        i32 expr_i = expr->next;
        expr = &ctx->nodes[expr_i];
        switch (expr->type) {
        case NS_AST_JUMP_STMT:
            ret = ns_vm_parse_jump_stmt(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_VAR_DEF:
            ret = ns_vm_parse_local_var_def(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_IF_STMT:
            ret = ns_vm_parse_if_stmt(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_FOR_STMT:
            ret = ns_vm_parse_for_stmt(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_CALL_EXPR:
        case NS_AST_BINARY_EXPR:
        case NS_AST_PRIMARY_EXPR:
        case NS_AST_MEMBER_EXPR:
        case NS_AST_GEN_EXPR:
        case NS_AST_DESIG_EXPR:
        case NS_AST_UNARY_EXPR: {
            ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, expr_i);
            if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};
        } break;
        default: {
            return ns_return_error(void, ns_ast_state_loc(ctx, expr->state), NS_ERR_SYNTAX, "unimplemented stmt type.");
        } break;
        }
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_global_expr(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        switch (n->type) {
            case NS_AST_EXPR:
            case NS_AST_CALL_EXPR: {
                ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, s_i);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
            } break;
            case NS_AST_IMPORT_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OPS_FN_DEF:
            case NS_AST_STRUCT_DEF:
            case NS_AST_PROGRAM:
                break; // already parsed
            default: {
                ns_str type = ns_ast_type_to_string(n->type);
                if (!vm->repl) ns_warn("vm parse", "unimplemented global ast parse %.*s\n", type.len, type.data);
            } break;
        }
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_global_as_main(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_call call = (ns_call){.fn = NULL, .scope_top = ns_array_length(vm->scope_stack), .ret = ns_nil, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = 0};
    ns_enter_scope(vm);
    ns_array_push(vm->call_stack, call);
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        switch (n->type) {
            case NS_AST_EXPR:
            case NS_AST_CALL_EXPR: {
                ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, s_i);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
            } break;
            case NS_AST_VAR_DEF: {
                ns_return_void ret = ns_vm_parse_local_var_def(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_IMPORT_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OPS_FN_DEF:
            case NS_AST_STRUCT_DEF:
            case NS_AST_PROGRAM:
                break; // already parsed
            default: {
                ns_str type = ns_ast_type_to_string(n->type);
                if (!vm->repl) ns_warn("vm parse", "unimplemented global ast parse %.*s\n", type.len, type.data);
            } break;
        }
    }
    ns_array_pop(vm->call_stack);
    ns_exit_scope(vm);
    return ns_return_ok_void;
}

ns_return_bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_vm_parse_import_stmt(vm, ctx);

    ns_return_void ret = ns_vm_parse_struct_def(vm, ctx);
    if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

    ret = ns_vm_parse_struct_def_ref(vm, ctx);
    if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

    ns_vm_parse_fn_def_name(vm, ctx);
    ret = ns_vm_parse_ops_fn_def_name(vm, ctx);
    if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

    ret = ns_vm_parse_fn_def_type(vm, ctx);
    if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

    ret = ns_vm_parse_fn_def_body(vm, ctx);
    if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

    ns_bool main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main")) != ns_null;
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals(vm->lib, ns_str_cstr("main"));

    // if is not main module, skip top level expr, parse top level var def as global var def
    // if is main module and main fn exists, parse top level expr as global expr, top level var def as global var def
    // if is main module and main fn not exists, create main fn and parse top level expr as main fn body, top level var def as local var def
    if (!main_mod || vm->repl) {
        ret = ns_vm_parse_var_def(vm, ctx);
        if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
    } else {
        if (main_fn) {
            ret = ns_vm_parse_global_expr(vm, ctx);
            if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

            ret = ns_vm_parse_var_def(vm, ctx);
            if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
        } else {
            ret = ns_vm_parse_global_as_main(vm, ctx);
            if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
        }
    }

    vm->symbol_top = ns_array_length(vm->symbols);
    return ns_return_ok(bool, true);
}