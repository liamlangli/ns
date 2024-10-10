#include "ns_ast.h"
#include "ns_tokenize.h"
#include "ns_type.h"
#include "ns_vm.h"

NS_VALUE_TYPE ns_vm_parse_type(ns_vm *vm, ns_token_t t);
ns_record ns_vm_find_record(ns_vm *vm, ns_str s);

NS_VALUE_TYPE ns_vm_parse_record_type(ns_vm *vm, ns_str n) {
    ns_record r = ns_vm_find_record(vm, n);
    if (r.type == NS_RECORD_INVALID) {
        fprintf(stderr, "type not found: ");
        ns_str_printf(n);
        fprintf(stderr, "\n");
        assert(false);
    }
    switch (r.type) {
    case NS_RECORD_VALUE:
        return r.val.type;
    case NS_RECORD_FN:
        return NS_TYPE_FN;
    case NS_RECORD_STRUCT:
        return NS_TYPE_STRUCT;
    default:
        assert(false); // unexpected type
        break;
    }
}

NS_VALUE_TYPE ns_vm_parse_type(ns_vm *vm, ns_token_t t) {
    switch (t.type) {
    case NS_TOKEN_TYPE_INT8:
        return NS_TYPE_I8;
    case NS_TOKEN_TYPE_UINT8:
        return NS_TYPE_U8;
    case NS_TOKEN_TYPE_INT16:
        return NS_TYPE_I16;
    case NS_TOKEN_TYPE_UINT16:
        return NS_TYPE_U16;
    case NS_TOKEN_TYPE_INT32:
        return NS_TYPE_I32;
    case NS_TOKEN_TYPE_UINT32:
        return NS_TYPE_U32;
    case NS_TOKEN_TYPE_INT64:
        return NS_TYPE_I64;
    case NS_TOKEN_TYPE_UINT64:
        return NS_TYPE_U64;
    case NS_TOKEN_TYPE_F32:
        return NS_TYPE_F32;
    case NS_TOKEN_TYPE_F64:
        return NS_TYPE_F64;
    default:
        return ns_vm_parse_record_type(vm, t.val);
    }
    return NS_TYPE_INFER; // inference specific type later
}

ns_record ns_vm_find_record(ns_vm *vm, ns_str s) {
    for (int i = 0; i < vm->record_count; i++) {
        if (ns_str_equals(vm->records[i].name, s)) {
            return vm->records[i];
        }
    }
    return (ns_record){.type = NS_RECORD_INVALID};
}

int ns_vm_push_record(ns_vm *vm, ns_record r) {
    r.index = vm->record_count;
    ns_array_push(vm->records, r);
    return r.index;
}

int ns_vm_parse_fn_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_record r = (ns_record){.type = NS_RECORD_FN};
    r.name = n.fn_def.name.val;
    r.fn.body = n.fn_def.body;
    ns_array_set_length(r.fn.args, n.fn_def.arg_count);
    ns_ast_t *arg = &n;
    for (int i = 0; i < n.fn_def.arg_count; i++) {
        arg = &ctx->nodes[arg->next];
        ns_record arg_record = (ns_record){.type = NS_RECORD_VALUE, .index = i};
        arg_record.name = arg->arg.name.val;
        arg_record.val.type = ns_vm_parse_type(vm, arg->arg.type);
        r.fn.args[i] = arg_record;
    }
    return ns_vm_push_record(vm, r);
}

int ns_vm_parse_struct_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_record r = (ns_record){.type = NS_RECORD_STRUCT};
    r.name = n.struct_def.name.val;
    ns_array_set_length(r.st.fields, n.struct_def.count);
    ns_ast_t *field = &n;
    for (int i = 0; i < n.struct_def.count; i++) {
        field = &ctx->nodes[field->next];
        ns_record field_record = (ns_record){.type = NS_RECORD_VALUE, .index = i};
        field_record.name = field->arg.name.val;
        field_record.val.type = ns_vm_parse_type(vm, field->arg.type);
        r.st.fields[i] = field_record;
    }

    return ns_vm_push_record(vm, r);
}

int ns_vm_parse_var_def(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t n) {
    ns_record r = (ns_record){.type = NS_RECORD_VALUE};
    r.name = n.var_def.name.val;
    r.val.type = ns_vm_parse_type(vm, n.var_def.type);
    return ns_vm_push_record(vm, r);
}

bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx) {
    // parse def
    int begin = ctx->section_begin;
    int end = ctx->section_end;
    for (int i = begin; i < end; ++i) {
        ns_ast_t n = ctx->nodes[ctx->sections[i++]];

        switch (n.type) {
        case NS_AST_FN_DEF:
            ns_vm_parse_fn_def(vm, ctx, n);
            break;
        case NS_AST_STRUCT_DEF:
            ns_vm_parse_struct_def(vm, ctx, n);
            break;
        case NS_AST_VAR_DEF:
            ns_vm_parse_var_def(vm, ctx, n);
            break;
        default:
            break;
        }
    }

    return true;
}