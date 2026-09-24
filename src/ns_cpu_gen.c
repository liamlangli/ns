#include "ns_cpu.h"
#include "ns_cpu_isa.h"

// Errors come back to the caller without the debug build's print-and-exit
// (ns_return_error): a host that hot-loads an image has to survive rejecting
// it, and a program fault. ns_cpu reports the reason on stderr itself.
#define NS_CPU_ERROR(t, l, err, m) ((ns_return_##t){.s = (err), .e = {.msg = ns_str_cstr(m), .loc = (l)}})
#include "ns_ssa.h"

/*
 * SSA → ns_cpu image (doc/cpu.md).
 *
 * The lowering follows the AMD64 backend (src/ns_amd64.c) decision for decision,
 * so an interpreted image and a native build agree: the same runtime helper for
 * the same instruction, the same width for every load and store, the same
 * treatment of signedness and of floats kept as bit patterns. Only the target
 * differs. Where AMD64 gives each SSA value a stack slot, ns_cpu gives it a
 * register of the frame, and where AMD64 re-materializes a constant each time it
 * is reached, ns_cpu keeps constants in registers the loader fills on entry, so
 * a constant costs nothing inside a loop.
 */

typedef struct ns_cpu_gen_const {
    u8 kind;
    u64 value;
} ns_cpu_gen_const;

typedef struct ns_cpu_gen_fixup {
    u32 at;     // code offset of the u32 target operand
    i32 label;  // block index, or NS_CPU_GEN_STUB + stub index
} ns_cpu_gen_fixup;

typedef struct ns_cpu_gen_stub {
    i32 from, to;
} ns_cpu_gen_stub;

typedef struct ns_cpu_gen_ffi {
    u32 module, name;
    u8 ret;
    u8 *params;
} ns_cpu_gen_ffi;

#define NS_CPU_GEN_STUB 0x40000000

typedef struct ns_cpu_gen {
    ns_ssa_module *ssa;
    ns_str error;

    // module tables
    ns_str *strings;
    u32 *rt_names;
    ns_cpu_gen_ffi *ffi;
    u32 ncache;
    u32 *locs; // (file string, line) pairs named by ASSERT and TRAP

    // current function
    ns_ssa_fn *fn;
    i32 nvalues;     // SSA value ids are below this
    i32 *reg_of;     // value id -> register, -1 when never defined
    ns_type *type_of;
    i32 *uses;
    i32 *def_inst;   // value id -> defining instruction index
    ns_bool *fused;  // instruction index -> folded into the branch after it
    ns_bool *coalesced; // instruction index -> a COPY sharing its source's register
    i32 nparams;
    i32 nregs;       // parameters, values, scratch, tmp and sink
    i32 scratch;     // first of the phi scratch registers
    i32 tmp;         // two-step sequences (string compare)
    i32 sink;        // results nobody reads
    ns_cpu_gen_const *consts;
    u16 *code;
    i32 *block_at;
    ns_cpu_gen_fixup *fixups;
    ns_cpu_gen_stub *stubs;
    i32 cur_block;
} ns_cpu_gen;

/* ── byte writer ──────────────────────────────────────────────────────────── */
static void ns_cpu_w8(u8 **out, u8 v) { ns_array_push(*out, v); }
static void ns_cpu_w16(u8 **out, u16 v) { ns_cpu_w8(out, (u8)v); ns_cpu_w8(out, (u8)(v >> 8)); }
static void ns_cpu_w32(u8 **out, u32 v) { ns_cpu_w16(out, (u16)v); ns_cpu_w16(out, (u16)(v >> 16)); }
static void ns_cpu_w64(u8 **out, u64 v) { ns_cpu_w32(out, (u32)v); ns_cpu_w32(out, (u32)(v >> 32)); }

static void ns_cpu_gen_fail(ns_cpu_gen *g, const char *fmt, ns_str a) {
    if (g->error.len > 0) return;
    char buf[512];
    snprintf(buf, sizeof(buf), fmt, a.len, a.data ? a.data : "");
    i32 len = (i32)strlen(buf);
    i8 *copy = ns_malloc((szt)len + 1);
    memcpy(copy, buf, (szt)len + 1);
    g->error = (ns_str){.data = copy, .len = len, .dynamic = 1};
}

// The pool keeps its own copy of every entry: names belong to the SSA module.
static u32 ns_cpu_gen_string(ns_cpu_gen *g, ns_str s) {
    for (i32 i = 0, l = (i32)ns_array_length(g->strings); i < l; ++i) {
        if (ns_str_equals(g->strings[i], s)) return (u32)i;
    }
    i8 *copy = ns_malloc((szt)s.len + 1);
    if (s.len > 0) memcpy(copy, s.data, (szt)s.len);
    copy[s.len] = 0;
    ns_array_push(g->strings, ((ns_str){.data = copy, .len = s.len, .dynamic = 1}));
    return (u32)ns_array_length(g->strings) - 1;
}

static u16 ns_cpu_gen_rt(ns_cpu_gen *g, const char *name) {
    u32 id = ns_cpu_gen_string(g, ns_str_cstr((i8 *)name));
    for (i32 i = 0, l = (i32)ns_array_length(g->rt_names); i < l; ++i) {
        if (g->rt_names[i] == id) return (u16)i;
    }
    ns_array_push(g->rt_names, id);
    return (u16)(ns_array_length(g->rt_names) - 1);
}

/* ── constants ────────────────────────────────────────────────────────────── */
// Same literal grammar as ns_amd64_parse_u64: booleans, nil, signed decimal,
// hex, integer suffixes, and floats that hold an exact integer.
static ns_bool ns_cpu_parse_u64(ns_str s, u64 *out) {
    if (s.len <= 0 || !s.data) return false;
    if (ns_str_equals_STR(s, "true")) { *out = 1; return true; }
    if (ns_str_equals_STR(s, "false")) { *out = 0; return true; }
    if (ns_str_equals_STR(s, "nil")) { *out = 0; return true; }
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
        if (suf == 'u' || suf == 'U' || suf == 'i' || suf == 'I' || suf == 'l' || suf == 'L') {
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
            u64 d = v * 10u + (u64)(s.data[i] - '0');
            if (d < v) return false;
            v = d;
        }
        *out = neg ? (u64)(-(i64)v) : v;
        return true;
    }
    f64 fv = ns_str_to_f64(s);
    f64 pos = fv < 0.0 ? -fv : fv;
    u64 iv = (u64)pos;
    if ((f64)iv != pos) return false;
    *out = fv < 0.0 ? (u64)(-(i64)iv) : iv;
    return true;
}

static i32 ns_cpu_gen_const_reg(ns_cpu_gen *g, u8 kind, u64 value) {
    i32 n = (i32)ns_array_length(g->consts);
    for (i32 i = 0; i < n; ++i) {
        if (g->consts[i].kind == kind && g->consts[i].value == value) return g->nregs + i;
    }
    ns_array_push(g->consts, ((ns_cpu_gen_const){.kind = kind, .value = value}));
    return g->nregs + n;
}

static i32 ns_cpu_gen_imm(ns_cpu_gen *g, u64 value) {
    return ns_cpu_gen_const_reg(g, NS_CPU_CONST_BITS, value);
}

static void ns_cpu_gen_const_value(ns_cpu_gen *g, ns_ssa_inst *inst, u8 *kind, u64 *value) {
    *kind = NS_CPU_CONST_BITS;
    *value = 0;
    if (inst->op == NS_SSA_OP_UNDEF) return;
    if (ns_type_is(inst->type, NS_TYPE_STRING)) {
        ns_str bytes = ns_str_unescape(inst->name);
        *kind = NS_CPU_CONST_STRING;
        *value = ns_cpu_gen_string(g, bytes);
        ns_str_free(bytes);
        return;
    }
    if (ns_type_is(inst->type, NS_TYPE_F64)) {
        f64 fv = ns_str_to_f64(inst->name);
        memcpy(value, &fv, 8);
        return;
    }
    if (ns_type_is(inst->type, NS_TYPE_F32)) {
        f32 fv = (f32)ns_str_to_f64(inst->name);
        u32 bits = 0;
        memcpy(&bits, &fv, 4);
        *value = bits;
        return;
    }
    if (!ns_cpu_parse_u64(inst->name, value)) {
        ns_warn("cpu", "unsupported const '%.*s' in fn %.*s, using 0\n",
                inst->name.len, inst->name.data, g->fn->name.len, g->fn->name.data);
        *value = 0;
    }
}

