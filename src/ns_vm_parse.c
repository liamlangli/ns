#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"

ns_return_void ns_vm_parse_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_bool ns_vm_assign_number_compatible(ns_vm *vm, ns_type dst, ns_type src);

static const_str ns_vm_type_mismatch_msg(ns_vm *vm, const_str msg, ns_type expected, ns_type got) {
    ns_str e = ns_vm_get_type_name(vm, expected);
    ns_str g = ns_vm_get_type_name(vm, got);
    szt len = snprintf(ns_null, 0, "%s expected %.*s, got %.*s.", msg, e.len, e.data, g.len, g.data);
    i8 *data = ns_malloc(len + 1);
    snprintf(data, len + 1, "%s expected %.*s, got %.*s.", msg, e.len, e.data, g.len, g.data);
    return data;
}

static const_str ns_vm_type_mismatch_label_msg(ns_vm *vm, const_str msg, const_str expected, ns_type got) {
    ns_str g = ns_vm_get_type_name(vm, got);
    szt len = snprintf(ns_null, 0, "%s expected %s, got %.*s.", msg, expected, g.len, g.data);
    i8 *data = ns_malloc(len + 1);
    snprintf(data, len + 1, "%s expected %s, got %.*s.", msg, expected, g.len, g.data);
    return data;
}

static const_str ns_vm_type_pair_mismatch_msg(ns_vm *vm, const_str msg, const_str left_label, ns_type left, const_str right_label, ns_type right) {
    ns_str l = ns_vm_get_type_name(vm, left);
    ns_str r = ns_vm_get_type_name(vm, right);
    szt len = snprintf(ns_null, 0, "%s %s %.*s, %s %.*s.", msg, left_label, l.len, l.data, right_label, r.len, r.data);
    i8 *data = ns_malloc(len + 1);
    snprintf(data, len + 1, "%s %s %.*s, %s %.*s.", msg, left_label, l.len, l.data, right_label, r.len, r.data);
    return data;
}

ns_fn_symbol* ns_symbol_get_fn(ns_symbol *s) {
    assert(s->type == NS_SYMBOL_FN || s->type == NS_SYMBOL_BLOCK);
    if (s->type == NS_SYMBOL_FN) return &s->fn;
    if (s->type == NS_SYMBOL_BLOCK) return &s->bc.fn;
    return ns_null;
}

ns_bool ns_type_match(ns_vm *vm, ns_type r, ns_type p) {
    if (ns_type_is(r, NS_TYPE_ANY)) return true; // an `any` slot accepts every provided type
    if (ns_type_equals(r, p)) return true;
    else if (ns_type_is(r, NS_TYPE_UNION)) {
        // A value is assignable to a union when its concrete type matches the
        // union itself or any of the union's member types.
        ns_symbol *u = &vm->symbols[ns_type_index(r)];
        for (szt i = 0, l = ns_array_length(u->un.members); i < l; ++i) {
            if (ns_type_match(vm, u->un.members[i], p)) return true;
        }
        return false;
    }
    else if (ns_type_is(r, NS_TYPE_FN) && (ns_type_is(p, NS_TYPE_FN) || ns_type_is(p, NS_TYPE_BLOCK))) {
        ns_fn_symbol *fn_r = ns_symbol_get_fn(&vm->symbols[ns_type_index(r)]);
        ns_fn_symbol *fn_p = ns_symbol_get_fn(&vm->symbols[ns_type_index(p)]);

        i32 arg_required_r = fn_r->arg_required;
        i32 arg_required_p = fn_p->arg_required;

        for (i32 i = 0; i < arg_required_r; ++i) {
            if (i > arg_required_p) return false;

            ns_type arg_r = fn_r->args[i].t;
            ns_type arg_p = fn_p->args[i].t;

            if (!ns_type_match(vm, arg_r, arg_p)) return false;
        }

        ns_type ret_t = fn_r->ret;
        ns_type ret_f = fn_p->ret;
        if (!ns_type_match(vm, ret_f, ret_t)) return false;
        return true;
    } else {
        return false;
    }
}

ns_bool ns_vm_container_type_matches(ns_container_symbol *ct, ns_value_type kind, ns_type key, ns_type val) {
    if (!ns_type_is(ct->t, kind)) return false;
    if (!ns_type_equals(ct->key, key)) return false;
    if (kind == NS_TYPE_SET) return true;
    return ns_type_equals(ct->val, val);
}

ns_str ns_vm_container_type_name(ns_vm *vm, ns_value_type kind, ns_type key, ns_type val) {
    ns_str key_name = ns_vm_get_type_name(vm, key);
    ns_str val_name = ns_vm_get_type_name(vm, val);
    ns_str name = {.dynamic = true};
    if (kind == NS_TYPE_SET) {
        ns_str_append(&name, ns_str_cstr("set["));
        ns_str_append(&name, key_name);
        ns_str_append(&name, ns_str_cstr("]"));
    } else {
        ns_str_append(&name, ns_str_cstr("["));
        ns_str_append(&name, key_name);
        ns_str_append(&name, ns_str_cstr(": "));
        ns_str_append(&name, val_name);
        ns_str_append(&name, ns_str_cstr("]"));
    }
    ns_array_push(name.data, '\0');
    return name;
}

ns_type ns_vm_intern_container_type(ns_vm *vm, ns_value_type kind, ns_type key, ns_type val) {
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        if (s->type == NS_SYMBOL_CONTAINER && ns_vm_container_type_matches(&s->ct, kind, key, val)) {
            return s->ct.t;
        }
    }

    i32 index = ns_array_length(vm->symbols);
    ns_symbol ct = (ns_symbol){.type = NS_SYMBOL_CONTAINER, .parsed = true, .lib = vm->lib};
    ct.ct.t = ns_type_encode(kind, index, 0, true, true);
    ct.ct.key = key;
    ct.ct.val = val;
    ct.name = ns_vm_container_type_name(vm, kind, key, val);
    ns_vm_push_symbol_global(vm, ct);
    return ct.ct.t;
}

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

    // An array-typed value is a single handle (heap pointer), regardless of its
    // element type. Without this, `[i32]` (element type NS_TYPE_I32 + array flag)
    // would size to 4 bytes and a stored 8-byte handle would be truncated by the
    // next stack allocation. Element sizing always clears the array flag first,
    // so this does not affect index/stride computations.
    if (ns_type_is_array(t)) return ns_ptr_size;

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
    case NS_TYPE_ANY:
    case NS_TYPE_ARRAY:
    case NS_TYPE_DICT:
    case NS_TYPE_SET:
    case NS_TYPE_FN: return ns_ptr_size;
    // A str field is laid out as the C `ns_str` struct (data ptr + len + flag),
    // so its slot must match that ABI size for struct field offsets to line up
    // with native code. The ns value model only uses the low 8 bytes (a str_list
    // index), but the slot still reserves the full struct width.
    case NS_TYPE_STRING: return (i32)sizeof(ns_str);
    case NS_TYPE_STRUCT: {
        u64 ti = ns_type_index(t);
        ns_symbol *s = &vm->symbols[ti];
        if (ns_null == s) return -1;
        return s->st.stride;
    }
    case NS_TYPE_UNION: {
        // A union slot must fit its largest member.
        ns_symbol *s = &vm->symbols[ns_type_index(t)];
        i32 max = 0;
        for (szt i = 0, l = ns_array_length(s->un.members); i < l; ++i) {
            i32 ms = ns_type_size(vm, s->un.members[i]);
            if (ms < 0) return -1;
            if (ms > max) max = ms;
        }
        return max;
    }
    default:
        break;
    }
    return -1;
}

void ns_vm_push_symbol_global(ns_vm *vm, ns_symbol r) {
    ns_array_push(vm->symbols, r);
}

i32 ns_vm_push_symbol_local(ns_vm *vm, ns_symbol r) {
    i32 i = ns_array_length(vm->symbol_stack);
    ns_array_push(vm->symbol_stack, r);
    return i;
}

