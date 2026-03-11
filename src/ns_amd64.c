#include "ns_amd64.h"

/*
 * AMD64 (x86-64) code generator from SSA IR.
 *
 * Calling convention: Microsoft x64 ABI (used by Windows PE executables).
 *   Integer arguments : RCX(1), RDX(2), R8(8), R9(9)
 *   Return value      : RAX(0)
 *   Caller-saved      : RAX, RCX, RDX, R8, R9, R10, R11
 *   Callee-saved      : RBX, RBP, RDI, RSI, RSP, R12, R13, R14, R15
 *
 * Register index map (matches physical AMD64 register encoding):
 *   0=RAX  1=RCX  2=RDX  3=RBX  4=RSP  5=RBP  6=RSI  7=RDI
 *   8=R8   9=R9  10=R10 11=R11 12=R12 13=R13 14=R14 15=R15
 *
 * Virtual registers are allocated starting from R10 (index 10).
 * RSP(4) and RBP(5) are never allocated as virtuals.
 */

/* ── intra-function fixup ─────────────────────────────────────────────────── */
typedef struct ns_amd64_fixup {
    u32 off;          /* byte offset of the rel32 field to patch */
    i32 target_block; /* SSA block index to branch to */
    i32 cond_jcc;     /* JCC opcode byte (-1 = unconditional JMP) */
    i32 cond_reg;     /* register to test for JNZ/JZ (-1 if not used) */
} ns_amd64_fixup;

typedef struct ns_amd64_ctx {
    ns_ssa_fn *fn;
    u8 *text;
    i32 *vreg;        /* virtual-reg → physical-reg map */
    i32 next_gpr;     /* next allocatable GPR index */
    i32 *block_off;   /* start byte offset of each block */
    ns_amd64_fixup *fixups;
    ns_amd64_call_fixup *call_fixups;
    i32 arg_seq;      /* argument passing counter (reset per CALL) */
} ns_amd64_ctx;

/* ── low-level byte emitters ─────────────────────────────────────────────── */
static void ns_amd64_emit_u8(ns_amd64_ctx *c, u8 v) {
    ns_array_push(c->text, v);
}

static void ns_amd64_emit_u32(ns_amd64_ctx *c, u32 v) {
    ns_array_push(c->text, (u8)(v & 0xFF));
    ns_array_push(c->text, (u8)((v >> 8) & 0xFF));
    ns_array_push(c->text, (u8)((v >> 16) & 0xFF));
    ns_array_push(c->text, (u8)((v >> 24) & 0xFF));
}

static void ns_amd64_emit_u64(ns_amd64_ctx *c, u64 v) {
    for (i32 i = 0; i < 8; ++i) {
        ns_array_push(c->text, (u8)((v >> (i * 8)) & 0xFF));
    }
}

static void ns_amd64_patch_u32(u8 *text, u32 off, u32 v) {
    text[off + 0] = (u8)(v & 0xFF);
    text[off + 1] = (u8)((v >> 8) & 0xFF);
    text[off + 2] = (u8)((v >> 16) & 0xFF);
    text[off + 3] = (u8)((v >> 24) & 0xFF);
}

/* ── REX prefix helpers ──────────────────────────────────────────────────── */
/* REX prefix: 0100 WRXB
 *   W=1: 64-bit operand size
 *   R: extends ModRM.reg  (set if reg >= 8)
 *   X: extends SIB.index  (unused here)
 *   B: extends ModRM.rm or opcode reg (set if rm/rd >= 8)
 */
static u8 ns_amd64_rex(i32 reg_r, i32 reg_b) {
    u8 rex = 0x48; /* W=1 */
    if (reg_r >= 8) rex |= 0x04; /* REX.R */
    if (reg_b >= 8) rex |= 0x01; /* REX.B */
    return rex;
}