/* ── code emission ────────────────────────────────────────────────────────── */
static void ns_cpu_gen_u16(ns_cpu_gen *g, i32 v) { ns_array_push(g->code, (u16)v); }

static void ns_cpu_gen_u32(ns_cpu_gen *g, u32 v) {
    ns_cpu_gen_u16(g, (i32)(v & 0xffff));
    ns_cpu_gen_u16(g, (i32)(v >> 16));
}

static void ns_cpu_gen_op(ns_cpu_gen *g, ns_cpu_op op) { ns_cpu_gen_u16(g, op); }

static i32 ns_cpu_gen_reg(ns_cpu_gen *g, i32 value) {
    if (value < 0 || value >= g->nvalues || g->reg_of[value] < 0) {
        // A value no instruction defines reads as zero, like an UNDEF.
        return ns_cpu_gen_imm(g, 0);
    }
    return g->reg_of[value];
}

static i32 ns_cpu_gen_dst(ns_cpu_gen *g, i32 value) {
    if (value < 0 || value >= g->nvalues || g->reg_of[value] < 0) return g->sink;
    return g->reg_of[value];
}

static void ns_cpu_gen_rrr(ns_cpu_gen *g, ns_cpu_op op, i32 d, i32 a, i32 b) {
    ns_cpu_gen_op(g, op);
    ns_cpu_gen_u16(g, d);
    ns_cpu_gen_u16(g, a);
    ns_cpu_gen_u16(g, b);
}

static void ns_cpu_gen_rr(ns_cpu_gen *g, ns_cpu_op op, i32 d, i32 a) {
    ns_cpu_gen_op(g, op);
    ns_cpu_gen_u16(g, d);
    ns_cpu_gen_u16(g, a);
}

static void ns_cpu_gen_mov(ns_cpu_gen *g, i32 d, i32 a) {
    if (d == a) return;
    ns_cpu_gen_rr(g, NS_CPU_MOV, d, a);
}

static void ns_cpu_gen_target(ns_cpu_gen *g, i32 label) {
    ns_cpu_gen_fixup f = {.at = (u32)ns_array_length(g->code), .label = label};
    ns_array_push(g->fixups, f);
    ns_cpu_gen_u32(g, 0);
}

static void ns_cpu_gen_rt_call(ns_cpu_gen *g, i32 d, const char *name, const i32 *args, i32 nargs) {
    ns_cpu_gen_op(g, NS_CPU_RT);
    ns_cpu_gen_u16(g, d);
    ns_cpu_gen_u16(g, ns_cpu_gen_rt(g, name));
    ns_cpu_gen_u16(g, nargs);
    for (i32 i = 0; i < nargs; ++i) ns_cpu_gen_u16(g, args[i]);
}

static ns_type ns_cpu_gen_type(ns_cpu_gen *g, i32 value) {
    if (value < 0 || value >= g->nvalues) return ns_type_unknown;
    return g->type_of[value];
}

static ns_bool ns_cpu_is_float(ns_type t) {
    return ns_type_is(t, NS_TYPE_F32) || ns_type_is(t, NS_TYPE_F64);
}

static ns_bool ns_cpu_is_inline_struct(ns_type t) {
    return ns_type_is(t, NS_TYPE_STRUCT) && !ns_type_is_array(t) && !ns_type_is_ref(t);
}

// ns_amd64_load_size: the width a scalar of type `t` occupies in memory.
static i32 ns_cpu_load_size(ns_type t) {
    if (ns_type_is(t, NS_TYPE_I8)) return 1;
    if (ns_type_is(t, NS_TYPE_U8)) return -1;
    if (ns_type_is(t, NS_TYPE_I16)) return 2;
    if (ns_type_is(t, NS_TYPE_U16)) return -2;
    if (ns_type_is_ref(t) || ns_type_is(t, NS_TYPE_ANY) || ns_type_is(t, NS_TYPE_I64) ||
        ns_type_is(t, NS_TYPE_U64) || ns_type_is(t, NS_TYPE_F64)) return 8;
    return 4;
}

// Narrow a register in place to an integer type, as `300 as u8` requires.
static void ns_cpu_gen_narrow(ns_cpu_gen *g, i32 d, i32 a, ns_type t) {
    ns_cpu_op op = NS_CPU_MOV;
    if (ns_type_is(t, NS_TYPE_I8)) op = NS_CPU_SEXT8;
    else if (ns_type_is(t, NS_TYPE_U8)) op = NS_CPU_ZEXT8;
    else if (ns_type_is(t, NS_TYPE_I16)) op = NS_CPU_SEXT16;
    else if (ns_type_is(t, NS_TYPE_U16)) op = NS_CPU_ZEXT16;
    else if (ns_type_is(t, NS_TYPE_I32)) op = NS_CPU_SEXT32;
    else if (ns_type_is(t, NS_TYPE_U32)) op = NS_CPU_ZEXT32;
    if (op == NS_CPU_MOV) ns_cpu_gen_mov(g, d, a);
    else ns_cpu_gen_rr(g, op, d, a);
}

/* ── phi edges ────────────────────────────────────────────────────────────── */
typedef struct ns_cpu_gen_copy {
    i32 dst, src;
} ns_cpu_gen_copy;

static i32 ns_cpu_gen_edge_copies(ns_cpu_gen *g, i32 from, i32 to, ns_cpu_gen_copy *out, i32 max) {
    ns_ssa_block *tb = &g->fn->blocks[to];
    i32 n = 0;
    for (i32 ii = 0, il = (i32)ns_array_length(tb->insts); ii < il; ++ii) {
        ns_ssa_inst *inst = &g->fn->insts[tb->insts[ii]];
        if (inst->op != NS_SSA_OP_PHI) break;
        i32 src = ns_ssa_phi_incoming(inst, from);
        if (src < 0 || inst->dst < 0 || src == inst->dst) continue;
        i32 d = ns_cpu_gen_dst(g, inst->dst);
        i32 s = ns_cpu_gen_reg(g, src);
        if (d == s || n >= max) continue;
        out[n++] = (ns_cpu_gen_copy){.dst = d, .src = s};
    }
    return n;
}

// Phis take their inputs all at once. When one copy would overwrite the input
// of another (a swap across a loop edge), go through the scratch registers.
static void ns_cpu_gen_emit_copies(ns_cpu_gen *g, ns_cpu_gen_copy *copies, i32 n) {
    ns_bool conflict = false;
    for (i32 i = 0; i < n && !conflict; ++i) {
        for (i32 j = 0; j < n; ++j) {
            if (i != j && copies[j].src == copies[i].dst) { conflict = true; break; }
        }
    }
    if (!conflict) {
        for (i32 i = 0; i < n; ++i) ns_cpu_gen_mov(g, copies[i].dst, copies[i].src);
        return;
    }
    for (i32 i = 0; i < n; ++i) ns_cpu_gen_mov(g, g->scratch + i, copies[i].src);
    for (i32 i = 0; i < n; ++i) ns_cpu_gen_mov(g, copies[i].dst, g->scratch + i);
}

static i32 ns_cpu_gen_phi_count(ns_ssa_fn *fn, i32 block) {
    ns_ssa_block *b = &fn->blocks[block];
    i32 n = 0;
    for (i32 ii = 0, il = (i32)ns_array_length(b->insts); ii < il; ++ii) {
        if (fn->insts[b->insts[ii]].op != NS_SSA_OP_PHI) break;
        n++;
    }
    return n;
}

// The label a branch from `from` to `to` jumps to: the block itself, or a stub
// that performs the edge's phi copies first.
static i32 ns_cpu_gen_edge_label(ns_cpu_gen *g, i32 from, i32 to) {
    i32 max = ns_cpu_gen_phi_count(g->fn, to);
    if (max == 0) return to;
    ns_cpu_gen_copy *copies = ns_malloc(sizeof(ns_cpu_gen_copy) * (szt)max);
    i32 n = ns_cpu_gen_edge_copies(g, from, to, copies, max);
    ns_free(copies);
    if (n == 0) return to;
    ns_array_push(g->stubs, ((ns_cpu_gen_stub){.from = from, .to = to}));
    return NS_CPU_GEN_STUB + (i32)ns_array_length(g->stubs) - 1;
}

