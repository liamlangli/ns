#include "ns_native_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <unistd.h>

// A double-clicked app bundle starts with no useful working directory, so every
// relative path in the program would read nothing. The build packages the
// project's declared assets into Contents/Resources under the names they have
// in the project, so entering that directory before the program runs makes the
// same relative path read the same file it does from an interpreted run.
//
// Only an executable inside a bundle moves: a plain command-line program keeps
// the directory it was started from, which is the one its arguments name.
static void ns_rt_enter_bundle_resources(void) {
    static const char macos_suffix[] = "/Contents/MacOS";
    static const char resources[] = "/Contents/Resources";
    char executable[PATH_MAX];
    uint32_t size = (uint32_t)sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0) return;

    char bundle[PATH_MAX];
    if (!realpath(executable, bundle)) return;
    char *name = strrchr(bundle, '/');
    if (!name) return;
    *name = '\0';

    size_t length = strlen(bundle);
    size_t suffix_length = sizeof(macos_suffix) - 1;
    if (length < suffix_length || strcmp(bundle + length - suffix_length, macos_suffix) != 0) return;
    if (length - suffix_length + sizeof(resources) > sizeof(bundle)) return;
    memcpy(bundle + length - suffix_length, resources, sizeof(resources));
    if (chdir(bundle) != 0) {
        fprintf(stderr, "ns_rt: could not enter %s\n", bundle);
    }
}
#endif

#define NS_RT_GLOBAL_MAX 1024

static uint8_t *ns_rt_mem = NULL;
static uint32_t ns_rt_used = 16;
static uint32_t ns_rt_cap = 0;

static int64_t ns_rt_globals[NS_RT_GLOBAL_MAX];

static const char **ns_rt_strtab = NULL;
static const int32_t *ns_rt_strlens = NULL;
static int ns_rt_nstr = 0;
static int64_t *ns_rt_strcache = NULL;

#define NS_RT_CB_MAX 16
static struct {
    int64_t code;
    int64_t env;
} ns_rt_cb_slot[NS_RT_CB_MAX];
static int ns_rt_cb_n = 0;

static void ns_rt_grow(uint32_t need) {
    uint32_t cap = ns_rt_cap ? ns_rt_cap : 4096;
    while (cap < need) {
        uint32_t next = cap * 2;
        if (next < cap) {
            fprintf(stderr, "ns_rt: heap overflow\n");
            abort();
        }
        cap = next;
    }
    uint8_t *mem = (uint8_t *)realloc(ns_rt_mem, cap);
    if (!mem) {
        fprintf(stderr, "ns_rt: out of memory\n");
        abort();
    }
    if (cap > ns_rt_cap) memset(mem + ns_rt_cap, 0, cap - ns_rt_cap);
    ns_rt_mem = mem;
    ns_rt_cap = cap;
}

void ns_rt_init(void) {
    if (!ns_rt_mem) ns_rt_grow(4096);
    if (ns_rt_used < 16) ns_rt_used = 16;
#if defined(__APPLE__)
    // The allocator calls this on every miss, so the bundle is resolved once.
    static int entered_bundle = 0;
    if (!entered_bundle) {
        entered_bundle = 1;
        ns_rt_enter_bundle_resources();
    }
#endif
}

void ns_rt_reset(void) {
    ns_rt_used = 16;
    if (ns_rt_mem && ns_rt_cap) memset(ns_rt_mem, 0, ns_rt_cap);
    memset(ns_rt_globals, 0, sizeof(ns_rt_globals));
    ns_rt_cb_n = 0;
    memset(ns_rt_cb_slot, 0, sizeof(ns_rt_cb_slot));
    if (ns_rt_strcache) {
        free(ns_rt_strcache);
        ns_rt_strcache = NULL;
    }
}

void ns_rt_set_strtab(const char **tab, const int32_t *lens, int n) {
    ns_rt_strtab = tab;
    ns_rt_strlens = lens;
    ns_rt_nstr = n;
    if (ns_rt_strcache) {
        free(ns_rt_strcache);
        ns_rt_strcache = NULL;
    }
}

int64_t ns_rt_alloc(int64_t size) {
    ns_rt_init();
    if (size < 0) size = 0;
    uint32_t n = (uint32_t)size;
    uint32_t addr = (ns_rt_used + 7u) & ~7u;
    ns_rt_grow(addr + n + 16);
    ns_rt_used = addr + n;
    return (int64_t)addr;
}

static uint8_t *ns_rt_ptr(int64_t addr) {
    ns_rt_init();
    if (addr >= 0 && (uint64_t)addr < (uint64_t)ns_rt_cap) {
        return ns_rt_mem + (uint32_t)addr;
    }
    // Compiled `ref T` values from native modules are host pointers.
    if (addr > 0x10000) return (uint8_t *)(uintptr_t)addr;
    fprintf(stderr, "ns_rt: bad address %lld\n", (long long)addr);
    abort();
    return NULL;
}

int64_t ns_rt_clone(int64_t src, int64_t size) {
    int64_t dst = ns_rt_alloc(size);
    if (size > 0) memcpy(ns_rt_ptr(dst), ns_rt_ptr(src), (size_t)size);
    return dst;
}

int64_t ns_rt_load(int64_t addr, int64_t off, int64_t size) {
    uint8_t *p = ns_rt_ptr(addr + off);
    switch (size) {
    case 1: return (int64_t)(int8_t)p[0];
    case -1: return (int64_t)p[0];
    case 2: {
        int16_t v;
        memcpy(&v, p, 2);
        return (int64_t)v;
    }
    case -2: {
        uint16_t v;
        memcpy(&v, p, 2);
        return (int64_t)v;
    }
    case 8: {
        int64_t v;
        memcpy(&v, p, 8);
        return v;
    }
    default: {
        int32_t v;
        memcpy(&v, p, 4);
        return (int64_t)v;
    }
    }
}

