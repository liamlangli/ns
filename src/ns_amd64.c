#include "ns_amd64.h"

/*
 * AMD64 (x86-64) code generator from SSA IR.
 *
 * Model: every SSA value has a home slot in the stack frame. An instruction
 * loads its operands from their slots into scratch registers, computes, and
 * stores the result back to its own slot. This trades code density for a code
 * generator that is correct for an arbitrary number of live values and needs
 * no register allocation or spilling. Control-flow phis are lowered out of SSA
 * with copies inserted on each incoming edge (see ns_amd64_emit_edge_copies).
 *
 * Calling convention: Microsoft x64 ABI (used by Windows PE executables and,
 * for internal ns→ns calls, self-consistent on every host).
 *   Integer arguments : RCX(1), RDX(2), R8(8), R9(9)
 *   Return value      : RAX(0)
 *
 * Scratch registers used by codegen: RAX(0) and R11(11) for operands, RCX(1)
 * for shift counts, RDX(2) for the division remainder. None of these needs to
 * survive across an instruction because every value lives in memory.
 *
 * Register index map (matches physical AMD64 register encoding):
 *   0=RAX 1=RCX 2=RDX 3=RBX 4=RSP 5=RBP 6=RSI 7=RDI
 *   8=R8  9=R9 10=R10 11=R11 12=R12 13=R13 14=R14 15=R15
 */

#define NS_AMD64_RAX 0
#define NS_AMD64_RCX 1
#define NS_AMD64_RDX 2
#define NS_AMD64_RSP 4
#define NS_AMD64_RBP 5
#define NS_AMD64_R11 11

/* Physical register indices for Windows x64 arg passing */
static const i32 NS_AMD64_ARG_REGS[4] = { 1, 2, 8, 9 }; /* RCX, RDX, R8, R9 */

/* ── intra-function branch fixup ──────────────────────────────────────────── */
typedef struct ns_amd64_fixup {
    u32 off;          /* byte offset of the rel32 field to patch */
    i32 target_block; /* SSA block index to branch to */
} ns_amd64_fixup;

typedef struct ns_amd64_ctx {
    ns_ssa_fn *fn;
    u8 *text;
    i32 *block_off;   /* start byte offset of each block */
    ns_amd64_fixup *fixups;
    ns_amd64_call_fixup *call_fixups;
    i32 cur_block;    /* block currently being emitted (for edge copies) */
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

/* REX prefix: 0100 WRXB  (W=64-bit, R extends reg, B extends rm/base) */
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

/* ── memory (stack slot) access, base RBP ────────────────────────────────── */
/* Slot v lives at [RBP - 8*(v+1)]. */
static i32 ns_amd64_slot_disp(i32 v) {
    return -8 * (v + 1);
}

/* MOV reg, [RBP + disp32] — load a slot into a register */
static void ns_amd64_emit_load(ns_amd64_ctx *c, i32 reg, i32 disp) {
    ns_amd64_emit_u8(c, ns_amd64_rex(reg, NS_AMD64_RBP));
    ns_amd64_emit_u8(c, 0x8B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(2, reg, NS_AMD64_RBP)); /* mod=10 disp32, rm=RBP */
    ns_amd64_emit_u32(c, (u32)disp);
}

/* MOV [RBP + disp32], reg — store a register into a slot */
static void ns_amd64_emit_store(ns_amd64_ctx *c, i32 disp, i32 reg) {
    ns_amd64_emit_u8(c, ns_amd64_rex(reg, NS_AMD64_RBP));
    ns_amd64_emit_u8(c, 0x89);
    ns_amd64_emit_u8(c, ns_amd64_modrm(2, reg, NS_AMD64_RBP));
    ns_amd64_emit_u32(c, (u32)disp);
}

/* Load an SSA value into a register; a negative value id loads nothing. */
static void ns_amd64_load_value(ns_amd64_ctx *c, i32 reg, i32 v) {
    if (v < 0) { return; }
    ns_amd64_emit_load(c, reg, ns_amd64_slot_disp(v));
}

/* Store a register into an SSA value's slot. */
static void ns_amd64_store_value(ns_amd64_ctx *c, i32 v, i32 reg) {
    if (v < 0) return;
    ns_amd64_emit_store(c, ns_amd64_slot_disp(v), reg);
}

/* ── register/imm instruction encoding ───────────────────────────────────── */
static void ns_amd64_emit_mov_ri64(ns_amd64_ctx *c, i32 rd, u64 imm) {
    u8 rex = 0x48;
    if (rd >= 8) rex |= 0x01;
    ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, (u8)(0xB8 | (rd & 7)));
    ns_amd64_emit_u64(c, imm);
}

