#pragma once

#include "ns_ast.h"
#include "ns_type.h"

typedef enum {
    NS_SYMBOL_INVALID,
    NS_SYMBOL_VALUE,
    NS_SYMBOL_FN,
    NS_SYMBOL_STRUCT,
    NS_SYMBOL_BLOCK,
    NS_SYMBOL_TYPE,
    NS_SYMBOL_UNION,
    NS_SYMBOL_CONTAINER
} ns_symbol_type;

typedef enum ns_scope_state {
    NS_SCOPE_LINEAR,
    NS_SCOPE_CONTINUE,
    NS_SCOPE_BREAK,
    NS_SCOPE_BLOCK,
} ns_scope_state;

typedef struct ns_symbol ns_symbol;

typedef struct ns_fn_symbol {
    i32 ast;
    i32 body;
    ns_value fn;
    ns_type ret;
    ns_symbol *args;
    i32 arg_required;
    void *fn_ptr;
    ns_fn_type fn_type; // NS_FN_ASYNC: calls spawn a task and yield a task handle
} ns_fn_symbol;

typedef struct ns_struct_field {
    ns_str name;
    ns_type t;
    u64 o, s;
} ns_struct_field;

typedef struct ns_struct_symbol {
    i32 ast;
    ns_value st;
    u64 stride;
    ns_struct_field *fields;
} ns_struct_symbol;

typedef struct ns_scope_symbol {
    ns_symbol *vars;
} ns_scope_symbol;

typedef struct ns_array_symbol {
    ns_type element_type;
    ns_bool stack;
} ns_array_symbol;

typedef struct ns_block_symbol {
    i32 ast;
    ns_value val;
    ns_struct_symbol st;
    ns_fn_symbol fn;
} ns_block_symbol;

typedef struct ns_type_symbol {
    ns_type t;
} ns_type_symbol;

// A union type (`type T = A | B | C`) carries its own union type tag plus the
// list of member types it admits. A value is assignable to the union when its
// concrete type matches any member.
typedef struct ns_union_symbol {
    ns_type t;
    ns_type *members;
} ns_union_symbol;

typedef struct ns_container_symbol {
    ns_type t;
    ns_type key;
    ns_type val;
} ns_container_symbol;

typedef struct ns_symbol {
    ns_symbol_type type;
    ns_str name;
    ns_str lib;
    ns_bool parsed;
    union {
        ns_value val;
        ns_fn_symbol fn;
        ns_struct_symbol st;
        ns_block_symbol bc;
        ns_union_symbol un;
        ns_container_symbol ct;
        ns_type t;
    };
} ns_symbol;

typedef struct ns_fn {
    ns_str name;
    i32 ast;
} ns_fn;

typedef struct ns_struct {
    ns_str name;
    i32 index;
    ns_str *field_names;
} ns_struct;

typedef struct ns_scope {
    i32 stack_top;
    i32 symbol_top;
    ns_scope_state state: 4;
} ns_scope;

typedef struct ns_call {
    ns_symbol *callee;
    i32 arg_offset, arg_count;
    ns_value ret;
    u32 scope_top;
    ns_bool ret_set: 2;
    ns_bool brk_set: 2; // `break` requested; cleared by the enclosing loop
    ns_bool cnt_set: 2; // `continue` requested; cleared by the enclosing loop
} ns_call;

typedef struct ns_lib {
    void *lib;
    ns_str name;
    ns_str path;
} ns_lib;

typedef struct ns_vm {
    // parse state
    ns_symbol *symbols;
    i32 symbol_top; // parse symbol top
    u32 symbol_gen; // bumped on global symbol push / lib switch; invalidates ns_sym_cache

    // eval state
    ns_call *call_stack;
    ns_scope *scope_stack;
    ns_symbol *symbol_stack;

    ns_str *str_list;
    ns_data *data_list;
    ns_lib *libs;
    ns_str lib;
    ns_str ref_path;
    ns_str lib_path;
    ns_str lib_fallback_path;
    i8* stack;

    // mode
    ns_bool repl;

    // code
    ns_code_loc loc;

    i32 stack_depth;

    // task runtime (async/await + dispatch), created lazily on first spawn.
    // Non-null means other tasks may be runnable and the vm lock protocol is
    // active; see src/ns_task.c and doc/async_and_task.md.
    void *task_rt;
} ns_vm;

#define NS_MAX_STACK_DEPTH 255

// ops fn
ns_str ns_ops_name(ns_token_t op);
ns_str ns_ops_override_name(ns_str l, ns_str r, ns_token_t op);

// type
ns_bool ns_type_match(ns_vm *vm, ns_type require, ns_type provide); // check if provide type can be converted to require type
ns_number_type ns_vm_number_type(ns_type t);
ns_type ns_vm_number_type_upgrade(ns_type l, ns_type r);

// vm task (async/await + dispatch). Tasks are stackful: each runs on its own
// worker thread; a single vm lock serializes interpreter execution and is
// handed over at await/wait/sleep/statement boundaries (doc/async_and_task.md).
ns_type ns_vm_intern_container_type(ns_vm *vm, ns_value_type kind, ns_type key, ns_type val);
ns_type ns_task_type(ns_vm *vm, ns_type val); // intern task type with result type `val`
void ns_task_register_module(ns_vm *vm);      // push the bare `task` type symbol (on `use task`)
ns_bool ns_task_step(ns_vm *vm);              // statement safepoint; false when the current task is cancelled
ns_return_value ns_task_spawn_async_call(ns_vm *vm, ns_ast_ctx *ctx, ns_symbol *sym, i32 call_i);
ns_return_value ns_task_await(ns_vm *vm, ns_value task_v, ns_code_loc loc);
ns_return_bool ns_task_vm_call(ns_vm *vm, ns_ast_ctx *ctx); // `task` module intrinsics (dispatch/wait/...)
ns_str ns_task_fmt(ns_vm *vm, ns_value task_v);
void ns_task_eval_enter(ns_vm *vm); // re-acquire the vm lock when a top-level eval begins
void ns_task_eval_exit(ns_vm *vm);  // cancel + join outstanding tasks when a top-level eval ends
// Temporarily hand off the VM lock around a known-blocking foreign call.
// The opaque owner token is null when the task runtime is inactive.
void *ns_task_ffi_leave(ns_vm *vm);
void ns_task_ffi_reenter(ns_vm *vm, void *owner);