i32 ns_vm_push_string(ns_vm *vm, ns_str s) {
    i32 i = ns_array_length(vm->str_list);
    i8 *data = (i8 *)ns_buffer_alloc(sizeof(i8), (szt)s.len + 1, ns_type_i8);
    if (data) {
        if (s.len > 0) memcpy(data, s.data, (szt)s.len);
        data[s.len] = '\0';
        ns_buffer_header(data)->len = (szt)s.len;
    }
    ns_str str = (ns_str){.data = data, .len = s.len, .dynamic = false};
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
    case NS_TOKEN_REL_OP:
        if (ns_str_equals_STR(op.val, "<"))
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
    if (ns_str_is_empty(op_name)) return ns_str_null;

    szt len = l.len + r.len + op_name.len + 3;
    i8* data = (i8*)ns_malloc(len);
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
    case NS_TYPE_STRUCT:
    case NS_TYPE_UNION:
    case NS_TYPE_DICT:
    case NS_TYPE_SET: {
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

// reverse search call_stack get last ns_call which callee type is NS_TYPE_FN rather than NS_TYPE_BLOCK
i32 ns_vm_get_last_call(ns_vm *vm) {
    i32 l = ns_array_length(vm->call_stack);
    for (i32 i = l - 1; i >= 0; --i) {
        ns_call *call = &vm->call_stack[i];
        ns_symbol *callee = call->callee;
        if (callee == ns_null || callee->type == NS_SYMBOL_FN) return i;
    }
    return -1;
}

void ns_vm_block_capture_symbol(ns_vm *vm, ns_symbol *bc, ns_symbol *s) {
    assert(bc->type == NS_SYMBOL_BLOCK);

    szt field_count = ns_array_length(bc->bc.st.fields);
    for (szt i = 0; i < field_count; ++i) {
        ns_struct_field *f = &bc->bc.st.fields[i];
        if (ns_str_equals(f->name, s->name)) {
            if (ns_type_match(vm, f->t, s->t)) {
                return;
            } else {
                assert(false); // type mismatch
            }
        }
    }

    ns_struct_field f = {.name = s->name, .t = s->t};
    ns_array_push(bc->bc.st.fields, f);
}

ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s, ns_bool capture) {
    i32 l = ns_vm_get_last_call(vm);
    ns_symbol *ret = ns_null;
    if (l >= 0) {
        ns_call *call = &vm->call_stack[l];
        i32 scope_top = call->scope_top;
        i32 symbol_top = vm->scope_stack[scope_top].symbol_top;
        i32 symbol_count = ns_array_length(vm->symbol_stack);

        i32 j = symbol_count - 1;
        for (; j >= symbol_top; --j) {
            if (ns_str_equals(vm->symbol_stack[j].name, s)) {
                ret = &vm->symbol_stack[j];
                break;
            }
        }

        if (capture && ret) {
            i32 t = (i32)ns_array_length(vm->call_stack) - 1;
            ns_call *top_call = &vm->call_stack[t];
            if (t > l && top_call->callee->type == NS_SYMBOL_BLOCK && j < top_call->arg_offset) {
                ns_vm_block_capture_symbol(vm, top_call->callee, ret);
            }
        }
        
        if (ret) return ret;
    }

    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; i++) {
        if (ns_str_equals(vm->symbols[i].name, s) && ns_str_equals(vm->symbols[i].lib, vm->lib)) {
            return &vm->symbols[i];
        }
    }

    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; i++) {
        if (ns_str_equals(vm->symbols[i].name, s)) {
            return &vm->symbols[i];
        }
    }
    return ns_null;
}

ns_type ns_vm_parse_generic_type(ns_token_t t) {
    ns_type ret = ns_type_encode(NS_TYPE_UNKNOWN, 0, false, false, true);
    switch (t.type) {
    case NS_TOKEN_NIL: ret.type = NS_TYPE_NIL; break;
    case NS_TOKEN_TYPE_ANY: ret.type = NS_TYPE_ANY; break;
    case NS_TOKEN_TYPE_VOID: ret.type = NS_TYPE_VOID; break;
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
    default:
        break;
    }
    return ret;
}

ns_return_type ns_vm_parse_type_by_token(ns_vm *vm, ns_token_t t, ns_code_loc loc) {
    ns_type type = ns_vm_parse_generic_type(t);
    if (type.type != NS_TYPE_UNKNOWN) return ns_return_ok(type, type);

    ns_symbol *r = ns_vm_find_symbol(vm, t.val, true);
    if (!r) {
        return ns_return_error(type, loc, NS_ERR_SYNTAX, "unknown type.");
    }

    switch (r->type) {
    case NS_SYMBOL_VALUE:
        return ns_return_ok(type, r->val.t);
    case NS_SYMBOL_FN:
        return ns_return_ok(type, r->fn.fn.t);
    case NS_SYMBOL_STRUCT:
        return ns_return_ok(type, r->st.st.t);
    case NS_SYMBOL_TYPE: // type alias resolves to its underlying type
        return ns_return_ok(type, r->val.t);
    case NS_SYMBOL_UNION:
        return ns_return_ok(type, r->un.t);
    default: {
        return ns_return_error(type, loc, NS_ERR_SYNTAX, "unknown symbol.");
    } break;
    }
}

ns_return_type ns_vm_parse_type(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t *n) {
    ns_token_t t;
    ns_bool is_ref = false;
    if (n->type == NS_AST_PROGRAM) {
        return ns_return_ok(type, ns_type_infer);
    } else if (n->type == NS_AST_TYPE_LABEL) {
        t = n->type_label.name;
        is_ref = n->type_label.is_ref;
    } else if (n->type == NS_AST_CAST_EXPR) {
        t = n->cast_expr.type;
    } else {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unknown ast type.");
    }

    // Array-ness of a type label (e.g. `[i32]`, `[Tok]`) must be carried onto
    // the resolved type. Both the generic (primitive) and by-token (named)
    // paths below honor it; without this, array parameters lose their array
    // flag and indexing them fails to type-check.
    ns_bool is_array = (n->type == NS_AST_TYPE_LABEL) ? n->type_label.is_array : false;
    ns_bool is_dict = (n->type == NS_AST_TYPE_LABEL) ? n->type_label.is_dict : false;
    ns_bool is_set = (n->type == NS_AST_TYPE_LABEL) ? n->type_label.is_set : false;

    if (is_dict) {
        ns_return_type key_ret = ns_vm_parse_type(vm, ctx, &ctx->nodes[n->type_label.key]);
        if (ns_return_is_error(key_ret)) return key_ret;
        ns_return_type val_ret = ns_vm_parse_type(vm, ctx, &ctx->nodes[n->type_label.val]);
        if (ns_return_is_error(val_ret)) return val_ret;
        ns_type dict_t = ns_vm_intern_container_type(vm, NS_TYPE_DICT, key_ret.r, val_ret.r);
        return ns_return_ok(type, ns_type_set_ref(dict_t, is_ref));
    }

    if (is_set) {
        ns_return_type elem_ret = ns_vm_parse_type(vm, ctx, &ctx->nodes[n->type_label.elem]);
        if (ns_return_is_error(elem_ret)) return elem_ret;
        ns_type set_t = ns_vm_intern_container_type(vm, NS_TYPE_SET, elem_ret.r, ns_type_void);
        return ns_return_ok(type, ns_type_set_ref(set_t, is_ref));
    }

    if (is_array && n->type_label.elem != 0) {
        ns_return_type elem_ret = ns_vm_parse_type(vm, ctx, &ctx->nodes[n->type_label.elem]);
        if (ns_return_is_error(elem_ret)) return elem_ret;
        ns_type rt = ns_type_set_ref(elem_ret.r, is_ref);
        rt.array = true;
        rt.stack = true;
        return ns_return_ok(type, rt);
    }

    ns_type ret = ns_vm_parse_generic_type(t);
    if (ret.type != NS_TYPE_UNKNOWN) {
        if (is_array) { ret.array = true; ret.stack = true; }
        return ns_return_ok(type, ret);
    }

    ns_return_type ret_t = ns_vm_parse_type_by_token(vm, t, ns_ast_state_loc(ctx, n->state));
    if (ns_return_is_error(ret_t)) return ns_return_change_type(type, ret_t);
    ns_type rt = ns_type_set_ref(ret_t.r, is_ref);
    if (is_array) { rt.array = true; rt.stack = true; }
    return ns_return_ok(type, rt);
}