void ns_rt_store(int64_t addr, int64_t off, int64_t val, int64_t size) {
    uint8_t *p = ns_rt_ptr(addr + off);
    switch (size) {
    case 1:
    case -1:
        p[0] = (uint8_t)val;
        break;
    case 2:
    case -2: {
        uint16_t v = (uint16_t)val;
        memcpy(p, &v, 2);
    } break;
    case 8:
        memcpy(p, &val, 8);
        break;
    default: {
        int32_t v = (int32_t)val;
        memcpy(p, &v, 4);
    } break;
    }
}

void ns_rt_copy(int64_t dst, int64_t doff, int64_t src, int64_t size) {
    if (size <= 0) return;
    memcpy(ns_rt_ptr(dst + doff), ns_rt_ptr(src), (size_t)size);
}

int64_t ns_rt_from_bytes(const char *s, int32_t len) {
    if (!s) {
        s = "";
        len = 0;
    }
    if (len < 0) len = (int32_t)strlen(s);
    int64_t desc = ns_rt_alloc(8 + len);
    int32_t bytes = (int32_t)(desc + 8);
    ns_rt_store(desc, 0, bytes, 4);
    ns_rt_store(desc, 4, len, 4);
    if (len > 0) memcpy(ns_rt_ptr(bytes), s, (size_t)len);
    return desc;
}

static void ns_rt_str_parts(int64_t str, int32_t *bytes, int32_t *len) {
    if (str <= 0) {
        *bytes = 0;
        *len = 0;
        return;
    }
    *bytes = (int32_t)ns_rt_load(str, 0, 4);
    *len = (int32_t)ns_rt_load(str, 4, 4);
}

static char *ns_rt_cstr(int64_t str) {
    int32_t bytes = 0, len = 0;
    ns_rt_str_parts(str, &bytes, &len);
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) abort();
    if (len > 0) memcpy(out, ns_rt_ptr(bytes), (size_t)len);
    out[len] = 0;
    return out;
}

int64_t ns_rt_array_new(int64_t count, int64_t stride) {
    if (count < 0) count = 0;
    if (stride < 0) stride = 0;
    int64_t payload = count * stride;
    int64_t arr = ns_rt_alloc(16 + payload);
    int32_t data = (int32_t)(arr + 16);
    ns_rt_store(arr, 0, data, 4);
    ns_rt_store(arr, 4, count, 4);
    ns_rt_store(arr, 8, count, 4);
    return arr;
}

static int32_t ns_rt_array_len(int64_t arr) {
    return (int32_t)ns_rt_load(arr, 4, 4);
}

void ns_rt_array_store(int64_t arr, int64_t idx, int64_t val, int64_t stride) {
    int32_t len = ns_rt_array_len(arr);
    if (idx < 0 || idx >= (int64_t)len) {
        fprintf(stderr, "ns_rt: array store oob %lld / %d\n", (long long)idx, len);
        abort();
    }
    int32_t data = (int32_t)ns_rt_load(arr, 0, 4);
    if (stride <= 8) {
        ns_rt_store(data, idx * stride, val, stride);
    } else {
        int64_t dst = (int64_t)data + idx * stride;
        memcpy(ns_rt_ptr(dst), ns_rt_ptr(val), (size_t)stride);
    }
}

int64_t ns_rt_array_slot(int64_t arr, int64_t idx, int64_t stride) {
    int32_t len = ns_rt_array_len(arr);
    if (idx < 0 || idx >= (int64_t)len) {
        fprintf(stderr, "ns_rt: array slot oob %lld / %d\n", (long long)idx, len);
        abort();
    }
    if (stride < 0) stride = -stride;
    if (stride <= 0) stride = 1;
    int32_t data = (int32_t)ns_rt_load(arr, 0, 4);
    return (int64_t)data + idx * stride;
}

int64_t ns_rt_array_index(int64_t arr, int64_t idx, int64_t stride) {
    int32_t len = ns_rt_array_len(arr);
    if (idx < 0 || idx >= (int64_t)len) {
        fprintf(stderr, "ns_rt: array index oob %lld / %d\n", (long long)idx, len);
        abort();
    }
    int32_t data = (int32_t)ns_rt_load(arr, 0, 4);
    int64_t step = stride;
    int64_t load_size = stride;
    if (stride == -1) {
        step = 1;
        load_size = -1;
    } else if (stride == -2) {
        step = 2;
        load_size = -2;
    } else if (stride <= 0) {
        step = 1;
        load_size = 1;
    }
    int64_t at = (int64_t)data + idx * step;
    if (step > 8) return at;
    return ns_rt_load(at, 0, load_size);
}

int64_t ns_rt_gget(int64_t idx) {
    if (idx < 0 || idx >= NS_RT_GLOBAL_MAX) return 0;
    return ns_rt_globals[idx];
}

void ns_rt_gset(int64_t idx, int64_t val) {
    if (idx < 0 || idx >= NS_RT_GLOBAL_MAX) return;
    ns_rt_globals[idx] = val;
}