// vm parse stage
void ns_vm_push_symbol_global(ns_vm *vm, ns_symbol r);
i32 ns_vm_push_string(ns_vm *vm, ns_str s);
i32 ns_vm_push_data(ns_vm *vm, ns_data d);
i32 ns_type_size(ns_vm *vm, ns_type t);
ns_str ns_vm_get_type_name(ns_vm *vm, ns_type t);
i32 ns_vm_get_last_call(ns_vm *vm);
ns_symbol* ns_vm_find_symbol(ns_vm *vm, ns_str s, ns_bool capture);
ns_symbol* ns_vm_find_symbol_cached(ns_vm *vm, ns_str s, ns_sym_cache *c); // eval-only, no capture
ns_fn_symbol* ns_symbol_get_fn(ns_symbol *s);
ns_return_bool ns_vm_parse(ns_vm *vm, ns_ast_ctx *ctx);
ns_return_type ns_vm_parse_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i, ns_type t);
ns_return_type ns_vm_parse_type(ns_vm *vm, ns_ast_ctx *ctx, ns_ast_t *n);
ns_return_type ns_vm_parse_type_by_token(ns_vm *vm, ns_token_t t, ns_code_loc loc);

// eval fn
#define ns_eval_value_def(type) type ns_eval_number_##type(ns_vm *vm, ns_value n);
ns_eval_value_def(i8)
ns_eval_value_def(i16)
ns_eval_value_def(i32)
ns_eval_value_def(i64)
ns_eval_value_def(u8)
ns_eval_value_def(u16)
ns_eval_value_def(u32)
ns_eval_value_def(u64)
ns_eval_value_def(f32)
ns_eval_value_def(f64)
ns_bool ns_eval_bool(ns_vm *vm, ns_value n);
ns_str ns_eval_str(ns_vm *vm, ns_value n);
void *ns_eval_array_raw(ns_vm *vm, ns_value n);
u64 ns_eval_alloc(ns_vm *vm, i32 stride);
ns_return_value ns_eval_copy(ns_vm *vm, ns_value dst, ns_value src, i32 size);

ns_call *ns_call_enter(ns_vm *vm, ns_symbol *callee);
ns_call *ns_call_exit(ns_vm *vm);
ns_scope *ns_scope_enter(ns_vm *vm);
ns_scope *ns_scope_exit(ns_vm *vm);

ns_return_value ns_eval_var_def(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_module_globals(ns_vm *vm, ns_ast_ctx *ctx);
ns_return_value ns_eval_expr(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_value ns_eval_ast(ns_vm *vm, ns_ast_ctx *ctx);
ns_return_value ns_eval(ns_vm *vm, ns_str source, ns_str filename);
// Like ns_eval, but attaches a source map so diagnostics in a merged/linked
// translation unit report their original file and line (see ns_project_link).
ns_return_value ns_eval_with_map(ns_vm *vm, ns_str source, ns_str filename, ns_line_loc *line_map);

// vm eval stage
void ns_vm_symbol_print(ns_vm *vm);
ns_return_bool ns_vm_call_ref(ns_vm *vm);

// Invoke a closure (block or fn value) registered as a native callback, passing
// a single absolute pointer argument (e.g. the C `view*`) as the closure's
// `ref` parameter. `cap_base` is a heap snapshot of the closure's captured
// fields (or ns_null for a non-capturing closure). Re-enters the interpreter
// from a C trampoline.
ns_return_value ns_eval_invoke_callback(ns_vm *vm, ns_ast_ctx *ctx, ns_value closure, void *cap_base, void *arg_ptr);
// Build a C-callable `void(*)(void*)` trampoline that invokes `closure` via
// ns_eval_invoke_callback. Used to store an ns closure into a fn-pointer struct
// field consumed by native code. Returns the executable pointer (lives for the
// process lifetime), or ns_null if FFI is unavailable.
void *ns_callback_bridge_create(ns_vm *vm, ns_ast_ctx *ctx, ns_value closure);

// vm struct
i32 ns_struct_field_index(ns_symbol *st, ns_str s);

// vm mod
// Set the directory used to resolve reference modules such as std.ns and
// shader.ns. The VM borrows ref_path; its storage must remain valid while an
// evaluation can import modules. Pass ns_str_null to restore the default path.
void ns_vm_set_ref_path(ns_vm *vm, ns_str ref_path);
// Set the directory used to load FFI dynamic libraries. Like ref_path, the VM
// borrows this string for the lifetime of evaluations that may import modules.
void ns_vm_set_lib_path(ns_vm *vm, ns_str lib_path);
// Optional second FFI directory tried only when loading from lib_path fails.
void ns_vm_set_lib_fallback_path(ns_vm *vm, ns_str lib_path);
ns_lib* ns_lib_import(ns_vm *vm, ns_str lib);
ns_lib* ns_lib_find(ns_vm *vm, ns_str lib);

// vm repl
void ns_repl(ns_vm* vm);