ns_return_void ns_vm_parse_name(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals(vm->lib, ns_str_cstr("main"));
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s];
        i32 symbol_index = ns_array_length(vm->symbols);
        switch (n->type)
        {
        case NS_AST_FN_DEF: {
            if (!main_mod && ns_str_equals_STR(n->fn_def.name.val, "main")) continue; // skip main in lib
            ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = s, .body = n->fn_def.body}, .lib =  vm->lib };
            fn.fn.fn.t = ns_type_encode(NS_TYPE_FN, symbol_index, n->fn_def.is_ref, false, true);
            fn.name = n->fn_def.name.val;
            ns_vm_push_symbol_global(vm, fn);
        } break;

        case NS_AST_OP_FN_DEF: {
            ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = s, .body = n->ops_fn_def.body}, .lib =  vm->lib};
            fn.fn.fn.t = ns_type_encode(NS_TYPE_FN, symbol_index, n->ops_fn_def.is_ref, false, true);
            ns_ast_t l = ctx->nodes[n->ops_fn_def.left];
            ns_ast_t r = ctx->nodes[n->ops_fn_def.right];

            ns_str l_type = ctx->nodes[l.arg.type].type_label.name.val;
            ns_str r_type = ctx->nodes[r.arg.type].type_label.name.val;

            fn.name = ns_ops_override_name(l_type, r_type, n->ops_fn_def.ops);
            if (ns_str_is_empty(fn.name)) {
                return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unknown ops override.");
            }

            ns_vm_push_symbol_global(vm, fn);
        } break;

        case NS_AST_STRUCT_DEF: {
            ns_symbol st = (ns_symbol){.type = NS_SYMBOL_STRUCT, .st = {.ast = s }, .lib = vm->lib};
            st.st.st.t = ns_type_encode(NS_TYPE_STRUCT, symbol_index, 0, false, true);
            st.name = n->struct_def.name.val;
            ns_vm_push_symbol_global(vm, st);
        } break;

        default:
            break;
        }
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

        fn->fn.fn.t = ns_type_encode(NS_TYPE_FN, i, 0, false, true);
        if (n->type == NS_AST_FN_DEF) {
            ns_ast_t *ret_type = &ctx->nodes[n->fn_def.ret];
            ns_return_type ret = ns_vm_parse_type(vm, ctx, ret_type);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            fn->fn.ret = ns_type_set_mut(ret.r, true);

            fn->fn.arg_required = n->fn_def.arg_required;
            ns_array_set_length(fn->fn.args, n->fn_def.arg_count);
            ns_ast_t *arg = n;
            for (i32 i = 0, l = n->fn_def.arg_count; i < l; i++) {
                arg = &ctx->nodes[arg->next];
                ns_symbol arg_record = (ns_symbol){.type = NS_SYMBOL_VALUE};
                arg_record.name = arg->arg.name.val;

                ns_ast_t *arg_type = &ctx->nodes[arg->arg.type];
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, arg_type);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);

                arg_record.val.t = ret_t.r;
                fn->fn.args[i] = arg_record;
            }
        } else if (n->type == NS_AST_OP_FN_DEF) {
            ns_ast_t *l = &ctx->nodes[n->ops_fn_def.left];
            ns_ast_t *r = &ctx->nodes[n->ops_fn_def.right];
            ns_ast_t *ret_type = &ctx->nodes[n->ops_fn_def.ret];
            ns_return_type ret = ns_vm_parse_type(vm, ctx, ret_type);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            fn->fn.ret = ns_type_set_mut(ret.r, true);
            fn->fn.arg_required = n->ops_fn_def.arg_required; // TODO, for single arg ops fn, ast arg_required as 1

            ns_array_set_length(fn->fn.args, 2);
            ns_symbol l_arg = (ns_symbol){.type = NS_SYMBOL_VALUE};
            l_arg.name = l->arg.name.val;
            ns_ast_t *l_type = &ctx->nodes[l->arg.type];
            ret = ns_vm_parse_type(vm, ctx, l_type);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            l_arg.val.t = ret.r;
            fn->fn.args[0] = l_arg;

            ns_symbol r_arg = (ns_symbol){.type = NS_SYMBOL_VALUE};
            r_arg.name = r->arg.name.val;

            ns_ast_t *r_type = &ctx->nodes[r->arg.type];
            ret = ns_vm_parse_type(vm, ctx, r_type);
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
        // Parsing the body can push new global symbols (each block/closure
        // registers one), reallocating vm->symbols. Keep the call frame's
        // callee on a stable stack copy and re-fetch the symbol afterwards so
        // neither dangles across that growth.
        ns_symbol fn_sym = *fn;
        ns_call call = (ns_call){.callee = &fn_sym, .scope_top = ns_array_length(vm->scope_stack)};

        ns_array_push(vm->call_stack, call);
        ns_scope_enter(vm);

        for (i32 j = 0, l = ns_array_length(fn_sym.fn.args); j < l; ++j) {
            ns_symbol *arg = &fn_sym.fn.args[j];
            ns_vm_push_symbol_local(vm, *arg);
        }

        ret = ns_vm_parse_compound_stmt(vm, ctx, body);
        if (ns_return_is_error(ret)) return ret;
        
        ns_scope_exit(vm);
        ns_array_pop(vm->call_stack);
        fn = &vm->symbols[i];
        fn->fn.fn = (ns_value){.t = ns_type_encode(NS_TYPE_FN, i, is_ref, false, true) };
        fn->parsed = true;
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_type_def(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t *n = &ctx->nodes[ctx->sections[i]];
        if (n->type != NS_AST_TYPE_DEF)
            continue;
        ns_ast_t *t = &ctx->nodes[n->type_def.type];
        if (n->type_def.count > 1) { // union type: `type T = A | B | C`
            ns_symbol u = (ns_symbol){.type = NS_SYMBOL_UNION, .lib = vm->lib, .parsed = true};
            u.name = n->type_def.name.val;
            i32 index = ns_array_length(vm->symbols);
            u.un.t = ns_type_encode(NS_TYPE_UNION, index, 0, false, true);

            i32 member = n->type_def.type;
            for (i32 m = 0; m < n->type_def.count; ++m) {
                ns_ast_t *member_node = &ctx->nodes[member];
                ns_return_type ret = ns_vm_parse_type(vm, ctx, member_node);
                if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
                if (ret.r.type == NS_TYPE_UNKNOWN) {
                    return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "union type with unknown member type.");
                }
                ns_array_push(u.un.members, ret.r);
                member = member_node->next;
            }

            ns_vm_push_symbol_global(vm, u);
        } else if (t->type_label.is_fn) { // if t is fn, create a new fn type
            ns_symbol fn = (ns_symbol){.type = NS_SYMBOL_FN, .fn = {.ast = n->type_def.type, .body = 0}, .lib = vm->lib, .parsed = true};
            fn.name = n->type_def.name.val;

            ns_ast_t *arg = &ctx->nodes[n->type_def.type];
            fn.fn.arg_required = t->type_label.arg_count;
            ns_array_set_length(fn.fn.args, t->type_label.arg_count);
            for (i32 a = 0, an = t->type_label.arg_count; a < an; ++a) {
                // type def ignore arg name
                ns_symbol arg_record = (ns_symbol){.type = NS_SYMBOL_VALUE};
                ns_ast_t *arg_type = &ctx->nodes[arg->next];
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, arg_type);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
                arg_record.val.t = ret_t.r;
                fn.fn.args[a] = arg_record;
                arg = &ctx->nodes[arg->next];
            }

            ns_ast_t *ret = &ctx->nodes[t->type_label.ret];
            ns_return_type ret_t = ns_vm_parse_type(vm, ctx, ret);
            if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
            if (ret_t.r.type == NS_TYPE_UNKNOWN) {
                return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "typedef with unknown type.");
            }
            fn.fn.ret = ret_t.r;
            i32 index = ns_array_length(vm->symbols);
            fn.fn.fn = (ns_value){.t = ns_type_encode(NS_TYPE_FN, index, 0, false, true)};

            ns_vm_push_symbol_global(vm, fn);
        } else {
            ns_symbol type_sym = (ns_symbol){.type = NS_SYMBOL_TYPE, .lib = vm->lib};
            type_sym.name = n->type_def.name.val;
            ns_ast_t *type = &ctx->nodes[n->type_def.type];
            ns_return_type ret = ns_vm_parse_type(vm, ctx, type);
            if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            if (ret.r.type == NS_TYPE_UNKNOWN) {
                return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "typedef with unknown type.");
            }
            type_sym.val.t = ret.r;
            ns_vm_push_symbol_global(vm, type_sym);
        }
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx) {
    i32 size;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *st = &vm->symbols[i];
        if (st->type != NS_SYMBOL_STRUCT || st->parsed)
            continue;
        ns_ast_t *n = &ctx->nodes[st->st.ast];
        ns_array_set_length(st->st.fields, n->struct_def.count);
        i32 offset = 0;
        ns_ast_t *field = n;
        for (i32 i = 0; i < n->struct_def.count; i++) {
            field = &ctx->nodes[field->next];
            ns_ast_t *type = &ctx->nodes[field->arg.type];
            ns_type t;
            if (type->type_label.is_array) {
                size = ns_ptr_size;
                ns_return_type ret_item_type = ns_vm_parse_type(vm, ctx, type);
                if (ns_return_is_error(ret_item_type)) return ns_return_change_type(void, ret_item_type);

                ns_type item_type = ret_item_type.r;
                t = (ns_type){.type = item_type.type, .ref = type->type_label.is_ref, .array = true, .mut = item_type.mut, .stack = true};
            } else {
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, type);
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
            st->st.fields[i] = f;
        }
        st->st.stride = offset; // 4 bytes align
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
            ns_symbol *t = ns_vm_find_symbol(vm, n, false);
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
        ns_symbol r = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true, .lib = vm->lib};
        r.name = n->var_def.name.val;

        ns_bool type_required = n->var_def.type != 0;
        ns_ast_t *type = &ctx->nodes[n->var_def.type];
        ns_return_type ret = ns_vm_parse_type(vm, ctx, type);
        if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
        if (type_required && ret.r.type == NS_TYPE_UNKNOWN) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "var def with unknown type.");
        }

        if (ret.r.type == NS_TYPE_INFER) {
            if (n->var_def.expr == -1) {
                return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "implicit var def.");
            }
            ns_return_type ret_expr = ns_vm_parse_expr(vm, ctx, n->var_def.expr, ret.r);
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
        ret = ns_vm_parse_expr(vm, ctx, next, ns_type_infer);
        n->expr.type = ret.r;
        if (ns_return_is_error(ret)) return ret;
    }
    return ns_return_ok(type, ns_type_str);
}