int64_t ns_rt_intern(int64_t id) {
    if (id < 0 || id >= ns_rt_nstr || !ns_rt_strtab) return 0;
    if (!ns_rt_strcache) {
        ns_rt_strcache = (int64_t *)calloc((size_t)ns_rt_nstr, sizeof(int64_t));
        if (!ns_rt_strcache) abort();
    }
    if (ns_rt_strcache[id] != 0) return ns_rt_strcache[id];
    int32_t len = ns_rt_strlens ? ns_rt_strlens[id] : (int32_t)strlen(ns_rt_strtab[id] ? ns_rt_strtab[id] : "");
    const char *s = ns_rt_strtab[id] ? ns_rt_strtab[id] : "";
    int64_t desc = ns_rt_from_bytes(s, len);
    ns_rt_strcache[id] = desc;
    return desc;
}

int64_t ns_rt_print(int64_t str) {
    if (str <= 0) {
        fputc('\n', stdout);
        return 0;
    }
    int32_t bytes = 0, len = 0;
    ns_rt_str_parts(str, &bytes, &len);
    if (len > 0) fwrite(ns_rt_ptr(bytes), 1, (size_t)len, stdout);
    return 0;
}

int64_t ns_rt_strcat(int64_t a, int64_t b) {
    int32_t ab = 0, al = 0, bb = 0, bl = 0;
    ns_rt_str_parts(a, &ab, &al);
    ns_rt_str_parts(b, &bb, &bl);
    int64_t desc = ns_rt_alloc(8 + (int64_t)al + (int64_t)bl);
    int32_t bytes = (int32_t)(desc + 8);
    ns_rt_store(desc, 0, bytes, 4);
    ns_rt_store(desc, 4, (int64_t)al + (int64_t)bl, 4);
    if (al > 0) memcpy(ns_rt_ptr(bytes), ns_rt_ptr(ab), (size_t)al);
    if (bl > 0) memcpy(ns_rt_ptr(bytes + al), ns_rt_ptr(bb), (size_t)bl);
    return desc;
}

int64_t ns_rt_strcmp(int64_t a, int64_t b) {
    int32_t ab = 0, al = 0, bb = 0, bl = 0;
    ns_rt_str_parts(a, &ab, &al);
    ns_rt_str_parts(b, &bb, &bl);
    int32_t n = al < bl ? al : bl;
    int c = 0;
    if (n > 0) c = memcmp(ns_rt_ptr(ab), ns_rt_ptr(bb), (size_t)n);
    if (c == 0) c = al - bl;
    if (c < 0) return -1;
    if (c > 0) return 1;
    return 0;
}

int64_t ns_rt_substr(int64_t str, int64_t start, int64_t len) {
    int32_t bytes = 0, slen = 0;
    ns_rt_str_parts(str, &bytes, &slen);
    if (start < 0) start = 0;
    if (start > slen) start = slen;
    if (len < 0) len = 0;
    if (start + len > slen) len = slen - start;
    if (len == 0) return ns_rt_from_bytes("", 0);
    return ns_rt_from_bytes((const char *)ns_rt_ptr(bytes + (int32_t)start), (int32_t)len);
}

int64_t ns_rt_unescape(int64_t str) {
    int32_t bytes = 0, len = 0;
    ns_rt_str_parts(str, &bytes, &len);
    const uint8_t *s = len > 0 ? ns_rt_ptr(bytes) : (const uint8_t *)"";
    uint8_t *tmp = (uint8_t *)malloc((size_t)len * 4 + 1);
    if (!tmp) abort();
    int32_t o = 0;
    for (int32_t i = 0; i < len; ++i) {
        if (s[i] != '\\' || i + 1 >= len) {
            tmp[o++] = s[i];
            continue;
        }
        ++i;
        switch (s[i]) {
        case 'n': tmp[o++] = '\n'; break;
        case 't': tmp[o++] = '\t'; break;
        case 'r': tmp[o++] = '\r'; break;
        case '\\': tmp[o++] = '\\'; break;
        case '"': tmp[o++] = '"'; break;
        case '{': tmp[o++] = '{'; break;
        case 'u':
            if (i + 1 < len && s[i + 1] == '{') {
                i += 2;
                uint32_t cp = 0;
                while (i < len && s[i] != '}') {
                    unsigned char ch = s[i++];
                    cp <<= 4;
                    if (ch >= '0' && ch <= '9') cp |= (uint32_t)(ch - '0');
                    else if (ch >= 'a' && ch <= 'f') cp |= (uint32_t)(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F') cp |= (uint32_t)(ch - 'A' + 10);
                }
                if (cp < 0x80u) {
                    tmp[o++] = (uint8_t)cp;
                } else if (cp < 0x800u) {
                    tmp[o++] = (uint8_t)(0xC0u | (cp >> 6));
                    tmp[o++] = (uint8_t)(0x80u | (cp & 0x3Fu));
                } else if (cp < 0x10000u) {
                    tmp[o++] = (uint8_t)(0xE0u | (cp >> 12));
                    tmp[o++] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
                    tmp[o++] = (uint8_t)(0x80u | (cp & 0x3Fu));
                } else {
                    tmp[o++] = (uint8_t)(0xF0u | (cp >> 18));
                    tmp[o++] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
                    tmp[o++] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
                    tmp[o++] = (uint8_t)(0x80u | (cp & 0x3Fu));
                }
            } else {
                tmp[o++] = 'u';
            }
            break;
        default:
            tmp[o++] = s[i];
            break;
        }
    }
    int64_t out = ns_rt_from_bytes((const char *)tmp, o);
    free(tmp);
    return out;
}

int64_t ns_rt_utf8_len(int64_t str) {
    int32_t bytes = 0, len = 0;
    ns_rt_str_parts(str, &bytes, &len);
    const uint8_t *s = len > 0 ? ns_rt_ptr(bytes) : (const uint8_t *)"";
    int64_t n = 0;
    for (int32_t i = 0; i < len; ) {
        uint8_t c = s[i];
        if ((c & 0x80u) == 0) i += 1;
        else if ((c & 0xE0u) == 0xC0u) i += 2;
        else if ((c & 0xF0u) == 0xE0u) i += 3;
        else i += 4;
        if (i > len) break;
        n++;
    }
    return n;
}

int64_t ns_rt_itos(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return ns_rt_from_bytes(buf, n);
}

int64_t ns_rt_utos(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(uint64_t)v);
    return ns_rt_from_bytes(buf, n);
}

