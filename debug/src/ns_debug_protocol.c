#include "ns_debug.h"
#include <pthread.h>

static i32 _last_seq = 0;

typedef enum ns_debug_request_type {
    NS_DEBUG_REQ_UNKNOWN,
    NS_DEBUG_REQ_INIT,
    NS_DEBUG_REQ_SET_BREAKPOINTS,
    NS_DEBUG_REQ_SET_EXCEPTION_BREAKPOINTS,
    NS_DEBUG_REQ_THREADS,
    NS_DEBUG_REQ_STACK_TRACE,
    NS_DEBUG_REQ_SCOPES,
    NS_DEBUG_REQ_VARIABLES,
    NS_DEBUG_REQ_EVALUATE,
    NS_DEBUG_REQ_CONFIGURATION_DONE,
    NS_DEBUG_REQ_CONTINUE,
    NS_DEBUG_REQ_NEXT,
    NS_DEBUG_REQ_STEP_IN,
    NS_DEBUG_REQ_STEP_OUT,
    NS_DEBUG_REQ_LAUNCH,
    NS_DEBUG_REQ_ATTACH,
    NS_DEBUG_REQ_DISCONNECT
} ns_debug_request_type;

void ns_debug_handle_init(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_set_breakpoints(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_set_exception_breakpoints(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_threads(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_stack_trace(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_scopes(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_variables(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_evaluate(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_configuration_done(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_continue(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_next(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_step_in(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_step_out(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_launch(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_attach(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_disconnect(ns_debug_session *sess, ns_json_ref json);
ns_return_void ns_debug_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

static ns_debug_session *_active_sess = ns_null;
static pthread_t _dap_eval_thread;
static ns_bool _dap_eval_running = false;
static pthread_mutex_t _dap_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _dap_cv = PTHREAD_COND_INITIALIZER;

#define NS_DEBUG_LOCALS_REF 1

static ns_str ns_debug_str_copy(ns_str s) {
    if (s.len <= 0 || !s.data) return ns_str_null;
    return ns_str_slice(s, 0, s.len);
}

static void ns_debug_free_breakpoints(ns_debug_session *sess) {
    i32 n = ns_array_length(sess->breakpoints);
    for (i32 i = 0; i < n; ++i) {
        ns_str_free(sess->breakpoints[i].f);
    }
    ns_array_set_length(sess->breakpoints, 0);
}

static void ns_debug_set_path(ns_str *dst, ns_str src) {
    ns_str_free(*dst);
    *dst = ns_debug_str_copy(src);
}

static ns_str ns_debug_basename(ns_str p) {
    if (p.len <= 0 || !p.data) return p;
    for (i32 i = p.len - 1; i >= 0; --i) {
        if (p.data[i] == '/' || p.data[i] == '\\') {
            return (ns_str){.data = p.data + i + 1, .len = p.len - i - 1, .dynamic = 0};
        }
    }
    return p;
}

static ns_bool ns_debug_path_match(ns_str a, ns_str b) {
    if (ns_str_equals(a, b)) return true;
    if (ns_str_equals(ns_debug_basename(a), ns_debug_basename(b))) return true;

    if (a.len > 0 && b.len > a.len) {
        i32 start = b.len - a.len;
        if (strncmp(b.data + start, a.data, a.len) == 0) {
            i8 prev = start > 0 ? b.data[start - 1] : '/';
            if (prev == '/' || prev == '\\') return true;
        }
    }
    if (b.len > 0 && a.len > b.len) {
        i32 start = a.len - b.len;
        if (strncmp(a.data + start, b.data, b.len) == 0) {
            i8 prev = start > 0 ? a.data[start - 1] : '/';
            if (prev == '/' || prev == '\\') return true;
        }
    }
    return false;
}

static ns_bool ns_debug_json_to_bool(ns_json_ref r, ns_bool dft) {
    if (!r) return dft;
    ns_json *j = ns_json_get(r);
    if (!j) return dft;
    if (j->type == NS_JSON_TRUE) return true;
    if (j->type == NS_JSON_FALSE) return false;
    return dft;
}

ns_debug_request_type ns_debug_parse_type(ns_str type) {
    if (ns_str_equals_STR(type, "initialize")) {
        return NS_DEBUG_REQ_INIT;
    } else if (ns_str_equals_STR(type, "setBreakpoints")) {
        return NS_DEBUG_REQ_SET_BREAKPOINTS;
    } else if (ns_str_equals_STR(type, "setExceptionBreakpoints")) {
        return NS_DEBUG_REQ_SET_EXCEPTION_BREAKPOINTS;
    } else if (ns_str_equals_STR(type, "threads")) {
        return NS_DEBUG_REQ_THREADS;
    } else if (ns_str_equals_STR(type, "stackTrace")) {
        return NS_DEBUG_REQ_STACK_TRACE;
    } else if (ns_str_equals_STR(type, "scopes")) {
        return NS_DEBUG_REQ_SCOPES;
    } else if (ns_str_equals_STR(type, "variables")) {
        return NS_DEBUG_REQ_VARIABLES;
    } else if (ns_str_equals_STR(type, "evaluate")) {
        return NS_DEBUG_REQ_EVALUATE;
    } else if (ns_str_equals_STR(type, "configurationDone")) {
        return NS_DEBUG_REQ_CONFIGURATION_DONE;
    } else if (ns_str_equals_STR(type, "continue")) {
        return NS_DEBUG_REQ_CONTINUE;
    } else if (ns_str_equals_STR(type, "next")) {
        return NS_DEBUG_REQ_NEXT;
    } else if (ns_str_equals_STR(type, "stepIn")) {
        return NS_DEBUG_REQ_STEP_IN;
    } else if (ns_str_equals_STR(type, "stepOut")) {
        return NS_DEBUG_REQ_STEP_OUT;
    } else if (ns_str_equals_STR(type, "launch")) {
        return NS_DEBUG_REQ_LAUNCH;
    } else if (ns_str_equals_STR(type, "attach")) {
        return NS_DEBUG_REQ_ATTACH;
    } else if (ns_str_equals_STR(type, "disconnect")) {
        return NS_DEBUG_REQ_DISCONNECT;
    }
    return NS_DEBUG_REQ_UNKNOWN;
}

ns_json_ref ns_debug_response_ack(i32 seq, ns_str cmd, ns_bool suc) {
    ns_json_ref res = ns_json_make_object();
    ns_json_set(res, ns_str_cstr("type"), ns_json_make_string(ns_str_cstr("response")));
    ns_json_set(res, ns_str_cstr("seq"), ns_json_make_number(_last_seq++));
    ns_json_set(res, ns_str_cstr("request_seq"), ns_json_make_number(seq));
    ns_json_set(res, ns_str_cstr("command"), ns_json_make_string(cmd));
    ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(suc));
    return res;
}

ns_json_ref ns_debug_send_event(ns_str event, ns_json_ref body) {
    ns_json_ref res = ns_json_make_object();
    ns_json_set(res, ns_str_cstr("type"), ns_json_make_string(ns_str_cstr("event")));
    ns_json_set(res, ns_str_cstr("seq"), ns_json_make_number(_last_seq++));
    ns_json_set(res, ns_str_cstr("event"), ns_json_make_string(event));
    ns_json_set(res, ns_str_cstr("body"), body);
    return res;
}

static void ns_debug_send_output(ns_debug_session *sess, ns_str text, ns_str category) {
    ns_json_ref body = ns_json_make_object();
    ns_json_set(body, ns_str_cstr("category"), ns_json_make_string(category));
    ns_json_set(body, ns_str_cstr("output"), ns_json_make_string(text));
    ns_debug_session_response(sess, ns_debug_send_event(ns_str_cstr("output"), body));
}

void ns_debug_handle(ns_debug_session *sess, ns_json_ref req) {
    _active_sess = sess;
    ns_str cmd = ns_json_to_string(ns_json_get_prop(req, ns_str_cstr("command")));
    ns_debug_request_type type = ns_debug_parse_type(cmd);
    switch (type)
    {
        case NS_DEBUG_REQ_INIT: return ns_debug_handle_init(sess, req);
        case NS_DEBUG_REQ_SET_BREAKPOINTS: return ns_debug_handle_set_breakpoints(sess, req);
        case NS_DEBUG_REQ_SET_EXCEPTION_BREAKPOINTS: return ns_debug_handle_set_breakpoints(sess, req);
        case NS_DEBUG_REQ_THREADS: return ns_debug_handle_threads(sess, req);
        case NS_DEBUG_REQ_STACK_TRACE: return ns_debug_handle_stack_trace(sess, req);
        case NS_DEBUG_REQ_SCOPES: return ns_debug_handle_scopes(sess, req);
        case NS_DEBUG_REQ_VARIABLES: return ns_debug_handle_variables(sess, req);
        case NS_DEBUG_REQ_EVALUATE: return ns_debug_handle_evaluate(sess, req);
        case NS_DEBUG_REQ_CONFIGURATION_DONE: return ns_debug_handle_configuration_done(sess, req);
        case NS_DEBUG_REQ_CONTINUE: return ns_debug_handle_continue(sess, req);
        case NS_DEBUG_REQ_NEXT: return ns_debug_handle_next(sess, req);
        case NS_DEBUG_REQ_STEP_IN: return ns_debug_handle_step_in(sess, req);
        case NS_DEBUG_REQ_STEP_OUT: return ns_debug_handle_step_out(sess, req);
        case NS_DEBUG_REQ_LAUNCH: return ns_debug_handle_launch(sess, req);
        case NS_DEBUG_REQ_ATTACH: return ns_debug_handle_attach(sess, req);
        case NS_DEBUG_REQ_DISCONNECT: return ns_debug_handle_disconnect(sess, req);
        default:
            ns_warn("ns_debug", "invalid request type: %.*s\n", cmd.len, cmd.data);
            break;
    }
}

void ns_debug_handle_init(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("initialize"), true);
    ns_json_ref body = ns_json_make_object();
    ns_json_set(body, ns_str_cstr("supportsConfigurationDoneRequest"), ns_json_make_bool(true));
    ns_json_set(body, ns_str_cstr("supportsEvaluateForHovers"), ns_json_make_bool(true));
    ns_json_set(body, ns_str_cstr("supportsSetVariable"), ns_json_make_bool(false));
    ns_json_set(body, ns_str_cstr("supportsStepInTargetsRequest"), ns_json_make_bool(false));
    ns_json_set(body, ns_str_cstr("supportsTerminateRequest"), ns_json_make_bool(true));
    ns_json_set(res, ns_str_cstr("body"), body);

    ns_vm *vm = (ns_vm*)ns_malloc(sizeof(ns_vm));
    memset(vm, 0, sizeof(ns_vm));
    vm->step_hook = ns_debug_step_hook;
    sess->vm = vm;
    sess->state = NS_DEBUG_STATE_INIT;
    sess->step_mode = NS_DEBUG_STEP_NONE;
    sess->stop_on_entry = false;
    sess->entry_pending = false;

    ns_debug_session_response(sess, res);
    ns_debug_session_response(sess, ns_debug_send_event(ns_str_cstr("initialized"), ns_json_make_null()));
}

void ns_debug_handle_set_breakpoints(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("setBreakpoints"), true);

    ns_debug_free_breakpoints(sess);

    ns_json_ref args = ns_json_get_prop(json, ns_str_cstr("arguments"));
    ns_json_ref source = ns_json_get_prop(args, ns_str_cstr("source"));
    ns_str source_path = ns_json_to_string(ns_json_get_prop(source, ns_str_cstr("path")));
    if (source_path.len == 0) source_path = sess->options.filename;
    ns_str source_path_own = ns_debug_str_copy(source_path);

    ns_json_ref body = ns_json_make_object();
    ns_json_ref bp_list = ns_json_make_array();

    ns_json_ref bps = ns_json_get_prop(args, ns_str_cstr("breakpoints"));
    for (ns_json_ref it = bps ? ns_json_get(bps)->prop : 0; it != 0; it = ns_json_get(it)->next) {
        ns_json_ref line_ref = ns_json_get_prop(it, ns_str_cstr("line"));
        i32 line = ns_json_to_i32(line_ref);
        if (line <= 0) continue;

        ns_debug_breakpoint bp_rec = {.f = ns_debug_str_copy(source_path_own), .l = line};
        ns_array_push(sess->breakpoints, bp_rec);

        ns_json_ref bp = ns_json_make_object();
        ns_json_set(bp, ns_str_cstr("verified"), ns_json_make_bool(true));
        ns_json_set(bp, ns_str_cstr("line"), ns_json_make_number(line));
        ns_json_push(bp_list, bp);
    }

    ns_json_ref lines = ns_json_get_prop(args, ns_str_cstr("lines"));
    for (ns_json_ref it = lines ? ns_json_get(lines)->prop : 0; it != 0; it = ns_json_get(it)->next) {
        i32 line = ns_json_to_i32(it);
        if (line <= 0) continue;
        ns_debug_breakpoint bp_rec = {.f = ns_debug_str_copy(source_path_own), .l = line};
        ns_array_push(sess->breakpoints, bp_rec);

        ns_json_ref bp = ns_json_make_object();
        ns_json_set(bp, ns_str_cstr("verified"), ns_json_make_bool(true));
        ns_json_set(bp, ns_str_cstr("line"), ns_json_make_number(line));
        ns_json_push(bp_list, bp);
    }

    ns_json_set(body, ns_str_cstr("breakpoints"), bp_list);
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
    ns_str_free(source_path_own);
}

void ns_debug_handle_set_exception_breakpoints(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("setExceptionBreakpoints"), true);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_threads(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("threads"), true);
    ns_json_ref body = ns_json_make_object();
    ns_json_ref threads = ns_json_make_array();
    ns_json_ref t = ns_json_make_object();
    ns_json_set(t, ns_str_cstr("id"), ns_json_make_number(1));
    ns_json_set(t, ns_str_cstr("name"), ns_json_make_string(ns_str_cstr("main")));
    ns_json_push(threads, t);
    ns_json_set(body, ns_str_cstr("threads"), threads);
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
}

static ns_bool ns_debug_hit_breakpoint(ns_debug_session *sess, ns_str f, i32 l) {
    for (i32 i = 0, len = ns_array_length(sess->breakpoints); i < len; ++i) {
        ns_debug_breakpoint bp = sess->breakpoints[i];
        if (bp.l == l && ns_debug_path_match(bp.f, f)) return true;
    }
    return false;
}

static ns_bool ns_debug_should_pause(ns_debug_session *sess, ns_vm *vm, ns_ast_state state, ns_str f, ns_bool *bp_hit) {
    *bp_hit = false;
    i32 depth = ns_array_length(vm->call_stack);
    if (ns_debug_hit_breakpoint(sess, f, state.l)) {
        *bp_hit = true;
        return true;
    }
    switch (sess->step_mode) {
    case NS_DEBUG_STEP_INTO:
        return true;
    case NS_DEBUG_STEP_OVER:
        if (depth < sess->step_depth) return true;
        if (depth == sess->step_depth && state.l != sess->pause_line) return true;
        return false;
    case NS_DEBUG_STEP_OUT:
        if (depth < sess->step_depth) return true;
        return false;
    case NS_DEBUG_STEP_NONE:
    default:
        return false;
    }
}

static void ns_debug_resume(ns_debug_session *sess, ns_debug_step_mode mode) {
    pthread_mutex_lock(&_dap_mtx);
    sess->step_mode = mode;
    sess->step_depth = ns_array_length(sess->vm->call_stack);
    sess->state = NS_DEBUG_STATE_RUNNING;
    pthread_cond_signal(&_dap_cv);
    pthread_mutex_unlock(&_dap_mtx);
}

static void *ns_debug_eval_main(void *arg) {
    ns_debug_session *sess = (ns_debug_session *)arg;
    ns_str run_msg = ns_str_concat(ns_str_cstr("[ns_debug] running "), sess->options.filename);
    ns_str_append(&run_msg, ns_str_cstr("\n"));
    ns_debug_send_output(sess, run_msg, ns_str_cstr("console"));
    ns_str_free(run_msg);

    ns_return_value ret = ns_eval(sess->vm, sess->source, sess->options.filename);
    _dap_eval_running = false;

    if (ns_return_is_error(ret)) {
        ns_str err_msg = ns_str_concat(ns_str_cstr("[ns_debug] error: "), ret.e.msg);
        ns_str_append(&err_msg, ns_str_cstr("\n"));
        ns_debug_send_output(sess, err_msg, ns_str_cstr("stderr"));
        ns_str_free(err_msg);

        ns_json_ref body = ns_json_make_object();
        ns_json_set(body, ns_str_cstr("reason"), ns_json_make_string(ns_str_cstr("exception")));
        ns_json_set(body, ns_str_cstr("threadId"), ns_json_make_number(1));
        ns_json_set(body, ns_str_cstr("text"), ns_json_make_string(ret.e.msg));
        ns_debug_session_response(sess, ns_debug_send_event(ns_str_cstr("stopped"), body));
        sess->state = NS_DEBUG_STATE_PAUSED;
        return ns_null;
    }

    ns_json_ref body = ns_json_make_object();
    ns_json_set(body, ns_str_cstr("restart"), ns_json_make_bool(false));
    ns_debug_session_response(sess, ns_debug_send_event(ns_str_cstr("terminated"), body));
    ns_debug_send_output(sess, ns_str_cstr("[ns_debug] terminated\n"), ns_str_cstr("console"));
    sess->state = NS_DEBUG_STATE_TERMINATED;
    return ns_null;
}

static ns_json_ref ns_debug_make_source(ns_str path) {
    ns_json_ref source = ns_json_make_object();
    ns_json_set(source, ns_str_cstr("name"), ns_json_make_string(path));
    ns_json_set(source, ns_str_cstr("path"), ns_json_make_string(path));
    return source;
}

void ns_debug_handle_stack_trace(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("stackTrace"), true);

    ns_json_ref body = ns_json_make_object();
    ns_json_ref frames = ns_json_make_array();

    ns_json_ref frame = ns_json_make_object();
    ns_str file = sess->pause_file.len ? sess->pause_file : sess->options.filename;
    i32 line = sess->pause_line > 0 ? sess->pause_line : (sess->vm ? sess->vm->loc.l : 1);
    i32 col = sess->vm ? ns_max(sess->vm->loc.o, 1) : 1;

    ns_json_set(frame, ns_str_cstr("id"), ns_json_make_number(1));
    ns_json_set(frame, ns_str_cstr("name"), ns_json_make_string(ns_str_cstr("main")));
    ns_json_set(frame, ns_str_cstr("line"), ns_json_make_number(line));
    ns_json_set(frame, ns_str_cstr("column"), ns_json_make_number(col));
    ns_json_set(frame, ns_str_cstr("source"), ns_debug_make_source(file));
    ns_json_push(frames, frame);

    ns_json_set(body, ns_str_cstr("stackFrames"), frames);
    ns_json_set(body, ns_str_cstr("totalFrames"), ns_json_make_number(1));
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_scopes(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("scopes"), true);

    ns_json_ref body = ns_json_make_object();
    ns_json_ref scopes = ns_json_make_array();

    ns_json_ref locals = ns_json_make_object();
    ns_json_set(locals, ns_str_cstr("name"), ns_json_make_string(ns_str_cstr("Locals")));
    ns_json_set(locals, ns_str_cstr("variablesReference"), ns_json_make_number(NS_DEBUG_LOCALS_REF));
    ns_json_set(locals, ns_str_cstr("expensive"), ns_json_make_bool(false));
    ns_json_push(scopes, locals);

    ns_json_set(body, ns_str_cstr("scopes"), scopes);
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
}

static ns_json_ref ns_debug_make_dap_var(ns_vm *vm, ns_symbol *s) {
    ns_json_ref v = ns_json_make_object();
    ns_json_set(v, ns_str_cstr("name"), ns_json_make_string(s->name));
    ns_json_set(v, ns_str_cstr("variablesReference"), ns_json_make_number(0));

    if (s->type == NS_SYMBOL_VALUE) {
        ns_str type = ns_vm_get_type_name(vm, s->val.t);
        ns_json_set(v, ns_str_cstr("type"), ns_json_make_string(type));

        ns_str val = ns_fmt_value(vm, s->val);
        ns_json_set(v, ns_str_cstr("value"), ns_json_make_string(val));
        ns_str_free(val);
    } else {
        ns_json_set(v, ns_str_cstr("type"), ns_json_make_string(ns_str_cstr("symbol")));
        ns_json_set(v, ns_str_cstr("value"), ns_json_make_string(ns_str_cstr("<non-value>")));
    }
    return v;
}

void ns_debug_handle_variables(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("variables"), true);

    ns_json_ref body = ns_json_make_object();
    ns_json_ref vars = ns_json_make_array();
    ns_json_set(body, ns_str_cstr("variables"), vars);

    ns_json_ref args = ns_json_get_prop(json, ns_str_cstr("arguments"));
    i32 ref = ns_json_to_i32(ns_json_get_prop(args, ns_str_cstr("variablesReference")));
    ns_vm *vm = sess->vm;
    if (!vm || ref != NS_DEBUG_LOCALS_REF) {
        ns_json_set(res, ns_str_cstr("body"), body);
        ns_debug_session_response(sess, res);
        return;
    }

    i32 from = 0;
    i32 to = ns_array_length(vm->symbol_stack);
    ns_scope *scope = ns_array_last_safe(vm->scope_stack);
    if (scope) from = scope->symbol_top;

    for (i32 i = from; i < to; ++i) {
        ns_symbol *s = &vm->symbol_stack[i];
        if (s->type != NS_SYMBOL_VALUE) continue;
        ns_json_push(vars, ns_debug_make_dap_var(vm, s));
    }

    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_evaluate(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("evaluate"), true);
    ns_json_ref body = ns_json_make_object();

    ns_json_ref args = ns_json_get_prop(json, ns_str_cstr("arguments"));
    ns_str expr = ns_json_to_string(ns_json_get_prop(args, ns_str_cstr("expression")));
    ns_vm *vm = sess->vm;
    if (!vm || expr.len == 0) {
        ns_json_set(body, ns_str_cstr("result"), ns_json_make_string(ns_str_cstr("")));
        ns_json_set(body, ns_str_cstr("variablesReference"), ns_json_make_number(0));
        ns_json_set(res, ns_str_cstr("body"), body);
        ns_debug_session_response(sess, res);
        return;
    }

    setenv("NS_REPL_RECOVER", "1", 1);

    ns_ast_ctx eval_ctx = {0};
    eval_ctx.source = expr;
    eval_ctx.filename = ns_str_cstr("<debug-eval>");
    eval_ctx.token.line = 1;
    eval_ctx.current = 0;

    ns_return_bool ret_p = ns_parse_expr(&eval_ctx);
    if (ns_return_is_error(ret_p) || !ret_p.r) {
        unsetenv("NS_REPL_RECOVER");
        ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(false));
        ns_str msg = ns_return_is_error(ret_p) ? ret_p.e.msg : ns_str_cstr("invalid expression");
        ns_json_set(res, ns_str_cstr("message"), ns_json_make_string(msg));
        ns_debug_session_response(sess, res);
        return;
    }

    ns_return_value ret_v = ns_eval_expr(vm, &eval_ctx, eval_ctx.current);
    if (ns_return_is_error(ret_v)) {
        unsetenv("NS_REPL_RECOVER");
        ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(false));
        ns_json_set(res, ns_str_cstr("message"), ns_json_make_string(ret_v.e.msg));
        ns_debug_session_response(sess, res);
        return;
    }
    ns_value out = ret_v.r;

    ns_str val = ns_fmt_value(vm, out);
    ns_json_set(body, ns_str_cstr("result"), ns_json_make_string(val));
    ns_json_set(body, ns_str_cstr("variablesReference"), ns_json_make_number(0));
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
    ns_str_free(val);
    unsetenv("NS_REPL_RECOVER");
}

void ns_debug_handle_launch(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("launch"), true);

    if (!sess->vm) {
        sess->vm = (ns_vm*)ns_malloc(sizeof(ns_vm));
        memset(sess->vm, 0, sizeof(ns_vm));
        sess->vm->step_hook = ns_debug_step_hook;
    }

    ns_json_ref args = ns_json_get_prop(json, ns_str_cstr("arguments"));
    ns_str program = ns_json_to_string(ns_json_get_prop(args, ns_str_cstr("program")));
    ns_bool stop_on_entry = ns_debug_json_to_bool(ns_json_get_prop(args, ns_str_cstr("stopOnEntry")), false);
    if (program.len == 0) program = sess->options.filename;
    if (program.len == 0 || !program.data) {
        ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(false));
        ns_json_set(res, ns_str_cstr("message"), ns_json_make_string(ns_str_cstr("missing launch.arguments.program")));
        ns_debug_session_response(sess, res);
        return;
    }

    ns_str program_own = ns_debug_str_copy(program);
    ns_str source = ns_fs_read_file(program_own);
    if (source.len == 0 || !source.data) {
        ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(false));
        ns_json_set(res, ns_str_cstr("message"), ns_json_make_string(ns_str_cstr("failed to read program source")));
        ns_debug_session_response(sess, res);
        ns_str_free(program_own);
        return;
    }

    ns_str_free(sess->options.filename);
    ns_str_free(sess->source);
    ns_str_free(sess->pause_file);
    sess->options.filename = program_own;
    sess->source = source;
    sess->state = NS_DEBUG_STATE_READY;
    sess->step_mode = NS_DEBUG_STEP_NONE;
    sess->stop_on_entry = stop_on_entry;
    sess->entry_pending = stop_on_entry;
    sess->pause_line = -1;
    sess->pause_file = ns_debug_str_copy(program_own);

    ns_debug_session_response(sess, res);
}

void ns_debug_handle_configuration_done(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("configurationDone"), true);
    ns_debug_session_response(sess, res);

    if (sess->state == NS_DEBUG_STATE_READY && !_dap_eval_running && sess->source.len > 0 && sess->source.data) {
        _dap_eval_running = true;
        sess->state = NS_DEBUG_STATE_RUNNING;
        sess->step_mode = sess->stop_on_entry ? NS_DEBUG_STEP_INTO : NS_DEBUG_STEP_NONE;
        pthread_create(&_dap_eval_thread, ns_null, ns_debug_eval_main, sess);
    }
}

void ns_debug_handle_continue(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("continue"), true);
    if (sess->state == NS_DEBUG_STATE_READY && !_dap_eval_running) {
        if (sess->source.len == 0 || !sess->source.data) {
            ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(false));
            ns_json_set(res, ns_str_cstr("message"), ns_json_make_string(ns_str_cstr("no program loaded")));
            ns_debug_session_response(sess, res);
            return;
        }
    }

    ns_json_ref body = ns_json_make_object();
    ns_json_set(body, ns_str_cstr("allThreadsContinued"), ns_json_make_bool(true));
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);

    if (sess->state == NS_DEBUG_STATE_READY && !_dap_eval_running) {
        _dap_eval_running = true;
        sess->state = NS_DEBUG_STATE_RUNNING;
        pthread_create(&_dap_eval_thread, ns_null, ns_debug_eval_main, sess);
    } else if (sess->state == NS_DEBUG_STATE_PAUSED) {
        ns_debug_resume(sess, NS_DEBUG_STEP_NONE);
    }
}