static void ns_cpu_gen_edge_inline(ns_cpu_gen *g, i32 from, i32 to) {
    i32 max = ns_cpu_gen_phi_count(g->fn, to);
    if (max == 0) return;
    ns_cpu_gen_copy *copies = ns_malloc(sizeof(ns_cpu_gen_copy) * (szt)max);
    i32 n = ns_cpu_gen_edge_copies(g, from, to, copies, max);
    ns_cpu_gen_emit_copies(g, copies, n);
    ns_free(copies);
}

/* ── compares ─────────────────────────────────────────────────────────────── */
typedef struct ns_cpu_gen_cmp {
    ns_cpu_op set;    // the flag-producing op (EQ..FLE32)
    ns_cpu_op branch; // the fused branch op (BEQ..BFLE32)
    i32 a, b;
} ns_cpu_gen_cmp;

static ns_bool ns_cpu_is_cmp(ns_ssa_op op) {
    return op == NS_SSA_OP_EQ || op == NS_SSA_OP_NE || op == NS_SSA_OP_LT ||
           op == NS_SSA_OP_LE || op == NS_SSA_OP_GT || op == NS_SSA_OP_GE;
}

// Lower a compare to one of the ordered forms. GT and GE swap their operands,
// which also keeps an unordered float compare false, as UCOMIS* does natively.
// A string compare first calls ns_rt_strcmp into the tmp register.
static ns_cpu_gen_cmp ns_cpu_gen_compare(ns_cpu_gen *g, ns_ssa_inst *inst) {
    ns_type at = ns_cpu_gen_type(g, inst->a);
    ns_type bt = ns_cpu_gen_type(g, inst->b);
    ns_bool a_str = ns_type_is(at, NS_TYPE_STRING);
    ns_bool b_str = ns_type_is(bt, NS_TYPE_STRING);
    ns_cpu_gen_cmp c = {0};
    c.a = ns_cpu_gen_reg(g, inst->a);
    c.b = ns_cpu_gen_reg(g, inst->b);
    ns_bool swap = inst->op == NS_SSA_OP_GT || inst->op == NS_SSA_OP_GE;
    ns_bool or_eq = inst->op == NS_SSA_OP_LE || inst->op == NS_SSA_OP_GE;
    if ((ns_cpu_is_float(at) || ns_cpu_is_float(bt)) && !a_str && !b_str) {
        ns_bool f64 = ns_type_is(at, NS_TYPE_F64) || ns_type_is(bt, NS_TYPE_F64);
        switch (inst->op) {
        case NS_SSA_OP_EQ: c.set = f64 ? NS_CPU_FEQ64 : NS_CPU_FEQ32; break;
        case NS_SSA_OP_NE: c.set = f64 ? NS_CPU_FNE64 : NS_CPU_FNE32; break;
        default:
            if (or_eq) c.set = f64 ? NS_CPU_FLE64 : NS_CPU_FLE32;
            else c.set = f64 ? NS_CPU_FLT64 : NS_CPU_FLT32;
            break;
        }
    } else {
        if (a_str || b_str) {
            i32 args[2] = {c.a, c.b};
            ns_cpu_gen_rt_call(g, g->tmp, "ns_rt_strcmp", args, 2);
            c.a = g->tmp;
            c.b = ns_cpu_gen_imm(g, 0);
        }
        ns_bool uns = ns_type_unsigned(at) && !a_str;
        switch (inst->op) {
        case NS_SSA_OP_EQ: c.set = NS_CPU_EQ; break;
        case NS_SSA_OP_NE: c.set = NS_CPU_NE; break;
        default:
            if (or_eq) c.set = uns ? NS_CPU_LEU : NS_CPU_LES;
            else c.set = uns ? NS_CPU_LTU : NS_CPU_LTS;
            break;
        }
    }
    if (swap) {
        i32 t = c.a;
        c.a = c.b;
        c.b = t;
    }
    c.branch = (ns_cpu_op)(NS_CPU_BEQ + (c.set - NS_CPU_EQ));
    return c;
}

/* ── calls ────────────────────────────────────────────────────────────────── */
static const char *ns_cpu_map_std(ns_str module, ns_str name) {
    if (!ns_str_equals_STR(module, "std")) return NULL;
    static const char *const map[][2] = {
        {"print", "ns_rt_print"}, {"open", "ns_rt_open"}, {"read", "ns_rt_read"},
        {"write", "ns_rt_write"}, {"close", "ns_rt_close"}, {"sqrt", "ns_rt_sqrt"},
        {"sin", "ns_rt_sin"}, {"cos", "ns_rt_cos"}, {"tan", "ns_rt_tan"},
        {"atan2", "ns_rt_atan2"}, {"ftos", "ns_rt_ftos"}, {"stof", "ns_rt_stof"},
        {"substr", "ns_rt_substr"}, {"unescape", "ns_rt_unescape"}, {"utf8_len", "ns_rt_utf8_len"},
    };
    for (szt i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (ns_str_equals_STR(name, map[i][0])) return map[i][1];
    }
    return NULL;
}

static const char *ns_cpu_map_task(ns_str module, ns_str name) {
    if (!ns_str_equals_STR(module, "task")) return NULL;
    static const char *const map[][2] = {
        {"dispatch", "ns_rt_task_dispatch"}, {"wait", "ns_rt_task_wait"},
        {"cancel", "ns_rt_task_cancel"}, {"done", "ns_rt_task_done"},
        {"cancelled", "ns_rt_task_cancelled"}, {"sleep", "ns_rt_task_sleep"},
        {"queue_main", "ns_rt_queue_main"}, {"queue_worker", "ns_rt_queue_worker"},
        {"queue_idle", "ns_rt_queue_idle"},
    };
    for (szt i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (ns_str_equals_STR(name, map[i][0])) return map[i][1];
    }
    return NULL;
}

static ns_bool ns_cpu_is_ffi_module(ns_str module) {
    if (module.len == 0) return false;
    return !ns_str_equals_STR(module, "std") && !ns_str_equals_STR(module, "task") &&
           !ns_str_equals_STR(module, "simd") && !ns_str_equals_STR(module, "shader");
}

static i32 ns_cpu_gen_find_fn(ns_cpu_gen *g, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(g->ssa->fns); i < l; ++i) {
        if (ns_str_equals(g->ssa->fns[i].name, name)) return i;
    }
    return -1;
}

static ns_ssa_import *ns_cpu_gen_find_import(ns_cpu_gen *g, ns_str module, ns_str name) {
    for (i32 i = 0, l = (i32)ns_array_length(g->ssa->imports); i < l; ++i) {
        ns_ssa_import *im = &g->ssa->imports[i];
        if (ns_str_equals(im->module, module) && ns_str_equals(im->name, name)) return im;
    }
    return NULL;
}

static u8 ns_cpu_ffi_kind_of(ns_type t) {
    if (ns_type_is_array(t)) return NS_CPU_FFI_ARRAY;
    if (ns_type_is(t, NS_TYPE_STRING)) return NS_CPU_FFI_STR;
    if (ns_type_is_ref(t)) return NS_CPU_FFI_REF;
    switch (t.type) {
    case NS_TYPE_F32: return NS_CPU_FFI_F32;
    case NS_TYPE_F64: return NS_CPU_FFI_F64;
    case NS_TYPE_I8: return NS_CPU_FFI_I8;
    case NS_TYPE_U8: return NS_CPU_FFI_U8;
    case NS_TYPE_I16: return NS_CPU_FFI_I16;
    case NS_TYPE_U16: return NS_CPU_FFI_U16;
    case NS_TYPE_BOOL:
    case NS_TYPE_I32: return NS_CPU_FFI_I32;
    case NS_TYPE_U32: return NS_CPU_FFI_U32;
    case NS_TYPE_VOID:
    case NS_TYPE_INFER: return NS_CPU_FFI_VOID;
    default: return NS_CPU_FFI_I64;
    }
}

// The ARG instructions that directly precede a call in its block, in order.
static i32 ns_cpu_gen_call_args(ns_cpu_gen *g, i32 inst_pos, ns_ssa_inst *inst, i32 *args, i32 max) {
    ns_ssa_block *bb = &g->fn->blocks[g->cur_block];
    i32 nargs = inst->c > 0 ? inst->c : 0;
    if (nargs > max) nargs = max;
    i32 got = 0;
    for (i32 ii = inst_pos - 1; ii >= 0 && got < nargs; --ii) {
        ns_ssa_inst *pi = &g->fn->insts[bb->insts[ii]];
        if (pi->op != NS_SSA_OP_ARG) break;
        args[nargs - 1 - got] = pi->a;
        got++;
    }
    if (got < nargs) {
        // Fewer ARGs than the call counts: keep the ones found at the front.
        memmove(args, args + (nargs - got), sizeof(i32) * (szt)got);
        nargs = got;
    }
    return nargs;
}