int64_t ns_rt_btos(int64_t v) {
    return v ? ns_rt_from_bytes("true", 4) : ns_rt_from_bytes("false", 5);
}

int64_t ns_rt_ftos(int64_t bits) {
    double x;
    memcpy(&x, &bits, 8);
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.9g", x);
    return ns_rt_from_bytes(buf, n);
}

int64_t ns_rt_stof(int64_t str) {
    char *s = ns_rt_cstr(str);
    double x = strtod(s, NULL);
    free(s);
    int64_t bits = 0;
    memcpy(&bits, &x, 8);
    return bits;
}

static int64_t ns_rt_f64_unop(int64_t bits, double (*fn)(double)) {
    double x;
    memcpy(&x, &bits, 8);
    double y = fn(x);
    int64_t out = 0;
    memcpy(&out, &y, 8);
    return out;
}

int64_t ns_rt_fmod(int64_t a_bits, int64_t b_bits) {
    double a, b;
    memcpy(&a, &a_bits, 8);
    memcpy(&b, &b_bits, 8);
    double r = fmod(a, b);
    int64_t out = 0;
    memcpy(&out, &r, 8);
    return out;
}

int64_t ns_rt_fmodf(int64_t a_bits, int64_t b_bits) {
    float a, b;
    memcpy(&a, &a_bits, 4);
    memcpy(&b, &b_bits, 4);
    float r = fmodf(a, b);
    int64_t out = 0;
    memcpy(&out, &r, 4);
    return out;
}

int64_t ns_rt_sqrt(int64_t bits) { return ns_rt_f64_unop(bits, sqrt); }
int64_t ns_rt_sin(int64_t bits) { return ns_rt_f64_unop(bits, sin); }
int64_t ns_rt_cos(int64_t bits) { return ns_rt_f64_unop(bits, cos); }
int64_t ns_rt_tan(int64_t bits) { return ns_rt_f64_unop(bits, tan); }

int64_t ns_rt_atan2(int64_t y_bits, int64_t x_bits) {
    double y, x;
    memcpy(&y, &y_bits, 8);
    memcpy(&x, &x_bits, 8);
    double r = atan2(y, x);
    int64_t out = 0;
    memcpy(&out, &r, 8);
    return out;
}

#define NS_RT_MAP_SET 1
#define NS_RT_MAP_STRKEY 2
#define NS_RT_MAP_SLOT 24