/* ModRM byte: mod(2) | reg(3) | rm(3) */
static u8 ns_amd64_modrm(i32 mod, i32 reg, i32 rm) {
    return (u8)(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* ── instruction encoding helpers ─────────────────────────────────────────── */

/* MOV r64, imm64 */
static void ns_amd64_emit_mov_ri64(ns_amd64_ctx *c, i32 rd, u64 imm) {
    /* REX.W + (B8+rd) + imm64 */
    u8 rex = 0x48;
    if (rd >= 8) rex |= 0x01; /* REX.B */
    ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, (u8)(0xB8 | (rd & 7)));
    ns_amd64_emit_u64(c, imm);
}

/* MOV r64, r64  (dst = src) — uses MOV r64, r/m64 (opcode 0x8B) */
static void ns_amd64_emit_mov_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    if (dst == src) return;
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src)); /* dst in reg field, src in rm */
    ns_amd64_emit_u8(c, 0x8B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* XOR r32, r32 — zeros a 64-bit register (implicit zero-extension) */
static void ns_amd64_emit_xor32_rr(ns_amd64_ctx *c, i32 rd) {
    /* No REX.W needed: 32-bit XOR zeros upper 32 bits */
    u8 rex = 0;
    if (rd >= 8) rex = 0x41; /* REX.B only */
    if (rex) ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, 0x33);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

/* ADD r64, r/m64  (dst += src) */
static void ns_amd64_emit_add_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x03);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* SUB r64, r/m64  (dst -= src) */
static void ns_amd64_emit_sub_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x2B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* SUB r/m64, imm8 (signed) */
static void ns_amd64_emit_sub_ri8(ns_amd64_ctx *c, i32 dst, i8 imm) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, dst));
    ns_amd64_emit_u8(c, 0x83);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 5, dst)); /* /5 = SUB */
    ns_amd64_emit_u8(c, (u8)imm);
}

/* ADD r/m64, imm8 (signed) */
static void ns_amd64_emit_add_ri8(ns_amd64_ctx *c, i32 dst, i8 imm) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, dst));
    ns_amd64_emit_u8(c, 0x83);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 0, dst)); /* /0 = ADD */
    ns_amd64_emit_u8(c, (u8)imm);
}

/* IMUL r64, r/m64  (dst *= src) */
static void ns_amd64_emit_imul_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, 0xAF);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* NEG r64 */
static void ns_amd64_emit_neg_r(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rd));
    ns_amd64_emit_u8(c, 0xF7);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 3, rd)); /* /3 = NEG */
}

/* AND r64, r/m64 */
static void ns_amd64_emit_and_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x23);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* OR r64, r/m64 */
static void ns_amd64_emit_or_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x0B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* XOR r64, r/m64 */
static void ns_amd64_emit_xor_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x33);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* SHL r64, CL  (rd <<= CL) */
static void ns_amd64_emit_shl_rcl(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rd));
    ns_amd64_emit_u8(c, 0xD3);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 4, rd)); /* /4 = SHL */
}

/* SAR r64, CL  (arithmetic right shift) */
static void ns_amd64_emit_sar_rcl(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rd));
    ns_amd64_emit_u8(c, 0xD3);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 7, rd)); /* /7 = SAR */
}

/* CQO — sign-extend RAX to RDX:RAX */
static void ns_amd64_emit_cqo(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0x48); /* REX.W */
    ns_amd64_emit_u8(c, 0x99);
}

/* IDIV r/m64 — signed divide RDX:RAX by rm; quotient->RAX, remainder->RDX */
static void ns_amd64_emit_idiv_r(ns_amd64_ctx *c, i32 rm) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rm));
    ns_amd64_emit_u8(c, 0xF7);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 7, rm)); /* /7 = IDIV */
}

/* CMP r/m64, r64  — sets flags: rm - reg */
static void ns_amd64_emit_cmp_rr(ns_amd64_ctx *c, i32 rm, i32 reg) {
    ns_amd64_emit_u8(c, ns_amd64_rex(reg, rm));
    ns_amd64_emit_u8(c, 0x39);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, rm));
}

