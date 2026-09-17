#include "ns_amd64.h"

#include <string.h>

/*
 * AMD64 (x86-64) code generator from SSA IR.
 *
 * Model mirrors the aarch64 backend: every SSA value has a home slot in the
 * stack frame. An instruction loads its operands from their slots into scratch
 * registers, computes, and stores the result back to its own slot. This trades
 * code density for a code generator that is correct for an arbitrary number of
 * live values and needs no register allocation or spilling. Control-flow phis
 * are lowered out of SSA with copies inserted on each incoming edge (see
 * ns_amd64_emit_edge_copies).
 *
 * Calling convention: the host ABI, so a compiled program calls the C runtime
 * (ns_rt_*) and dlopen'd feature modules directly.
 *   System V (Linux, BSD): integer args RDI, RSI, RDX, RCX, R8, R9; floats in
 *                          XMM0-7; return in RAX / XMM0.
 *   Microsoft x64 (PE)   : integer args RCX, RDX, R8, R9, aliased positionally
 *                          with XMM0-3; 32 bytes of shadow space per call.
 * ns-to-ns calls pass every value, floats included, in the integer registers:
 * an ns value is 64 bits of payload and both sides agree on that.
 *
 * Scratch registers: RAX and R11 hold operands, RCX the shift count, RDX the
 * division remainder, XMM0/XMM1 the floating-point operands. None of them has
 * to survive an instruction because every value lives in memory.
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

#define NS_AMD64_XMM0 0
#define NS_AMD64_XMM1 1

/* Argument registers, in order, for each ABI. */
static const i32 NS_AMD64_SYSV_INT_REGS[6] = { 7, 6, 2, 1, 8, 9 }; /* RDI RSI RDX RCX R8 R9 */
static const i32 NS_AMD64_WIN_INT_REGS[4] = { 1, 2, 8, 9 };        /* RCX RDX R8 R9 */

#define NS_AMD64_EXTRA_MAX 64
/* One spill slot per register-passed argument, for the ffi pointer dance, plus
 * one for the target of an indirect call. */
#define NS_AMD64_FFI_SCRATCH 8
/* Bytes the Microsoft x64 ABI reserves below the outgoing arguments. */
#define NS_AMD64_SHADOW 32

/* ── intra-function branch fixup ──────────────────────────────────────────── */
typedef struct ns_amd64_fixup {
    u32 off;          /* byte offset of the rel32 field to patch */
    i32 target_block; /* SSA block index to branch to */
} ns_amd64_fixup;

typedef struct ns_amd64_ctx {
    ns_ssa_module *ssa;
    ns_ssa_fn *fn;
    u8 *text;
    i32 *block_off;   /* start byte offset of each block */
    ns_amd64_fixup *fixups;
    ns_amd64_call_fixup *call_fixups;
    i32 cur_block;    /* block currently being emitted (for edge copies) */
    i32 arg_seq;      /* argument passing counter (reset per CALL) */
    i32 extra_args[NS_AMD64_EXTRA_MAX];
    i32 nextra;
    /* first slot past the SSA values, used to park converted ffi pointers */
    i32 scratch_base;
    /* slot holding the target of an indirect call while its args are set up */
    i32 indirect_slot;
    /* host ABI */
    ns_bool win_abi;
    const i32 *int_regs;
    i32 nint_regs;
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

/* MOV [RSP + disp32], reg — park an outgoing stack argument */
static void ns_amd64_emit_store_rsp(ns_amd64_ctx *c, i32 disp, i32 reg) {
    ns_amd64_emit_u8(c, ns_amd64_rex(reg, NS_AMD64_RSP));
    ns_amd64_emit_u8(c, 0x89);
    ns_amd64_emit_u8(c, ns_amd64_modrm(2, reg, NS_AMD64_RSP));
    ns_amd64_emit_u8(c, 0x24); /* SIB: base=RSP, no index */
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
    if (rd >= 8) rex = 0x45;
    if (rex) ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, 0x33);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

/* MOV r32, imm32 — the short form for a constant that fits, zero-extended. */
static void ns_amd64_emit_mov_ri32(ns_amd64_ctx *c, i32 rd, u32 imm) {
    if (imm == 0) { ns_amd64_emit_xor32_rr(c, rd); return; }
    if (rd >= 8) ns_amd64_emit_u8(c, 0x41);
    ns_amd64_emit_u8(c, (u8)(0xB8 | (rd & 7)));
    ns_amd64_emit_u32(c, imm);
}

/* Load a 64-bit constant, preferring the short encodings. */
static void ns_amd64_emit_const_u64(ns_amd64_ctx *c, i32 rd, u64 val) {
    if (val <= 0xFFFFFFFFull) ns_amd64_emit_mov_ri32(c, rd, (u32)val);
    else ns_amd64_emit_mov_ri64(c, rd, val);
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
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, digit, rd)); /* /4=SHL /5=SHR /7=SAR */
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

static void ns_amd64_emit_div_r(ns_amd64_ctx *c, i32 rm) {
    ns_amd64_emit_u8(c, ns_amd64_rex(0, rm));
    ns_amd64_emit_u8(c, 0xF7);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 6, rm)); /* /6 = DIV */
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

/* SETcc r/m8. setcc_opc is the second byte of 0F 9x. */
static void ns_amd64_emit_setcc_byte(ns_amd64_ctx *c, i32 rd, u8 setcc_opc) {
    if (rd >= 8) {
        ns_amd64_emit_u8(c, 0x41);
    } else if (rd >= 4 && rd <= 7) {
        /* SPL/BPL/SIL/DIL need a REX prefix to be addressable as bytes. */
        ns_amd64_emit_u8(c, 0x40);
    }
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, setcc_opc);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, 0, rd));
}

/* MOVZX r64, r8 — widen a SETcc result to the slot width. */
static void ns_amd64_emit_movzx_r8(ns_amd64_ctx *c, i32 rd) {
    ns_amd64_emit_u8(c, ns_amd64_rex(rd, rd));
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, 0xB6);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, rd, rd));
}

static void ns_amd64_emit_setcc_r(ns_amd64_ctx *c, i32 rd, u8 setcc_opc) {
    ns_amd64_emit_setcc_byte(c, rd, setcc_opc);
    ns_amd64_emit_movzx_r8(c, rd);
}

/* AND/OR r/m8, r8 — combine the two flag tests an ordered float compare needs.
 * `opcode` is 0x20 for AND and 0x08 for OR. */
static void ns_amd64_emit_byte_alu_rr(ns_amd64_ctx *c, u8 opcode, i32 rm, i32 reg) {
    u8 rex = 0x40;
    if (reg >= 8) rex |= 0x04;
    if (rm >= 8) rex |= 0x01;
    ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, opcode);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, rm));
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

/* CALL [RBP + disp32] — jump through a slot, used for a call by value. */
static void ns_amd64_emit_call_slot(ns_amd64_ctx *c, i32 disp) {
    ns_amd64_emit_u8(c, 0xFF);
    ns_amd64_emit_u8(c, ns_amd64_modrm(2, 2, NS_AMD64_RBP)); /* /2 = CALL r/m64 */
    ns_amd64_emit_u32(c, (u32)disp);
}

