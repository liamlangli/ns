#include "ns_profile.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

ns_profile_state ns_profile = {0};

#pragma pack(push, 1)
typedef struct ns_profile_tl_event {
    u8 kind;
    u8 depth;
    u16 thread;
    u16 sym;
    u16 pad;
    // Microseconds since profile start / duration / self (ms * 1000).
    i32 start_us;
    i32 elapsed_us;
    i32 self_us;
} ns_profile_tl_event;
#pragma pack(pop)

typedef i32 (*ns_profile_zstd_bound_fn)(i32);
typedef i32 (*ns_profile_zstd_encode_fn)(const u8 *, i32, u8 *, i32, i32);

static void *ns_profile_compress_lib = ns_null;
static ns_profile_zstd_bound_fn ns_profile_zstd_bound = ns_null;
static ns_profile_zstd_encode_fn ns_profile_zstd_encode = ns_null;

static void ns_profile_load_compress(void) {
    if (ns_profile_compress_lib) return;
#if defined(_WIN32)
    (void)0;
#else
    const char *cands[] = {
        "bin/compress.dylib",
        "compress.dylib",
        "../lib/compress.dylib",
        "lib/compress.dylib",
        ns_null,
    };
    // Prefer the dylib next to the running ns executable.
    i8 exe_buf[1024];
    i8 beside[1100];
    ssize_t n = -1;
#if defined(__APPLE__)
    u32 sz = (u32)sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &sz) == 0) n = (ssize_t)strlen(exe_buf);
#elif defined(__linux__)
    n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) exe_buf[n] = 0;
#endif
    if (n > 0) {
        i8 *slash = strrchr(exe_buf, '/');
        if (slash) {
            *slash = 0;
            snprintf(beside, sizeof(beside), "%s/compress.dylib", exe_buf);
            ns_profile_compress_lib = dlopen(beside, RTLD_LAZY | RTLD_LOCAL);
        }
    }
    for (i32 i = 0; !ns_profile_compress_lib && cands[i]; i++) {
        ns_profile_compress_lib = dlopen(cands[i], RTLD_LAZY | RTLD_LOCAL);
    }
    if (!ns_profile_compress_lib) return;
    ns_profile_zstd_bound = (ns_profile_zstd_bound_fn)dlsym(ns_profile_compress_lib, "compress_zstd_bound");
    ns_profile_zstd_encode = (ns_profile_zstd_encode_fn)dlsym(ns_profile_compress_lib, "compress_zstd_encode");
    if (!ns_profile_zstd_bound || !ns_profile_zstd_encode) {
        dlclose(ns_profile_compress_lib);
        ns_profile_compress_lib = ns_null;
        ns_profile_zstd_bound = ns_null;
        ns_profile_zstd_encode = ns_null;
    }
#endif
}

f64 ns_profile_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return ((f64)counter.QuadPart * 1000.0) / (f64)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((f64)ts.tv_sec * 1000.0) + ((f64)ts.tv_nsec / 1000000.0);
#endif
}

static void ns_profile_ensure_main_thread(void) {
    if (ns_profile.thread_count > 0) return;
    ns_profile.threads[0] = ns_str_cstr("main");
    ns_profile.thread_count = 1;
    ns_profile.current_thread = 0;
}

void ns_profile_reset(void) {
    for (i32 i = 0; i < ns_profile.thread_count; i++) {
        if (ns_profile.threads[i].dynamic) ns_str_free(ns_profile.threads[i]);
    }
    ns_array_free(ns_profile.events);
    memset(&ns_profile, 0, sizeof(ns_profile));
}

void ns_profile_enable(f64 start_ms) {
    ns_profile.start_ms = start_ms;
    ns_profile.enabled = true;
    ns_profile_ensure_main_thread();
}

static u32 ns_profile_hash_str(ns_str s) {
    u32 h = 2166136261u;
    for (i32 i = 0; i < s.len; i++) {
        h ^= (u32)(u8)s.data[i];
        h *= 16777619u;
    }
    return h;
}

static ns_bool ns_profile_str_eq(ns_str a, ns_str b) {
    if (a.len != b.len) return false;
    if (a.data == b.data) return true;
    for (i32 i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) return false;
    }
    return true;
}