static void ns_amd64_emit_mov_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    if (dst == src) return;
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x8B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

static void ns_amd64_emit_xor32_rr(ns_amd64_ctx *c, i32 rd) {
    u8 rex = 0;
    if (rd >= 8) rex = 0x41;
    if (rex) ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, 0x33);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

static void ns_amd64_emit_add_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x03);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

static void ns_amd64_emit_sub_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x2B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

/* op r/m64, imm32 (sign-extended); /digit selects ADD(0) or SUB(5) */
static void ns_amd64_emit_alu_ri32(ns_amd64_ctx *c, i32 digit, i32 dst, i32 imm) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, dst));
    ns_amd64_emit_u8(c, 0x81);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, digit, dst));
    ns_amd64_emit_u32(c, (u32)imm);
}

static void ns_amd64_emit_imul_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, 0xAF);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

static void ns_amd64_emit_neg_r(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rd));
    ns_amd64_emit_u8(c, 0xF7);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 3, rd)); /* /3 = NEG */
}

static void ns_amd64_emit_and_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x23);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

static void ns_amd64_emit_or_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x0B);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

static void ns_amd64_emit_xor_rr(ns_amd64_ctx *c, i32 dst, i32 src) {
    ns_amd64_emit_u8(c, ns_amd64_rex(dst, src));
    ns_amd64_emit_u8(c, 0x33);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, dst, src));
}

static void ns_amd64_emit_shift_rcl(ns_amd64_ctx *c, i32 rd, i32 digit) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rd));
    ns_amd64_emit_u8(c, 0xD3);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, digit, rd)); /* /4=SHL /7=SAR */
}

static void ns_amd64_emit_cqo(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0x48);
    ns_amd64_emit_u8(c, 0x99);
}

static void ns_amd64_emit_idiv_r(ns_amd64_ctx *c, i32 rm) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rm));
    ns_amd64_emit_u8(c, 0xF7);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 7, rm)); /* /7 = IDIV */
}

static void ns_amd64_emit_cmp_rr(ns_amd64_ctx *c, i32 rm, i32 reg) {
    ns_amd64_emit_u8(c, ns_amd64_rex(reg, rm));
    ns_amd64_emit_u8(c, 0x39);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, rm));
}

static void ns_amd64_emit_test_rr(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(rd, rd));
    ns_amd64_emit_u8(c, 0x85);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

/* SETcc r/m8 then MOVZX to 64-bit. setcc_opc is the second byte of 0F 9x. */
static void ns_amd64_emit_setcc_r(ns_amd64_ctx *c, i32 rd, u8 setcc_opc) {
    if (rd >= 8) {
        ns_amd64_emit_u8(c, 0x41);
    } else if (rd >= 4 && rd <= 7) {
        ns_amd64_emit_u8(c, 0x40);
    }
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, setcc_opc);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 0, rd));
    ns_amd64_emit_u8(c, ns_amd64_rex(rd, rd));
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, 0xB6);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

static void ns_amd64_emit_push_r(ns_amd64_ctx *c, i32 rd) {
    if (rd >= 8) ns_amd64_emit_u8(c, 0x41);
    ns_amd64_emit_u8(c, (u8)(0x50 | (rd & 7)));
}