/* CMP r/m64, 0 — TEST r64, r64 (flags from rd & rd) */
static void ns_amd64_emit_test_rr(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(rd, rd));
    ns_amd64_emit_u8(c, 0x85);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

/* SETcc al/cl/... byte register, then MOVZX to 64-bit
 * setcc_opc: second byte of 0F 9x encoding (e.g. 0x94 = SETE) */
static void ns_amd64_emit_setcc_r(ns_amd64_ctx *c, i32 rd, u8 setcc_opc) {
    /* SETcc: 0F 9x /0 — operates on 8-bit register */
    /* For registers >= 8 we need REX.B but no REX.W */
    if (rd >= 8) {
        ns_amd64_emit_u8(c, 0x41); /* REX.B */
    } else if (rd >= 4 && rd <= 7) {
        /* For RSP(4), RBP(5), RSI(6), RDI(7), REX prefix needed to access
         * SPL, BPL, SIL, DIL (vs AH, CH, DH, BH without REX) */
        ns_amd64_emit_u8(c, 0x40); /* REX (no flags) to access low byte */
    }
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, setcc_opc);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 0, rd));
    /* MOVZX r64, r/m8 — zero-extend byte result to 64 bits */
    ns_amd64_emit_u8(c, ns_amd64_rex(rd, rd));
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, 0xB6);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

/* PUSH r64 */
static void ns_amd64_emit_push_r(ns_amd64_ctx *c, i32 rd) {
    if (rd >= 8) ns_amd64_emit_u8(c, 0x41); /* REX.B */
    ns_amd64_emit_u8(c, (u8)(0x50 | (rd & 7)));
}

/* POP r64 */
static void ns_amd64_emit_pop_r(ns_amd64_ctx *c, i32 rd) {
    if (rd >= 8) ns_amd64_emit_u8(c, 0x41); /* REX.B */
    ns_amd64_emit_u8(c, (u8)(0x58 | (rd & 7)));
}

/* JMP rel32 — emits opcode + 4-byte placeholder; returns offset of rel32 */
static u32 ns_amd64_emit_jmp(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0xE9);
    u32 rel_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0); /* placeholder */
    return rel_off;
}

/* Jcc rel32 — conditional jump; jcc_opc2 is second byte of 0F 8x */
static u32 ns_amd64_emit_jcc(ns_amd64_ctx *c, u8 jcc_opc2) {
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, jcc_opc2);
    u32 rel_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0); /* placeholder */
    return rel_off;
}

/* JNZ rel32 (test reg then branch if nonzero) */
static u32 ns_amd64_emit_test_jnz(ns_amd64_ctx *c, i32 reg) {
    ns_amd64_emit_test_rr(c, reg);
    return ns_amd64_emit_jcc(c, 0x85); /* JNZ = 0F 85 */
}

/* CALL rel32 */
static u32 ns_amd64_emit_call_rel32(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0xE8);
    u32 rel_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0); /* placeholder */
    return rel_off;
}

/* RET */
static void ns_amd64_emit_ret(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0xC3);
}

/* INT3 (debug breakpoint / trap) */
static void ns_amd64_emit_int3(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0xCC);
}

/* NOP */
static void ns_amd64_emit_nop(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0x90);
}

/* ── register allocation ──────────────────────────────────────────────────── */

/* Physical register indices for Windows x64 arg passing */
static const i32 NS_AMD64_ARG_REGS[4] = { 1, 2, 8, 9 }; /* RCX, RDX, R8, R9 */

#define NS_AMD64_RAX 0
#define NS_AMD64_RCX 1
#define NS_AMD64_RDX 2
#define NS_AMD64_RBP 5

