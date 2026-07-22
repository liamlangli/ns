// ns_wasm.c — WebAssembly binary emitter for NanoScript
//
// Translates SSA IR into a .wasm binary (MVP / WebAssembly 1.0).
//
// Control-flow strategy
// ─────────────────────
// SSA basic-blocks are lowered to WASM using the "block-wrapping" scheme:
//   • Forward edges  → nested `block` instructions; depth formula: br(j−k−1)
//   • Backward edges → `block $exit` + `loop $head` pairs; br 0 = back-edge,
//                      br 1 = exit the loop.
// A label stack tracks every open block/loop.  br_depth(target) linearly
// searches the stack from the top (depth 0 = innermost open label).

#include "ns_wasm.h"
#include <errno.h>
#include <math.h>

// ──────────────────────────────────────────────────────────────────────────
// WASM binary constants
// ──────────────────────────────────────────────────────────────────────────

static const u8 ns_wasm_magic[4]   = {0x00, 0x61, 0x73, 0x6D};
static const u8 ns_wasm_version[4] = {0x01, 0x00, 0x00, 0x00};

// Section ids
#define NS_WASM_SECT_TYPE      1
#define NS_WASM_SECT_IMPORT    2
#define NS_WASM_SECT_FUNCTION  3
#define NS_WASM_SECT_MEMORY    5
#define NS_WASM_SECT_GLOBAL    6
#define NS_WASM_SECT_EXPORT    7
#define NS_WASM_SECT_ELEMENT   9
#define NS_WASM_SECT_CODE     10
#define NS_WASM_SECT_DATA     11

// Value types
#define NS_WASM_I32   0x7F
#define NS_WASM_I64   0x7E
#define NS_WASM_F32   0x7D
#define NS_WASM_F64   0x7C
#define NS_WASM_BTVOID 0x40   // blocktype: no result

// Opcodes
#define NS_WASM_UNREACHABLE  0x00
#define NS_WASM_NOP          0x01
#define NS_WASM_BLOCK        0x02
#define NS_WASM_LOOP         0x03
#define NS_WASM_IF           0x04
#define NS_WASM_ELSE         0x05
#define NS_WASM_END          0x0B
#define NS_WASM_BR           0x0C
#define NS_WASM_BR_IF        0x0D
#define NS_WASM_RETURN       0x0F
#define NS_WASM_CALL         0x10
#define NS_WASM_DROP         0x1A
#define NS_WASM_LOCAL_GET    0x20
#define NS_WASM_LOCAL_SET    0x21
#define NS_WASM_LOCAL_TEE    0x22
#define NS_WASM_GLOBAL_GET   0x23
#define NS_WASM_GLOBAL_SET   0x24
#define NS_WASM_MEMORY_SIZE  0x3F
#define NS_WASM_MEMORY_GROW  0x40
#define NS_WASM_MISC_PREFIX  0xFC
#define NS_WASM_MEMORY_COPY  0x0A

#define NS_WASM_I32_LOAD     0x28
#define NS_WASM_I64_LOAD     0x29
#define NS_WASM_F32_LOAD     0x2A
#define NS_WASM_F64_LOAD     0x2B
#define NS_WASM_I32_LOAD8_S  0x2C
#define NS_WASM_I32_LOAD8_U  0x2D
#define NS_WASM_I32_LOAD16_S 0x2E
#define NS_WASM_I32_LOAD16_U 0x2F
#define NS_WASM_I32_STORE    0x36
#define NS_WASM_I64_STORE    0x37
#define NS_WASM_F32_STORE    0x38
#define NS_WASM_F64_STORE    0x39
#define NS_WASM_I32_STORE8   0x3A
#define NS_WASM_I32_STORE16  0x3B

#define NS_WASM_I32_CONST    0x41
#define NS_WASM_I64_CONST    0x42
#define NS_WASM_F32_CONST    0x43
#define NS_WASM_F64_CONST    0x44

// i32 comparisons
#define NS_WASM_I32_EQZ      0x45
#define NS_WASM_I32_EQ       0x46
#define NS_WASM_I32_NE       0x47
#define NS_WASM_I32_LT_S     0x48
#define NS_WASM_I32_LT_U     0x49
#define NS_WASM_I32_GT_S     0x4A
#define NS_WASM_I32_GT_U     0x4B
#define NS_WASM_I32_LE_S     0x4C
#define NS_WASM_I32_LE_U     0x4D
#define NS_WASM_I32_GE_S     0x4E
#define NS_WASM_I32_GE_U     0x4F

// i64 comparisons
#define NS_WASM_I64_EQ       0x51
#define NS_WASM_I64_NE       0x52
#define NS_WASM_I64_LT_S     0x53
#define NS_WASM_I64_LT_U     0x54
#define NS_WASM_I64_GT_S     0x55
#define NS_WASM_I64_GT_U     0x56
#define NS_WASM_I64_LE_S     0x57
#define NS_WASM_I64_LE_U     0x58
#define NS_WASM_I64_GE_S     0x59
#define NS_WASM_I64_GE_U     0x5A

// f32 comparisons
#define NS_WASM_F32_EQ       0x5B
#define NS_WASM_F32_NE       0x5C
#define NS_WASM_F32_LT       0x5D
#define NS_WASM_F32_GT       0x5E
#define NS_WASM_F32_LE       0x5F
#define NS_WASM_F32_GE       0x60

// f64 comparisons
#define NS_WASM_F64_EQ       0x61
#define NS_WASM_F64_NE       0x62
#define NS_WASM_F64_LT       0x63
#define NS_WASM_F64_GT       0x64
#define NS_WASM_F64_LE       0x65
#define NS_WASM_F64_GE       0x66

// i32 arithmetic
#define NS_WASM_I32_ADD      0x6A
#define NS_WASM_I32_SUB      0x6B
#define NS_WASM_I32_MUL      0x6C
#define NS_WASM_I32_DIV_S    0x6D
#define NS_WASM_I32_DIV_U    0x6E
#define NS_WASM_I32_REM_S    0x6F
#define NS_WASM_I32_REM_U    0x70
#define NS_WASM_I32_AND      0x71
#define NS_WASM_I32_OR       0x72
#define NS_WASM_I32_XOR      0x73
#define NS_WASM_I32_SHL      0x74
#define NS_WASM_I32_SHR_S    0x75
#define NS_WASM_I32_SHR_U    0x76

// i64 arithmetic
#define NS_WASM_I64_ADD      0x7C
#define NS_WASM_I64_SUB      0x7D
#define NS_WASM_I64_MUL      0x7E
#define NS_WASM_I64_DIV_S    0x7F
#define NS_WASM_I64_DIV_U    0x80
#define NS_WASM_I64_REM_S    0x81
#define NS_WASM_I64_REM_U    0x82
#define NS_WASM_I64_AND      0x83
#define NS_WASM_I64_OR       0x84
#define NS_WASM_I64_XOR      0x85
#define NS_WASM_I64_SHL      0x86
#define NS_WASM_I64_SHR_S    0x87
#define NS_WASM_I64_SHR_U    0x88

// f32 arithmetic
#define NS_WASM_F32_NEG      0x8C
#define NS_WASM_F32_ADD      0x92
#define NS_WASM_F32_SUB      0x93
#define NS_WASM_F32_MUL      0x94
#define NS_WASM_F32_DIV      0x95

// f64 arithmetic
#define NS_WASM_F64_NEG      0x9A
#define NS_WASM_F64_ADD      0xA0
#define NS_WASM_F64_SUB      0xA1
#define NS_WASM_F64_MUL      0xA2
#define NS_WASM_F64_DIV      0xA3

// Conversions
#define NS_WASM_I32_WRAP_I64       0xA7
#define NS_WASM_I64_EXTEND_I32_S   0xAC
#define NS_WASM_I64_EXTEND_I32_U   0xAD
#define NS_WASM_F32_CONVERT_I32_S  0xB2
#define NS_WASM_F32_CONVERT_I32_U  0xB3
#define NS_WASM_F32_CONVERT_I64_S  0xB4
#define NS_WASM_F32_CONVERT_I64_U  0xB5
#define NS_WASM_F32_DEMOTE_F64     0xB6
#define NS_WASM_F64_CONVERT_I32_S  0xB7
#define NS_WASM_F64_CONVERT_I32_U  0xB8
#define NS_WASM_F64_CONVERT_I64_S  0xB9
#define NS_WASM_F64_CONVERT_I64_U  0xBA
#define NS_WASM_F64_PROMOTE_F32    0xBB
#define NS_WASM_I32_TRUNC_F32_S    0xA8
#define NS_WASM_I32_TRUNC_F32_U    0xA9
#define NS_WASM_I32_TRUNC_F64_S    0xAA
#define NS_WASM_I32_TRUNC_F64_U    0xAB
#define NS_WASM_I64_TRUNC_F32_S    0xAE
#define NS_WASM_I64_TRUNC_F32_U    0xAF
#define NS_WASM_I64_TRUNC_F64_S    0xB0
#define NS_WASM_I64_TRUNC_F64_U    0xB1

// ──────────────────────────────────────────────────────────────────────────
// LEB128 / byte helpers
// ──────────────────────────────────────────────────────────────────────────

static void ns_wasm_u8(u8 **out, u8 v) {
    ns_array_push(*out, v);
}

static void ns_wasm_u32leb(u8 **out, u32 v) {
    do {
        u8 b = (u8)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        ns_array_push(*out, b);
    } while (v);
}

static void ns_wasm_i64leb(u8 **out, i64 v) {
    for (;;) {
        u8 b = (u8)(v & 0x7F);
        v >>= 7;
        ns_bool done = ((v == 0) && !(b & 0x40)) || ((v == -1) && (b & 0x40));
        if (!done) b |= 0x80;
        ns_array_push(*out, b);
        if (done) break;
    }
}