void ns_debug_handle_next(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("next"), true);
    ns_debug_session_response(sess, res);
    if (sess->state == NS_DEBUG_STATE_PAUSED) ns_debug_resume(sess, NS_DEBUG_STEP_OVER);
}

void ns_debug_handle_step_in(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("stepIn"), true);
    ns_debug_session_response(sess, res);
    if (sess->state == NS_DEBUG_STATE_PAUSED) ns_debug_resume(sess, NS_DEBUG_STEP_INTO);
}

void ns_debug_handle_step_out(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("stepOut"), true);
    ns_debug_session_response(sess, res);
    if (sess->state == NS_DEBUG_STATE_PAUSED) ns_debug_resume(sess, NS_DEBUG_STEP_OUT);
}

void ns_debug_handle_attach(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("attach"), true);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_disconnect(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("disconnect"), true);

    if (_dap_eval_running) {
        pthread_mutex_lock(&_dap_mtx);
        sess->state = NS_DEBUG_STATE_RUNNING;
        pthread_cond_signal(&_dap_cv);
        pthread_mutex_unlock(&_dap_mtx);
    }

    ns_vm *vm = sess->vm;
    if (vm) {
        ns_free(vm);
        sess->vm = ns_null;
    }
    ns_debug_free_breakpoints(sess);
    ns_str_free(sess->options.filename);
    sess->options.filename = ns_str_null;
    ns_str_free(sess->source);
    sess->source = ns_str_null;
    ns_str_free(sess->pause_file);
    sess->pause_file = ns_str_null;

    ns_debug_session_response(sess, res);
    sess->state = NS_DEBUG_STATE_TERMINATED;
}

