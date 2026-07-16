#include "ns_vm.h"

// task runtime: async/await + dispatch (doc/async_and_task.md)
//
// Tasks are stackful units of concurrency. Each task runs on its own worker
// thread, so awaits and sleeps may appear at any call depth. Interpreter
// state is guarded by a single vm lock: exactly one task interprets at a
// time, and the lock is handed over cooperatively at await/wait/sleep,
// statement boundaries (time slice) and task start/finish. Every task owns a
// private eval context (call/scope/symbol/value stacks); taking the lock
// swaps the owner's context into the shared ns_vm, so the single-threaded
// interpreter core runs unmodified.
//
// Values that cross a task boundary (captures, async-call arguments, the
// result) are snapshotted onto the heap: plain values are copied, reference
// values (arrays, dicts, sets, strings, native refs) share their referent.

ns_return_void ns_eval_compound_stmt(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_cast_number(ns_vm *vm, ns_value v, ns_type dst, ns_code_loc loc);

#ifdef NS_WIN
    #include <windows.h>
    typedef HANDLE ns_task_thread;
    typedef CRITICAL_SECTION ns_task_mutex;
    typedef CONDITION_VARIABLE ns_task_cond;
    #define ns_task_mutex_init(m) InitializeCriticalSection(m)
    #define ns_task_mutex_lock(m) EnterCriticalSection(m)
    #define ns_task_mutex_unlock(m) LeaveCriticalSection(m)
    #define ns_task_cond_init(c) InitializeConditionVariable(c)
    #define ns_task_cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
    #define ns_task_cond_broadcast(c) WakeAllConditionVariable(c)
    #define ns_task_thread_yield() SwitchToThread()
    static void ns_task_msleep(i32 ms) { Sleep((DWORD)ms); }
#else
    #include <pthread.h>
    #include <sched.h>
    #include <time.h>
    typedef pthread_t ns_task_thread;
    typedef pthread_mutex_t ns_task_mutex;
    typedef pthread_cond_t ns_task_cond;
    #define ns_task_mutex_init(m) pthread_mutex_init(m, ns_null)
    #define ns_task_mutex_lock(m) pthread_mutex_lock(m)
    #define ns_task_mutex_unlock(m) pthread_mutex_unlock(m)
    #define ns_task_cond_init(c) pthread_cond_init(c, ns_null)
    #define ns_task_cond_wait(c, m) pthread_cond_wait(c, m)
    #define ns_task_cond_broadcast(c) pthread_cond_broadcast(c)
    #define ns_task_thread_yield() sched_yield()
    static void ns_task_msleep(i32 ms) {
        struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
        nanosleep(&ts, ns_null);
    }
#endif

// statements interpreted between offers of the vm lock to other tasks
#define NS_TASK_SLICE 64
// sleep granularity, so cancellation interrupts a long sleep promptly
#define NS_TASK_SLEEP_CHUNK_MS 10

typedef enum {
    NS_TASK_PENDING = 0,
    NS_TASK_RUNNING,
    NS_TASK_DONE,
    NS_TASK_CANCELLED,
} ns_task_state;

// the eval-context fields of ns_vm that belong to a single task
typedef struct ns_task_ectx {
    ns_call *call_stack;
    ns_scope *scope_stack;
    ns_symbol *symbol_stack;
    i8 *stack;
    ns_code_loc loc;
    ns_str lib;
    i32 stack_depth;
} ns_task_ectx;

typedef struct ns_task {
    i32 id;
    volatile i32 state;      // ns_task_state; written under the vm lock
    volatile ns_bool cancel; // cooperative cancellation request
    ns_bool joined;
    ns_value result;         // snapshot of the body's return value
    ns_vm *vm;
    ns_ast_ctx *ctx;
    i32 callee;              // symbol index of the async fn / dispatched closure
    void *cap_base;          // heap snapshot of a dispatched block's captures
    ns_value *args;          // snapshot args of an async fn call (ns_array)
    ns_task_ectx ectx;       // saved eval context while not holding the vm lock
    ns_task_thread thread;
    struct ns_task *next;
} ns_task;

typedef struct ns_task_rt {
    ns_task_mutex gil;   // the vm lock
    ns_task_cond done_cv; // broadcast on task completion and cancellation requests
    ns_task *tasks;      // all tasks ever spawned in this vm (freed never; handles stay valid)
    ns_task *current;    // task whose context is loaded in the vm; null while unlocked
    ns_task main_task;   // pseudo-task for the primary thread
    ns_bool main_holds;  // primary thread is inside a top-level eval
    i32 next_id;
    i32 slice;
    i32 live;            // unfinished worker tasks
    ns_type handle_t;    // cached task handle type
} ns_task_rt;

static void ns_task_ectx_save(ns_vm *vm, ns_task_ectx *c) {
    c->call_stack = vm->call_stack;
    c->scope_stack = vm->scope_stack;
    c->symbol_stack = vm->symbol_stack;
    c->stack = vm->stack;
    c->loc = vm->loc;
    c->lib = vm->lib;
    c->stack_depth = vm->stack_depth;
}

static void ns_task_ectx_load(ns_vm *vm, ns_task_ectx *c) {
    vm->call_stack = c->call_stack;
    vm->scope_stack = c->scope_stack;
    vm->symbol_stack = c->symbol_stack;
    vm->stack = c->stack;
    vm->loc = c->loc;
    vm->lib = c->lib;
    vm->stack_depth = c->stack_depth;
}

// take the vm lock and install t's eval context
static void ns_task_lock(ns_vm *vm, ns_task *t) {
    ns_task_rt *rt = vm->task_rt;
    ns_task_mutex_lock(&rt->gil);
    ns_task_ectx_load(vm, &t->ectx);
    rt->current = t;
}

// save the owner's eval context and release the vm lock
static void ns_task_unlock(ns_vm *vm) {
    ns_task_rt *rt = vm->task_rt;
    ns_task_ectx_save(vm, &rt->current->ectx);
    rt->current = ns_null;
    ns_task_mutex_unlock(&rt->gil);
}

// suspend the owner on the completion condvar; the lock is re-taken and the
// owner's context re-installed before returning
static void ns_task_wait_done(ns_vm *vm) {
    ns_task_rt *rt = vm->task_rt;
    ns_task *self = rt->current;
    ns_task_ectx_save(vm, &self->ectx);
    rt->current = ns_null;
    ns_task_cond_wait(&rt->done_cv, &rt->gil);
    ns_task_ectx_load(vm, &self->ectx);
    rt->current = self;
}

// create the runtime on first spawn. The creating (primary) thread is mid-eval
// and owns the vm, so it enters the lock protocol as the current holder.
static ns_task_rt *ns_task_rt_get(ns_vm *vm) {
    if (vm->task_rt) return (ns_task_rt *)vm->task_rt;
    ns_task_rt *rt = (ns_task_rt *)ns_malloc(sizeof(ns_task_rt));
    memset(rt, 0, sizeof(ns_task_rt));
    ns_task_mutex_init(&rt->gil);
    ns_task_cond_init(&rt->done_cv);
    rt->main_task.id = 0;
    rt->main_task.state = NS_TASK_RUNNING;
    rt->main_task.vm = vm;
    rt->next_id = 1;
    rt->slice = NS_TASK_SLICE;
    ns_task_mutex_lock(&rt->gil);
    rt->current = &rt->main_task;
    rt->main_holds = true;
    vm->task_rt = rt;
    return rt;
}

ns_type ns_task_type(ns_vm *vm, ns_type val) {
    // a closure without an explicit or inferable return type produces no result
    if (ns_type_is(val, NS_TYPE_INFER) || ns_type_is_unknown(val)) val = ns_type_void;
    return ns_vm_intern_container_type(vm, NS_TYPE_TASK, ns_type_nil, val);
}

void ns_task_register_module(ns_vm *vm) {
    ns_str name = ns_str_cstr("task");
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        if (vm->symbols[i].type == NS_SYMBOL_CONTAINER && ns_str_equals(vm->symbols[i].name, name)) return;
    }
    i32 index = ns_array_length(vm->symbols);
    ns_symbol ct = (ns_symbol){.type = NS_SYMBOL_CONTAINER, .parsed = true, .lib = name};
    ct.ct.t = ns_type_encode(NS_TYPE_TASK, index, 0, true, true);
    ct.ct.key = ns_type_nil;
    ct.ct.val = (ns_type){.type = NS_TYPE_ANY};
    ct.name = name;
    ns_vm_push_symbol_global(vm, ct);
}