static i32 ns_profile_thread_intern(ns_str name) {
    ns_profile_ensure_main_thread();
    if (name.len <= 0 || name.data == ns_null) return 0;
    for (i32 i = 0; i < ns_profile.thread_count; i++) {
        if (ns_profile_str_eq(ns_profile.threads[i], name)) return i;
    }
    if (ns_profile.thread_count >= NS_PROFILE_MAX_THREADS) return 0;
    i32 ti = ns_profile.thread_count++;
    // Own a copy so callers can pass ephemeral buffers (e.g. "task#12").
    i8 *buf = (i8 *)ns_malloc((szt)name.len + 1);
    memcpy(buf, name.data, (szt)name.len);
    buf[name.len] = 0;
    ns_profile.threads[ti] = (ns_str){.data = buf, .len = name.len, .dynamic = 1};
    return ti;
}

void ns_profile_bind_thread(ns_str name, ns_profile_thread_ctx *ctx) {
    if (!ns_profile.enabled) {
        if (ctx) {
            ctx->open_len = 0;
            ctx->thread = -1;
        }
        return;
    }
    ns_profile_ensure_main_thread();
    if (name.len > 0 && name.data != ns_null) {
        ns_profile.current_thread = ns_profile_thread_intern(name);
    }
    ns_profile.open_len = 0;
    if (ctx && ctx->open_len > 0) {
        i32 n = ctx->open_len;
        if (n > NS_PROFILE_MAX_STACK) n = NS_PROFILE_MAX_STACK;
        memcpy(ns_profile.open, ctx->open, sizeof(ns_profile_open) * (szt)n);
        ns_profile.open_len = n;
    }
    if (ctx) ctx->thread = ns_profile.current_thread;
}

void ns_profile_park_thread(ns_profile_thread_ctx *ctx) {
    if (!ctx) return;
    if (!ns_profile.enabled) {
        ctx->open_len = 0;
        ctx->thread = -1;
        return;
    }
    i32 n = ns_profile.open_len;
    if (n > NS_PROFILE_MAX_STACK) n = NS_PROFILE_MAX_STACK;
    if (n > 0) memcpy(ctx->open, ns_profile.open, sizeof(ns_profile_open) * (szt)n);
    ctx->open_len = n;
    ctx->thread = ns_profile.current_thread;
    ns_profile.open_len = 0;
}

static i32 ns_profile_fn_get(ns_str name, ns_str lib, u8 kind) {
    u32 h = ns_profile_hash_str(name) ^ (ns_profile_hash_str(lib) * 0x9e3779b9u) ^ (u32)kind;
    for (i32 n = 0; n < NS_PROFILE_FN_HASH; n++) {
        i32 slot = (i32)((h + (u32)n) & (NS_PROFILE_FN_HASH - 1));
        i32 idx = ns_profile.fn_hash[slot];
        if (idx == 0) {
            if (ns_profile.fn_count >= NS_PROFILE_MAX_FNS) {
                ns_profile.fns_dropped++;
                return -1;
            }
            i32 fi = ns_profile.fn_count++;
            ns_profile.fns[fi] = (ns_profile_fn_stat){
                .name = name,
                .lib = lib,
                .kind = kind,
                .min_ms = 0.0,
                .max_ms = 0.0,
            };
            ns_profile.fn_hash[slot] = fi + 1;
            return fi;
        }
        i32 fi = idx - 1;
        ns_profile_fn_stat *s = &ns_profile.fns[fi];
        if (s->kind == kind && ns_profile_str_eq(s->name, name) && ns_profile_str_eq(s->lib, lib)) {
            return fi;
        }
    }
    ns_profile.fns_dropped++;
    return -1;
}

static i32 ns_profile_flame_get(i32 parent, i32 fn_index) {
    if (fn_index < 0) return -1;
    u32 h = ((u32)(parent + 2) * 0x9e3779b9u) ^ ((u32)(fn_index + 1) * 0x85ebca6bu);
    for (i32 n = 0; n < NS_PROFILE_FLAME_HASH; n++) {
        i32 slot = (i32)((h + (u32)n) & (NS_PROFILE_FLAME_HASH - 1));
        i32 idx = ns_profile.flame_hash[slot];
        if (idx == 0) {
            if (ns_profile.flame_count >= NS_PROFILE_MAX_FLAME) {
                ns_profile.flames_dropped++;
                return -1;
            }
            i32 fi = ns_profile.flame_count++;
            ns_profile.flames[fi] = (ns_profile_flame){
                .parent = parent,
                .fn_index = fn_index,
            };
            ns_profile.flame_hash[slot] = fi + 1;
            return fi;
        }
        i32 fi = idx - 1;
        if (ns_profile.flames[fi].parent == parent && ns_profile.flames[fi].fn_index == fn_index) {
            return fi;
        }
    }
    ns_profile.flames_dropped++;
    return -1;
}

