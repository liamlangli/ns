#pragma once

#include "ns_type.h"
#include "ns_vm.h"
#include "ns_json.h"
#include "ns_net.h"
#include "ns_os.h"

typedef enum ns_debug_mode {
    NS_DEBUG_REPL,
    NS_DEBUG_STDIO,
    NS_DEBUG_SOCKET
} ns_debug_mode;

typedef struct ns_debug_options {
    ns_debug_mode mode;
    u16 port; // for socket mode
    ns_str filename; // for repl mode
} ns_debug_options;

typedef struct ns_debug_breakpoint {
    ns_str f;
    i32 l;
} ns_debug_breakpoint;

typedef enum ns_debug_state {
    NS_DEBUG_STATE_INIT,
    NS_DEBUG_STATE_READY,
    NS_DEBUG_STATE_RUNNING,
    NS_DEBUG_STATE_PAUSED,
    NS_DEBUG_STATE_TERMINATED
} ns_debug_state;

typedef struct ns_debug_session {
    // common
    ns_debug_options options;
    ns_str source;
    ns_vm *vm;
    ns_debug_state state;
    ns_debug_breakpoint *breakpoints;

    // socket mode
    ns_conn *conn;
} ns_debug_session;

void ns_debug_handle(ns_debug_session *session, ns_json_ref req);
void ns_debug_session_response(ns_debug_session *session, ns_json_ref res);

i32 ns_debug_repl(ns_debug_options options);
i32 ns_debug_stdio(ns_debug_options options);
i32 ns_debug_socket(ns_debug_options options);