/* LEA reg, [RIP + disp32] — the relocation supplies the displacement. */
static u32 ns_amd64_emit_lea_rip(ns_amd64_ctx *c, i32 reg) {
    ns_amd64_emit_u8(c, ns_amd64_rex(reg, 0));
    ns_amd64_emit_u8(c, 0x8D);
    ns_amd64_emit_u8(c, ns_amd64_modrm(0, reg, 5)); /* mod=00 rm=101 → RIP-relative */
    u32 disp_off = (u32)ns_array_length(c->text);
    ns_amd64_emit_u32(c, 0);
    return disp_off;
}

static void ns_amd64_emit_leave(ns_amd64_ctx *c) { ns_amd64_emit_u8(c, 0xC9); }
static void ns_amd64_emit_ret(ns_amd64_ctx *c) { ns_amd64_emit_u8(c, 0xC3); }
static void ns_amd64_emit_int3(ns_amd64_ctx *c) { ns_amd64_emit_u8(c, 0xCC); }

static void ns_amd64_emit_rt_call(ns_amd64_ctx *c, const char *name) {
    u32 rel_off = ns_amd64_emit_call_rel32(c);
    ns_amd64_call_fixup cf = {.off = rel_off, .callee = ns_str_cstr((i8 *)name), .kind = 0};
    ns_array_push(c->call_fixups, cf);
}

/* ── SSE: floats live in slots as their bit pattern ──────────────────────── */
/* [prefix] [REX] 0F opcode /r, with both operands in registers. */
static void ns_amd64_emit_sse_rr(ns_amd64_ctx *c, u8 prefix, ns_bool wide, u8 opcode,
                                 i32 reg, i32 rm) {
    if (prefix) ns_amd64_emit_u8(c, prefix);
    u8 rex = 0x40;
    if (wide) rex |= 0x08;
    if (reg >= 8) rex |= 0x04;
    if (rm >= 8) rex |= 0x01;
    if (rex != 0x40) ns_amd64_emit_u8(c, rex);
    ns_amd64_emit_u8(c, 0x0F);
    ns_amd64_emit_u8(c, opcode);
    ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, rm));
}

/* MOVQ xmm, r64 */
static void ns_amd64_emit_movq_xr(ns_amd64_ctx *c, i32 xmm, i32 gpr) {
    ns_amd64_emit_sse_rr(c, 0x66, true, 0x6E, xmm, gpr);
}

/* MOVQ r64, xmm */
static void ns_amd64_emit_movq_rx(ns_amd64_ctx *c, i32 gpr, i32 xmm) {
    ns_amd64_emit_sse_rr(c, 0x66, true, 0x7E, xmm, gpr);
}

static ns_bool ns_amd64_is_float(ns_type t) {
    return ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64);
}

static ns_bool ns_amd64_is_string(ns_type t) {
    return ns_type_is(t, NS_TYPE_STRING);
}

/* ADDSD/SUBSD/MULSD/DIVSD, or their single-precision forms. */
static void ns_amd64_emit_farith(ns_amd64_ctx *c, u8 opcode, ns_bool f64, i32 x, i32 y) {
    ns_amd64_emit_sse_rr(c, f64 ? 0xF2 : 0xF3, false, opcode, x, y);
}

/* UCOMISD/UCOMISS xmm_a, xmm_b */
static void ns_amd64_emit_fcmp(ns_amd64_ctx *c, ns_bool f64, i32 a, i32 b) {
    ns_amd64_emit_sse_rr(c, f64 ? 0x66 : 0x00, false, 0x2E, a, b);
}

/* Flip the sign bit of a float held as a bit pattern in a register. */
static void ns_amd64_emit_fneg(ns_amd64_ctx *c, i32 reg, ns_bool f64) {
    ns_amd64_emit_const_u64(c, NS_AMD64_R11, f64 ? 0x8000000000000000ull : 0x80000000ull);
    ns_amd64_emit_xor_rr(c, reg, NS_AMD64_R11);
}

/* Native code sees a str as a char*, a ref as a host pointer, and an array as
 * the bare buffer its elements live in. Each of those is a runtime call away
 * from the ns value; every other argument type crosses the boundary as it is. */
static const char *ns_amd64_ffi_convert(ns_type t) {
    if (ns_type_is_array(t)) return "ns_rt_array_ptr";
    if (ns_amd64_is_string(t)) return "ns_rt_to_cstr";
    if (ns_type_is_ref(t)) return "ns_rt_native_ptr";
    return NULL;
}

static const char *ns_amd64_map_std(ns_str module, ns_str name) {
    if (!ns_str_equals(module, ns_str_cstr("std"))) return NULL;
    if (ns_str_equals(name, ns_str_cstr("print"))) return "ns_rt_print";
    if (ns_str_equals(name, ns_str_cstr("open"))) return "ns_rt_open";
    if (ns_str_equals(name, ns_str_cstr("read"))) return "ns_rt_read";
    if (ns_str_equals(name, ns_str_cstr("write"))) return "ns_rt_write";
    if (ns_str_equals(name, ns_str_cstr("close"))) return "ns_rt_close";
    if (ns_str_equals(name, ns_str_cstr("sqrt"))) return "ns_rt_sqrt";
    if (ns_str_equals(name, ns_str_cstr("sin"))) return "ns_rt_sin";
    if (ns_str_equals(name, ns_str_cstr("cos"))) return "ns_rt_cos";
    if (ns_str_equals(name, ns_str_cstr("tan"))) return "ns_rt_tan";
    if (ns_str_equals(name, ns_str_cstr("atan2"))) return "ns_rt_atan2";
    if (ns_str_equals(name, ns_str_cstr("ftos"))) return "ns_rt_ftos";
    if (ns_str_equals(name, ns_str_cstr("stof"))) return "ns_rt_stof";
    if (ns_str_equals(name, ns_str_cstr("substr"))) return "ns_rt_substr";
    if (ns_str_equals(name, ns_str_cstr("unescape"))) return "ns_rt_unescape";
    if (ns_str_equals(name, ns_str_cstr("utf8_len"))) return "ns_rt_utf8_len";
    return NULL;
}

static const char *ns_amd64_map_task(ns_str module, ns_str name) {
    if (!ns_str_equals(module, ns_str_cstr("task"))) return NULL;
    if (ns_str_equals(name, ns_str_cstr("dispatch"))) return "ns_rt_task_dispatch";
    if (ns_str_equals(name, ns_str_cstr("wait"))) return "ns_rt_task_wait";
    if (ns_str_equals(name, ns_str_cstr("cancel"))) return "ns_rt_task_cancel";
    if (ns_str_equals(name, ns_str_cstr("done"))) return "ns_rt_task_done";
    if (ns_str_equals(name, ns_str_cstr("cancelled"))) return "ns_rt_task_cancelled";
    if (ns_str_equals(name, ns_str_cstr("sleep"))) return "ns_rt_task_sleep";
    if (ns_str_equals(name, ns_str_cstr("queue_main"))) return "ns_rt_queue_main";
    if (ns_str_equals(name, ns_str_cstr("queue_worker"))) return "ns_rt_queue_worker";
    if (ns_str_equals(name, ns_str_cstr("queue_idle"))) return "ns_rt_queue_idle";
    return NULL;
}