static void ns_profile_fn_add(i32 fn_index, f64 elapsed_ms, f64 self_ms) {
    if (fn_index < 0) return;
    ns_profile_fn_stat *s = &ns_profile.fns[fn_index];
    if (s->calls == 0) {
        s->min_ms = elapsed_ms;
        s->max_ms = elapsed_ms;
    } else {
        if (elapsed_ms < s->min_ms) s->min_ms = elapsed_ms;
        if (elapsed_ms > s->max_ms) s->max_ms = elapsed_ms;
    }
    s->calls++;
    s->total_ms += elapsed_ms;
    s->self_ms += self_ms;
}

static void ns_profile_record_event(u8 kind, i32 depth, i32 fn_index, f64 start_ms, f64 elapsed_ms, f64 self_ms) {
    // Keep aggregates for every call, but omit micro-scopes from the timeline
    // so a hot leaf helper cannot balloon the on-disk report into gigabytes.
    if (kind == NS_PROFILE_EVENT_SCOPE && elapsed_ms < NS_PROFILE_TIMELINE_MIN_MS) {
        ns_profile.timeline_skipped++;
        return;
    }
    i32 n = (i32)ns_array_length(ns_profile.events);
    if (n >= NS_PROFILE_MAX_TIMELINE_EVENTS) {
        ns_profile.timeline_skipped++;
        return;
    }
    ns_profile_ensure_main_thread();
    // name/lib live in fns[fn_index] - the shared string pool. Samples only
    // keep the pool index so millions of events stay compact in RAM.
    ns_profile_event ev = {
        .kind = kind,
        .depth = depth,
        .thread = ns_profile.current_thread,
        .fn_index = fn_index,
        .start_ms = start_ms - ns_profile.start_ms,
        .elapsed_ms = elapsed_ms,
        .self_ms = self_ms,
    };
    ns_array_push(ns_profile.events, ev);
}

void ns_profile_scope_enter(ns_str name, ns_str lib) {
    if (!ns_profile.enabled) return;
    if (ns_profile.open_len >= NS_PROFILE_MAX_STACK) {
        ns_profile.stack_overflows++;
        return;
    }
    i32 fn_index = ns_profile_fn_get(name, lib, NS_PROFILE_EVENT_SCOPE);
    i32 parent = ns_profile.open_len > 0 ? ns_profile.open[ns_profile.open_len - 1].flame_index : -1;
    i32 flame_index = ns_profile_flame_get(parent, fn_index);
    ns_profile.open[ns_profile.open_len++] = (ns_profile_open){
        .fn_index = fn_index,
        .flame_index = flame_index,
        .child_ms = 0.0,
    };
}

void ns_profile_record_ffi(ns_str name, ns_str lib, f64 start_ms, f64 elapsed_ms) {
    if (!ns_profile.enabled) return;

    ns_profile.ffi_calls++;
    ns_profile.ffi_total_ms += elapsed_ms;

    i32 fn_index = ns_profile_fn_get(name, lib, NS_PROFILE_EVENT_FFI);
    i32 parent = ns_profile.open_len > 0 ? ns_profile.open[ns_profile.open_len - 1].flame_index : -1;
    i32 flame_index = ns_profile_flame_get(parent, fn_index);
    i32 depth = ns_profile.open_len;
    if (ns_profile.open_len > 0) {
        ns_profile.open[ns_profile.open_len - 1].child_ms += elapsed_ms;
    }
    ns_profile_fn_add(fn_index, elapsed_ms, elapsed_ms);
    if (flame_index >= 0) {
        ns_profile.flames[flame_index].calls++;
        ns_profile.flames[flame_index].total_ms += elapsed_ms;
        ns_profile.flames[flame_index].self_ms += elapsed_ms;
    }
    ns_profile_record_event(NS_PROFILE_EVENT_FFI, depth, fn_index, start_ms, elapsed_ms, elapsed_ms);
}