// the handle type used for task values created at eval time. The parse pass
// interned a task container type for every spawn site, so a scan normally
// finds one; the fallback intern only triggers for handles created outside a
// parsed program.
static ns_type ns_task_handle_type(ns_vm *vm) {
    ns_task_rt *rt = ns_task_rt_get(vm);
    if (!ns_type_is_unknown(rt->handle_t)) return rt->handle_t;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        if (s->type == NS_SYMBOL_CONTAINER && ns_type_is(s->ct.t, NS_TYPE_TASK)) {
            rt->handle_t = ns_type_set_stack(s->ct.t, false);
            return rt->handle_t;
        }
    }
    rt->handle_t = ns_type_set_stack(ns_task_type(vm, (ns_type){.type = NS_TYPE_ANY}), false);
    return rt->handle_t;
}

// eval-side lookup of the interned task type for result type `val`. Never
// pushes a symbol: eval must not grow vm->symbols while call frames hold
// symbol pointers, so fall back to the generic handle type when the parse
// pass did not intern the precise one.
static ns_type ns_task_find_type(ns_vm *vm, ns_type val) {
    if (ns_type_is(val, NS_TYPE_INFER) || ns_type_is_unknown(val)) val = ns_type_void;
    for (i32 i = 0, l = ns_array_length(vm->symbols); i < l; ++i) {
        ns_symbol *s = &vm->symbols[i];
        if (s->type == NS_SYMBOL_CONTAINER && ns_type_is(s->ct.t, NS_TYPE_TASK) && ns_type_equals(s->ct.val, val)) {
            return ns_type_set_stack(s->ct.t, false);
        }
    }
    return ns_task_handle_type(vm);
}

