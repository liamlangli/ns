#include "ns_cpu.h"
#include "ns_cpu_isa.h"

// Errors come back to the caller without the debug build's print-and-exit
// (ns_return_error): a host that hot-loads an image has to survive rejecting
// it, and a program fault. ns_cpu reports the reason on stderr itself.
#define NS_CPU_ERROR(t, l, err, m) ((ns_return_##t){.s = (err), .e = {.msg = ns_str_cstr(m), .loc = (l)}})
#include "ns_native_rt.h"

#include <setjmp.h>
#include <stdarg.h>

#ifndef NS_XCLIB
#include <dlfcn.h>
#ifndef NS_DARWIN
#include <ffi.h>
#else
#include <ffi/ffi.h>
#endif
#define NS_CPU_HAS_FFI 1
#elif !defined(_WIN32)
#include <dlfcn.h>
#endif

/*
 * ns_cpu loader, verifier and interpreter (doc/cpu.md).
 *
 * A loaded function is its code (16-bit units), the size of its frame and the
 * constants the frame starts with. Calling one takes the next window of the
 * thread's register stack: the arguments land in the first registers, the
 * constants in the last ones, and the caller's return point goes on a separate
 * frame stack, so the interpreter never recurses on the C stack for a call.
 *
 * The heap is the ns_rt linear memory a native build uses, and every
 * instruction the native backends lower to an ns_rt_* call is either the same
 * call here or an inline equivalent (loads, stores and array accesses), which
 * is what keeps an image and a native executable in agreement.
 *
 * Native code sometimes calls ns code back: a task body, a view callback, a
 * dispatched closure. It does so through a code pointer it reads from a
 * function value, so FNADDR hands out a pointer to one of a fixed pool of C
 * thunks bound to the function. A thunk re-enters the interpreter on whatever
 * thread called it. The pool is compiled in, so no code is ever generated at
 * run time.
 */

/* ── operand formats ──────────────────────────────────────────────────────── */
static const char *const ns_cpu_op_names[NS_CPU_OP_COUNT] = {
#define NS_CPU_OP_NAME(name, fmt) #name,
    NS_CPU_OPS(NS_CPU_OP_NAME)
#undef NS_CPU_OP_NAME
};

static const char *const ns_cpu_op_formats[NS_CPU_OP_COUNT] = {
#define NS_CPU_OP_FMT(name, fmt) fmt,
    NS_CPU_OPS(NS_CPU_OP_FMT)
#undef NS_CPU_OP_FMT
};

/* ── module ───────────────────────────────────────────────────────────────── */
typedef struct ns_cpu_fn {
    u16 *code;
    u32 ncode;
    u16 nparams;
    u16 nregs;  // parameters, values and scratch
    u16 nconst; // constants, above nregs
    u32 frame;  // nregs + nconst
    u64 *consts;
    ns_str name;
    ns_cpu_module *module;
    i32 thunk; // index into the thunk pool, or -1
} ns_cpu_fn;

typedef struct ns_cpu_rt {
    const char *name;
    void *fn;
    u8 arity;
    u8 is_void;
} ns_cpu_rt;

typedef struct ns_cpu_ffi_site {
    void *fn;
    ns_str module, name;
    u8 ret;
    u8 nparams;
    u8 *params;
#ifdef NS_CPU_HAS_FFI
    ffi_cif cif;
    ffi_type **types;
#endif
} ns_cpu_ffi_site;

// The last function an indirect call site reached. One pointer, so a task on
// another thread reads either the old entry or the new one, never half of
// each; the call checks that the entry's thunk is the code it is calling.
typedef struct ns_cpu_cache {
    ns_cpu_fn *fn;
} ns_cpu_cache;

typedef struct ns_cpu_lib {
    ns_str module;
    void *handle;
} ns_cpu_lib;

struct ns_cpu_module {
    ns_str *strings;
    u32 nstrings;
    i64 *literals; // heap address of each string of the pool, materialized on use
    ns_cpu_fn *fns;
    u32 nfn;
    const ns_cpu_rt **rt;
    u32 nrt;
    ns_cpu_ffi_site *ffi;
    u32 nffi;
    ns_cpu_cache *cache;
    u32 ncache;
    u32 *locs; // file string, line
    u32 nlocs;
    u64 *globals;
    u32 nglobals;
    u32 *global_names;
    u32 *global_types;
    u32 main_fn, init_fn;
    ns_cpu_host host;
    ns_cpu_lib *libs;
};

/* ── runtime helpers ──────────────────────────────────────────────────────── */
#define NS_CPU_RT(name, arity, is_void) {#name, (void *)(uintptr_t)name, arity, is_void}
static const ns_cpu_rt ns_cpu_rt_table[] = {
    NS_CPU_RT(ns_rt_alloc, 1, 0),
    NS_CPU_RT(ns_rt_clone, 2, 0),
    NS_CPU_RT(ns_rt_scope_enter, 0, 0),
    NS_CPU_RT(ns_rt_scope_leave, 3, 0),
    NS_CPU_RT(ns_rt_pin, 2, 1),
    NS_CPU_RT(ns_rt_pin_str, 1, 1),
    NS_CPU_RT(ns_rt_pin_array, 2, 1),
    NS_CPU_RT(ns_rt_pin_all, 0, 1),
    NS_CPU_RT(ns_rt_load, 3, 0),
    NS_CPU_RT(ns_rt_store, 4, 1),
    NS_CPU_RT(ns_rt_copy, 4, 1),
    NS_CPU_RT(ns_rt_array_new, 2, 0),
    NS_CPU_RT(ns_rt_array_store, 4, 1),
    NS_CPU_RT(ns_rt_array_index, 3, 0),
    NS_CPU_RT(ns_rt_array_slot, 3, 0),
    NS_CPU_RT(ns_rt_print, 1, 0),
    NS_CPU_RT(ns_rt_strcat, 2, 0),
    NS_CPU_RT(ns_rt_strcmp, 2, 0),
    NS_CPU_RT(ns_rt_substr, 3, 0),
    NS_CPU_RT(ns_rt_unescape, 1, 0),
    NS_CPU_RT(ns_rt_utf8_len, 1, 0),
    NS_CPU_RT(ns_rt_itos, 1, 0),
    NS_CPU_RT(ns_rt_utos, 1, 0),
    NS_CPU_RT(ns_rt_btos, 1, 0),
    NS_CPU_RT(ns_rt_ftos, 1, 0),
    NS_CPU_RT(ns_rt_fmtf, 1, 0),
    NS_CPU_RT(ns_rt_stof, 1, 0),
    NS_CPU_RT(ns_rt_fmod, 2, 0),
    NS_CPU_RT(ns_rt_fmodf, 2, 0),
    NS_CPU_RT(ns_rt_sqrt, 1, 0),
    NS_CPU_RT(ns_rt_sin, 1, 0),
    NS_CPU_RT(ns_rt_cos, 1, 0),
    NS_CPU_RT(ns_rt_tan, 1, 0),
    NS_CPU_RT(ns_rt_atan2, 2, 0),
    NS_CPU_RT(ns_rt_map_new, 2, 0),
    NS_CPU_RT(ns_rt_map_get, 2, 0),
    NS_CPU_RT(ns_rt_map_set, 3, 1),
    NS_CPU_RT(ns_rt_map_has, 2, 0),
    NS_CPU_RT(ns_rt_map_insert, 2, 0),
    NS_CPU_RT(ns_rt_map_remove, 2, 0),
    NS_CPU_RT(ns_rt_map_slot_live, 2, 0),
    NS_CPU_RT(ns_rt_map_slot_key, 2, 0),
    NS_CPU_RT(ns_rt_open, 2, 0),
    NS_CPU_RT(ns_rt_read, 1, 0),
    NS_CPU_RT(ns_rt_write, 2, 0),
    NS_CPU_RT(ns_rt_close, 1, 1),
    NS_CPU_RT(ns_rt_union_new, 2, 0),
    NS_CPU_RT(ns_rt_union_as, 2, 0),
    NS_CPU_RT(ns_rt_to_cstr, 1, 0),
    NS_CPU_RT(ns_rt_from_cstr, 1, 0),
    NS_CPU_RT(ns_rt_native_ptr, 1, 0),
    NS_CPU_RT(ns_rt_array_ptr, 1, 0),
    NS_CPU_RT(ns_rt_callback, 1, 0),
    NS_CPU_RT(ns_rt_shader_host_bind, 4, 0),
    NS_CPU_RT(ns_rt_shader_host_bind_secondary, 1, 0),
    NS_CPU_RT(ns_rt_shader_host_root, 1, 0),
    NS_CPU_RT(ns_rt_shader_host_invocation, 3, 1),
    NS_CPU_RT(ns_rt_shader_host_swap, 0, 1),
    NS_CPU_RT(ns_rt_shader_host_release, 0, 1),
    NS_CPU_RT(ns_rt_shader_global_id, 1, 0),
    NS_CPU_RT(ns_rt_shader_root_f32, 1, 0),
    NS_CPU_RT(ns_rt_shader_read_texture, 2, 0),
    NS_CPU_RT(ns_rt_shader_write_texture, 4, 1),
    NS_CPU_RT(ns_rt_shader_source_hash, 1, 0),
    NS_CPU_RT(ns_rt_shader_unsupported, 1, 1),
    NS_CPU_RT(ns_rt_queue_main, 0, 0),
    NS_CPU_RT(ns_rt_queue_worker, 0, 0),
    NS_CPU_RT(ns_rt_queue_idle, 0, 0),
    NS_CPU_RT(ns_rt_task_spawn, 8, 0),
    NS_CPU_RT(ns_rt_task_dispatch, 2, 0),
    NS_CPU_RT(ns_rt_task_await, 1, 0),
    NS_CPU_RT(ns_rt_task_wait, 1, 1),
    NS_CPU_RT(ns_rt_task_cancel, 1, 1),
    NS_CPU_RT(ns_rt_task_done, 1, 0),
    NS_CPU_RT(ns_rt_task_cancelled, 1, 0),
    NS_CPU_RT(ns_rt_task_sleep, 1, 1),
};
#undef NS_CPU_RT