ns_return_type ns_vm_parse_block_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_type t) {
    ns_ast_t *n = &ctx->nodes[i];

    // create block name with {filename}_{line}
    szt s = snprintf(NULL, 0, "%.*s:%d:%d", ctx->filename.len, ctx->filename.data, n->state.l, n->state.o);
    i8 *block_buff = (i8*)ns_malloc(s + 1);
    snprintf(block_buff, s + 1, "%.*s:%d:%d", ctx->filename.len, ctx->filename.data, n->state.l, n->state.o);
    ns_str block_name = (ns_str){.data = block_buff, .len = s, .dynamic = 1};

    ns_fn_symbol sym_fn = {.ast = 0, .body = n->block_expr.body, .arg_required = n->block_expr.arg_count, .ret = ns_type_infer};
    ns_symbol sym = (ns_symbol){.type = NS_SYMBOL_BLOCK, .name = block_name, .parsed = true, .bc.fn = sym_fn, .lib = vm->lib};

    ns_symbol *fn_t = nil;
    if (t.type == NS_TYPE_FN) {
        // The expected type is a fn type whose symbol carries the resolved arg
        // list and required count. Read those from the symbol rather than the
        // backing AST node: for a `type X = (...) -> ...` alias the node is a
        // type_label, not a fn_def, so fn_def.arg_count would alias garbage.
        fn_t = &vm->symbols[ns_type_index(t)];
        i32 fn_arg_count = (i32)ns_array_length(fn_t->fn.args);
        if (n->block_expr.arg_count < fn_t->fn.arg_required || n->block_expr.arg_count > fn_arg_count) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "block expr arg count mismatch.");
        }
    }

    ns_array_set_length(sym.bc.fn.args, n->block_expr.arg_count);
    ns_call call = (ns_call){.callee = &sym, .scope_top = ns_array_length(vm->scope_stack), .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = n->block_expr.arg_count};
    ns_array_push(vm->call_stack, call);
    ns_scope_enter(vm);

    ns_ast_t *arg = &ctx->nodes[n->next];
    for (i32 i = 0, l = n->block_expr.arg_count; i < l; ++i) {
        ns_symbol arg_sym = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true};
        arg_sym.name = arg->arg.name.val;

        if (fn_t == nil) { // if fn_t is nil, it means the block expr must define all types
            if (arg->arg.type == 0) {
                return ns_return_error(type, ns_ast_state_loc(ctx, arg->state), NS_ERR_SYNTAX, "block expr arg type mismatch.");
            } else {
                ns_ast_t *arg_type = &ctx->nodes[arg->arg.type];
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, arg_type);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(type, ret_t);
                arg_sym.val.t = ret_t.r;
            }
        } else { // if fn_t is not nil, it means the block expr must use the fn_t type or match the fn_t type
            ns_type arg_t = fn_t->fn.args[i].val.t;
            if (arg->arg.type == 0) {
                ns_ast_t *arg_type = &ctx->nodes[arg->arg.type];
                ns_return_type ret_t = ns_vm_parse_type(vm, ctx, arg_type);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(type, ret_t);
                if (!ns_type_is(ret_t.r, NS_TYPE_INFER) && !ns_type_equals(ret_t.r, arg_t)) {
                    return ns_return_error(type, ns_ast_state_loc(ctx, arg->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "block expr arg type mismatch.", arg_t, ret_t.r));
                }
                arg_sym.val.t = arg_t;
            } else {
                arg_sym.val.t = arg_t;
            }
        }

        sym.bc.fn.args[i] = arg_sym;
        ns_vm_push_symbol_local(vm, arg_sym);
        arg = &ctx->nodes[arg->next];
    }

    // parse block ret type
    if (fn_t == nil) {
        ns_return_type ret_t = ns_vm_parse_type(vm, ctx, &ctx->nodes[n->block_expr.ret]);
        if (ns_return_is_error(ret_t)) return ns_return_change_type(type, ret_t);
        sym.bc.fn.ret = ret_t.r;
    } else {
        ns_ast_t *ret_type = &ctx->nodes[n->block_expr.ret];
        ns_return_type ret_t = ns_vm_parse_type(vm, ctx, ret_type);
        if (ns_return_is_error(ret_t)) return ns_return_change_type(type, ret_t);
        if (!ns_type_is(ret_t.r, NS_TYPE_INFER) && !ns_type_equals(ret_t.r, fn_t->fn.ret)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "block expr ret type mismatch.", fn_t->fn.ret, ret_t.r));
        }
        sym.bc.fn.ret = fn_t->fn.ret;
    }

    ns_return_void stmt_ret = ns_vm_parse_compound_stmt(vm, ctx, n->block_expr.body);
    if (ns_return_is_error(stmt_ret)) return ns_return_change_type(type, stmt_ret);
    ns_scope_exit(vm);
    ns_array_pop(vm->call_stack);

    i32 index = ns_array_length(vm->symbols);
    n->block_expr.rt.index = index;
    szt field_count = ns_array_length(sym.bc.st.fields);

    // if no arg captured by block, set the block type to fn type
    if (field_count == 0) {
        ns_type fn_t = ns_type_encode(NS_TYPE_FN, index, 0, true, true);
        sym.bc.fn.fn = (ns_value){.t = fn_t};
        ns_symbol bc_fn = (ns_symbol){.name = sym.name, .type = NS_SYMBOL_FN, .fn = sym.bc.fn, .lib = vm->lib, .parsed = true};
        ns_vm_push_symbol_global(vm, bc_fn);
        return ns_return_ok(type, fn_t);
    } else {
        ns_array_set_length(sym.bc.st.fields, field_count);
        u64 offset = 0;
        for (szt i = 0; i < field_count; ++i) {
            ns_struct_field *f = &sym.bc.st.fields[i];
            f->s = ns_type_size(vm, f->t);
            offset = ns_align(offset, f->s);
            f->o = offset;
            offset += f->s;
        }
        sym.bc.st.stride = offset;
        ns_type block_t = ns_type_encode(NS_TYPE_BLOCK, index, 0, true, true);
        sym.bc.val.t = block_t;
        ns_vm_push_symbol_global(vm, sym);
        return ns_return_ok(type, block_t);
    }
}