// SSA integer constants can contain the full u64 range. Parse through u64 so
// values above INT64_MAX keep their exact bit pattern when encoded as i64.
static i64 ns_wasm_integer_bits(ns_str s) {
    i32 i = 0;
    ns_bool negative = false;
    if (i < s.len && (s.data[i] == '-' || s.data[i] == '+')) {
        negative = s.data[i] == '-';
        i++;
    }
    u32 base = 10;
    if (i + 1 < s.len && s.data[i] == '0' && (s.data[i + 1] == 'x' || s.data[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    u64 value = 0;
    for (; i < s.len; i++) {
        i8 c = s.data[i];
        u32 digit;
        if (c >= '0' && c <= '9') digit = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (u32)(c - 'A' + 10);
        else break;
        if (digit >= base) break;
        value = value * base + digit;
    }
    if (negative) value = 0u - value;
    return (i64)value;
}

static void ns_wasm_f32bytes(u8 **out, f32 v) {
    u32 bits; memcpy(&bits, &v, 4);
    ns_array_push(*out, (u8)(bits));
    ns_array_push(*out, (u8)(bits >> 8));
    ns_array_push(*out, (u8)(bits >> 16));
    ns_array_push(*out, (u8)(bits >> 24));
}

static void ns_wasm_f64bytes(u8 **out, f64 v) {
    u64 bits; memcpy(&bits, &v, 8);
    for (i32 i = 0; i < 8; i++) ns_array_push(*out, (u8)(bits >> (i * 8)));
}

static void ns_wasm_u32bytes(u8 **out, u32 v) {
    for (i32 i = 0; i < 4; ++i) ns_array_push(*out, (u8)(v >> (i * 8)));
}

// Emit a section: id byte, u32leb size, then the content bytes
static void ns_wasm_section(u8 **out, u8 id, u8 *content) {
    ns_wasm_u8(out, id);
    u32 sz = (u32)ns_array_length(content);
    ns_wasm_u32leb(out, sz);
    for (u32 i = 0; i < sz; i++) ns_array_push(*out, content[i]);
}

// ──────────────────────────────────────────────────────────────────────────
// NS type → WASM valtype
// ──────────────────────────────────────────────────────────────────────────

static u8 ns_wasm_valtype(ns_value_type t) {
    switch (t) {
    case NS_TYPE_I8:  case NS_TYPE_U8:
    case NS_TYPE_I16: case NS_TYPE_U16:
    case NS_TYPE_I32: case NS_TYPE_U32:
    case NS_TYPE_BOOL:
        return NS_WASM_I32;
    case NS_TYPE_I64: case NS_TYPE_U64:
        return NS_WASM_I64;
    case NS_TYPE_F32:
        return NS_WASM_F32;
    case NS_TYPE_F64:
        return NS_WASM_F64;
    default:
        return NS_WASM_I32; // conservative default
    }
}

static u8 ns_wasm_type_valtype(ns_type t) {
    if (ns_type_is_array(t) || ns_type_is(t, NS_TYPE_STRUCT) || ns_type_is(t, NS_TYPE_STRING) ||
        ns_type_is(t, NS_TYPE_ANY) || ns_type_is_ref(t)) return NS_WASM_I32;
    return ns_wasm_valtype(t.type);
}

// ──────────────────────────────────────────────────────────────────────────
// Function context
// ──────────────────────────────────────────────────────────────────────────

// An entry on the label stack.  depth 0 = top (innermost open label).
typedef struct {
    i32      block_idx; // SSA block this label corresponds to
    ns_bool  is_loop;   // true → `loop` (br goes to start), false → `block` (br goes past end)
} ns_wasm_label;

// Per-function code-generation context
typedef struct {
    ns_ssa_fn     *fn;
    u8            *code;        // instruction bytes being built
    i32            n_params;    // number of function parameters
    i32            n_values;    // total SSA value slots (params + locals)
    i32            pc_local;    // dispatcher local used for structured CFG lowering
    u8            *vtypes;      // WASM valtype for each SSA value index
    u8            *unsigneds;   // integer signedness for each SSA value index
    ns_wasm_label *labels;      // label stack (index 0 = bottom, top = last element)
    ns_ssa_fn     *all_fns;     // all functions in the module (for CALL lookup)
    i32            n_fns;
    ns_ssa_import *imports;
    i32            n_imports;
} ns_wasm_fn_ctx;

// ──────────────────────────────────────────────────────────────────────────
// Type inference
// ──────────────────────────────────────────────────────────────────────────

// Infer whether a constant literal is float
static ns_bool ns_wasm_is_float_lit(ns_str s) {
    for (i32 i = 0; i < s.len; i++) {
        if (s.data[i] == '.' || s.data[i] == 'e' || s.data[i] == 'E') return true;
    }
    return false;
}

// Build vtypes array: one WASM type per SSA value index.
// Returns n_values (= max value index + 1).
static ns_bool ns_wasm_type_unsigned(ns_type t) {
    return ns_type_is(t, NS_TYPE_U8) || ns_type_is(t, NS_TYPE_U16) ||
           ns_type_is(t, NS_TYPE_U32) || ns_type_is(t, NS_TYPE_U64);
}

static i32 ns_wasm_infer_types(ns_ssa_fn *fn, u8 **vtypes_out, u8 **unsigneds_out) {
    // First pass: find max value index
    i32 max_val = 0;
    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; bi++) {
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ii++) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            if (inst->dst > max_val) max_val = inst->dst;
            if (inst->a   > max_val) max_val = inst->a;
            if (inst->b   > max_val) max_val = inst->b;
        }
    }
    i32 n = max_val + 1;

    u8 *vt = ns_null;
    ns_array_set_length(vt, n);
    memset(vt, NS_WASM_I32, (szt)n); // default: i32
    u8 *us = ns_null;
    ns_array_set_length(us, n);
    memset(us, 0, (szt)n);

    // First pass: set known types
    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; bi++) {
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ii++) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            if (inst->dst < 0 || inst->dst >= n) continue;

            switch (inst->op) {
            case NS_SSA_OP_PARAM:
                if (inst->type.type != NS_TYPE_UNKNOWN)
                    vt[inst->dst] = ns_wasm_type_valtype(inst->type);
                us[inst->dst] = ns_wasm_type_unsigned(inst->type);
                break;
            case NS_SSA_OP_CONST:
                if (inst->type.type != NS_TYPE_UNKNOWN) {
                    vt[inst->dst] = ns_wasm_type_valtype(inst->type);
                    us[inst->dst] = ns_wasm_type_unsigned(inst->type);
                } else if (inst->token.type == NS_TOKEN_FLT_LITERAL) {
                    vt[inst->dst] = inst->token.suffix == NS_NUM_SUFFIX_F64 ? NS_WASM_F64 : NS_WASM_F32;
                } else if (inst->token.type == NS_TOKEN_INT_LITERAL &&
                           (inst->token.suffix == NS_NUM_SUFFIX_I64 || inst->token.suffix == NS_NUM_SUFFIX_U64)) {
                    vt[inst->dst] = NS_WASM_I64;
                } else if (ns_wasm_is_float_lit(inst->name)) {
                    vt[inst->dst] = NS_WASM_F32;
                }
                // else keep i32
                break;
            case NS_SSA_OP_CAST:
            case NS_SSA_OP_CALL:
                if (inst->type.type != NS_TYPE_UNKNOWN)
                    vt[inst->dst] = ns_wasm_type_valtype(inst->type);
                us[inst->dst] = ns_wasm_type_unsigned(inst->type);
                break;
            default:
                break;
            }
        }
    }

    // Second pass: propagate types through operations
    for (i32 pass = 0; pass < 3; pass++) {
        for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; bi++) {
            ns_ssa_block *bb = &fn->blocks[bi];
            for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ii++) {
                ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
                if (inst->dst < 0 || inst->dst >= n) continue;
                if (inst->op == NS_SSA_OP_PARAM || inst->op == NS_SSA_OP_CONST) continue;

                if (inst->type.type != NS_TYPE_UNKNOWN) {
                    vt[inst->dst] = ns_wasm_type_valtype(inst->type);
                    us[inst->dst] = ns_wasm_type_unsigned(inst->type);
                    continue;
                }

                // Comparison results are always i32 (bool)
                switch (inst->op) {
                case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
                case NS_SSA_OP_LT: case NS_SSA_OP_LE:
                case NS_SSA_OP_GT: case NS_SSA_OP_GE:
                case NS_SSA_OP_AND: case NS_SSA_OP_OR:
                case NS_SSA_OP_NOT:
                    vt[inst->dst] = NS_WASM_I32;
                    continue;
                default:
                    break;
                }

                // Propagate from first operand
                if (inst->a >= 0 && inst->a < n) {
                    vt[inst->dst] = vt[inst->a];
                    us[inst->dst] = us[inst->a];
                }
            }
        }
    }

    *vtypes_out = vt;
    *unsigneds_out = us;
    return n;
}

// ──────────────────────────────────────────────────────────────────────────
// Label stack helpers
// ──────────────────────────────────────────────────────────────────────────

static void ns_wasm_push_label(ns_wasm_fn_ctx *ctx, i32 block_idx, ns_bool is_loop) {
    ns_wasm_label l = {block_idx, is_loop};
    ns_array_push(ctx->labels, l);
}

static void ns_wasm_pop_label(ns_wasm_fn_ctx *ctx) {
    i32 len = (i32)ns_array_length(ctx->labels);
    if (len > 0) ns_array_splice(ctx->labels, len - 1);
}

// depth 0 = top of stack (last element = innermost)
static i32 ns_wasm_br_depth(ns_wasm_fn_ctx *ctx, i32 block_idx) {
    i32 len = (i32)ns_array_length(ctx->labels);
    for (i32 d = 0; d < len; d++) {
        if (ctx->labels[len - 1 - d].block_idx == block_idx) return d;
    }
    return -1;
}

// ──────────────────────────────────────────────────────────────────────────
// Emit helpers
// ──────────────────────────────────────────────────────────────────────────

static void ns_wasm_local_get(ns_wasm_fn_ctx *ctx, i32 idx) {
    ns_wasm_u8(&ctx->code, NS_WASM_LOCAL_GET);
    ns_wasm_u32leb(&ctx->code, (u32)idx);
}

static void ns_wasm_local_set(ns_wasm_fn_ctx *ctx, i32 idx) {
    ns_wasm_u8(&ctx->code, NS_WASM_LOCAL_SET);
    ns_wasm_u32leb(&ctx->code, (u32)idx);
}

static void ns_wasm_br(ns_wasm_fn_ctx *ctx, u32 depth) {
    ns_wasm_u8(&ctx->code, NS_WASM_BR);
    ns_wasm_u32leb(&ctx->code, depth);
}

static void ns_wasm_br_if(ns_wasm_fn_ctx *ctx, u32 depth) {
    ns_wasm_u8(&ctx->code, NS_WASM_BR_IF);
    ns_wasm_u32leb(&ctx->code, depth);
}

// Emit branch to block_idx.  If it equals fallthrough_next, emit nothing.
static void ns_wasm_jump_to(ns_wasm_fn_ctx *ctx, i32 block_idx, i32 fallthrough_next) {
    if (block_idx == fallthrough_next) return; // natural fall-through
    i32 depth = ns_wasm_br_depth(ctx, block_idx);
    if (depth < 0) {
        ns_warn("wasm", "fn %.*s: no label for block %d, emitting unreachable\n",
                ctx->fn->name.len, ctx->fn->name.data, block_idx);
        ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
        return;
    }
    ns_wasm_br(ctx, (u32)depth);
}

// ──────────────────────────────────────────────────────────────────────────
// Single-instruction emission
// ──────────────────────────────────────────────────────────────────────────

// Map (SSA op, wasm type) → WASM arithmetic/comparison opcode
static u8 ns_wasm_arith_op(ns_ssa_op op, u8 wtype, ns_bool is_unsigned) {
    switch (op) {
    case NS_SSA_OP_ADD:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_ADD :
               wtype == NS_WASM_F32 ? NS_WASM_F32_ADD :
               wtype == NS_WASM_F64 ? NS_WASM_F64_ADD : NS_WASM_I32_ADD;
    case NS_SSA_OP_SUB:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_SUB :
               wtype == NS_WASM_F32 ? NS_WASM_F32_SUB :
               wtype == NS_WASM_F64 ? NS_WASM_F64_SUB : NS_WASM_I32_SUB;
    case NS_SSA_OP_MUL:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_MUL :
               wtype == NS_WASM_F32 ? NS_WASM_F32_MUL :
               wtype == NS_WASM_F64 ? NS_WASM_F64_MUL : NS_WASM_I32_MUL;
    case NS_SSA_OP_DIV:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_DIV_U : NS_WASM_I64_DIV_S) :
               wtype == NS_WASM_F32 ? NS_WASM_F32_DIV :
               wtype == NS_WASM_F64 ? NS_WASM_F64_DIV : (is_unsigned ? NS_WASM_I32_DIV_U : NS_WASM_I32_DIV_S);
    case NS_SSA_OP_MOD:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_REM_U : NS_WASM_I64_REM_S) :
               (is_unsigned ? NS_WASM_I32_REM_U : NS_WASM_I32_REM_S);
    case NS_SSA_OP_SHL:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_SHL : NS_WASM_I32_SHL;
    case NS_SSA_OP_SHR:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_SHR_U : NS_WASM_I64_SHR_S) :
               (is_unsigned ? NS_WASM_I32_SHR_U : NS_WASM_I32_SHR_S);
    case NS_SSA_OP_BAND:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_AND : NS_WASM_I32_AND;
    case NS_SSA_OP_BOR:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_OR : NS_WASM_I32_OR;
    case NS_SSA_OP_BXOR:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_XOR : NS_WASM_I32_XOR;
    // Logical AND/OR treated as integer AND/OR (booleans are i32)
    case NS_SSA_OP_AND: return NS_WASM_I32_AND;
    case NS_SSA_OP_OR:  return NS_WASM_I32_OR;
    // Comparisons (operand type drives the opcode; result is always i32)
    case NS_SSA_OP_EQ:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_EQ :
               wtype == NS_WASM_F32 ? NS_WASM_F32_EQ :
               wtype == NS_WASM_F64 ? NS_WASM_F64_EQ : NS_WASM_I32_EQ;
    case NS_SSA_OP_NE:
        return wtype == NS_WASM_I64 ? NS_WASM_I64_NE :
               wtype == NS_WASM_F32 ? NS_WASM_F32_NE :
               wtype == NS_WASM_F64 ? NS_WASM_F64_NE : NS_WASM_I32_NE;
    case NS_SSA_OP_LT:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_LT_U : NS_WASM_I64_LT_S) :
               wtype == NS_WASM_F32 ? NS_WASM_F32_LT :
               wtype == NS_WASM_F64 ? NS_WASM_F64_LT : (is_unsigned ? NS_WASM_I32_LT_U : NS_WASM_I32_LT_S);
    case NS_SSA_OP_LE:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_LE_U : NS_WASM_I64_LE_S) :
               wtype == NS_WASM_F32 ? NS_WASM_F32_LE :
               wtype == NS_WASM_F64 ? NS_WASM_F64_LE : (is_unsigned ? NS_WASM_I32_LE_U : NS_WASM_I32_LE_S);
    case NS_SSA_OP_GT:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_GT_U : NS_WASM_I64_GT_S) :
               wtype == NS_WASM_F32 ? NS_WASM_F32_GT :
               wtype == NS_WASM_F64 ? NS_WASM_F64_GT : (is_unsigned ? NS_WASM_I32_GT_U : NS_WASM_I32_GT_S);
    case NS_SSA_OP_GE:
        return wtype == NS_WASM_I64 ? (is_unsigned ? NS_WASM_I64_GE_U : NS_WASM_I64_GE_S) :
               wtype == NS_WASM_F32 ? NS_WASM_F32_GE :
               wtype == NS_WASM_F64 ? NS_WASM_F64_GE : (is_unsigned ? NS_WASM_I32_GE_U : NS_WASM_I32_GE_S);
    default:
        return NS_WASM_NOP;
    }
}

// Find the function index for a given name
static i32 ns_wasm_fn_index(ns_wasm_fn_ctx *ctx, ns_str name) {
    for (i32 i = 0; i < ctx->n_fns; i++) {
        if (ns_str_equals(ctx->all_fns[i].name, name)) return ctx->n_imports + i;
    }
    return -1;
}

