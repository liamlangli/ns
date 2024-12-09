#pragma once

#include "ns_type.h"
#include "ns_vm.h"
#include "ns_json.h"
#include "ns_net.h"

typedef enum ns_debug_mode {
    NS_DEBUG_STDIO,
    NS_DEBUG_SOCKET,
    NS_DEBUG_REPL,
} ns_debug_mode;

typedef struct ns_debug_options {
    ns_debug_mode mode;
    u16 port;
} ns_debug_options;

typedef struct ns_debug_breakpoint {
    ns_str f;
    i32 l;
} ns_debug_breakpoint;

typedef struct ns_debug_session {
    ns_debug_options options;
    ns_conn *conn;
    ns_vm *vm;
    ns_bool terminated;
    ns_debug_breakpoint *breakpoints;
} ns_debug_session;

void ns_debug_handle(ns_debug_session *session, ns_json_ref req);
void ns_debug_session_response(ns_debug_session *session, ns_json_ref res);