static uint64_t ns_rt_hash_bytes(const uint8_t *data, int32_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (int32_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t ns_rt_hash_int(uint64_t n) {
    n ^= n >> 33;
    n *= 0xff51afd7ed558ccdULL;
    n ^= n >> 33;
    n *= 0xc4ceb9fe1a85ec53ULL;
    n ^= n >> 33;
    return n;
}

static uint64_t ns_rt_map_hash(int64_t map, int64_t key) {
    int32_t flags = (int32_t)ns_rt_load(map, 12, 4);
    if (flags & NS_RT_MAP_STRKEY) {
        int32_t bytes = 0, len = 0;
        ns_rt_str_parts(key, &bytes, &len);
        return ns_rt_hash_bytes(len > 0 ? ns_rt_ptr(bytes) : (const uint8_t *)"", len);
    }
    return ns_rt_hash_int((uint64_t)key);
}

static int ns_rt_map_keyeq(int64_t map, int64_t a, int64_t b) {
    int32_t flags = (int32_t)ns_rt_load(map, 12, 4);
    if (!(flags & NS_RT_MAP_STRKEY)) return a == b;
    int32_t ab = 0, al = 0, bb = 0, bl = 0;
    ns_rt_str_parts(a, &ab, &al);
    ns_rt_str_parts(b, &bb, &bl);
    if (al != bl) return 0;
    if (al <= 0) return 1;
    return memcmp(ns_rt_ptr(ab), ns_rt_ptr(bb), (size_t)al) == 0;
}

static int64_t ns_rt_map_slot(int64_t map, int64_t idx) {
    return map + 16 + idx * NS_RT_MAP_SLOT;
}

int64_t ns_rt_map_new(int64_t cap, int64_t flags) {
    if (cap < 1) cap = 1;
    int64_t map = ns_rt_alloc(16 + cap * NS_RT_MAP_SLOT);
    ns_rt_store(map, 0, 0, 4);
    ns_rt_store(map, 4, 0, 4);
    ns_rt_store(map, 8, cap, 4);
    ns_rt_store(map, 12, flags, 4);
    return map;
}

static int64_t ns_rt_map_probe(int64_t map, int64_t key, int64_t *insert_slot) {
    if (insert_slot) *insert_slot = 0;
    int32_t cap = (int32_t)ns_rt_load(map, 8, 4);
    if (cap <= 0) return 0;
    uint64_t h = ns_rt_map_hash(map, key);
    int32_t start = (int32_t)(h % (uint64_t)cap);
    int64_t reuse = 0;
    for (int32_t step = 0; step < cap; ++step) {
        int32_t index = (start + step) % cap;
        int64_t slot = ns_rt_map_slot(map, index);
        int32_t state = (int32_t)(uint8_t)ns_rt_load(slot, 0, -1);
        if (state == 0) {
            if (!reuse) reuse = slot;
            break;
        }
        if (state == 2) {
            if (!reuse) reuse = slot;
            continue;
        }
        int64_t sk = ns_rt_load(slot, 8, 8);
        if (ns_rt_map_keyeq(map, sk, key)) return slot;
    }
    if (insert_slot) *insert_slot = reuse;
    return 0;
}

int64_t ns_rt_map_get(int64_t map, int64_t key) {
    int64_t slot = ns_rt_map_probe(map, key, NULL);
    if (!slot) {
        fprintf(stderr, "ns_rt: dict key not found\n");
        abort();
    }
    return ns_rt_load(slot, 16, 8);
}

void ns_rt_map_set(int64_t map, int64_t key, int64_t val) {
    int64_t insert = 0;
    int64_t slot = ns_rt_map_probe(map, key, &insert);
    if (slot) {
        ns_rt_store(slot, 16, val, 8);
        return;
    }
    int32_t live = (int32_t)ns_rt_load(map, 4, 4);
    int32_t cap = (int32_t)ns_rt_load(map, 8, 4);
    if (!insert || live >= cap) {
        fprintf(stderr, "ns_rt: dict capacity exceeded\n");
        abort();
    }
    ns_rt_store(insert, 0, 1, 1);
    ns_rt_store(insert, 8, key, 8);
    ns_rt_store(insert, 16, val, 8);
    ns_rt_store(map, 4, live + 1, 4);
}

int64_t ns_rt_map_has(int64_t map, int64_t key) {
    return ns_rt_map_probe(map, key, NULL) != 0;
}

int64_t ns_rt_map_insert(int64_t map, int64_t key) {
    int64_t insert = 0;
    int64_t slot = ns_rt_map_probe(map, key, &insert);
    if (slot) return 0;
    int32_t live = (int32_t)ns_rt_load(map, 4, 4);
    int32_t cap = (int32_t)ns_rt_load(map, 8, 4);
    if (!insert || live >= cap) {
        fprintf(stderr, "ns_rt: set capacity exceeded\n");
        abort();
    }
    ns_rt_store(insert, 0, 1, 1);
    ns_rt_store(insert, 8, key, 8);
    ns_rt_store(map, 4, live + 1, 4);
    return 1;
}

int64_t ns_rt_map_remove(int64_t map, int64_t key) {
    int64_t slot = ns_rt_map_probe(map, key, NULL);
    if (!slot) return 0;
    ns_rt_store(slot, 0, 2, 1);
    int32_t live = (int32_t)ns_rt_load(map, 4, 4);
    if (live > 0) ns_rt_store(map, 4, live - 1, 4);
    return 1;
}

int64_t ns_rt_map_slot_live(int64_t map, int64_t idx) {
    int32_t cap = (int32_t)ns_rt_load(map, 8, 4);
    if (idx < 0 || idx >= cap) return 0;
    int64_t slot = ns_rt_map_slot(map, idx);
    return (int32_t)(uint8_t)ns_rt_load(slot, 0, -1) == 1;
}

int64_t ns_rt_map_slot_key(int64_t map, int64_t idx) {
    int64_t slot = ns_rt_map_slot(map, idx);
    return ns_rt_load(slot, 8, 8);
}

int64_t ns_rt_open(int64_t path, int64_t mode) {
    char *p = ns_rt_cstr(path);
    char *m = ns_rt_cstr(mode);
    FILE *f = fopen(p, m);
    free(p);
    free(m);
    return (int64_t)(uintptr_t)f;
}

int64_t ns_rt_read(int64_t fd) {
    FILE *f = (FILE *)(uintptr_t)fd;
    if (!f) return ns_rt_from_bytes("", 0);
    long pos = ftell(f);
    int32_t requested = -1;
    if (pos >= 0 && fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end >= pos && fseek(f, pos, SEEK_SET) == 0) requested = (int32_t)(end - pos);
    } else if (pos >= 0) {
        fseek(f, pos, SEEK_SET);
    }
    if (requested >= 0) {
        char *buf = (char *)malloc((size_t)requested);
        if (!buf) abort();
        size_t n = requested > 0 ? fread(buf, 1, (size_t)requested, f) : 0;
        int64_t out = ns_rt_from_bytes(buf, (int32_t)n);
        free(buf);
        return out;
    }
    char chunk[4096];
    char *acc = NULL;
    size_t total = 0;
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n == 0) break;
        char *next = (char *)realloc(acc, total + n);
        if (!next) {
            free(acc);
            abort();
        }
        acc = next;
        memcpy(acc + total, chunk, n);
        total += n;
    }
    int64_t out = ns_rt_from_bytes(acc ? acc : "", (int32_t)total);
    free(acc);
    return out;
}

int64_t ns_rt_write(int64_t fd, int64_t buf) {
    FILE *f = (FILE *)(uintptr_t)fd;
    int32_t bytes = 0, len = 0;
    ns_rt_str_parts(buf, &bytes, &len);
    if (!f || len <= 0) return 0;
    size_t n = fwrite(ns_rt_ptr(bytes), (size_t)len, 1, f);
    return (int64_t)n;
}

void ns_rt_close(int64_t fd) {
    FILE *f = (FILE *)(uintptr_t)fd;
    if (f) fclose(f);
}

/* Type tags match include/ns_type.h ns_value_type for primitive members. */
#define NS_RT_T_I8 5
#define NS_RT_T_U8 6
#define NS_RT_T_I16 7
#define NS_RT_T_U16 8
#define NS_RT_T_I32 9
#define NS_RT_T_U32 10
#define NS_RT_T_I64 11
#define NS_RT_T_U64 12
#define NS_RT_T_F32 13
#define NS_RT_T_F64 14
#define NS_RT_T_BOOL 15