static i32 ns_wasm_import_index(ns_wasm_fn_ctx *ctx, ns_str module, ns_str name) {
    for (i32 i = 0; i < ctx->n_imports; ++i) {
        if (ns_str_equals(ctx->imports[i].module, module) &&
            ns_str_equals(ctx->imports[i].name, name)) return i;
    }
    return -1;
}

// Find the PARAM instruction that defines SSA value `val` in function `fn`
static ns_ssa_inst *ns_wasm_find_param_def(ns_ssa_fn *fn, i32 val) {
    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; bi++) {
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ii++) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            if (inst->dst == val && inst->op == NS_SSA_OP_PARAM) return inst;
        }
    }
    return ns_null;
}

static void ns_wasm_memarg(ns_wasm_fn_ctx *ctx, u32 offset) {
    ns_wasm_u32leb(&ctx->code, 0); // conservative byte alignment is always valid
    ns_wasm_u32leb(&ctx->code, offset);
}

static u8 ns_wasm_load_op(ns_type type) {
    if (ns_type_is(type, NS_TYPE_I8)) return NS_WASM_I32_LOAD8_S;
    if (ns_type_is(type, NS_TYPE_U8)) return NS_WASM_I32_LOAD8_U;
    if (ns_type_is(type, NS_TYPE_I16)) return NS_WASM_I32_LOAD16_S;
    if (ns_type_is(type, NS_TYPE_U16)) return NS_WASM_I32_LOAD16_U;
    u8 valtype = ns_wasm_type_valtype(type);
    return valtype == NS_WASM_I64 ? NS_WASM_I64_LOAD :
           valtype == NS_WASM_F32 ? NS_WASM_F32_LOAD :
           valtype == NS_WASM_F64 ? NS_WASM_F64_LOAD : NS_WASM_I32_LOAD;
}

static u8 ns_wasm_store_op(ns_type type) {
    if (ns_type_is(type, NS_TYPE_I8) || ns_type_is(type, NS_TYPE_U8)) return NS_WASM_I32_STORE8;
    if (ns_type_is(type, NS_TYPE_I16) || ns_type_is(type, NS_TYPE_U16)) return NS_WASM_I32_STORE16;
    u8 valtype = ns_wasm_type_valtype(type);
    return valtype == NS_WASM_I64 ? NS_WASM_I64_STORE :
           valtype == NS_WASM_F32 ? NS_WASM_F32_STORE :
           valtype == NS_WASM_F64 ? NS_WASM_F64_STORE : NS_WASM_I32_STORE;
}

static void ns_wasm_bounds_check(ns_wasm_fn_ctx *ctx, i32 descriptor, i32 index) {
    ns_wasm_local_get(ctx, index);
    ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, 0);
    ns_wasm_u8(&ctx->code, NS_WASM_I32_LT_S);
    ns_wasm_u8(&ctx->code, NS_WASM_IF); ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
    ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
    ns_wasm_u8(&ctx->code, NS_WASM_END);
    ns_wasm_local_get(ctx, index);
    ns_wasm_local_get(ctx, descriptor);
    ns_wasm_u8(&ctx->code, NS_WASM_I32_LOAD); ns_wasm_memarg(ctx, 4);
    ns_wasm_u8(&ctx->code, NS_WASM_I32_GE_U);
    ns_wasm_u8(&ctx->code, NS_WASM_IF); ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
    ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
    ns_wasm_u8(&ctx->code, NS_WASM_END);
}

// Emit one non-terminator instruction
static void ns_wasm_emit_inst(ns_wasm_fn_ctx *ctx, ns_ssa_inst *inst) {
    switch (inst->op) {

    case NS_SSA_OP_PARAM:
        // Parameters map directly to WASM locals 0..n_params-1.
        // SSA allocates them as values 0, 1, ... so local[dst] == param slot.
        // No instruction needed; WASM already has them.
        break;

    case NS_SSA_OP_UNDEF:
        if (inst->dst >= 0) {
            // Emit a zero constant of the right type
            u8 vt = (inst->dst < ctx->n_values) ? ctx->vtypes[inst->dst] : NS_WASM_I32;
            switch (vt) {
            case NS_WASM_I64:
                ns_wasm_u8(&ctx->code, NS_WASM_I64_CONST);
                ns_wasm_i64leb(&ctx->code, 0); break;
            case NS_WASM_F32:
                ns_wasm_u8(&ctx->code, NS_WASM_F32_CONST);
                ns_wasm_f32bytes(&ctx->code, 0.0f); break;
            case NS_WASM_F64:
                ns_wasm_u8(&ctx->code, NS_WASM_F64_CONST);
                ns_wasm_f64bytes(&ctx->code, 0.0); break;
            default:
                ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
                ns_wasm_i64leb(&ctx->code, 0); break;
            }
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_CONST: {
        if (inst->dst < 0) break;
        u8 vt = (inst->dst < ctx->n_values) ? ctx->vtypes[inst->dst] : NS_WASM_I32;
        if (ns_type_is(inst->type, NS_TYPE_STRING)) {
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, inst->c);
        } else switch (vt) {
        case NS_WASM_I64:
            ns_wasm_u8(&ctx->code, NS_WASM_I64_CONST);
            ns_wasm_i64leb(&ctx->code, ns_wasm_integer_bits(inst->name));
            break;
        case NS_WASM_F32: {
            f32 fv = (f32)ns_str_to_f64(inst->name);
            ns_wasm_u8(&ctx->code, NS_WASM_F32_CONST);
            ns_wasm_f32bytes(&ctx->code, fv);
        } break;
        case NS_WASM_F64: {
            f64 dv = ns_str_to_f64(inst->name);
            ns_wasm_u8(&ctx->code, NS_WASM_F64_CONST);
            ns_wasm_f64bytes(&ctx->code, dv);
        } break;
        default:
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            if (inst->token.type == NS_TOKEN_TRUE) ns_wasm_i64leb(&ctx->code, 1);
            else if (inst->token.type == NS_TOKEN_FALSE || inst->token.type == NS_TOKEN_NIL) ns_wasm_i64leb(&ctx->code, 0);
            else ns_wasm_i64leb(&ctx->code, (i64)ns_str_to_i32(inst->name));
            break;
        }
        ns_wasm_local_set(ctx, inst->dst);
    } break;

    case NS_SSA_OP_COPY:
        if (inst->dst >= 0 && inst->a >= 0 && inst->dst != inst->a) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_PHI:
        // Materialized on predecessor edges by ns_wasm_emit_phi_moves.
        break;

    case NS_SSA_OP_ARG:
        // Push argument value onto stack for an upcoming CALL
        if (inst->a >= 0) ns_wasm_local_get(ctx, inst->a);
        break;

    case NS_SSA_OP_GLOBAL_GET:
        if (inst->dst >= 0 && inst->c >= 0) {
            ns_wasm_u8(&ctx->code, NS_WASM_GLOBAL_GET);
            ns_wasm_u32leb(&ctx->code, (u32)inst->c + 1u);
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_GLOBAL_SET:
        if (inst->a >= 0 && inst->c >= 0) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_GLOBAL_SET);
            ns_wasm_u32leb(&ctx->code, (u32)inst->c + 1u);
        }
        break;

    case NS_SSA_OP_ALLOC:
        if (inst->dst >= 0 && inst->c >= 0) {
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, inst->c);
            ns_wasm_u8(&ctx->code, NS_WASM_CALL);
            ns_wasm_u32leb(&ctx->code, (u32)(ctx->n_imports + ctx->n_fns));
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_CLONE:
        if (inst->dst >= 0 && inst->a >= 0 && inst->c > 0) {
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, inst->c);
            ns_wasm_u8(&ctx->code, NS_WASM_CALL);
            ns_wasm_u32leb(&ctx->code, (u32)(ctx->n_imports + ctx->n_fns));
            ns_wasm_local_set(ctx, inst->dst);
            ns_wasm_local_get(ctx, inst->dst);
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, inst->c);
            ns_wasm_u8(&ctx->code, NS_WASM_MISC_PREFIX);
            ns_wasm_u32leb(&ctx->code, NS_WASM_MEMORY_COPY);
            ns_wasm_u32leb(&ctx->code, 0);
            ns_wasm_u32leb(&ctx->code, 0);
        }
        break;

    case NS_SSA_OP_LOAD:
        if (inst->dst >= 0 && inst->a >= 0 && inst->c >= 0) {
            ns_wasm_local_get(ctx, inst->a);
            if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_ref(inst->type)) {
                ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
                ns_wasm_i64leb(&ctx->code, inst->c);
                ns_wasm_u8(&ctx->code, NS_WASM_I32_ADD);
            } else {
                ns_wasm_u8(&ctx->code, ns_wasm_load_op(inst->type));
                ns_wasm_memarg(ctx, (u32)inst->c);
            }
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_STORE:
        if (inst->a >= 0 && inst->b >= 0 && inst->c >= 0) {
            ns_wasm_local_get(ctx, inst->a);
            if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_ref(inst->type)) {
                ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
                ns_wasm_i64leb(&ctx->code, inst->c);
                ns_wasm_u8(&ctx->code, NS_WASM_I32_ADD);
                ns_wasm_local_get(ctx, inst->b);
                ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
                ns_wasm_i64leb(&ctx->code, inst->target0);
                ns_wasm_u8(&ctx->code, NS_WASM_MISC_PREFIX);
                ns_wasm_u32leb(&ctx->code, NS_WASM_MEMORY_COPY);
                ns_wasm_u32leb(&ctx->code, 0);
                ns_wasm_u32leb(&ctx->code, 0);
            } else {
                ns_wasm_local_get(ctx, inst->b);
                ns_wasm_u8(&ctx->code, ns_wasm_store_op(inst->type));
                ns_wasm_memarg(ctx, (u32)inst->c);
            }
        }
        break;

    case NS_SSA_OP_ARRAY_NEW:
        if (inst->dst >= 0 && inst->a >= 0 && inst->c > 0) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, 0);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_LT_S);
            ns_wasm_u8(&ctx->code, NS_WASM_IF); ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
            ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
            ns_wasm_u8(&ctx->code, NS_WASM_END);
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, inst->c);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_MUL);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, 16);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_ADD);
            ns_wasm_u8(&ctx->code, NS_WASM_CALL);
            ns_wasm_u32leb(&ctx->code, (u32)(ctx->n_imports + ctx->n_fns));
            ns_wasm_local_set(ctx, inst->dst);
            ns_wasm_local_get(ctx, inst->dst);
            ns_wasm_local_get(ctx, inst->dst);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, 16);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_ADD);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_STORE); ns_wasm_memarg(ctx, 0);
            for (u32 offset = 4; offset <= 8; offset += 4) {
                ns_wasm_local_get(ctx, inst->dst);
                ns_wasm_local_get(ctx, inst->a);
                ns_wasm_u8(&ctx->code, NS_WASM_I32_STORE); ns_wasm_memarg(ctx, offset);
            }
        }
        break;

    case NS_SSA_OP_ARRAY_STORE:
        if (inst->a >= 0 && inst->b >= 0 && inst->target0 >= 0 && inst->c > 0) {
            ns_wasm_bounds_check(ctx, inst->a, inst->target0);
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_LOAD); ns_wasm_memarg(ctx, 0);
            ns_wasm_local_get(ctx, inst->target0);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, inst->c);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_MUL);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_ADD);
            ns_wasm_local_get(ctx, inst->b);
            if (ns_type_is(inst->type, NS_TYPE_STRUCT)) {
                ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, inst->c);
                ns_wasm_u8(&ctx->code, NS_WASM_MISC_PREFIX); ns_wasm_u32leb(&ctx->code, NS_WASM_MEMORY_COPY);
                ns_wasm_u32leb(&ctx->code, 0); ns_wasm_u32leb(&ctx->code, 0);
            } else {
                ns_wasm_u8(&ctx->code, ns_wasm_store_op(inst->type)); ns_wasm_memarg(ctx, 0);
            }
        }
        break;

    case NS_SSA_OP_CALL: {
        ns_str callee_name = inst->name;
        if (ns_str_is_empty(callee_name) && inst->a >= 0) {
            ns_ssa_inst *def = ns_wasm_find_param_def(ctx->fn, inst->a);
            if (def) callee_name = def->name;
        }
        i32 fi = ns_str_is_empty(inst->module)
            ? ns_wasm_fn_index(ctx, callee_name)
            : ns_wasm_import_index(ctx, inst->module, callee_name);
        if (fi < 0) {
            break;
        }
        ns_wasm_u8(&ctx->code, NS_WASM_CALL);
        ns_wasm_u32leb(&ctx->code, (u32)fi);
        if (inst->dst >= 0 && !ns_type_is(inst->type, NS_TYPE_VOID)) {
            ns_wasm_local_set(ctx, inst->dst);
        }
        // If dst < 0 and callee returns non-void, caller must drop the result.
        // We assume: dst < 0 ⇒ callee is void (conservative; may need DROP).
    } break;

    case NS_SSA_OP_INDEX:
        if (inst->dst >= 0 && inst->a >= 0 && inst->b >= 0 && inst->c > 0) {
            ns_wasm_bounds_check(ctx, inst->a, inst->b);
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_LOAD); ns_wasm_memarg(ctx, 0);
            ns_wasm_local_get(ctx, inst->b);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST); ns_wasm_i64leb(&ctx->code, inst->c);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_MUL);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_ADD);
            if (!ns_type_is(inst->type, NS_TYPE_STRUCT)) {
                ns_type load_type = inst->type;
                if (inst->c == 1 && ns_type_is(inst->type, NS_TYPE_I32)) load_type = ns_type_u8;
                ns_wasm_u8(&ctx->code, ns_wasm_load_op(load_type)); ns_wasm_memarg(ctx, 0);
            }
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_NEG: {
        if (inst->a < 0 || inst->dst < 0) break;
        u8 vt = (inst->a < ctx->n_values) ? ctx->vtypes[inst->a] : NS_WASM_I32;
        if (vt == NS_WASM_F32) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_F32_NEG);
        } else if (vt == NS_WASM_F64) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_F64_NEG);
        } else if (vt == NS_WASM_I64) {
            ns_wasm_u8(&ctx->code, NS_WASM_I64_CONST);
            ns_wasm_i64leb(&ctx->code, 0);
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I64_SUB);
        } else {
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, 0);
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_SUB);
        }
        ns_wasm_local_set(ctx, inst->dst);
    } break;

    case NS_SSA_OP_NOT:
        if (inst->a >= 0 && inst->dst >= 0) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_EQZ);
            ns_wasm_local_set(ctx, inst->dst);
        }
        break;

    case NS_SSA_OP_CAST: {
        if (inst->a < 0 || inst->dst < 0) break;
        u8 src_vt = (inst->a < ctx->n_values) ? ctx->vtypes[inst->a] : NS_WASM_I32;
        u8 dst_vt = (inst->dst < ctx->n_values) ? ctx->vtypes[inst->dst] : NS_WASM_I32;
        ns_bool src_unsigned = inst->a < ctx->n_values && ctx->unsigneds[inst->a];
        ns_bool dst_unsigned = inst->dst < ctx->n_values && ctx->unsigneds[inst->dst];
        ns_wasm_local_get(ctx, inst->a);
        if (src_vt == dst_vt) {
            // No conversion needed
        } else if (src_vt == NS_WASM_I32 && dst_vt == NS_WASM_I64) {
            ns_wasm_u8(&ctx->code, src_unsigned ? NS_WASM_I64_EXTEND_I32_U : NS_WASM_I64_EXTEND_I32_S);
        } else if (src_vt == NS_WASM_I64 && dst_vt == NS_WASM_I32) {
            ns_wasm_u8(&ctx->code, NS_WASM_I32_WRAP_I64);
        } else if (src_vt == NS_WASM_I32 && dst_vt == NS_WASM_F32) {
            ns_wasm_u8(&ctx->code, src_unsigned ? NS_WASM_F32_CONVERT_I32_U : NS_WASM_F32_CONVERT_I32_S);
        } else if (src_vt == NS_WASM_I32 && dst_vt == NS_WASM_F64) {
            ns_wasm_u8(&ctx->code, src_unsigned ? NS_WASM_F64_CONVERT_I32_U : NS_WASM_F64_CONVERT_I32_S);
        } else if (src_vt == NS_WASM_I64 && dst_vt == NS_WASM_F32) {
            ns_wasm_u8(&ctx->code, src_unsigned ? NS_WASM_F32_CONVERT_I64_U : NS_WASM_F32_CONVERT_I64_S);
        } else if (src_vt == NS_WASM_I64 && dst_vt == NS_WASM_F64) {
            ns_wasm_u8(&ctx->code, src_unsigned ? NS_WASM_F64_CONVERT_I64_U : NS_WASM_F64_CONVERT_I64_S);
        } else if (src_vt == NS_WASM_F32 && dst_vt == NS_WASM_F64) {
            ns_wasm_u8(&ctx->code, NS_WASM_F64_PROMOTE_F32);
        } else if (src_vt == NS_WASM_F64 && dst_vt == NS_WASM_F32) {
            ns_wasm_u8(&ctx->code, NS_WASM_F32_DEMOTE_F64);
        } else if (src_vt == NS_WASM_F32 && dst_vt == NS_WASM_I32) {
            ns_wasm_u8(&ctx->code, dst_unsigned ? NS_WASM_I32_TRUNC_F32_U : NS_WASM_I32_TRUNC_F32_S);
        } else if (src_vt == NS_WASM_F64 && dst_vt == NS_WASM_I32) {
            ns_wasm_u8(&ctx->code, dst_unsigned ? NS_WASM_I32_TRUNC_F64_U : NS_WASM_I32_TRUNC_F64_S);
        } else if (src_vt == NS_WASM_F32 && dst_vt == NS_WASM_I64) {
            ns_wasm_u8(&ctx->code, dst_unsigned ? NS_WASM_I64_TRUNC_F32_U : NS_WASM_I64_TRUNC_F32_S);
        } else if (src_vt == NS_WASM_F64 && dst_vt == NS_WASM_I64) {
            ns_wasm_u8(&ctx->code, dst_unsigned ? NS_WASM_I64_TRUNC_F64_U : NS_WASM_I64_TRUNC_F64_S);
        }
        // else: unsupported cast, no conversion
        ns_wasm_local_set(ctx, inst->dst);
    } break;

    case NS_SSA_OP_ADD: case NS_SSA_OP_SUB: case NS_SSA_OP_MUL:
    case NS_SSA_OP_DIV: case NS_SSA_OP_MOD:
    case NS_SSA_OP_SHL: case NS_SSA_OP_SHR:
    case NS_SSA_OP_BAND: case NS_SSA_OP_BOR: case NS_SSA_OP_BXOR:
    case NS_SSA_OP_AND: case NS_SSA_OP_OR:
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
    case NS_SSA_OP_LT: case NS_SSA_OP_LE:
    case NS_SSA_OP_GT: case NS_SSA_OP_GE: {
        if (inst->a < 0 || inst->b < 0 || inst->dst < 0) break;
        u8 op_vt = (inst->a < ctx->n_values) ? ctx->vtypes[inst->a] : NS_WASM_I32;
        ns_wasm_local_get(ctx, inst->a);
        ns_wasm_local_get(ctx, inst->b);
        ns_bool is_unsigned = inst->a < ctx->n_values && ctx->unsigneds[inst->a];
        u8 opcode = ns_wasm_arith_op(inst->op, op_vt, is_unsigned);
        ns_wasm_u8(&ctx->code, opcode);
        ns_wasm_local_set(ctx, inst->dst);
    } break;

    case NS_SSA_OP_ASSERT:
        // assert: if value is zero, trap
        if (inst->a >= 0) {
            ns_wasm_local_get(ctx, inst->a);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_EQZ);
            ns_wasm_u8(&ctx->code, NS_WASM_IF);
            ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
            ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
            ns_wasm_u8(&ctx->code, NS_WASM_END);
        }
        break;

    case NS_SSA_OP_TRAP:
        ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
        break;

    default:
        break;
    }
}