static i32 ns_amd64_alloc_reg(ns_amd64_ctx *c) {
    /* skip RSP(4) and RBP(5) */
    if (c->next_gpr == 4 || c->next_gpr == 5) c->next_gpr = 6;
    if (c->next_gpr > 15) {
        ns_warn("amd64", "register pressure too high in fn %.*s, reusing R15\n",
            c->fn->name.len, c->fn->name.data);
        return 15;
    }
    return c->next_gpr++;
}

static i32 ns_amd64_reg_for_value(ns_amd64_ctx *c, i32 v) {
    if (v < 0) return NS_AMD64_RAX;
    if (v >= (i32)ns_array_length(c->vreg)) {
        i32 old = ns_array_length(c->vreg);
        ns_array_set_length(c->vreg, v + 1);
        for (i32 i = old; i <= v; ++i) c->vreg[i] = -1;
    }
    if (c->vreg[v] < 0) c->vreg[v] = ns_amd64_alloc_reg(c);
    return c->vreg[v];
}

/* ── constant parsing ─────────────────────────────────────────────────────── */
static ns_bool ns_amd64_parse_u64(ns_str s, u64 *out) {
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
            if (d < v) return false;
            v = d;
        }
        *out = v;
        return true;
    }
    f64 fv = ns_str_to_f64(s);
    if (fv < 0.0) return false;
    u64 iv = (u64)fv;
    if ((f64)iv != fv) return false;
    *out = iv;
    return true;
}