static ns_task *ns_task_from_value(ns_vm *vm, ns_value v) {
    if (!ns_type_is(v.t, NS_TYPE_TASK)) return ns_null;
    u64 h = ns_type_in_stack(v.t) ? *(u64 *)&vm->stack[v.o] : v.o;
    return (ns_task *)h;
}

// snapshot a value out of the current eval context so another task (or a
// later point in time) can read it: plain values become self-contained
// copies, reference values keep sharing their referent.
static ns_value ns_task_snapshot_value(ns_vm *vm, ns_value v) {
    ns_type t = v.t;
    if (ns_type_is(t, NS_TYPE_NIL) || ns_type_is(t, NS_TYPE_VOID) || ns_type_is_unknown(t)) return ns_nil;

    if (ns_type_is(t, NS_TYPE_STRING) && !ns_type_is_array(t)) {
        // strings are indices into the shared vm string table
        u64 idx = ns_type_in_stack(t) ? *(u64 *)&vm->stack[v.o] : v.o;
        return (ns_value){.t = ns_type_str, .o = idx};
    }

    if (ns_type_is_array(t) || ns_type_is(t, NS_TYPE_DICT) || ns_type_is(t, NS_TYPE_SET) ||
        ns_type_is(t, NS_TYPE_FN) || ns_type_is(t, NS_TYPE_TASK)) {
        // a single u64 heap handle; the container/fn itself is shared
        u64 handle = ns_type_in_stack(t) ? *(u64 *)&vm->stack[v.o] : v.o;
        return (ns_value){.t = ns_type_set_mut(ns_type_set_stack(t, false), true), .o = handle};
    }

    if (ns_type_is_ref(t)) {
        // an absolute ref shares its referent; a ref into this context's value
        // stack would dangle once the context is gone, so copy the referent
        if (!ns_type_in_stack(t)) return v;
        ns_type base = ns_type_set_ref(t, false);
        i32 size = ns_type_size(vm, base);
        if (size <= 0) return ns_nil;
        void *buf = ns_malloc((szt)size);
        memcpy(buf, &vm->stack[v.o], (szt)size);
        return (ns_value){.t = ns_type_set_stack(t, false), .o = (u64)buf};
    }

    if (ns_type_is(t, NS_TYPE_BLOCK)) {
        // heap-copy the capture record so the closure survives its scope
        ns_symbol *sym = &vm->symbols[ns_type_index(t)];
        u64 stride = sym->bc.st.stride;
        if (stride == 0) return (ns_value){.t = ns_type_set_stack(t, false), .o = 0};
        void *buf = ns_malloc((szt)stride);
        const void *src = ns_type_in_stack(t) ? (const void *)&vm->stack[v.o] : (const void *)v.o;
        memcpy(buf, src, (szt)stride);
        return (ns_value){.t = ns_type_set_mut(ns_type_set_stack(t, false), true), .o = (u64)buf};
    }

    if (ns_type_is(t, NS_TYPE_STRUCT) || ns_type_is(t, NS_TYPE_UNION)) {
        i32 size = ns_type_size(vm, t);
        if (size <= 0) return ns_nil;
        void *buf = ns_malloc((szt)size);
        const void *src = ns_type_in_stack(t) ? (const void *)&vm->stack[v.o] : (const void *)v.o;
        memcpy(buf, src, (szt)size);
        return (ns_value){.t = ns_type_set_mut(ns_type_set_stack(t, false), true), .o = (u64)buf};
    }

    // numbers and bools become immediates
    if (ns_type_is_const(t)) return v;
    i32 size = ns_type_size(vm, t);
    ns_value out = (ns_value){.t = ns_type_set_mut(ns_type_set_stack(t, false), false), .o = 0};
    if (size > 0 && size <= (i32)sizeof(u64)) {
        const void *src = ns_type_in_stack(t) ? (const void *)&vm->stack[v.o] : (const void *)v.o;
        memcpy(&out.o, src, (szt)size);
    }
    return out;
}