int64_t ns_rt_union_new(int64_t tag, int64_t payload) {
    int64_t u = ns_rt_alloc(16);
    ns_rt_store(u, 0, tag, 4);
    ns_rt_store(u, 8, payload, 8);
    return u;
}

static int ns_rt_is_int_tag(int32_t tag) {
    return tag >= NS_RT_T_I8 && tag <= NS_RT_T_U64;
}

static int ns_rt_is_float_tag(int32_t tag) {
    return tag == NS_RT_T_F32 || tag == NS_RT_T_F64;
}

static double ns_rt_payload_as_double(int32_t tag, int64_t payload) {
    if (tag == NS_RT_T_F64) {
        double d;
        memcpy(&d, &payload, 8);
        return d;
    }
    if (tag == NS_RT_T_F32) {
        uint32_t bits = (uint32_t)payload;
        float f;
        memcpy(&f, &bits, 4);
        return (double)f;
    }
    if (tag == NS_RT_T_BOOL) return payload ? 1.0 : 0.0;
    if (tag == NS_RT_T_U8 || tag == NS_RT_T_U16 || tag == NS_RT_T_U32) {
        return (double)(uint32_t)payload;
    }
    if (tag == NS_RT_T_U64) return (double)(uint64_t)payload;
    return (double)payload;
}

int64_t ns_rt_union_as(int64_t u, int64_t want_tag) {
    int32_t tag = (int32_t)ns_rt_load(u, 0, 4);
    int64_t payload = ns_rt_load(u, 8, 8);
    int32_t want = (int32_t)want_tag;
    if (tag == want) return payload;
    if ((ns_rt_is_int_tag(tag) || ns_rt_is_float_tag(tag) || tag == NS_RT_T_BOOL) &&
        (ns_rt_is_int_tag(want) || ns_rt_is_float_tag(want) || want == NS_RT_T_BOOL)) {
        double d = ns_rt_payload_as_double(tag, payload);
        if (want == NS_RT_T_F64) {
            int64_t bits;
            memcpy(&bits, &d, 8);
            return bits;
        }
        if (want == NS_RT_T_F32) {
            float f = (float)d;
            uint32_t bits = 0;
            memcpy(&bits, &f, 4);
            return (int64_t)bits;
        }
        if (want == NS_RT_T_BOOL) return d != 0.0 ? 1 : 0;
        if (want == NS_RT_T_U64) return (int64_t)(uint64_t)d;
        return (int64_t)d;
    }
    return payload;
}

int64_t ns_rt_to_cstr(int64_t str) {
    return (int64_t)(uintptr_t)ns_rt_cstr(str);
}

int64_t ns_rt_from_cstr(int64_t ptr) {
    const char *s = (const char *)(uintptr_t)ptr;
    return ns_rt_from_bytes(s, s ? (int32_t)strlen(s) : 0);
}

int64_t ns_rt_native_ptr(int64_t addr) {
    if (addr <= 0) return 0;
    return (int64_t)(uintptr_t)ns_rt_ptr(addr);
}

/* Native code reads an ns array as a bare buffer, so an array argument crosses
 * the ffi boundary as the address of its payload, not of its header. */
int64_t ns_rt_array_ptr(int64_t arr) {
    if (arr <= 0) return 0;
    int64_t data = ns_rt_load(arr, 0, 4);
    if (data <= 0) return 0;
    return (int64_t)(uintptr_t)ns_rt_ptr(data);
}

#define NS_RT_CB_FN(i) \
static void ns_rt_cb_fn##i(void *arg) { \
    int64_t (*fn)(int64_t, int64_t) = (int64_t (*)(int64_t, int64_t))(uintptr_t)ns_rt_cb_slot[i].code; \
    if (fn) fn(ns_rt_cb_slot[i].env, (int64_t)(uintptr_t)arg); \
}
NS_RT_CB_FN(0) NS_RT_CB_FN(1) NS_RT_CB_FN(2) NS_RT_CB_FN(3)
NS_RT_CB_FN(4) NS_RT_CB_FN(5) NS_RT_CB_FN(6) NS_RT_CB_FN(7)
NS_RT_CB_FN(8) NS_RT_CB_FN(9) NS_RT_CB_FN(10) NS_RT_CB_FN(11)
NS_RT_CB_FN(12) NS_RT_CB_FN(13) NS_RT_CB_FN(14) NS_RT_CB_FN(15)
#undef NS_RT_CB_FN

static void (*const ns_rt_cb_fns[NS_RT_CB_MAX])(void *) = {
    ns_rt_cb_fn0, ns_rt_cb_fn1, ns_rt_cb_fn2, ns_rt_cb_fn3,
    ns_rt_cb_fn4, ns_rt_cb_fn5, ns_rt_cb_fn6, ns_rt_cb_fn7,
    ns_rt_cb_fn8, ns_rt_cb_fn9, ns_rt_cb_fn10, ns_rt_cb_fn11,
    ns_rt_cb_fn12, ns_rt_cb_fn13, ns_rt_cb_fn14, ns_rt_cb_fn15,
};

int64_t ns_rt_callback(int64_t fnval) {
    if (fnval <= 0) return 0;
    if (ns_rt_cb_n >= NS_RT_CB_MAX) {
        fprintf(stderr, "ns_rt: too many native callbacks\n");
        abort();
    }
    int i = ns_rt_cb_n++;
    ns_rt_cb_slot[i].code = ns_rt_load(fnval, 0, 8);
    ns_rt_cb_slot[i].env = fnval;
    return (int64_t)(uintptr_t)ns_rt_cb_fns[i];
}