static void ns_wasm_emit_phi_moves(ns_wasm_fn_ctx *ctx, i32 predecessor, i32 target) {
    if (target < 0 || target >= (i32)ns_array_length(ctx->fn->blocks)) return;
    ns_ssa_block *bb = &ctx->fn->blocks[target];
    for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ++ii) {
        ns_ssa_inst *phi = &ctx->fn->insts[bb->insts[ii]];
        if (phi->op != NS_SSA_OP_PHI) continue;
        i32 source = phi->target0 == predecessor ? phi->a :
                     phi->target1 == predecessor ? phi->b : -1;
        if (source >= 0 && source != phi->dst) {
            ns_wasm_local_get(ctx, source);
            ns_wasm_local_set(ctx, phi->dst);
        }
    }
}

// Lower arbitrary reducible SSA control flow through a compact pc dispatcher.
// This is intentionally less clever than shape-specific block nesting, but it
// gives every CFG edge a concrete place to materialize PHIs and remains valid
// for nested conditionals, loops, break, and continue.
static void ns_wasm_emit_dispatch(ns_wasm_fn_ctx *ctx) {
    ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
    ns_wasm_i64leb(&ctx->code, ctx->fn->entry);
    ns_wasm_local_set(ctx, ctx->pc_local);

    ns_wasm_u8(&ctx->code, NS_WASM_LOOP);
    ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);

    for (i32 bi = 0, bl = (i32)ns_array_length(ctx->fn->blocks); bi < bl; ++bi) {
        ns_ssa_block *bb = &ctx->fn->blocks[bi];
        ns_wasm_local_get(ctx, ctx->pc_local);
        ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
        ns_wasm_i64leb(&ctx->code, bi);
        ns_wasm_u8(&ctx->code, NS_WASM_I32_EQ);
        ns_wasm_u8(&ctx->code, NS_WASM_IF);
        ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);

        i32 count = (i32)ns_array_length(bb->insts);
        ns_ssa_inst *term = count > 0 ? &ctx->fn->insts[bb->insts[count - 1]] : ns_null;
        i32 body_count = term && (term->op == NS_SSA_OP_RET || term->op == NS_SSA_OP_JMP ||
                                  term->op == NS_SSA_OP_BR || term->op == NS_SSA_OP_TRAP)
            ? count - 1 : count;
        for (i32 ii = 0; ii < body_count; ++ii) {
            ns_wasm_emit_inst(ctx, &ctx->fn->insts[bb->insts[ii]]);
        }

        if (!term || body_count == count) {
            ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
        } else if (term->op == NS_SSA_OP_RET) {
            if (term->a >= 0) ns_wasm_local_get(ctx, term->a);
            ns_wasm_u8(&ctx->code, NS_WASM_RETURN);
        } else if (term->op == NS_SSA_OP_TRAP) {
            ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
        } else if (term->op == NS_SSA_OP_JMP) {
            ns_wasm_emit_phi_moves(ctx, bi, term->target0);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, term->target0);
            ns_wasm_local_set(ctx, ctx->pc_local);
        } else {
            ns_wasm_local_get(ctx, term->a);
            ns_wasm_u8(&ctx->code, NS_WASM_IF);
            ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
            ns_wasm_emit_phi_moves(ctx, bi, term->target0);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, term->target0);
            ns_wasm_local_set(ctx, ctx->pc_local);
            ns_wasm_u8(&ctx->code, NS_WASM_ELSE);
            ns_wasm_emit_phi_moves(ctx, bi, term->target1);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_CONST);
            ns_wasm_i64leb(&ctx->code, term->target1);
            ns_wasm_local_set(ctx, ctx->pc_local);
            ns_wasm_u8(&ctx->code, NS_WASM_END);
        }

        ns_wasm_u8(&ctx->code, NS_WASM_BR);
        ns_wasm_u32leb(&ctx->code, 1); // leave the block test and continue the dispatcher loop
        ns_wasm_u8(&ctx->code, NS_WASM_END);
    }

    ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
    ns_wasm_u8(&ctx->code, NS_WASM_END);
    // A loop with a void block type does not itself prove a function result;
    // make the impossible fallthrough explicit for non-void functions.
    ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
}

// ──────────────────────────────────────────────────────────────────────────
// Control-flow emission
// ──────────────────────────────────────────────────────────────────────────

// Forward declarations
static void ns_wasm_emit_range(ns_wasm_fn_ctx *ctx, i32 start, i32 end,
                               i32 *loop_heads, i32 *loop_ends, i32 n_loops,
                               i32 fallthrough_after);

