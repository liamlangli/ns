#include "ns_native_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>

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

#if defined(__linux__)
// A cross-built app writes `.ns-resources` beside the executable after copying
// the project's assets there. Entering that directory makes `res/house.vox`
// resolve the same way it does from an interpreted run at the project root.
static void ns_rt_enter_packaged_directory(void) {
    char executable[4096];
    ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n <= 0) return;
    executable[n] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash || slash == executable) return;
    *slash = '\0';
    char marker[4096];
    int wrote = snprintf(marker, sizeof(marker), "%s/.ns-resources", executable);
    if (wrote < 0 || wrote >= (int)sizeof(marker)) return;
    if (access(marker, F_OK) != 0) return;
    if (chdir(executable) != 0) {
        fprintf(stderr, "ns_rt: could not enter %s\n", executable);
    }
}
#endif

// Addresses are 32-bit offsets, so the heap cannot exceed 4 GiB. Reserve that
// VA once and commit pages in place: realloc would move the base and invalidate
// host pointers other threads already hold from ns_rt_ptr / array slots.
#define NS_RT_HEAP_RESERVE ((size_t)4 * 1024 * 1024 * 1024)

// The base and committed size are visible to the ns_cpu interpreter, which
// reads and writes the heap in place instead of calling ns_rt_load/ns_rt_store
// (see ns_native_rt.h). The base never moves once reserved.
uint8_t *ns_rt_mem = NULL;
static uint32_t ns_rt_used = 16;
uint32_t ns_rt_cap = 0;
static size_t ns_rt_page = 0;

#ifdef _WIN32
static CRITICAL_SECTION ns_rt_heap_mu;
static INIT_ONCE ns_rt_heap_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK ns_rt_heap_once_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&ns_rt_heap_mu);
    return TRUE;
}
static void ns_rt_heap_lock(void) {
    InitOnceExecuteOnce(&ns_rt_heap_once, ns_rt_heap_once_init, NULL, NULL);
    EnterCriticalSection(&ns_rt_heap_mu);
}
static void ns_rt_heap_unlock(void) { LeaveCriticalSection(&ns_rt_heap_mu); }
#else
static pthread_mutex_t ns_rt_heap_mu = PTHREAD_MUTEX_INITIALIZER;
static void ns_rt_heap_lock(void) { pthread_mutex_lock(&ns_rt_heap_mu); }
static void ns_rt_heap_unlock(void) { pthread_mutex_unlock(&ns_rt_heap_mu); }
#endif

#ifdef _WIN32
typedef DWORD ns_rt_thread;
static ns_rt_thread ns_rt_self(void) { return GetCurrentThreadId(); }
static int ns_rt_thread_eq(ns_rt_thread a, ns_rt_thread b) { return a == b; }
#else
typedef pthread_t ns_rt_thread;
static ns_rt_thread ns_rt_self(void) { return pthread_self(); }
static int ns_rt_thread_eq(ns_rt_thread a, ns_rt_thread b) { return pthread_equal(a, b) != 0; }
#endif

// The bump pointer is shared by every thread, so a scope may only drop what its
// own thread put on top of the heap. A run is an unbroken stretch of
// allocations by one thread, and `ns_rt_runs` lists them in address order:
// another thread allocating starts a new run, and a scope whose region holds
// someone else's live run keeps its memory instead of dropping data that
// thread still uses. A task thread that ends marks its runs dead - what it
// left behind is either pinned or garbage - so they no longer stop the thread
// that dispatched it from releasing its own temporaries around them.
typedef struct {
    ns_rt_thread owner;
    uint32_t base;
    int dead;
} ns_rt_run;
static ns_rt_run *ns_rt_runs = NULL;
static int32_t ns_rt_run_n = 0;
static int32_t ns_rt_run_cap = 0;

// Allocations that must outlive every scope: a string literal is materialized
// once and cached forever, so releasing past it would leave the cache holding a
// recycled address.
static _Atomic uint32_t ns_rt_floor = 0;

// A running task reads what its closure and arguments captured, all of which
// was allocated before it was spawned. Each running task holds the heap below
// its spawn point until it ends; `ns_rt_hold` is the highest such hold, and no
// scope releases below it meanwhile. Unlike the floor, a hold goes away.
#define NS_RT_HOLD_MAX 256
static uint32_t ns_rt_holds[NS_RT_HOLD_MAX];
static int32_t ns_rt_hold_n = 0;
static _Atomic uint32_t ns_rt_hold = 0;

// Everything below this address is kept, whichever thread asks. Heap lock held.
static uint32_t ns_rt_keep_locked(void) {
    uint32_t hold = atomic_load_explicit(&ns_rt_hold, memory_order_relaxed);
    return ns_rt_floor > hold ? ns_rt_floor : hold;
}

// A value stored into a container that is itself still in some scope's region
// only has to live as long as that container. Such a store records an owned
// pin, {owner: the container, top: the end of the value}, instead of raising
// the floor for good: a release to T keeps the top of every pin whose owner
// survives it (owner < T), and forgets the pins whose owner it drops.
//
// Only the pins no other pin dominates are kept - one with a lower owner and a
// higher top keeps at least as much for at least as long - so the list is
// ordered by owner with ascending tops.
typedef struct {
    uint32_t owner;
    uint32_t top;
} ns_rt_owned;
static ns_rt_owned *ns_rt_owned_pins = NULL;
static int32_t ns_rt_owned_n = 0;
static int32_t ns_rt_owned_cap = 0;