// run the task body in the (fresh) context of the calling worker thread
static ns_return_value ns_task_invoke(ns_vm *vm, ns_ast_ctx *ctx, ns_task *t) {
    // root frame: symbol lookups scan locals of the innermost fn (or null)
    // frame, so a lone block frame would leave its own args/captures
    // invisible. The null-callee root anchors the scan, like the frame the
    // interpreter pushes for a program without a main fn.
    ns_scope_enter(vm);
    ns_call root = (ns_call){.callee = ns_null, .scope_top = 0};
    ns_array_push(vm->call_stack, root);

    ns_symbol *sym = &vm->symbols[t->callee];
    ns_fn_symbol *fn = ns_symbol_get_fn(sym);

    ns_value ret_val = (ns_value){.t = ns_type_set_stack(fn->ret, true), .o = 0};
    i32 ret_size = ns_type_size(vm, fn->ret);
    if (ret_size > 0) {
        ret_val.o = ns_eval_alloc(vm, ret_size);
        memset(&vm->stack[ret_val.o], 0, (szt)ret_size); // cancelled tasks yield zeroes
    }

    ns_call call = (ns_call){.callee = sym, .scope_top = ns_array_length(vm->scope_stack), .ret = ret_val, .ret_set = false, .arg_offset = ns_array_length(vm->symbol_stack), .arg_count = ns_array_length(fn->args)};
    ns_scope_enter(vm);

    for (i32 a_i = 0, l = (i32)ns_array_length(t->args); a_i < l && a_i < (i32)ns_array_length(fn->args); ++a_i) {
        ns_symbol arg = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = fn->args[a_i].name, .val = t->args[a_i], .parsed = true};
        ns_array_push(vm->symbol_stack, arg);
    }

    ns_array_push(vm->call_stack, call);
    if (sym->type == NS_SYMBOL_BLOCK && t->cap_base) {
        // captured fields live in the heap snapshot; address them absolutely
        szt field_count = ns_array_length(sym->bc.st.fields);
        for (szt f_i = 0; f_i < field_count; ++f_i) {
            ns_struct_field *field = &sym->bc.st.fields[f_i];
            i8 *slot = (i8 *)t->cap_base + field->o;
            // mut=1: a non-stack value with mut=0 would be read as an immediate
            // (union bits) instead of dereferencing the snapshot slot
            ns_value v = ns_type_is_ref(field->t)
                ? (ns_value){.t = ns_type_set_stack(field->t, false), .o = *(u64 *)slot}
                : (ns_value){.t = ns_type_set_mut(ns_type_set_stack(field->t, false), true), .o = (u64)slot};
            ns_symbol cap = (ns_symbol){.type = NS_SYMBOL_VALUE, .name = field->name, .val = v, .parsed = true};
            ns_array_push(vm->symbol_stack, cap);
        }
    }

    ns_return_void ret = ns_eval_compound_stmt(vm, ctx, fn->body);
    if (ns_return_is_error(ret)) {
        (void)ns_array_pop(vm->call_stack);
        ns_scope_exit(vm);
        (void)ns_array_pop(vm->call_stack); // root frame
        ns_scope_exit(vm);
        return ns_return_change_type(value, ret);
    }
    call = ns_array_pop(vm->call_stack);
    ns_scope_exit(vm);
    (void)ns_array_pop(vm->call_stack); // root frame
    ns_scope_exit(vm);
    return ns_return_ok(value, call.ret);
}