static ns_bool ns_amd64_is_ffi_module(ns_str module) {
    if (module.len == 0) return false;
    if (ns_str_equals(module, ns_str_cstr("std"))) return false;
    if (ns_str_equals(module, ns_str_cstr("task"))) return false;
    if (ns_str_equals(module, ns_str_cstr("simd"))) return false;
    if (ns_str_equals(module, ns_str_cstr("shader"))) return false;
    return true;
}

static ns_ssa_import *ns_amd64_find_import(ns_amd64_ctx *c, ns_str module, ns_str name) {
    if (!c->ssa) return NULL;
    for (i32 i = 0, l = (i32)ns_array_length(c->ssa->imports); i < l; ++i) {
        ns_ssa_import *im = &c->ssa->imports[i];
        if (ns_str_equals(im->module, module) && ns_str_equals(im->name, name)) return im;
    }
    return NULL;
}

static i32 ns_amd64_collect_call_args(ns_amd64_ctx *c, ns_ssa_inst *inst, i32 *args, i32 max_args) {
    ns_ssa_block *bb = &c->fn->blocks[c->cur_block];
    i32 nargs = inst->c > 0 ? inst->c : 0;
    if (nargs > max_args) nargs = max_args;
    i32 call_i = -1;
    for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
        if (&c->fn->insts[bb->insts[ii]] == inst) { call_i = ii; break; }
    }
    i32 got = 0;
    for (i32 ii = call_i - 1; ii >= 0 && got < nargs; --ii) {
        ns_ssa_inst *pi = &c->fn->insts[bb->insts[ii]];
        if (pi->op != NS_SSA_OP_ARG) break;
        args[nargs - 1 - got] = pi->a;
        got++;
    }
    return got < nargs ? got : nargs;
}

static ns_type ns_amd64_value_type(ns_ssa_fn *fn, i32 value) {
    if (value < 0 || !fn) return ns_type_unknown;
    for (i32 i = (i32)ns_array_length(fn->insts) - 1; i >= 0; --i) {
        if (fn->insts[i].dst == value) return fn->insts[i].type;
    }
    return ns_type_unknown;
}

static i32 ns_amd64_load_size(ns_type t) {
    if (ns_type_is(t, NS_TYPE_I8)) return 1;
    if (ns_type_is(t, NS_TYPE_U8)) return -1;
    if (ns_type_is(t, NS_TYPE_I16)) return 2;
    if (ns_type_is(t, NS_TYPE_U16)) return -2;
    if (ns_type_is_ref(t) || ns_type_is(t, NS_TYPE_ANY) ||
        ns_type_is(t, NS_TYPE_I64) || ns_type_is(t, NS_TYPE_U64) ||
        ns_type_is(t, NS_TYPE_F64)) return 8;
    if (ns_type_is(t, NS_TYPE_F32)) return 4;
    return 4;
}

/* Reserve `bytes` of outgoing argument space, keeping RSP 16-byte aligned. */
static i32 ns_amd64_open_arg_space(ns_amd64_ctx *c, i32 nstack) {
    i32 base = c->win_abi ? NS_AMD64_SHADOW : 0;
    if (nstack <= 0) return 0;
    i32 space = (base + nstack * 8 + 15) & ~15;
    ns_amd64_emit_alu_ri32(c, 5, NS_AMD64_RSP, space); /* SUB RSP, space */
    return space;
}

static void ns_amd64_close_arg_space(ns_amd64_ctx *c, i32 space) {
    if (space > 0) ns_amd64_emit_alu_ri32(c, 0, NS_AMD64_RSP, space); /* ADD RSP, space */
}

/* ── constant parsing ─────────────────────────────────────────────────────── */
static ns_bool ns_amd64_parse_u64(ns_str s, u64 *out) {
    if (s.len <= 0 || !s.data) return false;
    if (ns_str_equals(s, ns_str_cstr("true"))) { *out = 1; return true; }
    if (ns_str_equals(s, ns_str_cstr("false"))) { *out = 0; return true; }
    if (ns_str_equals(s, ns_str_cstr("nil"))) { *out = 0; return true; }

    /* A leading sign appears in signed enum members lowered to constants. */
    i32 start = 0;
    ns_bool neg = false;
    if (s.data[0] == '-' || s.data[0] == '+') {
        neg = s.data[0] == '-';
        start = 1;
        if (start >= s.len) return false;
    }

    i32 end = s.len;
    while (end > start) {
        i8 suf = s.data[end - 1];
        if (suf == 'u' || suf == 'U' || suf == 'i' || suf == 'I' ||
            suf == 'l' || suf == 'L') {
            end--;
            continue;
        }
        break;
    }
    if (end <= start) return false;

    if (start + 1 < end && s.data[start] == '0' && (s.data[start + 1] == 'x' || s.data[start + 1] == 'X')) {
        u64 v = 0;
        for (i32 i = start + 2; i < end; ++i) {
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
    for (i32 i = start; i < end; ++i) {
        if (s.data[i] == '.') { has_dot = true; break; }
        if (s.data[i] < '0' || s.data[i] > '9') return false;
    }
    if (!has_dot) {
        u64 v = 0;
        for (i32 i = start; i < end; ++i) {
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

/* Narrow a register to the width/signedness of an integer value so that e.g.
 * `300 as u8` yields 44. Wider or non-integer targets are no-ops. */
static void ns_amd64_narrow_reg(ns_amd64_ctx *c, i32 reg, ns_type t) {
    if (ns_type_is(t, NS_TYPE_I8)) {        /* MOVSX r64, r8  */
        ns_amd64_emit_u8(c, ns_amd64_rex(reg, reg)); ns_amd64_emit_u8(c, 0x0F);
        ns_amd64_emit_u8(c, 0xBE); ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, reg));
    } else if (ns_type_is(t, NS_TYPE_U8)) { /* MOVZX r64, r8  */
        ns_amd64_emit_u8(c, ns_amd64_rex(reg, reg)); ns_amd64_emit_u8(c, 0x0F);
        ns_amd64_emit_u8(c, 0xB6); ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, reg));
    } else if (ns_type_is(t, NS_TYPE_I16)) { /* MOVSX r64, r16 */
        ns_amd64_emit_u8(c, ns_amd64_rex(reg, reg)); ns_amd64_emit_u8(c, 0x0F);
        ns_amd64_emit_u8(c, 0xBF); ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, reg));
    } else if (ns_type_is(t, NS_TYPE_U16)) { /* MOVZX r64, r16 */
        ns_amd64_emit_u8(c, ns_amd64_rex(reg, reg)); ns_amd64_emit_u8(c, 0x0F);
        ns_amd64_emit_u8(c, 0xB7); ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, reg));
    } else if (ns_type_is(t, NS_TYPE_I32)) { /* MOVSXD r64, r32 */
        ns_amd64_emit_u8(c, ns_amd64_rex(reg, reg)); ns_amd64_emit_u8(c, 0x63);
        ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, reg));
    } else if (ns_type_is(t, NS_TYPE_U32)) { /* MOV r32, r32 zero-extends */
        u8 rex = 0x40;
        if (reg >= 8) rex |= 0x05;
        if (rex != 0x40) ns_amd64_emit_u8(c, rex);
        ns_amd64_emit_u8(c, 0x8B);
        ns_amd64_emit_u8(c, ns_amd64_modrm(3, reg, reg));
    }
}