void ns_profile_record_scope(ns_str name, ns_str lib, i32 depth, f64 start_ms, f64 elapsed_ms) {
    if (!ns_profile.enabled) return;

    ns_profile.scope_calls++;

    i32 fn_index = -1;
    i32 flame_index = -1;
    f64 self_ms = elapsed_ms;

    if (ns_profile.open_len > 0) {
        ns_profile_open o = ns_profile.open[--ns_profile.open_len];
        fn_index = o.fn_index;
        flame_index = o.flame_index;
        self_ms = elapsed_ms - o.child_ms;
        if (self_ms < 0.0) self_ms = 0.0;
        if (ns_profile.open_len > 0) {
            ns_profile.open[ns_profile.open_len - 1].child_ms += elapsed_ms;
        }
        depth = ns_profile.open_len;
    } else {
        fn_index = ns_profile_fn_get(name, lib, NS_PROFILE_EVENT_SCOPE);
        flame_index = ns_profile_flame_get(-1, fn_index);
    }

    ns_profile.scope_self_ms += self_ms;
    ns_profile_fn_add(fn_index, elapsed_ms, self_ms);
    if (flame_index >= 0) {
        ns_profile.flames[flame_index].calls++;
        ns_profile.flames[flame_index].total_ms += elapsed_ms;
        ns_profile.flames[flame_index].self_ms += self_ms;
    }

    ns_profile_record_event(NS_PROFILE_EVENT_SCOPE, depth, fn_index, start_ms, elapsed_ms, self_ms);
}

static int ns_profile_fn_cmp(const void *a, const void *b) {
    const ns_profile_fn_stat *x = a;
    const ns_profile_fn_stat *y = b;
    if (x->self_ms < y->self_ms) return 1;
    if (x->self_ms > y->self_ms) return -1;
    if (x->total_ms < y->total_ms) return 1;
    if (x->total_ms > y->total_ms) return -1;
    if (x->calls < y->calls) return 1;
    if (x->calls > y->calls) return -1;
    return 0;
}

static int ns_profile_event_cmp(const void *a, const void *b) {
    const ns_profile_event *x = a;
    const ns_profile_event *y = b;
    if (x->start_ms < y->start_ms) return -1;
    if (x->start_ms > y->start_ms) return 1;
    if (x->thread < y->thread) return -1;
    if (x->thread > y->thread) return 1;
    if (x->kind == NS_PROFILE_EVENT_SCOPE && y->kind != NS_PROFILE_EVENT_SCOPE) return -1;
    if (x->kind != NS_PROFILE_EVENT_SCOPE && y->kind == NS_PROFILE_EVENT_SCOPE) return 1;
    if (x->depth < y->depth) return -1;
    if (x->depth > y->depth) return 1;
    return 0;
}

static int ns_profile_flame_self_cmp(const void *a, const void *b) {
    const i32 *ia = a;
    const i32 *ib = b;
    const ns_profile_flame *x = &ns_profile.flames[*ia];
    const ns_profile_flame *y = &ns_profile.flames[*ib];
    if (x->self_ms < y->self_ms) return 1;
    if (x->self_ms > y->self_ms) return -1;
    if (x->total_ms < y->total_ms) return 1;
    if (x->total_ms > y->total_ms) return -1;
    return 0;
}

static void ns_profile_write_symbol(FILE *f, ns_str lib, ns_str name) {
    if (lib.len > 0) fprintf(f, "%.*s::", lib.len, lib.data);
    fprintf(f, "%.*s", name.len, name.data);
}

static void ns_profile_write_stack(FILE *f, i32 flame_index) {
    i32 chain[NS_PROFILE_MAX_STACK];
    i32 n = 0;
    for (i32 i = flame_index; i >= 0 && n < NS_PROFILE_MAX_STACK; i = ns_profile.flames[i].parent) {
        chain[n++] = i;
    }
    for (i32 i = n - 1; i >= 0; i--) {
        if (i < n - 1) fputc(';', f);
        ns_profile_fn_stat *s = &ns_profile.fns[ns_profile.flames[chain[i]].fn_index];
        ns_profile_write_symbol(f, s->lib, s->name);
    }
}

static void ns_profile_sort_events(void) {
    i32 n = (i32)ns_array_length(ns_profile.events);
    if (n <= 1) return;
    qsort(ns_profile.events, (size_t)n, sizeof(ns_profile_event), ns_profile_event_cmp);
}

static ns_bool ns_profile_same_span(const ns_profile_event *a, const ns_profile_event *b) {
    if (a->kind != b->kind) return false;
    if (a->thread != b->thread) return false;
    if (a->depth != b->depth) return false;
    if (a->fn_index != b->fn_index) return false;
    return b->start_ms <= a->start_ms + a->elapsed_ms + 0.05;
}