/* ── instruction emission ─────────────────────────────────────────────────── */
static void ns_amd64_emit_inst(ns_amd64_ctx *c, ns_ssa_inst *inst) {
    switch (inst->op) {
    case NS_SSA_OP_UNDEF: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        ns_amd64_emit_xor32_rr(c, rd); /* zero register */
    } break;
    case NS_SSA_OP_PARAM: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        if (inst->c >= 0 && inst->c < 4) {
            i32 arg_reg = NS_AMD64_ARG_REGS[inst->c];
            ns_amd64_emit_mov_rr(c, rd, arg_reg);
        }
    } break;
    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        u64 val = 0;
        if (ns_amd64_parse_u64(inst->name, &val)) {
            if (val == 0) {
                ns_amd64_emit_xor32_rr(c, rd);
            } else {
                ns_amd64_emit_mov_ri64(c, rd, val);
            }
        } else {
            ns_warn("amd64", "unsupported const '%.*s' in fn %.*s, using 0\n",
                inst->name.len, inst->name.data, c->fn->name.len, c->fn->name.data);
            ns_amd64_emit_xor32_rr(c, rd);
        }
    } break;
    case NS_SSA_OP_COPY:
    case NS_SSA_OP_PHI:
    case NS_SSA_OP_CAST: {
        if (inst->dst < 0 || inst->a < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 rm = ns_amd64_reg_for_value(c, inst->a);
        ns_amd64_emit_mov_rr(c, rd, rm);
    } break;
    case NS_SSA_OP_ADD: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_add_rr(c, rd, rb);
    } break;
    case NS_SSA_OP_SUB: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_sub_rr(c, rd, rb);
    } break;
    case NS_SSA_OP_MUL: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_imul_rr(c, rd, rb);
    } break;
    case NS_SSA_OP_DIV:
    case NS_SSA_OP_MOD: {
        /*
         * IDIV requires dividend in RDX:RAX.
         * Steps:
         *   MOV RAX, dividend
         *   CQO          ; sign-extend RAX -> RDX:RAX
         *   IDIV divisor ; RAX = quotient, RDX = remainder
         *   MOV dst, RAX (DIV) or MOV dst, RDX (MOD)
         */
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a); /* dividend */
        i32 rb = ns_amd64_reg_for_value(c, inst->b); /* divisor */
        /* move dividend into RAX */
        ns_amd64_emit_mov_rr(c, NS_AMD64_RAX, ra);
        /* sign-extend */
        ns_amd64_emit_cqo(c);
        /* IDIV (divisor must not be RAX or RDX) */
        if (rb == NS_AMD64_RAX || rb == NS_AMD64_RDX) {
            /* use a scratch register R11 to hold divisor */
            ns_amd64_emit_mov_rr(c, 11, rb);
            ns_amd64_emit_idiv_r(c, 11);
        } else {
            ns_amd64_emit_idiv_r(c, rb);
        }
        /* move result to dst */
        i32 result_reg = (inst->op == NS_SSA_OP_DIV) ? NS_AMD64_RAX : NS_AMD64_RDX;
        ns_amd64_emit_mov_rr(c, rd, result_reg);
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_neg_r(c, rd);
    } break;
    case NS_SSA_OP_NOT: {
        /* logical NOT: rd = (rn == 0) ? 1 : 0 */
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        ns_amd64_emit_test_rr(c, ra);
        ns_amd64_emit_setcc_r(c, rd, 0x94); /* SETE (ZF=1 when ra==0) */
    } break;
    case NS_SSA_OP_BAND:
    case NS_SSA_OP_AND: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_and_rr(c, rd, rb);
    } break;
    case NS_SSA_OP_BOR:
    case NS_SSA_OP_OR: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_or_rr(c, rd, rb);
    } break;
    case NS_SSA_OP_BXOR: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_xor_rr(c, rd, rb);
    } break;
    case NS_SSA_OP_SHL: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        /* SHL uses CL register for shift count */
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_mov_rr(c, NS_AMD64_RCX, rb);
        ns_amd64_emit_shl_rcl(c, rd);
    } break;
    case NS_SSA_OP_SHR: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        ns_amd64_emit_mov_rr(c, rd, ra);
        ns_amd64_emit_mov_rr(c, NS_AMD64_RCX, rb);
        ns_amd64_emit_sar_rcl(c, rd); /* arithmetic right shift */
    } break;
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
    case NS_SSA_OP_LT: case NS_SSA_OP_LE:
    case NS_SSA_OP_GT: case NS_SSA_OP_GE: {
        if (inst->dst < 0) break;
        i32 rd = ns_amd64_reg_for_value(c, inst->dst);
        i32 ra = ns_amd64_reg_for_value(c, inst->a);
        i32 rb = ns_amd64_reg_for_value(c, inst->b);
        /* CMP ra, rb  →  flags from (ra - rb) */
        ns_amd64_emit_cmp_rr(c, ra, rb);
        u8 setcc_opc;
        switch (inst->op) {
        case NS_SSA_OP_EQ: setcc_opc = 0x94; break; /* SETE */
        case NS_SSA_OP_NE: setcc_opc = 0x95; break; /* SETNE */
        case NS_SSA_OP_LT: setcc_opc = 0x9C; break; /* SETL  (signed) */
        case NS_SSA_OP_LE: setcc_opc = 0x9E; break; /* SETLE (signed) */
        case NS_SSA_OP_GT: setcc_opc = 0x9F; break; /* SETG  (signed) */
        default:           setcc_opc = 0x9D; break; /* SETGE (signed) */
        }
        ns_amd64_emit_setcc_r(c, rd, setcc_opc);
    } break;
    case NS_SSA_OP_ARG: {
        /* move argument into the appropriate arg register */
        if (c->arg_seq >= 4) {
            ns_warn("amd64", "more than 4 args in fn %.*s, arg %d skipped\n",
                c->fn->name.len, c->fn->name.data, c->arg_seq);
            c->arg_seq++;
            break;
        }
        i32 src = ns_amd64_reg_for_value(c, inst->a);
        i32 dst_reg = NS_AMD64_ARG_REGS[c->arg_seq++];
        ns_amd64_emit_mov_rr(c, dst_reg, src);
    } break;
    case NS_SSA_OP_CALL: {
        c->arg_seq = 0;
        /* find callee name from the SSA value definition */
        ns_str callee_name = ns_str_null;
        i32 callee_val = inst->a;
        for (i32 ii = 0; ii < (i32)ns_array_length(c->fn->insts); ++ii) {
            if (c->fn->insts[ii].dst == callee_val) {
                callee_name = c->fn->insts[ii].name;
                break;
            }
        }
        /* CALL rel32 placeholder */
        u32 rel_off = ns_amd64_emit_call_rel32(c);
        if (callee_name.len > 0) {
            ns_amd64_call_fixup cf = {.off = rel_off, .callee = callee_name};
            ns_array_push(c->call_fixups, cf);
        }
        /* move return value (RAX) into destination register */
        if (inst->dst >= 0) {
            i32 rd = ns_amd64_reg_for_value(c, inst->dst);
            ns_amd64_emit_mov_rr(c, rd, NS_AMD64_RAX);
        }
    } break;
    case NS_SSA_OP_BR: {
        /*
         * Conditional branch: test cond_reg; JNZ target0; JMP target1
         * Both jumps are patched via fixups after all blocks are laid out.
         */
        i32 cond_reg = ns_amd64_reg_for_value(c, inst->a);
        u32 jnz_rel = ns_amd64_emit_test_jnz(c, cond_reg);
        u32 jmp_rel = ns_amd64_emit_jmp(c);
        ns_amd64_fixup f0 = {.off = jnz_rel, .target_block = inst->target0,
                             .cond_jcc = 0x85, .cond_reg = cond_reg};
        ns_amd64_fixup f1 = {.off = jmp_rel, .target_block = inst->target1,
                             .cond_jcc = -1,  .cond_reg = -1};
        ns_array_push(c->fixups, f0);
        ns_array_push(c->fixups, f1);
    } break;
    case NS_SSA_OP_JMP: {
        u32 jmp_rel = ns_amd64_emit_jmp(c);
        ns_amd64_fixup f = {.off = jmp_rel, .target_block = inst->target0,
                            .cond_jcc = -1, .cond_reg = -1};
        ns_array_push(c->fixups, f);
    } break;
    case NS_SSA_OP_RET: {
        if (inst->a >= 0) {
            i32 rv = ns_amd64_reg_for_value(c, inst->a);
            ns_amd64_emit_mov_rr(c, NS_AMD64_RAX, rv);
        }
        /* epilogue: remove shadow space and restore frame pointer */
        ns_amd64_emit_add_ri8(c, 4, 32); /* ADD RSP, 32 (shadow space) */
        ns_amd64_emit_pop_r(c, NS_AMD64_RBP);
        ns_amd64_emit_ret(c);
    } break;
    case NS_SSA_OP_ASSERT: {
        if (inst->a < 0) break;
        i32 rn = ns_amd64_reg_for_value(c, inst->a);
        /* TEST rn, rn; JNZ +1; INT3 */
        ns_amd64_emit_test_rr(c, rn);
        ns_amd64_emit_jcc(c, 0x85); /* JNZ +1... simplified: just emit INT3 unconditionally on fail */
        /* patch: skip INT3 if nonzero */
        u32 cur = (u32)ns_array_length(c->text);
        c->text[cur - 4] = 1; /* rel32 = 1: skip over the INT3 */
        c->text[cur - 3] = 0;
        c->text[cur - 2] = 0;
        c->text[cur - 1] = 0;
        ns_amd64_emit_int3(c);
    } break;
    case NS_SSA_OP_TRAP: {
        ns_amd64_emit_int3(c);
    } break;
    default:
        ns_warn("amd64", "unsupported ssa op %d in fn %.*s, emitting nop\n",
            inst->op, c->fn->name.len, c->fn->name.data);
        ns_amd64_emit_nop(c);
        break;
    }
}