// resolve the number type an unsuffixed literal adopts from the expected type
// `t`: a number type is adopted as-is (bool is number-like for casts but never
// a literal type), a union adopts its first member the literal class fits.
static ns_type ns_vm_literal_expect(ns_vm *vm, ns_type t, ns_bool flt) {
    if (ns_type_is(t, NS_TYPE_UNION)) {
        ns_symbol *u = &vm->symbols[ns_type_index(t)];
        ns_type fallback = ns_type_unknown;
        for (szt i = 0, l = ns_array_length(u->un.members); i < l; ++i) {
            ns_type m = u->un.members[i];
            if (!ns_type_is_number(m) || ns_type_is(m, NS_TYPE_BOOL)) continue;
            if (ns_type_is_float(m) == flt) return (ns_type){.type = m.type};
            // an int literal can widen into a float member if no int member exists
            if (!flt && ns_type_is_float(m) && ns_type_is_unknown(fallback)) fallback = (ns_type){.type = m.type};
        }
        return fallback;
    }
    if (!ns_type_is_number(t) || ns_type_is(t, NS_TYPE_BOOL)) return ns_type_unknown;
    if (flt && !ns_type_is_float(t)) return ns_type_unknown;
    return (ns_type){.type = t.type};
}

ns_return_type ns_vm_parse_primary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_type t) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->primary_expr.token.type) {
    case NS_TOKEN_INT_LITERAL: {
        ns_type lt;
        switch (n->primary_expr.token.suffix) {
        case NS_NUM_SUFFIX_I8: lt = ns_type_i8; break;
        case NS_NUM_SUFFIX_U8: lt = ns_type_u8; break;
        case NS_NUM_SUFFIX_I16: lt = ns_type_i16; break;
        case NS_NUM_SUFFIX_U16: lt = ns_type_u16; break;
        case NS_NUM_SUFFIX_U32: lt = ns_type_u32; break;
        case NS_NUM_SUFFIX_I64: lt = ns_type_i64; break;
        case NS_NUM_SUFFIX_U64: lt = ns_type_u64; break;
        default: { // unsuffixed: adopt the expected number type, i32 otherwise
            lt = ns_vm_literal_expect(vm, t, false);
            if (ns_type_is_unknown(lt)) lt = ns_type_i32;
        } break;
        }
        n->primary_expr.t = lt;
        return ns_return_ok(type, lt);
    }
    case NS_TOKEN_FLT_LITERAL: {
        // unsuffixed: adopt the expected float type, f32 otherwise
        ns_type lt;
        if (n->primary_expr.token.suffix == NS_NUM_SUFFIX_F16) {
            ns_warn("numeric", "half-float literal fallback to f32 on CPU.\n");
            lt = ns_type_f32;
        } else if (n->primary_expr.token.suffix == NS_NUM_SUFFIX_BF16) {
            ns_warn("numeric", "brain-float literal fallback to f32 on CPU.\n");
            lt = ns_type_f32;
        } else if (n->primary_expr.token.suffix == NS_NUM_SUFFIX_F64) {
            lt = ns_type_f64;
        } else {
            lt = ns_vm_literal_expect(vm, t, true);
        }
        if (ns_type_is_unknown(lt)) lt = ns_type_f32;
        n->primary_expr.t = lt;
        return ns_return_ok(type, lt);
    }
    case NS_TOKEN_STR_LITERAL:
    case NS_TOKEN_STR_FORMAT:
        return ns_return_ok(type, ns_type_str);
    case NS_TOKEN_TRUE:
    case NS_TOKEN_FALSE:
        return ns_return_ok(type, ns_type_bool);
    case NS_TOKEN_NIL:
        return ns_return_ok(type, ns_type_nil);
    case NS_TOKEN_IDENTIFIER:
        return ns_vm_parse_type_by_token(vm, n->primary_expr.token, ns_ast_state_loc(ctx, n->state));
    default:
        break;
    }
    ns_ast_state s = ctx->nodes[i].state;
    return ns_return_error(type, ns_ast_state_loc(ctx, s), NS_ERR_SYNTAX, "invalid primary expr.");
}

ns_return_type ns_vm_parse_call_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_ast_t *callee_n = &ctx->nodes[n->call_expr.callee];
    ns_return_type ret_callee = ns_vm_parse_expr(vm, ctx, n->call_expr.callee, ns_type_nil);
    if (ns_return_is_error(ret_callee)) return ret_callee;

    ns_type fn = ret_callee.r;
    if (ns_type_is(fn, NS_TYPE_UNKNOWN)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, callee_n->state), NS_ERR_EVAL, "invalid callee.");
    }

    ns_symbol *fn_record = &vm->symbols[ns_type_index(fn)];
    if (!fn_record || fn_record->type != NS_SYMBOL_FN) {
        return ns_return_error(type, ns_ast_state_loc(ctx, callee_n->state), NS_ERR_EVAL, "unknown callee.");
    }

    // Registering a closure into a fn-typed struct field, e.g. `v.on_frame({ ... })`.
    // The callee is a member access whose field type is itself a function type and
    // the sole argument is a closure. The closure must match the field's fn type,
    // so check it against `fn` directly rather than against the field-fn's first
    // parameter; the call evaluates to void (it stores the callback, not invokes it).
    if (callee_n->type == NS_AST_MEMBER_EXPR && n->call_expr.arg_count == 1) {
        ns_ast_t *arg0 = &ctx->nodes[n->next];
        i32 arg0_body = arg0->type == NS_AST_EXPR ? arg0->expr.body : n->next;
        if (ctx->nodes[arg0_body].type == NS_AST_BLOCK_EXPR) {
            ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->next, fn);
            if (ns_return_is_error(ret_t)) return ret_t;
            if (!ns_type_match(vm, fn, ret_t.r)) {
                return ns_return_error(type, ns_ast_state_loc(ctx, arg0->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "callback type mismatch.", fn, ret_t.r));
            }
            return ns_return_ok(type, ns_type_void);
        }
    }

    i32 next = n->next;
    for (i32 a_i = 0, l = n->call_expr.arg_count; a_i < l; ++a_i) {
        ns_ast_t arg = ctx->nodes[next];
        // re-fetch each iteration: parsing a block arg pushes a global symbol,
        // which may realloc vm->symbols and move the fn record
        fn_record = &vm->symbols[ns_type_index(fn)];
        ns_type arg_t = fn_record->fn.args[a_i].val.t;

        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, next, arg_t);
        if (ns_return_is_error(ret_t)) return ret_t;

        ns_type t = ret_t.r;
        next = arg.next;
        // a numeric param accepts any numeric arg (converted when the call
        // pushes args in ns_eval_call_expr), matching designated-expr fields
        ns_bool number_ok = !ns_type_is_ref(arg_t) && ns_type_is_number(arg_t) && ns_type_is_number(t);
        if (!number_ok && !ns_type_match(vm, arg_t, t)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, arg.state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "call expr type mismatch.", arg_t, t));
        }
    }
    fn_record = &vm->symbols[ns_type_index(fn)];
    return ns_return_ok(type, fn_record->fn.ret);
}

ns_return_type ns_vm_parse_member_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->member_expr.left, ns_type_infer);
    if (ns_return_is_error(ret_t)) return ret_t;

    ns_type t = ret_t.r;

    ns_ast_t lf = ctx->nodes[n->member_expr.right];
    if (lf.type == NS_AST_PRIMARY_EXPR &&
        (ns_str_equals_STR(lf.primary_expr.token.val, "len") ||
         ns_str_equals_STR(lf.primary_expr.token.val, "size") ||
         ns_str_equals_STR(lf.primary_expr.token.val, "cap"))) {
        if (ns_type_is_array(t) || (ns_type_is(t, NS_TYPE_STRING) && !ns_type_is_array(t)) ||
            ns_type_is(t, NS_TYPE_DICT) || ns_type_is(t, NS_TYPE_SET)) {
            return ns_return_ok(type, ns_type_i32);
        }
    }

    // string .len/.size/.cap yield i32 metadata.
    if (ns_type_is(t, NS_TYPE_STRING) && !ns_type_is_array(t)) {
        ns_ast_t lf = ctx->nodes[n->member_expr.right];
        if (lf.type == NS_AST_PRIMARY_EXPR &&
            (ns_str_equals_STR(lf.primary_expr.token.val, "len") ||
             ns_str_equals_STR(lf.primary_expr.token.val, "size") ||
             ns_str_equals_STR(lf.primary_expr.token.val, "cap"))) {
            return ns_return_ok(type, ns_type_i32);
        }
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown string member.");
    }

    if (ns_type_is_array(t) || ns_type_is(t, NS_TYPE_DICT) || ns_type_is(t, NS_TYPE_SET)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown container member.");
    }

    if (!ns_type_is(t, NS_TYPE_STRUCT)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_label_msg(vm, "member expr type mismatch.", "struct", t));
    }

    ns_ast_t field = ctx->nodes[n->member_expr.right];
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
    return ns_vm_parse_expr(vm, ctx, n->member_expr.right, ns_type_infer);
}