static const ns_cpu_rt *ns_cpu_rt_find(ns_str name) {
    for (szt i = 0; i < sizeof(ns_cpu_rt_table) / sizeof(ns_cpu_rt_table[0]); ++i) {
        if (ns_str_equals_STR(name, ns_cpu_rt_table[i].name)) return &ns_cpu_rt_table[i];
    }
    return ns_null;
}

typedef i64 (*ns_cpu_rt0)(void);
typedef i64 (*ns_cpu_rt1)(i64);
typedef i64 (*ns_cpu_rt2)(i64, i64);
typedef i64 (*ns_cpu_rt3)(i64, i64, i64);
typedef i64 (*ns_cpu_rt4)(i64, i64, i64, i64);
typedef i64 (*ns_cpu_rt8)(i64, i64, i64, i64, i64, i64, i64, i64);
typedef void (*ns_cpu_rtv0)(void);
typedef void (*ns_cpu_rtv1)(i64);
typedef void (*ns_cpu_rtv2)(i64, i64);
typedef void (*ns_cpu_rtv3)(i64, i64, i64);
typedef void (*ns_cpu_rtv4)(i64, i64, i64, i64);

static i64 ns_cpu_rt_invoke(const ns_cpu_rt *rt, const i64 *a) {
    if (rt->is_void) {
        switch (rt->arity) {
        case 0: ((ns_cpu_rtv0)rt->fn)(); break;
        case 1: ((ns_cpu_rtv1)rt->fn)(a[0]); break;
        case 2: ((ns_cpu_rtv2)rt->fn)(a[0], a[1]); break;
        case 3: ((ns_cpu_rtv3)rt->fn)(a[0], a[1], a[2]); break;
        default: ((ns_cpu_rtv4)rt->fn)(a[0], a[1], a[2], a[3]); break;
        }
        return 0;
    }
    switch (rt->arity) {
    case 0: return ((ns_cpu_rt0)rt->fn)();
    case 1: return ((ns_cpu_rt1)rt->fn)(a[0]);
    case 2: return ((ns_cpu_rt2)rt->fn)(a[0], a[1]);
    case 3: return ((ns_cpu_rt3)rt->fn)(a[0], a[1], a[2]);
    case 4: return ((ns_cpu_rt4)rt->fn)(a[0], a[1], a[2], a[3]);
    default: return ((ns_cpu_rt8)rt->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    }
}

/* ── threads ──────────────────────────────────────────────────────────────── */
typedef struct ns_cpu_frame {
    const u16 *pc;
    ns_cpu_fn *fn;
    u64 *r;
    u32 dst;
} ns_cpu_frame;

#define NS_CPU_STACK_REGS (1u << 20)
#define NS_CPU_MAX_DEPTH (1 << 16)

typedef struct ns_cpu_thread {
    u64 *stack;
    u64 *end;
    u64 *top; // first register above the innermost running frame
    ns_cpu_frame *frames;
    i32 depth;
    jmp_buf *jmp; // set by the outermost API call; faults unwind to it
    char msg[512];
} ns_cpu_thread;

#if defined(_MSC_VER)
static __declspec(thread) ns_cpu_thread *ns_cpu_tls = ns_null;
#else
static __thread ns_cpu_thread *ns_cpu_tls = ns_null;
#endif

static ns_cpu_thread *ns_cpu_thread_new(void) {
    ns_cpu_thread *t = calloc(1, sizeof(ns_cpu_thread));
    if (!t) abort();
    // Only the pages a program touches are ever committed.
    t->stack = malloc(sizeof(u64) * NS_CPU_STACK_REGS);
    t->frames = malloc(sizeof(ns_cpu_frame) * NS_CPU_MAX_DEPTH);
    if (!t->stack || !t->frames) abort();
    t->end = t->stack + NS_CPU_STACK_REGS;
    t->top = t->stack;
    return t;
}

static void ns_cpu_thread_free(ns_cpu_thread *t) {
    if (!t) return;
    free(t->stack);
    free(t->frames);
    free(t);
}

#if defined(__GNUC__) || defined(__clang__)
#define NS_CPU_NORETURN __attribute__((noreturn))
#define NS_CPU_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NS_CPU_NORETURN
#define NS_CPU_UNLIKELY(x) (x)
#endif

NS_CPU_NORETURN static void ns_cpu_fault(ns_cpu_fn *fn, const char *fmt, ...) {
    ns_cpu_thread *t = ns_cpu_tls;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char msg[512];
    if (fn) snprintf(msg, sizeof(msg), "%s in fn %.*s", buf, fn->name.len, fn->name.data);
    else snprintf(msg, sizeof(msg), "%s", buf);
    if (t && t->jmp) {
        memcpy(t->msg, msg, sizeof(t->msg));
        longjmp(*t->jmp, 1);
    }
    // Entered from native code (a thunk) with no API call to unwind to.
    fprintf(stderr, "ns_cpu: %s\n", msg);
    fflush(stdout);
    abort();
}

/* ── memory ───────────────────────────────────────────────────────────────── */
static u8 *ns_cpu_addr_slow(ns_cpu_fn *fn, i64 addr) {
    if (!ns_rt_mem) ns_rt_init();
    if (addr >= 0 && (u64)addr < (u64)ns_rt_cap) return ns_rt_mem + addr;
    // A `ref T` from a native module is a host pointer.
    if (addr > 0x10000) return (u8 *)(uintptr_t)addr;
    ns_cpu_fault(fn, "bad address %lld", (long long)addr);
}

static inline u8 *ns_cpu_addr(ns_cpu_fn *fn, i64 addr) {
    if ((u64)addr < (u64)ns_rt_cap) return ns_rt_mem + addr;
    return ns_cpu_addr_slow(fn, addr);
}

#define NS_CPU_RD(T, p) (*(const T *)(const void *)(p))
static inline i16 ns_cpu_rd_i16(const u8 *p) { i16 v; memcpy(&v, p, 2); return v; }
static inline u16 ns_cpu_rd_u16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline i32 ns_cpu_rd_i32(const u8 *p) { i32 v; memcpy(&v, p, 4); return v; }
static inline i64 ns_cpu_rd_i64(const u8 *p) { i64 v; memcpy(&v, p, 8); return v; }
static inline void ns_cpu_wr16(u8 *p, u16 v) { memcpy(p, &v, 2); }
static inline void ns_cpu_wr32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static inline void ns_cpu_wr64(u8 *p, u64 v) { memcpy(p, &v, 8); }

// The address of element `idx` of the array handle `arr`, bounds checked the
// way ns_rt_array_index does.
static inline u8 *ns_cpu_elem(ns_cpu_fn *fn, i64 arr, i64 idx, i64 step) {
    u8 *h = ns_cpu_addr(fn, arr);
    i32 data = ns_cpu_rd_i32(h);
    i32 len = ns_cpu_rd_i32(h + 4);
    if (NS_CPU_UNLIKELY(idx < 0 || idx >= (i64)len)) {
        ns_cpu_fault(fn, "array index out of bounds %lld / %d", (long long)idx, len);
    }
    return ns_cpu_addr(fn, (i64)data + idx * step);
}

static inline i64 ns_cpu_elem_at(ns_cpu_fn *fn, i64 arr, i64 idx, i64 step) {
    u8 *h = ns_cpu_addr(fn, arr);
    i32 data = ns_cpu_rd_i32(h);
    i32 len = ns_cpu_rd_i32(h + 4);
    if (NS_CPU_UNLIKELY(idx < 0 || idx >= (i64)len)) {
        ns_cpu_fault(fn, "array index out of bounds %lld / %d", (long long)idx, len);
    }
    return (i64)data + idx * step;
}

/* ── floats kept as bit patterns ──────────────────────────────────────────── */
static inline f64 ns_cpu_f64(u64 bits) { f64 v; memcpy(&v, &bits, 8); return v; }
static inline u64 ns_cpu_b64(f64 v) { u64 bits; memcpy(&bits, &v, 8); return bits; }
static inline f32 ns_cpu_f32(u64 bits) { u32 lo = (u32)bits; f32 v; memcpy(&v, &lo, 4); return v; }
static inline u64 ns_cpu_b32(f32 v) { u32 bits; memcpy(&bits, &v, 4); return bits; }

// CVTTSD2SI: truncate toward zero; NaN and out-of-range give INT64_MIN.
static inline i64 ns_cpu_ftoi(f64 v) {
    if (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) return INT64_MIN;
    return (i64)v;
}

/* ── thunks ───────────────────────────────────────────────────────────────── */
#define NS_CPU_THUNKS 256
static ns_cpu_fn *ns_cpu_thunk_fn[NS_CPU_THUNKS];

static u64 ns_cpu_exec(ns_cpu_thread *t, ns_cpu_fn *fn, const u64 *args, i32 nargs);

static i64 ns_cpu_thunk_enter(i32 index, const i64 *args) {
    ns_cpu_fn *fn = ns_cpu_thunk_fn[index];
    if (!fn) {
        fprintf(stderr, "ns_cpu: call through a released function value\n");
        abort();
    }
    ns_cpu_thread *t = ns_cpu_tls;
    ns_bool own = t == ns_null;
    if (own) {
        // A worker thread (a task) or a native callback thread: give it a
        // register stack for as long as the call lasts.
        t = ns_cpu_thread_new();
        ns_cpu_tls = t;
    }
    jmp_buf *saved = t->jmp;
    t->jmp = ns_null;
    u64 r = ns_cpu_exec(t, fn, (const u64 *)args, 8);
    t->jmp = saved;
    if (own) {
        ns_cpu_tls = ns_null;
        ns_cpu_thread_free(t);
    }
    return (i64)r;
}

// The pool is spelled out by macros: a thunk's name is a string of base-4
// digits and its index the matching constant expression.
#define NS_CPU_THUNK(n, i) \
    static i64 ns_cpu_thunk_##n(i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6, i64 a7) { \
        const i64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7}; \
        return ns_cpu_thunk_enter((i), args); \
    }
