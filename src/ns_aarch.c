#include "ns_aarch.h"

/* ── intra-function fixup ─────────────────────────────────────────────────── */
typedef struct ns_aarch_fixup {
    u32 off;           /* byte offset of the branch instruction in text */
    i32 target_block;  /* SSA block index to branch to */
    i32 cond_reg;      /* physical register for CBNZ (-1 = unconditional B) */
} ns_aarch_fixup;

typedef struct ns_aarch_ctx {
    ns_ssa_fn *fn;
    u8 *text;
    i32 *vreg;
    i32 next_gpr;
    /* block start offsets (byte index), indexed by block id */
    i32 *block_off;
    ns_aarch_fixup *fixups;
    /* inter-function call fixups */
    ns_aarch_call_fixup *call_fixups;
    /* argument passing counter (reset after each CALL) */
    i32 arg_seq;
} ns_aarch_ctx;

/* ── encoding helpers ─────────────────────────────────────────────────────── */
static void ns_aarch_emit_u32(ns_aarch_ctx *c, u32 inst) {
    ns_array_push(c->text, (u8)(inst & 0xFF));
    ns_array_push(c->text, (u8)((inst >> 8) & 0xFF));
    ns_array_push(c->text, (u8)((inst >> 16) & 0xFF));
    ns_array_push(c->text, (u8)((inst >> 24) & 0xFF));
}

static void ns_aarch_patch_u32(u8 *text, u32 off, u32 inst) {
    text[off + 0] = (u8)(inst & 0xFF);
    text[off + 1] = (u8)((inst >> 8) & 0xFF);
    text[off + 2] = (u8)((inst >> 16) & 0xFF);
    text[off + 3] = (u8)((inst >> 24) & 0xFF);
}

/* MOVZ Xd, #imm16, LSL #lsl_bits */
static u32 ns_aarch_movz(i32 rd, u16 imm16, i32 lsl_bits) {
    i32 hw = (lsl_bits / 16) & 0x3;
    return 0xD2800000u | ((u32)hw << 21) | ((u32)imm16 << 5) | (u32)rd;
}

/* MOVK Xd, #imm16, LSL #lsl_bits */
static u32 ns_aarch_movk(i32 rd, u16 imm16, i32 lsl_bits) {
    i32 hw = (lsl_bits / 16) & 0x3;
    return 0xF2800000u | ((u32)hw << 21) | ((u32)imm16 << 5) | (u32)rd;
}

/* ORR Xd, XZR, Xm  (MOV alias) */
static u32 ns_aarch_mov_rr(i32 rd, i32 rm) {
    return 0xAA0003E0u | ((u32)rm << 16) | (u32)rd;
}