// Emit all non-terminator instructions for block block_idx, then the terminator.
// fallthrough_next: the block index that immediately follows in the emission order
// (-1 if nothing follows; caller handles the end-of-function).
static void ns_wasm_emit_block(ns_wasm_fn_ctx *ctx, i32 block_idx, i32 fallthrough_next) {
    ns_ssa_block *bb = &ctx->fn->blocks[block_idx];
    i32 n_insts = (i32)ns_array_length(bb->insts);

    // Emit non-terminator instructions
    i32 term_idx = n_insts - 1;
    for (i32 ii = 0; ii < term_idx; ii++) {
        ns_ssa_inst *inst = &ctx->fn->insts[bb->insts[ii]];
        ns_wasm_emit_inst(ctx, inst);
    }

    if (term_idx < 0) return; // empty block

    ns_ssa_inst *term = &ctx->fn->insts[bb->insts[term_idx]];

    switch (term->op) {
    case NS_SSA_OP_RET:
        if (term->a >= 0) {
            ns_wasm_local_get(ctx, term->a);
        }
        ns_wasm_u8(&ctx->code, NS_WASM_RETURN);
        break;

    case NS_SSA_OP_JMP:
        ns_wasm_jump_to(ctx, term->target0, fallthrough_next);
        break;

    case NS_SSA_OP_BR: {
        // BR cond → target0 (true), target1 (false)
        i32 t0 = term->target0;
        i32 t1 = term->target1;
        i32 cond = term->a;

        if (t0 == fallthrough_next) {
            // If true: fall through to t0.  If false: jump to t1.
            // Condition is already non-zero for true → negate and br_if
            ns_wasm_local_get(ctx, cond);
            ns_wasm_u8(&ctx->code, NS_WASM_I32_EQZ); // negate
            i32 d1 = ns_wasm_br_depth(ctx, t1);
            if (d1 >= 0) {
                ns_wasm_br_if(ctx, (u32)d1);
            } else {
                // t1 not in label stack, just emit unreachable path
                ns_wasm_u8(&ctx->code, NS_WASM_IF);
                ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
                ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
                ns_wasm_u8(&ctx->code, NS_WASM_END);
            }
            // Fall through to t0
        } else if (t1 == fallthrough_next) {
            // If true: jump to t0.  If false: fall through to t1.
            ns_wasm_local_get(ctx, cond);
            i32 d0 = ns_wasm_br_depth(ctx, t0);
            if (d0 >= 0) {
                ns_wasm_br_if(ctx, (u32)d0);
            } else {
                ns_wasm_u8(&ctx->code, NS_WASM_IF);
                ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
                ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
                ns_wasm_u8(&ctx->code, NS_WASM_END);
            }
            // Fall through to t1
        } else {
            // Neither target falls through.  Emit if/else/end.
            i32 d0 = ns_wasm_br_depth(ctx, t0);
            i32 d1 = ns_wasm_br_depth(ctx, t1);
            ns_wasm_local_get(ctx, cond);
            ns_wasm_u8(&ctx->code, NS_WASM_IF);
            ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
            if (d0 >= 0) ns_wasm_br(ctx, (u32)(d0 + 1)); // +1 for the if block
            else ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
            ns_wasm_u8(&ctx->code, NS_WASM_ELSE);
            if (d1 >= 0) ns_wasm_br(ctx, (u32)(d1 + 1));
            else ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
            ns_wasm_u8(&ctx->code, NS_WASM_END);
        }
    } break;

    case NS_SSA_OP_TRAP:
        ns_wasm_u8(&ctx->code, NS_WASM_UNREACHABLE);
        break;

    default:
        // Non-terminator treated as last instruction (shouldn't happen)
        ns_wasm_emit_inst(ctx, term);
        break;
    }
}

// Emit blocks [start..end] as a DAG (no back edges within this range).
// The label stack must already contain any outer context labels.
// fallthrough_after: block index that comes right after `end` in the emission;
// -1 if nothing follows.
static void ns_wasm_emit_dag(ns_wasm_fn_ctx *ctx, i32 start, i32 end,
                             i32 fallthrough_after) {
    if (start > end) return;

    // Emit nested `block` wrappers: outermost first.
    // After all wrappers + B[start]'s code, closing wrapper j (depth j-start-1 from B[start])
    // exposes B[j]'s code area.
    // Wrapper for B[j] is at label depth j-start-1 from B[start].
    for (i32 j = end; j >= start + 1; j--) {
        ns_wasm_u8(&ctx->code, NS_WASM_BLOCK);
        ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
        ns_wasm_push_label(ctx, j, false);
    }

    // Emit B[start]
    i32 ft = (start < end) ? start + 1 : fallthrough_after;
    ns_wasm_emit_block(ctx, start, ft);

    // Close wrappers and emit subsequent blocks
    for (i32 k = start; k < end; k++) {
        ns_wasm_u8(&ctx->code, NS_WASM_END);
        ns_wasm_pop_label(ctx);
        i32 ft_k = (k + 1 < end) ? k + 2 : fallthrough_after;
        ns_wasm_emit_block(ctx, k + 1, ft_k);
    }
}

// Emit blocks [start..end] with loop detection and proper nesting.
static void ns_wasm_emit_range(ns_wasm_fn_ctx *ctx, i32 start, i32 end,
                               i32 *loop_heads, i32 *loop_ends, i32 n_loops,
                               i32 fallthrough_after) {
    if (start > end) return;

    // Find the first (smallest) loop header within [start, end]
    i32 found_h = -1, found_k = -1;
    for (i32 li = 0; li < n_loops; li++) {
        i32 h = loop_heads[li];
        i32 k = loop_ends[li];
        if (h >= start && h <= end && k <= end) {
            if (found_h < 0 || h < found_h) {
                found_h = h;
                found_k = k;
            }
        }
    }

    if (found_h < 0) {
        // No loops: emit as DAG
        ns_wasm_emit_dag(ctx, start, end, fallthrough_after);
        return;
    }

    // Emit blocks before the loop
    if (found_h > start) {
        // These are forward-only blocks; their fallthrough lands at found_h.
        // We need a label for found_h so these blocks can jump to it.
        // Add a block wrapper for found_h visible to [start, found_h-1].
        ns_wasm_u8(&ctx->code, NS_WASM_BLOCK);
        ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
        ns_wasm_push_label(ctx, found_h, false);
        ns_wasm_emit_range(ctx, start, found_h - 1,
                           loop_heads, loop_ends, n_loops, found_h);
        ns_wasm_u8(&ctx->code, NS_WASM_END);
        ns_wasm_pop_label(ctx);
    }

    // Emit the loop
    i32 exit_block = found_k + 1; // first block after the loop

    // Outer block: breaking out of it (br 1 from inside the loop) goes to exit_block
    ns_wasm_u8(&ctx->code, NS_WASM_BLOCK);
    ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
    ns_wasm_push_label(ctx, exit_block, false);

    // Inner loop: br 0 from inside goes back to found_h
    ns_wasm_u8(&ctx->code, NS_WASM_LOOP);
    ns_wasm_u8(&ctx->code, NS_WASM_BTVOID);
    ns_wasm_push_label(ctx, found_h, true);

    // Emit the loop body [found_h .. found_k].
    // Exclude the current loop from the table to prevent infinite re-detection.
    i32 cur_li = -1;
    for (i32 li = 0; li < n_loops; li++) {
        if (loop_heads[li] == found_h && loop_ends[li] == found_k) { cur_li = li; break; }
    }
    if (cur_li >= 0 && cur_li < n_loops - 1) {
        i32 t;
        t = loop_heads[cur_li]; loop_heads[cur_li] = loop_heads[n_loops-1]; loop_heads[n_loops-1] = t;
        t = loop_ends[cur_li];  loop_ends[cur_li]  = loop_ends[n_loops-1];  loop_ends[n_loops-1]  = t;
    }
    i32 body_n_loops = (cur_li >= 0) ? n_loops - 1 : n_loops;
    ns_wasm_emit_range(ctx, found_h, found_k,
                       loop_heads, loop_ends, body_n_loops, found_h);
    // Restore swapped entry
    if (cur_li >= 0 && cur_li < n_loops - 1) {
        i32 t;
        t = loop_heads[cur_li]; loop_heads[cur_li] = loop_heads[n_loops-1]; loop_heads[n_loops-1] = t;
        t = loop_ends[cur_li];  loop_ends[cur_li]  = loop_ends[n_loops-1];  loop_ends[n_loops-1]  = t;
    }

    ns_wasm_u8(&ctx->code, NS_WASM_END); // end loop
    ns_wasm_pop_label(ctx);

    ns_wasm_u8(&ctx->code, NS_WASM_END); // end outer block
    ns_wasm_pop_label(ctx);

    // Emit blocks after the loop
    if (exit_block <= end) {
        ns_wasm_emit_range(ctx, exit_block, end,
                           loop_heads, loop_ends, n_loops, fallthrough_after);
    }
}

// ──────────────────────────────────────────────────────────────────────────
// Function signature collection
// ──────────────────────────────────────────────────────────────────────────

// Infer the return type of a function by examining its RET instructions.
static u8 ns_wasm_fn_ret_type(ns_ssa_fn *fn, u8 *vtypes, i32 n_values) {
    if (!ns_type_is(fn->ret, NS_TYPE_UNKNOWN) && !ns_type_is(fn->ret, NS_TYPE_VOID)) {
        return ns_wasm_type_valtype(fn->ret);
    }
    if (ns_type_is(fn->ret, NS_TYPE_VOID)) return 0;
    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; bi++) {
        ns_ssa_block *bb = &fn->blocks[bi];
        for (i32 ii = 0, il = (i32)ns_array_length(bb->insts); ii < il; ii++) {
            ns_ssa_inst *inst = &fn->insts[bb->insts[ii]];
            if (inst->op == NS_SSA_OP_RET && inst->a >= 0 && inst->a < n_values) {
                return vtypes[inst->a];
            }
        }
    }
    return 0; // void
}

// Count parameters (PARAM instructions with c >= 0 in the entry block)
static i32 ns_wasm_fn_param_count(ns_ssa_fn *fn) {
    i32 n = 0;
    if (ns_array_length(fn->blocks) == 0) return 0;
    ns_ssa_block *entry = &fn->blocks[fn->entry];
    for (i32 ii = 0, il = (i32)ns_array_length(entry->insts); ii < il; ii++) {
        ns_ssa_inst *inst = &fn->insts[entry->insts[ii]];
        if (inst->op == NS_SSA_OP_PARAM && inst->c >= 0) n++;
    }
    return n;
}

// ──────────────────────────────────────────────────────────────────────────
// Build WASM sections
// ──────────────────────────────────────────────────────────────────────────

// Each function gets its own type entry (deduplicated later if desired; here
// we keep it simple and emit one type per function).

// Emit the type section: one functype per function.
static void ns_wasm_build_type_section(u8 **out, ns_ssa_module *ssa,
                                       u8 **all_vtypes, u8 **all_unsigneds, i32 *all_nvals) {
    u8 *sec = ns_null;
    i32 n_fns = (i32)ns_array_length(ssa->fns);
    i32 n_imports = (i32)ns_array_length(ssa->imports);
    ns_wasm_u32leb(&sec, (u32)(n_imports + n_fns + 2));

    for (i32 ii = 0; ii < n_imports; ++ii) {
        ns_ssa_import *import = &ssa->imports[ii];
        ns_wasm_u8(&sec, 0x60);
        ns_wasm_u32leb(&sec, (u32)ns_array_length(import->params));
        for (i32 pi = 0, pl = (i32)ns_array_length(import->params); pi < pl; ++pi) {
            ns_wasm_u8(&sec, ns_wasm_type_valtype(import->params[pi]));
        }
        if (!ns_type_is(import->ret, NS_TYPE_VOID)) {
            ns_wasm_u32leb(&sec, 1);
            ns_wasm_u8(&sec, ns_wasm_type_valtype(import->ret));
        } else {
            ns_wasm_u32leb(&sec, 0);
        }
    }

    for (i32 fi = 0; fi < n_fns; fi++) {
        ns_ssa_fn *fn = &ssa->fns[fi];

        u8 *vt = ns_null;
        u8 *us = ns_null;
        i32 nv = ns_wasm_infer_types(fn, &vt, &us);
        all_vtypes[fi] = vt;
        all_unsigneds[fi] = us;
        all_nvals[fi] = nv;

        i32 np = ns_wasm_fn_param_count(fn);
        u8 ret_vt = ns_wasm_fn_ret_type(fn, vt, nv);

        ns_wasm_u8(&sec, 0x60); // functype marker
        ns_wasm_u32leb(&sec, (u32)np);
        // Param types: values 0..np-1 in vtypes
        // But we need to get them from the PARAM instructions in order
        ns_ssa_block *entry = &fn->blocks[fn->entry];
        i32 param_written = 0;
        for (i32 ii = 0, il = (i32)ns_array_length(entry->insts); ii < il && param_written < np; ii++) {
            ns_ssa_inst *inst = &fn->insts[entry->insts[ii]];
            if (inst->op == NS_SSA_OP_PARAM && inst->c >= 0) {
                u8 pvt = (inst->dst >= 0 && inst->dst < nv) ? vt[inst->dst] : NS_WASM_I32;
                ns_wasm_u8(&sec, pvt);
                param_written++;
            }
        }

        if (ret_vt != 0) {
            ns_wasm_u32leb(&sec, 1);
            ns_wasm_u8(&sec, ret_vt);
        } else {
            ns_wasm_u32leb(&sec, 0);
        }
    }

    // __ns_alloc(i32) -> i32
    ns_wasm_u8(&sec, 0x60);
    ns_wasm_u32leb(&sec, 1);
    ns_wasm_u8(&sec, NS_WASM_I32);
    ns_wasm_u32leb(&sec, 1);
    ns_wasm_u8(&sec, NS_WASM_I32);
    // __ns_init() -> void
    ns_wasm_u8(&sec, 0x60);
    ns_wasm_u32leb(&sec, 0);
    ns_wasm_u32leb(&sec, 0);

    ns_wasm_section(out, NS_WASM_SECT_TYPE, sec);
    ns_array_free(sec);
}