static void *ns_task_thread_main(void *arg) {
    ns_task *t = (ns_task *)arg;
    ns_vm *vm = t->vm;
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;

    ns_task_lock(vm, t); // installs the task's fresh (empty) eval context
    t->state = NS_TASK_RUNNING;

    ns_bool failed = false;
    ns_value ret = ns_nil;
    if (!t->cancel) {
        ns_return_value r = ns_task_invoke(vm, t->ctx, t);
        if (ns_return_is_error(r)) {
            failed = true;
            ns_warn("task", "task #%d failed: %.*s\n", t->id, r.e.msg.len, r.e.msg.data);
        } else {
            ret = r.r;
        }
    }

    if (!failed && !t->cancel) {
        t->result = ns_task_snapshot_value(vm, ret);
        t->state = NS_TASK_DONE;
    } else {
        t->result = ns_nil;
        t->state = NS_TASK_CANCELLED;
    }
    rt->live--;

    // the context is finished: release its eval arrays before handing the vm back
    ns_array_free(vm->call_stack);
    ns_array_free(vm->scope_stack);
    ns_array_free(vm->symbol_stack);
    ns_array_free(vm->stack);
    vm->stack_depth = 0;

    ns_task_cond_broadcast(&rt->done_cv);
    ns_task_unlock(vm);
    return ns_null;
}

#ifdef NS_WIN
static DWORD WINAPI ns_task_thread_entry(LPVOID arg) {
    ns_task_thread_main(arg);
    return 0;
}
static ns_bool ns_task_thread_create(ns_task_thread *th, ns_task *t) {
    *th = CreateThread(ns_null, 0, ns_task_thread_entry, t, 0, ns_null);
    return *th != ns_null;
}
static void ns_task_thread_join(ns_task_thread th) {
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
}
#else
static ns_bool ns_task_thread_create(ns_task_thread *th, ns_task *t) {
    return pthread_create(th, ns_null, ns_task_thread_main, t) == 0;
}
static void ns_task_thread_join(ns_task_thread th) {
    pthread_join(th, ns_null);
}
#endif