static void ns_profile_coalesce_events(void) {
    ns_profile_sort_events();
    i32 n = (i32)ns_array_length(ns_profile.events);
    if (n <= 1) return;
    i32 w = 0;
    for (i32 r = 1; r < n; r++) {
        ns_profile_event *a = &ns_profile.events[w];
        ns_profile_event *b = &ns_profile.events[r];
        if (ns_profile_same_span(a, b)) {
            f64 a_end = a->start_ms + a->elapsed_ms;
            f64 b_end = b->start_ms + b->elapsed_ms;
            if (b_end > a_end) a->elapsed_ms = b_end - a->start_ms;
            a->self_ms += b->self_ms;
        } else {
            w++;
            if (w != r) ns_profile.events[w] = ns_profile.events[r];
        }
    }
    ns_array_set_length(ns_profile.events, w + 1);
}

static void ns_profile_write_u16(u8 **p, u16 v) {
    (*p)[0] = (u8)(v & 0xff);
    (*p)[1] = (u8)((v >> 8) & 0xff);
    *p += 2;
}

static void ns_profile_write_u32(u8 **p, u32 v) {
    (*p)[0] = (u8)(v & 0xff);
    (*p)[1] = (u8)((v >> 8) & 0xff);
    (*p)[2] = (u8)((v >> 16) & 0xff);
    (*p)[3] = (u8)((v >> 24) & 0xff);
    *p += 4;
}

static void ns_profile_write_i32(u8 **p, i32 v) {
    ns_profile_write_u32(p, (u32)v);
}

static i32 ns_profile_ms_to_us(f64 ms) {
    if (ms <= 0.0) return 0;
    f64 us = ms * 1000.0;
    if (us > 2147483647.0) return 2147483647;
    return (i32)(us + 0.5);
}

static ns_bool ns_profile_write_timeline_blob(const char *path, i32 event_count, i32 *out_bytes, ns_bool *out_zstd) {
    if (out_bytes) *out_bytes = 0;
    if (out_zstd) *out_zstd = false;
    if (!path || event_count < 0) return false;

    // map[fn_index] -> dense timeline symbol id (first-use order).
    i32 map[NS_PROFILE_MAX_FNS];
    for (i32 i = 0; i < NS_PROFILE_MAX_FNS; i++) map[i] = -1;
    i32 sym_count = 0;
    for (i32 i = 0; i < event_count; i++) {
        i32 fi = ns_profile.events[i].fn_index;
        if (fi < 0 || fi >= ns_profile.fn_count) continue;
        if (map[fi] < 0) map[fi] = sym_count++;
    }

    i32 order[NS_PROFILE_MAX_FNS];
    for (i32 i = 0; i < sym_count; i++) order[i] = -1;
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        if (map[i] >= 0 && map[i] < sym_count) order[map[i]] = i;
    }

    szt header = 4 + 4 + 4 + 4 + 4 + 4; // magic ver flags threads syms events
    szt strings = 0;
    for (i32 i = 0; i < ns_profile.thread_count; i++) {
        strings += 2 + (szt)ns_profile.threads[i].len;
    }
    for (i32 i = 0; i < sym_count; i++) {
        i32 fi = order[i];
        if (fi < 0) continue;
        strings += 1 + 2 + 2 + (szt)ns_profile.fns[fi].lib.len + (szt)ns_profile.fns[fi].name.len;
    }
    szt payload = header + strings + (szt)event_count * sizeof(ns_profile_tl_event);
    if (payload > (szt)0x7fffffff) return false;

    u8 *raw = (u8 *)ns_malloc(payload);
    if (!raw) return false;
    u8 *p = raw;
    ns_profile_write_u32(&p, NS_PROFILE_TL_MAGIC);
    ns_profile_write_u32(&p, NS_PROFILE_TL_VERSION);
    ns_profile_write_u32(&p, 0); // flags filled after compression decision
    ns_profile_write_u32(&p, (u32)ns_profile.thread_count);
    ns_profile_write_u32(&p, (u32)sym_count);
    ns_profile_write_u32(&p, (u32)event_count);

    for (i32 i = 0; i < ns_profile.thread_count; i++) {
        ns_str t = ns_profile.threads[i];
        u16 len = t.len > 0xffff ? 0xffff : (u16)t.len;
        ns_profile_write_u16(&p, len);
        if (len > 0) {
            memcpy(p, t.data, len);
            p += len;
        }
    }
    for (i32 i = 0; i < sym_count; i++) {
        i32 fi = order[i];
        ns_profile_fn_stat *s = &ns_profile.fns[fi];
        u8 kind = s->kind;
        u16 lib_len = s->lib.len > 0xffff ? 0xffff : (u16)s->lib.len;
        u16 name_len = s->name.len > 0xffff ? 0xffff : (u16)s->name.len;
        *p++ = kind;
        ns_profile_write_u16(&p, lib_len);
        ns_profile_write_u16(&p, name_len);
        if (lib_len > 0) {
            memcpy(p, s->lib.data, lib_len);
            p += lib_len;
        }
        if (name_len > 0) {
            memcpy(p, s->name.data, name_len);
            p += name_len;
        }
    }
    for (i32 i = 0; i < event_count; i++) {
        ns_profile_event *e = &ns_profile.events[i];
        i32 ti = e->thread;
        if (ti < 0 || ti >= ns_profile.thread_count) ti = 0;
        i32 si = 0;
        if (e->fn_index >= 0 && e->fn_index < ns_profile.fn_count && map[e->fn_index] >= 0) {
            si = map[e->fn_index];
        }
        if (si > 0xffff) si = 0xffff;
        i32 depth = e->depth;
        if (depth < 0) depth = 0;
        if (depth > 255) depth = 255;
        *p++ = e->kind;
        *p++ = (u8)depth;
        ns_profile_write_u16(&p, (u16)ti);
        ns_profile_write_u16(&p, (u16)si);
        ns_profile_write_u16(&p, 0);
        ns_profile_write_i32(&p, ns_profile_ms_to_us(e->start_ms));
        ns_profile_write_i32(&p, ns_profile_ms_to_us(e->elapsed_ms));
        ns_profile_write_i32(&p, ns_profile_ms_to_us(e->self_ms));
    }
    szt raw_len = (szt)(p - raw);

    ns_profile_load_compress();
    ns_bool used_zstd = false;
    const u8 *out = raw;
    szt out_len = raw_len;
    u8 *zbuf = ns_null;
    if (ns_profile_zstd_bound && ns_profile_zstd_encode && raw_len <= (szt)0x7fffffff) {
        i32 bound = ns_profile_zstd_bound((i32)raw_len);
        if (bound > 0) {
            zbuf = (u8 *)ns_malloc((szt)bound);
            if (zbuf) {
                i32 zlen = ns_profile_zstd_encode(raw, (i32)raw_len, zbuf, bound, 1);
                if (zlen > 0 && (szt)zlen < raw_len) {
                    // Mark the uncompressed payload's flags before wrapping so
                    // the decoder knows the frame carries zstd content size.
                    // The on-disk file is the zstd frame alone; the viewer
                    // decompresses to the NSTL payload.
                    used_zstd = true;
                    out = zbuf;
                    out_len = (szt)zlen;
                }
            }
        }
    }

    i8 blob_path[4096];
    snprintf(blob_path, sizeof(blob_path), "%s.tl%s", path, used_zstd ? ".zst" : "");
    FILE *bf = fopen(blob_path, "wb");
    ns_bool ok = false;
    if (bf) {
        ok = fwrite(out, 1, out_len, bf) == out_len;
        fclose(bf);
    }
    if (ok) {
        if (out_bytes) *out_bytes = (i32)out_len;
        if (out_zstd) *out_zstd = used_zstd;
    }
    ns_free(zbuf);
    ns_free(raw);
    return ok;
}