/* ── lower a single SSA function to AMD64 machine code ───────────────────── */
static ns_amd64_fn_bin ns_amd64_lower_fn(ns_ssa_fn *fn) {
    ns_amd64_ctx c = {0};
    c.fn = fn;
    /* virtual registers start at R10 (10), skipping arg regs and RAX/RDX */
    c.next_gpr = 10;

    /* function prologue: save RBP and establish frame pointer */
    ns_amd64_emit_push_r(&c, NS_AMD64_RBP);            /* PUSH RBP */
    ns_amd64_emit_mov_rr(&c, NS_AMD64_RBP, 4);         /* MOV RBP, RSP */
    ns_amd64_emit_sub_ri8(&c, 4, 32);                  /* SUB RSP, 32 (shadow space) */

    i32 num_blocks = (i32)ns_array_length(fn->blocks);
    ns_array_set_length(c.block_off, num_blocks);
    for (i32 i = 0; i < num_blocks; ++i) c.block_off[i] = -1;

    for (i32 bi = 0; bi < num_blocks; ++bi) {
        c.block_off[bi] = (i32)ns_array_length(c.text);
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            ns_amd64_emit_inst(&c, inst);
        }
    }

    /* patch intra-function branch fixups */
    for (i32 i = 0, l = (i32)ns_array_length(c.fixups); i < l; ++i) {
        ns_amd64_fixup *fix = &c.fixups[i];
        i32 tb = fix->target_block;
        if (tb < 0 || tb >= num_blocks) continue;
        i32 target_off = c.block_off[tb];
        if (target_off < 0) continue;
        /* rel32 = target - (fix->off + 4)  [end of the rel32 field] */
        i32 rel32 = target_off - ((i32)fix->off + 4);
        ns_amd64_patch_u32(c.text, fix->off, (u32)rel32);
    }

    ns_array_free(c.vreg);
    ns_array_free(c.block_off);
    ns_array_free(c.fixups);

    return (ns_amd64_fn_bin){
        .name = fn->name,
        .text = c.text,
        .call_fixups = c.call_fixups
    };
}