// Heap lock held.
static void ns_rt_owned_add(uint32_t owner, uint32_t top) {
    int32_t lo = 0, hi = ns_rt_owned_n;
    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;
        if (ns_rt_owned_pins[mid].owner < owner) lo = mid + 1;
        else hi = mid;
    }
    if (lo > 0 && ns_rt_owned_pins[lo - 1].top >= top) return;
    if (lo < ns_rt_owned_n && ns_rt_owned_pins[lo].owner == owner && ns_rt_owned_pins[lo].top >= top) return;
    int32_t end = lo;
    while (end < ns_rt_owned_n && ns_rt_owned_pins[end].top <= top) end++;
    if (end == lo) {
        if (ns_rt_owned_n == ns_rt_owned_cap) {
            int32_t cap = ns_rt_owned_cap ? ns_rt_owned_cap * 2 : 256;
            ns_rt_owned *pins = (ns_rt_owned *)realloc(ns_rt_owned_pins, (size_t)cap * sizeof(*pins));
            if (!pins) abort();
            ns_rt_owned_pins = pins;
            ns_rt_owned_cap = cap;
        }
        memmove(ns_rt_owned_pins + lo + 1, ns_rt_owned_pins + lo,
                (size_t)(ns_rt_owned_n - lo) * sizeof(*ns_rt_owned_pins));
        ns_rt_owned_n++;
    } else if (end > lo + 1) {
        memmove(ns_rt_owned_pins + lo + 1, ns_rt_owned_pins + end,
                (size_t)(ns_rt_owned_n - end) * sizeof(*ns_rt_owned_pins));
        ns_rt_owned_n -= end - lo - 1;
    }
    ns_rt_owned_pins[lo].owner = owner;
    ns_rt_owned_pins[lo].top = top;
}

// Heap lock held. The lowest address a release aiming at `target` may drop to:
// raised past every owned pin whose owner stays, until no pin moves it.
static uint32_t ns_rt_owned_target(uint32_t target) {
    int32_t i = 0;
    while (i < ns_rt_owned_n && ns_rt_owned_pins[i].owner < target) {
        if (ns_rt_owned_pins[i].top > target) target = ns_rt_owned_pins[i].top;
        i++;
    }
    return target;
}

// Heap lock held. The region from `top` up is gone, and the pins it owned with
// it; pins already below what is kept for good need no record.
static void ns_rt_owned_trim(uint32_t top) {
    while (ns_rt_owned_n > 0 && ns_rt_owned_pins[ns_rt_owned_n - 1].owner >= top) ns_rt_owned_n--;
    int32_t drop = 0;
    while (drop < ns_rt_owned_n && ns_rt_owned_pins[drop].top <= ns_rt_floor) drop++;
    if (drop > 0) {
        memmove(ns_rt_owned_pins, ns_rt_owned_pins + drop,
                (size_t)(ns_rt_owned_n - drop) * sizeof(*ns_rt_owned_pins));
        ns_rt_owned_n -= drop;
    }
}

// Each thread's open scope marks, innermost last. Only ns_rt_pin_in reads
// them, to tell a container the running call built from one it was handed.
// A leave that does not match the innermost mark poisons the stack, and a
// poisoned thread pins everything as it did before.
#define NS_RT_SCOPE_DEPTH_MAX 4096
static __thread uint32_t ns_rt_scope_marks[NS_RT_SCOPE_DEPTH_MAX];
static __thread int32_t ns_rt_scope_depth = 0;
static __thread int ns_rt_scope_poisoned = 0;

// A release another thread's live run blocked. The scope at that depth has
// ended and returned nothing in its region, so once that thread is done the
// region is garbage; the next scope at the same depth that releases retries
// it. Any allocation by the code around those scopes - which may be holding
// on to it - drops the retry instead.
static __thread uint32_t ns_rt_deferred_mark = 0;
static __thread int32_t ns_rt_deferred_depth = 0;

// How far the heap has ever been used. Committing a page zeroes it, so an
// allocation that has never been handed out is already blank; one that comes
// back from a released scope is not, and a program that reads a fresh array or
// struct before writing it has always seen zeroes.
static uint32_t ns_rt_high = 16;

static int64_t *ns_rt_globals = NULL;
static size_t ns_rt_global_capacity = 0;

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

static size_t ns_rt_page_size(void) {
    if (ns_rt_page) return ns_rt_page;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    ns_rt_page = info.dwPageSize ? (size_t)info.dwPageSize : 4096;
#else
    long page = sysconf(_SC_PAGESIZE);
    ns_rt_page = page > 0 ? (size_t)page : 4096;
#endif
    return ns_rt_page;
}

static uint32_t ns_rt_align_page(uint32_t n) {
    size_t page = ns_rt_page_size();
    size_t aligned = ((size_t)n + page - 1) & ~(page - 1);
    if (aligned > (size_t)0xffffffffu) return 0xffffffffu;
    return (uint32_t)aligned;
}

static void ns_rt_commit(uint32_t from, uint32_t to) {
    if (to <= from) return;
#ifdef _WIN32
    if (!VirtualAlloc(ns_rt_mem + from, (SIZE_T)(to - from), MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "ns_rt: out of memory\n");
        abort();
    }
#else
    if (mprotect(ns_rt_mem + from, (size_t)(to - from), PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "ns_rt: out of memory\n");
        abort();
    }
#endif
    memset(ns_rt_mem + from, 0, (size_t)(to - from));
}