static ns_task *ns_task_create(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_task_rt *rt = ns_task_rt_get(vm);
    ns_task *t = (ns_task *)ns_malloc(sizeof(ns_task));
    memset(t, 0, sizeof(ns_task));
    t->id = rt->next_id++;
    t->vm = vm;
    t->ctx = ctx;
    t->state = NS_TASK_PENDING;
    t->result = ns_nil;
    t->next = rt->tasks;
    rt->tasks = t;
    rt->live++;
    return t;
}

static ns_return_value ns_task_start(ns_vm *vm, ns_task *t, ns_type handle_t, ns_code_loc loc) {
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
    if (!ns_task_thread_create(&t->thread, t)) {
        rt->live--;
        t->state = NS_TASK_CANCELLED;
        t->joined = true;
        return ns_return_error(value, loc, NS_ERR_RUNTIME, "failed to start task thread.");
    }
    ns_value handle = (ns_value){.t = handle_t, .o = (u64)t};
    return ns_return_ok(value, handle);
}

// statement safepoint. Offers the vm lock to other runnable tasks once per
// time slice, and unwinds the current task when it has been cancelled by
// forcing a pending return on the innermost call frame (each enclosing frame
// re-triggers at its own next statement, so the whole task unwinds without
// touching the error machinery).
ns_bool ns_task_step(ns_vm *vm) {
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
    if (!rt || !rt->current) return true;
    ns_task *self = rt->current;
    ns_bool is_main = self == &rt->main_task;
    if (--rt->slice <= 0) {
        rt->slice = NS_TASK_SLICE;
        if (rt->live > (is_main ? 0 : 1)) {
            ns_task_unlock(vm);
            ns_task_thread_yield();
            ns_task_lock(vm, self);
        }
    }
    if (!is_main && self->cancel) {
        i32 l = ns_array_length(vm->call_stack);
        if (l > 0) vm->call_stack[l - 1].ret_set = true;
        return false;
    }
    return true;
}

static ns_return_value ns_task_wait_value(ns_vm *vm, ns_value tv, ns_code_loc loc) {
    ns_task *target = ns_task_from_value(vm, tv);
    if (!target) return ns_return_error(value, loc, NS_ERR_RUNTIME, "await expects a task value.");
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
    if (!rt) return ns_return_error(value, loc, NS_ERR_RUNTIME, "task runtime not active.");
    ns_task *self = rt->current;
    if (self == target) return ns_return_error(value, loc, NS_ERR_RUNTIME, "task can not await itself.");
    while (target->state < NS_TASK_DONE) {
        if (self != &rt->main_task && self->cancel) {
            i32 l = ns_array_length(vm->call_stack);
            if (l > 0) vm->call_stack[l - 1].ret_set = true;
            return ns_return_ok(value, ns_nil);
        }
        ns_task_wait_done(vm);
        self = rt->current;
    }
    return ns_return_ok(value, target->result);
}

ns_return_value ns_task_await(ns_vm *vm, ns_value task_v, ns_code_loc loc) {
    return ns_task_wait_value(vm, task_v, loc);
}

static void ns_task_sleep_ms(ns_vm *vm, i32 ms) {
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
    if (ms <= 0) return;
    if (!rt || !rt->current) { // no task runtime yet: plain sleep
        ns_task_msleep(ms);
        return;
    }
    ns_task *self = rt->current;
    ns_bool is_main = self == &rt->main_task;
    ns_task_unlock(vm);
    i32 remain = ms;
    while (remain > 0) {
        if (!is_main && self->cancel) break;
        i32 chunk = remain < NS_TASK_SLEEP_CHUNK_MS ? remain : NS_TASK_SLEEP_CHUNK_MS;
        ns_task_msleep(chunk);
        remain -= chunk;
    }
    ns_task_lock(vm, self);
}

