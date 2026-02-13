#include "ns_aarch.h"

typedef struct ns_aarch_ctx {
    ns_ssa_fn *fn;
    u8 *text;
    i32 *vreg;
    i32 next_gpr;
} ns_aarch_ctx;

static void ns_aarch_emit_u32(ns_aarch_ctx *c, u32 inst) {
    ns_array_push(c->text, (u8)(inst & 0xFF));
    ns_array_push(c->text, (u8)((inst >> 8) & 0xFF));
    ns_array_push(c->text, (u8)((inst >> 16) & 0xFF));
    ns_array_push(c->text, (u8)((inst >> 24) & 0xFF));
}

static u32 ns_aarch_movz(i32 rd, u16 imm16, i32 lsl_bits) {
    i32 hw = (lsl_bits / 16) & 0x3;
    return 0xD2800000u | ((u32)hw << 21) | ((u32)imm16 << 5) | (u32)rd;
}

static u32 ns_aarch_mov_rr(i32 rd, i32 rm) {
    // alias: ORR Xd, XZR, Xm
    return 0xAA0003E0u | ((u32)rm << 16) | (u32)rd;
}

static u32 ns_aarch_add_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x8B000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_sub_rrr(i32 rd, i32 rn, i32 rm) {
    return 0xCB000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_ret(void) {
    return 0xD65F03C0u;
}

static u32 ns_aarch_nop(void) {
    return 0xD503201Fu;
}

static i32 ns_aarch_alloc_reg(ns_aarch_ctx *c) {
    if (c->next_gpr > 27) {
        ns_warn("aarch", "register pressure too high in function %.*s, reusing x27\n", c->fn->name.len, c->fn->name.data);
        return 27;
    }
    return c->next_gpr++;
}

static i32 ns_aarch_reg_for_value(ns_aarch_ctx *c, i32 v) {
    if (v < 0) return 0;
    if (v >= (i32)ns_array_length(c->vreg)) {
        i32 old = ns_array_length(c->vreg);
        ns_array_set_length(c->vreg, v + 1);
        for (i32 i = old; i <= v; ++i) {
            c->vreg[i] = -1;
        }
    }
    if (c->vreg[v] < 0) {
        c->vreg[v] = ns_aarch_alloc_reg(c);
    }
    return c->vreg[v];
}

static ns_bool ns_aarch_parse_u16(ns_str s, u16 *out) {
    if (s.len <= 0 || !s.data) return false;
    i32 i = 0, dot = 0;
    for (; i < s.len; ++i) {
        if (s.data[i] == '.') { dot++; continue; }
        if (s.data[i] < '0' || s.data[i] > '9') return false;
    }

    if (dot == 0) {
        u32 v = 0;
        for (i = 0; i < s.len; ++i) {
            v = (v * 10u) + (u32)(s.data[i] - '0');
            if (v > 65535u) return false;
        }
        *out = (u16)v;
        return true;
    }

    f64 fv = ns_str_to_f64(s);
    if (fv < 0.0 || fv > 65535.0) return false;
    u32 iv = (u32)fv;
    if ((f64)iv != fv) return false;
    *out = (u16)iv;
    return true;
}

static void ns_aarch_emit_inst(ns_aarch_ctx *c, ns_ssa_inst *inst) {
    switch (inst->op) {
    case NS_SSA_OP_PARAM: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        if (inst->c >= 0 && inst->c < 8) {
            i32 arg_reg = inst->c;
            if (rd != arg_reg) {
                ns_aarch_emit_u32(c, ns_aarch_mov_rr(rd, arg_reg));
            }
        }
    } break;
    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        u16 imm16 = 0;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        if (ns_aarch_parse_u16(inst->name, &imm16)) {
            ns_aarch_emit_u32(c, ns_aarch_movz(rd, imm16, 0));
        } else {
            ns_warn("aarch", "unsupported const literal '%.*s' in fn %.*s, using 0\n", inst->name.len, inst->name.data, c->fn->name.len, c->fn->name.data);
            ns_aarch_emit_u32(c, ns_aarch_movz(rd, 0, 0));
        }
    } break;
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB: {
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        if (inst->op == NS_SSA_OP_ADD) ns_aarch_emit_u32(c, ns_aarch_add_rrr(rd, rn, rm));
        else ns_aarch_emit_u32(c, ns_aarch_sub_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_COPY: {
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rm = ns_aarch_reg_for_value(c, inst->a);
        ns_aarch_emit_u32(c, ns_aarch_mov_rr(rd, rm));
    } break;
    case NS_SSA_OP_RET: {
        if (inst->a >= 0) {
            i32 rv = ns_aarch_reg_for_value(c, inst->a);
            if (rv != 0) ns_aarch_emit_u32(c, ns_aarch_mov_rr(0, rv));
        }
        ns_aarch_emit_u32(c, ns_aarch_ret());
    } break;
    default:
        ns_warn("aarch", "unsupported ssa op %d in fn %.*s, emitting nop\n", inst->op, c->fn->name.len, c->fn->name.data);
        ns_aarch_emit_u32(c, ns_aarch_nop());
        break;
    }
}

static ns_aarch_fn_bin ns_aarch_lower_fn(ns_ssa_fn *fn) {
    ns_aarch_ctx c = {0};
    c.fn = fn;
    c.next_gpr = 9; // keep x0..x7 for args and x8 for indirect result/syscall

    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; ++bi) {
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            ns_aarch_emit_inst(&c, inst);
        }
    }

    ns_array_free(c.vreg);
    return (ns_aarch_fn_bin){.name = fn->name, .text = c.text};
}