/* ADD Xd, Xn, Xm */
static u32 ns_aarch_add_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x8B000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* SUB Xd, Xn, Xm */
static u32 ns_aarch_sub_rrr(i32 rd, i32 rn, i32 rm) {
    return 0xCB000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* MUL Xd, Xn, Xm  (MADD Xd, Xn, Xm, XZR) */
static u32 ns_aarch_mul_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9B007C00u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* SDIV Xd, Xn, Xm */
static u32 ns_aarch_sdiv_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9AC00C00u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* MSUB Xd, Xn, Xm, Xa  (Xd = Xa - Xn*Xm, used for MOD) */
static u32 ns_aarch_msub_rrrr(i32 rd, i32 rn, i32 rm, i32 ra) {
    return 0x9B008000u | ((u32)rm << 16) | ((u32)ra << 10) | ((u32)rn << 5) | (u32)rd;
}

/* NEG Xd, Xm  (SUB Xd, XZR, Xm) */
static u32 ns_aarch_neg_rr(i32 rd, i32 rm) {
    return 0xCB0003E0u | ((u32)rm << 16) | (u32)rd;
}

/* AND Xd, Xn, Xm */
static u32 ns_aarch_and_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x8A000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* ORR Xd, Xn, Xm */
static u32 ns_aarch_orr_rrr(i32 rd, i32 rn, i32 rm) {
    return 0xAA000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* EOR Xd, Xn, Xm */
static u32 ns_aarch_eor_rrr(i32 rd, i32 rn, i32 rm) {
    return 0xCA000000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* LSLV Xd, Xn, Xm */
static u32 ns_aarch_lslv_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9AC02000u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* LSRV Xd, Xn, Xm */
static u32 ns_aarch_lsrv_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9AC02400u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* CMP Xn, Xm  (SUBS XZR, Xn, Xm) */
static u32 ns_aarch_cmp_rr(i32 rn, i32 rm) {
    return 0xEB00001Fu | ((u32)rm << 16) | ((u32)rn << 5);
}

/* CSET Xd, cond  (CSINC Xd, XZR, XZR, inv_cond) */
static u32 ns_aarch_cset_r(i32 rd, u32 inv_cond) {
    return 0x9A9F07E0u | (inv_cond << 12) | (u32)rd;
}

/* RET */
static u32 ns_aarch_ret(void) { return 0xD65F03C0u; }

/* NOP */
static u32 ns_aarch_nop(void) { return 0xD503201Fu; }

/* BRK #0 */
static u32 ns_aarch_brk0(void) { return 0xD4200000u; }

/* CBNZ Xt, imm19  (branch if != 0) */
static u32 ns_aarch_cbnz(i32 rt, i32 imm19) {
    return 0xB5000000u | (((u32)imm19 & 0x7FFFFu) << 5) | (u32)rt;
}

/* B imm26 */
static u32 ns_aarch_b(i32 imm26) {
    return 0x14000000u | ((u32)imm26 & 0x3FFFFFFu);
}

/* BL imm26 */
static u32 ns_aarch_bl(i32 imm26) {
    return 0x94000000u | ((u32)imm26 & 0x3FFFFFFu);
}

/* ── emit a 64-bit constant into rd ──────────────────────────────────────── */
static void ns_aarch_emit_const_u64(ns_aarch_ctx *c, i32 rd, u64 val) {
    if (val == 0) {
        ns_aarch_emit_u32(c, ns_aarch_movz(rd, 0, 0));
        return;
    }
    ns_bool first = true;
    for (i32 hw = 0; hw < 4; ++hw) {
        u16 chunk = (u16)((val >> (hw * 16)) & 0xFFFF);
        if (chunk == 0) continue;
        if (first) {
            ns_aarch_emit_u32(c, ns_aarch_movz(rd, chunk, hw * 16));
            first = false;
        } else {
            ns_aarch_emit_u32(c, ns_aarch_movk(rd, chunk, hw * 16));
        }
    }
}

/* ── register allocation ──────────────────────────────────────────────────── */
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

/* ── parse constant string ────────────────────────────────────────────────── */
static ns_bool ns_aarch_parse_u64(ns_str s, u64 *out) {
    if (s.len <= 0 || !s.data) return false;
    ns_bool has_dot = false;
    for (i32 i = 0; i < s.len; ++i) {
        if (s.data[i] == '.') { has_dot = true; break; }
        if (s.data[i] < '0' || s.data[i] > '9') return false;
    }
    if (!has_dot) {
        u64 v = 0;
        for (i32 i = 0; i < s.len; ++i) {
            if (s.data[i] < '0' || s.data[i] > '9') return false;
            u64 d = v * 10u + (u64)(s.data[i] - '0');
            if (d < v) return false; /* overflow */
            v = d;
        }
        *out = v;
        return true;
    }
    /* Float: only accept exact non-negative integers */
    f64 fv = ns_str_to_f64(s);
    if (fv < 0.0) return false;
    u64 iv = (u64)fv;
    if ((f64)iv != fv) return false;
    *out = iv;
    return true;
}

/* ── instruction emission ─────────────────────────────────────────────────── */
static void ns_aarch_emit_inst(ns_aarch_ctx *c, ns_ssa_inst *inst) {
    switch (inst->op) {
    case NS_SSA_OP_UNDEF: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        ns_aarch_emit_u32(c, ns_aarch_movz(rd, 0, 0));
    } break;
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
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        u64 val = 0;
        if (ns_aarch_parse_u64(inst->name, &val)) {
            ns_aarch_emit_const_u64(c, rd, val);
        } else {
            ns_warn("aarch", "unsupported const literal '%.*s' in fn %.*s, using 0\n",
                inst->name.len, inst->name.data, c->fn->name.len, c->fn->name.data);
            ns_aarch_emit_u32(c, ns_aarch_movz(rd, 0, 0));
        }
    } break;
    case NS_SSA_OP_COPY: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rm = ns_aarch_reg_for_value(c, inst->a);
        if (rd != rm) ns_aarch_emit_u32(c, ns_aarch_mov_rr(rd, rm));
    } break;
    case NS_SSA_OP_PHI: {
        /* simplified: treat as copy from first incoming value */
        if (inst->dst < 0 || inst->a < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rm = ns_aarch_reg_for_value(c, inst->a);
        if (rd != rm) ns_aarch_emit_u32(c, ns_aarch_mov_rr(rd, rm));
    } break;
    case NS_SSA_OP_CAST: {
        /* treat as identity copy for now */
        if (inst->dst < 0 || inst->a < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rm = ns_aarch_reg_for_value(c, inst->a);
        if (rd != rm) ns_aarch_emit_u32(c, ns_aarch_mov_rr(rd, rm));
    } break;
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        if (inst->op == NS_SSA_OP_ADD) ns_aarch_emit_u32(c, ns_aarch_add_rrr(rd, rn, rm));
        else                           ns_aarch_emit_u32(c, ns_aarch_sub_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_MUL: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_mul_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_DIV: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_sdiv_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_MOD: {
        /* Xd = Xn % Xm = Xn - (Xn / Xm) * Xm  (SDIV + MSUB) */
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        /* use rd as a temp for the quotient - safe if rd != rn/rm */
        ns_aarch_emit_u32(c, ns_aarch_sdiv_rrr(rd, rn, rm));         /* rd = rn / rm */
        ns_aarch_emit_u32(c, ns_aarch_msub_rrrr(rd, rd, rm, rn));    /* rd = rn - rd*rm */
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rm = ns_aarch_reg_for_value(c, inst->a);
        ns_aarch_emit_u32(c, ns_aarch_neg_rr(rd, rm));
    } break;
    case NS_SSA_OP_NOT: {
        /* logical NOT: rd = (rn == 0) ? 1 : 0 */
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        ns_aarch_emit_u32(c, 0xF100001Fu | ((u32)rn << 5));  /* CMP Xn, #0 */
        ns_aarch_emit_u32(c, ns_aarch_cset_r(rd, 1u));       /* CSET Xd, EQ (inv=NE=1) */
    } break;
    case NS_SSA_OP_BAND: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_and_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_BOR:
    case NS_SSA_OP_AND:
    case NS_SSA_OP_OR: {
        /* bitwise/logical OR: for booleans AND/OR == BAND/BOR */
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_orr_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_BXOR: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_eor_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_SHL: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_lslv_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_SHR: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_lsrv_rrr(rd, rn, rm));
    } break;
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
    case NS_SSA_OP_LT: case NS_SSA_OP_LE:
    case NS_SSA_OP_GT: case NS_SSA_OP_GE: {
        if (inst->dst < 0) break;
        i32 rd = ns_aarch_reg_for_value(c, inst->dst);
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        i32 rm = ns_aarch_reg_for_value(c, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_cmp_rr(rn, rm));
        /* CSET Xd, cond -- inv_cond values for signed integer comparison */
        u32 inv_cond;
        switch (inst->op) {
        case NS_SSA_OP_EQ: inv_cond = 1u;  break; /* NE */
        case NS_SSA_OP_NE: inv_cond = 0u;  break; /* EQ */
        case NS_SSA_OP_LT: inv_cond = 10u; break; /* GE */
        case NS_SSA_OP_LE: inv_cond = 12u; break; /* GT */
        case NS_SSA_OP_GT: inv_cond = 13u; break; /* LE */
        default:           inv_cond = 11u; break; /* LT (for GE) */
        }
        ns_aarch_emit_u32(c, ns_aarch_cset_r(rd, inv_cond));
    } break;
    case NS_SSA_OP_ARG: {
        /* move argument value into x0..x7 in order */
        if (c->arg_seq >= 8) {
            ns_warn("aarch", "more than 8 args in fn %.*s, arg %d skipped\n",
                c->fn->name.len, c->fn->name.data, c->arg_seq);
            c->arg_seq++;
            break;
        }
        i32 src = ns_aarch_reg_for_value(c, inst->a);
        i32 dst_reg = c->arg_seq++;
        if (src != dst_reg) ns_aarch_emit_u32(c, ns_aarch_mov_rr(dst_reg, src));
    } break;
    case NS_SSA_OP_CALL: {
        c->arg_seq = 0;
        /* find the SSA instruction that defined the callee value to get its name */
        ns_str callee_name = ns_str_null;
        i32 callee_val = inst->a;
        for (i32 ii = 0; ii < (i32)ns_array_length(c->fn->insts); ++ii) {
            if (c->fn->insts[ii].dst == callee_val) {
                callee_name = c->fn->insts[ii].name;
                break;
            }
        }
        /* emit BL placeholder and record inter-function fixup */
        u32 bl_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_bl(0));
        if (callee_name.len > 0) {
            ns_aarch_call_fixup cf = {.off = bl_off, .callee = callee_name};
            ns_array_push(c->call_fixups, cf);
        }
        /* move return value (x0) into destination register */
        if (inst->dst >= 0) {
            i32 rd = ns_aarch_reg_for_value(c, inst->dst);
            if (rd != 0) ns_aarch_emit_u32(c, ns_aarch_mov_rr(rd, 0));
        }
    } break;
    case NS_SSA_OP_BR: {
        /* CBNZ Xcond, target0;  B target1  (both patched via fixups) */
        i32 cond_reg = ns_aarch_reg_for_value(c, inst->a);
        u32 cbnz_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_cbnz(cond_reg, 0)); /* placeholder */
        u32 b_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_b(0));               /* placeholder */
        ns_aarch_fixup f0 = {.off = cbnz_off, .target_block = inst->target0, .cond_reg = cond_reg};
        ns_aarch_fixup f1 = {.off = b_off,    .target_block = inst->target1, .cond_reg = -1};
        ns_array_push(c->fixups, f0);
        ns_array_push(c->fixups, f1);
    } break;
    case NS_SSA_OP_JMP: {
        u32 b_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_b(0));
        ns_aarch_fixup f = {.off = b_off, .target_block = inst->target0, .cond_reg = -1};
        ns_array_push(c->fixups, f);
    } break;
    case NS_SSA_OP_RET: {
        if (inst->a >= 0) {
            i32 rv = ns_aarch_reg_for_value(c, inst->a);
            if (rv != 0) ns_aarch_emit_u32(c, ns_aarch_mov_rr(0, rv));
        }
        /* epilogue: restore frame pointer and link register */
        ns_aarch_emit_u32(c, 0xA8C17BFDu); /* LDP x29, x30, [sp], #16 */
        ns_aarch_emit_u32(c, ns_aarch_ret());
    } break;
    case NS_SSA_OP_ASSERT: {
        if (inst->a < 0) break;
        i32 rn = ns_aarch_reg_for_value(c, inst->a);
        /* CBNZ rn, +2 (skip BRK if condition is true) */
        ns_aarch_emit_u32(c, ns_aarch_cbnz(rn, 2));
        ns_aarch_emit_u32(c, ns_aarch_brk0());
    } break;
    case NS_SSA_OP_TRAP: {
        ns_aarch_emit_u32(c, ns_aarch_brk0());
    } break;
    default:
        ns_warn("aarch", "unsupported ssa op %d in fn %.*s, emitting nop\n",
            inst->op, c->fn->name.len, c->fn->name.data);
        ns_aarch_emit_u32(c, ns_aarch_nop());
        break;
    }
}