/* JMP rel32 — returns the offset of the rel32 field for later patching. */
static u32 ns_amd64_emit_jmp(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0xE9);
    u32 rel_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0);
    return rel_off;
}

/* Jcc rel32 — jcc_opc2 is the second byte of 0F 8x. Returns the rel32 offset. */
static u32 ns_amd64_emit_jcc(ns_amd64_ctx *c, u8 jcc_opc2) {
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, jcc_opc2);
    u32 rel_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0);
    return rel_off;
}

static u32 ns_amd64_emit_call_rel32(ns_amd64_ctx *c) {
    ns_amd64_emit_u8(c, 0xE8);
    u32 rel_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0);
    return rel_off;
}

static void ns_amd64_emit_leave(ns_amd64_ctx *c) { ns_amd64_emit_u8(c, 0xC9); }
static void ns_amd64_emit_ret(ns_amd64_ctx *c) { ns_amd64_emit_u8(c, 0xC3); }
static void ns_amd64_emit_int3(ns_amd64_ctx *c) { ns_amd64_emit_u8(c, 0xCC); }

/* ── constant parsing ─────────────────────────────────────────────────────── */
static ns_bool ns_amd64_parse_u64(ns_str s, u64 *out) {
    if (s.len <= 0 || !s.data) return false;
    if (ns_str_equals(s, ns_str_cstr("true"))) { *out = 1; return true; }
    if (ns_str_equals(s, ns_str_cstr("false"))) { *out = 0; return true; }

    /* A leading sign appears in signed enum members lowered to constants. */
    i32 start = 0;
    ns_bool neg = false;
    if (s.data[0] == '-' || s.data[0] == '+') {
        neg = s.data[0] == '-';
        start = 1;
        if (start >= s.len) return false;
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
            if (d < v) return false;
            v = d;
        }
        *out = neg ? (u64)(-(i64)v) : v;
        return true;
    }
    f64 fv = ns_str_to_f64(s);
    if (fv < 0.0) return false;
    u64 iv = (u64)fv;
    if ((f64)iv != fv) return false;
    *out = neg ? (u64)(-(i64)iv) : iv;
    return true;
}

/* Narrow RAX to the width/signedness of an integer cast's destination type so
 * that e.g. `300 as u8` yields 44. Wider or non-integer targets are no-ops. */
static void ns_amd64_narrow_rax(ns_amd64_ctx *c, ns_type t) {
    if (ns_type_is(t, NS_TYPE_I8)) {        /* MOVSX RAX, AL  */ ns_amd64_emit_u8(c, 0x48); ns_amd64_emit_u8(c, 0x0F); ns_amd64_emit_u8(c, 0xBE); ns_amd64_emit_u8(c, 0xC0); }
    else if (ns_type_is(t, NS_TYPE_U8)) {   /* MOVZX RAX, AL  */ ns_amd64_emit_u8(c, 0x48); ns_amd64_emit_u8(c, 0x0F); ns_amd64_emit_u8(c, 0xB6); ns_amd64_emit_u8(c, 0xC0); }
    else if (ns_type_is(t, NS_TYPE_I16)) {  /* MOVSX RAX, AX  */ ns_amd64_emit_u8(c, 0x48); ns_amd64_emit_u8(c, 0x0F); ns_amd64_emit_u8(c, 0xBF); ns_amd64_emit_u8(c, 0xC0); }
    else if (ns_type_is(t, NS_TYPE_U16)) {  /* MOVZX RAX, AX  */ ns_amd64_emit_u8(c, 0x48); ns_amd64_emit_u8(c, 0x0F); ns_amd64_emit_u8(c, 0xB7); ns_amd64_emit_u8(c, 0xC0); }
    else if (ns_type_is(t, NS_TYPE_I32)) {  /* MOVSXD RAX, EAX */ ns_amd64_emit_u8(c, 0x48); ns_amd64_emit_u8(c, 0x63); ns_amd64_emit_u8(c, 0xC0); }
    else if (ns_type_is(t, NS_TYPE_U32)) {  /* MOV EAX, EAX (zero-extend) */ ns_amd64_emit_u8(c, 0x89); ns_amd64_emit_u8(c, 0xC0); }
}

