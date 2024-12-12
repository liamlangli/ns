#include "ns_debug.h"

static i32 _last_seq = 0;

typedef enum ns_debug_request_type {
    NS_DEBUG_UNKNOWN,
    NS_DEBUG_INIT,
    NS_DEBUG_SET_BREAKPOINTS,
    NS_DEBUG_SET_EXCEPTION_BREAKPOINTS,
    NS_DEBUG_THREADS,
    NS_DEBUG_LAUNCH,
    NS_DEBUG_ATTACH,
    NS_DEBUG_DISCONNECT
} ns_debug_request_type;

void ns_debug_handle_init(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_set_breakpoints(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_set_exception_breakpoints(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_threads(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_launch(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_attach(ns_debug_session *sess, ns_json_ref json);
void ns_debug_handle_disconnect(ns_debug_session *sess, ns_json_ref json);
ns_return_void ns_debug_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i);

ns_debug_request_type ns_debug_parse_type(ns_str type) {
    if (ns_str_equals_STR(type, "initialize")) {
        return NS_DEBUG_INIT;
    } else if (ns_str_equals_STR(type, "setBreakpoints")) {
        return NS_DEBUG_SET_BREAKPOINTS;
    } else if (ns_str_equals_STR(type, "setExceptionBreakpoints")) {
        return NS_DEBUG_SET_EXCEPTION_BREAKPOINTS;
    } else if (ns_str_equals_STR(type, "threads")) {
        return NS_DEBUG_THREADS;
    } else if (ns_str_equals_STR(type, "launch")) {
        return NS_DEBUG_LAUNCH;
    } else if (ns_str_equals_STR(type, "attach")) {
        return NS_DEBUG_ATTACH;
    } else if (ns_str_equals_STR(type, "disconnect")) {
        return NS_DEBUG_DISCONNECT;
    }
    return NS_DEBUG_UNKNOWN;
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

void ns_debug_handle(ns_debug_session *sess, ns_json_ref req) {
    ns_str cmd = ns_json_to_string(ns_json_get_prop(req, ns_str_cstr("command")));
    ns_debug_request_type type = ns_debug_parse_type(cmd);
    switch (type)
    {
        case NS_DEBUG_INIT: return ns_debug_handle_init(sess, req);
        case NS_DEBUG_SET_BREAKPOINTS: return ns_debug_handle_set_breakpoints(sess, req);
        case NS_DEBUG_SET_EXCEPTION_BREAKPOINTS: return ns_debug_handle_set_breakpoints(sess, req);
        case NS_DEBUG_THREADS: return ns_debug_handle_threads(sess, req);
        case NS_DEBUG_LAUNCH: return ns_debug_handle_launch(sess, req);
        case NS_DEBUG_ATTACH: return ns_debug_handle_attach(sess, req);
        case NS_DEBUG_DISCONNECT: return ns_debug_handle_disconnect(sess, req);
        default:
            ns_warn("ns_debug", "invalid request type: %.*s\n", cmd.len, cmd.data);
            break;
    }
}

void ns_debug_handle_init(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("initialize"), true);

    ns_vm *vm = (ns_vm*)malloc(sizeof(ns_vm));
    vm->step_hook = ns_debug_step_hook;

    ns_debug_session_response(sess, res);
    ns_debug_session_response(sess, ns_debug_send_event(ns_str_cstr("initialized"), ns_json_make_null()));
}

void ns_debug_handle_set_breakpoints(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("setBreakpoints"), true);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_set_exception_breakpoints(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("setExceptionBreakpoints"), true);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_threads(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("threads"), true);
    ns_json_ref body = ns_json_make_array();
    ns_json_push(body, ns_json_make_object());
    ns_json_set(res, ns_str_cstr("body"), body);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_launch(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("launch"), true);
    

    ns_debug_session_response(sess, res);
}

void ns_debug_handle_attach(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("attach"), true);
    ns_debug_session_response(sess, res);
}

void ns_debug_handle_disconnect(ns_debug_session *sess, ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(seq, ns_str_cstr("disconnect"), true);

    ns_vm *vm = sess->vm;
    free(vm);

    ns_debug_session_response(sess, res);
    sess->state = NS_DEBUG_STATE_TERMINATED;
}

ns_return_void ns_debug_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_ast_state state = ctx->nodes[i].state;
    ns_str f = ctx->filename;
    ns_info("ns_debug", "step hook at %.*s:%d:%d\n", f.len, f.data, state.l, state.o);
    return ns_return_ok_void;
}