/* ── public API ───────────────────────────────────────────────────────────── */
ns_return_ptr ns_amd64_from_ssa(ns_ssa_module *ssa) {
    if (!ssa) {
        return ns_return_error(ptr, ns_code_loc_nil, NS_ERR_SYNTAX, "ssa module is null");
    }

    ns_amd64_module_bin *m = ns_malloc(sizeof(ns_amd64_module_bin));
    memset(m, 0, sizeof(*m));

    ns_asm_target target = {0};
    ns_asm_get_current_target(&target);
    if (target.arch != NS_ARCH_X64) {
        ns_warn("amd64", "current host arch is %.*s; still emitting amd64 bytes\n",
            ns_arch_str(target.arch).len, ns_arch_str(target.arch).data);
    }

    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        ns_amd64_fn_bin fn = ns_amd64_lower_fn(&ssa->fns[i]);
        ns_array_push(m->fns, fn);
    }

    return ns_return_ok(ptr, m);
}

void ns_amd64_print(ns_amd64_module_bin *m) {
    if (!m) return;
    for (i32 fi = 0, fl = (i32)ns_array_length(m->fns); fi < fl; ++fi) {
        ns_amd64_fn_bin *fn = &m->fns[fi];
        printf("amd64 fn %.*s text[%zu bytes]\n",
            fn->name.len, fn->name.data, ns_array_length(fn->text));
        for (i32 i = 0, l = (i32)ns_array_length(fn->text); i < l; ++i) {
            if (i % 16 == 0) printf("  %04x: ", i);
            printf("%02x ", fn->text[i]);
            if ((i % 16) == 15 || i + 1 == l) printf("\n");
        }
    }
}

void ns_amd64_free(ns_amd64_module_bin *m) {
    if (!m) return;
    for (i32 i = 0, l = (i32)ns_array_length(m->fns); i < l; ++i) {
        ns_array_free(m->fns[i].text);
        ns_array_free(m->fns[i].call_fixups);
    }
    ns_array_free(m->fns);
    ns_free(m);
}