ns_return_void ns_debug_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    if (!_active_sess || _active_sess->options.mode != NS_DEBUG_STDIO) return ns_return_ok_void;
    ns_ast_state state = ctx->nodes[i].state;
    ns_str f = ctx->filename;

    ns_bool bp_hit = false;
    if (!ns_debug_should_pause(_active_sess, vm, state, f, &bp_hit)) return ns_return_ok_void;

    pthread_mutex_lock(&_dap_mtx);
    _active_sess->pause_line = state.l;
    ns_debug_set_path(&_active_sess->pause_file, f);
    _active_sess->state = NS_DEBUG_STATE_PAUSED;
    _active_sess->step_mode = NS_DEBUG_STEP_NONE;

    ns_json_ref body = ns_json_make_object();
    ns_str reason = bp_hit ? ns_str_cstr("breakpoint") : (_active_sess->entry_pending ? ns_str_cstr("entry") : ns_str_cstr("step"));
    _active_sess->entry_pending = false;
    ns_json_set(body, ns_str_cstr("reason"), ns_json_make_string(reason));
    ns_json_set(body, ns_str_cstr("threadId"), ns_json_make_number(1));
    ns_json_set(body, ns_str_cstr("allThreadsStopped"), ns_json_make_bool(true));
    ns_debug_session_response(_active_sess, ns_debug_send_event(ns_str_cstr("stopped"), body));

    while (_active_sess->state == NS_DEBUG_STATE_PAUSED) {
        pthread_cond_wait(&_dap_cv, &_dap_mtx);
    }
    pthread_mutex_unlock(&_dap_mtx);
    return ns_return_ok_void;
}