ns_bool ns_vm_parse_type_generable(ns_type t) {
    return ns_type_is(t, NS_TYPE_STRING) || ns_type_is(t, NS_TYPE_ARRAY);
}

ns_return_type ns_vm_parse_gen_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->gen_expr.range) {
        // range bounds are i32; unsuffixed literals adopt it
        ns_return_type from_ret = ns_vm_parse_expr(vm, ctx, n->gen_expr.from, ns_type_i32);
        if (ns_return_is_error(from_ret)) return from_ret;
        ns_return_type to_ret = ns_vm_parse_expr(vm, ctx, n->gen_expr.to, ns_type_i32);
        if (ns_return_is_error(to_ret)) return to_ret;

        ns_type from_t = from_ret.r;
        ns_type to_t = to_ret.r;
        if (!ns_type_is(from_t, NS_TYPE_I32) || !ns_type_is(to_t, NS_TYPE_I32)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_pair_mismatch_msg(vm, "gen expr type mismatch.", "from", from_t, "to", to_t));
        }
        return ns_return_ok(type, ns_type_i32);
    } else {
        ns_return_type from_ret = ns_vm_parse_expr(vm, ctx, n->gen_expr.from, ns_type_infer);
        if (ns_return_is_error(from_ret)) return from_ret;
        ns_type from_t = from_ret.r;
        if (!ns_vm_parse_type_generable(from_t)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_label_msg(vm, "gen expr type mismatch.", "string or array", from_t));
        }
        return ns_return_ok(type, from_t);
    }
}

ns_return_type ns_vm_parse_desig_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol *st = ns_vm_find_symbol(vm, n->desig_expr.name.val, true);
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
        // the field expr adopts the field type so number literals unify
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, field->field_def.expr, f->t);
        if (ns_return_is_error(ret_t)) return ret_t;

        ns_type t = ret_t.r;
        // A numeric field accepts any numeric expr (converted at store time by
        // ns_eval_desig_expr), so float literals (f64) initialize f32 fields.
        ns_bool number_ok = ns_type_is_number(f->t) && ns_type_is_number(t);
        if (!number_ok && !ns_type_equals(t, f->t) && !ns_type_match(vm, f->t, t)) return ns_return_error(type, ns_ast_state_loc(ctx, field->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "designated expr type mismatch.", f->t, t));

        field->field_def.rt.index = field_i;
    }

    return ns_return_ok(type, st->st.st.t);
}

ns_return_type ns_vm_parse_unary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->unary_expr.expr, ns_type_infer);
    if (ns_return_is_error(ret_t)) return ret_t;

    ns_type t = ret_t.r;
    ns_token_t op = n->unary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
        if (!ns_type_is_number(t)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_label_msg(vm, "unary expr type mismatch.", "number", t));
        }
        return ns_return_ok(type, t);
    case NS_TOKEN_CMP_OP:       // logical not: !bool -> bool
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "logical not requires a bool operand.");
        }
        return ns_return_ok(type, ns_type_bool);
    case NS_TOKEN_BIT_INVERT_OP: // bitwise not: ~int -> int (same type)
        if (!ns_type_is_number(t) || ns_type_is_float(t) || ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "bitwise not requires an integer operand.");
        }
        return ns_return_ok(type, t);
    case NS_TOKEN_REF:
        return ns_return_ok(type, ns_type_set_ref(t, true));
    default:
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown unary ops.");
    }
    return ns_return_ok(type, ns_type_unknown);
}

ns_return_type ns_vm_parse_array_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_type t) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->array_expr.literal) {
        ns_type elem_t = ns_type_unknown;
        if (ns_type_is_array(t)) {
            elem_t = t;
            elem_t.array = false;
        }

        if (n->array_expr.elem_count == 0) {
            if (ns_type_is_unknown(elem_t)) {
                return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "empty array literal needs an array target type.");
            }
            n->array_expr.rt = (ns_type){.type = elem_t.type, .ref = elem_t.ref, .array = true, .mut = true, .stack = true, .index = elem_t.index};
            return ns_return_ok(type, n->array_expr.rt);
        }

        i32 next = n->next;
        for (i32 e_i = 0; e_i < n->array_expr.elem_count; ++e_i) {
            ns_ast_t elem = ctx->nodes[next];
            ns_return_type ret_e = ns_vm_parse_expr(vm, ctx, next, ns_type_is_unknown(elem_t) ? ns_type_infer : elem_t);
            if (ns_return_is_error(ret_e)) return ret_e;
            ns_type et = ret_e.r;
            if (ns_type_is_unknown(elem_t)) {
                elem_t = et;
            } else if (!ns_type_equals(elem_t, et)) {
                if (!ns_vm_assign_number_compatible(vm, elem_t, et) && !ns_type_match(vm, elem_t, et)) {
                    return ns_return_error(type, ns_ast_state_loc(ctx, elem.state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "array literal element type mismatch.", elem_t, et));
                }
            }
            next = elem.next;
        }

        n->array_expr.rt = (ns_type){.type = elem_t.type, .ref = elem_t.ref, .array = true, .mut = true, .stack = true, .index = elem_t.index};
        return ns_return_ok(type, n->array_expr.rt);
    }

    ns_ast_t *type = &ctx->nodes[n->array_expr.type];
    ns_return_type ret_t = ns_vm_parse_type(vm, ctx, type);
    if (ns_return_is_error(ret_t)) return ret_t;
    ns_type ctor_t = ret_t.r;
    if (type->type_label.is_dict || type->type_label.is_set) {
        n->array_expr.rt = ctor_t;
        return ns_return_ok(type, ctor_t);
    }
    // Preserve the element type's index (e.g. the struct symbol) so that
    // element access like arr[i].field can resolve the element's type.
    ns_type arr_t = (ns_type){.type = ctor_t.type, .ref = ctor_t.ref, .array = true, .mut = ctor_t.mut, .stack = true, .index = ctor_t.index};
    n->array_expr.rt = arr_t;
    return ns_return_ok(type, arr_t);
}

ns_return_type ns_vm_parse_index_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_l = ns_vm_parse_expr(vm, ctx, n->index_expr.table, ns_type_infer);
    if (ns_return_is_error(ret_l)) return ret_l;
    // array/string indexes are i32 and dict keys use the dict key type, so
    // unsuffixed number literals adopt the right width
    ns_type idx_t = ns_type_is(ret_l.r, NS_TYPE_DICT) ? vm->symbols[ns_type_index(ret_l.r)].ct.key : ns_type_i32;
    ns_return_type ret_r = ns_vm_parse_expr(vm, ctx, n->index_expr.expr, idx_t);
    if (ns_return_is_error(ret_r)) return ret_r;

    ns_type l = ret_l.r;
    ns_type r = ret_r.r;
    // string indexing yields an i32 char code.
    if (ns_type_is(l, NS_TYPE_STRING) && !ns_type_is_array(l)) {
        if (!ns_type_is(r, NS_TYPE_I32)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "index expr type mismatch.", ns_type_i32, r));
        }
        return ns_return_ok(type, ns_type_i32);
    }
    if (ns_type_is(l, NS_TYPE_DICT)) {
        ns_symbol *dict = &vm->symbols[ns_type_index(l)];
        if (!ns_type_match(vm, dict->ct.key, r)) {
            return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "dict key type mismatch.", dict->ct.key, r));
        }
        return ns_return_ok(type, dict->ct.val);
    }
    if (!ns_type_is_array(l)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_label_msg(vm, "index expr type mismatch.", "array, dict, or string", l));
    }
    if (!ns_type_is(r, NS_TYPE_I32)) {
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "index expr type mismatch.", ns_type_i32, r));
    }
    ns_type t = (ns_type){.type = l.type, .ref = l.ref, .array = false, .mut = l.mut, .stack = true, .index = l.index };
    return ns_return_ok(type, t);
}