// spawn the body of an async fn call: evaluate the arguments in the caller's
// context, snapshot them, and start a worker task. The call expression's
// value is the task handle.
ns_return_value ns_task_spawn_async_call(ns_vm *vm, ns_ast_ctx *ctx, ns_symbol *sym, i32 call_i) {
    ns_ast_t *n = &ctx->nodes[call_i];
    ns_fn_symbol *fn = ns_symbol_get_fn(sym);
    i32 callee_index = (i32)ns_type_index(fn->fn.t);
    ns_code_loc loc = ns_ast_state_loc(ctx, n->state);
    // resolve the handle type before arg eval: evaluating an argument may
    // import a module and grow vm->symbols, leaving sym/fn stale
    ns_type handle_t = ns_task_find_type(vm, fn->ret);

    ns_value *args = ns_null;
    ns_scope_enter(vm);
    i32 next = n->next;
    for (i32 a_i = 0, l = n->call_expr.arg_count; a_i < l; ++a_i) {
        ns_return_value ret_v = ns_eval_expr(vm, ctx, next);
        if (ns_return_is_error(ret_v)) {
            ns_scope_exit(vm);
            ns_array_free(args);
            return ret_v;
        }
        ns_value v = ret_v.r;
        if (a_i < (i32)ns_array_length(fn->args)) {
            ns_type pt = fn->args[a_i].val.t;
            if (!ns_type_is_ref(pt) && ns_type_is_number(pt) && ns_type_is_number(v.t) && !ns_type_equals(pt, v.t)) {
                ns_return_value cast_ret = ns_eval_cast_number(vm, v, pt, loc);
                if (ns_return_is_error(cast_ret)) {
                    ns_scope_exit(vm);
                    ns_array_free(args);
                    return cast_ret;
                }
                v = cast_ret.r;
            }
        }
        ns_array_push(args, ns_task_snapshot_value(vm, v));
        next = ctx->nodes[next].next;
    }
    ns_scope_exit(vm);

    ns_task *t = ns_task_create(vm, ctx);
    t->callee = callee_index;
    t->args = args;
    return ns_task_start(vm, t, handle_t, loc);
}

