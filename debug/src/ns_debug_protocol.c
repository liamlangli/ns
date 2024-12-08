#include "ns_debug.h"

typedef enum ns_debug_request_type {
    NS_DEBUG_UNKNOWN,
    NS_DEBUG_INIT,
    NS_DEBUG_LAUNCH,
    NS_DEBUG_ATTACH,
    NS_DEBUG_DISCONNECT
} ns_debug_request_type;

ns_debug_request_type ns_debug_parse_type(ns_str type) {
    if (ns_str_equals_STR(type, "initialize")) {
        return NS_DEBUG_INIT;
    } else if (ns_str_equals_STR(type, "launch")) {
        return NS_DEBUG_LAUNCH;
    } else if (ns_str_equals_STR(type, "attach")) {
        return NS_DEBUG_ATTACH;
    } else if (ns_str_equals_STR(type, "disconnect")) {
        return NS_DEBUG_DISCONNECT;
    }
    return NS_DEBUG_UNKNOWN;
}

static i32 _last_seq = 0;

ns_json_ref ns_debug_response_ack(ns_str type, i32 seq, ns_str cmd, ns_bool suc) {
    ns_json_ref res = ns_json_make_object();
    ns_json_set(res, ns_str_cstr("type"), ns_json_make_string(type));
    ns_json_set(res, ns_str_cstr("seq"), ns_json_make_number(_last_seq++));
    ns_json_set(res, ns_str_cstr("request_seq"), ns_json_make_number(seq));
    ns_json_set(res, ns_str_cstr("command"), ns_json_make_string(cmd));
    ns_json_set(res, ns_str_cstr("success"), ns_json_make_bool(suc));
    return res;
}

ns_return_json ns_debug_handle_init(ns_json_ref json);
ns_return_json ns_debug_handle_launch(ns_json_ref json);
ns_return_json ns_debug_handle_attach(ns_json_ref json);
ns_return_json ns_debug_handle_disconnect(ns_json_ref json);

ns_return_json ns_debug_handle(ns_json_ref req) {
    ns_str cmd = ns_json_to_string(ns_json_get_prop(req, ns_str_cstr("command")));
    ns_debug_request_type type = ns_debug_parse_type(cmd);
    switch (type)
    {
        case NS_DEBUG_INIT: return ns_debug_handle_init(req);
        case NS_DEBUG_LAUNCH: return ns_debug_handle_launch(req);
        case NS_DEBUG_ATTACH: return ns_debug_handle_attach(req);
        case NS_DEBUG_DISCONNECT: return ns_debug_handle_disconnect(req);
        default: break;
    }
    return ns_return_error(json, ns_code_loc_nil, NS_ERR_RUNTIME, "invalid request type");
}

ns_return_json ns_debug_handle_init(ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(ns_str_cstr("response"), seq, ns_str_cstr("initialize"), true);
    return ns_return_ok(json, res);
}

ns_return_json ns_debug_handle_launch(ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(ns_str_cstr("response"), seq, ns_str_cstr("launch"), true);
    return ns_return_ok(json, res);
}

ns_return_json ns_debug_handle_attach(ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(ns_str_cstr("response"), seq, ns_str_cstr("attach"), true);
    return ns_return_ok(json, res);
}

ns_return_json ns_debug_handle_disconnect(ns_json_ref json) {
    i32 seq = ns_json_to_i32(ns_json_get_prop(json, ns_str_cstr("seq")));
    ns_json_ref res = ns_debug_response_ack(ns_str_cstr("response"), seq, ns_str_cstr("disconnect"), true);
    return ns_return_ok(json, res);
}