static const char *ns_profile_basename(const char *path) {
    if (!path) return "ns.profile";
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *bslash = strrchr(path, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

void ns_profile_write_report(FILE *f, const char *path, f64 elapsed_ms, i32 argc, i8 **argv) {
    f64 ffi_ms = ns_profile.ffi_total_ms;
    f64 ffi_pct = elapsed_ms > 0.0 ? (ffi_ms / elapsed_ms) * 100.0 : 0.0;
    i32 raw_events = (i32)ns_array_length(ns_profile.events);
    ns_profile_coalesce_events();
    i32 event_count = (i32)ns_array_length(ns_profile.events);
    ns_profile_ensure_main_thread();

    i32 ffi_event_count = 0;
    i32 scope_event_count = 0;
    for (i32 i = 0; i < event_count; i++) {
        if (ns_profile.events[i].kind == NS_PROFILE_EVENT_SCOPE) scope_event_count++;
        else ffi_event_count++;
    }

    i32 scope_symbols = 0;
    i32 ffi_symbols = 0;
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        if (ns_profile.fns[i].kind == NS_PROFILE_EVENT_SCOPE) scope_symbols++;
        else ffi_symbols++;
    }

    i32 blob_bytes = 0;
    ns_bool blob_zstd = false;
    ns_bool blob_ok = false;
    if (path && event_count > 0) {
        blob_ok = ns_profile_write_timeline_blob(path, event_count, &blob_bytes, &blob_zstd);
    }

    fprintf(f, "format: ns-profile-v6\n");
    fprintf(f, "elapsed_ms: %.3f\n", elapsed_ms);
    fprintf(f, "ffi_calls: %llu\n", (unsigned long long)ns_profile.ffi_calls);
    fprintf(f, "ffi_ms: %.3f\n", ffi_ms);
    fprintf(f, "ffi_pct: %.1f\n", ffi_pct);
    fprintf(f, "ffi_symbols: %d\n", ffi_symbols);
    fprintf(f, "ffi_events: %d\n", ffi_event_count);
    fprintf(f, "scope_calls: %llu\n", (unsigned long long)ns_profile.scope_calls);
    fprintf(f, "scope_self_ms: %.3f\n", ns_profile.scope_self_ms);
    fprintf(f, "scope_symbols: %d\n", scope_symbols);
    fprintf(f, "scope_events: %d\n", scope_event_count);
    fprintf(f, "timeline_events: %d\n", event_count);
    fprintf(f, "timeline_raw: %d\n", raw_events);
    fprintf(f, "timeline_skipped: %llu\n", (unsigned long long)ns_profile.timeline_skipped);
    fprintf(f, "timeline_min_ms: %.3f\n", NS_PROFILE_TIMELINE_MIN_MS);
    fprintf(f, "flame_frames: %d\n", ns_profile.flame_count);
    fprintf(f, "threads: %d\n", ns_profile.thread_count);
    if (blob_ok) {
        fprintf(f, "timeline_blob: %s.tl%s\n", ns_profile_basename(path), blob_zstd ? ".zst" : "");
        fprintf(f, "timeline_blob_bytes: %d\n", blob_bytes);
        fprintf(f, "timeline_blob_codec: %s\n", blob_zstd ? "zstd" : "raw");
    }
    fprintf(f, "argv:");
    for (i32 i = 0; i < argc; i++) fprintf(f, " %s", argv[i]);
    fprintf(f, "\n");

    fprintf(f, "thread_table: id name\n");
    for (i32 i = 0; i < ns_profile.thread_count; i++) {
        fprintf(f, "thread: %d %.*s\n", i, ns_profile.threads[i].len, ns_profile.threads[i].data);
    }

    ns_profile_fn_stat ordered[NS_PROFILE_MAX_FNS];
    memcpy(ordered, ns_profile.fns, sizeof(ns_profile_fn_stat) * (size_t)ns_profile.fn_count);
    qsort(ordered, (size_t)ns_profile.fn_count, sizeof(ns_profile_fn_stat), ns_profile_fn_cmp);
    fprintf(f, "fn_table: kind calls total_ms self_ms avg_ms min_ms max_ms symbol\n");
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ordered[i];
        f64 avg_ms = s->calls ? s->total_ms / (f64)s->calls : 0.0;
        const char *kind = s->kind == NS_PROFILE_EVENT_SCOPE ? "scope" : "ffi";
        fprintf(f, "fn: %s %llu %.3f %.3f %.4f %.4f %.4f ", kind, (unsigned long long)s->calls, s->total_ms, s->self_ms,
                avg_ms, s->min_ms, s->max_ms);
        ns_profile_write_symbol(f, s->lib, s->name);
        fputc('\n', f);
    }
    if (ns_profile.fns_dropped > 0) {
        fprintf(f, "fn_dropped: %llu\n", (unsigned long long)ns_profile.fns_dropped);
    }
    fprintf(f, "ffi_table: calls total_ms avg_ms min_ms max_ms symbol\n");
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ordered[i];
        if (s->kind != NS_PROFILE_EVENT_FFI) continue;
        f64 avg_ms = s->calls ? s->total_ms / (f64)s->calls : 0.0;
        fprintf(f, "ffi: %llu %.3f %.4f %.4f %.4f ", (unsigned long long)s->calls, s->total_ms, avg_ms, s->min_ms, s->max_ms);
        ns_profile_write_symbol(f, s->lib, s->name);
        fputc('\n', f);
    }

    fprintf(f, "flame: calls total_ms self_ms stack\n");
    i32 order[NS_PROFILE_MAX_FLAME];
    for (i32 i = 0; i < ns_profile.flame_count; i++) order[i] = i;
    qsort(order, (size_t)ns_profile.flame_count, sizeof(i32), ns_profile_flame_self_cmp);
    for (i32 i = 0; i < ns_profile.flame_count; i++) {
        ns_profile_flame *fl = &ns_profile.flames[order[i]];
        fprintf(f, "flame: %llu %.3f %.3f ", (unsigned long long)fl->calls, fl->total_ms, fl->self_ms);
        ns_profile_write_stack(f, order[i]);
        fputc('\n', f);
    }
    if (ns_profile.flames_dropped > 0) {
        fprintf(f, "flame_dropped: %llu\n", (unsigned long long)ns_profile.flames_dropped);
    }
    if (ns_profile.stack_overflows > 0) {
        fprintf(f, "stack_overflows: %llu\n", (unsigned long long)ns_profile.stack_overflows);
    }
}