#define NS_CPU_GEN_MAX_ARGS 255

static void ns_cpu_gen_call(ns_cpu_gen *g, i32 inst_pos, ns_ssa_inst *inst) {
    i32 args[NS_CPU_GEN_MAX_ARGS];
    i32 nargs = ns_cpu_gen_call_args(g, inst_pos, inst, args, NS_CPU_GEN_MAX_ARGS);
    i32 d = ns_cpu_gen_dst(g, inst->dst);
    ns_str name = inst->name;
    const char *std_rt = ns_cpu_map_std(inst->module, name);
    const char *task_rt = ns_cpu_map_task(inst->module, name);
    ns_bool indirect = name.len == 0 && inst->a >= 0 && !std_rt && !task_rt;

    if (indirect) {
        ns_cpu_gen_op(g, NS_CPU_CALLI);
        ns_cpu_gen_u16(g, d);
        ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, inst->a));
        ns_cpu_gen_u16(g, (i32)g->ncache++);
        ns_cpu_gen_u16(g, nargs);
        for (i32 i = 0; i < nargs; ++i) ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, args[i]));
        if (g->ncache > 0xffff) ns_cpu_gen_fail(g, "too many indirect calls in fn %.*s\n", g->fn->name);
        return;
    }

    if (ns_cpu_is_ffi_module(inst->module) && name.len > 0) {
        ns_ssa_import *im = ns_cpu_gen_find_import(g, inst->module, name);
        ns_cpu_gen_ffi site = {0};
        site.module = ns_cpu_gen_string(g, inst->module);
        site.name = ns_cpu_gen_string(g, name);
        ns_type ret = im ? im->ret : inst->type;
        site.ret = ns_cpu_ffi_kind_of(ret);
        // An array result is a handle native code produced: take it as is.
        if (ns_type_is_array(ret) || ns_type_is_ref(ret)) site.ret = NS_CPU_FFI_I64;
        for (i32 i = 0; i < nargs; ++i) {
            ns_type at = im && i < (i32)ns_array_length(im->params) ? im->params[i]
                                                                    : ns_cpu_gen_type(g, args[i]);
            ns_array_push(site.params, ns_cpu_ffi_kind_of(at));
            if (site.params[i] == NS_CPU_FFI_VOID) site.params[i] = NS_CPU_FFI_I64;
        }
        i32 index = -1;
        for (i32 i = 0, l = (i32)ns_array_length(g->ffi); i < l && index < 0; ++i) {
            ns_cpu_gen_ffi *f = &g->ffi[i];
            if (f->module != site.module || f->name != site.name || f->ret != site.ret) continue;
            if (ns_array_length(f->params) != ns_array_length(site.params)) continue;
            if (nargs == 0 || memcmp(f->params, site.params, (szt)nargs) == 0) index = i;
        }
        if (index < 0) {
            ns_array_push(g->ffi, site);
            index = (i32)ns_array_length(g->ffi) - 1;
        } else {
            ns_array_free(site.params);
        }
        ns_cpu_gen_op(g, NS_CPU_FFI);
        ns_cpu_gen_u16(g, d);
        ns_cpu_gen_u16(g, index);
        ns_cpu_gen_u16(g, nargs);
        for (i32 i = 0; i < nargs; ++i) ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, args[i]));
        return;
    }

    const char *rt = std_rt ? std_rt : task_rt;
    if (!rt) {
        i32 callee = ns_cpu_gen_find_fn(g, name);
        if (callee >= 0) {
            ns_cpu_gen_op(g, NS_CPU_CALL);
            ns_cpu_gen_u16(g, d);
            ns_cpu_gen_u16(g, callee);
            ns_cpu_gen_u16(g, nargs);
            for (i32 i = 0; i < nargs; ++i) ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, args[i]));
            return;
        }
    }
    if (!rt && name.len > 6 && strncmp(name.data, "ns_rt_", 6) == 0) {
        // ns_rt_* helpers are looked up by name when the image loads.
        char buf[128];
        snprintf(buf, sizeof(buf), "%.*s", name.len, name.data);
        i32 regs[NS_CPU_GEN_MAX_ARGS];
        for (i32 i = 0; i < nargs; ++i) regs[i] = ns_cpu_gen_reg(g, args[i]);
        ns_cpu_gen_rt_call(g, d, buf, regs, nargs);
        return;
    }
    if (!rt) {
        ns_cpu_gen_fail(g, "unresolved call to %.*s\n", name);
        return;
    }
    i32 regs[NS_CPU_GEN_MAX_ARGS];
    for (i32 i = 0; i < nargs; ++i) regs[i] = ns_cpu_gen_reg(g, args[i]);
    ns_cpu_gen_rt_call(g, d, rt, regs, nargs);
}

/* ── instructions ─────────────────────────────────────────────────────────── */
// The source location of an instruction, as an index into the image's
// location table: the file and line a fault reports.
static u32 ns_cpu_gen_loc(ns_cpu_gen *g, ns_ssa_inst *inst) {
    ns_ast_ctx *ctx = g->ssa->ctx;
    ns_code_loc loc = {0};
    if (ctx && inst->ast >= 0 && inst->ast < (i32)ns_array_length(ctx->nodes)) {
        loc = ns_ast_state_loc(ctx, ctx->nodes[inst->ast].state);
    } else if (ctx && inst->token.line > 0) {
        loc = ns_ctx_loc(ctx, inst->token.line, 0);
    }
    u32 file = ns_cpu_gen_string(g, loc.f.data ? loc.f : ns_str_cstr(""));
    u32 line = loc.l > 0 ? (u32)loc.l : 0;
    for (i32 i = 0, l = (i32)ns_array_length(g->locs); i + 1 < l; i += 2) {
        if (g->locs[i] == file && g->locs[i + 1] == line) return (u32)i / 2;
    }
    ns_array_push(g->locs, file);
    ns_array_push(g->locs, line);
    return (u32)ns_array_length(g->locs) / 2 - 1;
}