#define NS_CPU_THUNK4(n, i) NS_CPU_THUNK(n##0, (i) * 4 + 0) NS_CPU_THUNK(n##1, (i) * 4 + 1) \
                            NS_CPU_THUNK(n##2, (i) * 4 + 2) NS_CPU_THUNK(n##3, (i) * 4 + 3)
#define NS_CPU_THUNK16(n, i) NS_CPU_THUNK4(n##0, (i) * 4 + 0) NS_CPU_THUNK4(n##1, (i) * 4 + 1) \
                             NS_CPU_THUNK4(n##2, (i) * 4 + 2) NS_CPU_THUNK4(n##3, (i) * 4 + 3)
#define NS_CPU_THUNK64(n, i) NS_CPU_THUNK16(n##0, (i) * 4 + 0) NS_CPU_THUNK16(n##1, (i) * 4 + 1) \
                             NS_CPU_THUNK16(n##2, (i) * 4 + 2) NS_CPU_THUNK16(n##3, (i) * 4 + 3)
NS_CPU_THUNK64(t0, 0) NS_CPU_THUNK64(t1, 1) NS_CPU_THUNK64(t2, 2) NS_CPU_THUNK64(t3, 3)

typedef i64 (*ns_cpu_thunk_ptr)(i64, i64, i64, i64, i64, i64, i64, i64);

#define NS_CPU_THUNK_REF(n) ns_cpu_thunk_##n,
#define NS_CPU_THUNK_REF4(n) NS_CPU_THUNK_REF(n##0) NS_CPU_THUNK_REF(n##1) NS_CPU_THUNK_REF(n##2) NS_CPU_THUNK_REF(n##3)
#define NS_CPU_THUNK_REF16(n) NS_CPU_THUNK_REF4(n##0) NS_CPU_THUNK_REF4(n##1) NS_CPU_THUNK_REF4(n##2) NS_CPU_THUNK_REF4(n##3)
#define NS_CPU_THUNK_REF64(n) NS_CPU_THUNK_REF16(n##0) NS_CPU_THUNK_REF16(n##1) NS_CPU_THUNK_REF16(n##2) NS_CPU_THUNK_REF16(n##3)
static const ns_cpu_thunk_ptr ns_cpu_thunks[NS_CPU_THUNKS] = {
    NS_CPU_THUNK_REF64(t0) NS_CPU_THUNK_REF64(t1) NS_CPU_THUNK_REF64(t2) NS_CPU_THUNK_REF64(t3)
};

static ns_cpu_fn *ns_cpu_thunk_lookup(u64 code) {
    for (i32 i = 0; i < NS_CPU_THUNKS; ++i) {
        if ((u64)(uintptr_t)ns_cpu_thunks[i] == code) return ns_cpu_thunk_fn[i];
    }
    return ns_null;
}

/* ── foreign calls ────────────────────────────────────────────────────────── */
#define NS_CPU_FFI_MAX_ARGS 64

static u64 ns_cpu_ffi_invoke(ns_cpu_fn *fn, ns_cpu_ffi_site *site, const u64 *r, const u16 *regs, i32 nargs) {
    if (NS_CPU_UNLIKELY(!site->fn)) {
        ns_cpu_fault(fn, "unresolved native symbol %.*s.%.*s", site->module.len, site->module.data,
                     site->name.len, site->name.data);
    }
    u64 slots[NS_CPU_FFI_MAX_ARGS];
    for (i32 i = 0; i < nargs; ++i) {
        u64 v = r[regs[i]];
        switch (site->params[i]) {
        case NS_CPU_FFI_STR: v = (u64)ns_rt_to_cstr((i64)v); break;
        case NS_CPU_FFI_REF: v = (u64)ns_rt_native_ptr((i64)v); break;
        case NS_CPU_FFI_ARRAY: v = (u64)ns_rt_array_ptr((i64)v); break;
        default: break;
        }
        slots[i] = v;
    }
    u64 ret[2] = {0, 0};
#ifdef NS_CPU_HAS_FFI
    void *values[NS_CPU_FFI_MAX_ARGS];
    for (i32 i = 0; i < nargs; ++i) values[i] = &slots[i];
    ffi_call(&site->cif, FFI_FN(site->fn), ret, values);
#else
    // Without libffi only integer and pointer arguments are callable; the
    // loader refused every other signature.
    typedef i64 (*ns_cpu_ffi8)(i64, i64, i64, i64, i64, i64, i64, i64);
    i64 a[8] = {0};
    for (i32 i = 0; i < nargs && i < 8; ++i) a[i] = (i64)slots[i];
    ret[0] = (u64)((ns_cpu_ffi8)site->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
#endif
    u64 v = ret[0];
    switch (site->ret) {
    case NS_CPU_FFI_VOID: return 0;
    case NS_CPU_FFI_I8: return (u64)(i64)(i8)v;
    case NS_CPU_FFI_U8: return (u64)(u8)v;
    case NS_CPU_FFI_I16: return (u64)(i64)(i16)v;
    case NS_CPU_FFI_U16: return (u64)(u16)v;
    case NS_CPU_FFI_I32: return (u64)(i64)(i32)v;
    case NS_CPU_FFI_U32: return (u64)(u32)v;
    case NS_CPU_FFI_F32: return (u64)(u32)v;
    case NS_CPU_FFI_STR: return (u64)ns_rt_from_cstr((i64)v);
    default: return v;
    }
}

/* ── interpreter ──────────────────────────────────────────────────────────── */
#define NS_CPU_U32(p) ((u32)(p)[0] | ((u32)(p)[1] << 16))

#if defined(__GNUC__) || defined(__clang__)
#define NS_CPU_THREADED 1
#endif

static inline void ns_cpu_enter_frame(ns_cpu_fn *fn, u64 *r, const u64 *args, i32 nargs) {
    i32 n = nargs < fn->nparams ? nargs : fn->nparams;
    for (i32 i = 0; i < n; ++i) r[i] = args[i];
    for (i32 i = n; i < fn->nparams; ++i) r[i] = 0;
    if (fn->nconst) memcpy(r + fn->nregs, fn->consts, sizeof(u64) * fn->nconst);
}

static u64 ns_cpu_exec(ns_cpu_thread *t, ns_cpu_fn *entry, const u64 *args, i32 nargs) {
    u64 *saved_top = t->top;
    i32 base_depth = t->depth;
    u64 *r = t->top;
    if (NS_CPU_UNLIKELY(r + entry->frame > t->end)) ns_cpu_fault(entry, "register stack overflow");
    ns_cpu_enter_frame(entry, r, args, nargs);
    ns_cpu_fn *fn = entry;
    ns_cpu_module *m = fn->module;
    const u16 *code = fn->code;
    const u16 *pc = code;
    u64 result = 0;

#ifdef NS_CPU_THREADED
    static void *const labels[NS_CPU_OP_COUNT] = {
#define NS_CPU_OP_LABEL(name, fmt) &&ns_cpu_op_##name,
        NS_CPU_OPS(NS_CPU_OP_LABEL)
#undef NS_CPU_OP_LABEL
    };
#define NS_CPU_NEXT() goto *labels[*pc]
#define NS_CPU_CASE(name) ns_cpu_op_##name:
    NS_CPU_NEXT();
#else
#define NS_CPU_NEXT() goto dispatch
#define NS_CPU_CASE(name) case NS_CPU_##name:
dispatch:
    switch ((ns_cpu_op)*pc) {
#endif

#define D r[pc[1]]
#define A r[pc[2]]
#define B r[pc[3]]
#define NS_CPU_BIN(name, expr) NS_CPU_CASE(name) { u64 a = A, b = B; D = (expr); pc += 4; NS_CPU_NEXT(); }
#define NS_CPU_UN(name, expr) NS_CPU_CASE(name) { u64 a = A; D = (expr); pc += 3; NS_CPU_NEXT(); }
#define NS_CPU_BRANCH(name, cond) NS_CPU_CASE(name) { \
        u64 a = r[pc[1]], b = r[pc[2]]; (void)a; (void)b; \
        pc = code + ((cond) ? NS_CPU_U32(pc + 3) : NS_CPU_U32(pc + 5)); NS_CPU_NEXT(); }
#define NS_CPU_LOAD(name, expr) NS_CPU_CASE(name) { \
        const u8 *p = ns_cpu_addr(fn, (i64)A + (i32)NS_CPU_U32(pc + 3)); D = (u64)(expr); pc += 5; NS_CPU_NEXT(); }
#define NS_CPU_STORE(name, stmt) NS_CPU_CASE(name) { \
        u8 *p = ns_cpu_addr(fn, (i64)r[pc[1]] + (i32)NS_CPU_U32(pc + 2)); u64 v = r[pc[4]]; stmt; pc += 5; NS_CPU_NEXT(); }
#define NS_CPU_AIDX(name, step, expr) NS_CPU_CASE(name) { \
        const u8 *p = ns_cpu_elem(fn, (i64)A, (i64)B, step); D = (u64)(expr); pc += 4; NS_CPU_NEXT(); }
#define NS_CPU_ASTORE(name, step, stmt) NS_CPU_CASE(name) { \
        u8 *p = ns_cpu_elem(fn, (i64)r[pc[1]], (i64)r[pc[2]], step); u64 v = r[pc[3]]; stmt; pc += 4; NS_CPU_NEXT(); }

    NS_CPU_CASE(NOP) { pc += 1; NS_CPU_NEXT(); }
    NS_CPU_CASE(MOV) { D = A; pc += 3; NS_CPU_NEXT(); }
    NS_CPU_CASE(ZERO) { D = 0; pc += 2; NS_CPU_NEXT(); }

    NS_CPU_BIN(ADD, a + b)
    NS_CPU_BIN(SUB, a - b)
    NS_CPU_BIN(MUL, a * b)
    NS_CPU_CASE(DIVS) {
        i64 a = (i64)A, b = (i64)B;
        if (NS_CPU_UNLIKELY(b == 0)) ns_cpu_fault(fn, "division by zero");
        D = (u64)(b == -1 ? (i64)(0 - (u64)a) : a / b);
        pc += 4;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(DIVU) {
        u64 a = A, b = B;
        if (NS_CPU_UNLIKELY(b == 0)) ns_cpu_fault(fn, "division by zero");
        D = a / b;
        pc += 4;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(MODS) {
        i64 a = (i64)A, b = (i64)B;
        if (NS_CPU_UNLIKELY(b == 0)) ns_cpu_fault(fn, "division by zero");
        D = (u64)(b == -1 ? 0 : a % b);
        pc += 4;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(MODU) {
        u64 a = A, b = B;
        if (NS_CPU_UNLIKELY(b == 0)) ns_cpu_fault(fn, "division by zero");
        D = a % b;
        pc += 4;
        NS_CPU_NEXT();
    }
    NS_CPU_BIN(AND, a & b)
    NS_CPU_BIN(OR, a | b)
    NS_CPU_BIN(XOR, a ^ b)
    // x86 shifts use the low six bits of the count.
    NS_CPU_BIN(SHL, a << (b & 63))
    NS_CPU_BIN(SHRS, (u64)((i64)a >> (b & 63)))
    NS_CPU_BIN(SHRU, a >> (b & 63))
    NS_CPU_UN(NEG, 0 - a)
    // i32 arithmetic: the 64-bit result wrapped to 32 bits and sign-extended.
    NS_CPU_BIN(ADDW, (u64)(i64)(i32)(u32)(a + b))
    NS_CPU_BIN(SUBW, (u64)(i64)(i32)(u32)(a - b))
    NS_CPU_BIN(MULW, (u64)(i64)(i32)(u32)(a * b))
    NS_CPU_BIN(SHLW, (u64)(i64)(i32)(u32)(a << (b & 63)))
    NS_CPU_UN(NEGW, (u64)(i64)(i32)(u32)(0 - a))
    NS_CPU_UN(NOT, (u64)(a == 0))
    NS_CPU_CASE(ADDI) { D = A + (u64)(i64)(i32)NS_CPU_U32(pc + 3); pc += 5; NS_CPU_NEXT(); }

    NS_CPU_BIN(FADD64, ns_cpu_b64(ns_cpu_f64(a) + ns_cpu_f64(b)))
    NS_CPU_BIN(FSUB64, ns_cpu_b64(ns_cpu_f64(a) - ns_cpu_f64(b)))
    NS_CPU_BIN(FMUL64, ns_cpu_b64(ns_cpu_f64(a) * ns_cpu_f64(b)))
    NS_CPU_BIN(FDIV64, ns_cpu_b64(ns_cpu_f64(a) / ns_cpu_f64(b)))
    NS_CPU_BIN(FADD32, ns_cpu_b32(ns_cpu_f32(a) + ns_cpu_f32(b)))
    NS_CPU_BIN(FSUB32, ns_cpu_b32(ns_cpu_f32(a) - ns_cpu_f32(b)))
    NS_CPU_BIN(FMUL32, ns_cpu_b32(ns_cpu_f32(a) * ns_cpu_f32(b)))
    NS_CPU_BIN(FDIV32, ns_cpu_b32(ns_cpu_f32(a) / ns_cpu_f32(b)))
    NS_CPU_UN(FNEG64, a ^ 0x8000000000000000ull)
    NS_CPU_UN(FNEG32, a ^ 0x80000000ull)

    NS_CPU_BIN(EQ, (u64)(a == b))
    NS_CPU_BIN(NE, (u64)(a != b))
    NS_CPU_BIN(LTS, (u64)((i64)a < (i64)b))
    NS_CPU_BIN(LES, (u64)((i64)a <= (i64)b))
    NS_CPU_BIN(LTU, (u64)(a < b))
    NS_CPU_BIN(LEU, (u64)(a <= b))
    NS_CPU_BIN(FEQ64, (u64)(ns_cpu_f64(a) == ns_cpu_f64(b)))
    NS_CPU_BIN(FNE64, (u64)(ns_cpu_f64(a) != ns_cpu_f64(b)))
    NS_CPU_BIN(FLT64, (u64)(ns_cpu_f64(a) < ns_cpu_f64(b)))
    NS_CPU_BIN(FLE64, (u64)(ns_cpu_f64(a) <= ns_cpu_f64(b)))
    NS_CPU_BIN(FEQ32, (u64)(ns_cpu_f32(a) == ns_cpu_f32(b)))
    NS_CPU_BIN(FNE32, (u64)(ns_cpu_f32(a) != ns_cpu_f32(b)))
    NS_CPU_BIN(FLT32, (u64)(ns_cpu_f32(a) < ns_cpu_f32(b)))
    NS_CPU_BIN(FLE32, (u64)(ns_cpu_f32(a) <= ns_cpu_f32(b)))

    NS_CPU_CASE(JMP) { pc = code + NS_CPU_U32(pc + 1); NS_CPU_NEXT(); }
    NS_CPU_CASE(BNZ) { pc = code + (r[pc[1]] ? NS_CPU_U32(pc + 2) : NS_CPU_U32(pc + 4)); NS_CPU_NEXT(); }
    NS_CPU_BRANCH(BEQ, a == b)
    NS_CPU_BRANCH(BNE, a != b)
    NS_CPU_BRANCH(BLTS, (i64)a < (i64)b)
    NS_CPU_BRANCH(BLES, (i64)a <= (i64)b)
    NS_CPU_BRANCH(BLTU, a < b)
    NS_CPU_BRANCH(BLEU, a <= b)
    NS_CPU_BRANCH(BFEQ64, ns_cpu_f64(a) == ns_cpu_f64(b))
    NS_CPU_BRANCH(BFNE64, ns_cpu_f64(a) != ns_cpu_f64(b))
    NS_CPU_BRANCH(BFLT64, ns_cpu_f64(a) < ns_cpu_f64(b))
    NS_CPU_BRANCH(BFLE64, ns_cpu_f64(a) <= ns_cpu_f64(b))
    NS_CPU_BRANCH(BFEQ32, ns_cpu_f32(a) == ns_cpu_f32(b))
    NS_CPU_BRANCH(BFNE32, ns_cpu_f32(a) != ns_cpu_f32(b))
    NS_CPU_BRANCH(BFLT32, ns_cpu_f32(a) < ns_cpu_f32(b))
    NS_CPU_BRANCH(BFLE32, ns_cpu_f32(a) <= ns_cpu_f32(b))

    NS_CPU_UN(SEXT8, (u64)(i64)(i8)a)
    NS_CPU_UN(ZEXT8, (u64)(u8)a)
    NS_CPU_UN(SEXT16, (u64)(i64)(i16)a)
    NS_CPU_UN(ZEXT16, (u64)(u16)a)
    NS_CPU_UN(SEXT32, (u64)(i64)(i32)a)
    NS_CPU_UN(ZEXT32, (u64)(u32)a)
    NS_CPU_UN(I2F64, ns_cpu_b64((f64)(i64)a))
    NS_CPU_UN(I2F32, ns_cpu_b32((f32)(i64)a))
    NS_CPU_UN(F64TOI, (u64)ns_cpu_ftoi(ns_cpu_f64(a)))
    NS_CPU_UN(F32TOI, (u64)ns_cpu_ftoi((f64)ns_cpu_f32(a)))
    NS_CPU_UN(F32TO64, ns_cpu_b64((f64)ns_cpu_f32(a)))
    NS_CPU_UN(F64TO32, ns_cpu_b32((f32)ns_cpu_f64(a)))

    NS_CPU_LOAD(LD8S, (i64)(i8)p[0])
    NS_CPU_LOAD(LD8U, p[0])
    NS_CPU_LOAD(LD16S, (i64)ns_cpu_rd_i16(p))
    NS_CPU_LOAD(LD16U, ns_cpu_rd_u16(p))
    NS_CPU_LOAD(LD32, (i64)ns_cpu_rd_i32(p))
    NS_CPU_LOAD(LD64, ns_cpu_rd_i64(p))
    NS_CPU_CASE(LDX64) { D = (u64)ns_cpu_rd_i64(ns_cpu_addr(fn, (i64)(A + B))); pc += 4; NS_CPU_NEXT(); }
    NS_CPU_STORE(ST8, p[0] = (u8)v)
    NS_CPU_STORE(ST16, ns_cpu_wr16(p, (u16)v))
    NS_CPU_STORE(ST32, ns_cpu_wr32(p, (u32)v))
    NS_CPU_STORE(ST64, ns_cpu_wr64(p, v))
    NS_CPU_CASE(STCOPY) {
        u32 size = NS_CPU_U32(pc + 5);
        if (size > 0) {
            u8 *dst = ns_cpu_addr(fn, (i64)r[pc[1]] + (i32)NS_CPU_U32(pc + 2));
            const u8 *src = ns_cpu_addr(fn, (i64)r[pc[4]]);
            memcpy(dst, src, size);
        }
        pc += 7;
        NS_CPU_NEXT();
    }
    NS_CPU_AIDX(AIDX8S, 1, (i64)(i8)p[0])
    NS_CPU_AIDX(AIDX8U, 1, p[0])
    NS_CPU_AIDX(AIDX16S, 2, (i64)ns_cpu_rd_i16(p))
    NS_CPU_AIDX(AIDX16U, 2, ns_cpu_rd_u16(p))
    NS_CPU_AIDX(AIDX32, 4, (i64)ns_cpu_rd_i32(p))
    NS_CPU_AIDX(AIDX64, 8, ns_cpu_rd_i64(p))
    NS_CPU_CASE(ASLOT) {
        D = (u64)ns_cpu_elem_at(fn, (i64)A, (i64)B, (i64)NS_CPU_U32(pc + 4));
        pc += 6;
        NS_CPU_NEXT();
    }
    NS_CPU_ASTORE(AST8, 1, p[0] = (u8)v)
    NS_CPU_ASTORE(AST16, 2, ns_cpu_wr16(p, (u16)v))
    NS_CPU_ASTORE(AST32, 4, ns_cpu_wr32(p, (u32)v))
    NS_CPU_ASTORE(AST64, 8, ns_cpu_wr64(p, v))
    NS_CPU_CASE(ASTCOPY) {
        u32 stride = NS_CPU_U32(pc + 4);
        u8 *dst = ns_cpu_elem(fn, (i64)r[pc[1]], (i64)r[pc[2]], (i64)stride);
        memcpy(dst, ns_cpu_addr(fn, (i64)r[pc[3]]), stride);
        pc += 6;
        NS_CPU_NEXT();
    }

    NS_CPU_CASE(GGET) { D = m->globals[pc[2]]; pc += 3; NS_CPU_NEXT(); }
    NS_CPU_CASE(GSET) { m->globals[pc[1]] = r[pc[2]]; pc += 3; NS_CPU_NEXT(); }

    NS_CPU_CASE(CALL) {
        ns_cpu_fn *callee = &m->fns[pc[2]];
        u32 n = pc[3];
        u64 *nr = r + fn->frame;
        if (NS_CPU_UNLIKELY(nr + callee->frame > t->end)) ns_cpu_fault(callee, "register stack overflow");
        if (NS_CPU_UNLIKELY(t->depth >= NS_CPU_MAX_DEPTH)) ns_cpu_fault(callee, "call stack overflow");
        u32 np = callee->nparams;
        u32 k = n < np ? n : np;
        for (u32 i = 0; i < k; ++i) nr[i] = r[pc[4 + i]];
        for (u32 i = k; i < np; ++i) nr[i] = 0;
        if (callee->nconst) memcpy(nr + callee->nregs, callee->consts, sizeof(u64) * callee->nconst);
        ns_cpu_frame *f = &t->frames[t->depth++];
        f->pc = pc + 4 + n;
        f->fn = fn;
        f->r = r;
        f->dst = pc[1];
        r = nr;
        fn = callee;
        code = pc = fn->code;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(CALLI) {
        u32 n = pc[4];
        i64 fnval = (i64)A;
        u64 target = fnval > 0 ? (u64)ns_cpu_rd_i64(ns_cpu_addr(fn, fnval)) : 0;
        ns_cpu_cache *cache = &m->cache[pc[3]];
        ns_cpu_fn *callee = cache->fn;
        if (!callee || (u64)(uintptr_t)ns_cpu_thunks[callee->thunk] != target) {
            callee = target ? ns_cpu_thunk_lookup(target) : ns_null;
            if (callee) cache->fn = callee;
        }
        if (!callee) {
            // Not an ns function: a code pointer native code handed over.
            if (!target) ns_cpu_fault(fn, "call through a nil function value");
            i64 a[8] = {0};
            for (u32 i = 0; i < n && i < 8; ++i) a[i] = (i64)r[pc[5 + i]];
            t->top = r + fn->frame;
            D = (u64)((ns_cpu_thunk_ptr)(uintptr_t)target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
            pc += 5 + n;
            NS_CPU_NEXT();
        }
        u64 *nr = r + fn->frame;
        if (NS_CPU_UNLIKELY(nr + callee->frame > t->end)) ns_cpu_fault(callee, "register stack overflow");
        if (NS_CPU_UNLIKELY(t->depth >= NS_CPU_MAX_DEPTH)) ns_cpu_fault(callee, "call stack overflow");
        u32 np = callee->nparams;
        u32 k = n < np ? n : np;
        for (u32 i = 0; i < k; ++i) nr[i] = r[pc[5 + i]];
        for (u32 i = k; i < np; ++i) nr[i] = 0;
        if (callee->nconst) memcpy(nr + callee->nregs, callee->consts, sizeof(u64) * callee->nconst);
        ns_cpu_frame *f = &t->frames[t->depth++];
        f->pc = pc + 5 + n;
        f->fn = fn;
        f->r = r;
        f->dst = pc[1];
        r = nr;
        fn = callee;
        m = fn->module;
        code = pc = fn->code;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(RT) {
        const ns_cpu_rt *rt = m->rt[pc[2]];
        u32 n = pc[3];
        i64 a[8] = {0};
        for (u32 i = 0; i < n && i < 8; ++i) a[i] = (i64)r[pc[4 + i]];
        t->top = r + fn->frame;
        D = (u64)ns_cpu_rt_invoke(rt, a);
        pc += 4 + n;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(FFI) {
        u32 n = pc[3];
        t->top = r + fn->frame;
        D = ns_cpu_ffi_invoke(fn, &m->ffi[pc[2]], r, pc + 4, (i32)n);
        pc += 4 + n;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(FNADDR) {
        ns_cpu_fn *target = &m->fns[pc[2]];
        D = (u64)(uintptr_t)ns_cpu_thunks[target->thunk];
        pc += 3;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(RET) {
        u64 v = r[pc[1]];
        if (t->depth == base_depth) {
            result = v;
            goto done;
        }
        ns_cpu_frame *f = &t->frames[--t->depth];
        r = f->r;
        fn = f->fn;
        m = fn->module;
        code = fn->code;
        pc = f->pc;
        r[f->dst] = v;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(ASSERT) {
        if (NS_CPU_UNLIKELY(!r[pc[1]])) {
            const u32 *loc = &m->locs[NS_CPU_U32(pc + 2) * 2];
            ns_str file = m->strings[loc[0]];
            ns_cpu_fault(fn, "assertion failed at %.*s:%u", file.len, file.data, loc[1]);
        }
        pc += 4;
        NS_CPU_NEXT();
    }
    NS_CPU_CASE(TRAP) {
        const u32 *loc = &m->locs[NS_CPU_U32(pc + 1) * 2];
        ns_str file = m->strings[loc[0]];
        ns_cpu_fault(fn, "trap at %.*s:%u", file.len, file.data, loc[1]);
    }

#ifndef NS_CPU_THREADED
    default: ns_cpu_fault(fn, "bad opcode %u", (unsigned)*pc);
    }
#endif

#undef D
#undef A
#undef B
#undef NS_CPU_BIN
#undef NS_CPU_UN
#undef NS_CPU_BRANCH
#undef NS_CPU_LOAD
#undef NS_CPU_STORE
#undef NS_CPU_AIDX
#undef NS_CPU_ASTORE
#undef NS_CPU_NEXT
#undef NS_CPU_CASE

done:
    t->top = saved_top;
    return result;
}

/* ── image reader ─────────────────────────────────────────────────────────── */
typedef struct ns_cpu_reader {
    const u8 *p, *end;
    ns_bool bad;
} ns_cpu_reader;

static ns_bool ns_cpu_need(ns_cpu_reader *rd, szt n) {
    if (rd->bad || (szt)(rd->end - rd->p) < n) {
        rd->bad = true;
        return false;
    }
    return true;
}

static u8 ns_cpu_r8(ns_cpu_reader *rd) {
    if (!ns_cpu_need(rd, 1)) return 0;
    return *rd->p++;
}

static u16 ns_cpu_r16(ns_cpu_reader *rd) {
    if (!ns_cpu_need(rd, 2)) return 0;
    u16 v = (u16)(rd->p[0] | (rd->p[1] << 8));
    rd->p += 2;
    return v;
}

static u32 ns_cpu_r32(ns_cpu_reader *rd) {
    u32 lo = ns_cpu_r16(rd);
    u32 hi = ns_cpu_r16(rd);
    return lo | (hi << 16);
}

static u64 ns_cpu_r64(ns_cpu_reader *rd) {
    u64 lo = ns_cpu_r32(rd);
    u64 hi = ns_cpu_r32(rd);
    return lo | (hi << 32);
}

ns_bool ns_cpu_image_is(const u8 *data, szt size) {
    return data && size >= 8 && memcmp(data, NS_CPU_IMAGE_MAGIC, 5) == 0 && data[5] == 0;
}

/* ── verifier ─────────────────────────────────────────────────────────────── */
static ns_bool ns_cpu_is_terminator(u16 op) {
    return op == NS_CPU_JMP || op == NS_CPU_RET || op == NS_CPU_TRAP ||
           (op >= NS_CPU_BNZ && op <= NS_CPU_BFLE32);
}

// Walk every instruction of `fn`: the opcode must exist, its operands must fit
// in the code, every register must lie in the frame (destinations below the
// constants), every table index must exist, every branch must land on an
// instruction, and the last one must not fall off the end of the code.
static ns_bool ns_cpu_verify_fn(ns_cpu_module *m, ns_cpu_fn *fn, char *err, szt errlen) {
    u32 n = fn->ncode;
    const u16 *c = fn->code;
    u8 *starts = calloc(n + 1, 1);
    u32 *targets = ns_null;
    u32 pc = 0;
    u16 last = NS_CPU_NOP;
    ns_bool ok = true;
#define NS_CPU_BAD(...) do { snprintf(err, errlen, __VA_ARGS__); ok = false; goto out; } while (0)
    if (n == 0) NS_CPU_BAD("empty code");
    while (pc < n) {
        starts[pc] = 1;
        u16 op = c[pc];
        if (op >= NS_CPU_OP_COUNT) NS_CPU_BAD("bad opcode %u at %u", (unsigned)op, pc);
        last = op;
        u32 at = pc + 1;
        for (const char *f = ns_cpu_op_formats[op]; *f; ++f) {
            u32 width = (*f == 't' || *f == 'w' || *f == 'l') ? 2 : 1;
            if (at + width > n) NS_CPU_BAD("truncated %s at %u", ns_cpu_op_names[op], pc);
            u32 v = c[at];
            switch (*f) {
            case 'd': if (v >= fn->nregs) NS_CPU_BAD("destination r%u out of frame at %u", v, pc); break;
            case 'r': if (v >= fn->frame) NS_CPU_BAD("register r%u out of frame at %u", v, pc); break;
            case 't': ns_array_push(targets, NS_CPU_U32(c + at)); break;
            case 'w': break;
            case 'f':
                if (v >= m->nfn) NS_CPU_BAD("function %u out of range at %u", v, pc);
                if (op == NS_CPU_FNADDR && m->fns[v].thunk < 0) {
                    // Reserve a thunk now so FNADDR never has to.
                    for (i32 i = 0; i < NS_CPU_THUNKS; ++i) {
                        if (!ns_cpu_thunk_fn[i]) {
                            ns_cpu_thunk_fn[i] = &m->fns[v];
                            m->fns[v].thunk = i;
                            break;
                        }
                    }
                    if (m->fns[v].thunk < 0) NS_CPU_BAD("more than %d function values", NS_CPU_THUNKS);
                }
                break;
            case 'g': if (v >= m->nglobals) NS_CPU_BAD("global %u out of range at %u", v, pc); break;
            case 'x': if (v >= m->nrt) NS_CPU_BAD("helper %u out of range at %u", v, pc); break;
            case 'y': if (v >= m->nffi) NS_CPU_BAD("foreign call %u out of range at %u", v, pc); break;
            case 'k': if (v >= m->ncache) NS_CPU_BAD("call cache %u out of range at %u", v, pc); break;
            case 'l': if (NS_CPU_U32(c + at) >= m->nlocs) NS_CPU_BAD("location out of range at %u", pc); break;
            case 'n': {
                if (at + 1 + v > n) NS_CPU_BAD("truncated argument list at %u", pc);
                if (op == NS_CPU_RT && v > 8) NS_CPU_BAD("helper call with %u arguments at %u", v, pc);
                if (op == NS_CPU_FFI && v != m->ffi[c[at - 1]].nparams) {
                    NS_CPU_BAD("foreign call arity mismatch at %u", pc);
                }
                for (u32 i = 0; i < v; ++i) {
                    if (c[at + 1 + i] >= fn->frame) NS_CPU_BAD("argument register out of frame at %u", pc);
                }
                width = 1 + v;
            } break;
            default: NS_CPU_BAD("bad operand format");
            }
            at += width;
        }
        pc = at;
    }
    if (!ns_cpu_is_terminator(last)) NS_CPU_BAD("code falls off the end");
    for (i32 i = 0, l = (i32)ns_array_length(targets); i < l; ++i) {
        if (targets[i] >= n || !starts[targets[i]]) NS_CPU_BAD("branch into the middle of an instruction");
    }
out:
#undef NS_CPU_BAD
    free(starts);
    ns_array_free(targets);
    return ok;
}

/* ── linking ──────────────────────────────────────────────────────────────── */
static void *ns_cpu_default_resolve(ns_cpu_module *m, ns_str module, ns_str name) {
    char sym[256];
    snprintf(sym, sizeof(sym), "%.*s", name.len, name.data);
#if defined(NS_CPU_HAS_FFI) || (defined(NS_XCLIB) && !defined(_WIN32))
    void *handle = ns_null;
    ns_bool found = false;
    for (i32 i = 0, l = (i32)ns_array_length(m->libs); i < l; ++i) {
        if (ns_str_equals(m->libs[i].module, module)) {
            handle = m->libs[i].handle;
            found = true;
            break;
        }
    }
#ifndef NS_XCLIB
    if (!found) {
        ns_str dirs[2] = {m->host.lib_path, m->host.lib_fallback_path};
        for (i32 d = 0; d < 2 && !handle; ++d) {
            if (!dirs[d].data || dirs[d].len == 0) continue;
            char path[1024];
            snprintf(path, sizeof(path), "%.*s/%.*s%.*s", dirs[d].len, dirs[d].data, module.len, module.data,
                     ns_lib_ext.len, ns_lib_ext.data);
            handle = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
        }
        ns_cpu_lib lib = {.module = module, .handle = handle};
        ns_array_push(m->libs, lib);
    }
#else
    ns_unused(found);
#endif
    void *fn = handle ? dlsym(handle, sym) : ns_null;
    if (!fn) fn = dlsym(RTLD_DEFAULT, sym);
    return fn;
#else
    ns_unused(m);
    ns_unused(module);
    return ns_null;
#endif
}

#ifdef NS_CPU_HAS_FFI
static ffi_type *ns_cpu_ffi_type(u8 kind) {
    switch (kind) {
    case NS_CPU_FFI_VOID: return &ffi_type_void;
    case NS_CPU_FFI_I8: return &ffi_type_sint8;
    case NS_CPU_FFI_U8: return &ffi_type_uint8;
    case NS_CPU_FFI_I16: return &ffi_type_sint16;
    case NS_CPU_FFI_U16: return &ffi_type_uint16;
    case NS_CPU_FFI_I32: return &ffi_type_sint32;
    case NS_CPU_FFI_U32: return &ffi_type_uint32;
    case NS_CPU_FFI_F32: return &ffi_type_float;
    case NS_CPU_FFI_F64: return &ffi_type_double;
    case NS_CPU_FFI_STR:
    case NS_CPU_FFI_REF:
    case NS_CPU_FFI_ARRAY: return &ffi_type_pointer;
    default: return &ffi_type_sint64;
    }
}
#endif

static void *ns_cpu_resolve_site(ns_cpu_module *m, ns_cpu_ffi_site *site) {
    char mod[128], sym[256];
    snprintf(mod, sizeof(mod), "%.*s", site->module.len, site->module.data);
    snprintf(sym, sizeof(sym), "%.*s", site->name.len, site->name.data);
    return m->host.resolve ? m->host.resolve(m->host.user, mod, sym)
                           : ns_cpu_default_resolve(m, site->module, site->name);
}

// Symbols are looked up when the image loads, but one that is missing only
// faults when a call reaches it: an image keeps loading on a host that lacks a
// module it does not use there. Nothing is resolved later, so running code
// never touches the loader's tables from several threads.
static ns_bool ns_cpu_link_ffi(ns_cpu_module *m, ns_cpu_ffi_site *site, char *err, szt errlen) {
    char mod[128], sym[256];
    snprintf(mod, sizeof(mod), "%.*s", site->module.len, site->module.data);
    snprintf(sym, sizeof(sym), "%.*s", site->name.len, site->name.data);
    site->fn = ns_cpu_resolve_site(m, site);
#ifdef NS_CPU_HAS_FFI
    site->types = malloc(sizeof(ffi_type *) * (site->nparams + 1));
    for (i32 i = 0; i < site->nparams; ++i) site->types[i] = ns_cpu_ffi_type(site->params[i]);
    if (ffi_prep_cif(&site->cif, FFI_DEFAULT_ABI, site->nparams, ns_cpu_ffi_type(site->ret), site->types) != FFI_OK) {
        snprintf(err, errlen, "cannot prepare the native call %.100s.%.100s", mod, sym);
        return false;
    }
#else
    ns_bool floats = site->ret == NS_CPU_FFI_F32 || site->ret == NS_CPU_FFI_F64;
    for (i32 i = 0; i < site->nparams; ++i) {
        if (site->params[i] == NS_CPU_FFI_F32 || site->params[i] == NS_CPU_FFI_F64) floats = true;
    }
    if (floats || site->nparams > 8) {
        snprintf(err, errlen, "native call %.100s.%.100s needs libffi (float or >8 arguments)", mod, sym);
        return false;
    }
#endif
    return true;
}

// String literals are materialized once, when the image loads, and pinned, so
// no call scope ever releases one.
static i64 ns_cpu_literal(ns_cpu_module *m, u32 index) {
    if (m->literals[index]) return m->literals[index];
    ns_str s = m->strings[index];
    i64 addr = ns_rt_from_bytes(s.data, s.len);
    ns_rt_pin(addr, 8 + s.len);
    m->literals[index] = addr;
    return addr;
}

void ns_cpu_unload(ns_cpu_module *m) {
    if (!m) return;
    // A module that failed to load may stop anywhere: every table is either
    // absent or allocated for its full count, and zeroed.
    for (u32 i = 0; m->fns && i < m->nfn; ++i) {
        ns_cpu_fn *fn = &m->fns[i];
        if (fn->thunk >= 0 && ns_cpu_thunk_fn[fn->thunk] == fn) ns_cpu_thunk_fn[fn->thunk] = ns_null;
        free(fn->code);
        free(fn->consts);
    }
    for (u32 i = 0; m->ffi && i < m->nffi; ++i) {
        free(m->ffi[i].params);
#ifdef NS_CPU_HAS_FFI
        free(m->ffi[i].types);
#endif
    }
    for (u32 i = 0; m->strings && i < m->nstrings; ++i) free(m->strings[i].data);
    ns_array_free(m->libs);
    free(m->strings);
    free(m->literals);
    free(m->fns);
    free(m->rt);
    free(m->ffi);
    free(m->cache);
    free(m->locs);
    free(m->globals);
    free(m->global_names);
    free(m->global_types);
    free(m);
}

static ns_str ns_cpu_string_at(ns_cpu_module *m, u32 index) {
    return index < m->nstrings ? m->strings[index] : ns_str_null;
}

ns_return_ptr ns_cpu_load(const u8 *data, szt size, const ns_cpu_host *host, ns_cpu_module *prev) {
    if (!ns_cpu_image_is(data, size)) {
        return NS_CPU_ERROR(ptr, ns_code_loc_nil, NS_ERR_BITCODE, "not an ns_cpu image");
    }
    ns_cpu_reader rd = {.p = data + 6, .end = data + size};
    if (ns_cpu_r16(&rd) != NS_CPU_IMAGE_VERSION) {
        return NS_CPU_ERROR(ptr, ns_code_loc_nil, NS_ERR_BITCODE, "unsupported ns_cpu image version");
    }
    ns_rt_init();

    ns_cpu_module *m = calloc(1, sizeof(ns_cpu_module));
    if (host) m->host = *host;
    char err[256] = {0};
#define NS_CPU_LOAD_FAIL(...) do { snprintf(err, sizeof(err), __VA_ARGS__); goto fail; } while (0)
#define NS_CPU_COUNT(v, limit) do { \
        u32 count_ = ns_cpu_r32(&rd); \
        if (rd.bad || count_ > (limit)) NS_CPU_LOAD_FAIL("corrupt image"); \
        v = count_; \
    } while (0)

    NS_CPU_COUNT(m->nstrings, size);
    m->strings = calloc(m->nstrings + 1, sizeof(ns_str));
    m->literals = calloc(m->nstrings + 1, sizeof(i64));
    for (u32 i = 0; i < m->nstrings; ++i) {
        u32 len = ns_cpu_r32(&rd);
        if (!ns_cpu_need(&rd, len)) NS_CPU_LOAD_FAIL("corrupt string pool");
        i8 *s = malloc(len + 1);
        memcpy(s, rd.p, len);
        s[len] = 0;
        rd.p += len;
        m->strings[i] = (ns_str){.data = s, .len = (i32)len, .dynamic = 1};
    }

    NS_CPU_COUNT(m->nglobals, 0xffff);
    m->globals = calloc(m->nglobals + 1, sizeof(u64));
    m->global_names = calloc(m->nglobals + 1, sizeof(u32));
    m->global_types = calloc(m->nglobals + 1, sizeof(u32));
    for (u32 i = 0; i < m->nglobals; ++i) {
        m->global_names[i] = ns_cpu_r32(&rd);
        m->global_types[i] = ns_cpu_r32(&rd);
        if (rd.bad || m->global_names[i] >= m->nstrings || m->global_types[i] >= m->nstrings) {
            NS_CPU_LOAD_FAIL("corrupt globals");
        }
    }

    NS_CPU_COUNT(m->nrt, 0xffff);
    m->rt = calloc(m->nrt + 1, sizeof(ns_cpu_rt *));
    for (u32 i = 0; i < m->nrt; ++i) {
        ns_str name = ns_cpu_string_at(m, ns_cpu_r32(&rd));
        m->rt[i] = ns_cpu_rt_find(name);
        if (!m->rt[i]) NS_CPU_LOAD_FAIL("unknown runtime helper %.*s", name.len, name.data);
    }

    NS_CPU_COUNT(m->nffi, 0xffff);
    m->ffi = calloc(m->nffi + 1, sizeof(ns_cpu_ffi_site));
    for (u32 i = 0; i < m->nffi; ++i) {
        ns_cpu_ffi_site *site = &m->ffi[i];
        site->module = ns_cpu_string_at(m, ns_cpu_r32(&rd));
        site->name = ns_cpu_string_at(m, ns_cpu_r32(&rd));
        site->ret = ns_cpu_r8(&rd);
        site->nparams = ns_cpu_r8(&rd);
        site->params = calloc(site->nparams + 1, 1);
        for (u32 j = 0; j < site->nparams; ++j) site->params[j] = ns_cpu_r8(&rd);
        if (rd.bad || site->name.len == 0 || site->ret >= NS_CPU_FFI_KIND_COUNT ||
            site->nparams > NS_CPU_FFI_MAX_ARGS) {
            NS_CPU_LOAD_FAIL("corrupt foreign call table");
        }
        for (u32 j = 0; j < site->nparams; ++j) {
            if (site->params[j] == NS_CPU_FFI_VOID || site->params[j] >= NS_CPU_FFI_KIND_COUNT) NS_CPU_LOAD_FAIL("corrupt foreign call table");
        }
    }

    NS_CPU_COUNT(m->ncache, 0x10000);
    m->cache = calloc(m->ncache + 1, sizeof(ns_cpu_cache));

    NS_CPU_COUNT(m->nlocs, size / 8);
    m->locs = calloc((szt)m->nlocs * 2 + 2, sizeof(u32));
    for (u32 i = 0; i < m->nlocs; ++i) {
        m->locs[(szt)i * 2] = ns_cpu_r32(&rd);
        m->locs[(szt)i * 2 + 1] = ns_cpu_r32(&rd);
        if (rd.bad || m->locs[(szt)i * 2] >= m->nstrings) NS_CPU_LOAD_FAIL("corrupt location table");
    }

    NS_CPU_COUNT(m->nfn, 0xffff);
    m->fns = calloc(m->nfn + 1, sizeof(ns_cpu_fn));
    for (u32 i = 0; i < m->nfn; ++i) m->fns[i].thunk = -1;
    for (u32 i = 0; i < m->nfn; ++i) {
        ns_cpu_fn *fn = &m->fns[i];
        fn->module = m;
        fn->name = ns_cpu_string_at(m, ns_cpu_r32(&rd));
        fn->nparams = ns_cpu_r16(&rd);
        fn->nregs = ns_cpu_r16(&rd);
        fn->nconst = ns_cpu_r16(&rd);
        fn->ncode = ns_cpu_r32(&rd);
        fn->frame = (u32)fn->nregs + fn->nconst;
        if (rd.bad || fn->nparams > fn->nregs || fn->frame > NS_CPU_MAX_REGS || fn->ncode > size) {
            NS_CPU_LOAD_FAIL("corrupt function header");
        }
        fn->consts = calloc(fn->nconst + 1, sizeof(u64));
        for (u32 k = 0; k < fn->nconst; ++k) {
            u8 kind = ns_cpu_r8(&rd);
            u64 value = ns_cpu_r64(&rd);
            if (kind == NS_CPU_CONST_STRING) {
                if (value >= m->nstrings) NS_CPU_LOAD_FAIL("corrupt string constant");
                value = (u64)ns_cpu_literal(m, (u32)value);
            } else if (kind != NS_CPU_CONST_BITS) {
                NS_CPU_LOAD_FAIL("corrupt constant");
            }
            fn->consts[k] = value;
        }
        if (!ns_cpu_need(&rd, (szt)fn->ncode * 2)) NS_CPU_LOAD_FAIL("truncated code");
        fn->code = malloc(sizeof(u16) * (fn->ncode + 1));
        for (u32 k = 0; k < fn->ncode; ++k) fn->code[k] = ns_cpu_r16(&rd);
    }
    m->main_fn = ns_cpu_r32(&rd);
    m->init_fn = ns_cpu_r32(&rd);
    if (rd.bad) NS_CPU_LOAD_FAIL("truncated image");
    if (m->main_fn != NS_CPU_NO_FN && m->main_fn >= m->nfn) NS_CPU_LOAD_FAIL("corrupt entry");
    if (m->init_fn != NS_CPU_NO_FN && m->init_fn >= m->nfn) NS_CPU_LOAD_FAIL("corrupt entry");

    for (u32 i = 0; i < m->nfn; ++i) {
        char why[200];
        if (!ns_cpu_verify_fn(m, &m->fns[i], why, sizeof(why))) {
            NS_CPU_LOAD_FAIL("fn %.*s: %.150s", m->fns[i].name.len > 80 ? 80 : m->fns[i].name.len, m->fns[i].name.data, why);
        }
    }
    for (u32 i = 0; i < m->nffi; ++i) {
        if (!ns_cpu_link_ffi(m, &m->ffi[i], err, sizeof(err))) goto fail;
    }

    // Hot update: keep the value of every global the new image still declares
    // with the same type.
    if (prev) {
        for (u32 i = 0; i < m->nglobals; ++i) {
            ns_str name = m->strings[m->global_names[i]];
            ns_str type = m->strings[m->global_types[i]];
            for (u32 j = 0; j < prev->nglobals; ++j) {
                if (ns_str_equals(name, prev->strings[prev->global_names[j]]) &&
                    ns_str_equals(type, prev->strings[prev->global_types[j]])) {
                    m->globals[i] = prev->globals[j];
                    break;
                }
            }
        }
    }
    return ns_return_ok(ptr, m);

fail:
#undef NS_CPU_LOAD_FAIL
#undef NS_CPU_COUNT
    fprintf(stderr, "ns_cpu: %s\n", err);
    ns_cpu_unload(m);
    return NS_CPU_ERROR(ptr, ns_code_loc_nil, NS_ERR_BITCODE, "invalid ns_cpu image");
}

/* ── running ──────────────────────────────────────────────────────────────── */
static ns_return_bool ns_cpu_invoke(ns_cpu_fn *fn, const i64 *args, i32 nargs, i64 *result) {
    ns_cpu_thread *volatile t = ns_cpu_tls;
    if (!t) {
        // The calling thread keeps its register stack for later calls.
        t = ns_cpu_thread_new();
        ns_cpu_tls = t;
    }
    jmp_buf buf;
    jmp_buf *saved_jmp = t->jmp;
    u64 *saved_top = t->top;
    i32 saved_depth = t->depth;
    t->jmp = &buf;
    if (setjmp(buf) != 0) {
        t->jmp = saved_jmp;
        t->top = saved_top;
        t->depth = saved_depth;
        fflush(stdout);
        fprintf(stderr, "ns_cpu: %s\n", t->msg);
        return NS_CPU_ERROR(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "ns_cpu fault");
    }
    u64 r = ns_cpu_exec(t, fn, (const u64 *)args, nargs);
    t->jmp = saved_jmp;
    if (result) *result = (i64)r;
    return ns_return_ok(bool, true);
}

ns_return_bool ns_cpu_run_init(ns_cpu_module *m) {
    if (!m || m->init_fn == NS_CPU_NO_FN) return ns_return_ok(bool, true);
    return ns_cpu_invoke(&m->fns[m->init_fn], ns_null, 0, ns_null);
}

ns_return_bool ns_cpu_run_main(ns_cpu_module *m, i64 *status) {
    if (status) *status = 0;
    ns_return_bool ret = ns_cpu_run_init(m);
    if (ns_return_is_error(ret)) return ret;
    if (m->main_fn == NS_CPU_NO_FN) return ns_return_ok(bool, true);
    ret = ns_cpu_invoke(&m->fns[m->main_fn], ns_null, 0, status);
    fflush(stdout);
    return ret;
}

ns_return_bool ns_cpu_call(ns_cpu_module *m, const char *name, const i64 *args, i32 nargs, i64 *result) {
    if (!m) return NS_CPU_ERROR(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "no ns_cpu module");
    for (u32 i = 0; i < m->nfn; ++i) {
        if (ns_str_equals_STR(m->fns[i].name, name)) return ns_cpu_invoke(&m->fns[i], args, nargs, result);
    }
    return NS_CPU_ERROR(bool, ns_code_loc_nil, NS_ERR_RUNTIME, "no such ns_cpu function");
}

/* ── files and listing ────────────────────────────────────────────────────── */
ns_return_bool ns_cpu_image_write(ns_str path, const u8 *data, szt size) {
    FILE *f = fopen(path.data, "wb");
    if (!f) return NS_CPU_ERROR(bool, ns_code_loc_nil, NS_ERR, "cannot open image for writing");
    szt wrote = fwrite(data, 1, size, f);
    ns_bool ok = fclose(f) == 0 && wrote == size;
    if (!ok) return NS_CPU_ERROR(bool, ns_code_loc_nil, NS_ERR, "cannot write image");
    return ns_return_ok(bool, true);
}

void ns_cpu_disasm(ns_cpu_module *m) {
    if (!m) return;
    for (u32 fi = 0; fi < m->nfn; ++fi) {
        ns_cpu_fn *fn = &m->fns[fi];
        printf("fn %.*s  params %u  regs %u  consts %u  code %u\n", fn->name.len, fn->name.data,
               fn->nparams, fn->nregs, fn->nconst, fn->ncode);
        for (u32 k = 0; k < fn->nconst; ++k) {
            printf("    r%-5u = 0x%llx\n", fn->nregs + k, (unsigned long long)fn->consts[k]);
        }
        u32 pc = 0;
        while (pc < fn->ncode) {
            u16 op = fn->code[pc];
            printf("  %5u  %-8s", pc, ns_cpu_op_names[op]);
            u32 at = pc + 1;
            for (const char *f = ns_cpu_op_formats[op]; *f; ++f) {
                u16 v = fn->code[at];
                switch (*f) {
                case 'd': case 'r': printf(" r%u", v); at++; break;
                case 't': printf(" @%u", NS_CPU_U32(fn->code + at)); at += 2; break;
                case 'w': printf(" #%d", (i32)NS_CPU_U32(fn->code + at)); at += 2; break;
                case 'l': {
                    u32 li = NS_CPU_U32(fn->code + at);
                    ns_str file = m->strings[m->locs[li * 2]];
                    printf(" %.*s:%u", file.len, file.data, m->locs[li * 2 + 1]);
                    at += 2;
                } break;
                case 'f': printf(" %.*s", m->fns[v].name.len, m->fns[v].name.data); at++; break;
                case 'g': printf(" g%u", v); at++; break;
                case 'x': printf(" %s", m->rt[v]->name); at++; break;
                case 'y': printf(" %.*s.%.*s", m->ffi[v].module.len, m->ffi[v].module.data,
                                 m->ffi[v].name.len, m->ffi[v].name.data); at++; break;
                case 'k': printf(" k%u", v); at++; break;
                case 'n':
                    printf(" (");
                    for (u32 i = 0; i < v; ++i) printf(i ? ", r%u" : "r%u", fn->code[at + 1 + i]);
                    printf(")");
                    at += 1 + v;
                    break;
                default: break;
                }
            }
            printf("\n");
            pc = at;
        }
    }
}
