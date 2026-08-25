#include "ns_aarch.h"

#include <string.h>

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

#define NS_AARCH_X0  0
#define NS_AARCH_X9  9
#define NS_AARCH_X10 10
#define NS_AARCH_X11 11
#define NS_AARCH_X16 16
#define NS_AARCH_FP  29
#define NS_AARCH_SP  31
#define NS_AARCH_EXTRA_MAX 64
/* One spill slot per register-passed argument, for the ffi pointer dance. */
#define NS_AARCH_FFI_SCRATCH 8

/* ── intra-function fixup ─────────────────────────────────────────────────── */
typedef struct ns_aarch_fixup {
    u32 off;           /* byte offset of the (unconditional B) branch in text */
    i32 target_block;  /* SSA block index to branch to */
} ns_aarch_fixup;

typedef struct ns_aarch_ctx {
    ns_ssa_module *ssa;
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
    i32 extra_args[NS_AARCH_EXTRA_MAX];
    i32 nextra;
    /* first slot past the SSA values, used to park converted ffi pointers */
    i32 scratch_base;
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

/* ADD/SUB SP, SP, Xm, UXTX #0 — Rd/Rn=31 is SP here, not XZR. */
static u32 ns_aarch_add_sp_ext(i32 rm) {
    return 0x8B2063FFu | ((u32)rm << 16);
}

static u32 ns_aarch_sub_sp_ext(i32 rm) {
    return 0xCB2063FFu | ((u32)rm << 16);
}

/* MUL Xd, Xn, Xm  (MADD Xd, Xn, Xm, XZR) */
static u32 ns_aarch_mul_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9B007C00u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* SDIV Xd, Xn, Xm */
static u32 ns_aarch_sdiv_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9AC00C00u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

/* UDIV Xd, Xn, Xm */
static u32 ns_aarch_udiv_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9AC00800u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
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

/* ASRV Xd, Xn, Xm */
static u32 ns_aarch_asrv_rrr(i32 rd, i32 rn, i32 rm) {
    return 0x9AC02800u | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
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

static void ns_aarch_emit_rt_call(ns_aarch_ctx *c, const char *name) {
    u32 bl_off = (u32)ns_array_length(c->text);
    ns_aarch_emit_u32(c, ns_aarch_bl(0));
    ns_aarch_call_fixup cf = {.off = bl_off, .callee = ns_str_cstr((i8 *)name)};
    ns_array_push(c->call_fixups, cf);
}

static ns_bool ns_aarch_is_float(ns_type t) {
    return ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64);
}

static ns_bool ns_aarch_is_string(ns_type t) {
    return ns_type_is(t, NS_TYPE_STRING);
}

/* Native code sees a str as a char*, a ref as a host pointer, and an array as
 * the bare buffer its elements live in. Each of those is a runtime call away
 * from the ns value; every other argument type crosses the boundary as it is. */
static const char *ns_aarch_ffi_convert(ns_type t) {
    if (ns_type_is_array(t)) return "ns_rt_array_ptr";
    if (ns_aarch_is_string(t)) return "ns_rt_to_cstr";
    if (ns_type_is_ref(t)) return "ns_rt_native_ptr";
    return NULL;
}

static const char *ns_aarch_map_std(ns_str module, ns_str name) {
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

static const char *ns_aarch_map_task(ns_str module, ns_str name) {
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

static ns_bool ns_aarch_is_ffi_module(ns_str module) {
    if (module.len == 0) return false;
    if (ns_str_equals(module, ns_str_cstr("std"))) return false;
    if (ns_str_equals(module, ns_str_cstr("task"))) return false;
    if (ns_str_equals(module, ns_str_cstr("simd"))) return false;
    if (ns_str_equals(module, ns_str_cstr("shader"))) return false;
    return true;
}

static ns_ssa_import *ns_aarch_find_import(ns_aarch_ctx *c, ns_str module, ns_str name) {
    if (!c->ssa) return NULL;
    for (i32 i = 0, l = (i32)ns_array_length(c->ssa->imports); i < l; ++i) {
        ns_ssa_import *im = &c->ssa->imports[i];
        if (ns_str_equals(im->module, module) && ns_str_equals(im->name, name)) return im;
    }
    return NULL;
}

static i32 ns_aarch_collect_call_args(ns_aarch_ctx *c, ns_ssa_inst *inst, i32 *args, i32 max_args) {
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

static ns_type ns_aarch_value_type(ns_ssa_fn *fn, i32 value) {
    if (value < 0 || !fn) return ns_type_unknown;
    for (i32 i = (i32)ns_array_length(fn->insts) - 1; i >= 0; --i) {
        if (fn->insts[i].dst == value) return fn->insts[i].type;
    }
    return ns_type_unknown;
}

static i32 ns_aarch_load_size(ns_type t) {
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

/* FMOV Dd, Xn / FMOV Sd, Wn */
static u32 ns_aarch_fmov_dx(i32 rd, i32 rn, ns_bool f64) {
    return (f64 ? 0x9E670000u : 0x1E270000u) | ((u32)rn << 5) | (u32)rd;
}

/* FMOV Xd, Dn / FMOV Wd, Sn */
static u32 ns_aarch_fmov_xd(i32 rd, i32 rn, ns_bool f64) {
    return (f64 ? 0x9E660000u : 0x1E260000u) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_fadd(i32 rd, i32 rn, i32 rm, ns_bool f64) {
    return (f64 ? 0x1E602800u : 0x1E202800u) | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_fsub(i32 rd, i32 rn, i32 rm, ns_bool f64) {
    return (f64 ? 0x1E603800u : 0x1E203800u) | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_fmul(i32 rd, i32 rn, i32 rm, ns_bool f64) {
    return (f64 ? 0x1E600800u : 0x1E200800u) | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_fdiv(i32 rd, i32 rn, i32 rm, ns_bool f64) {
    return (f64 ? 0x1E601800u : 0x1E201800u) | ((u32)rm << 16) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_fneg(i32 rd, i32 rn, ns_bool f64) {
    return (f64 ? 0x1E614000u : 0x1E214000u) | ((u32)rn << 5) | (u32)rd;
}

static u32 ns_aarch_fcmp(i32 rn, i32 rm, ns_bool f64) {
    return (f64 ? 0x1E602000u : 0x1E202000u) | ((u32)rm << 16) | ((u32)rn << 5);
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
/* Slot v lives at [x29, #-8*(v+1)] so outgoing stack args can move SP. */
static void ns_aarch_addr_from_fp(ns_aarch_ctx *c, i32 bytes) {
    ns_aarch_emit_const_u64(c, NS_AARCH_X16, (u64)(u32)bytes);
    ns_aarch_emit_u32(c, ns_aarch_sub_rrr(NS_AARCH_X16, NS_AARCH_FP, NS_AARCH_X16));
}

static void ns_aarch_load_value(ns_aarch_ctx *c, i32 reg, i32 v) {
    if (v < 0) return;
    i32 off = 8 * (v + 1);
    if (off <= 255) {
        ns_aarch_emit_u32(c, 0xF8400000u | (((u32)(-off) & 0x1FFu) << 12) |
            ((u32)NS_AARCH_FP << 5) | (u32)reg); /* LDUR Xt, [x29, #-off] */
        return;
    }
    ns_aarch_addr_from_fp(c, off);
    ns_aarch_emit_u32(c, 0xF9400000u | ((u32)NS_AARCH_X16 << 5) | (u32)reg);
}

static void ns_aarch_store_value(ns_aarch_ctx *c, i32 v, i32 reg) {
    if (v < 0) return;
    i32 off = 8 * (v + 1);
    if (off <= 255) {
        ns_aarch_emit_u32(c, 0xF8000000u | (((u32)(-off) & 0x1FFu) << 12) |
            ((u32)NS_AARCH_FP << 5) | (u32)reg); /* STUR Xt, [x29, #-off] */
        return;
    }
    ns_aarch_addr_from_fp(c, off);
    ns_aarch_emit_u32(c, 0xF9000000u | ((u32)NS_AARCH_X16 << 5) | (u32)reg);
}

static void ns_aarch_emit_float_binop(ns_aarch_ctx *c, ns_ssa_inst *inst) {
    ns_bool f64 = ns_type_is(inst->type, NS_TYPE_F64);
    ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
    ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
    ns_aarch_emit_u32(c, ns_aarch_fmov_dx(0, NS_AARCH_X9, f64));
    ns_aarch_emit_u32(c, ns_aarch_fmov_dx(1, NS_AARCH_X10, f64));
    switch (inst->op) {
    case NS_SSA_OP_ADD: ns_aarch_emit_u32(c, ns_aarch_fadd(0, 0, 1, f64)); break;
    case NS_SSA_OP_SUB: ns_aarch_emit_u32(c, ns_aarch_fsub(0, 0, 1, f64)); break;
    case NS_SSA_OP_MUL: ns_aarch_emit_u32(c, ns_aarch_fmul(0, 0, 1, f64)); break;
    default:            ns_aarch_emit_u32(c, ns_aarch_fdiv(0, 0, 1, f64)); break;
    }
    ns_aarch_emit_u32(c, ns_aarch_fmov_xd(NS_AARCH_X9, 0, f64));
    ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
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

/* Narrow a register to the width/signedness of an integer value so
 * that e.g. `300 as u8` yields 44. Wider or non-integer targets are no-ops. */
static void ns_aarch_narrow_reg(ns_aarch_ctx *c, i32 reg, ns_type t) {
    u32 rn_rd = ((u32)reg << 5) | (u32)reg;
    if (ns_type_is(t, NS_TYPE_I8))       ns_aarch_emit_u32(c, 0x93401C00u | rn_rd); /* SXTB Xd, Wn */
    else if (ns_type_is(t, NS_TYPE_U8))  ns_aarch_emit_u32(c, 0x12001C00u | rn_rd); /* AND Wd, Wn, #0xFF */
    else if (ns_type_is(t, NS_TYPE_I16)) ns_aarch_emit_u32(c, 0x93403C00u | rn_rd); /* SXTH Xd, Wn */
    else if (ns_type_is(t, NS_TYPE_U16)) ns_aarch_emit_u32(c, 0x12003C00u | rn_rd); /* AND Wd, Wn, #0xFFFF */
    else if (ns_type_is(t, NS_TYPE_I32)) ns_aarch_emit_u32(c, 0x93407C00u | rn_rd); /* SXTW Xd, Wn */
    else if (ns_type_is(t, NS_TYPE_U32)) ns_aarch_emit_u32(c, 0x2A0003E0u | ((u32)reg << 16) | (u32)reg); /* MOV Wd, Wn */
}

static void ns_aarch_narrow_x9(ns_aarch_ctx *c, ns_type t) {
    ns_aarch_narrow_reg(c, NS_AARCH_X9, t);
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
        i32 src = ns_ssa_phi_incoming(inst, from);
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
        if (inst->dst < 0 || inst->c < 0) break;
        if (inst->c < 8) {
            ns_aarch_store_value(c, inst->dst, inst->c);
            break;
        }
        i32 imm12 = 2 + (inst->c - 8); /* [x29, #16 + 8*(c-8)] */
        ns_aarch_emit_u32(c, 0xF9400000u | (((u32)imm12 & 0xFFFu) << 10) |
            ((u32)NS_AARCH_FP << 5) | (u32)NS_AARCH_X9);
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        if (ns_type_is(inst->type, NS_TYPE_STRING)) {
            ns_aarch_emit_const_u64(c, NS_AARCH_X0, (u64)(u32)inst->c);
            ns_aarch_emit_rt_call(c, "ns_rt_intern");
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
            break;
        }
        if (ns_type_is(inst->type, NS_TYPE_F64)) {
            f64 fv = ns_str_to_f64(inst->name);
            u64 bits = 0;
            memcpy(&bits, &fv, 8);
            ns_aarch_emit_const_u64(c, NS_AARCH_X9, bits);
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
            break;
        }
        if (ns_type_is(inst->type, NS_TYPE_F32)) {
            f32 fv = (f32)ns_str_to_f64(inst->name);
            u32 bits = 0;
            memcpy(&bits, &fv, 4);
            ns_aarch_emit_const_u64(c, NS_AARCH_X9, bits);
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
            break;
        }
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
        ns_type src_t = ns_aarch_value_type(c->fn, inst->a);
        ns_bool src_f = ns_aarch_is_float(src_t);
        ns_bool dst_f = ns_aarch_is_float(inst->type);
        if (dst_f && src_f) {
            ns_bool src64 = ns_type_is(src_t, NS_TYPE_F64);
            ns_bool dst64 = ns_type_is(inst->type, NS_TYPE_F64);
            if (src64 != dst64) {
                ns_aarch_emit_u32(c, ns_aarch_fmov_dx(0, NS_AARCH_X9, src64));
                ns_aarch_emit_u32(c, src64 ? 0x1E624000u : 0x1E22C000u); /* FCVT S0,D0 / D0,S0 */
                ns_aarch_emit_u32(c, ns_aarch_fmov_xd(NS_AARCH_X9, 0, dst64));
            }
        } else if (dst_f && !src_f) {
            ns_bool dst64 = ns_type_is(inst->type, NS_TYPE_F64);
            ns_bool uns = ns_type_unsigned(src_t);
            u32 scvtf = dst64 ? (uns ? 0x9E630000u : 0x9E620000u)
                              : (uns ? 0x9E230000u : 0x9E220000u);
            ns_aarch_emit_u32(c, scvtf | ((u32)NS_AARCH_X9 << 5)); /* [SU]CVTF D0/S0, X9 */
            ns_aarch_emit_u32(c, ns_aarch_fmov_xd(NS_AARCH_X9, 0, dst64));
        } else if (!dst_f && src_f) {
            ns_bool src64 = ns_type_is(src_t, NS_TYPE_F64);
            ns_bool uns = ns_type_unsigned(inst->type);
            ns_aarch_emit_u32(c, ns_aarch_fmov_dx(0, NS_AARCH_X9, src64));
            u32 fcvtz = src64 ? (uns ? 0x9E790000u : 0x9E780000u)
                              : (uns ? 0x9E390000u : 0x9E380000u);
            ns_aarch_emit_u32(c, fcvtz); /* FCVTZ[SU] X0, D0/S0 */
            ns_aarch_emit_u32(c, 0xAA0003E9u); /* MOV X9, X0 */
            ns_aarch_narrow_x9(c, inst->type);
        } else {
            ns_aarch_narrow_x9(c, inst->type);
        }
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
        ns_type at = ns_aarch_value_type(c->fn, inst->a);
        if (inst->op == NS_SSA_OP_ADD &&
            (ns_aarch_is_string(inst->type) || ns_aarch_is_string(at))) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_load_value(c, 1, inst->b);
            ns_aarch_emit_rt_call(c, "ns_rt_strcat");
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
            break;
        }
        if ((inst->op == NS_SSA_OP_ADD || inst->op == NS_SSA_OP_SUB || inst->op == NS_SSA_OP_MUL) &&
            ns_aarch_is_float(inst->type)) {
            ns_aarch_emit_float_binop(c, inst);
            break;
        }
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
        default:
            if (ns_type_unsigned(at)) {
                ns_aarch_emit_u32(c, ns_aarch_lsrv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10));
            } else {
                ns_aarch_emit_u32(c, ns_aarch_asrv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10));
            }
            break;
        }
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_DIV: {
        if (inst->dst < 0) break;
        if (ns_aarch_is_float(inst->type)) {
            ns_aarch_emit_float_binop(c, inst);
            break;
        }
        ns_type at = ns_aarch_value_type(c->fn, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
        if (ns_type_unsigned(at)) {
            ns_aarch_emit_u32(c, ns_aarch_udiv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10));
        } else {
            ns_aarch_emit_u32(c, ns_aarch_sdiv_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10));
        }
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_MOD: {
        if (inst->dst < 0) break;
        if (ns_aarch_is_float(inst->type)) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_load_value(c, 1, inst->b);
            ns_aarch_emit_rt_call(c, ns_type_is(inst->type, NS_TYPE_F32) ? "ns_rt_fmodf" : "ns_rt_fmod");
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
            break;
        }
        /* Xd = Xn - (Xn / Xm) * Xm  (SDIV + MSUB) */
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
        ns_aarch_emit_u32(c, ns_aarch_sdiv_rrr(NS_AARCH_X11, NS_AARCH_X9, NS_AARCH_X10));
        ns_aarch_emit_u32(c, ns_aarch_msub_rrrr(NS_AARCH_X9, NS_AARCH_X11, NS_AARCH_X10, NS_AARCH_X9));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
        ns_type nt = ns_aarch_is_float(inst->type) ? inst->type : ns_aarch_value_type(c->fn, inst->a);
        if (ns_aarch_is_float(nt)) {
            ns_bool f64 = ns_type_is(nt, NS_TYPE_F64);
            ns_aarch_emit_u32(c, ns_aarch_fmov_dx(0, NS_AARCH_X9, f64));
            ns_aarch_emit_u32(c, ns_aarch_fneg(0, 0, f64));
            ns_aarch_emit_u32(c, ns_aarch_fmov_xd(NS_AARCH_X9, 0, f64));
        } else {
            ns_aarch_emit_u32(c, ns_aarch_neg_rr(NS_AARCH_X9, NS_AARCH_X9));
        }
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
        ns_type at = ns_aarch_value_type(c->fn, inst->a);
        ns_type bt = ns_aarch_value_type(c->fn, inst->b);
        if (ns_aarch_is_string(at) || ns_aarch_is_string(bt)) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_load_value(c, 1, inst->b);
            ns_aarch_emit_rt_call(c, "ns_rt_strcmp");
            ns_aarch_emit_u32(c, ns_aarch_movz(NS_AARCH_X10, 0, 0));
            ns_aarch_emit_u32(c, ns_aarch_cmp_rr(NS_AARCH_X0, NS_AARCH_X10));
        } else {
            ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
            ns_aarch_load_value(c, NS_AARCH_X10, inst->b);
            ns_bool use_fcmp = ns_aarch_is_float(at) || ns_aarch_is_float(bt);
            if (use_fcmp) {
                ns_bool f64 = ns_type_is(at, NS_TYPE_F64) || ns_type_is(bt, NS_TYPE_F64);
                ns_aarch_emit_u32(c, ns_aarch_fmov_dx(0, NS_AARCH_X9, f64));
                ns_aarch_emit_u32(c, ns_aarch_fmov_dx(1, NS_AARCH_X10, f64));
                ns_aarch_emit_u32(c, ns_aarch_fcmp(0, 1, f64));
            } else {
                ns_aarch_emit_u32(c, ns_aarch_cmp_rr(NS_AARCH_X9, NS_AARCH_X10));
            }
        }
        ns_bool uns = ns_type_unsigned(at) && !ns_aarch_is_float(at) && !ns_aarch_is_string(at);
        u32 inv_cond;
        switch (inst->op) {
        case NS_SSA_OP_EQ: inv_cond = 1u;  break; /* NE */
        case NS_SSA_OP_NE: inv_cond = 0u;  break; /* EQ */
        case NS_SSA_OP_LT: inv_cond = uns ? 2u : 10u; break; /* HS / GE */
        case NS_SSA_OP_LE: inv_cond = uns ? 8u : 12u; break; /* HI / GT */
        case NS_SSA_OP_GT: inv_cond = uns ? 9u : 13u; break; /* LS / LE */
        default:           inv_cond = uns ? 3u : 11u; break; /* LO / LT (for GE) */
        }
        ns_aarch_emit_u32(c, ns_aarch_cset_r(NS_AARCH_X9, inv_cond));
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
    } break;
    case NS_SSA_OP_ARG: {
        if (c->arg_seq < 8) {
            ns_aarch_load_value(c, c->arg_seq++, inst->a);
            break;
        }
        if (c->nextra < NS_AARCH_EXTRA_MAX) c->extra_args[c->nextra++] = inst->a;
        c->arg_seq++;
    } break;
    case NS_SSA_OP_CALL: {
        ns_str peek_name = inst->name;
        ns_bool peek_ffi = ns_aarch_is_ffi_module(inst->module);
        ns_bool indirect = peek_name.len == 0 && inst->a >= 0 &&
                           ns_aarch_map_std(inst->module, inst->name) == NULL &&
                           ns_aarch_map_task(inst->module, inst->name) == NULL;
        i32 nextra = c->nextra;
        i32 space = 0;
        if (nextra > 0 && !indirect && !peek_ffi) {
            space = (nextra * 8 + 15) & ~15;
            if (space <= 4095) ns_aarch_emit_u32(c, ns_aarch_sub_sp_imm(space));
            else {
                ns_aarch_emit_const_u64(c, NS_AARCH_X9, (u64)(u32)space);
                ns_aarch_emit_u32(c, ns_aarch_sub_sp_ext(NS_AARCH_X9));
            }
            for (i32 ei = 0; ei < nextra; ++ei) {
                ns_aarch_load_value(c, NS_AARCH_X9, c->extra_args[ei]);
                ns_aarch_emit_u32(c, 0xF9000000u | (((u32)ei & 0xFFFu) << 10) |
                    ((u32)NS_AARCH_SP << 5) | (u32)NS_AARCH_X9); /* STR X9, [sp, #8*ei] */
            }
        }
        c->arg_seq = 0;
        c->nextra = 0;
        ns_str callee_name = inst->name;
        const char *std_rt = ns_aarch_map_std(inst->module, callee_name);
        if (std_rt) callee_name = ns_str_cstr((i8 *)std_rt);
        const char *task_rt = ns_aarch_map_task(inst->module, callee_name);
        if (task_rt) callee_name = ns_str_cstr((i8 *)task_rt);
        ns_bool ffi = ns_aarch_is_ffi_module(inst->module) && callee_name.len > 0;
        if (ffi) {
            ns_ssa_import *im = ns_aarch_find_import(c, inst->module, inst->name);
            i32 args[NS_AARCH_EXTRA_MAX + 8];
            i32 nargs = ns_aarch_collect_call_args(c, inst, args, NS_AARCH_EXTRA_MAX + 8);
            i32 dest_reg[NS_AARCH_EXTRA_MAX + 8];
            i32 stack_n = 0;
            i32 stack_args[NS_AARCH_EXTRA_MAX];
            i32 igpr = 0;
            i32 fpr = 0;
            for (i32 ai = 0; ai < nargs; ++ai) dest_reg[ai] = -1;
            for (i32 ai = 0; ai < nargs; ++ai) {
                ns_type at = ns_type_unknown;
                if (im && ai < (i32)ns_array_length(im->params)) at = im->params[ai];
                else at = ns_aarch_value_type(c->fn, args[ai]);
                if (ns_aarch_is_float(at) && fpr < 8) {
                    dest_reg[ai] = 8 + fpr;
                    fpr++;
                    continue;
                }
                if (igpr < 8) dest_reg[ai] = igpr++;
                else stack_args[stack_n++] = args[ai];
            }
            if (stack_n > 0) {
                space = (stack_n * 8 + 15) & ~15;
                if (space <= 4095) ns_aarch_emit_u32(c, ns_aarch_sub_sp_imm(space));
                else {
                    ns_aarch_emit_const_u64(c, NS_AARCH_X9, (u64)(u32)space);
                    ns_aarch_emit_u32(c, ns_aarch_sub_sp_ext(NS_AARCH_X9));
                }
                for (i32 ei = 0; ei < stack_n; ++ei) {
                    ns_type st = ns_aarch_value_type(c->fn, stack_args[ei]);
                    const char *conv = ns_aarch_ffi_convert(st);
                    if (conv) {
                        ns_aarch_load_value(c, NS_AARCH_X0, stack_args[ei]);
                        ns_aarch_emit_rt_call(c, conv);
                        ns_aarch_emit_u32(c, 0xAA0003E9u); /* MOV X9, X0 */
                    } else {
                        ns_aarch_load_value(c, NS_AARCH_X9, stack_args[ei]);
                    }
                    ns_aarch_emit_u32(c, 0xF9000000u | (((u32)ei & 0xFFFu) << 10) |
                        ((u32)NS_AARCH_SP << 5) | (u32)NS_AARCH_X9);
                }
            }
            // The conversion helpers return in x0, and reaching one costs a call
            // that clobbers x0-x7. A pointer left in an argument register would
            // not survive the conversion of the next argument, so every
            // conversion parks its result in a scratch slot and the registers
            // are filled once the last call has been made.
            i32 spill_slot[NS_AARCH_EXTRA_MAX + 8];
            for (i32 ai = 0; ai < nargs; ++ai) spill_slot[ai] = -1;
            i32 nspill = 0;
            for (i32 ai = 0; ai < nargs; ++ai) {
                ns_type at = ns_type_unknown;
                if (im && ai < (i32)ns_array_length(im->params)) at = im->params[ai];
                else at = ns_aarch_value_type(c->fn, args[ai]);
                const char *conv = ns_aarch_ffi_convert(at);
                if (!conv) continue;
                i32 dest = dest_reg[ai];
                if (dest < 0 || dest >= 8) continue;
                ns_aarch_load_value(c, NS_AARCH_X0, args[ai]);
                ns_aarch_emit_rt_call(c, conv);
                spill_slot[ai] = c->scratch_base + nspill++;
                ns_aarch_store_value(c, spill_slot[ai], NS_AARCH_X0);
            }
            for (i32 ai = 0; ai < nargs; ++ai) {
                if (spill_slot[ai] < 0) continue;
                ns_aarch_load_value(c, dest_reg[ai], spill_slot[ai]);
            }
            for (i32 ai = 0; ai < nargs; ++ai) {
                ns_type at = ns_type_unknown;
                if (im && ai < (i32)ns_array_length(im->params)) at = im->params[ai];
                else at = ns_aarch_value_type(c->fn, args[ai]);
                if (ns_aarch_ffi_convert(at)) continue;
                i32 dest = dest_reg[ai];
                if (dest >= 8) {
                    ns_aarch_load_value(c, NS_AARCH_X9, args[ai]);
                    ns_aarch_emit_u32(c, ns_aarch_fmov_dx(dest - 8, NS_AARCH_X9,
                                                          ns_type_is(at, NS_TYPE_F64)));
                } else if (dest >= 0) {
                    ns_aarch_load_value(c, dest, args[ai]);
                }
            }
            u32 bl_off = (u32)ns_array_length(c->text);
            ns_aarch_emit_u32(c, ns_aarch_bl(0));
            ns_aarch_call_fixup cf = {.off = bl_off, .callee = callee_name, .kind = 0};
            ns_array_push(c->call_fixups, cf);
            ns_type ret = im ? im->ret : inst->type;
            if (ns_aarch_is_float(ret)) {
                ns_aarch_emit_u32(c, ns_aarch_fmov_xd(0, 0, ns_type_is(ret, NS_TYPE_F64)));
            } else if (ns_aarch_is_string(ret) && !ns_type_is_array(ret)) {
                ns_aarch_emit_rt_call(c, "ns_rt_from_cstr");
            } else {
                // C callees may leave the upper half of x0 unspecified for a
                // result narrower than 64 bits. Normalize it before Nano
                // Script performs signed comparisons or stores the value.
                ns_aarch_narrow_reg(c, NS_AARCH_X0, ret);
            }
            if (inst->dst >= 0) ns_aarch_store_value(c, inst->dst, 0);
            if (space > 0) {
                if (space <= 4095) {
                    ns_aarch_emit_u32(c, 0x910003FFu | (((u32)space & 0xFFFu) << 10));
                } else {
                    ns_aarch_emit_const_u64(c, NS_AARCH_X9, (u64)(u32)space);
                    ns_aarch_emit_u32(c, ns_aarch_add_sp_ext(NS_AARCH_X9));
                }
            }
            break;
        }
        if (callee_name.len == 0 && inst->a >= 0) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_emit_const_u64(c, 1, 0);
            ns_aarch_emit_const_u64(c, 2, 8);
            ns_aarch_emit_rt_call(c, "ns_rt_load");
            ns_aarch_emit_u32(c, 0xAA0003F1u); /* MOV X17, X0 — keep code off X16 */
            ns_ssa_block *bb = &c->fn->blocks[c->cur_block];
            i32 args[NS_AARCH_EXTRA_MAX + 8];
            i32 nargs = inst->c > 0 ? inst->c : 0;
            if (nargs > NS_AARCH_EXTRA_MAX + 8) nargs = NS_AARCH_EXTRA_MAX + 8;
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
            if (got < nargs) nargs = got;
            i32 nextra_i = 0;
            if (nargs > 8) {
                nextra_i = nargs - 8;
                i32 extra_space = (nextra_i * 8 + 15) & ~15;
                if (extra_space <= 4095) ns_aarch_emit_u32(c, ns_aarch_sub_sp_imm(extra_space));
                else {
                    ns_aarch_emit_const_u64(c, NS_AARCH_X9, (u64)(u32)extra_space);
                    ns_aarch_emit_u32(c, ns_aarch_sub_sp_ext(NS_AARCH_X9));
                }
                space = extra_space;
                for (i32 ei = 0; ei < nextra_i; ++ei) {
                    ns_aarch_load_value(c, NS_AARCH_X9, args[8 + ei]);
                    ns_aarch_emit_u32(c, 0xF9000000u | (((u32)ei & 0xFFFu) << 10) |
                        ((u32)NS_AARCH_SP << 5) | (u32)NS_AARCH_X9);
                }
            }
            i32 nreg = nargs < 8 ? nargs : 8;
            for (i32 ri = 0; ri < nreg; ++ri) ns_aarch_load_value(c, ri, args[ri]);
            ns_aarch_emit_u32(c, 0xD63F0220u); /* BLR X17 */
        } else {
            u32 bl_off = (u32)ns_array_length(c->text);
            ns_aarch_emit_u32(c, ns_aarch_bl(0));
            if (callee_name.len > 0) {
                ns_aarch_call_fixup cf = {.off = bl_off, .callee = callee_name, .kind = 0};
                ns_array_push(c->call_fixups, cf);
            }
        }
        if (inst->dst >= 0) ns_aarch_store_value(c, inst->dst, 0);
        if (space > 0) {
            if (space <= 4095) {
                ns_aarch_emit_u32(c, 0x910003FFu | (((u32)space & 0xFFFu) << 10)); /* ADD SP, SP, #space */
            } else {
                ns_aarch_emit_const_u64(c, NS_AARCH_X9, (u64)(u32)space);
                ns_aarch_emit_u32(c, ns_aarch_add_sp_ext(NS_AARCH_X9));
            }
        }
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
        else ns_aarch_emit_u32(c, ns_aarch_movz(0, 0, 0));
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
    case NS_SSA_OP_GLOBAL_GET: {
        if (inst->dst < 0) break;
        ns_aarch_emit_const_u64(c, NS_AARCH_X0, (u64)(u32)inst->c);
        ns_aarch_emit_rt_call(c, "ns_rt_gget");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
    } break;
    case NS_SSA_OP_GLOBAL_SET: {
        ns_aarch_emit_const_u64(c, NS_AARCH_X0, (u64)(u32)inst->c);
        ns_aarch_load_value(c, 1, inst->a);
        ns_aarch_emit_rt_call(c, "ns_rt_gset");
    } break;
    case NS_SSA_OP_ALLOC: {
        if (inst->dst < 0) break;
        ns_aarch_emit_const_u64(c, NS_AARCH_X0, (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_aarch_emit_rt_call(c, "ns_rt_alloc");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
    } break;
    case NS_SSA_OP_CLONE: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_emit_const_u64(c, 1, (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_aarch_emit_rt_call(c, "ns_rt_clone");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
    } break;
    case NS_SSA_OP_SCOPE_ENTER: {
        if (inst->dst < 0) break;
        ns_aarch_emit_rt_call(c, "ns_rt_scope_enter");
        ns_aarch_store_value(c, inst->dst, 0);
    } break;
    case NS_SSA_OP_SCOPE_LEAVE: {
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        if (inst->b >= 0) ns_aarch_load_value(c, 1, inst->b);
        else ns_aarch_emit_u32(c, ns_aarch_movz(1, 0, 0));
        ns_aarch_emit_const_u64(c, 2, (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_aarch_emit_rt_call(c, "ns_rt_scope_leave");
        if (inst->dst >= 0) ns_aarch_store_value(c, inst->dst, 0);
    } break;
    case NS_SSA_OP_PIN: {
        /* target0 selects how much of the value the runtime has to keep: a flat
         * block of inst->c bytes, a string and its bytes, an array and its
         * payload of inst->c-byte elements, or everything allocated so far. */
        if (inst->target0 == 3 || inst->a < 0) {
            ns_aarch_emit_rt_call(c, "ns_rt_pin_all");
            break;
        }
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        if (inst->target0 == 1) {
            ns_aarch_emit_rt_call(c, "ns_rt_pin_str");
            break;
        }
        ns_aarch_emit_const_u64(c, 1, (u64)(u32)(inst->c > 0 ? inst->c : 0));
        ns_aarch_emit_rt_call(c, inst->target0 == 2 ? "ns_rt_pin_array" : "ns_rt_pin");
    } break;
    case NS_SSA_OP_LOAD: {
        if (inst->dst < 0) break;
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_ref(inst->type)) {
            ns_aarch_load_value(c, NS_AARCH_X9, inst->a);
            ns_aarch_emit_const_u64(c, NS_AARCH_X10, (u64)(u32)inst->c);
            ns_aarch_emit_u32(c, ns_aarch_add_rrr(NS_AARCH_X9, NS_AARCH_X9, NS_AARCH_X10));
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X9);
            break;
        }
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_emit_const_u64(c, 1, (u64)(u32)inst->c);
        {
            i32 size = inst->target0 > 0 ? inst->target0 : ns_aarch_load_size(inst->type);
            ns_aarch_emit_const_u64(c, 2, (u64)(u32)size);
        }
        ns_aarch_emit_rt_call(c, "ns_rt_load");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
    } break;
    case NS_SSA_OP_STORE: {
        i32 size = inst->target0 > 0 ? inst->target0 : ns_aarch_load_size(inst->type);
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_ref(inst->type) && size > 0) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_emit_const_u64(c, 1, (u64)(u32)inst->c);
            ns_aarch_load_value(c, 2, inst->b);
            ns_aarch_emit_const_u64(c, 3, (u64)(u32)size);
            ns_aarch_emit_rt_call(c, "ns_rt_copy");
            break;
        }
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_emit_const_u64(c, 1, (u64)(u32)inst->c);
        ns_aarch_load_value(c, 2, inst->b);
        ns_aarch_emit_const_u64(c, 3, (u64)(u32)size);
        ns_aarch_emit_rt_call(c, "ns_rt_store");
    } break;
    case NS_SSA_OP_ARRAY_NEW: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_emit_const_u64(c, 1, (u64)(u32)(inst->c > 0 ? inst->c : 1));
        ns_aarch_emit_rt_call(c, "ns_rt_array_new");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
    } break;
    case NS_SSA_OP_ARRAY_STORE: {
        i32 stride = inst->c > 0 ? inst->c : 4;
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_ref(inst->type)) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_load_value(c, 1, inst->target0);
            ns_aarch_emit_const_u64(c, 2, (u64)(u32)stride);
            ns_aarch_emit_rt_call(c, "ns_rt_array_slot");
            ns_aarch_emit_const_u64(c, 1, 0);
            ns_aarch_load_value(c, 2, inst->b);
            ns_aarch_emit_const_u64(c, 3, (u64)(u32)stride);
            ns_aarch_emit_rt_call(c, "ns_rt_copy");
            break;
        }
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_load_value(c, 1, inst->target0);
        ns_aarch_load_value(c, 2, inst->b);
        ns_aarch_emit_const_u64(c, 3, (u64)(u32)stride);
        ns_aarch_emit_rt_call(c, "ns_rt_array_store");
    } break;
    case NS_SSA_OP_INDEX: {
        if (inst->dst < 0) break;
        i64 stride = inst->c > 0 ? inst->c : 1;
        if (ns_type_is(inst->type, NS_TYPE_STRUCT)) {
            ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
            ns_aarch_load_value(c, 1, inst->b);
            ns_aarch_emit_const_u64(c, 2, (u64)stride);
            ns_aarch_emit_rt_call(c, "ns_rt_array_slot");
            ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
            break;
        }
        /* String byte index is emitted as i32 with stride 1; load it unsigned. */
        if (stride == 1 && ns_type_is(inst->type, NS_TYPE_I32)) stride = -1;
        else if (ns_type_is(inst->type, NS_TYPE_U8)) stride = -1;
        else if (ns_type_is(inst->type, NS_TYPE_U16)) stride = -2;
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_load_value(c, 1, inst->b);
        ns_aarch_emit_const_u64(c, 2, (u64)stride);
        ns_aarch_emit_rt_call(c, "ns_rt_array_index");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
    } break;
    case NS_SSA_OP_FNADDR: {
        if (inst->dst < 0) break;
        i32 rd = NS_AARCH_X9;
        /* ADRP Xd, #0 ; ADD Xd, Xd, #0 — linker/test fills PAGE21 + PAGEOFF12. */
        u32 adrp_off = (u32)ns_array_length(c->text);
        ns_aarch_emit_u32(c, 0x90000000u | (u32)rd);
        ns_aarch_emit_u32(c, 0x91000000u | ((u32)rd << 5) | (u32)rd);
        if (inst->name.len > 0) {
            ns_aarch_call_fixup cf = {.off = adrp_off, .callee = inst->name, .kind = 1};
            ns_array_push(c->call_fixups, cf);
        }
        ns_aarch_store_value(c, inst->dst, rd);
    } break;
    case NS_SSA_OP_MEMBER: {
        if (inst->dst < 0) break;
        ns_aarch_load_value(c, NS_AARCH_X0, inst->a);
        ns_aarch_load_value(c, 1, inst->b);
        ns_aarch_emit_const_u64(c, 2, 8);
        ns_aarch_emit_rt_call(c, "ns_rt_load");
        ns_aarch_store_value(c, inst->dst, NS_AARCH_X0);
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
static ns_aarch_fn_bin ns_aarch_lower_fn(ns_ssa_module *ssa, ns_ssa_fn *fn, ns_bool call_rt_init, ns_bool call_mod_init) {
    ns_aarch_ctx c = {0};
    c.ssa = ssa;
    c.fn = fn;

    i32 nslots = ns_aarch_slot_count(fn);
    c.scratch_base = nslots;
    nslots += NS_AARCH_FFI_SCRATCH;
    /* Reserve one 8-byte slot per value, rounded up so SP stays 16-aligned. */
    i32 frame = ((nslots * 8) + 15) & ~15;

    /* emit function prologue */
    ns_aarch_emit_u32(&c, 0xA9BF7BFDu); /* STP x29, x30, [sp, #-16]! */
    ns_aarch_emit_u32(&c, 0x910003FDu); /* MOV x29, sp */
    if (frame > 0) {
        if (frame <= 4095) {
            ns_aarch_emit_u32(&c, ns_aarch_sub_sp_imm(frame));
        } else {
            ns_aarch_emit_const_u64(&c, NS_AARCH_X9, (u64)(u32)frame);
            ns_aarch_emit_u32(&c, ns_aarch_sub_sp_ext(NS_AARCH_X9));
        }
    }
    if (call_rt_init) ns_aarch_emit_rt_call(&c, "ns_rt_init");
    if (call_mod_init) ns_aarch_emit_rt_call(&c, "__module_init");

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
        ns_aarch_fn_bin fn = ns_aarch_lower_fn(ssa, &ssa->fns[i], is_main, is_main && has_init);
        ns_array_push(m->fns, fn);
    }
    if (!has_main && has_init) {
        ns_aarch_ctx c = {0};
        ns_aarch_emit_u32(&c, 0xA9BF7BFDu); /* STP x29, x30, [sp, #-16]! */
        ns_aarch_emit_u32(&c, 0x910003FDu); /* MOV x29, sp */
        ns_aarch_emit_rt_call(&c, "ns_rt_init");
        ns_aarch_emit_rt_call(&c, "__module_init");
        ns_aarch_emit_u32(&c, ns_aarch_movz(0, 0, 0));
        ns_aarch_emit_u32(&c, 0xA8C17BFDu); /* LDP x29, x30, [sp], #16 */
        ns_aarch_emit_u32(&c, ns_aarch_ret());
        ns_aarch_fn_bin main_fn = {.name = ns_str_cstr("main"), .text = c.text, .call_fixups = c.call_fixups};
        ns_array_push(m->fns, main_fn);
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
    for (i32 i = 0; i < m->nstr; ++i) ns_free(m->strtab[i]);
    free(m->strtab);
    free(m->strlens);
    ns_free(m);
}