static void ns_cpu_gen_inst(ns_cpu_gen *g, i32 pos, i32 index) {
    ns_ssa_inst *inst = &g->fn->insts[index];
    if (g->fused[index]) return;
    switch (inst->op) {
    case NS_SSA_OP_PHI:
    case NS_SSA_OP_UNDEF:
    case NS_SSA_OP_CONST:
    case NS_SSA_OP_PARAM:
    case NS_SSA_OP_ARG:
        break;
    case NS_SSA_OP_COPY:
        if (inst->dst < 0 || inst->a < 0 || g->coalesced[index]) break;
        ns_cpu_gen_mov(g, ns_cpu_gen_dst(g, inst->dst), ns_cpu_gen_reg(g, inst->a));
        break;
    case NS_SSA_OP_CAST: {
        if (inst->dst < 0 || inst->a < 0) break;
        i32 d = ns_cpu_gen_dst(g, inst->dst);
        i32 a = ns_cpu_gen_reg(g, inst->a);
        ns_type src_t = ns_cpu_gen_type(g, inst->a);
        ns_bool src_f = ns_cpu_is_float(src_t);
        ns_bool dst_f = ns_cpu_is_float(inst->type);
        ns_bool dst64 = ns_type_is(inst->type, NS_TYPE_F64);
        ns_bool src64 = ns_type_is(src_t, NS_TYPE_F64);
        if (dst_f && src_f) {
            if (src64 == dst64) ns_cpu_gen_mov(g, d, a);
            else ns_cpu_gen_rr(g, src64 ? NS_CPU_F64TO32 : NS_CPU_F32TO64, d, a);
        } else if (dst_f) {
            ns_cpu_gen_rr(g, dst64 ? NS_CPU_I2F64 : NS_CPU_I2F32, d, a);
        } else if (src_f) {
            ns_cpu_gen_rr(g, src64 ? NS_CPU_F64TOI : NS_CPU_F32TOI, d, a);
            ns_cpu_gen_narrow(g, d, d, inst->type);
        } else {
            ns_cpu_gen_narrow(g, d, a, inst->type);
        }
    } break;
    case NS_SSA_OP_ADD:
    case NS_SSA_OP_SUB:
    case NS_SSA_OP_MUL:
    case NS_SSA_OP_DIV:
    case NS_SSA_OP_MOD:
    case NS_SSA_OP_BAND: case NS_SSA_OP_AND:
    case NS_SSA_OP_BOR: case NS_SSA_OP_OR:
    case NS_SSA_OP_BXOR:
    case NS_SSA_OP_SHL: case NS_SSA_OP_SHR: {
        if (inst->dst < 0) break;
        i32 d = ns_cpu_gen_dst(g, inst->dst);
        i32 a = ns_cpu_gen_reg(g, inst->a);
        i32 b = ns_cpu_gen_reg(g, inst->b);
        ns_type at = ns_cpu_gen_type(g, inst->a);
        if (inst->op == NS_SSA_OP_ADD &&
            (ns_type_is(inst->type, NS_TYPE_STRING) || ns_type_is(at, NS_TYPE_STRING))) {
            i32 args[2] = {a, b};
            ns_cpu_gen_rt_call(g, d, "ns_rt_strcat", args, 2);
            break;
        }
        if (ns_cpu_is_float(inst->type) &&
            (inst->op == NS_SSA_OP_ADD || inst->op == NS_SSA_OP_SUB ||
             inst->op == NS_SSA_OP_MUL || inst->op == NS_SSA_OP_DIV)) {
            ns_bool f64 = ns_type_is(inst->type, NS_TYPE_F64);
            ns_cpu_op op = inst->op == NS_SSA_OP_ADD ? NS_CPU_FADD64 :
                           inst->op == NS_SSA_OP_SUB ? NS_CPU_FSUB64 :
                           inst->op == NS_SSA_OP_MUL ? NS_CPU_FMUL64 : NS_CPU_FDIV64;
            if (!f64) op = (ns_cpu_op)(op + (NS_CPU_FADD32 - NS_CPU_FADD64));
            ns_cpu_gen_rrr(g, op, d, a, b);
            break;
        }
        if (ns_cpu_is_float(inst->type) && inst->op == NS_SSA_OP_MOD) {
            i32 args[2] = {a, b};
            ns_cpu_gen_rt_call(g, d, ns_type_is(inst->type, NS_TYPE_F32) ? "ns_rt_fmodf" : "ns_rt_fmod", args, 2);
            break;
        }
        ns_bool uns = ns_type_unsigned(at);
        ns_cpu_op op = NS_CPU_ADD;
        switch (inst->op) {
        case NS_SSA_OP_ADD: op = NS_CPU_ADD; break;
        case NS_SSA_OP_SUB: op = NS_CPU_SUB; break;
        case NS_SSA_OP_MUL: op = NS_CPU_MUL; break;
        case NS_SSA_OP_DIV: op = uns ? NS_CPU_DIVU : NS_CPU_DIVS; break;
        case NS_SSA_OP_MOD: op = uns ? NS_CPU_MODU : NS_CPU_MODS; break;
        case NS_SSA_OP_BAND: case NS_SSA_OP_AND: op = NS_CPU_AND; break;
        case NS_SSA_OP_BOR: case NS_SSA_OP_OR: op = NS_CPU_OR; break;
        case NS_SSA_OP_BXOR: op = NS_CPU_XOR; break;
        case NS_SSA_OP_SHL: op = NS_CPU_SHL; break;
        default: op = uns ? NS_CPU_SHRU : NS_CPU_SHRS; break;
        }
        ns_cpu_gen_rrr(g, op, d, a, b);
    } break;
    case NS_SSA_OP_NEG: {
        if (inst->dst < 0) break;
        ns_type nt = ns_cpu_is_float(inst->type) ? inst->type : ns_cpu_gen_type(g, inst->a);
        ns_cpu_op op = !ns_cpu_is_float(nt) ? NS_CPU_NEG :
                       ns_type_is(nt, NS_TYPE_F64) ? NS_CPU_FNEG64 : NS_CPU_FNEG32;
        ns_cpu_gen_rr(g, op, ns_cpu_gen_dst(g, inst->dst), ns_cpu_gen_reg(g, inst->a));
    } break;
    case NS_SSA_OP_NOT:
        if (inst->dst < 0) break;
        ns_cpu_gen_rr(g, NS_CPU_NOT, ns_cpu_gen_dst(g, inst->dst), ns_cpu_gen_reg(g, inst->a));
        break;
    case NS_SSA_OP_EQ: case NS_SSA_OP_NE:
    case NS_SSA_OP_LT: case NS_SSA_OP_LE:
    case NS_SSA_OP_GT: case NS_SSA_OP_GE: {
        if (inst->dst < 0) break;
        ns_cpu_gen_cmp c = ns_cpu_gen_compare(g, inst);
        ns_cpu_gen_rrr(g, c.set, ns_cpu_gen_dst(g, inst->dst), c.a, c.b);
    } break;
    case NS_SSA_OP_CALL:
        ns_cpu_gen_call(g, pos, inst);
        break;
    case NS_SSA_OP_BR: {
        i32 cond_def = inst->a >= 0 && inst->a < g->nvalues ? g->def_inst[inst->a] : -1;
        i32 t0 = ns_cpu_gen_edge_label(g, g->cur_block, inst->target0);
        i32 t1 = ns_cpu_gen_edge_label(g, g->cur_block, inst->target1);
        if (cond_def >= 0 && g->fused[cond_def]) {
            ns_cpu_gen_cmp c = ns_cpu_gen_compare(g, &g->fn->insts[cond_def]);
            ns_cpu_gen_op(g, c.branch);
            ns_cpu_gen_u16(g, c.a);
            ns_cpu_gen_u16(g, c.b);
        } else {
            ns_cpu_gen_op(g, NS_CPU_BNZ);
            ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, inst->a));
        }
        ns_cpu_gen_target(g, t0);
        ns_cpu_gen_target(g, t1);
    } break;
    case NS_SSA_OP_JMP: {
        ns_cpu_gen_edge_inline(g, g->cur_block, inst->target0);
        if (inst->target0 == g->cur_block + 1) break; // falls through
        ns_cpu_gen_op(g, NS_CPU_JMP);
        ns_cpu_gen_target(g, inst->target0);
    } break;
    case NS_SSA_OP_RET:
        ns_cpu_gen_op(g, NS_CPU_RET);
        ns_cpu_gen_u16(g, inst->a >= 0 ? ns_cpu_gen_reg(g, inst->a) : ns_cpu_gen_imm(g, 0));
        break;
    case NS_SSA_OP_ASSERT:
        if (inst->a < 0) break;
        ns_cpu_gen_op(g, NS_CPU_ASSERT);
        ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, inst->a));
        ns_cpu_gen_u32(g, ns_cpu_gen_loc(g, inst));
        break;
    case NS_SSA_OP_TRAP:
        ns_cpu_gen_op(g, NS_CPU_TRAP);
        ns_cpu_gen_u32(g, ns_cpu_gen_loc(g, inst));
        break;
    case NS_SSA_OP_GLOBAL_GET:
        if (inst->dst < 0 || inst->c < 0) break;
        ns_cpu_gen_op(g, NS_CPU_GGET);
        ns_cpu_gen_u16(g, ns_cpu_gen_dst(g, inst->dst));
        ns_cpu_gen_u16(g, inst->c);
        break;
    case NS_SSA_OP_GLOBAL_SET:
        if (inst->c < 0) break;
        ns_cpu_gen_op(g, NS_CPU_GSET);
        ns_cpu_gen_u16(g, inst->c);
        ns_cpu_gen_u16(g, ns_cpu_gen_reg(g, inst->a));
        break;
    case NS_SSA_OP_ALLOC: {
        if (inst->dst < 0) break;
        i32 args[1] = {ns_cpu_gen_imm(g, (u64)(u32)(inst->c > 0 ? inst->c : 0))};
        ns_cpu_gen_rt_call(g, ns_cpu_gen_dst(g, inst->dst), "ns_rt_alloc", args, 1);
    } break;
    case NS_SSA_OP_CLONE: {
        if (inst->dst < 0) break;
        i32 args[2] = {ns_cpu_gen_reg(g, inst->a), ns_cpu_gen_imm(g, (u64)(u32)(inst->c > 0 ? inst->c : 0))};
        ns_cpu_gen_rt_call(g, ns_cpu_gen_dst(g, inst->dst), "ns_rt_clone", args, 2);
    } break;
    case NS_SSA_OP_SCOPE_ENTER:
        if (inst->dst < 0) break;
        ns_cpu_gen_rt_call(g, ns_cpu_gen_dst(g, inst->dst), "ns_rt_scope_enter", ns_null, 0);
        break;
    case NS_SSA_OP_SCOPE_LEAVE: {
        i32 args[3] = {ns_cpu_gen_reg(g, inst->a),
                       inst->b >= 0 ? ns_cpu_gen_reg(g, inst->b) : ns_cpu_gen_imm(g, 0),
                       ns_cpu_gen_imm(g, (u64)(u32)(inst->c > 0 ? inst->c : 0))};
        ns_cpu_gen_rt_call(g, ns_cpu_gen_dst(g, inst->dst), "ns_rt_scope_leave", args, 3);
    } break;
    case NS_SSA_OP_PIN: {
        if (inst->target0 == 3 || inst->a < 0) {
            ns_cpu_gen_rt_call(g, g->sink, "ns_rt_pin_all", ns_null, 0);
            break;
        }
        i32 args[2] = {ns_cpu_gen_reg(g, inst->a), ns_cpu_gen_imm(g, (u64)(u32)(inst->c > 0 ? inst->c : 0))};
        if (inst->target0 == 1) ns_cpu_gen_rt_call(g, g->sink, "ns_rt_pin_str", args, 1);
        else ns_cpu_gen_rt_call(g, g->sink, inst->target0 == 2 ? "ns_rt_pin_array" : "ns_rt_pin", args, 2);
    } break;
    case NS_SSA_OP_LOAD: {
        if (inst->dst < 0) break;
        i32 d = ns_cpu_gen_dst(g, inst->dst);
        i32 a = ns_cpu_gen_reg(g, inst->a);
        if (ns_cpu_is_inline_struct(inst->type)) {
            // A struct field is addressed, not loaded.
            ns_cpu_gen_rr(g, NS_CPU_ADDI, d, a);
            ns_cpu_gen_u32(g, (u32)inst->c);
            break;
        }
        i32 size = inst->target0 > 0 ? inst->target0 : ns_cpu_load_size(inst->type);
        ns_cpu_op op = size == 1 ? NS_CPU_LD8S : size == -1 ? NS_CPU_LD8U :
                       size == 2 ? NS_CPU_LD16S : size == -2 ? NS_CPU_LD16U :
                       size == 8 ? NS_CPU_LD64 : NS_CPU_LD32;
        ns_cpu_gen_rr(g, op, d, a);
        ns_cpu_gen_u32(g, (u32)inst->c);
    } break;
    case NS_SSA_OP_STORE: {
        i32 size = inst->target0 > 0 ? inst->target0 : ns_cpu_load_size(inst->type);
        i32 a = ns_cpu_gen_reg(g, inst->a);
        i32 b = ns_cpu_gen_reg(g, inst->b);
        if (ns_cpu_is_inline_struct(inst->type) && size > 0) {
            ns_cpu_gen_op(g, NS_CPU_STCOPY);
            ns_cpu_gen_u16(g, a);
            ns_cpu_gen_u32(g, (u32)inst->c);
            ns_cpu_gen_u16(g, b);
            ns_cpu_gen_u32(g, (u32)size);
            break;
        }
        ns_cpu_op op = (size == 1 || size == -1) ? NS_CPU_ST8 : (size == 2 || size == -2) ? NS_CPU_ST16 :
                       size == 8 ? NS_CPU_ST64 : NS_CPU_ST32;
        ns_cpu_gen_op(g, op);
        ns_cpu_gen_u16(g, a);
        ns_cpu_gen_u32(g, (u32)inst->c);
        ns_cpu_gen_u16(g, b);
    } break;
    case NS_SSA_OP_ARRAY_NEW: {
        if (inst->dst < 0) break;
        i32 args[2] = {ns_cpu_gen_reg(g, inst->a), ns_cpu_gen_imm(g, (u64)(u32)(inst->c > 0 ? inst->c : 1))};
        ns_cpu_gen_rt_call(g, ns_cpu_gen_dst(g, inst->dst), "ns_rt_array_new", args, 2);
    } break;
    case NS_SSA_OP_ARRAY_STORE: {
        i32 stride = inst->c > 0 ? inst->c : 4;
        i32 arr = ns_cpu_gen_reg(g, inst->a);
        i32 idx = ns_cpu_gen_reg(g, inst->target0);
        i32 val = ns_cpu_gen_reg(g, inst->b);
        if (ns_cpu_is_inline_struct(inst->type) || stride > 8) {
            ns_cpu_gen_rrr(g, NS_CPU_ASTCOPY, arr, idx, val);
            ns_cpu_gen_u32(g, (u32)stride);
            break;
        }
        if (stride == 1 || stride == 2 || stride == 4 || stride == 8) {
            ns_cpu_op op = stride == 1 ? NS_CPU_AST8 : stride == 2 ? NS_CPU_AST16 :
                           stride == 4 ? NS_CPU_AST32 : NS_CPU_AST64;
            ns_cpu_gen_rrr(g, op, arr, idx, val);
            break;
        }
        i32 args[4] = {arr, idx, val, ns_cpu_gen_imm(g, (u64)stride)};
        ns_cpu_gen_rt_call(g, g->sink, "ns_rt_array_store", args, 4);
    } break;
    case NS_SSA_OP_INDEX: {
        if (inst->dst < 0) break;
        i64 stride = inst->c > 0 ? inst->c : 1;
        i32 d = ns_cpu_gen_dst(g, inst->dst);
        i32 a = ns_cpu_gen_reg(g, inst->a);
        i32 b = ns_cpu_gen_reg(g, inst->b);
        if (ns_type_is(inst->type, NS_TYPE_STRUCT) && !ns_type_is_array(inst->type)) {
            ns_cpu_gen_rrr(g, NS_CPU_ASLOT, d, a, b);
            ns_cpu_gen_u32(g, (u32)stride);
            break;
        }
        // A string byte index is emitted as i32 with stride 1; load it unsigned.
        if (stride == 1 && ns_type_is(inst->type, NS_TYPE_I32)) stride = -1;
        else if (ns_type_is(inst->type, NS_TYPE_U8)) stride = -1;
        else if (ns_type_is(inst->type, NS_TYPE_U16)) stride = -2;
        ns_cpu_op op = NS_CPU_NOP;
        switch (stride) {
        case -1: op = NS_CPU_AIDX8U; break;
        case -2: op = NS_CPU_AIDX16U; break;
        case 1: op = NS_CPU_AIDX8S; break;
        case 2: op = NS_CPU_AIDX16S; break;
        case 4: op = NS_CPU_AIDX32; break;
        case 8: op = NS_CPU_AIDX64; break;
        default: break;
        }
        if (op != NS_CPU_NOP) {
            ns_cpu_gen_rrr(g, op, d, a, b);
        } else if (stride > 8) {
            // ns_rt_array_index hands back the element address for wide strides.
            ns_cpu_gen_rrr(g, NS_CPU_ASLOT, d, a, b);
            ns_cpu_gen_u32(g, (u32)stride);
        } else {
            i32 args[3] = {a, b, ns_cpu_gen_imm(g, (u64)stride)};
            ns_cpu_gen_rt_call(g, d, "ns_rt_array_index", args, 3);
        }
    } break;
    case NS_SSA_OP_FNADDR: {
        if (inst->dst < 0) break;
        i32 callee = ns_cpu_gen_find_fn(g, inst->name);
        if (callee < 0) {
            ns_cpu_gen_fail(g, "address of unknown fn %.*s\n", inst->name);
            break;
        }
        ns_cpu_gen_op(g, NS_CPU_FNADDR);
        ns_cpu_gen_u16(g, ns_cpu_gen_dst(g, inst->dst));
        ns_cpu_gen_u16(g, callee);
    } break;
    case NS_SSA_OP_MEMBER:
        if (inst->dst < 0) break;
        ns_cpu_gen_rrr(g, NS_CPU_LDX64, ns_cpu_gen_dst(g, inst->dst),
                       ns_cpu_gen_reg(g, inst->a), ns_cpu_gen_reg(g, inst->b));
        break;
    default:
        ns_cpu_gen_fail(g, "unsupported ssa op in fn %.*s\n", g->fn->name);
        break;
    }
}