/* ── lower a single SSA function to machine code ─────────────────────────── */
static ns_aarch_fn_bin ns_aarch_lower_fn(ns_ssa_fn *fn) {
    ns_aarch_ctx c = {0};
    c.fn = fn;
    c.next_gpr = 9; /* x0..x7 reserved for args, x8 for indirect result */

    /* emit function prologue */
    ns_aarch_emit_u32(&c, 0xA9BF7BFDu); /* STP x29, x30, [sp, #-16]! */
    ns_aarch_emit_u32(&c, 0x910003FDu); /* MOV x29, sp (ADD x29, sp, #0) */

    i32 num_blocks = (i32)ns_array_length(fn->blocks);
    ns_array_set_length(c.block_off, num_blocks);
    for (i32 i = 0; i < num_blocks; ++i) c.block_off[i] = -1;

    for (i32 bi = 0; bi < num_blocks; ++bi) {
        c.block_off[bi] = (i32)ns_array_length(c.text);
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            ns_aarch_emit_inst(&c, inst);
        }
    }

    /* apply intra-function branch fixups */
    for (i32 i = 0, l = (i32)ns_array_length(c.fixups); i < l; ++i) {
        ns_aarch_fixup *fix = &c.fixups[i];
        i32 target_block = fix->target_block;
        if (target_block < 0 || target_block >= num_blocks) continue;
        i32 target_off = c.block_off[target_block];
        if (target_off < 0) continue;
        i32 delta = target_off - (i32)fix->off; /* signed byte delta */
        i32 imm = delta / 4;                    /* instruction-count delta */
        if (fix->cond_reg < 0) {
            /* unconditional B */
            ns_aarch_patch_u32(c.text, fix->off, ns_aarch_b(imm));
        } else {
            /* CBNZ Xreg, target */
            ns_aarch_patch_u32(c.text, fix->off, ns_aarch_cbnz(fix->cond_reg, imm));
        }
    }

    ns_array_free(c.vreg);
    ns_array_free(c.block_off);
    ns_array_free(c.fixups);

    return (ns_aarch_fn_bin){.name = fn->name, .text = c.text, .call_fixups = c.call_fixups};
}

/* ── public API ───────────────────────────────────────────────────────────── */
ns_return_ptr ns_aarch_from_ssa(ns_ssa_module *ssa) {
    if (!ssa) {
        return ns_return_error(ptr, ns_code_loc_nil, NS_ERR_SYNTAX, "ssa module is null");
    }

    ns_aarch_module_bin *m = ns_malloc(sizeof(ns_aarch_module_bin));
    memset(m, 0, sizeof(*m));

    ns_asm_target target = {0};
    ns_asm_get_current_target(&target);
    if (target.arch != NS_ARCH_AARCH64) {
        ns_warn("aarch", "current host arch is %.*s; still emitting aarch64 bytes\n",
            ns_arch_str(target.arch).len, ns_arch_str(target.arch).data);
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
                u32 inst = (u32)fn->text[i] | ((u32)fn->text[i + 1] << 8)
                         | ((u32)fn->text[i + 2] << 16) | ((u32)fn->text[i + 3] << 24);
                printf("%08x ", inst);
            } else {
                for (i32 j = i; j < l; ++j) printf("%02x", fn->text[j]);
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
        ns_array_free(m->fns[i].call_fixups);
    }
    ns_array_free(m->fns);
    ns_free(m);
}