// `task` module intrinsics; called from ns_eval_call_expr with the call frame
// and arguments already in place.
ns_return_bool ns_task_vm_call(ns_vm *vm, ns_ast_ctx *ctx) {
    ns_call *call = ns_array_last(vm->call_stack);
    ns_str name = call->callee->name;
    ns_code_loc loc = vm->loc;

    if (ns_str_equals(name, ns_str_cstr("dispatch"))) {
        ns_value closure = vm->symbol_stack[call->arg_offset].val;
        if (!ns_type_is(closure.t, NS_TYPE_FN) && !ns_type_is(closure.t, NS_TYPE_BLOCK)) {
            return ns_return_error(bool, loc, NS_ERR_RUNTIME, "dispatch expects a block or fn value.");
        }
        ns_symbol *sym = &vm->symbols[ns_type_index(closure.t)];
        ns_fn_symbol *fn = ns_symbol_get_fn(sym);
        if (ns_array_length(fn->args) > 0) {
            return ns_return_error(bool, loc, NS_ERR_RUNTIME, "dispatch closure must take no arguments.");
        }
        ns_type handle_t = ns_task_find_type(vm, fn->ret);
        ns_task *t = ns_task_create(vm, ctx);
        t->callee = (i32)ns_type_index(closure.t);
        if (sym->type == NS_SYMBOL_BLOCK && sym->bc.st.stride > 0) {
            // snapshot the captures so the task outlives the dispatching scope
            u64 stride = sym->bc.st.stride;
            t->cap_base = ns_malloc((szt)stride);
            const void *src = ns_type_in_stack(closure.t) ? (const void *)&vm->stack[closure.o] : (const void *)closure.o;
            memcpy(t->cap_base, src, (szt)stride);
        }
        ns_return_value ret = ns_task_start(vm, t, handle_t, loc);
        if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
        call = ns_array_last(vm->call_stack); // spawn may grow the call stack storage
        call->ret = ret.r;
    } else if (ns_str_equals(name, ns_str_cstr("wait"))) {
        ns_return_value ret = ns_task_wait_value(vm, vm->symbol_stack[call->arg_offset].val, loc);
        if (ns_return_is_error(ret)) return ns_return_change_type(bool, ret);
        call = ns_array_last(vm->call_stack);
        call->ret = ns_nil;
    } else if (ns_str_equals(name, ns_str_cstr("cancel"))) {
        ns_task *t = ns_task_from_value(vm, vm->symbol_stack[call->arg_offset].val);
        if (!t) return ns_return_error(bool, loc, NS_ERR_RUNTIME, "cancel expects a task value.");
        ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
        if (t->state < NS_TASK_DONE) {
            t->cancel = true;
            // wake tasks parked in await so a cancelled awaiter can unwind
            if (rt) ns_task_cond_broadcast(&rt->done_cv);
        }
        call->ret = ns_nil;
    } else if (ns_str_equals(name, ns_str_cstr("done"))) {
        ns_task *t = ns_task_from_value(vm, vm->symbol_stack[call->arg_offset].val);
        if (!t) return ns_return_error(bool, loc, NS_ERR_RUNTIME, "done expects a task value.");
        call->ret = (ns_value){.t = ns_type_bool, .b = t->state >= NS_TASK_DONE};
    } else if (ns_str_equals(name, ns_str_cstr("cancelled"))) {
        ns_task *t = ns_task_from_value(vm, vm->symbol_stack[call->arg_offset].val);
        if (!t) return ns_return_error(bool, loc, NS_ERR_RUNTIME, "cancelled expects a task value.");
        call->ret = (ns_value){.t = ns_type_bool, .b = t->state == NS_TASK_CANCELLED};
    } else if (ns_str_equals(name, ns_str_cstr("sleep"))) {
        i32 ms = ns_eval_number_i32(vm, vm->symbol_stack[call->arg_offset].val);
        ns_task_sleep_ms(vm, ms);
        call = ns_array_last(vm->call_stack);
        call->ret = ns_nil;
    } else {
        return ns_return_error(bool, loc, NS_ERR_RUNTIME, "unknown task fn.");
    }
    return ns_return_ok(bool, true);
}

ns_str ns_task_fmt(ns_vm *vm, ns_value task_v) {
    ns_task *t = ns_task_from_value(vm, task_v);
    const_str state = "nil";
    i32 id = 0;
    if (t) {
        id = t->id;
        switch (t->state) {
        case NS_TASK_PENDING: state = "pending"; break;
        case NS_TASK_RUNNING: state = "running"; break;
        case NS_TASK_DONE: state = "done"; break;
        case NS_TASK_CANCELLED: state = "cancelled"; break;
        default: break;
        }
    }
    szt len = snprintf(ns_null, 0, "task#%d[%s]", id, state);
    i8 *data = (i8 *)ns_malloc(len + 1);
    snprintf(data, len + 1, "task#%d[%s]", id, state);
    return (ns_str){.data = data, .len = (i32)len, .dynamic = 1};
}

void ns_task_eval_enter(ns_vm *vm) {
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
    if (!rt || rt->main_holds) return;
    ns_task_lock(vm, &rt->main_task);
    rt->main_holds = true;
}

// end of a top-level eval: tasks must not outlive the ast they interpret, so
// request cancellation of everything still running and join the workers. Task
// handles stay valid for done/cancelled queries afterwards.
void ns_task_eval_exit(ns_vm *vm) {
    ns_task_rt *rt = (ns_task_rt *)vm->task_rt;
    if (!rt || !rt->main_holds) return;
    for (ns_task *t = rt->tasks; t; t = t->next) {
        if (t->state < NS_TASK_DONE) t->cancel = true;
    }
    ns_task_cond_broadcast(&rt->done_cv);
    rt->main_holds = false;
    ns_task_unlock(vm);
    for (ns_task *t = rt->tasks; t; t = t->next) {
        if (!t->joined) {
            ns_task_thread_join(t->thread);
            t->joined = true;
        }
    }
}