/* ── functions ────────────────────────────────────────────────────────────── */
static void ns_cpu_gen_count_use(ns_cpu_gen *g, i32 v) {
    if (v >= 0 && v < g->nvalues) g->uses[v]++;
}

static void ns_cpu_gen_analyze(ns_cpu_gen *g) {
    ns_ssa_fn *fn = g->fn;
    i32 ninst = (i32)ns_array_length(fn->insts);
    i32 max_dst = -1;
    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        if (in->dst > max_dst) max_dst = in->dst;
    }
    g->nvalues = max_dst + 1;
    g->reg_of = ns_malloc(sizeof(i32) * (szt)(g->nvalues + 1));
    g->type_of = ns_malloc(sizeof(ns_type) * (szt)(g->nvalues + 1));
    g->uses = ns_malloc(sizeof(i32) * (szt)(g->nvalues + 1));
    g->def_inst = ns_malloc(sizeof(i32) * (szt)(g->nvalues + 1));
    g->fused = ns_malloc(sizeof(ns_bool) * (szt)(ninst + 1));
    g->coalesced = ns_malloc(sizeof(ns_bool) * (szt)(ninst + 1));
    for (i32 v = 0; v < g->nvalues; ++v) {
        g->reg_of[v] = -1;
        g->type_of[v] = ns_type_unknown;
        g->uses[v] = 0;
        g->def_inst[v] = -1;
    }
    for (i32 i = 0; i < ninst; ++i) g->fused[i] = g->coalesced[i] = false;

    // The last definition of a value gives its type, as in the native backends.
    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        if (in->dst < 0) continue;
        g->type_of[in->dst] = in->type;
        g->def_inst[in->dst] = i;
    }

    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        switch (in->op) {
        case NS_SSA_OP_PHI:
            ns_cpu_gen_count_use(g, in->a);
            ns_cpu_gen_count_use(g, in->b);
            for (i32 e = 0, el = (i32)ns_array_length(in->phi_edges); e < el; ++e) {
                ns_cpu_gen_count_use(g, in->phi_edges[e].value);
            }
            break;
        case NS_SSA_OP_ARRAY_STORE:
            ns_cpu_gen_count_use(g, in->a);
            ns_cpu_gen_count_use(g, in->b);
            ns_cpu_gen_count_use(g, in->target0);
            break;
        case NS_SSA_OP_CONST:
        case NS_SSA_OP_PARAM:
        case NS_SSA_OP_UNDEF:
        case NS_SSA_OP_FNADDR:
        case NS_SSA_OP_GLOBAL_GET:
        case NS_SSA_OP_ALLOC:
        case NS_SSA_OP_SCOPE_ENTER:
            break;
        default:
            ns_cpu_gen_count_use(g, in->a);
            ns_cpu_gen_count_use(g, in->b);
            break;
        }
    }

    // A compare read only by the branch that ends its block becomes part of
    // that branch.
    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; ++bi) {
        ns_ssa_block *b = &fn->blocks[bi];
        i32 n = (i32)ns_array_length(b->insts);
        if (n == 0) continue;
        ns_ssa_inst *br = &fn->insts[b->insts[n - 1]];
        if (br->op != NS_SSA_OP_BR || br->a < 0 || br->a >= g->nvalues) continue;
        if (g->uses[br->a] != 1) continue;
        i32 def = g->def_inst[br->a];
        if (def < 0 || !ns_cpu_is_cmp(fn->insts[def].op)) continue;
        for (i32 ii = 0; ii < n - 1; ++ii) {
            if (b->insts[ii] == def) { g->fused[def] = true; break; }
        }
    }

    // Registers: parameters by position, then each other value, then the phi
    // scratch registers, the tmp register and the sink.
    i32 nparams = (i32)ns_array_length(fn->params);
    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        if (in->op == NS_SSA_OP_PARAM && in->c >= nparams) nparams = in->c + 1;
    }
    g->nparams = nparams;

    // A COPY names its source's register instead of taking one of its own when
    // both values are defined once. A register is written once per execution
    // of its definition, except a phi's, which the edges into its block write;
    // a copy of it is defined inside that block, after those writes, so no
    // path reaches a use of the copy across a rewrite of the phi.
    i32 *defs = ns_malloc(sizeof(i32) * (szt)(g->nvalues + 1));
    for (i32 v = 0; v < g->nvalues; ++v) defs[v] = 0;
    for (i32 i = 0; i < ninst; ++i) {
        if (fn->insts[i].dst >= 0) defs[fn->insts[i].dst]++;
    }
    i32 next = nparams;
    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        if (in->dst < 0 || g->reg_of[in->dst] >= 0) continue;
        if (in->op == NS_SSA_OP_PARAM && in->c >= 0) {
            g->reg_of[in->dst] = in->c;
        } else if (in->op == NS_SSA_OP_COPY && in->a >= 0 && in->a < g->nvalues &&
                   defs[in->dst] == 1 && defs[in->a] == 1) {
            continue; // resolved below, once its source has a register
        } else if (in->op != NS_SSA_OP_CONST && in->op != NS_SSA_OP_UNDEF) {
            g->reg_of[in->dst] = next++;
        }
    }
    // Coalesced copies take their source's register, through chains of copies.
    // A copy of a constant, of an undefined value, or of a value that has no
    // register for any other reason keeps a register of its own and a MOV.
    for (ns_bool changed = true; changed;) {
        changed = false;
        for (i32 i = 0; i < ninst; ++i) {
            ns_ssa_inst *in = &fn->insts[i];
            if (in->op != NS_SSA_OP_COPY || in->dst < 0 || g->reg_of[in->dst] >= 0) continue;
            if (in->a < 0 || in->a >= g->nvalues || g->reg_of[in->a] < 0) continue;
            g->reg_of[in->dst] = g->reg_of[in->a];
            g->coalesced[i] = true;
            changed = true;
        }
    }
    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        if (in->op == NS_SSA_OP_COPY && in->dst >= 0 && g->reg_of[in->dst] < 0) g->reg_of[in->dst] = next++;
    }
    i32 max_phis = 0;
    for (i32 bi = 0, bl = (i32)ns_array_length(fn->blocks); bi < bl; ++bi) {
        i32 n = ns_cpu_gen_phi_count(fn, bi);
        if (n > max_phis) max_phis = n;
    }
    g->scratch = next;
    next += max_phis;
    g->tmp = next++;
    g->sink = next++;
    g->nregs = next;

    // Constants live above the frame's own registers; the loader fills them.
    for (i32 i = 0; i < ninst; ++i) {
        ns_ssa_inst *in = &fn->insts[i];
        if (in->dst < 0 || g->reg_of[in->dst] >= 0) continue;
        if (in->op != NS_SSA_OP_CONST && in->op != NS_SSA_OP_UNDEF) continue;
        u8 kind;
        u64 value;
        ns_cpu_gen_const_value(g, in, &kind, &value);
        g->reg_of[in->dst] = ns_cpu_gen_const_reg(g, kind, value);
    }
    ns_free(defs);
}