// Emit the function section: type indices (one per function, all 0..n-1 in order)
static void ns_wasm_build_import_section(u8 **out, ns_ssa_module *ssa) {
    i32 n_imports = (i32)ns_array_length(ssa->imports);
    if (n_imports == 0) return;
    u8 *sec = ns_null;
    ns_wasm_u32leb(&sec, (u32)n_imports);
    for (i32 i = 0; i < n_imports; ++i) {
        ns_ssa_import *import = &ssa->imports[i];
        ns_wasm_u32leb(&sec, (u32)import->module.len);
        for (i32 c = 0; c < import->module.len; ++c) ns_wasm_u8(&sec, (u8)import->module.data[c]);
        ns_wasm_u32leb(&sec, (u32)import->name.len);
        for (i32 c = 0; c < import->name.len; ++c) ns_wasm_u8(&sec, (u8)import->name.data[c]);
        ns_wasm_u8(&sec, 0x00);
        ns_wasm_u32leb(&sec, (u32)i);
    }
    ns_wasm_section(out, NS_WASM_SECT_IMPORT, sec);
    ns_array_free(sec);
}

static void ns_wasm_build_function_section(u8 **out, i32 n_imports, i32 n_fns) {
    u8 *sec = ns_null;
    ns_wasm_u32leb(&sec, (u32)(n_fns + 2));
    for (i32 i = 0; i < n_fns; i++) {
        ns_wasm_u32leb(&sec, (u32)(n_imports + i));
    }
    ns_wasm_u32leb(&sec, (u32)(n_imports + n_fns));
    ns_wasm_u32leb(&sec, (u32)(n_imports + n_fns + 1));
    ns_wasm_section(out, NS_WASM_SECT_FUNCTION, sec);
    ns_array_free(sec);
}

// Emit the export section: export all functions by name
static void ns_wasm_build_memory_section(u8 **out, u32 heap_start) {
    u8 *sec = ns_null;
    u32 pages = (heap_start + 65535u) / 65536u;
    if (pages == 0) pages = 1;
    ns_wasm_u32leb(&sec, 1);
    ns_wasm_u8(&sec, 0x00);
    ns_wasm_u32leb(&sec, pages);
    ns_wasm_section(out, NS_WASM_SECT_MEMORY, sec);
    ns_array_free(sec);
}

static void ns_wasm_build_global_section(u8 **out, ns_ssa_module *ssa, u32 heap_start) {
    u8 *sec = ns_null;
    ns_wasm_u32leb(&sec, (u32)ns_array_length(ssa->globals) + 2u);
    ns_wasm_u8(&sec, NS_WASM_I32);
    ns_wasm_u8(&sec, 0x01);
    ns_wasm_u8(&sec, NS_WASM_I32_CONST);
    ns_wasm_i64leb(&sec, heap_start);
    ns_wasm_u8(&sec, NS_WASM_END);
    for (i32 i = 0, l = (i32)ns_array_length(ssa->globals); i < l; ++i) {
        u8 type = ns_wasm_type_valtype(ssa->globals[i].type);
        ns_wasm_u8(&sec, type);
        ns_wasm_u8(&sec, 0x01);
        if (type == NS_WASM_I64) {
            ns_wasm_u8(&sec, NS_WASM_I64_CONST); ns_wasm_i64leb(&sec, 0);
        } else if (type == NS_WASM_F32) {
            ns_wasm_u8(&sec, NS_WASM_F32_CONST); ns_wasm_f32bytes(&sec, 0);
        } else if (type == NS_WASM_F64) {
            ns_wasm_u8(&sec, NS_WASM_F64_CONST); ns_wasm_f64bytes(&sec, 0);
        } else {
            ns_wasm_u8(&sec, NS_WASM_I32_CONST); ns_wasm_i64leb(&sec, 0);
        }
        ns_wasm_u8(&sec, NS_WASM_END);
    }
    // Private idempotence flag for __ns_init.
    ns_wasm_u8(&sec, NS_WASM_I32);
    ns_wasm_u8(&sec, 0x01);
    ns_wasm_u8(&sec, NS_WASM_I32_CONST); ns_wasm_i64leb(&sec, 0);
    ns_wasm_u8(&sec, NS_WASM_END);
    ns_wasm_section(out, NS_WASM_SECT_GLOBAL, sec);
    ns_array_free(sec);
}

static void ns_wasm_name(u8 **out, const char *name) {
    u32 len = (u32)strlen(name);
    ns_wasm_u32leb(out, len);
    for (u32 i = 0; i < len; ++i) ns_wasm_u8(out, (u8)name[i]);
}

static void ns_wasm_build_export_section(u8 **out, ns_ssa_module *ssa) {
    u8 *sec = ns_null;
    i32 n_fns = (i32)ns_array_length(ssa->fns);
    i32 n_imports = (i32)ns_array_length(ssa->imports);
    ns_wasm_u32leb(&sec, (u32)(n_fns + 3));
    for (i32 fi = 0; fi < n_fns; fi++) {
        ns_ssa_fn *fn = &ssa->fns[fi];
        ns_wasm_u32leb(&sec, (u32)fn->name.len);
        for (i32 c = 0; c < fn->name.len; c++) ns_wasm_u8(&sec, (u8)fn->name.data[c]);
        ns_wasm_u8(&sec, 0x00); // kind: function
        ns_wasm_u32leb(&sec, (u32)(n_imports + fi));
    }
    ns_wasm_name(&sec, "memory");
    ns_wasm_u8(&sec, 0x02);
    ns_wasm_u32leb(&sec, 0);
    ns_wasm_name(&sec, "__ns_alloc");
    ns_wasm_u8(&sec, 0x00);
    ns_wasm_u32leb(&sec, (u32)(n_imports + n_fns));
    ns_wasm_name(&sec, "__ns_init");
    ns_wasm_u8(&sec, 0x00);
    ns_wasm_u32leb(&sec, (u32)(n_imports + n_fns + 1));
    ns_wasm_section(out, NS_WASM_SECT_EXPORT, sec);
    ns_array_free(sec);
}

static void ns_wasm_build_element_section(u8 **out) {
    u8 *sec = ns_null;
    ns_wasm_u32leb(&sec, 0); // indirect calls/closures are intentionally absent in browser v1
    ns_wasm_section(out, NS_WASM_SECT_ELEMENT, sec);
    ns_array_free(sec);
}

// DFS color codes for back-edge detection
#define NS_WASM_COLOR_WHITE 0   // unvisited
#define NS_WASM_COLOR_GRAY  1   // on DFS stack (edge to GRAY = true back-edge)
#define NS_WASM_COLOR_BLACK 2   // finished

static void ns_wasm_dfs_visit(ns_ssa_fn *fn, i32 bi, u8 *color,
                               i32 **lh, i32 **le) {
    color[bi] = NS_WASM_COLOR_GRAY;

    ns_ssa_block *bb = &fn->blocks[bi];
    i32 n_insts = (i32)ns_array_length(bb->insts);
    if (n_insts > 0) {
        ns_ssa_inst *term = &fn->insts[bb->insts[n_insts - 1]];
        i32 targets[2] = {term->target0, term->target1};
        for (i32 t = 0; t < 2; t++) {
            i32 j = targets[t];
            if (j < 0) continue;
            if (color[j] == NS_WASM_COLOR_GRAY) {
                // True back-edge bi → j: j is a loop header, bi is the loop end
                ns_bool found = false;
                for (i32 li = 0, ll = (i32)ns_array_length(*lh); li < ll; li++) {
                    if ((*lh)[li] == j) {
                        if ((*le)[li] < bi) (*le)[li] = bi;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ns_array_push(*lh, j);
                    ns_array_push(*le, bi);
                }
            } else if (color[j] == NS_WASM_COLOR_WHITE) {
                ns_wasm_dfs_visit(fn, j, color, lh, le);
            }
            // BLACK: forward/cross edge — not a loop back-edge
        }
    }

    color[bi] = NS_WASM_COLOR_BLACK;
}

// Detect loop back-edges and collect (header, end) pairs using DFS.
// A back-edge is an edge to a GRAY (on DFS stack) node; forward/cross
// edges to BLACK nodes (e.g., merge blocks in if/else chains) are NOT loops.
static void ns_wasm_detect_loops(ns_ssa_fn *fn,
                                 i32 **loop_heads_out, i32 **loop_ends_out,
                                 i32 *n_loops_out) {
    i32 *lh = ns_null;
    i32 *le = ns_null;

    i32 n_blocks = (i32)ns_array_length(fn->blocks);
    if (n_blocks > 0) {
        u8 *color = (u8 *)ns_malloc((szt)n_blocks * sizeof(u8));
        memset(color, NS_WASM_COLOR_WHITE, (szt)n_blocks * sizeof(u8));
        ns_wasm_dfs_visit(fn, 0, color, &lh, &le);
        ns_free(color);
    }

    *loop_heads_out = lh;
    *loop_ends_out  = le;
    *n_loops_out    = (i32)ns_array_length(lh);
}

// Emit the code section
static void ns_wasm_build_code_section(u8 **out, ns_ssa_module *ssa,
                                       u8 **all_vtypes, u8 **all_unsigneds, i32 *all_nvals) {
    u8 *sec = ns_null;
    i32 n_fns = (i32)ns_array_length(ssa->fns);
    ns_wasm_u32leb(&sec, (u32)(n_fns + 2));

    for (i32 fi = 0; fi < n_fns; fi++) {
        ns_ssa_fn *fn = &ssa->fns[fi];
        u8  *vt = all_vtypes[fi];
        i32  nv = all_nvals[fi];
        i32  np = ns_wasm_fn_param_count(fn);

        // Keep the structured lowering helpers compiled while the dispatcher
        // is the authoritative path; the dispatcher handles all CFG shapes.
        i32 *lheads = ns_null, *lends = ns_null, n_loops = 0;
        ns_wasm_detect_loops(fn, &lheads, &lends, &n_loops);

        // Build function body
        ns_wasm_fn_ctx ctx = {0};
        ctx.fn       = fn;
        ctx.n_params = np;
        ctx.n_values = nv;
        ctx.pc_local = nv;
        ctx.vtypes   = vt;
        ctx.unsigneds = all_unsigneds[fi];
        ctx.all_fns  = ssa->fns;
        ctx.n_fns    = n_fns;
        ctx.imports  = ssa->imports;
        ctx.n_imports = (i32)ns_array_length(ssa->imports);

        // Local declarations:
        // WASM params are locals 0..np-1 (already bound, not declared).
        // Additional locals np..nv-1 need to be declared. WASM assigns local
        // indices in declaration order, so only coalesce adjacent equal types;
        // grouping every i32 before every i64 would detach SSA value ids from
        // their local indices when a function mixes widths.
        u8 *locals_sec = ns_null;
        i32 n_extra = nv - np;
        if (n_extra > 0) {
            u32 n_groups = 1;
            for (i32 vi = np + 1; vi < nv; vi++) {
                if (vt[vi] != vt[vi - 1]) n_groups++;
            }
            // One additional i32 group stores the CFG dispatcher pc unless the
            // final SSA local group is already i32, in which case extend it.
            ns_bool merge_pc = vt[nv - 1] == NS_WASM_I32;
            ns_wasm_u32leb(&locals_sec, n_groups + (merge_pc ? 0 : 1));
            i32 run_start = np;
            for (i32 vi = np + 1; vi <= nv; vi++) {
                if (vi == nv || vt[vi] != vt[run_start]) {
                    u32 run_count = (u32)(vi - run_start);
                    if (vi == nv && vt[run_start] == NS_WASM_I32) run_count++;
                    ns_wasm_u32leb(&locals_sec, run_count);
                    ns_wasm_u8(&locals_sec, vt[run_start]);
                    run_start = vi;
                }
            }
            if (!merge_pc) {
                ns_wasm_u32leb(&locals_sec, 1);
                ns_wasm_u8(&locals_sec, NS_WASM_I32);
            }
        } else {
            ns_wasm_u32leb(&locals_sec, 1);
            ns_wasm_u32leb(&locals_sec, 1);
            ns_wasm_u8(&locals_sec, NS_WASM_I32);
        }

        // Emit function body instructions
        i32 n_blocks = (i32)ns_array_length(fn->blocks);
        if (false && n_blocks > 0) ns_wasm_emit_range(&ctx, 0, n_blocks - 1, lheads, lends, n_loops, -1);
        if (n_blocks > 0) ns_wasm_emit_dispatch(&ctx);

        // WASM functions must end with END opcode
        ns_wasm_u8(&ctx.code, NS_WASM_END);

        // Assemble function body: locals_sec + ctx.code, prefixed by body size
        u32 body_size = (u32)(ns_array_length(locals_sec) + ns_array_length(ctx.code));
        ns_wasm_u32leb(&sec, body_size);
        for (u32 i = 0; i < (u32)ns_array_length(locals_sec); i++) ns_array_push(sec, locals_sec[i]);
        for (u32 i = 0; i < (u32)ns_array_length(ctx.code);    i++) ns_array_push(sec, ctx.code[i]);

        // Cleanup
        ns_array_free(ctx.code);
        ns_array_free(ctx.labels);
        ns_array_free(locals_sec);
        ns_array_free(lheads);
        ns_array_free(lends);
    }

    // __ns_alloc: aligned bump allocation with checked memory growth.
    u8 *alloc = ns_null;
    ns_wasm_u32leb(&alloc, 1);
    ns_wasm_u32leb(&alloc, 2);
    ns_wasm_u8(&alloc, NS_WASM_I32);
    ns_wasm_u8(&alloc, NS_WASM_GLOBAL_GET); ns_wasm_u32leb(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_LOCAL_SET); ns_wasm_u32leb(&alloc, 1);
    ns_wasm_u8(&alloc, NS_WASM_GLOBAL_GET); ns_wasm_u32leb(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_LOCAL_GET); ns_wasm_u32leb(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_I32_ADD);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, 7);
    ns_wasm_u8(&alloc, NS_WASM_I32_ADD);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, -8);
    ns_wasm_u8(&alloc, NS_WASM_I32_AND);
    ns_wasm_u8(&alloc, NS_WASM_LOCAL_TEE); ns_wasm_u32leb(&alloc, 2);
    ns_wasm_u8(&alloc, NS_WASM_GLOBAL_SET); ns_wasm_u32leb(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_LOCAL_GET); ns_wasm_u32leb(&alloc, 2);
    ns_wasm_u8(&alloc, NS_WASM_MEMORY_SIZE); ns_wasm_u8(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, 16);
    ns_wasm_u8(&alloc, NS_WASM_I32_SHL);
    ns_wasm_u8(&alloc, NS_WASM_I32_GT_U);
    ns_wasm_u8(&alloc, NS_WASM_IF); ns_wasm_u8(&alloc, NS_WASM_BTVOID);
    ns_wasm_u8(&alloc, NS_WASM_LOCAL_GET); ns_wasm_u32leb(&alloc, 2);
    ns_wasm_u8(&alloc, NS_WASM_MEMORY_SIZE); ns_wasm_u8(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, 16);
    ns_wasm_u8(&alloc, NS_WASM_I32_SHL);
    ns_wasm_u8(&alloc, NS_WASM_I32_SUB);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, 65535);
    ns_wasm_u8(&alloc, NS_WASM_I32_ADD);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, 16);
    ns_wasm_u8(&alloc, NS_WASM_I32_SHR_U);
    ns_wasm_u8(&alloc, NS_WASM_MEMORY_GROW); ns_wasm_u8(&alloc, 0);
    ns_wasm_u8(&alloc, NS_WASM_I32_CONST); ns_wasm_i64leb(&alloc, -1);
    ns_wasm_u8(&alloc, NS_WASM_I32_EQ);
    ns_wasm_u8(&alloc, NS_WASM_IF); ns_wasm_u8(&alloc, NS_WASM_BTVOID);
    ns_wasm_u8(&alloc, NS_WASM_UNREACHABLE);
    ns_wasm_u8(&alloc, NS_WASM_END);
    ns_wasm_u8(&alloc, NS_WASM_END);
    ns_wasm_u8(&alloc, NS_WASM_LOCAL_GET); ns_wasm_u32leb(&alloc, 1);
    ns_wasm_u8(&alloc, NS_WASM_END);
    ns_wasm_u32leb(&sec, (u32)ns_array_length(alloc));
    for (u32 i = 0; i < (u32)ns_array_length(alloc); ++i) ns_array_push(sec, alloc[i]);
    ns_array_free(alloc);

    // __ns_init calls top-level module initialization exactly once per instance.
    u8 *init = ns_null;
    ns_wasm_u32leb(&init, 0);
    u32 init_global = (u32)ns_array_length(ssa->globals) + 1u;
    ns_wasm_u8(&init, NS_WASM_GLOBAL_GET); ns_wasm_u32leb(&init, init_global);
    ns_wasm_u8(&init, NS_WASM_I32_EQZ);
    ns_wasm_u8(&init, NS_WASM_IF); ns_wasm_u8(&init, NS_WASM_BTVOID);
    for (i32 fi = 0; fi < n_fns; ++fi) {
        if (ns_str_equals(ssa->fns[fi].name, ns_str_cstr("__module_init"))) {
            ns_wasm_u8(&init, NS_WASM_CALL);
            ns_wasm_u32leb(&init, (u32)(ns_array_length(ssa->imports) + fi));
            break;
        }
    }
    ns_wasm_u8(&init, NS_WASM_I32_CONST); ns_wasm_i64leb(&init, 1);
    ns_wasm_u8(&init, NS_WASM_GLOBAL_SET); ns_wasm_u32leb(&init, init_global);
    ns_wasm_u8(&init, NS_WASM_END);
    ns_wasm_u8(&init, NS_WASM_END);
    ns_wasm_u32leb(&sec, (u32)ns_array_length(init));
    for (u32 i = 0; i < (u32)ns_array_length(init); ++i) ns_array_push(sec, init[i]);
    ns_array_free(init);

    ns_wasm_section(out, NS_WASM_SECT_CODE, sec);
    ns_array_free(sec);
}