void ns_profile_write_text(FILE *f, f64 elapsed_ms, i32 argc, i8 **argv) {
    ns_profile_write_report(f, ns_null, elapsed_ms, argc, argv);
}

static const char *ns_profile_pct_color(f64 pct) {
    if (pct >= 25.0) return ns_color_err;
    if (pct >= 10.0) return ns_color_wrn;
    if (pct >= 3.0) return ns_color_log;
    return ns_color_ign;
}

void ns_profile_print_summary(FILE *out, f64 elapsed_ms) {
    f64 ffi_ms = ns_profile.ffi_total_ms;
    f64 ffi_pct = elapsed_ms > 0.0 ? (ffi_ms / elapsed_ms) * 100.0 : 0.0;
    fprintf(out, "\n");
    fprintf(out, "profile  %.3f ms total\n", elapsed_ms);
    fprintf(out, "  scopes      %llu   self %.3f ms\n", (unsigned long long)ns_profile.scope_calls, ns_profile.scope_self_ms);
    fprintf(out, "  ffi calls   %llu   %s%.3f ms / %.1f%%%s\n", (unsigned long long)ns_profile.ffi_calls,
            ns_profile_pct_color(ffi_pct), ffi_ms, ffi_pct, ns_color_nil);
    fprintf(out, "  timeline    %d events\n", (i32)ns_array_length(ns_profile.events));
    fprintf(out, "  stacks      %d unique\n", ns_profile.flame_count);
    fprintf(out, "  threads     %d\n", ns_profile.thread_count > 0 ? ns_profile.thread_count : 1);
    fprintf(out, "  share       %s>=25%%%s  %s>=10%%%s  %s>=3%%%s  %s<3%%%s\n",
            ns_color_err, ns_color_nil, ns_color_wrn, ns_color_nil,
            ns_color_log, ns_color_nil, ns_color_ign, ns_color_nil);

    ns_profile_fn_stat ordered[NS_PROFILE_MAX_FNS];
    memcpy(ordered, ns_profile.fns, sizeof(ns_profile_fn_stat) * (size_t)ns_profile.fn_count);
    qsort(ordered, (size_t)ns_profile.fn_count, sizeof(ns_profile_fn_stat), ns_profile_fn_cmp);

    fprintf(out, "\nhot functions by self time:\n");
    i32 fn_show = ns_profile.fn_count < 12 ? ns_profile.fn_count : 12;
    for (i32 i = 0; i < fn_show; i++) {
        ns_profile_fn_stat *s = &ordered[i];
        f64 pct = elapsed_ms > 0.0 ? (s->self_ms / elapsed_ms) * 100.0 : 0.0;
        const char *kind = s->kind == NS_PROFILE_EVENT_SCOPE ? "scope" : "ffi  ";
        fprintf(out, "  %s%10.3f ms  %5.1f%%  %8llux  %s  ", ns_profile_pct_color(pct), s->self_ms, pct,
                (unsigned long long)s->calls, kind);
        ns_profile_write_symbol(out, s->lib, s->name);
        fprintf(out, "%s\n", ns_color_nil);
    }

    fprintf(out, "\nhot stacks by self time:\n");
    i32 order[NS_PROFILE_MAX_FLAME];
    for (i32 i = 0; i < ns_profile.flame_count; i++) order[i] = i;
    qsort(order, (size_t)ns_profile.flame_count, sizeof(i32), ns_profile_flame_self_cmp);
    i32 st_show = ns_profile.flame_count < 8 ? ns_profile.flame_count : 8;
    for (i32 i = 0; i < st_show; i++) {
        ns_profile_flame *fl = &ns_profile.flames[order[i]];
        f64 pct = elapsed_ms > 0.0 ? (fl->self_ms / elapsed_ms) * 100.0 : 0.0;
        fprintf(out, "  %s%10.3f ms  %5.1f%%  ", ns_profile_pct_color(pct), fl->self_ms, pct);
        ns_profile_write_stack(out, order[i]);
        fprintf(out, "%s\n", ns_color_nil);
    }

    fprintf(out, "\nrun: ns profiler\n");
}