static void ns_cpu_gen_fn_reset(ns_cpu_gen *g) {
    ns_free(g->reg_of);
    ns_free(g->type_of);
    ns_free(g->uses);
    ns_free(g->def_inst);
    ns_free(g->fused);
    ns_free(g->coalesced);
    g->coalesced = ns_null;
    g->reg_of = ns_null;
    g->type_of = ns_null;
    g->uses = ns_null;
    g->def_inst = ns_null;
    g->fused = ns_null;
    ns_array_free(g->consts);
    ns_array_free(g->code);
    ns_array_free(g->block_at);
    ns_array_free(g->fixups);
    ns_array_free(g->stubs);
}

static void ns_cpu_gen_fn(ns_cpu_gen *g, ns_ssa_fn *fn, u8 **out) {
    g->fn = fn;
    ns_cpu_gen_analyze(g);

    i32 nblocks = (i32)ns_array_length(fn->blocks);
    ns_array_set_length(g->block_at, nblocks);
    for (i32 bi = 0; bi < nblocks; ++bi) {
        g->cur_block = bi;
        g->block_at[bi] = (i32)ns_array_length(g->code);
        ns_ssa_block *b = &fn->blocks[bi];
        i32 n = (i32)ns_array_length(b->insts);
        for (i32 ii = 0; ii < n; ++ii) ns_cpu_gen_inst(g, ii, b->insts[ii]);
        ns_ssa_op last = n > 0 ? fn->insts[b->insts[n - 1]].op : NS_SSA_OP_UNKNOWN;
        ns_bool terminated = last == NS_SSA_OP_BR || last == NS_SSA_OP_JMP ||
                             last == NS_SSA_OP_RET || last == NS_SSA_OP_TRAP;
        if (!terminated) {
            // A block without a terminator runs into the next one, or returns
            // nothing at the end of the function.
            if (bi + 1 < nblocks) {
                ns_cpu_gen_edge_inline(g, bi, bi + 1);
            } else {
                ns_cpu_gen_op(g, NS_CPU_RET);
                ns_cpu_gen_u16(g, ns_cpu_gen_imm(g, 0));
            }
        }
    }
    if (nblocks == 0) {
        ns_cpu_gen_op(g, NS_CPU_RET);
        ns_cpu_gen_u16(g, ns_cpu_gen_imm(g, 0));
    }

    // Edge stubs: the phi copies of one edge, then the jump into the block.
    i32 *stub_at = ns_null;
    for (i32 si = 0; si < (i32)ns_array_length(g->stubs); ++si) {
        ns_array_push(stub_at, (i32)ns_array_length(g->code));
        ns_cpu_gen_stub st = g->stubs[si];
        ns_cpu_gen_edge_inline(g, st.from, st.to);
        ns_cpu_gen_op(g, NS_CPU_JMP);
        ns_cpu_gen_target(g, st.to);
    }
    for (i32 fi = 0, fl = (i32)ns_array_length(g->fixups); fi < fl; ++fi) {
        ns_cpu_gen_fixup f = g->fixups[fi];
        i32 at = 0;
        if (f.label >= NS_CPU_GEN_STUB) at = stub_at[f.label - NS_CPU_GEN_STUB];
        else if (f.label >= 0 && f.label < nblocks) at = g->block_at[f.label];
        else ns_cpu_gen_fail(g, "branch to a missing block in fn %.*s\n", fn->name);
        g->code[f.at] = (u16)((u32)at & 0xffff);
        g->code[f.at + 1] = (u16)((u32)at >> 16);
    }
    ns_array_free(stub_at);

    i32 nconst = (i32)ns_array_length(g->consts);
    if (g->nregs + nconst > NS_CPU_MAX_REGS) {
        ns_cpu_gen_fail(g, "fn %.*s needs more than 65534 registers\n", fn->name);
    }

    ns_cpu_w32(out, ns_cpu_gen_string(g, fn->name));
    ns_cpu_w16(out, (u16)g->nparams);
    ns_cpu_w16(out, (u16)g->nregs);
    ns_cpu_w16(out, (u16)nconst);
    ns_cpu_w32(out, (u32)ns_array_length(g->code));
    for (i32 i = 0; i < nconst; ++i) {
        ns_cpu_w8(out, g->consts[i].kind);
        ns_cpu_w64(out, g->consts[i].value);
    }
    for (i32 i = 0, l = (i32)ns_array_length(g->code); i < l; ++i) ns_cpu_w16(out, g->code[i]);

    ns_cpu_gen_fn_reset(g);
}