static void ns_rt_grow(uint64_t need) {
    if (need <= (uint64_t)ns_rt_cap) return;
    if (need > (uint64_t)UINT32_MAX) {
        fprintf(stderr, "ns_rt: heap overflow\n");
        abort();
    }
    uint64_t cap = ns_rt_cap ? ns_rt_cap : ns_rt_align_page(4096);
    while (cap < need) {
        cap *= 2;
        if (cap > (uint64_t)UINT32_MAX) cap = UINT32_MAX;
    }
    cap = ns_rt_align_page((uint32_t)cap);
    if (cap < need) {
        fprintf(stderr, "ns_rt: heap overflow\n");
        abort();
    }
    ns_rt_commit(ns_rt_cap, (uint32_t)cap);
    ns_rt_cap = (uint32_t)cap;
}

static void ns_rt_init_locked(void) {
    if (!ns_rt_mem) {
#ifdef _WIN32
        ns_rt_mem = (uint8_t *)VirtualAlloc(NULL, NS_RT_HEAP_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
        if (!ns_rt_mem) {
            fprintf(stderr, "ns_rt: out of memory\n");
            abort();
        }
#else
        void *mem = mmap(NULL, NS_RT_HEAP_RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) {
            fprintf(stderr, "ns_rt: out of memory\n");
            abort();
        }
        ns_rt_mem = (uint8_t *)mem;
#endif
        ns_rt_cap = 0;
        ns_rt_grow(4096);
    }
    if (ns_rt_used < 16) ns_rt_used = 16;
#if defined(__APPLE__) || defined(__linux__)
    // The allocator calls this on every miss, so the directory is resolved once.
    static int entered_bundle = 0;
    if (!entered_bundle) {
        entered_bundle = 1;
#if defined(__APPLE__)
        ns_rt_enter_bundle_resources();
#elif defined(__linux__)
        ns_rt_enter_packaged_directory();
#endif
    }
#endif
}

void ns_rt_init(void) {
    if (ns_rt_mem) return;
    ns_rt_heap_lock();
    ns_rt_init_locked();
    ns_rt_heap_unlock();
}

void ns_rt_reset(void) {
    ns_rt_heap_lock();
    ns_rt_used = 16;
    ns_rt_high = 16;
    ns_rt_floor = 0;
    ns_rt_run_n = 0;
    ns_rt_owned_n = 0;
    ns_rt_hold_n = 0;
    ns_rt_hold = 0;
    if (ns_rt_mem && ns_rt_cap) memset(ns_rt_mem, 0, ns_rt_cap);
    if (ns_rt_globals) memset(ns_rt_globals, 0, ns_rt_global_capacity * sizeof(*ns_rt_globals));
    ns_rt_cb_n = 0;
    memset(ns_rt_cb_slot, 0, sizeof(ns_rt_cb_slot));
    if (ns_rt_strcache) {
        free(ns_rt_strcache);
        ns_rt_strcache = NULL;
    }
    ns_rt_heap_unlock();
}

void ns_rt_set_strtab(const char **tab, const int32_t *lens, int n) {
    ns_rt_strtab = tab;
    ns_rt_strlens = lens;
    ns_rt_nstr = n;
    if (ns_rt_strcache) {
        free(ns_rt_strcache);
        ns_rt_strcache = NULL;
    }
    // Materialize every literal now, before any call scope exists. Interning
    // one lazily instead would pin the heap wherever the first use happened to
    // land and stop every scope below that point from reclaiming anything.
    for (int i = 0; i < n; ++i) ns_rt_intern(i);
}

// Runs entirely below what is kept can never be released into again, and two
// neighbouring dead runs, or two of the same live thread, are one run.
static void ns_rt_run_compact(void) {
    uint32_t keep = ns_rt_keep_locked();
    int32_t first = 0;
    while (first + 1 < ns_rt_run_n && ns_rt_runs[first + 1].base <= keep) first++;
    int32_t n = 0;
    for (int32_t i = first; i < ns_rt_run_n; ++i) {
        ns_rt_run r = ns_rt_runs[i];
        if (n > 0) {
            ns_rt_run *prev = &ns_rt_runs[n - 1];
            if ((prev->dead && r.dead) ||
                (!prev->dead && !r.dead && ns_rt_thread_eq(prev->owner, r.owner))) {
                continue;
            }
        }
        ns_rt_runs[n++] = r;
    }
    ns_rt_run_n = n;
}

// Heap lock held. Record that `self` allocated at `addr`, the top of the heap.
static void ns_rt_run_push(ns_rt_thread self, uint32_t addr) {
    if (ns_rt_run_n > 0) {
        ns_rt_run *top = &ns_rt_runs[ns_rt_run_n - 1];
        if (!top->dead && ns_rt_thread_eq(top->owner, self)) return;
    }
    if (ns_rt_run_n == ns_rt_run_cap) ns_rt_run_compact();
    if (ns_rt_run_n == ns_rt_run_cap) {
        int32_t cap = ns_rt_run_cap ? ns_rt_run_cap * 2 : 64;
        ns_rt_run *runs = (ns_rt_run *)realloc(ns_rt_runs, (size_t)cap * sizeof(*runs));
        if (!runs) abort();
        ns_rt_runs = runs;
        ns_rt_run_cap = cap;
    }
    ns_rt_runs[ns_rt_run_n].owner = self;
    ns_rt_runs[ns_rt_run_n].base = addr;
    ns_rt_runs[ns_rt_run_n].dead = 0;
    ns_rt_run_n++;
}

// Heap lock held. Whether every allocation in [target, used) belongs to `self`
// or to a thread that has ended.
static int ns_rt_run_releasable(ns_rt_thread self, uint32_t target) {
    for (int32_t i = ns_rt_run_n - 1; i >= 0; --i) {
        ns_rt_run *r = &ns_rt_runs[i];
        if (!r->dead && !ns_rt_thread_eq(r->owner, self)) return 0;
        if (r->base <= target) return 1;
    }
    return ns_rt_run_n > 0;
}

// Heap lock held. Forget the runs above `top`, the new top of the heap.
static void ns_rt_run_trim(uint32_t top) {
    while (ns_rt_run_n > 0 && ns_rt_runs[ns_rt_run_n - 1].base >= top) ns_rt_run_n--;
}

int64_t ns_rt_alloc(int64_t size) {
    if (size < 0) size = 0;
    if ((uint64_t)size > (uint64_t)UINT32_MAX) {
        fprintf(stderr, "ns_rt: heap overflow\n");
        abort();
    }
    uint32_t n = (uint32_t)size;
    ns_rt_heap_lock();
    ns_rt_init_locked();
    uint32_t addr = (ns_rt_used + 7u) & ~7u;
    ns_rt_grow((uint64_t)addr + n + 16);
    ns_rt_used = addr + n;
    if (addr < ns_rt_high) {
        uint32_t stop = ns_rt_used < ns_rt_high ? ns_rt_used : ns_rt_high;
        memset(ns_rt_mem + addr, 0, (size_t)(stop - addr));
    }
    if (ns_rt_used > ns_rt_high) ns_rt_high = ns_rt_used;
    ns_rt_run_push(ns_rt_self(), addr);
    ns_rt_heap_unlock();
    if (ns_rt_deferred_mark && ns_rt_scope_depth < ns_rt_deferred_depth) ns_rt_deferred_mark = 0;
    return (int64_t)addr;
}

static uint8_t *ns_rt_ptr(int64_t addr) {
    if (!ns_rt_mem) ns_rt_init();
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


int64_t ns_rt_scope_enter(void) {
    ns_rt_heap_lock();
    ns_rt_init_locked();
    uint32_t mark = (ns_rt_used + 7u) & ~7u;
    ns_rt_heap_unlock();
    if (ns_rt_scope_depth < NS_RT_SCOPE_DEPTH_MAX) ns_rt_scope_marks[ns_rt_scope_depth] = mark;
    ns_rt_scope_depth++;
    return (int64_t)mark;
}

static void ns_rt_str_parts(int64_t str, int32_t *bytes, int32_t *len);

// An address that is reachable after the call that made it - stored into a
// global, into a container, or handed to a native library that may keep it -
// has to survive every scope release, and a bump heap can only give back a
// contiguous top. `ns_rt_floor` is the highest address any such value reaches,
// and no scope drops below it. Pinning something the caller already owned costs
// nothing, because that address is below the marks that matter.
static void ns_rt_raise_floor(uint32_t to) {
    if (to > ns_rt_used) to = ns_rt_used;
    if (to > ns_rt_floor) ns_rt_floor = to;
}

// A self-contained block: a flat struct, whose bytes hold no further address.
void ns_rt_pin(int64_t addr, int64_t size) {
    if (addr <= 0 || size < 0 || (uint64_t)addr > UINT32_MAX) return;
    uint32_t floor = atomic_load_explicit(&ns_rt_floor, memory_order_acquire);
    if ((uint64_t)addr + (uint64_t)size <= floor) return;
    ns_rt_heap_lock();
    if ((uint64_t)addr < (uint64_t)ns_rt_used) ns_rt_raise_floor((uint32_t)(addr + size));
    ns_rt_heap_unlock();
}

// A string keeps its bytes in a second block, which a substring may share with
// the string it was cut from, so both extents are pinned.
void ns_rt_pin_str(int64_t str) {
    if (str <= 0 || (uint64_t)str > UINT32_MAX) return;
    // Published descriptors/bytes are immutable and cannot be reclaimed by a
    // scope. Re-pinning them needs no mutex, which lets parallel FFI readers
    // consume the same source strings without serializing on the heap lock.
    uint32_t floor = atomic_load_explicit(&ns_rt_floor, memory_order_acquire);
    if ((uint64_t)str + 8 <= floor) {
        int32_t bytes = 0, len = 0;
        ns_rt_str_parts(str, &bytes, &len);
        if (bytes <= 0 || len <= 0 || (uint64_t)(uint32_t)bytes + (uint32_t)len <= floor) return;
    }
    ns_rt_heap_lock();
    // Reading the descriptor goes through ns_rt_ptr, which takes this lock to
    // initialize the heap; only an initialized heap can hold this address.
    if (ns_rt_mem && (uint64_t)str < (uint64_t)ns_rt_used) {
        int32_t bytes = 0, len = 0;
        ns_rt_str_parts(str, &bytes, &len);
        ns_rt_raise_floor((uint32_t)(str + 8));
        if (bytes > 0 && len > 0) ns_rt_raise_floor((uint32_t)(bytes + len));
    }
    ns_rt_heap_unlock();
}

// An array of values: its header plus the payload the header points at.
void ns_rt_pin_array(int64_t arr, int64_t stride) {
    if (arr <= 0) return;
    ns_rt_heap_lock();
    if (ns_rt_mem && (uint64_t)arr < (uint64_t)ns_rt_used) {
        int32_t data = (int32_t)ns_rt_load(arr, 0, 4);
        int32_t count = (int32_t)ns_rt_load(arr, 4, 4);
        if (stride < 0) stride = 0;
        ns_rt_raise_floor((uint32_t)(arr + 16));
        if (data > 0 && count > 0) ns_rt_raise_floor((uint32_t)(data + count * (int32_t)stride));
    }
    ns_rt_heap_unlock();
}

// A value whose reachable set the compiler cannot describe - a dictionary, a
// closure, an array of handles, a struct with a string in it. Everything
// allocated so far stays.
void ns_rt_pin_all(void) {
    ns_rt_heap_lock();
    ns_rt_raise_floor(ns_rt_used);
    ns_rt_heap_unlock();
}

// A complex ns ref can reach arbitrary heap objects, but an opaque native
// pointer (ui_renderer, os_lock, etc.) is outside the offset-addressed heap.
// Passing those handles to FFI must not pin the entire ns heap on every call.
void ns_rt_pin_ref(int64_t addr) {
    if (addr > 0 && (uint64_t)addr <= UINT32_MAX) ns_rt_pin_all();
}

static void ns_rt_pin_kind(int64_t value, int64_t kind, int64_t size) {
    switch (kind) {
    case 0: ns_rt_pin(value, size); break;
    case 1: ns_rt_pin_str(value); break;
    case 2: ns_rt_pin_array(value, size); break;
    case 4: ns_rt_pin_ref(value); break;
    default: ns_rt_pin_all(); break;
    }
}

// Heap lock held. The end of everything a value of this pin kind reaches, or 0
// when it reaches nothing in the heap.
static uint32_t ns_rt_pin_top(int64_t value, int64_t kind, int64_t size) {
    if (kind == 3) return ns_rt_used;
    if (value <= 0 || (uint64_t)value >= (uint64_t)ns_rt_used) return 0;
    uint32_t addr = (uint32_t)value;
    uint64_t top = addr;
    switch (kind) {
    case 0:
        top = (uint64_t)addr + (uint64_t)(size > 0 ? size : 0);
        break;
    case 1: {
        int32_t bytes = 0, len = 0;
        ns_rt_str_parts(value, &bytes, &len);
        top = (uint64_t)addr + 8;
        if (bytes > 0 && len > 0 && (uint64_t)(uint32_t)bytes + (uint32_t)len > top) {
            top = (uint64_t)(uint32_t)bytes + (uint32_t)len;
        }
    } break;
    case 2: {
        int32_t data = (int32_t)ns_rt_load(value, 0, 4);
        int32_t count = (int32_t)ns_rt_load(value, 4, 4);
        top = (uint64_t)addr + 16;
        if (data > 0 && count > 0 && size > 0) {
            uint64_t end = (uint64_t)(uint32_t)data + (uint64_t)count * (uint64_t)size;
            if (end > top) top = end;
        }
    } break;
    default:
        top = ns_rt_used;
        break;
    }
    return top > ns_rt_used ? ns_rt_used : (uint32_t)top;
}

// Pin `value`, which is being stored into `container`, for as long as the
// container lives rather than for good.
//
// A container the running call allocated, that nothing has kept yet, needs no
// pin at all: it is released together with what it holds, and when it escapes
// it is pinned whole, which keeps everything allocated before it. It counts as
// the call's own when it lies at or above this thread's innermost scope mark
// and above everything kept for this call. A container another thread
// allocated there is only reachable through something that thread published,
// which kept it; one a running task captured lies below that task's hold.
//
// Any other container still in some scope's region gets an owned pin. Only a
// container kept for good - a global's, or one handed to a native library -
// raises the floor.
//
// Pinning everything for good kept, for the life of the process, every
// temporary allocated before the stored value: filling a fresh struct or
// dictionary, or a caller's array from a worker, leaked the whole region.
void ns_rt_pin_in(int64_t container, int64_t value, int64_t kind, int64_t size) {
    if (container <= 0 || (uint64_t)container > UINT32_MAX) {
        ns_rt_pin_kind(value, kind, size);
        return;
    }
    ns_rt_heap_lock();
    if (!ns_rt_mem || (uint64_t)container >= (uint64_t)ns_rt_used) {
        ns_rt_heap_unlock();
        ns_rt_pin_kind(value, kind, size);
        return;
    }
    uint32_t owner = (uint32_t)container;
    uint32_t keep = ns_rt_keep_locked();
    if (!ns_rt_scope_poisoned && ns_rt_scope_depth > 0 && ns_rt_scope_depth <= NS_RT_SCOPE_DEPTH_MAX) {
        uint32_t mark = ns_rt_scope_marks[ns_rt_scope_depth - 1];
        uint32_t release = ns_rt_owned_target(mark > keep ? mark : keep);
        if (owner >= mark && owner >= release) {
            ns_rt_heap_unlock();
            return;
        }
    }
    uint32_t top = ns_rt_pin_top(value, kind, size);
    if (owner < ns_rt_floor) {
        ns_rt_raise_floor(top);
    } else if (top > ns_rt_floor) {
        ns_rt_owned_add(owner, top);
    }
    ns_rt_heap_unlock();
}

int64_t ns_rt_scope_leave(int64_t mark, int64_t value, int64_t size) {
    int32_t depth = ns_rt_scope_depth;
    if (ns_rt_deferred_mark && depth < ns_rt_deferred_depth) ns_rt_deferred_mark = 0;
    if (ns_rt_scope_depth > 0) {
        ns_rt_scope_depth--;
        if (ns_rt_scope_depth < NS_RT_SCOPE_DEPTH_MAX &&
            (int64_t)ns_rt_scope_marks[ns_rt_scope_depth] != mark) {
            ns_rt_scope_poisoned = 1;
        }
    } else {
        ns_rt_scope_poisoned = 1;
    }
    if (mark <= 0 || (uint64_t)mark > (uint64_t)UINT32_MAX) return value;
    uint32_t target = (uint32_t)mark;
    int64_t result = value;
    ns_rt_heap_lock();
    // Whatever was pinned inside the call stays, and the release starts above
    // it: the rest of the region is this call's own dead temporaries.
    // Values stored into containers that outlive the call stay too.
    uint32_t keep = ns_rt_keep_locked();
    if (target < keep) target = keep;
    target = ns_rt_owned_target(target);
    ns_rt_thread self = ns_rt_self();
    if (target <= ns_rt_used && ns_rt_run_releasable(self, target)) {
        if (value < (int64_t)target || value >= (int64_t)ns_rt_used) {
            ns_rt_used = target;
            ns_rt_run_trim(target);
            ns_rt_owned_trim(target);
            uint32_t older = ns_rt_deferred_mark;
            if (older && depth == ns_rt_deferred_depth) {
                if (older < keep) older = keep;
                older = ns_rt_owned_target(older);
                if (older >= ns_rt_used) {
                    ns_rt_deferred_mark = 0;
                } else if (ns_rt_run_releasable(self, older)) {
                    ns_rt_used = older;
                    ns_rt_run_trim(older);
                    ns_rt_owned_trim(older);
                    ns_rt_deferred_mark = 0;
                }
            }
        } else if (size > 0 && value + size <= (int64_t)ns_rt_used) {
            // A struct result built inside the scope moves down to the release
            // point, so the caller reads the same bytes from live memory.
            memmove(ns_rt_mem + target, ns_rt_mem + (uint32_t)value, (size_t)size);
            ns_rt_used = target + (uint32_t)size;
            result = (int64_t)target;
            ns_rt_run_trim(target);
            ns_rt_owned_trim(target);
            ns_rt_run_push(self, target);
        }
        // Any other heap result is one the scope cannot move: keeping the
        // region costs memory but never hands back a recycled address.
    } else if (target < ns_rt_used) {
        if (value >= (int64_t)target && value < (int64_t)ns_rt_used) {
            ns_rt_deferred_mark = 0;
        } else if (!ns_rt_deferred_mark || depth <= ns_rt_deferred_depth) {
            if (!ns_rt_deferred_mark || target < ns_rt_deferred_mark || depth < ns_rt_deferred_depth) {
                ns_rt_deferred_mark = target;
            }
            ns_rt_deferred_depth = depth;
        }
    }
    ns_rt_heap_unlock();
    return result;
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
    if (idx < 0) return 0;
    ns_rt_heap_lock();
    int64_t value = (uint64_t)idx < ns_rt_global_capacity ? ns_rt_globals[idx] : 0;
    ns_rt_heap_unlock();
    return value;
}

void ns_rt_gset(int64_t idx, int64_t val) {
    if (idx < 0) return;
    ns_rt_heap_lock();
    if ((uint64_t)idx >= ns_rt_global_capacity) {
        if ((uint64_t)idx >= SIZE_MAX / sizeof(*ns_rt_globals)) abort();
        size_t needed = (size_t)idx + 1;
        size_t capacity = ns_rt_global_capacity ? ns_rt_global_capacity : 1024;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / sizeof(*ns_rt_globals) / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        int64_t *globals = realloc(ns_rt_globals, capacity * sizeof(*globals));
        if (!globals) abort();
        memset(globals + ns_rt_global_capacity, 0,
               (capacity - ns_rt_global_capacity) * sizeof(*globals));
        ns_rt_globals = globals;
        ns_rt_global_capacity = capacity;
    }
    ns_rt_globals[idx] = val;
    ns_rt_heap_unlock();
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
    // The cache keeps this address for the life of the program, so no call
    // scope may reclaim it, whichever frame first asked for the literal.
    ns_rt_heap_lock();
    if (ns_rt_used > ns_rt_floor) ns_rt_floor = ns_rt_used;
    ns_rt_heap_unlock();
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

// A float inside a format string, as the interpreter prints it (ns_fmt.c):
// two decimals. `ftos` keeps its own round-trip precision.
int64_t ns_rt_fmtf(int64_t bits) {
    double x;
    memcpy(&x, &bits, 8);
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%.2f", x);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
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

/* ── shader host execution ─────────────────────────────────────────────────
 * A compute fn a program calls directly runs one invocation at a time on the
 * CPU, reading the image, root words and invocation coordinate bound here,
 * exactly as the interpreter's shader_host_* intrinsics do (ns_shader.c).
 * Textures are f32 arrays of four components per texel. */
#define NS_RT_SHADER_ROOT_WORDS 1024

static struct {
    float *read, *write, *write_secondary;
    float *buffers[32];
    int64_t buffer_lengths[32];
    int32_t width, height;
    float root[NS_RT_SHADER_ROOT_WORDS];
    int32_t global_id[3];
    int bound;
} ns_rt_shader;

static float *ns_rt_f32_array(int64_t arr, int64_t *length) {
    *length = 0;
    if (arr <= 0) return NULL;
    *length = ns_rt_load(arr, 4, 4);
    int64_t data = ns_rt_load(arr, 0, 4);
    return data > 0 ? (float *)(void *)ns_rt_ptr(data) : NULL;
}

int64_t ns_rt_shader_host_buffer(int64_t slot, int64_t array) {
    int64_t length = 0;
    float *values = ns_rt_f32_array(array, &length);
    if (slot < 0 || slot >= 32 || !values) return 0;
    ns_rt_shader.buffers[slot] = values;
    ns_rt_shader.buffer_lengths[slot] = length;
    return 1;
}

int64_t ns_rt_shader_buffer_f32(int64_t slot, int64_t index) {
    float value = 0.0f;
    if (slot >= 0 && slot < 32 && ns_rt_shader.buffers[slot] && index >= 0 && index < ns_rt_shader.buffer_lengths[slot])
        value = ns_rt_shader.buffers[slot][index];
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}

void ns_rt_shader_buffer_store_f32(int64_t slot, int64_t index, int64_t bits) {
    if (slot >= 0 && slot < 32 && ns_rt_shader.buffers[slot] && index >= 0 && index < ns_rt_shader.buffer_lengths[slot]) {
        uint32_t word = (uint32_t)bits;
        memcpy(&ns_rt_shader.buffers[slot][index], &word, 4);
    }
}

int64_t ns_rt_shader_host_bind(int64_t read, int64_t write, int64_t width, int64_t height) {
    int64_t read_len = 0, write_len = 0;
    float *r = ns_rt_f32_array(read, &read_len);
    float *w = ns_rt_f32_array(write, &write_len);
    int64_t texels = (width > 0 ? width : 0) * (height > 0 ? height : 0);
    int ok = r && w && width > 0 && height > 0 && read_len >= texels * 4 && write_len >= texels * 4;
    if (ok) {
        ns_rt_shader.read = r;
        ns_rt_shader.write = w;
        ns_rt_shader.write_secondary = NULL;
        ns_rt_shader.width = (int32_t)width;
        ns_rt_shader.height = (int32_t)height;
        ns_rt_shader.bound = 1;
    }
    return ok;
}

int64_t ns_rt_shader_host_bind_secondary(int64_t write) {
    int64_t len = 0;
    float *w = ns_rt_f32_array(write, &len);
    int64_t texels = (int64_t)ns_rt_shader.width * ns_rt_shader.height;
    int ok = ns_rt_shader.bound && w && len >= texels * 4;
    if (ok) ns_rt_shader.write_secondary = w;
    return ok;
}

int64_t ns_rt_shader_host_root(int64_t words) {
    int64_t len = 0;
    float *w = ns_rt_f32_array(words, &len);
    if (!w || len <= 0) return 0;
    int64_t taken = len < NS_RT_SHADER_ROOT_WORDS ? len : NS_RT_SHADER_ROOT_WORDS;
    memset(ns_rt_shader.root, 0, sizeof(ns_rt_shader.root));
    memcpy(ns_rt_shader.root, w, (size_t)taken * sizeof(float));
    return 1;
}

void ns_rt_shader_host_invocation(int64_t x, int64_t y, int64_t z) {
    ns_rt_shader.global_id[0] = (int32_t)x;
    ns_rt_shader.global_id[1] = (int32_t)y;
    ns_rt_shader.global_id[2] = (int32_t)z;
}

void ns_rt_shader_host_swap(void) {
    float *read = ns_rt_shader.read;
    ns_rt_shader.read = ns_rt_shader.write;
    ns_rt_shader.write = read;
}

void ns_rt_shader_host_release(void) {
    memset(&ns_rt_shader, 0, sizeof(ns_rt_shader));
}

int64_t ns_rt_shader_global_id(int64_t axis) {
    return axis >= 0 && axis < 3 ? ns_rt_shader.global_id[axis] : 0;
}

int64_t ns_rt_shader_root_f32(int64_t index) {
    float word = index >= 0 && index < NS_RT_SHADER_ROOT_WORDS ? ns_rt_shader.root[index] : 0.0f;
    uint32_t bits;
    memcpy(&bits, &word, 4);
    return (int64_t)bits;
}

static int32_t ns_rt_shader_texel(int64_t x, int64_t y) {
    if (!ns_rt_shader.bound) {
        fprintf(stderr, "shader: texture intrinsics need shader_host_bind outside a transpiled shader.\n");
        abort();
    }
    if (x < 0 || y < 0 || x >= ns_rt_shader.width || y >= ns_rt_shader.height) return -1;
    return (int32_t)((y * ns_rt_shader.width + x) * 4);
}

// A float4 is four f32 fields in declaration order.
int64_t ns_rt_shader_read_texture(int64_t x, int64_t y) {
    int32_t texel = ns_rt_shader_texel(x, y);
    int64_t out = ns_rt_alloc(16);
    float *dst = (float *)(void *)ns_rt_ptr(out);
    for (int i = 0; i < 4; ++i) dst[i] = texel >= 0 ? ns_rt_shader.read[texel + i] : 0.0f;
    return out;
}

void ns_rt_shader_write_texture(int64_t x, int64_t y, int64_t color, int64_t secondary) {
    int32_t texel = ns_rt_shader_texel(x, y);
    float *destination = secondary ? ns_rt_shader.write_secondary : ns_rt_shader.write;
    if (!destination) {
        fprintf(stderr, "shader: secondary write texture is not bound.\n");
        abort();
    }
    if (texel < 0) return;
    const float *src = (const float *)(const void *)ns_rt_ptr(color);
    for (int i = 0; i < 4; ++i) destination[texel + i] = src[i];
}

// FNV-1a 64 over the string's bytes, as ns_shader_source_hash.
int64_t ns_rt_shader_source_hash(int64_t str) {
    int32_t bytes = 0, len = 0;
    ns_rt_str_parts(str, &bytes, &len);
    uint64_t hash = 14695981039346656037ull;
    const uint8_t *p = len > 0 ? ns_rt_ptr(bytes) : NULL;
    for (int32_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ull;
    }
    return (int64_t)hash;
}

void ns_rt_shader_unsupported(int64_t name) {
    char *text = ns_rt_cstr(name);
    fprintf(stderr, "shader: %s has no CPU implementation; it only runs inside a transpiled shader.\n", text);
    free(text);
    abort();
}

/* A str crosses into native code as a malloc'd, NUL-terminated copy. The call
 * site releases it with ns_rt_free_cstr once the callee returns, so a library
 * that keeps the text past the call copies it. */
int64_t ns_rt_to_cstr(int64_t str) {
    return (int64_t)(uintptr_t)ns_rt_cstr(str);
}

void ns_rt_free_cstr(int64_t ptr) {
    free((void *)(uintptr_t)ptr);
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
    int32_t returns;
    uint32_t hold;
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

// Heap lock held for both.
static void ns_rt_hold_update(void) {
    uint32_t max = 0;
    for (int32_t i = 0; i < ns_rt_hold_n; ++i) {
        if (ns_rt_holds[i] > max) max = ns_rt_holds[i];
    }
    atomic_store_explicit(&ns_rt_hold, max, memory_order_release);
}

static void ns_rt_hold_add(ns_rt_task *t) {
    t->hold = 0;
    if (ns_rt_hold_n >= NS_RT_HOLD_MAX) {
        // Too many tasks at once to track: keep their captures for good.
        ns_rt_raise_floor(ns_rt_used);
        return;
    }
    t->hold = ns_rt_used;
    ns_rt_holds[ns_rt_hold_n++] = t->hold;
    ns_rt_hold_update();
}

// A thread task has run to its end. Its result may be an address the awaiting
// thread has yet to read, so a task that returns one keeps everything; then
// its hold goes, and its runs stop belonging to a live thread.
static void ns_rt_task_release(ns_rt_task *t) {
    ns_rt_heap_lock();
    if (t->returns) ns_rt_raise_floor(ns_rt_used);
    if (t->hold) {
        for (int32_t i = 0; i < ns_rt_hold_n; ++i) {
            if (ns_rt_holds[i] == t->hold) {
                ns_rt_holds[i] = ns_rt_holds[--ns_rt_hold_n];
                break;
            }
        }
        t->hold = 0;
        ns_rt_hold_update();
    }
    ns_rt_thread self = ns_rt_self();
    for (int32_t i = 0; i < ns_rt_run_n; ++i) {
        if (!ns_rt_runs[i].dead && ns_rt_thread_eq(ns_rt_runs[i].owner, self)) ns_rt_runs[i].dead = 1;
    }
    ns_rt_heap_unlock();
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
    ns_rt_task_release(t);
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
    ns_rt_task_release(t);
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

static int64_t ns_rt_task_start(int64_t fn, int64_t env, int64_t nargs,
                                int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                                int64_t queue, int32_t returns) {
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
    t->returns = returns;
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
    // The compiler does not pin what a task is handed (ns_ssa.c): the hold
    // keeps it while the task runs.
    ns_rt_heap_lock();
    ns_rt_init_locked();
    ns_rt_hold_add(t);
    ns_rt_heap_unlock();
#ifdef _WIN32
    t->thread = CreateThread(NULL, 0, ns_rt_task_thread, t, 0, NULL);
    t->started = t->thread != NULL;
    if (t->started) CloseHandle(t->thread);
#else
    t->started = pthread_create(&t->thread, NULL, ns_rt_task_thread, t) == 0;
    // Task completion is joined through its condition variable. No OS join
    // consumes this thread, so release its resources automatically at exit.
    if (t->started) pthread_detach(t->thread);
#endif
    if (!t->started) {
        ns_rt_heap_lock();
        if (t->hold) {
            for (int32_t i = 0; i < ns_rt_hold_n; ++i) {
                if (ns_rt_holds[i] == t->hold) {
                    ns_rt_holds[i] = ns_rt_holds[--ns_rt_hold_n];
                    break;
                }
            }
            t->hold = 0;
            ns_rt_hold_update();
        }
        ns_rt_heap_unlock();
        ns_rt_task_finish(t, 0, 1);
    }
    return handle;
}

int64_t ns_rt_task_spawn(int64_t fn, int64_t env, int64_t nargs,
                         int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                         int64_t queue) {
    return ns_rt_task_start(fn, env, nargs, a0, a1, a2, a3, queue, 1);
}

// A dispatched block returns nothing the caller can read.
int64_t ns_rt_task_dispatch(int64_t queue, int64_t fnval) {
    int64_t code = fnval > 0 ? ns_rt_load(fnval, 0, 8) : 0;
    return ns_rt_task_start(code, fnval, 0, 0, 0, 0, 0, queue, 0);
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
// Omitted when this file is compiled into bin/ns (NS_HOST_TOOL), which already
// defines _ns_malloc in ns_type.c and links src/ns_os.c, and when compiled into
// a generated Apple app (NS_XCLIB), which links Runtime/src/ns_os.c. The
// separate bin/ns_native_rt.o keeps these fallbacks for `ns build` artifacts.
#if !defined(NS_HOST_TOOL) && !defined(NS_XCLIB)
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