/* ── phi edge copies ──────────────────────────────────────────────────────── */
/* Before a branch from `from` to `to`, materialize `to`'s phis by copying the
 * input that flows in along this edge into each phi's slot. Phi input a comes
 * from block target0, input b from target1 (stashed by the SSA builder). */
static void ns_amd64_emit_edge_copies(ns_amd64_ctx *c, i32 from, i32 to) {
    ns_ssa_block *tb = &c->fn->blocks[to];
    for (i32 ii = 0, il = (i32)ns_array_length(tb->insts); ii < il; ++ii) {
        ns_ssa_inst *inst = &c->fn->insts[tb->insts[ii]];
        if (inst->op != NS_SSA_OP_PHI) break; /* phis lead the block */
        i32 src = -1;
        if (inst->target0 == from) src = inst->a;
        else if (inst->target1 == from) src = inst->b;
        if (src < 0 || inst->dst < 0 || src == inst->dst) continue;
        ns_amd64_load_value(c, NS_AMD64_RAX, src);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    }
}

/* ── instruction emission ─────────────────────────────────────────────────── */
static void ns_amd64_emit_inst(ns_amd64_ctx *c, ns_ssa_inst *inst) {
    switch (inst->op) {
    case NS_SSA_OP_PHI:
        /* Handled by edge copies in predecessors; nothing to emit here. */
        break;
    case NS_SSA_OP_UNDEF: {
        if (inst->dst < 0) break;
        ns_amd64_emit_xor32_rr(c, NS_AMD64_RAX);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_PARAM: {
        /* Real parameters carry an arg index 0..3; other PARAM nodes (e.g. a
         * callee name placeholder) hold no runtime value. */
        if (inst->dst < 0 || inst->c < 0 || inst->c >= 4) break;
        ns_amd64_store_value(c, inst->dst, NS_AMD64_ARG_REGS[inst->c]);
    } break;
    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        u64 val = 0;
        if (ns_amd64_parse_u64(inst->name, &val)) {
            if (val == 0) ns_amd64_emit_xor32_rr(c, NS_AMD64_RAX);
            else ns_amd64_emit_mov_ri64(c, NS_AMD64_RAX, val);
        } else {
            ns_warn("amd64", "unsupported const '%.*s' in fn %.*s, using 0\n",
                inst->name.len, inst->name.data, c->fn->name.len, c->fn->name.data);
            ns_amd64_emit_xor32_rr(c, NS_AMD64_RAX);
        }
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_COPY: {
        if (inst->dst < 0 || inst->a < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_CAST: {
        if (inst->dst < 0 || inst->a < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_narrow_rax(c, inst->type); /* truncate to a narrower integer target */
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB:
    case NS_SSA_OP_MUL:
    case NS_SSA_OP_BAND: case NS_SSA_OP_AND:
    case NS_SSA_OP_BOR:  case NS_SSA_OP_OR:
    case NS_SSA_OP_BXOR: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_load_value(c, NS_AMD64_R11, inst->b);
        switch (inst->op) {
        case NS_SSA_OP_ADD: ns_amd64_emit_add_rr(c, NS_AMD64_RAX, NS_AMD64_R11); break;
        case NS_SSA_OP_SUB: ns_amd64_emit_sub_rr(c, NS_AMD64_RAX, NS_AMD64_R11); break;
        case NS_SSA_OP_MUL: ns_amd64_emit_imul_rr(c, NS_AMD64_RAX, NS_AMD64_R11); break;
        case NS_SSA_OP_BAND: case NS_SSA_OP_AND: ns_amd64_emit_and_rr(c, NS_AMD64_RAX, NS_AMD64_R11); break;
        case NS_SSA_OP_BOR:  case NS_SSA_OP_OR:  ns_amd64_emit_or_rr(c, NS_AMD64_RAX, NS_AMD64_R11); break;
        default:            ns_amd64_emit_xor_rr(c, NS_AMD64_RAX, NS_AMD64_R11); break;
        }
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_DIV:
    case NS_SSA_OP_MOD: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a); /* dividend */
        ns_amd64_load_value(c, NS_AMD64_R11, inst->b); /* divisor */
        ns_amd64_emit_cqo(c);                          /* sign-extend into RDX:RAX */
        ns_amd64_emit_idiv_r(c, NS_AMD64_R11);
        ns_amd64_store_value(c, inst->dst, inst->op == NS_SSA_OP_DIV ? NS_AMD64_RAX : NS_AMD64_RDX);
    } break;
    case NS_SSA_OP_SHL:
    case NS_SSA_OP_SHR: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_load_value(c, NS_AMD64_RCX, inst->b); /* count in CL */
        ns_amd64_emit_shift_rcl(c, NS_AMD64_RAX, inst->op == NS_SSA_OP_SHL ? 4 : 7);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_emit_neg_r(c, NS_AMD64_RAX);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_NOT: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_emit_test_rr(c, NS_AMD64_RAX);
        ns_amd64_emit_setcc_r(c, NS_AMD64_RAX, 0x94); /* SETE: 1 when operand==0 */
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
    case NS_SSA_OP_LT: case NS_SSA_OP_LE:
    case NS_SSA_OP_GT: case NS_SSA_OP_GE: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_load_value(c, NS_AMD64_R11, inst->b);
        ns_amd64_emit_cmp_rr(c, NS_AMD64_RAX, NS_AMD64_R11); /* flags from a - b */
        u8 setcc_opc;
        switch (inst->op) {
        case NS_SSA_OP_EQ: setcc_opc = 0x94; break; /* SETE */
        case NS_SSA_OP_NE: setcc_opc = 0x95; break; /* SETNE */
        case NS_SSA_OP_LT: setcc_opc = 0x9C; break; /* SETL  */
        case NS_SSA_OP_LE: setcc_opc = 0x9E; break; /* SETLE */
        case NS_SSA_OP_GT: setcc_opc = 0x9F; break; /* SETG  */
        default:           setcc_opc = 0x9D; break; /* SETGE */
        }
        ns_amd64_emit_setcc_r(c, NS_AMD64_RAX, setcc_opc);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_ARG: {
        if (c->arg_seq >= 4) {
            ns_warn("amd64", "more than 4 args in fn %.*s, arg %d skipped\n",
                c->fn->name.len, c->fn->name.data, c->arg_seq);
            c->arg_seq++;
            break;
        }
        ns_amd64_load_value(c, NS_AMD64_ARG_REGS[c->arg_seq++], inst->a);
    } break;
    case NS_SSA_OP_CALL: {
        c->arg_seq = 0;
        ns_str callee_name = ns_str_null;
        i32 callee_val = inst->a;
        for (i32 ii = 0; ii < (i32)ns_array_length(c->fn->insts); ++ii) {
            if (c->fn->insts[ii].dst == callee_val) { callee_name = c->fn->insts[ii].name; break; }
        }
        u32 rel_off = ns_amd64_emit_call_rel32(c);
        if (callee_name.len > 0) {
            ns_amd64_call_fixup cf = {.off = rel_off, .callee = callee_name};
            ns_array_push(c->call_fixups, cf);
        }
        if (inst->dst >= 0) ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_BR: {
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_emit_test_rr(c, NS_AMD64_RAX);
        u32 jz_off = ns_amd64_emit_jcc(c, 0x84); /* JZ → else path */
        /* then edge */
        ns_amd64_emit_edge_copies(c, c->cur_block, inst->target0);
        u32 j0 = ns_amd64_emit_jmp(c);
        ns_array_push(c->fixups, ((ns_amd64_fixup){.off = j0, .target_block = inst->target0}));
        /* patch JZ to land here, at the else path */
        ns_amd64_patch_u32(c->text, jz_off, (u32)((i32)ns_array_length(c->text) - ((i32)jz_off + 4)));
        ns_amd64_emit_edge_copies(c, c->cur_block, inst->target1);
        u32 j1 = ns_amd64_emit_jmp(c);
        ns_array_push(c->fixups, ((ns_amd64_fixup){.off = j1, .target_block = inst->target1}));
    } break;
    case NS_SSA_OP_JMP: {
        ns_amd64_emit_edge_copies(c, c->cur_block, inst->target0);
        u32 j = ns_amd64_emit_jmp(c);
        ns_array_push(c->fixups, ((ns_amd64_fixup){.off = j, .target_block = inst->target0}));
    } break;
    case NS_SSA_OP_RET: {
        if (inst->a >= 0) ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_emit_leave(c); /* MOV RSP,RBP ; POP RBP */
        ns_amd64_emit_ret(c);
    } break;
    case NS_SSA_OP_ASSERT: {
        if (inst->a < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_amd64_emit_test_rr(c, NS_AMD64_RAX);
        u32 jnz = ns_amd64_emit_jcc(c, 0x85); /* JNZ over the trap when true */
        ns_amd64_emit_int3(c);
        ns_amd64_patch_u32(c->text, jnz, (u32)((i32)ns_array_length(c->text) - ((i32)jnz + 4)));
    } break;
    case NS_SSA_OP_TRAP: {
        ns_amd64_emit_int3(c);
    } break;
    default:
        ns_warn("amd64", "unsupported ssa op %d in fn %.*s, emitting nop\n",
            inst->op, c->fn->name.len, c->fn->name.data);
        ns_amd64_emit_u8(c, 0x90);
        break;
    }
}

/* Number of stack slots a function needs: one per distinct SSA value. */
static i32 ns_amd64_slot_count(ns_ssa_fn *fn) {
    i32 max_dst = -1;
    for (i32 i = 0, l = (i32)ns_array_length(fn->insts); i < l; ++i) {
        if (fn->insts[i].dst > max_dst) max_dst = fn->insts[i].dst;
    }
    return max_dst + 1;
}

/* ── lower a single SSA function to AMD64 machine code ───────────────────── */
static ns_amd64_fn_bin ns_amd64_lower_fn(ns_ssa_fn *fn) {
    ns_amd64_ctx c = {0};
    c.fn = fn;

    i32 nslots = ns_amd64_slot_count(fn);
    /* Reserve slots plus 32 bytes of callee shadow space, rounded to a 16-byte
     * boundary so RSP stays aligned at every call site. */
    i32 frame = (nslots * 8) + 32;
    frame = (frame + 15) & ~15;

    /* prologue: establish a frame pointer and reserve the frame */
    ns_amd64_emit_push_r(&c, NS_AMD64_RBP);
    ns_amd64_emit_mov_rr(&c, NS_AMD64_RBP, NS_AMD64_RSP);
    ns_amd64_emit_alu_ri32(&c, 5, NS_AMD64_RSP, frame); /* SUB RSP, frame */

    i32 num_blocks = (i32)ns_array_length(fn->blocks);
    ns_array_set_length(c.block_off, num_blocks);
    for (i32 i = 0; i < num_blocks; ++i) c.block_off[i] = -1;

    for (i32 bi = 0; bi < num_blocks; ++bi) {
        c.block_off[bi] = (i32)ns_array_length(c.text);
        c.cur_block = bi;
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
            ns_amd64_emit_inst(&c, &fn->insts[bb->insts[ii]]);
        }
    }

    /* patch intra-function branch fixups */
    for (i32 i = 0, l = (i32)ns_array_length(c.fixups); i < l; ++i) {
        ns_amd64_fixup *fix = &c.fixups[i];
        i32 tb = fix->target_block;
        if (tb < 0 || tb >= num_blocks || c.block_off[tb] < 0) continue;
        i32 rel32 = c.block_off[tb] - ((i32)fix->off + 4);
        ns_amd64_patch_u32(c.text, fix->off, (u32)rel32);
    }

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
