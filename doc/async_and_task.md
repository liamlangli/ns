# async, await and the dispatch api

Language-level concurrency for ns is built from one primitive: the **task**.
A task is a unit of work that runs concurrently with the code that created it
and is observed through a first-class `task` handle. Tasks are created two
ways, both of which return a `task` handle immediately:

- calling an `async fn`
- passing a block or a function value to `dispatch`

```ns
use task

// an async fn runs concurrently when called; the call returns a task handle
async fn work(n: i32) i32 {
    sleep(5)
    return n * 2
}

fn main() {
    let t = work(21)            // t: task[i32]; the body starts concurrently
    let r = await t             // suspend until finished, r == 42

    // dispatch a block onto a worker thread. every variable the block
    // references is captured automatically and carried into the task.
    let base = 10
    let d = dispatch() { in return base + 32 }
    let x = await d             // x == 42

    // a task can be waited on without consuming its result, or cancelled
    let s = dispatch() { in
        let i = 0
        loop i < 1000 {
            sleep(10)           // cooperative suspension point
            i = i + 1
        }
        return i
    }
    cancel(s)                   // request cooperative cancellation
    wait(s)                     // block until the task has actually stopped
    assert cancelled(s)
}
```

## the `task` module

`use task` imports the concurrency api. Like `std` and `shader` it is a
VM-internal module: its functions are interpreter intrinsics, not FFI calls.

| fn | signature | behavior |
| -- | -- | -- |
| `dispatch` | `(f: any) task` | run a zero-arg block or fn value on a new worker thread; returns its task handle |
| `wait` | `(t: task) void` | block the current task until `t` finishes or is cancelled |
| `cancel` | `(t: task) void` | request cooperative cancellation of `t` |
| `done` | `(t: task) bool` | true once `t` has finished (normally or cancelled) |
| `cancelled` | `(t: task) bool` | true when `t` ended due to cancellation (or a runtime error) |
| `sleep` | `(ms: i32) void` | suspend the current task; other tasks run during the sleep |

`await` is a keyword, not a module fn, and is always available:

```ns
let v = await expr   // expr must be task-typed
```

## typing rules

- `task` is a value type holding an opaque handle (one pointer). The bare
  name `task` is usable in signatures after `use task`.
- Each task type carries its **result type**. `work(21)` above has the
  static type `task[i32]`; `await` on it yields `i32`. Task types are
  interned like dict/set container types, so no user-facing generics are
  introduced (in line with the language's no-generics design goal).
- `await e` requires `e` to be task-typed and evaluates to the task's result
  type. Awaiting a bare `task` (result type unknown) yields `any`.
- Calling an `async fn f(...) T` type-checks its arguments exactly like a
  normal call but the call expression has type `task[T]`, never `T`.
  `return` statements inside the async body still check against `T`.
- `dispatch(f)` requires `f` to be a block or fn value taking no arguments;
  the call has type `task[R]` where `R` is the closure's return type. A
  block without an explicit return type infers it from its first `return`
  statement. Dispatching an `async fn` value is rejected — call it instead.

## execution model: stackful tasks, cooperative interpreter lock

The interpreter implements tasks as **stackful concurrency**: every task owns
a real execution stack for the whole of its run, so `await`, `wait` and
`sleep` may appear at any depth of the call chain (unlike stackless
coroutines, no function-coloring or state-machine transform is needed). In
the current runtime each task's stack is an OS worker thread created at spawn
time; the design permits swapping this for user-space fibers later without
changing language semantics.

Interpreter state (the eval stacks, symbol tables, string table) is guarded
by a single **vm lock**. Exactly one task interprets code at a time; the lock
is handed over cooperatively at well-defined suspension points:

- `await` / `wait` — released until the target task completes
- `sleep` — released for the duration
- statement boundaries — the running task periodically offers the lock to
  other runnable tasks (time-sliced, every N statements)
- task start and finish

Each task keeps its own eval context (call stack, scope stack, symbol stack,
value stack); acquiring the vm lock swaps the owner's context into the vm.
This gives concurrent, interleaved execution with I/O overlap — the same
model as Python's GIL or JavaScript workers sharing a runtime — while keeping
the single-threaded interpreter core untouched. Native FFI calls currently
run while holding the lock, so a long blocking native call delays other
tasks.

## capture and value-passing semantics

A dispatched block captures its free variables automatically (the compiler
already computes the capture set for every block). At the `dispatch` call the
capture record is **snapshotted onto the heap**, so the task keeps working
after the defining scope exits and never aliases the spawning task's eval
stack:

- **value captures** (numbers, bools, structs) are copied at dispatch time;
  later mutation of the original is not observed by the task
- **`ref` captures and reference-typed values** (native object refs, arrays,
  dicts, sets, strings) carry the reference into the worker; the underlying
  object is shared

Arguments of an `async fn` call are snapshotted the same way, and so is the
task's result value when it finishes. Shared containers are not synchronized
by the runtime; coordinate access with tasks (`await` ordering) when mutating
them from more than one task.

## cancellation

Cancellation is **cooperative**. `cancel(t)` sets a flag; the task observes
it at its next suspension point or statement boundary and unwinds by forcing
each active call frame to return. A cancelled task's result is the zero
value, `done(t)` becomes true and `cancelled(t)` reports true. Cancelling an
already-finished task has no effect. `await` on a cancelled task returns the
zero value; use `cancelled(t)` to distinguish.

A task that fails with a runtime error is also marked finished with
`cancelled(t) == true` (the error is reported to stderr).

## lifetime

Tasks do not outlive the evaluation that spawned them: when the program's
top-level evaluation finishes (or a REPL input completes), the runtime
requests cancellation of every task still running and joins the worker
threads before returning. Use `await`/`wait` to guarantee completion of work
you care about. Task handles remain valid to query (`done`, `cancelled`)
afterwards.

## kernel fns (future work)

`kernel fn` (data-parallel dispatch over index domains, e.g. for pixels)
remains reserved and unimplemented; it will build on the same task runtime.