static u8 *ns_wasm_prepare_data(ns_ssa_module *ssa, u32 *heap_start) {
    u8 *data = ns_null;
    ns_array_set_length(data, 1024);
    memset(data, 0, 1024);
    for (i32 fi = 0, fl = (i32)ns_array_length(ssa->fns); fi < fl; ++fi) {
        ns_ssa_fn *fn = &ssa->fns[fi];
        for (i32 ii = 0, il = (i32)ns_array_length(fn->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[ii];
            if (inst->op != NS_SSA_OP_CONST || !ns_type_is(inst->type, NS_TYPE_STRING)) continue;
            ns_str value = ns_str_unescape(inst->name);
            u32 descriptor = (u32)((ns_array_length(data) + 7u) & ~7u);
            ns_array_set_length(data, descriptor);
            u32 bytes = descriptor + 8;
            ns_wasm_u32bytes(&data, bytes);
            ns_wasm_u32bytes(&data, (u32)value.len);
            for (i32 i = 0; i < value.len; ++i) ns_wasm_u8(&data, (u8)value.data[i]);
            inst->c = (i32)descriptor;
            ns_str_free(value);
        }
    }
    *heap_start = (u32)((ns_array_length(data) + 15u) & ~15u);
    return data;
}

static void ns_wasm_build_data_section(u8 **out, u8 *data) {
    if (ns_array_length(data) == 0) return;
    u8 *sec = ns_null;
    ns_wasm_u32leb(&sec, 1);
    ns_wasm_u32leb(&sec, 0);
    ns_wasm_u8(&sec, NS_WASM_I32_CONST);
    ns_wasm_i64leb(&sec, 0);
    ns_wasm_u8(&sec, NS_WASM_END);
    ns_wasm_u32leb(&sec, (u32)ns_array_length(data));
    for (u32 i = 0; i < (u32)ns_array_length(data); ++i) ns_array_push(sec, data[i]);
    ns_wasm_section(out, NS_WASM_SECT_DATA, sec);
    ns_array_free(sec);
}

static void ns_wasm_build_shader_custom_section(u8 **out, ns_ssa_module *ssa) {
    u8 *sec = ns_null;
    ns_wasm_name(&sec, "ns.shaders");
    ns_wasm_u32leb(&sec, 1);
    ns_wasm_u32leb(&sec, (u32)ns_array_length(ssa->shaders));
    for (i32 i = 0, l = (i32)ns_array_length(ssa->shaders); i < l; ++i) {
        ns_ssa_shader *shader = &ssa->shaders[i];
        ns_wasm_u32leb(&sec, shader->id);
        ns_wasm_u8(&sec, (u8)shader->stage);
        ns_wasm_u32leb(&sec, (u32)shader->name.len);
        for (i32 c = 0; c < shader->name.len; ++c) ns_wasm_u8(&sec, (u8)shader->name.data[c]);
        ns_wasm_u32leb(&sec, (u32)shader->wgsl.len);
        for (i32 c = 0; c < shader->wgsl.len; ++c) ns_wasm_u8(&sec, (u8)shader->wgsl.data[c]);
        ns_wasm_u32leb(&sec, (u32)shader->vertex_stride);
        ns_wasm_u32leb(&sec, (u32)ns_array_length(shader->vertex_offsets));
        for (i32 a = 0, al = (i32)ns_array_length(shader->vertex_offsets); a < al; ++a) {
            ns_wasm_u32leb(&sec, (u32)shader->vertex_offsets[a]);
            ns_wasm_u32leb(&sec, (u32)shader->vertex_sizes[a]);
        }
    }
    ns_wasm_section(out, 0, sec);
    ns_array_free(sec);
}

// ──────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────

static ns_bool ns_wasm_supported_module(ns_str module) {
    return ns_str_equals(module, ns_str_cstr("std")) ||
           ns_str_equals(module, ns_str_cstr("gpu")) ||
           ns_str_equals(module, ns_str_cstr("shader"));
}

static ns_bool ns_wasm_name_in(ns_str name, const char *names) {
    const char *begin = names;
    while (*begin) {
        const char *end = strchr(begin, '|');
        if (!end) end = begin + strlen(begin);
        if (name.len == end - begin && strncmp(name.data, begin, (szt)name.len) == 0) return true;
        begin = *end ? end + 1 : end;
    }
    return false;
}

static ns_bool ns_wasm_supported_import(ns_str module, ns_str name) {
    if (ns_str_equals(module, ns_str_cstr("std"))) {
        return ns_wasm_name_in(name,
            "print|sqrt|sin|cos|tan|atan2|ftos|stof|substr|unescape|utf8_len");
    }
    if (ns_str_equals(module, ns_str_cstr("shader"))) {
        return ns_wasm_name_in(name,
            "shader_transpile|shader_transpile_stage|shader_entry|shader_vertex_stride|"
            "shader_vertex_attr_count|shader_vertex_attr_offset|shader_vertex_attr_size");
    }
    if (ns_str_equals(module, ns_str_cstr("gpu"))) {
        return ns_wasm_name_in(name,
            "dispatch_gpu|gpu_create_pipeline|gpu_create_pipeline_ex|gpu_texture_new|gpu_texture_new_2d|"
            "gpu_texture_none|gpu_texture_valid|gpu_texture_bytes|gpu_texture_update|gpu_texture_update_all|"
            "gpu_texture_release|gpu_sampler_new|gpu_sampler_valid|gpu_sampler_release|gpu_render_state_new|"
            "gpu_render_state_bind|gpu_memory_alloc|gpu_memory_valid|gpu_memory_at|gpu_memory_write|"
            "gpu_memory_read|gpu_memory_free|gpu_shader_graphics|gpu_shader_compute|gpu_shader_valid|"
            "gpu_shader_bind|gpu_shader_release|gpu_pass_begin_target|gpu_request_device|gpu_destroy_device|"
            "gpu_shader_target|gpu_dispatch_compute_source|"
            "gpu_dispatch_compute_texture_source|gpu_create_buffer|gpu_create_index_buffer|"
            "gpu_create_uniform_buffer|gpu_update_buffer|gpu_update_texture_id|gpu_create_shader_source|"
            "gpu_create_pipeline_layout|gpu_create_pipeline_layout_ex|gpu_create_pipeline_layout_indexed_ex|"
            "gpu_create_mesh_1|gpu_create_mesh_indexed|gpu_create_texture_2d|gpu_create_texture_binding|"
            "gpu_create_buffer_texture_binding|gpu_create_depth_pass|gpu_create_screen_pass|"
            "gpu_destroy_texture_id|gpu_destroy_binding_id|gpu_destroy_buffer_id|gpu_destroy_shader_id|"
            "gpu_destroy_pipeline_id|gpu_destroy_mesh_id|gpu_destroy_render_pass_id|gpu_begin_render_pass_id|"
            "gpu_set_pipeline_id|gpu_set_mesh_id|gpu_set_binding_id|gpu_begin_render_pass|gpu_set_viewport|"
            "gpu_set_scissor|gpu_set_pipeline|gpu_set_binding|gpu_set_mesh|gpu_draw|gpu_end_pass|gpu_commit|"
            "gpu_caps|gpu_malloc|gpu_free|gpu_write|gpu_read|gpu_frame_alloc|gpu_texture_create|"
            "gpu_texture_upload|gpu_texture_destroy|gpu_sampler_create|gpu_sampler_destroy|"
            "gpu_shader_graphics_create|gpu_shader_compute_create|gpu_shader_destroy|gpu_state_create|"
            "gpu_pass_begin|gpu_screen_pass_begin|gpu_pass_end|gpu_set_shader|gpu_set_state|gpu_set_root|"
            "gpu_set_root_data|gpu_draw_vertices|gpu_draw_indexed|gpu_draw_indirect|gpu_dispatch|"
            "gpu_dispatch_indirect|gpu_signal_after|gpu_wait_before|gpu_pixel_format_size|"
            "gpu_pixel_format_row_pitch|gpu_pixel_format_surface_pitch");
    }
    return false;
}

static ns_bool ns_wasm_portable_use(ns_str module) {
    return ns_wasm_supported_module(module) || ns_str_equals(module, ns_str_cstr("simd"));
}

static ns_bool ns_wasm_unsupported_type(ns_type type) {
    return ns_type_is(type, NS_TYPE_ANY) || ns_type_is(type, NS_TYPE_DICT) ||
           ns_type_is(type, NS_TYPE_SET) || ns_type_is(type, NS_TYPE_TASK) ||
           ns_type_is(type, NS_TYPE_BLOCK) || ns_type_is(type, NS_TYPE_FN) ||
           ns_type_is(type, NS_TYPE_UNION);
}

static ns_bool ns_wasm_emittable_op(ns_ssa_op op) {
    switch (op) {
    case NS_SSA_OP_UNDEF: case NS_SSA_OP_CONST: case NS_SSA_OP_PARAM:
    case NS_SSA_OP_COPY: case NS_SSA_OP_PHI: case NS_SSA_OP_CAST:
    case NS_SSA_OP_CALL: case NS_SSA_OP_ARG: case NS_SSA_OP_GLOBAL_GET:
    case NS_SSA_OP_GLOBAL_SET: case NS_SSA_OP_ALLOC: case NS_SSA_OP_CLONE: case NS_SSA_OP_LOAD:
    case NS_SSA_OP_STORE: case NS_SSA_OP_ARRAY_NEW: case NS_SSA_OP_ARRAY_STORE:
    case NS_SSA_OP_INDEX: case NS_SSA_OP_NEG: case NS_SSA_OP_NOT:
    case NS_SSA_OP_ADD: case NS_SSA_OP_SUB: case NS_SSA_OP_MUL:
    case NS_SSA_OP_DIV: case NS_SSA_OP_MOD: case NS_SSA_OP_SHL:
    case NS_SSA_OP_SHR: case NS_SSA_OP_BAND: case NS_SSA_OP_BOR:
    case NS_SSA_OP_BXOR: case NS_SSA_OP_AND: case NS_SSA_OP_OR:
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE: case NS_SSA_OP_LT:
    case NS_SSA_OP_LE: case NS_SSA_OP_GT: case NS_SSA_OP_GE:
    case NS_SSA_OP_ASSERT: case NS_SSA_OP_BR: case NS_SSA_OP_JMP:
    case NS_SSA_OP_RET:
        return true;
    default:
        return false;
    }
}

static ns_bool ns_wasm_is_shader_fn(ns_ssa_module *ssa, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(ssa->shaders); i < l; ++i) {
        if (ns_str_equals(ssa->shaders[i].name, name)) return true;
    }
    return false;
}