ns_return_type ns_vm_parse_cast_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->cast_expr.expr, ns_type_infer);
    if (ns_return_is_error(ret_t)) return ret_t;
    ns_type t = ret_t.r;

    ns_return_type ret_cast = ns_vm_parse_type(vm, ctx, n);
    if (ns_return_is_error(ret_cast)) return ret_cast;

    ns_type cast = ret_cast.r;
    if (ns_type_equals(t, cast)) {
        return ns_return_ok(type, t);
    }

    if (ns_type_is_number(t) && ns_type_is_number(cast)) {
        return ns_return_ok(type, cast);
    }
    // Narrow a union to one of its member types (`u as T`). The interpreter
    // tracks the concrete member tag at runtime, so the value round-trips.
    if (ns_type_is(t, NS_TYPE_UNION) && ns_type_match(vm, t, cast)) {
        return ns_return_ok(type, cast);
    }
    // Widen a member value into a union it belongs to (`v as U`).
    if (ns_type_is(cast, NS_TYPE_UNION) && ns_type_match(vm, cast, t)) {
        return ns_return_ok(type, cast);
    }
    // ns_str t_name = ns_vm_get_type_name(vm, t);
    // ns_str cast_name = ns_vm_get_type_name(vm, cast);
    return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "cast expr type mismatch.", cast, t));
}

void ns_vm_parse_use_stmt(ns_vm *vm, ns_ast_ctx *ctx) {
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i]];
        if (n.type != NS_AST_USE_STMT)
            continue;
        ns_str lib = n.use_stmt.lib.val;
        ns_lib_import(vm, lib);
    }
}

ns_type ns_vm_parse_binary_override(ns_vm *vm, ns_type l, ns_type r, ns_token_t op) {
    ns_str l_name = ns_vm_get_type_name(vm, l);
    ns_str r_name = ns_vm_get_type_name(vm, r);
    ns_str fn_name = ns_ops_override_name(l_name, r_name, op);
    ns_symbol *fn = ns_vm_find_symbol(vm, fn_name, true);
    return fn ? fn->fn.ret : ns_type_unknown;
}

ns_bool ns_vm_assign_number_compatible(ns_vm *vm, ns_type dst, ns_type src) {
    if (!ns_type_is_number(dst) || !ns_type_is_number(src)) return false;
    if (ns_type_is_float(src) && !ns_type_is_float(dst)) return false;

    i32 dst_size = ns_type_size(vm, dst);
    i32 src_size = ns_type_size(vm, src);
    if (dst_size < src_size) return false;
    return true;
}

ns_return_type ns_vm_parse_binary_ops_number(ns_ast_ctx *ctx, ns_type t, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_token_t op = n->binary_expr.op;
    switch (op.type) {
    case NS_TOKEN_ADD_OP:
    case NS_TOKEN_MUL_OP:
    case NS_TOKEN_SHIFT_OP:
    case NS_TOKEN_BITWISE_OP:
        return ns_return_ok(type, t);
    case NS_TOKEN_LOGIC_OP:
    case NS_TOKEN_REL_OP:
    case NS_TOKEN_EQ_OP:
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
    } else if (ns_type_is(t, NS_TYPE_STRING)) {
        ns_token_t op = n->binary_expr.op;
        if (op.type == NS_TOKEN_ADD_OP && ns_str_equals(op.val, ns_str_cstr("+")))
            return ns_return_ok(type, ns_type_str);
        if (op.type == NS_TOKEN_EQ_OP || op.type == NS_TOKEN_REL_OP)
            return ns_return_ok(type, ns_type_bool);
        ns_type ret = ns_vm_parse_binary_override(vm, t, t, op);
        if (!ns_type_is_unknown(ret)) return ns_return_ok(type, ret);
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, "unknown string binary ops");
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
    ns_return_type ret_l = ns_vm_parse_expr(vm, ctx, n->binary_expr.left, ns_type_infer);
    if (ns_return_is_error(ret_l)) return ret_l;
    // the right side adopts the assign target type so number literals unify
    ns_return_type ret_r = ns_vm_parse_expr(vm, ctx, n->binary_expr.right, ret_l.r);
    if (ns_return_is_error(ret_r)) return ret_r;
    ns_type l = ret_l.r;
    ns_type r = ret_r.r;
    if (!ns_type_equals(l, r)) {
        if (ns_vm_assign_number_compatible(vm, l, r)) {
            return ns_return_ok(type, l);
        }
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_EVAL, ns_vm_type_mismatch_msg(vm, "assign expr type mismatch.", l, r));
    }
    return ns_return_ok(type, l);
}

ns_return_type ns_vm_parse_binary_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_type t) {
    ns_ast_t *n = &ctx->nodes[i];
    if (n->binary_expr.op.type == NS_TOKEN_ASSIGN_OP || n->binary_expr.op.type == NS_TOKEN_ASSIGN) {
        return ns_vm_parse_assign_expr(vm, ctx, i);
    }

    ns_return_type ret_l = ns_vm_parse_expr(vm, ctx, n->binary_expr.left, t);
    if (ns_return_is_error(ret_l)) return ret_l;
    ns_type l = ret_l.r;
    // the right side adopts the left number type so unsuffixed literals unify
    ns_return_type ret_r = ns_vm_parse_expr(vm, ctx, n->binary_expr.right, ns_type_is_number(l) ? l : t);
    if (ns_return_is_error(ret_r)) return ret_r;

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
            return ns_vm_parse_binary_ops(vm, ctx, t, i);
    }

    return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_pair_mismatch_msg(vm, "binary expr type mismatch.", "left", l, "right", r));
}

#define ns_vm_parse_case_expr(t, fn) case t: return fn(vm, ctx, i);
ns_return_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_type t) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->type) {
    case NS_AST_EXPR: return ns_vm_parse_expr(vm, ctx, n->expr.body, t);
    case NS_AST_BINARY_EXPR: return ns_vm_parse_binary_expr(vm, ctx, i, t);
    case NS_AST_PRIMARY_EXPR: return ns_vm_parse_primary_expr(vm, ctx, i, t);
    case NS_AST_CALL_EXPR: return ns_vm_parse_call_expr(vm, ctx, i);
    case NS_AST_MEMBER_EXPR: return ns_vm_parse_member_expr(vm, ctx, i);
    case NS_AST_GEN_EXPR: return ns_vm_parse_gen_expr(vm, ctx, i);
    case NS_AST_DESIG_EXPR: return ns_vm_parse_desig_expr(vm, ctx, i);
    case NS_AST_UNARY_EXPR: return ns_vm_parse_unary_expr(vm, ctx, i);
    case NS_AST_CAST_EXPR: return ns_vm_parse_cast_expr(vm, ctx, i);
    case NS_AST_ARRAY_EXPR: return ns_vm_parse_array_expr(vm, ctx, i, t);
    case NS_AST_INDEX_EXPR: return ns_vm_parse_index_expr(vm, ctx, i);
    case NS_AST_STR_FMT_EXPR: return ns_vm_parse_str_fmt(vm, ctx, i);
    case NS_AST_BLOCK_EXPR: return ns_vm_parse_block_expr(vm, ctx, i, t);
    default: {
        static char msg[96];
        snprintf(msg, sizeof(msg), "unimplemented expr type: %d.", n->type);
        return ns_return_error(type, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, msg);
    } break;
    }
    return ns_return_ok(type, ns_type_unknown);
}

