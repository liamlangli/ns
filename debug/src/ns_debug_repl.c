#include "ns_debug.h"

#include <readline/readline.h>
#include <readline/history.h>

/**
 *  nsdb - nanoscript debugger
 * - repl mode commands
 *     - run/r
 *     - step-into/si               : step into
 *     - step-over/so               : step over
 *     - step-out/so                : step out
 *     - break/b [line]             : set breakpoint
 *     - break-list/bl              : list breakpoints
 *     - break-delete/bd [line]     : remove breakpoint
 *     - break-error/be             : set error breakpoint
 *     - break-clear/bc             : clear all breakpoints
 *     - print/p [expr]             : print expression
 *     - quit/q                     : quit
 **/

typedef enum ns_debug_repl_command_type {
    NS_DEBUG_REPL_NONE,
    NS_DEBUG_REPL_RUN,
    NS_DEBUG_REPL_STEP_INTO,
    NS_DEBUG_REPL_STEP_OVER,
    NS_DEBUG_REPL_STEP_OUT,
    NS_DEBUG_REPL_BREAK,
    NS_DEBUG_REPL_BREAK_LIST,
    NS_DEBUG_REPL_BREAK_DELETE,
    NS_DEBUG_REPL_BREAK_ERROR,
    NS_DEBUG_REPL_BREAK_CLEAR,
    NS_DEBUG_REPL_PRINT,
    NS_DEBUG_REPL_QUIT,
    NS_DEBUG_REPL_UNKNOWN
} ns_debug_repl_command_type;

typedef struct ns_debug_repl_command {
    ns_debug_repl_command_type type;
    union {
        i32 line;
        ns_str expr;
    };
} ns_debug_repl_command;

ns_str ns_debug_repl_read_line(char *prompt);
ns_debug_repl_command ns_debug_repl_parse_command(ns_str line);
ns_return_void ns_debug_repl_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_void ns_debug_repl_loop();

void ns_debug_repl_list_breakpoints(ns_debug_session *sess);
void ns_debug_repl_set_breakpoint(ns_debug_session *sess, ns_str f, i32 l);
void ns_debug_repl_del_breakpoint(ns_debug_session *sess, ns_str f, i32 l);

static ns_vm _debug_repl_vm = {0};
static ns_debug_session _debug_repl_sess = {0};

ns_str ns_debug_repl_read_line(char *prompt) {
    char *line = readline(prompt);
    if (!line) return ns_str_null;
    add_history(line);
    ns_str s = ns_str_cstr(line);
    return s;
}

const ns_str run_cmd = ns_str_cstr("run");
const ns_str r_cmd = ns_str_cstr("r");
const ns_str si_cmd = ns_str_cstr("step-into");
const ns_str si_short_cmd = ns_str_cstr("si");
const ns_str so_cmd = ns_str_cstr("step-over");
const ns_str so_short_cmd = ns_str_cstr("so");
const ns_str b_cmd = ns_str_cstr("break");
const ns_str b_short_cmd = ns_str_cstr("b");
const ns_str bl_cmd = ns_str_cstr("break-list");
const ns_str bl_short_cmd = ns_str_cstr("bl");
const ns_str bd_cmd = ns_str_cstr("break-delete");
const ns_str bd_short_cmd = ns_str_cstr("bd");
const ns_str be_cmd = ns_str_cstr("break-error");
const ns_str be_short_cmd = ns_str_cstr("be");
const ns_str bc_cmd = ns_str_cstr("break-clear");
const ns_str bc_short_cmd = ns_str_cstr("bc");
const ns_str p_cmd = ns_str_cstr("print");
const ns_str p_short_cmd = ns_str_cstr("p");
const ns_str quit_cmd = ns_str_cstr("quit");
const ns_str q_cmd = ns_str_cstr("q");