static ns_bool ns_wasm_has_target(ns_ssa_module *ssa, ns_ssa_inst *call) {
    if (!ns_str_is_empty(call->module)) {
        for (i32 i = 0, l = (i32)ns_array_length(ssa->imports); i < l; ++i) {
            if (ns_str_equals(ssa->imports[i].module, call->module) && ns_str_equals(ssa->imports[i].name, call->name)) return true;
        }
        return false;
    }
    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        if (ns_str_equals(ssa->fns[i].name, call->name)) return true;
    }
    return false;
}

static ns_return_bool ns_wasm_validate(ns_ssa_module *ssa) {
    static char message[512];
    if (ssa->ctx) {
        for (i32 i = 0, l = (i32)ns_array_length(ssa->ctx->nodes); i < l; ++i) {
            ns_ast_t *node = &ssa->ctx->nodes[i];
            if (node->type != NS_AST_USE_STMT || ns_wasm_portable_use(node->use_stmt.lib.val)) continue;
            snprintf(message, sizeof(message), "wasm target does not support module `%.*s`.",
                     node->use_stmt.lib.val.len, node->use_stmt.lib.val.data);
            return ns_return_error(bool, ns_ast_state_loc(ssa->ctx, node->state), NS_ERR_EVAL, message);
        }
    }
    for (i32 i = 0, l = (i32)ns_array_length(ssa->imports); i < l; ++i) {
        if (!ns_wasm_supported_module(ssa->imports[i].module)) {
            snprintf(message, sizeof(message), "wasm target does not support module `%.*s`.",
                     ssa->imports[i].module.len, ssa->imports[i].module.data);
            return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, message);
        }
    }
    for (i32 i = 0, l = (i32)ns_array_length(ssa->globals); i < l; ++i) {
        if (!ns_wasm_unsupported_type(ssa->globals[i].type)) continue;
        snprintf(message, sizeof(message), "wasm target does not support global `%.*s` with this type.",
                 ssa->globals[i].name.len, ssa->globals[i].name.data);
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, message);
    }
    for (i32 fi = 0, fl = (i32)ns_array_length(ssa->fns); fi < fl; ++fi) {
        ns_ssa_fn *fn = &ssa->fns[fi];
        if (ns_wasm_unsupported_type(fn->ret)) {
            snprintf(message, sizeof(message), "wasm target does not support the result type of fn `%.*s`.",
                     fn->name.len, fn->name.data);
            return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, message);
        }
        for (i32 pi = 0, pl = (i32)ns_array_length(fn->params); pi < pl; ++pi) {
            if (!ns_wasm_unsupported_type(fn->params[pi])) continue;
            snprintf(message, sizeof(message), "wasm target does not support parameter %d of fn `%.*s`.",
                     pi + 1, fn->name.len, fn->name.data);
            return ns_return_error(bool, ns_code_loc_nil, NS_ERR_EVAL, message);
        }
        u8 *types = ns_null, *unsigneds = ns_null;
        i32 value_count = ns_wasm_infer_types(fn, &types, &unsigneds);
        for (i32 ii = 0, il = (i32)ns_array_length(fn->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[ii];
            ns_code_loc loc = inst->ast > 0 && ssa->ctx && inst->ast < (i32)ns_array_length(ssa->ctx->nodes)
                ? ns_ast_state_loc(ssa->ctx, ssa->ctx->nodes[inst->ast].state) : ns_code_loc_nil;
            if (ns_wasm_unsupported_type(inst->type)) {
                snprintf(message, sizeof(message), "wasm target does not support this value type in fn `%.*s`.",
                         fn->name.len, fn->name.data);
                ns_array_free(types); ns_array_free(unsigneds);
                return ns_return_error(bool, loc, NS_ERR_EVAL, message);
            }
            if (!ns_wasm_emittable_op(inst->op)) {
                snprintf(message, sizeof(message), "wasm target does not support `%.*s` in fn `%.*s`.",
                         ns_ssa_op_to_string(inst->op).len, ns_ssa_op_to_string(inst->op).data,
                         fn->name.len, fn->name.data);
                ns_array_free(types); ns_array_free(unsigneds);
                return ns_return_error(bool, loc, NS_ERR_EVAL, message);
            }
            if (inst->op == NS_SSA_OP_CALL && !ns_str_is_empty(inst->module) &&
                !ns_wasm_supported_import(inst->module, inst->name)) {
                snprintf(message, sizeof(message), "wasm target does not support import `%.*s.%.*s`.",
                         inst->module.len, inst->module.data, inst->name.len, inst->name.data);
                ns_array_free(types); ns_array_free(unsigneds);
                return ns_return_error(bool, loc, NS_ERR_EVAL, message);
            }
            if (inst->op == NS_SSA_OP_CALL && (!ns_wasm_has_target(ssa, inst) || ns_str_is_empty(inst->name))) {
                snprintf(message, sizeof(message), "wasm target cannot resolve direct call in fn `%.*s`.", fn->name.len, fn->name.data);
                ns_array_free(types); ns_array_free(unsigneds);
                return ns_return_error(bool, loc, NS_ERR_EVAL, message);
            }
            if (inst->op == NS_SSA_OP_PARAM && inst->c < 0) {
                ns_bool direct_callee = false;
                for (i32 ci = 0; ci < il; ++ci) {
                    if (fn->insts[ci].op == NS_SSA_OP_CALL && fn->insts[ci].a == inst->dst) { direct_callee = true; break; }
                }
                if (!direct_callee) {
                    snprintf(message, sizeof(message), "wasm target cannot lower unresolved global or function value `%.*s`.",
                             inst->name.len, inst->name.data);
                    ns_array_free(types); ns_array_free(unsigneds);
                    return ns_return_error(bool, loc, NS_ERR_EVAL, message);
                }
            }
            if (inst->op == NS_SSA_OP_CONST && ns_type_is(inst->type, NS_TYPE_STRING) && inst->token.type != NS_TOKEN_STR_LITERAL) {
                ns_array_free(types); ns_array_free(unsigneds);
                return ns_return_error(bool, loc, NS_ERR_EVAL,
                                       "wasm target does not yet support interpolated strings; build the string from portable std operations.");
            }
            if (inst->op == NS_SSA_OP_MOD && inst->a >= 0 && inst->a < value_count &&
                (types[inst->a] == NS_WASM_F32 || types[inst->a] == NS_WASM_F64)) {
                ns_array_free(types); ns_array_free(unsigneds);
                return ns_return_error(bool, loc, NS_ERR_EVAL, "wasm target does not support floating-point remainder.");
            }
        }
        ns_array_free(types); ns_array_free(unsigneds);
    }
    return ns_return_ok(bool, true);
}

ns_return_bool ns_wasm_emit(ns_ssa_module *ssa, ns_str output_path) {
    if (!ssa) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "null ssa module");
    }
    if (ns_str_is_empty(output_path)) {
        return ns_return_error(bool, ns_code_loc_nil, NS_ERR_SYNTAX, "empty output path");
    }
    // Shader entry functions are compiled ahead of time into the ns.shaders
    // custom section. They are function values on the CPU side (lowered to
    // compiler-assigned integer IDs), not callable Wasm functions. Excluding
    // their CPU SSA bodies also keeps shader-only structs and stage I/O out of
    // the wasm32 host ABI without weakening diagnostics for ordinary code.
    ns_ssa_module host = *ssa;
    host.fns = ns_null;
    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        if (!ns_wasm_is_shader_fn(ssa, ssa->fns[i].name)) ns_array_push(host.fns, ssa->fns[i]);
    }

    ns_return_bool validation = ns_wasm_validate(&host);
    if (ns_return_is_error(validation)) {
        ns_array_free(host.fns);
        return validation;
    }

    i32 n_fns = (i32)ns_array_length(host.fns);
    i32 n_imports = (i32)ns_array_length(host.imports);
    u32 heap_start = 0;
    u8 *data = ns_wasm_prepare_data(&host, &heap_start);

    // Per-function type info (allocated in type section pass, freed at end)
    u8 **all_vtypes = (u8 **)ns_malloc((szt)n_fns * sizeof(u8 *));
    u8 **all_unsigneds = (u8 **)ns_malloc((szt)n_fns * sizeof(u8 *));
    i32  *all_nvals = (i32  *)ns_malloc((szt)n_fns * sizeof(i32));
    memset(all_vtypes, 0, (szt)n_fns * sizeof(u8 *));
    memset(all_unsigneds, 0, (szt)n_fns * sizeof(u8 *));
    memset(all_nvals,  0, (szt)n_fns * sizeof(i32));

    u8 *buf = ns_null;

    // Magic + version
    for (i32 i = 0; i < 4; i++) ns_array_push(buf, ns_wasm_magic[i]);
    for (i32 i = 0; i < 4; i++) ns_array_push(buf, ns_wasm_version[i]);

    // Type section (also populates all_vtypes / all_nvals)
    ns_wasm_build_type_section(&buf, &host, all_vtypes, all_unsigneds, all_nvals);

    ns_wasm_build_import_section(&buf, &host);

    // Function section
    ns_wasm_build_function_section(&buf, n_imports, n_fns);

    ns_wasm_build_memory_section(&buf, heap_start);
    ns_wasm_build_global_section(&buf, &host, heap_start);

    // Export section
    ns_wasm_build_export_section(&buf, &host);
    ns_wasm_build_element_section(&buf);

    // Code section
    ns_wasm_build_code_section(&buf, &host, all_vtypes, all_unsigneds, all_nvals);

    ns_wasm_build_data_section(&buf, data);
    ns_wasm_build_shader_custom_section(&buf, ssa);

    // Write output file
    ns_str temporary = ns_str_concat(output_path, ns_str_cstr(".tmp"));
    FILE *f = fopen(temporary.data, "wb");
    ns_return_bool ret;
    if (!f) {
        ret = ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to open wasm output file");
    } else {
        szt written = fwrite(buf, 1, ns_array_length(buf), f);
        fclose(f);
        if (written != ns_array_length(buf)) {
            ret = ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to write wasm output");
        } else {
#if defined(_WIN32)
            remove(output_path.data);
#endif
            if (rename(temporary.data, output_path.data) != 0) {
                ret = ns_return_error(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "failed to replace wasm output");
            } else {
                ret = ns_return_ok(bool, true);
            }
        }
    }
    if (ns_return_is_error(ret)) remove(temporary.data);

    // Free per-function vtypes
    for (i32 i = 0; i < n_fns; i++) {
        if (all_vtypes[i]) ns_array_free(all_vtypes[i]);
        if (all_unsigneds[i]) ns_array_free(all_unsigneds[i]);
    }
    ns_free(all_vtypes);
    ns_free(all_unsigneds);
    ns_free(all_nvals);
    ns_array_free(buf);
    ns_array_free(data);
    ns_array_free(host.fns);
    ns_str_free(temporary);

    return ret;
}