ns_return_ptr ns_cpu_image_from_ssa(void *ssa_ptr) {
    ns_ssa_module *ssa = (ns_ssa_module *)ssa_ptr;
    if (!ssa) return NS_CPU_ERROR(ptr, ns_code_loc_nil, NS_ERR_SYNTAX, "ssa module is null");

    ns_cpu_gen g = {0};
    g.ssa = ssa;
    // Functions are emitted into their own buffer first: they add to the string
    // pool and the helper tables, which the image lists ahead of them.
    u8 *fns = ns_null;
    i32 nfn = (i32)ns_array_length(ssa->fns);
    for (i32 i = 0; i < nfn && g.error.len == 0; ++i) ns_cpu_gen_fn(&g, &ssa->fns[i], &fns);

    i32 main_fn = ns_cpu_gen_find_fn(&g, ns_str_cstr("main"));
    i32 init_fn = ns_cpu_gen_find_fn(&g, ns_str_cstr("__module_init"));

    // Globals are listed by name and type so a hot update can carry them over.
    u32 *global_names = ns_null;
    u32 *global_types = ns_null;
    for (i32 i = 0, l = (i32)ns_array_length(ssa->globals); i < l; ++i) {
        ns_array_push(global_names, ns_cpu_gen_string(&g, ssa->globals[i].name));
        char tbuf[64];
        ns_type t = ssa->globals[i].type;
        snprintf(tbuf, sizeof(tbuf), "%d:%d:%d:%u", (int)t.type, (int)t.array, (int)t.ref, (unsigned)t.index);
        ns_array_push(global_types, ns_cpu_gen_string(&g, ns_str_cstr(tbuf)));
    }

    u8 *out = ns_null;
    if (g.error.len == 0) {
        const char *magic = NS_CPU_IMAGE_MAGIC;
        for (i32 i = 0; i < 5; ++i) ns_cpu_w8(&out, (u8)magic[i]);
        ns_cpu_w8(&out, 0);
        ns_cpu_w16(&out, NS_CPU_IMAGE_VERSION);

        ns_cpu_w32(&out, (u32)ns_array_length(g.strings));
        for (i32 i = 0, l = (i32)ns_array_length(g.strings); i < l; ++i) {
            ns_cpu_w32(&out, (u32)g.strings[i].len);
            for (i32 j = 0; j < g.strings[i].len; ++j) ns_cpu_w8(&out, (u8)g.strings[i].data[j]);
        }
        ns_cpu_w32(&out, (u32)ns_array_length(global_names));
        for (i32 i = 0, l = (i32)ns_array_length(global_names); i < l; ++i) {
            ns_cpu_w32(&out, global_names[i]);
            ns_cpu_w32(&out, global_types[i]);
        }
        ns_cpu_w32(&out, (u32)ns_array_length(g.rt_names));
        for (i32 i = 0, l = (i32)ns_array_length(g.rt_names); i < l; ++i) ns_cpu_w32(&out, g.rt_names[i]);
        ns_cpu_w32(&out, (u32)ns_array_length(g.ffi));
        for (i32 i = 0, l = (i32)ns_array_length(g.ffi); i < l; ++i) {
            ns_cpu_gen_ffi *f = &g.ffi[i];
            ns_cpu_w32(&out, f->module);
            ns_cpu_w32(&out, f->name);
            ns_cpu_w8(&out, f->ret);
            ns_cpu_w8(&out, (u8)ns_array_length(f->params));
            for (i32 j = 0, jl = (i32)ns_array_length(f->params); j < jl; ++j) ns_cpu_w8(&out, f->params[j]);
        }
        ns_cpu_w32(&out, g.ncache);
        ns_cpu_w32(&out, (u32)ns_array_length(g.locs) / 2);
        for (i32 i = 0, l = (i32)ns_array_length(g.locs); i < l; ++i) ns_cpu_w32(&out, g.locs[i]);
        ns_cpu_w32(&out, (u32)nfn);
        for (i32 i = 0, l = (i32)ns_array_length(fns); i < l; ++i) ns_cpu_w8(&out, fns[i]);
        ns_cpu_w32(&out, main_fn >= 0 ? (u32)main_fn : NS_CPU_NO_FN);
        ns_cpu_w32(&out, init_fn >= 0 ? (u32)init_fn : NS_CPU_NO_FN);
    }

    for (i32 i = 0, l = (i32)ns_array_length(g.strings); i < l; ++i) ns_str_free(g.strings[i]);
    ns_array_free(g.strings);
    ns_array_free(g.rt_names);
    for (i32 i = 0, l = (i32)ns_array_length(g.ffi); i < l; ++i) ns_array_free(g.ffi[i].params);
    ns_array_free(g.ffi);
    ns_array_free(g.locs);
    ns_array_free(fns);
    ns_array_free(global_names);
    ns_array_free(global_types);
    ns_cpu_gen_fn_reset(&g);

    if (g.error.len > 0) {
        fprintf(stderr, "cpu: %.*s", g.error.len, g.error.data);
        ns_str_free(g.error);
        ns_array_free(out);
        return NS_CPU_ERROR(ptr, ns_code_loc_nil, NS_ERR_BITCODE, "ns_cpu lowering failed");
    }
    return ns_return_ok(ptr, out);
}