/* ── phi edge copies ──────────────────────────────────────────────────────── */
/* Before a branch from `from` to `to`, materialize `to`'s phis by copying the
 * input that flows in along this edge into each phi's slot. */
static void ns_amd64_emit_edge_copies(ns_amd64_ctx *c, i32 from, i32 to) {
    ns_ssa_block *tb = &c->fn->blocks[to];
    for (i32 ii = 0, il = (i32)ns_array_length(tb->insts); ii < il; ++ii) {
        ns_ssa_inst *inst = &c->fn->insts[tb->insts[ii]];
        if (inst->op != NS_SSA_OP_PHI) break; /* phis lead the block */
        i32 src = ns_ssa_phi_incoming(inst, from);
        if (src < 0 || inst->dst < 0 || src == inst->dst) continue;
        ns_amd64_load_value(c, NS_AMD64_RAX, src);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    }
}

/* ── float helpers ────────────────────────────────────────────────────────── */
static void ns_amd64_emit_float_binop(ns_amd64_ctx *c, ns_ssa_inst *inst) {
    ns_bool f64 = ns_type_is(inst->type, NS_TYPE_F64);
    ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
    ns_amd64_load_value(c, NS_AMD64_R11, inst->b);
    ns_amd64_emit_movq_xr(c, NS_AMD64_XMM0, NS_AMD64_RAX);
    ns_amd64_emit_movq_xr(c, NS_AMD64_XMM1, NS_AMD64_R11);
    switch (inst->op) {
    case NS_SSA_OP_ADD: ns_amd64_emit_farith(c, 0x58, f64, NS_AMD64_XMM0, NS_AMD64_XMM1); break;
    case NS_SSA_OP_SUB: ns_amd64_emit_farith(c, 0x5C, f64, NS_AMD64_XMM0, NS_AMD64_XMM1); break;
    case NS_SSA_OP_MUL: ns_amd64_emit_farith(c, 0x59, f64, NS_AMD64_XMM0, NS_AMD64_XMM1); break;
    default:            ns_amd64_emit_farith(c, 0x5E, f64, NS_AMD64_XMM0, NS_AMD64_XMM1); break;
    }
    ns_amd64_emit_movq_rx(c, NS_AMD64_RAX, NS_AMD64_XMM0);
    ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
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
        if (inst->dst < 0 || inst->c < 0) break;
        if (inst->c < c->nint_regs) {
            ns_amd64_store_value(c, inst->dst, c->int_regs[inst->c]);
            break;
        }
        /* Stack arguments start above the saved RBP and the return address,
         * and above the caller's shadow space where the ABI reserves one. */
        i32 disp = 16 + (c->win_abi ? NS_AMD64_SHADOW : 0) + 8 * (inst->c - c->nint_regs);
        ns_amd64_emit_load(c, NS_AMD64_RAX, disp);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        if (ns_type_is(inst->type, NS_TYPE_STRING)) {
            ns_amd64_emit_const_u64(c, c->int_regs[0], (u64)(u32)inst->c);
            ns_amd64_emit_rt_call(c, "ns_rt_intern");
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        if (ns_type_is(inst->type, NS_TYPE_F64)) {
            f64 fv = ns_str_to_f64(inst->name);
            u64 bits = 0;
            memcpy(&bits, &fv, 8);
            ns_amd64_emit_const_u64(c, NS_AMD64_RAX, bits);
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        if (ns_type_is(inst->type, NS_TYPE_F32)) {
            f32 fv = (f32)ns_str_to_f64(inst->name);
            u32 bits = 0;
            memcpy(&bits, &fv, 4);
            ns_amd64_emit_const_u64(c, NS_AMD64_RAX, bits);
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        u64 val = 0;
        if (!ns_amd64_parse_u64(inst->name, &val)) {
            ns_warn("amd64", "unsupported const '%.*s' in fn %.*s, using 0\n",
                inst->name.len, inst->name.data, c->fn->name.len, c->fn->name.data);
            val = 0;
        }
        ns_amd64_emit_const_u64(c, NS_AMD64_RAX, val);
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
        ns_type src_t = ns_amd64_value_type(c->fn, inst->a);
        ns_bool src_f = ns_amd64_is_float(src_t);
        ns_bool dst_f = ns_amd64_is_float(inst->type);
        if (dst_f && src_f) {
            ns_bool src64 = ns_type_is(src_t, NS_TYPE_F64);
            ns_bool dst64 = ns_type_is(inst->type, NS_TYPE_F64);
            if (src64 != dst64) {
                ns_amd64_emit_movq_xr(c, NS_AMD64_XMM0, NS_AMD64_RAX);
                /* CVTSD2SS / CVTSS2SD */
                ns_amd64_emit_sse_rr(c, src64 ? 0xF2 : 0xF3, false, 0x5A,
                                     NS_AMD64_XMM0, NS_AMD64_XMM0);
                ns_amd64_emit_movq_rx(c, NS_AMD64_RAX, NS_AMD64_XMM0);
            }
        } else if (dst_f && !src_f) {
            /* CVTSI2SD/SS from the 64-bit value. An unsigned source wider than
             * 63 bits would need the two-step trick; every narrower one is
             * already zero-extended and converts exactly. */
            ns_bool dst64 = ns_type_is(inst->type, NS_TYPE_F64);
            ns_amd64_emit_sse_rr(c, dst64 ? 0xF2 : 0xF3, true, 0x2A,
                                 NS_AMD64_XMM0, NS_AMD64_RAX);
            ns_amd64_emit_movq_rx(c, NS_AMD64_RAX, NS_AMD64_XMM0);
        } else if (!dst_f && src_f) {
            ns_bool src64 = ns_type_is(src_t, NS_TYPE_F64);
            ns_amd64_emit_movq_xr(c, NS_AMD64_XMM0, NS_AMD64_RAX);
            /* CVTTSD2SI / CVTTSS2SI, truncating toward zero into a 64-bit GPR */
            ns_amd64_emit_sse_rr(c, src64 ? 0xF2 : 0xF3, true, 0x2C,
                                 NS_AMD64_RAX, NS_AMD64_XMM0);
            ns_amd64_narrow_reg(c, NS_AMD64_RAX, inst->type);
        } else {
            ns_amd64_narrow_reg(c, NS_AMD64_RAX, inst->type);
        }
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB:
    case NS_SSA_OP_MUL:
    case NS_SSA_OP_BAND: case NS_SSA_OP_AND:
    case NS_SSA_OP_BOR:  case NS_SSA_OP_OR:
    case NS_SSA_OP_BXOR:
    case NS_SSA_OP_SHL:  case NS_SSA_OP_SHR: {
        if (inst->dst < 0) break;
        ns_type at = ns_amd64_value_type(c->fn, inst->a);
        if (inst->op == NS_SSA_OP_ADD &&
            (ns_amd64_is_string(inst->type) || ns_amd64_is_string(at))) {
            ns_amd64_load_value(c, c->int_regs[0], inst->a);
            ns_amd64_load_value(c, c->int_regs[1], inst->b);
            ns_amd64_emit_rt_call(c, "ns_rt_strcat");
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        if ((inst->op == NS_SSA_OP_ADD || inst->op == NS_SSA_OP_SUB || inst->op == NS_SSA_OP_MUL) &&
            ns_amd64_is_float(inst->type)) {
            ns_amd64_emit_float_binop(c, inst);
            break;
        }
        if (inst->op == NS_SSA_OP_SHL || inst->op == NS_SSA_OP_SHR) {
            /* The shift count must live in CL, which is also an argument
             * register, so load it after the value being shifted. */
            ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
            ns_amd64_load_value(c, NS_AMD64_RCX, inst->b);
            i32 digit = inst->op == NS_SSA_OP_SHL ? 4 : (ns_type_unsigned(at) ? 5 : 7);
            ns_amd64_emit_shift_rcl(c, NS_AMD64_RAX, digit);
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
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
        if (ns_amd64_is_float(inst->type)) {
            if (inst->op == NS_SSA_OP_DIV) {
                ns_amd64_emit_float_binop(c, inst);
            } else {
                ns_amd64_load_value(c, c->int_regs[0], inst->a);
                ns_amd64_load_value(c, c->int_regs[1], inst->b);
                ns_amd64_emit_rt_call(c, ns_type_is(inst->type, NS_TYPE_F32) ? "ns_rt_fmodf" : "ns_rt_fmod");
                ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            }
            break;
        }
        ns_type at = ns_amd64_value_type(c->fn, inst->a);
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a); /* dividend */
        ns_amd64_load_value(c, NS_AMD64_R11, inst->b); /* divisor */
        if (ns_type_unsigned(at)) {
            ns_amd64_emit_xor32_rr(c, NS_AMD64_RDX);   /* zero-extend into RDX:RAX */
            ns_amd64_emit_div_r(c, NS_AMD64_R11);
        } else {
            ns_amd64_emit_cqo(c);                      /* sign-extend into RDX:RAX */
            ns_amd64_emit_idiv_r(c, NS_AMD64_R11);
        }
        ns_amd64_store_value(c, inst->dst, inst->op == NS_SSA_OP_DIV ? NS_AMD64_RAX : NS_AMD64_RDX);
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
        ns_type nt = ns_amd64_is_float(inst->type) ? inst->type : ns_amd64_value_type(c->fn, inst->a);
        if (ns_amd64_is_float(nt)) {
            ns_amd64_emit_fneg(c, NS_AMD64_RAX, ns_type_is(nt, NS_TYPE_F64));
        } else {
            ns_amd64_emit_neg_r(c, NS_AMD64_RAX);
        }
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
        ns_type at = ns_amd64_value_type(c->fn, inst->a);
        ns_type bt = ns_amd64_value_type(c->fn, inst->b);
        ns_bool use_float = (ns_amd64_is_float(at) || ns_amd64_is_float(bt)) &&
                            !ns_amd64_is_string(at) && !ns_amd64_is_string(bt);
        if (use_float) {
            /* An unordered compare must answer false, so the two "less" forms
             * test the reversed operands rather than CF, which NaN also sets. */
            ns_bool f64 = ns_type_is(at, NS_TYPE_F64) || ns_type_is(bt, NS_TYPE_F64);
            ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
            ns_amd64_load_value(c, NS_AMD64_R11, inst->b);
            ns_amd64_emit_movq_xr(c, NS_AMD64_XMM0, NS_AMD64_RAX);
            ns_amd64_emit_movq_xr(c, NS_AMD64_XMM1, NS_AMD64_R11);
            switch (inst->op) {
            case NS_SSA_OP_LT: case NS_SSA_OP_LE:
                ns_amd64_emit_fcmp(c, f64, NS_AMD64_XMM1, NS_AMD64_XMM0);
                break;
            default:
                ns_amd64_emit_fcmp(c, f64, NS_AMD64_XMM0, NS_AMD64_XMM1);
                break;
            }
            switch (inst->op) {
            case NS_SSA_OP_EQ:
                /* ordered and equal: ZF=1 and PF=0 */
                ns_amd64_emit_setcc_byte(c, NS_AMD64_RAX, 0x94); /* SETE  */
                ns_amd64_emit_setcc_byte(c, NS_AMD64_R11, 0x9B); /* SETNP */
                ns_amd64_emit_byte_alu_rr(c, 0x20, NS_AMD64_RAX, NS_AMD64_R11);
                break;
            case NS_SSA_OP_NE:
                /* unordered, or ordered and different */
                ns_amd64_emit_setcc_byte(c, NS_AMD64_RAX, 0x95); /* SETNE */
                ns_amd64_emit_setcc_byte(c, NS_AMD64_R11, 0x9A); /* SETP  */
                ns_amd64_emit_byte_alu_rr(c, 0x08, NS_AMD64_RAX, NS_AMD64_R11);
                break;
            case NS_SSA_OP_LT: case NS_SSA_OP_GT:
                ns_amd64_emit_setcc_byte(c, NS_AMD64_RAX, 0x97); /* SETA  */
                break;
            default:
                ns_amd64_emit_setcc_byte(c, NS_AMD64_RAX, 0x93); /* SETAE */
                break;
            }
            ns_amd64_emit_movzx_r8(c, NS_AMD64_RAX);
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        if (ns_amd64_is_string(at) || ns_amd64_is_string(bt)) {
            ns_amd64_load_value(c, c->int_regs[0], inst->a);
            ns_amd64_load_value(c, c->int_regs[1], inst->b);
            ns_amd64_emit_rt_call(c, "ns_rt_strcmp");
            ns_amd64_emit_test_rr(c, NS_AMD64_RAX); /* flags from strcmp - 0 */
        } else {
            ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
            ns_amd64_load_value(c, NS_AMD64_R11, inst->b);
            ns_amd64_emit_cmp_rr(c, NS_AMD64_RAX, NS_AMD64_R11); /* flags from a - b */
        }
        ns_bool uns = ns_type_unsigned(at) && !ns_amd64_is_string(at);
        u8 setcc_opc;
        switch (inst->op) {
        case NS_SSA_OP_EQ: setcc_opc = 0x94; break;              /* SETE  */
        case NS_SSA_OP_NE: setcc_opc = 0x95; break;              /* SETNE */
        case NS_SSA_OP_LT: setcc_opc = uns ? 0x92 : 0x9C; break; /* SETB  / SETL  */
        case NS_SSA_OP_LE: setcc_opc = uns ? 0x96 : 0x9E; break; /* SETBE / SETLE */
        case NS_SSA_OP_GT: setcc_opc = uns ? 0x97 : 0x9F; break; /* SETA  / SETG  */
        default:           setcc_opc = uns ? 0x93 : 0x9D; break; /* SETAE / SETGE */
        }
        ns_amd64_emit_setcc_r(c, NS_AMD64_RAX, setcc_opc);
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_ARG: {
        if (c->arg_seq < c->nint_regs) {
            ns_amd64_load_value(c, c->int_regs[c->arg_seq++], inst->a);
            break;
        }
        if (c->nextra < NS_AMD64_EXTRA_MAX) c->extra_args[c->nextra++] = inst->a;
        c->arg_seq++;
    } break;
    case NS_SSA_OP_CALL: {
        ns_str peek_name = inst->name;
        ns_bool peek_ffi = ns_amd64_is_ffi_module(inst->module);
        ns_bool indirect = peek_name.len == 0 && inst->a >= 0 &&
                           ns_amd64_map_std(inst->module, inst->name) == NULL &&
                           ns_amd64_map_task(inst->module, inst->name) == NULL;
        i32 nextra = c->nextra;
        i32 space = 0;
        if (nextra > 0 && !indirect && !peek_ffi) {
            i32 base = c->win_abi ? NS_AMD64_SHADOW : 0;
            space = ns_amd64_open_arg_space(c, nextra);
            for (i32 ei = 0; ei < nextra; ++ei) {
                ns_amd64_load_value(c, NS_AMD64_RAX, c->extra_args[ei]);
                ns_amd64_emit_store_rsp(c, base + ei * 8, NS_AMD64_RAX);
            }
        }
        c->arg_seq = 0;
        c->nextra = 0;
        ns_str callee_name = inst->name;
        const char *std_rt = ns_amd64_map_std(inst->module, callee_name);
        if (std_rt) callee_name = ns_str_cstr((i8 *)std_rt);
        const char *task_rt = ns_amd64_map_task(inst->module, callee_name);
        if (task_rt) callee_name = ns_str_cstr((i8 *)task_rt);
        ns_bool ffi = ns_amd64_is_ffi_module(inst->module) && callee_name.len > 0;
        if (ffi) {
            ns_ssa_import *im = ns_amd64_find_import(c, inst->module, inst->name);
            i32 args[NS_AMD64_EXTRA_MAX + 8];
            i32 nargs = ns_amd64_collect_call_args(c, inst, args, NS_AMD64_EXTRA_MAX + 8);
            /* dest_reg holds an integer register index, or 16 + n for XMMn. */
            i32 dest_reg[NS_AMD64_EXTRA_MAX + 8];
            i32 stack_n = 0;
            i32 stack_args[NS_AMD64_EXTRA_MAX];
            i32 igpr = 0;
            i32 fpr = 0;
            for (i32 ai = 0; ai < nargs; ++ai) dest_reg[ai] = -1;
            for (i32 ai = 0; ai < nargs; ++ai) {
                ns_type at = ns_type_unknown;
                if (im && ai < (i32)ns_array_length(im->params)) at = im->params[ai];
                else at = ns_amd64_value_type(c->fn, args[ai]);
                if (c->win_abi) {
                    /* The Microsoft ABI aliases the integer and vector
                     * registers by position, so one counter covers both. */
                    if (igpr < c->nint_regs) {
                        dest_reg[ai] = ns_amd64_is_float(at) ? 16 + igpr : c->int_regs[igpr];
                        igpr++;
                    } else {
                        stack_args[stack_n++] = args[ai];
                    }
                    continue;
                }
                if (ns_amd64_is_float(at) && fpr < 8) {
                    dest_reg[ai] = 16 + fpr;
                    fpr++;
                    continue;
                }
                if (igpr < c->nint_regs) dest_reg[ai] = c->int_regs[igpr++];
                else stack_args[stack_n++] = args[ai];
            }
            if (stack_n > 0) {
                i32 base = c->win_abi ? NS_AMD64_SHADOW : 0;
                space = ns_amd64_open_arg_space(c, stack_n);
                for (i32 ei = 0; ei < stack_n; ++ei) {
                    ns_type st = ns_amd64_value_type(c->fn, stack_args[ei]);
                    const char *conv = ns_amd64_ffi_convert(st);
                    if (conv) {
                        ns_amd64_load_value(c, c->int_regs[0], stack_args[ei]);
                        ns_amd64_emit_rt_call(c, conv);
                    } else {
                        ns_amd64_load_value(c, NS_AMD64_RAX, stack_args[ei]);
                    }
                    ns_amd64_emit_store_rsp(c, base + ei * 8, NS_AMD64_RAX);
                }
            }
            // The conversion helpers return in RAX, and reaching one costs a
            // call that clobbers every argument register. A pointer left in one
            // would not survive the conversion of the next argument, so every
            // conversion parks its result in a scratch slot and the registers
            // are filled once the last call has been made.
            i32 spill_slot[NS_AMD64_EXTRA_MAX + 8];
            for (i32 ai = 0; ai < nargs; ++ai) spill_slot[ai] = -1;
            i32 nspill = 0;
            for (i32 ai = 0; ai < nargs; ++ai) {
                ns_type at = ns_type_unknown;
                if (im && ai < (i32)ns_array_length(im->params)) at = im->params[ai];
                else at = ns_amd64_value_type(c->fn, args[ai]);
                const char *conv = ns_amd64_ffi_convert(at);
                if (!conv) continue;
                if (dest_reg[ai] < 0 || dest_reg[ai] >= 16) continue;
                ns_amd64_load_value(c, c->int_regs[0], args[ai]);
                ns_amd64_emit_rt_call(c, conv);
                spill_slot[ai] = c->scratch_base + nspill++;
                ns_amd64_store_value(c, spill_slot[ai], NS_AMD64_RAX);
            }
            for (i32 ai = 0; ai < nargs; ++ai) {
                if (spill_slot[ai] < 0) continue;
                ns_amd64_load_value(c, dest_reg[ai], spill_slot[ai]);
            }
            for (i32 ai = 0; ai < nargs; ++ai) {
                ns_type at = ns_type_unknown;
                if (im && ai < (i32)ns_array_length(im->params)) at = im->params[ai];
                else at = ns_amd64_value_type(c->fn, args[ai]);
                if (ns_amd64_ffi_convert(at)) continue;
                i32 dest = dest_reg[ai];
                if (dest >= 16) {
                    ns_amd64_load_value(c, NS_AMD64_R11, args[ai]);
                    ns_amd64_emit_movq_xr(c, dest - 16, NS_AMD64_R11);
                } else if (dest >= 0) {
                    ns_amd64_load_value(c, dest, args[ai]);
                }
            }
            if (!c->win_abi) {
                /* A variadic callee reads AL for the number of vector
                 * registers used; a fixed one ignores it. */
                ns_amd64_emit_mov_ri32(c, NS_AMD64_RAX, (u32)fpr);
            }
            u32 rel_off = ns_amd64_emit_call_rel32(c);
            ns_amd64_call_fixup cf = {.off = rel_off, .callee = callee_name, .kind = 0};
            ns_array_push(c->call_fixups, cf);
            ns_type ret = im ? im->ret : inst->type;
            if (ns_amd64_is_float(ret)) {
                ns_amd64_emit_movq_rx(c, NS_AMD64_RAX, NS_AMD64_XMM0);
                if (ns_type_is(ret, NS_TYPE_F32)) {
                    /* Only the low half carries an f32; clear what follows it. */
                    ns_amd64_narrow_reg(c, NS_AMD64_RAX, ns_type_u32);
                }
            } else if (ns_amd64_is_string(ret) && !ns_type_is_array(ret)) {
                ns_amd64_emit_mov_rr(c, c->int_regs[0], NS_AMD64_RAX);
                ns_amd64_emit_rt_call(c, "ns_rt_from_cstr");
            } else {
                // C callees may leave the upper half of RAX unspecified for a
                // result narrower than 64 bits. Normalize it before Nano
                // Script performs signed comparisons or stores the value.
                ns_amd64_narrow_reg(c, NS_AMD64_RAX, ret);
            }
            if (inst->dst >= 0) ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            ns_amd64_close_arg_space(c, space);
            break;
        }
        if (callee_name.len == 0 && inst->a >= 0) {
            /* Call by value: the callee address is the first field of the
             * function value, and it has to survive the argument setup. */
            ns_amd64_load_value(c, c->int_regs[0], inst->a);
            ns_amd64_emit_const_u64(c, c->int_regs[1], 0);
            ns_amd64_emit_const_u64(c, c->int_regs[2], 8);
            ns_amd64_emit_rt_call(c, "ns_rt_load");
            ns_amd64_store_value(c, c->indirect_slot, NS_AMD64_RAX);
            i32 args[NS_AMD64_EXTRA_MAX + 8];
            i32 nargs = ns_amd64_collect_call_args(c, inst, args, NS_AMD64_EXTRA_MAX + 8);
            if (nargs > c->nint_regs) {
                i32 base = c->win_abi ? NS_AMD64_SHADOW : 0;
                i32 nstack = nargs - c->nint_regs;
                space = ns_amd64_open_arg_space(c, nstack);
                for (i32 ei = 0; ei < nstack; ++ei) {
                    ns_amd64_load_value(c, NS_AMD64_RAX, args[c->nint_regs + ei]);
                    ns_amd64_emit_store_rsp(c, base + ei * 8, NS_AMD64_RAX);
                }
            }
            i32 nreg = nargs < c->nint_regs ? nargs : c->nint_regs;
            for (i32 ri = 0; ri < nreg; ++ri) ns_amd64_load_value(c, c->int_regs[ri], args[ri]);
            ns_amd64_emit_call_slot(c, ns_amd64_slot_disp(c->indirect_slot));
        } else {
            u32 rel_off = ns_amd64_emit_call_rel32(c);
            if (callee_name.len > 0) {
                ns_amd64_call_fixup cf = {.off = rel_off, .callee = callee_name, .kind = 0};
                ns_array_push(c->call_fixups, cf);
            }
        }
        if (inst->dst >= 0) ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
        ns_amd64_close_arg_space(c, space);
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
        else ns_amd64_emit_xor32_rr(c, NS_AMD64_RAX);
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
    case NS_SSA_OP_GLOBAL_GET: {
        if (inst->dst < 0) break;
        ns_amd64_emit_const_u64(c, c->int_regs[0], (u64)(u32)inst->c);
        ns_amd64_emit_rt_call(c, "ns_rt_gget");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_GLOBAL_SET: {
        ns_amd64_load_value(c, c->int_regs[1], inst->a);
        ns_amd64_emit_const_u64(c, c->int_regs[0], (u64)(u32)inst->c);
        ns_amd64_emit_rt_call(c, "ns_rt_gset");
    } break;
    case NS_SSA_OP_ALLOC: {
        if (inst->dst < 0) break;
        ns_amd64_emit_const_u64(c, c->int_regs[0], (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_amd64_emit_rt_call(c, "ns_rt_alloc");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_CLONE: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_emit_const_u64(c, c->int_regs[1], (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_amd64_emit_rt_call(c, "ns_rt_clone");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_SCOPE_ENTER: {
        if (inst->dst < 0) break;
        ns_amd64_emit_rt_call(c, "ns_rt_scope_enter");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_SCOPE_LEAVE: {
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        if (inst->b >= 0) ns_amd64_load_value(c, c->int_regs[1], inst->b);
        else ns_amd64_emit_xor32_rr(c, c->int_regs[1]);
        ns_amd64_emit_const_u64(c, c->int_regs[2], (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_amd64_emit_rt_call(c, "ns_rt_scope_leave");
        if (inst->dst >= 0) ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_PIN: {
        /* target0 selects how much of the value the runtime has to keep: a flat
         * block of inst->c bytes, a string and its bytes, an array and its
         * payload of inst->c-byte elements, or everything allocated so far. */
        if (inst->target0 == 3 || inst->a < 0) {
            ns_amd64_emit_rt_call(c, "ns_rt_pin_all");
            break;
        }
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        if (inst->target0 == 1) {
            ns_amd64_emit_rt_call(c, "ns_rt_pin_str");
            break;
        }
        ns_amd64_emit_const_u64(c, c->int_regs[1], (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_amd64_emit_rt_call(c, inst->target0 == 2 ? "ns_rt_pin_array" : "ns_rt_pin");
    } break;
    case NS_SSA_OP_LOAD: {
        if (inst->dst < 0) break;
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_array(inst->type) &&
            !ns_type_is_ref(inst->type)) {
            /* A struct field is addressed, not loaded: the value is the
             * address of the field inside its owner. An array of structs is
             * not an inline struct but a handle, so reading such a field goes
             * through the runtime like any other scalar. */
            ns_amd64_load_value(c, NS_AMD64_RAX, inst->a);
            ns_amd64_emit_const_u64(c, NS_AMD64_R11, (u64)(u32)inst->c);
            ns_amd64_emit_add_rr(c, NS_AMD64_RAX, NS_AMD64_R11);
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_emit_const_u64(c, c->int_regs[1], (u64)(u32)inst->c);
        {
            i32 size = inst->target0 > 0 ? inst->target0 : ns_amd64_load_size(inst->type);
            ns_amd64_emit_const_u64(c, c->int_regs[2], (u64)(i64)size);
        }
        ns_amd64_emit_rt_call(c, "ns_rt_load");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_STORE: {
        i32 size = inst->target0 > 0 ? inst->target0 : ns_amd64_load_size(inst->type);
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_load_value(c, c->int_regs[2], inst->b);
        ns_amd64_emit_const_u64(c, c->int_regs[1], (u64)(u32)inst->c);
        ns_amd64_emit_const_u64(c, c->int_regs[3], (u64)(i64)size);
        /* Struct bytes are copied; an array field holds a handle and is
         * stored like any other scalar. */
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_array(inst->type) &&
            !ns_type_is_ref(inst->type) && size > 0) {
            ns_amd64_emit_rt_call(c, "ns_rt_copy");
            break;
        }
        ns_amd64_emit_rt_call(c, "ns_rt_store");
    } break;
    case NS_SSA_OP_ARRAY_NEW: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_emit_const_u64(c, c->int_regs[1], (u64)(u32)(inst->c > 0 ? inst->c : 1));
        ns_amd64_emit_rt_call(c, "ns_rt_array_new");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_ARRAY_STORE: {
        i32 stride = inst->c > 0 ? inst->c : 4;
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_array(inst->type) &&
            !ns_type_is_ref(inst->type)) {
            ns_amd64_load_value(c, c->int_regs[0], inst->a);
            ns_amd64_load_value(c, c->int_regs[1], inst->target0);
            ns_amd64_emit_const_u64(c, c->int_regs[2], (u64)(u32)stride);
            ns_amd64_emit_rt_call(c, "ns_rt_array_slot");
            ns_amd64_emit_mov_rr(c, c->int_regs[0], NS_AMD64_RAX);
            ns_amd64_load_value(c, c->int_regs[2], inst->b);
            ns_amd64_emit_const_u64(c, c->int_regs[1], 0);
            ns_amd64_emit_const_u64(c, c->int_regs[3], (u64)(u32)stride);
            ns_amd64_emit_rt_call(c, "ns_rt_copy");
            break;
        }
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_load_value(c, c->int_regs[1], inst->target0);
        ns_amd64_load_value(c, c->int_regs[2], inst->b);
        ns_amd64_emit_const_u64(c, c->int_regs[3], (u64)(u32)stride);
        ns_amd64_emit_rt_call(c, "ns_rt_array_store");
    } break;
    case NS_SSA_OP_INDEX: {
        if (inst->dst < 0) break;
        i64 stride = inst->c > 0 ? inst->c : 1;
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_array(inst->type)) {
            ns_amd64_load_value(c, c->int_regs[0], inst->a);
            ns_amd64_load_value(c, c->int_regs[1], inst->b);
            ns_amd64_emit_const_u64(c, c->int_regs[2], (u64)stride);
            ns_amd64_emit_rt_call(c, "ns_rt_array_slot");
            ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
            break;
        }
        /* String byte index is emitted as i32 with stride 1; load it unsigned. */
        if (stride == 1 && ns_type_is(inst->type, NS_TYPE_I32)) stride = -1;
        else if (ns_type_is(inst->type, NS_TYPE_U8)) stride = -1;
        else if (ns_type_is(inst->type, NS_TYPE_U16)) stride = -2;
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_load_value(c, c->int_regs[1], inst->b);
        ns_amd64_emit_const_u64(c, c->int_regs[2], (u64)stride);
        ns_amd64_emit_rt_call(c, "ns_rt_array_index");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_FNADDR: {
        if (inst->dst < 0) break;
        /* LEA RAX, [RIP + 0] — the relocation fills in the displacement. */
        u32 disp_off = ns_amd64_emit_lea_rip(c, NS_AMD64_RAX);
        if (inst->name.len > 0) {
            ns_amd64_call_fixup cf = {.off = disp_off, .callee = inst->name, .kind = 1};
            ns_array_push(c->call_fixups, cf);
        }
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
    } break;
    case NS_SSA_OP_MEMBER: {
        if (inst->dst < 0) break;
        ns_amd64_load_value(c, c->int_regs[0], inst->a);
        ns_amd64_load_value(c, c->int_regs[1], inst->b);
        ns_amd64_emit_const_u64(c, c->int_regs[2], 8);
        ns_amd64_emit_rt_call(c, "ns_rt_load");
        ns_amd64_store_value(c, inst->dst, NS_AMD64_RAX);
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

static void ns_amd64_ctx_set_abi(ns_amd64_ctx *c) {
    ns_asm_target target = {0};
    ns_asm_get_current_target(&target);
    c->win_abi = target.os == NS_OS_WINDOWS;
    c->int_regs = c->win_abi ? NS_AMD64_WIN_INT_REGS : NS_AMD64_SYSV_INT_REGS;
    c->nint_regs = c->win_abi ? 4 : 6;
}

/* ── lower a single SSA function to AMD64 machine code ───────────────────── */
static ns_amd64_fn_bin ns_amd64_lower_fn(ns_ssa_module *ssa, ns_ssa_fn *fn,
                                         ns_bool call_rt_init, ns_bool call_mod_init) {
    ns_amd64_ctx c = {0};
    c.ssa = ssa;
    c.fn = fn;
    ns_amd64_ctx_set_abi(&c);

    i32 nslots = ns_amd64_slot_count(fn);
    c.scratch_base = nslots;
    nslots += NS_AMD64_FFI_SCRATCH;
    c.indirect_slot = nslots++;
    /* One 8-byte slot per value, plus the Microsoft ABI's shadow space below
     * them, rounded so RSP stays 16-byte aligned at every call site. */
    i32 frame = (nslots * 8) + (c.win_abi ? NS_AMD64_SHADOW : 0);
    frame = (frame + 15) & ~15;

    /* prologue: establish a frame pointer and reserve the frame */
    ns_amd64_emit_push_r(&c, NS_AMD64_RBP);
    ns_amd64_emit_mov_rr(&c, NS_AMD64_RBP, NS_AMD64_RSP);
    ns_amd64_emit_alu_ri32(&c, 5, NS_AMD64_RSP, frame); /* SUB RSP, frame */
    if (call_rt_init) ns_amd64_emit_rt_call(&c, "ns_rt_init");
    if (call_mod_init) ns_amd64_emit_rt_call(&c, "__module_init");

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

    /* Intern every string constant once: the compiled program asks the runtime
     * for the table entry by index, and the linked string table carries the
     * bytes. The order here is the order ns_build_write_strtab_c records. */
    ns_bool has_init = false;
    ns_bool has_main = false;
    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        if (ns_str_equals_STR(ssa->fns[i].name, "__module_init")) has_init = true;
        if (ns_str_equals_STR(ssa->fns[i].name, "main")) has_main = true;
        for (i32 ii = 0, il = (i32)ns_array_length(ssa->fns[i].insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &ssa->fns[i].insts[ii];
            if (inst->op != NS_SSA_OP_CONST || !ns_type_is(inst->type, NS_TYPE_STRING)) continue;
            ns_str value = ns_str_unescape(inst->name);
            i32 id = -1;
            for (i32 s = 0; s < m->nstr; ++s) {
                if (m->strlens[s] == value.len &&
                    memcmp(m->strtab[s], value.data, (szt)value.len) == 0) {
                    id = s;
                    break;
                }
            }
            if (id < 0) {
                id = m->nstr;
                m->strtab = realloc(m->strtab, sizeof(char *) * (szt)(id + 1));
                m->strlens = realloc(m->strlens, sizeof(i32) * (szt)(id + 1));
                char *copy = ns_malloc((szt)value.len + 1);
                if (value.len > 0) memcpy(copy, value.data, (szt)value.len);
                copy[value.len] = 0;
                m->strtab[id] = copy;
                m->strlens[id] = value.len;
                m->nstr = id + 1;
            }
            inst->c = id;
            ns_str_free(value);
        }
    }

    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        ns_bool is_main = ns_str_equals_STR(ssa->fns[i].name, "main");
        ns_amd64_fn_bin fn = ns_amd64_lower_fn(ssa, &ssa->fns[i], is_main, is_main && has_init);
        ns_array_push(m->fns, fn);
    }
    if (!has_main && has_init) {
        /* A module without an entry still has to run its initializers when it
         * is linked as a program. */
        ns_amd64_ctx c = {0};
        ns_amd64_ctx_set_abi(&c);
        ns_amd64_emit_push_r(&c, NS_AMD64_RBP);
        ns_amd64_emit_mov_rr(&c, NS_AMD64_RBP, NS_AMD64_RSP);
        ns_amd64_emit_alu_ri32(&c, 5, NS_AMD64_RSP, c.win_abi ? NS_AMD64_SHADOW : 16);
        ns_amd64_emit_rt_call(&c, "ns_rt_init");
        ns_amd64_emit_rt_call(&c, "__module_init");
        ns_amd64_emit_xor32_rr(&c, NS_AMD64_RAX);
        ns_amd64_emit_leave(&c);
        ns_amd64_emit_ret(&c);
        ns_amd64_fn_bin main_fn = {.name = ns_str_cstr("main"), .text = c.text, .call_fixups = c.call_fixups};
        ns_array_push(m->fns, main_fn);
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
    for (i32 i = 0; i < m->nstr; ++i) ns_free(m->strtab[i]);
    free(m->strtab);
    free(m->strlens);
    ns_free(m);
}
