#include "ns_aarch.h"

/*
 * AArch64 code generator from SSA IR.
 *
 * Model mirrors the amd64 backend: every SSA value has a home slot in the stack
 * frame, and each instruction loads its operands into scratch registers,
 * computes, and stores the result back to its slot. This needs no register
 * allocation and is correct for any number of live values. Phis are lowered out
 * of SSA with copies inserted on the incoming edges.
 *
 * Arguments x0..x7, return value x0. Scratch registers: x9/x10 for operands,
 * x11 for the division temporary. Slot v lives at [SP + 8*v].
 */

#define NS_AARCH_X9  9
#define NS_AARCH_X10 10
#define NS_AARCH_X11 11
#define NS_AARCH_SP  31

/* ── intra-function fixup ─────────────────────────────────────────────────── */
typedef struct ns_aarch_fixup {
    u32 off;           /* byte offset of the (unconditional B) branch in text */
    i32 target_block;  /* SSA block index to branch to */
} ns_aarch_fixup;

typedef struct ns_aarch_ctx {
    ns_ssa_fn *fn;
    u8 *text;
    /* block start offsets (byte index), indexed by block id */
    i32 *block_off;
    ns_aarch_fixup *fixups;
    /* inter-function call fixups */
    ns_aarch_call_fixup *call_fixups;
    i32 cur_block;     /* block currently being emitted (for edge copies) */
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

/* CBZ Xt, imm19  (branch if == 0) */
static u32 ns_aarch_cbz(i32 rt, i32 imm19) {
    return 0xB4000000u | (((u32)imm19 & 0x7FFFFu) << 5) | (u32)rt;
}

/* LDR Xt, [SP, #(8*slot)] — unsigned scaled offset, slot < 4096 */
static u32 ns_aarch_ldr_slot(i32 rt, i32 slot) {
    return 0xF9400000u | (((u32)slot & 0xFFFu) << 10) | ((u32)NS_AARCH_SP << 5) | (u32)rt;
}

/* STR Xt, [SP, #(8*slot)] */
static u32 ns_aarch_str_slot(i32 rt, i32 slot) {
    return 0xF9000000u | (((u32)slot & 0xFFFu) << 10) | ((u32)NS_AARCH_SP << 5) | (u32)rt;
}

/* SUB SP, SP, #imm12 (imm 0..4095, unshifted) */
static u32 ns_aarch_sub_sp_imm(i32 imm12) {
    return 0xD10003FFu | (((u32)imm12 & 0xFFFu) << 10);
}

/* MOV SP, X29  (ADD SP, X29, #0) — restore the stack pointer in the epilogue */
static u32 ns_aarch_mov_sp_fp(void) {
    return 0x910003BFu;
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

/* ── stack slot access ────────────────────────────────────────────────────── */
/* Load an SSA value into a register; a negative value id loads nothing. */
static void ns_aarch_load_value(ns_aarch_ctx *c, i32 reg, i32 v) {
    if (v < 0) return;
    ns_aarch_emit_u32(c, ns_aarch_ldr_slot(reg, v));
}

/* Store a register into an SSA value's slot. */
static void ns_aarch_store_value(ns_aarch_ctx *c, i32 v, i32 reg) {
    if (v < 0) return;
    ns_aarch_emit_u32(c, ns_aarch_str_slot(reg, v));
}

/* ── parse constant string ────────────────────────────────────────────────── */
static ns_bool ns_aarch_parse_u64(ns_str s, u64 *out) {
    if (s.len <= 0 || !s.data) return false;
    if (ns_str_equals(s, ns_str_cstr("true"))) {
        *out = 1;
        return true;
    }
    if (ns_str_equals(s, ns_str_cstr("false"))) {
        *out = 0;
        return true;
    }

    i32 start = 0;
    ns_bool neg = false;
    if (s.data[0] == '-' || s.data[0] == '+') {
        neg = s.data[0] == '-';
        start = 1;
        if (start >= s.len) return false;
    }

    if (start + 1 < s.len && s.data[start] == '0' && (s.data[start + 1] == 'x' || s.data[start + 1] == 'X')) {
        u64 v = 0;
        for (i32 i = start + 2; i < s.len; ++i) {
            i8 ch = s.data[i];
            u64 d;
            if (ch >= '0' && ch <= '9') d = (u64)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') d = (u64)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') d = (u64)(ch - 'A' + 10);
            else return false;
            u64 next = (v << 4) | d;
            if (next < v) return false;
            v = next;
        }
        *out = neg ? (u64)(-(i64)v) : v;
        return true;
    }

    ns_bool has_dot = false;
    for (i32 i = start; i < s.len; ++i) {
        if (s.data[i] == '.') { has_dot = true; break; }
        if (s.data[i] < '0' || s.data[i] > '9') return false;
    }
    if (!has_dot) {
        u64 v = 0;
        for (i32 i = start; i < s.len; ++i) {
            if (s.data[i] < '0' || s.data[i] > '9') return false;
            u64 d = v * 10u + (u64)(s.data[i] - '0');
            if (d < v) return false; /* overflow */
            v = d;
        }
        *out = neg ? (u64)(-(i64)v) : v;
        return true;
    }
    /* Float: only accept exact integers */
    f64 fv = ns_str_to_f64(s);
    if (fv < 0.0) {
        f64 pos = -fv;
        u64 iv = (u64)pos;
        if ((f64)iv != pos) return false;
        *out = (u64)(-(i64)iv);
        return true;
    }
    u64 iv = (u64)fv;
    if ((f64)iv != fv) return false;
    *out = iv;
    return true;
}

/* Narrow X9 to the width/signedness of an integer cast's destination type so
 * that e.g. `300 as u8` yields 44. Wider or non-integer targets are no-ops. */
static void ns_aarch_narrow_x9(ns_aarch_ctx *c, ns_type t) {
    u32 rn_rd = ((u32)NS_AARCH_X9 << 5) | (u32)NS_AARCH_X9;
    if (ns_type_is(t, NS_TYPE_I8))       ns_aarch_emit_u32(c, 0x93401C00u | rn_rd); /* SXTB X9, W9 */
    else if (ns_type_is(t, NS_TYPE_U8))  ns_aarch_emit_u32(c, 0x12001C00u | rn_rd); /* AND W9, W9, #0xFF */
    else if (ns_type_is(t, NS_TYPE_I16)) ns_aarch_emit_u32(c, 0x93403C00u | rn_rd); /* SXTH X9, W9 */
    else if (ns_type_is(t, NS_TYPE_U16)) ns_aarch_emit_u32(c, 0x12003C00u | rn_rd); /* AND W9, W9, #0xFFFF */
    else if (ns_type_is(t, NS_TYPE_I32)) ns_aarch_emit_u32(c, 0x93407C00u | rn_rd); /* SXTW X9, W9 */
    else if (ns_type_is(t, NS_TYPE_U32)) ns_aarch_emit_u32(c, 0x2A0003E0u | ((u32)NS_AARCH_X9 << 16) | (u32)NS_AARCH_X9); /* MOV W9, W9 */
}

/* ── phi edge copies ──────────────────────────────────────────────────────── */
/* Before branching from `from` to `to`, materialize `to`'s phis by copying the
 * input that flows in along this edge into each phi's slot. Phi input a comes
 * from block target0, input b from target1 (stashed by the SSA builder). */
static void ns_aarch_emit_edge_copies(ns_aarch_ctx *c, i32 from, i32 to) {
    ns_ssa_block *tb = &c->fn->blocks[to];
    for (i32 ii = 0, il = (i32)ns_array_length(tb->insts); ii < il; ++ii) {
        ns_ssa_inst *inst = &c->fn->insts[tb->insts[ii]];
        if (inst->op != NS_SSA_OP_PHI) break; /* phis lead the block */
        i32 src = -1;
        if (inst->target0 == from) src = inst->a;
        else if (inst->target1 == from) src = inst->b;
        if (src < 0 || inst->dst < 0 || src == inst->dst) continue;
        ns_aarch_load_value(c, NS_AARCH_X9, src);
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    }
}

/* ── instruction emission ─────────────────────────────────────────────────── */
static void ns_aarch_emit_inst(ns_aarch_ctx *c, ns_ssa_inst *inst) {
    switch (inst->op) {
    case NS_SSA_OP_PHI:
        /* Handled by edge copies in predecessors; nothing to emit here. */
        break;
    case NS_SSA_OP_UNDEF: {
        if (inst->dst < 0) break;
        ns_aarch_emit_u32(c, ns_aarch_movz(NS_AARCH_X9, 0, 0));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_PARAM: {
        /* Real parameters carry an arg index 0..7; other PARAM nodes (e.g. a
         * callee name placeholder) hold no runtime value. */
        if (inst->dst < 0 || inst->c < 0 || inst->c >= 8) break;
        ns_aarch_store_value(c, inst->dst, inst->c);
    } break;
    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        u64 val = 0;
        if (!ns_aarch_parse_u64(inst->name, &val)) {
            ns_warn("aarch", "unsupported const '%.*s' in fn %.*s, using 0\n",
                inst->name.len, inst->name.data, c->fn->name.len, c->fn->name.data);
            val = 0;
        }
        ns_aarch_emit_const_u64(c, NS_AARCH_X9, val);
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_COPY: {
        if (inst->dst < 0 || inst->a < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_CAST: {
        if (inst->dst < 0 || inst->a < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_narrow_x9(c, inst->type); /* truncate to a narrower integer target */
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB:
    case NS_SSA_OP_MUL:
    case NS_SSA_OP_BAND: case NS_SSA_OP_AND:
    case NS_SSA_OP_BOR:  case NS_SSA_OP_OR:
    case NS_SSA_OP_BXOR:
    case NS_SSA_OP_SHL:  case NS_SSA_OP_SHR: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
        switch (inst->op) {
        case NS_SSA_OP_ADD:  ns_aarch_emit_u32(c, ns_aarch_add_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        case NS_SSA_OP_SUB:  ns_aarch_emit_u32(c, ns_aarch_sub_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        case NS_SSA_OP_MUL:  ns_aarch_emit_u32(c, ns_aarch_mul_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        case NS_SSA_OP_BAND: case NS_SSA_OP_AND: ns_aarch_emit_u32(c, ns_aarch_and_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        case NS_SSA_OP_BOR:  case NS_SSA_OP_OR:  ns_aarch_emit_u32(c, ns_aarch_orr_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        case NS_SSA_OP_BXOR: ns_aarch_emit_u32(c, ns_aarch_eor_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        case NS_SSA_OP_SHL:  ns_aarch_emit_u32(c, ns_aarch_lslv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        default:             ns_aarch_emit_u32(c, ns_aarch_lsrv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10)); break;
        }
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_DIV: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_sdiv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_MOD: {
        /* Xd = Xn - (Xn / Xm) * Xm  (SDIV + MSUB) */
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_sdiv_rrr(NS_AARCH_X11, NS_AARCH_X9, NS_AARCH_X10));
        ns_aarch_emit_u32(c, ns_aarch_msub_rrrr(NS_AARCH_X9, NS_AARCH_X11, NS_AARCH_X10, NS_AARCH_X9));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_emit_u32(c, ns_aarch_neg_rr(NS_AARCH_X9, NS_AARCH_X9));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_NOT: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_emit_u32(c, 0xF100001Fu | ((u32)NS_AARCH_X9 << 5)); /* CMP X9, #0 */
        ns_aarch_emit_u32(c, ns_aarch_cset_r(NS_AARCH_X9, 1u));      /* CSET X9, EQ */
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
    case NS_SSA_OP_LT: case NS_SSA_OP_LE:
    case NS_SSA_OP_GT: case NS_SSA_OP_GE: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_cmp_rr(NS_AARCH_X9, NS_AARCH_X10));
        u32 inv_cond;
        switch (inst->op) {
        case NS_SSA_OP_EQ: inv_cond = 1u;  break; /* NE */
        case NS_SSA_OP_NE: inv_cond = 0u;  break; /* EQ */
        case NS_SSA_OP_LT: inv_cond = 10u; break; /* GE */
        case NS_SSA_OP_LE: inv_cond = 12u; break; /* GT */
        case NS_SSA_OP_GT: inv_cond = 13u; break; /* LE */
        default:           inv_cond = 11u; break; /* LT (for GE) */
        }
        ns_aarch_emit_u32(c, ns_aarch_cset_r(NS_AARCH_X9, inv_cond));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_ARG: {
        if (c->arg_seq >= 8) {
            ns_warn("aarch", "more than 8 args in fn %.*s, arg %d skipped\n",
                c->fn->name.len, c->fn->name.data, c->arg_seq);
            c->arg_seq++;
            break;
        }
        ns_aarch_load_value(c, c->arg_seq++, inst->a);
    } break;
    case NS_SSA_OP_CALL: {
        c->arg_seq = 0;
        ns_str callee_name = ns_str_null;
        i32 callee_val = inst->a;
        for (i32 ii = 0; ii < (i32)ns_array_length(c->fn->insts); ++ii) {
            if (c->fn->insts[ii].dst == callee_val) { callee_name = c->fn->insts[ii].name; break; }
        }
        u32 bl_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_bl(0));
        if (callee_name.len > 0) {
            ns_aarch_call_fixup cf = {.off = bl_off, .callee = callee_name};
            ns_array_push(c->call_fixups, cf);
        }
        if (inst->dst >= 0) ns_aarch_store_value(c, inst->dst, 0);
    } break;
    case NS_SSA_OP_BR: {
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        u32 cbz_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_cbz(NS_AARCH_X9, 0)); /* if false, go to else path */
        /* then edge */
        ns_aarch_emit_edge_copies(c, c->cur_block, inst->target0);
        u32 b0 = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_b(0));
        ns_array_push(c->fixups, ((ns_aarch_fixup){.off = b0, .target_block = inst->target0}));
        /* patch CBZ to land at the else path */
        i32 cbz_imm = ((i32)ns_array_length(c->text) - (i32)cbz_off) / 4;
        ns_aarch_patch_u32(c->text, cbz_off, ns_aarch_cbz(NS_AARCH_X9, cbz_imm));
        ns_aarch_emit_edge_copies(c, c->cur_block, inst->target1);
        u32 b1 = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_b(0));
        ns_array_push(c->fixups, ((ns_aarch_fixup){.off = b1, .target_block = inst->target1}));
    } break;
    case NS_SSA_OP_JMP: {
        ns_aarch_emit_edge_copies(c, c->cur_block, inst->target0);
        u32 b_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, ns_aarch_b(0));
        ns_array_push(c->fixups, ((ns_aarch_fixup){.off = b_off, .target_block = inst->target0}));
    } break;
    case NS_SSA_OP_RET: {
        if (inst->a >= 0) ns_aarch_load_value(c, 0, inst->a);
        ns_aarch_emit_u32(c, ns_aarch_mov_sp_fp());   /* MOV sp, x29 */
        ns_aarch_emit_u32(c, 0xA8C17BFDu);            /* LDP x29, x30, [sp], #16 */
        ns_aarch_emit_u32(c, ns_aarch_ret());
    } break;
    case NS_SSA_OP_ASSERT: {
        if (inst->a < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_emit_u32(c, ns_aarch_cbnz(NS_AARCH_X9, 2)); /* skip BRK when true */
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

/* Number of stack slots a function needs: one per distinct SSA value. */
static i32 ns_aarch_slot_count(ns_ssa_fn *fn) {
    i32 max_dst = -1;
    for (i32 i = 0, l = (i32)ns_array_length(fn->insts); i < l; ++i) {
        if (fn->insts[i].dst > max_dst) max_dst = fn->insts[i].dst;
    }
    return max_dst + 1;
}

/* ── lower a single SSA function to machine code ─────────────────────────── */
static ns_aarch_fn_bin ns_aarch_lower_fn(ns_ssa_fn *fn) {
    ns_aarch_ctx c = {0};
    c.fn = fn;

    i32 nslots = ns_aarch_slot_count(fn);
    /* Reserve one 8-byte slot per value, rounded up so SP stays 16-aligned. */
    i32 frame = ((nslots * 8) + 15) & ~15;

    /* emit function prologue */
    ns_aarch_emit_u32(&c, 0xA9BF7BFDu); /* STP x29, x30, [sp, #-16]! */
    ns_aarch_emit_u32(&c, 0x910003FDu); /* MOV x29, sp */
    if (frame > 0) ns_aarch_emit_u32(&c, ns_aarch_sub_sp_imm(frame)); /* SUB sp, sp, #frame */

    i32 num_blocks = (i32)ns_array_length(fn->blocks);
    ns_array_set_length(c.block_off, num_blocks);
    for (i32 i = 0; i < num_blocks; ++i) c.block_off[i] = -1;

    for (i32 bi = 0; bi < num_blocks; ++bi) {
        c.block_off[bi] = (i32)ns_array_length(c.text);
        c.cur_block = bi;
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
            ns_aarch_emit_inst(&c, &fn->insts[bb->insts[ii]]);
        }
    }

    /* apply intra-function branch fixups (all unconditional B) */
    for (i32 i = 0, l = (i32)ns_array_length(c.fixups); i < l; ++i) {
        ns_aarch_fixup *fix = &c.fixups[i];
        i32 target_block = fix->target_block;
        if (target_block < 0 || target_block >= num_blocks) continue;
        i32 target_off = c.block_off[target_block];
        if (target_off < 0) continue;
        i32 imm = (target_off - (i32)fix->off) / 4;
        ns_aarch_patch_u32(c.text, fix->off, ns_aarch_b(imm));
    }

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