int64_t ns_rt_queue_main(void) { return 0; }
int64_t ns_rt_queue_worker(void) { return 1; }
int64_t ns_rt_queue_idle(void) { return 2; }

typedef int64_t (*ns_rt_fn0)(void);
typedef int64_t (*ns_rt_fn1)(int64_t);
typedef int64_t (*ns_rt_fn2)(int64_t, int64_t);
typedef int64_t (*ns_rt_fn3)(int64_t, int64_t, int64_t);
typedef int64_t (*ns_rt_fn4)(int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ns_rt_fn5)(int64_t, int64_t, int64_t, int64_t, int64_t);

typedef struct ns_rt_task {
    void *fn;
    int64_t env;
    int64_t args[4];
    int32_t nargs;
    int32_t queue;
    volatile int32_t state;
    volatile int32_t cancel;
    int64_t result;
#ifdef _WIN32
    HANDLE thread;
    CRITICAL_SECTION mu;
    CONDITION_VARIABLE cv;
#else
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
#endif
    int started;
} ns_rt_task;

#ifdef _WIN32
#define ns_rt_task_lock(t) EnterCriticalSection(&(t)->mu)
#define ns_rt_task_unlock(t) LeaveCriticalSection(&(t)->mu)
#define ns_rt_task_wait_cv(t) SleepConditionVariableCS(&(t)->cv, &(t)->mu, INFINITE)
#define ns_rt_task_signal(t) WakeAllConditionVariable(&(t)->cv)
#else
#define ns_rt_task_lock(t) pthread_mutex_lock(&(t)->mu)
#define ns_rt_task_unlock(t) pthread_mutex_unlock(&(t)->mu)
#define ns_rt_task_wait_cv(t) pthread_cond_wait(&(t)->cv, &(t)->mu)
#define ns_rt_task_signal(t) pthread_cond_broadcast(&(t)->cv)
#endif

static __thread ns_rt_task *ns_rt_task_self = NULL;

static int64_t ns_rt_task_invoke(ns_rt_task *t) {
    if (t->cancel) return 0;
    int64_t r = 0;
    if (t->env) {
        switch (t->nargs) {
        case 0: r = ((ns_rt_fn1)t->fn)(t->env); break;
        case 1: r = ((ns_rt_fn2)t->fn)(t->env, t->args[0]); break;
        case 2: r = ((ns_rt_fn3)t->fn)(t->env, t->args[0], t->args[1]); break;
        case 3: r = ((ns_rt_fn4)t->fn)(t->env, t->args[0], t->args[1], t->args[2]); break;
        default: r = ((ns_rt_fn5)t->fn)(t->env, t->args[0], t->args[1], t->args[2], t->args[3]); break;
        }
    } else {
        switch (t->nargs) {
        case 0: r = ((ns_rt_fn0)t->fn)(); break;
        case 1: r = ((ns_rt_fn1)t->fn)(t->args[0]); break;
        case 2: r = ((ns_rt_fn2)t->fn)(t->args[0], t->args[1]); break;
        case 3: r = ((ns_rt_fn3)t->fn)(t->args[0], t->args[1], t->args[2]); break;
        default: r = ((ns_rt_fn4)t->fn)(t->args[0], t->args[1], t->args[2], t->args[3]); break;
        }
    }
    return r;
}

static void ns_rt_task_finish(ns_rt_task *t, int64_t result, int cancelled) {
    ns_rt_task_lock(t);
    t->result = cancelled ? 0 : result;
    t->state = cancelled || t->cancel ? 3 : 2;
    ns_rt_task_signal(t);
    ns_rt_task_unlock(t);
}

#ifdef _WIN32
static DWORD WINAPI ns_rt_task_thread(LPVOID arg) {
    ns_rt_task *t = (ns_rt_task *)arg;
    ns_rt_task_self = t;
    t->state = 1;
    int64_t r = ns_rt_task_invoke(t);
    ns_rt_task_finish(t, r, t->cancel);
    ns_rt_task_self = NULL;
    return 0;
}
#else
static void *ns_rt_task_thread(void *arg) {
    ns_rt_task *t = (ns_rt_task *)arg;
    ns_rt_task_self = t;
    t->state = 1;
    int64_t r = ns_rt_task_invoke(t);
    ns_rt_task_finish(t, r, t->cancel);
    ns_rt_task_self = NULL;
    return NULL;
}
#endif

static ns_rt_task *ns_rt_task_obj(int64_t handle) {
    if (handle <= 0) return NULL;
    ns_rt_task *t = NULL;
    memcpy(&t, ns_rt_ptr(handle), sizeof(t));
    return t;
}

static int64_t ns_rt_task_box(ns_rt_task *t) {
    int64_t box = ns_rt_alloc(8);
    memcpy(ns_rt_ptr(box), &t, sizeof(t));
    return box;
}