ns_debug_repl_command ns_debug_repl_parse_command(ns_str line) {
    ns_debug_repl_command cmd = {0};
    if (ns_str_equals(line, run_cmd) || ns_str_equals(line, r_cmd)) {
        cmd.type = NS_DEBUG_REPL_RUN;
    } else if (ns_str_equals(line, si_cmd) || ns_str_equals(line, si_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_STEP_INTO;
    } else if (ns_str_equals(line, so_cmd) || ns_str_equals(line, so_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_STEP_OVER;
    } else if (ns_str_equals(line, so_cmd) || ns_str_equals(line, so_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_STEP_OUT;
    } else if (ns_str_equals(line, b_cmd) || ns_str_equals(line, b_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK;
        cmd.line = atoi(line.data + 6);
    } else if (ns_str_equals(line, bl_cmd) || ns_str_equals(line, bl_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_LIST;
    } else if (ns_str_equals(line, bd_cmd) || ns_str_equals(line, bd_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_DELETE;
        cmd.line = atoi(line.data + 10);
    } else if (ns_str_equals(line, be_cmd) || ns_str_equals(line, be_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_ERROR;
    } else if (ns_str_equals(line, bc_cmd) || ns_str_equals(line, bc_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_CLEAR;
    } else if (ns_str_equals(line, p_cmd) || ns_str_equals(line, p_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_PRINT;
    } else if (ns_str_equals(line, quit_cmd) || ns_str_equals(line, q_cmd)) {
        cmd.type = NS_DEBUG_REPL_QUIT;
    }
    return cmd;
}

ns_return_void ns_debug_repl_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_debug_session *sess = vm->debug_session;
    sess->state = NS_DEBUG_STATE_PAUSED;
    ns_ast_t *n = &ctx->nodes[i];
    ns_str f = ctx->filename;
    ns_info("ns_debug", "step hook at %.*s:%d:%d\n", f.len, f.data, n->state.l, n->state.o);
    ns_return_void ret = ns_debug_repl_loop();
    if (ns_return_is_error(ret)) {
        ns_error("ns_debug", "repl error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
    }
    return ret;
}

void ns_debug_repl_list_breakpoints(ns_debug_session *sess) {
    ns_info("ns_debug", "breakpoints:\n");
    for (i32 i = 0, l = ns_array_length(sess->breakpoints); i < l; i++) {
        ns_debug_breakpoint bp = sess->breakpoints[i];
        ns_info("ns_debug", "  %.*s:%d\n", bp.f.len, bp.f.data, bp.l);
    }
}

void ns_debug_repl_set_breakpoint(ns_debug_session *sess, ns_str f, i32 l) {
    // insert sort add
    ns_debug_breakpoint bp = {f, l};
    i32 len = ns_array_length(sess->breakpoints);
    if (len == 0) {
        ns_array_push(sess->breakpoints, bp);
        return;
    }
    for (i32 i = 0; i < len; i++) {
        ns_debug_breakpoint b = sess->breakpoints[i];
        if (b.l > l) {
            ns_array_insert(sess->breakpoints, i, bp);
            return;
        }
    }
}

void ns_debug_repl_del_breakpoint(ns_debug_session *sess, ns_str f, i32 l) {
    for (i32 i = 0, len = ns_array_length(sess->breakpoints); i < len; i++) {
        ns_debug_breakpoint bp = sess->breakpoints[i];
        if (ns_str_equals(bp.f, f) && bp.l == l) {
            ns_array_splice(sess->breakpoints, i);
            return;
        }
    }
}

ns_return_void ns_debug_repl_loop() {
    ns_debug_session *sess = &_debug_repl_sess;
    ns_vm *vm = &_debug_repl_vm;

    while (1) {
        ns_str line = ns_debug_repl_read_line(ns_color_log "nsdb" ns_color_nil "> ");
        if (line.len == 0) continue;
        ns_debug_repl_command cmd = ns_debug_repl_parse_command(line);
        switch (cmd.type)
        {
        case NS_DEBUG_REPL_RUN:
            ns_info("ns_debug", "run\n");
            switch (sess->state)
            {
            case NS_DEBUG_STATE_READY:
                sess->state = NS_DEBUG_STATE_RUNNING;
                ns_return_value ret = ns_eval(vm, sess->source, sess->options.filename);
                if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
                break;
            case NS_DEBUG_STATE_PAUSED:
                sess->state = NS_DEBUG_STATE_RUNNING;
                return ns_return_ok_void;
            case NS_DEBUG_STATE_TERMINATED:
                return ns_return_ok_void;
            default:
                return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "invalid state");
                break;
            }
            break;
        case NS_DEBUG_REPL_STEP_INTO:
            ns_info("ns_debug", "step into\n");
            break;
        case NS_DEBUG_REPL_STEP_OVER:
            ns_info("ns_debug", "step over\n");
            break;
        case NS_DEBUG_REPL_STEP_OUT:
            ns_info("ns_debug", "step out\n");
            break;
        case NS_DEBUG_REPL_BREAK:
            ns_debug_repl_set_breakpoint(sess, sess->options.filename, cmd.line);
            break;
        case NS_DEBUG_REPL_BREAK_LIST:
            ns_debug_repl_list_breakpoints(sess);
            break;
        case NS_DEBUG_REPL_BREAK_DELETE:
            ns_debug_repl_del_breakpoint(sess, sess->options.filename, cmd.line);
            break;
        case NS_DEBUG_REPL_BREAK_ERROR:
            ns_info("ns_debug", "break on error\n");
            break;
        case NS_DEBUG_REPL_BREAK_CLEAR:
            ns_array_set_length(sess->breakpoints, 0);
            break;
        case NS_DEBUG_REPL_QUIT:
            ns_info("ns_debug", "quit\n");
            return ns_return_ok_void;
        case NS_DEBUG_REPL_UNKNOWN:
            ns_warn("ns_debug", "unknown command: %.*s\n", line.len, line.data);
            break;
        default:
            break;
        }
    }

    return ns_return_ok_void;
}

i32 ns_debug_repl(ns_debug_options options) {
    ns_info("ns_debug", "repl mode\n");

    ns_debug_session *sess = &_debug_repl_sess;
    ns_vm *vm = &_debug_repl_vm;
    vm->debug_session = &sess;
    vm->step_hook = ns_debug_repl_step_hook;

    // ns_return_bool ret = ns_ast_parse(options.);
    if (options.filename.len == 0) {
        ns_warn("ns_debug", "no input file.\n");
        return 0;
    }

    ns_str src = ns_read_file(options.filename);
    if (src.len == 0) {
        ns_error("ns_debug", "file not found: %.*s\n", options.filename.len, options.filename.data);
        return 1;
    }

    sess->source = src;
    ns_return_void ret = ns_debug_repl_loop();
    if (ns_return_is_error(ret)) {
        ns_error("ns_debug", "repl error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
        return 1;
    }

    return 0;
}