ns_return_ptr ns_aarch_from_ssa(ns_ssa_module *ssa) {
    if (!ssa) {
        return ns_return_error(ptr, ns_code_loc_nil, NS_ERR_SYNTAX, "ssa module is null");
    }

    ns_aarch_module_bin *m = ns_malloc(sizeof(ns_aarch_module_bin));
    memset(m, 0, sizeof(*m));

    ns_asm_target target = {0};
    ns_asm_get_current_target(&target);
    if (target.arch != NS_ARCH_AARCH64) {
        ns_warn("aarch", "current host arch is %.*s; still emitting aarch64 bytes\n", ns_arch_str(target.arch).len, ns_arch_str(target.arch).data);
    }

    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        ns_aarch_fn_bin fn = ns_aarch_lower_fn(&ssa->fns[i]);
        ns_array_push(m->fns, fn);
    }

    return ns_return_ok(ptr, m);
}

void ns_aarch_print(ns_aarch_module_bin *m) {
    if (!m) return;
    for (i32 fi = 0, fl = (i32)ns_array_length(m->fns); fi < fl; ++fi) {
        ns_aarch_fn_bin *fn = &m->fns[fi];
        printf("aarch64 fn %.*s text[%zu bytes]\n", fn->name.len, fn->name.data, ns_array_length(fn->text));
        for (i32 i = 0, l = (i32)ns_array_length(fn->text); i < l; i += 4) {
            if (i % 16 == 0) printf("  %04x: ", i);
            if (i + 3 < l) {
                u32 inst = (u32)fn->text[i] | ((u32)fn->text[i + 1] << 8) | ((u32)fn->text[i + 2] << 16) | ((u32)fn->text[i + 3] << 24);
                printf("%08x ", inst);
            } else {
                for (i32 j = i; j < l; ++j) {
                    printf("%02x", fn->text[j]);
                }
                printf(" ");
            }
            if ((i % 16) == 12 || i + 4 >= l) printf("\n");
        }
    }
}

void ns_aarch_free(ns_aarch_module_bin *m) {
    if (!m) return;
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_array_free(m->fns[i].text);
    }
    ns_array_free(m->fns);
    ns_free(m);
}