int64_t ns_rt_task_spawn(int64_t fn, int64_t env, int64_t nargs,
                         int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                         int64_t queue) {
    ns_rt_task *t = (ns_rt_task *)calloc(1, sizeof(ns_rt_task));
    if (!t) abort();
    t->fn = (void *)(uintptr_t)fn;
    t->env = env;
    t->args[0] = a0;
    t->args[1] = a1;
    t->args[2] = a2;
    t->args[3] = a3;
    t->nargs = nargs < 0 ? 0 : (nargs > 4 ? 4 : (int32_t)nargs);
    t->queue = (int32_t)queue;
    t->state = 0;
#ifdef _WIN32
    InitializeCriticalSection(&t->mu);
    InitializeConditionVariable(&t->cv);
#else
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
#endif
    int64_t handle = ns_rt_task_box(t);
    if (queue == 0) {
        ns_rt_task *prev = ns_rt_task_self;
        ns_rt_task_self = t;
        t->state = 1;
        int64_t r = ns_rt_task_invoke(t);
        ns_rt_task_finish(t, r, t->cancel);
        ns_rt_task_self = prev;
        return handle;
    }
#ifdef _WIN32
    t->thread = CreateThread(NULL, 0, ns_rt_task_thread, t, 0, NULL);
    t->started = t->thread != NULL;
#else
    t->started = pthread_create(&t->thread, NULL, ns_rt_task_thread, t) == 0;
#endif
    if (!t->started) {
        ns_rt_task_finish(t, 0, 1);
    }
    return handle;
}

int64_t ns_rt_task_dispatch(int64_t queue, int64_t fnval) {
    int64_t code = fnval > 0 ? ns_rt_load(fnval, 0, 8) : 0;
    return ns_rt_task_spawn(code, fnval, 0, 0, 0, 0, 0, queue);
}

static void ns_rt_task_join_state(ns_rt_task *t) {
    if (!t) return;
    ns_rt_task_lock(t);
    while (t->state < 2) ns_rt_task_wait_cv(t);
    ns_rt_task_unlock(t);
}

int64_t ns_rt_task_await(int64_t handle) {
    ns_rt_task *t = ns_rt_task_obj(handle);
    if (!t) return 0;
    ns_rt_task_join_state(t);
    return t->state == 3 ? 0 : t->result;
}

void ns_rt_task_wait(int64_t handle) {
    ns_rt_task *t = ns_rt_task_obj(handle);
    ns_rt_task_join_state(t);
}

void ns_rt_task_cancel(int64_t handle) {
    ns_rt_task *t = ns_rt_task_obj(handle);
    if (!t) return;
    t->cancel = 1;
}

int64_t ns_rt_task_done(int64_t handle) {
    ns_rt_task *t = ns_rt_task_obj(handle);
    return t && t->state >= 2 ? 1 : 0;
}

int64_t ns_rt_task_cancelled(int64_t handle) {
    ns_rt_task *t = ns_rt_task_obj(handle);
    return t && t->state == 3 ? 1 : 0;
}

void ns_rt_task_sleep(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    int64_t remain = ms;
    while (remain > 0) {
        if (ns_rt_task_self && ns_rt_task_self->cancel) return;
        DWORD chunk = remain > 10 ? 10 : (DWORD)remain;
        Sleep(chunk);
        remain -= chunk;
    }
#else
    int64_t remain = ms;
    while (remain > 0) {
        if (ns_rt_task_self && ns_rt_task_self->cancel) return;
        int64_t chunk = remain < 10 ? remain : 10;
        struct timespec ts = { .tv_sec = (time_t)(chunk / 1000),
                               .tv_nsec = (long)(chunk % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        remain -= chunk;
    }
#endif
}

// Feature dylibs (io/ui/os/view) are built against the interpreter and import
// these debug allocators plus the compiler's file helpers. A native `ns build`
// executable has to export the same symbols or the first call jumps to NULL.
// Omitted when this file is compiled into bin/ns (NS_DEBUG), which already
// defines _ns_malloc from ns_type.c.
#ifndef NS_DEBUG
void *_ns_malloc(size_t size, const char *file, int line) {
    (void)file;
    (void)line;
    return malloc(size);
}

void *_ns_realloc(void *ptr, size_t size, const char *file, int line) {
    (void)file;
    (void)line;
    return realloc(ptr, size);
}

void _ns_free(void *ptr, const char *file, int line) {
    (void)file;
    (void)line;
    free(ptr);
}

typedef struct ns_rt_host_str {
    char *data;
    int32_t len;
    int32_t dynamic;
} ns_rt_host_str;

static ns_rt_host_str ns_rt_host_str_null(void) {
    ns_rt_host_str s;
    s.data = NULL;
    s.len = 0;
    s.dynamic = 0;
    return s;
}

int64_t ns_os_file_size(ns_rt_host_str path) {
    if (!path.data) return -1;
    FILE *file = fopen(path.data, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    fclose(file);
    return size < 0 ? -1 : (int64_t)size;
}

ns_rt_host_str ns_os_read_file_part(ns_rt_host_str path, int64_t offset, int64_t size) {
    if (!path.data || offset < 0 || size < 0 || size > 0x7fffffff) return ns_rt_host_str_null();
    FILE *file = fopen(path.data, "rb");
    if (!file) return ns_rt_host_str_null();
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ns_rt_host_str_null();
    }
    long file_size = ftell(file);
    if (file_size < 0 || offset > (int64_t)file_size) {
        fclose(file);
        return ns_rt_host_str_null();
    }
    if (size > (int64_t)file_size - offset) size = (int64_t)file_size - offset;
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        return ns_rt_host_str_null();
    }
    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return ns_rt_host_str_null();
    }
    size_t n = fread(buffer, 1, (size_t)size, file);
    if (n < (size_t)size && ferror(file)) {
        free(buffer);
        fclose(file);
        return ns_rt_host_str_null();
    }
    fclose(file);
    buffer[n] = '\0';
    ns_rt_host_str data;
    data.data = buffer;
    data.len = (int32_t)n;
    data.dynamic = 1;
    return data;
}

ns_rt_host_str ns_os_read_file(ns_rt_host_str path) {
    int64_t size = ns_os_file_size(path);
    if (size < 0) return ns_rt_host_str_null();
    return ns_os_read_file_part(path, 0, size);
}
#endif