ns_return_void ns_vm_parse_jump_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    switch (n->jump_stmt.label.type) {
    case NS_TOKEN_RETURN: {
        szt l = ns_array_length(vm->call_stack);
        if (l == 0) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "return stmt not in fn.");
        }
        ns_symbol *callee = vm->call_stack[l - 1].callee;
        ns_fn_symbol *fn = ns_symbol_get_fn(callee);

        // the return expr adopts the fn return type so number literals unify
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->jump_stmt.expr, fn->ret);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};
        ns_type t = ret_t.r;

        // a numeric return slot accepts any numeric expr (converted in
        // ns_eval_return_stmt), matching call args and designated-expr fields
        ns_bool number_ok = !ns_type_is_ref(fn->ret) && ns_type_is_number(fn->ret) && ns_type_is_number(t);
        if (!number_ok && !ns_type_equals(fn->ret, t) && !ns_type_match(vm, fn->ret, t)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "return stmt type mismatch.", fn->ret, t));
        }
    } break;
    case NS_TOKEN_BREAK:
    case NS_TOKEN_CONTINUE:
        break; // loop control: no type checking required
    default:
        return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, "unknown jump stmt type.");
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_assert_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->assert_stmt.expr, ns_type_infer);
    if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

    ns_type t = ret_t.r;
    if (!ns_type_is(t, NS_TYPE_BOOL)) {
        return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "assert stmt expr type mismatch.", ns_type_bool, t));
    }

    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_if_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->if_stmt.condition, ns_type_infer);
    if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

    ns_type t = ret_t.r;
    if (!ns_type_is(t, NS_TYPE_BOOL)) {
        ns_ast_t *cond = &ctx->nodes[n->if_stmt.condition];
        return ns_return_error(void, ns_ast_state_loc(ctx, cond->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "if stmt expr type mismatch.", ns_type_bool, t));
    }

    ns_return_void ret = ns_vm_parse_compound_stmt(vm, ctx, n->if_stmt.body);
    if (ns_return_is_error(ret)) return ret;

    if (n->if_stmt.else_body) {
        // `else if` chains store a nested if-stmt as the else body rather than a
        // compound block; dispatch on the actual node type.
        ns_ast_t *e = &ctx->nodes[n->if_stmt.else_body];
        if (e->type == NS_AST_IF_STMT)
            ret = ns_vm_parse_if_stmt(vm, ctx, n->if_stmt.else_body);
        else
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

        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, cond, ns_type_infer);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

        ns_type t = ret_t.r;
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "loop stmt expr type mismatch.", ns_type_bool, t));
        }
    } else {
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, cond, ns_type_infer);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

        ns_type t = ret_t.r;
        if (!ns_type_is(t, NS_TYPE_BOOL)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "loop stmt expr type mismatch.", ns_type_bool, t));
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

    ns_scope_enter(vm);
    ns_return_void ret = ns_vm_parse_compound_stmt(vm, ctx, n->for_stmt.body);
    if (ns_return_is_error(ret)) return ret;
    ns_scope_exit(vm);

    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_local_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_t *n = &ctx->nodes[i];
    ns_symbol s = (ns_symbol){.type = NS_SYMBOL_VALUE, .parsed = true};
    s.name = n->var_def.name.val;

    ns_ast_t *type = &ctx->nodes[n->var_def.type];
    ns_return_type ret_l = ns_vm_parse_type(vm, ctx, type);
    if (ns_return_is_error(ret_l)) return ns_return_change_type(void, ret_l);

    ns_type l = ret_l.r;
    if (n->var_def.expr != 0) {
        ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, n->var_def.expr, l);
        if (ns_return_is_error(ret_t)) return (ns_return_void){.s = ret_t.s, .e = ret_t.e};

        ns_type t = ret_t.r;
        if (!ns_type_is(l, NS_TYPE_INFER) && !ns_type_is(t, NS_TYPE_FN) && !ns_type_equals(l, t) && !ns_vm_assign_number_compatible(vm, l, t) && !ns_type_match(vm, l, t)) {
            return ns_return_error(void, ns_ast_state_loc(ctx, n->state), NS_ERR_SYNTAX, ns_vm_type_mismatch_msg(vm, "local var def type mismatch.", l, t));
        }
        s.val.t = ns_type_is(l, NS_TYPE_INFER) ? t : l;
        n->var_def.type_size = ns_type_size(vm, s.val.t);
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
        case NS_AST_EXPR: {
            ns_return_type type_ret = ns_vm_parse_expr(vm, ctx, expr_i, ns_type_infer);
            if (ns_return_is_error(type_ret)) return ns_return_change_type(void, type_ret);
        } break;
        case NS_AST_JUMP_STMT:
            ret = ns_vm_parse_jump_stmt(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_VAR_DEF:
            ret = ns_vm_parse_local_var_def(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_ASSERT_STMT:
            ret = ns_vm_parse_assert_stmt(vm, ctx, expr_i);
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
        case NS_AST_LOOP_STMT:
            ret = ns_vm_parse_loop_stmt(vm, ctx, expr_i);
            if (ns_return_is_error(ret)) return ret;
            break;
        case NS_AST_CALL_EXPR:
        case NS_AST_BINARY_EXPR:
        case NS_AST_PRIMARY_EXPR:
        case NS_AST_MEMBER_EXPR:
        case NS_AST_GEN_EXPR:
        case NS_AST_DESIG_EXPR:
        case NS_AST_UNARY_EXPR: {
            ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, expr_i, ns_type_infer);
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
                ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, s_i, ns_type_infer);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
            } break;
            case NS_AST_ASSERT_STMT: {
                ns_return_void ret = ns_vm_parse_assert_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_IF_STMT: {
                ns_return_void ret = ns_vm_parse_if_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_FOR_STMT: {
                ns_return_void ret = ns_vm_parse_for_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_LOOP_STMT: {
                ns_return_void ret = ns_vm_parse_loop_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_JUMP_STMT: {
                ns_return_void ret = ns_vm_parse_jump_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_TYPE_DEF:
            case NS_AST_VAR_DEF:
            case NS_AST_USE_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OP_FN_DEF:
            case NS_AST_STRUCT_DEF:
            case NS_AST_PROGRAM:
                break; // already parsed
            default: {
                ns_str type = ns_ast_type_to_string(n->type);
                // stmt or expr can not be defined in global level
                if (!vm->repl) ns_warn("vm parse", "invalid global ast %.*s\n", type.len, type.data);
            } break;
        }
    }
    return ns_return_ok_void;
}

ns_return_void ns_vm_parse_global_as_main(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_call call = (ns_call){.callee = NULL, .scope_top = ns_array_length(vm->scope_stack), .ret = ns_nil, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = 0};
    ns_array_push(vm->call_stack, call);
    ns_scope_enter(vm);
    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        switch (n->type) {
            case NS_AST_EXPR:
            case NS_AST_CALL_EXPR: {
                ns_return_type ret_t = ns_vm_parse_expr(vm, ctx, s_i, ns_type_infer);
                if (ns_return_is_error(ret_t)) return ns_return_change_type(void, ret_t);
            } break;
            case NS_AST_VAR_DEF: {
                ns_return_void ret = ns_vm_parse_local_var_def(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_ASSERT_STMT: {
                ns_return_void ret = ns_vm_parse_assert_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_IF_STMT: {
                ns_return_void ret = ns_vm_parse_if_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_FOR_STMT: {
                ns_return_void ret = ns_vm_parse_for_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_LOOP_STMT: {
                ns_return_void ret = ns_vm_parse_loop_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_JUMP_STMT: {
                ns_return_void ret = ns_vm_parse_jump_stmt(vm, ctx, s_i);
                if (ns_return_is_error(ret)) return ret;
            } break;
            case NS_AST_USE_STMT:
            case NS_AST_MODULE_STMT:
            case NS_AST_FN_DEF:
            case NS_AST_OP_FN_DEF:
            case NS_AST_STRUCT_DEF:
            case NS_AST_TYPE_DEF:
            case NS_AST_PROGRAM:
                break; // already parsed
            default: {
                ns_str type = ns_ast_type_to_string(n->type);
                if (!vm->repl) ns_warn("vm parse", "invalid global ast %.*s\n", type.len, type.data);
            } break;
        }
    }
    ns_scope_exit(vm);
    ns_array_pop(vm->call_stack);
    return ns_return_ok_void;
}

#define ns_vm_parse_global(fn) \
    ret = fn(vm, ctx); \
    if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);

ns_return_bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_vm_parse_use_stmt(vm, ctx);
    
    ns_return_void ret;
    ns_vm_parse_global(ns_vm_parse_name);
    // Resolve type aliases/unions before fn signatures so they can be used as
    // parameter and return types. Member/struct names are already registered by
    // ns_vm_parse_name, so only their type encoding (not layout) is needed here.
    ns_vm_parse_global(ns_vm_parse_type_def);
    ns_vm_parse_global(ns_vm_parse_fn_def_type);
    ns_vm_parse_global(ns_vm_parse_var_def);
    ns_vm_parse_global(ns_vm_parse_struct_def);
    ns_vm_parse_global(ns_vm_parse_struct_def_ref);
    ns_vm_parse_global(ns_vm_parse_fn_def_body);

    ns_bool main_fn = ns_vm_find_symbol(vm, ns_str_cstr("main"), false) != ns_null;
    ns_bool main_mod = vm->lib.len == 0 || ns_str_equals(vm->lib, ns_str_cstr("main"));

    // if is not main module, skip top level expr, parse top level var def as global var def
    // if is main module and main fn exists, parse top level expr as global expr, top level var def as global var def
    // if is main module and main fn not exists, create main fn and parse top level expr as main fn body, top level var def as local var def
    if (!main_mod || vm->repl) {
        // Module globals were registered by the common parse pass above; non-main
        // modules intentionally skip executable top-level expressions here.
    } else {
        if (main_fn) {
            ret = ns_vm_parse_global_expr(vm, ctx);
            if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
        } else {
            ret = ns_vm_parse_global_as_main(vm, ctx);
            if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
        }
    }

    vm->symbol_top = ns_array_length(vm->symbols);
    return ns_return_ok(bool, true);
}
