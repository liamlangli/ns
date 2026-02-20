#include "ns_debug.h"
#include "ns_ast.h"

#include <readline/readline.h>
#include <readline/history.h>

typedef enum ns_debug_repl_command_type {
    NS_DEBUG_REPL_NONE,
    NS_DEBUG_REPL_HELP,
    NS_DEBUG_REPL_LOAD,
    NS_DEBUG_REPL_RUN,
    NS_DEBUG_REPL_STEP_INTO,
    NS_DEBUG_REPL_STEP_NEXT,
    NS_DEBUG_REPL_STEP_OUT,
    NS_DEBUG_REPL_BREAK,
    NS_DEBUG_REPL_BREAK_LIST,
    NS_DEBUG_REPL_BREAK_DELETE,
    NS_DEBUG_REPL_BREAK_ERROR,
    NS_DEBUG_REPL_BREAK_CLEAR,
    NS_DEBUG_REPL_PRINT,
    NS_DEBUG_REPL_AST,
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

void ns_debug_repl_help();
ns_str ns_debug_repl_read_line(char *prompt);
ns_debug_repl_command ns_debug_repl_parse_command(ns_str line);
ns_return_void ns_debug_repl_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i);
ns_return_void ns_debug_repl_loop();

void ns_debug_repl_list_breakpoints(ns_debug_session *sess);
void ns_debug_repl_set_breakpoint(ns_debug_session *sess, ns_str f, i32 l);
void ns_debug_repl_del_breakpoint(ns_debug_session *sess, ns_str f, i32 l);
ns_bool ns_debug_hit_breakpoint(ns_debug_session *sess, ns_str f, i32 l);
void ns_debug_repl_print(ns_vm *vm, ns_str expr);
ns_bool ns_debug_should_pause(ns_debug_session *sess, ns_vm *vm, ns_ast_t *n, ns_str f);

static ns_vm _debug_repl_vm = {0};
static ns_debug_session _debug_repl_sess = {0};
static ns_ast_ctx _debug_repl_ctx = {0};
static ns_ast_ctx _debug_ast_ctx = {0};
static ns_str _debug_repl_history;

#define NS_DEBUG_REPL_HISTORY_FILE ".cache/ns/nsdb.history"

void ns_debug_repl_help() {
    ns_info("ns_debug", "nsdb nanoscript debugger\n");
    printf(" |name            |short                 |description\n");
    printf("  help             h                     : help\n");
    printf("  load             l [file]              : load file\n");
    printf("  run              r                     : run\n");
    printf("  step-into        si                    : step into\n");
    printf("  step-over        sn                    : step over\n");
    printf("  step-out         so                    : step out\n");
    printf("  break            b [line]              : set breakpoint\n");
    printf("  break-list       bl                    : list breakpoints\n");
    printf("  break-delete     bd [line]             : remove breakpoint\n");
    printf("  break-error      be                    : set error breakpoint\n");
    printf("  break-clear      bc                    : clear all breakpoints\n");
    printf("  print            p [expr]              : print expression\n");
    printf("  quit             q                     : quit\n");
    printf("  ast              ast                   : dump AST\n");
}

ns_str ns_debug_repl_read_line(char *prompt) {
    HIST_ENTRY *h = history_get(history_length);
    char *l = readline(prompt);
    if (!l) return ns_str_null;
    if (!h || strcmp(h->line, l)) {
        add_history(l);
        write_history(_debug_repl_history.data);
    }
    ns_str s = ns_str_cstr(l);
    return s;
}

const ns_str help_cmd = ns_str_cstr("help");
const ns_str h_cmd = ns_str_cstr("h");
const ns_str load_cmd = ns_str_cstr("load");
const ns_str l_cmd = ns_str_cstr("l");
const ns_str run_cmd = ns_str_cstr("run");
const ns_str r_cmd = ns_str_cstr("r");
const ns_str si_cmd = ns_str_cstr("step-into");
const ns_str si_short_cmd = ns_str_cstr("si");
const ns_str sn_cmd = ns_str_cstr("step-over");
const ns_str sn_short_cmd = ns_str_cstr("sn");
const ns_str step_out_cmd = ns_str_cstr("step-out");
const ns_str step_out_short_cmd = ns_str_cstr("so");
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
const ns_str ast_cmd = ns_str_cstr("ast");

ns_debug_repl_command ns_debug_repl_parse_command(ns_str line) {
    ns_debug_repl_command cmd = {0};
    if (ns_str_starts_with(line, help_cmd) || ns_str_starts_with(line, h_cmd)) {
        cmd.type = NS_DEBUG_REPL_HELP;
    } else if (ns_str_starts_with(line, load_cmd) || ns_str_starts_with(line, l_cmd)) {
        cmd.type = NS_DEBUG_REPL_LOAD;
        cmd.expr = ns_str_sub_expr(line);
    } else if (ns_str_starts_with(line, run_cmd) || ns_str_starts_with(line, r_cmd)) {
        cmd.type = NS_DEBUG_REPL_RUN;
    } else if (ns_str_starts_with(line, si_cmd) || ns_str_starts_with(line, si_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_STEP_INTO;
    } else if (ns_str_starts_with(line, sn_cmd) || ns_str_starts_with(line, sn_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_STEP_NEXT;
    } else if (ns_str_starts_with(line, step_out_cmd) || ns_str_starts_with(line, step_out_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_STEP_OUT;
    } else if (ns_str_starts_with(line, bl_cmd) || ns_str_starts_with(line, bl_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_LIST;
    } else if (ns_str_starts_with(line, bd_cmd) || ns_str_starts_with(line, bd_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_DELETE;
        cmd.line = ns_str_to_i32(ns_str_sub_expr(line));
    } else if (ns_str_starts_with(line, be_cmd) || ns_str_starts_with(line, be_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_ERROR;
    } else if (ns_str_starts_with(line, bc_cmd) || ns_str_starts_with(line, bc_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK_CLEAR;
    } else if (ns_str_starts_with(line, p_cmd) || ns_str_starts_with(line, p_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_PRINT;
        cmd.expr = ns_str_sub_expr(line);
    } else if (ns_str_starts_with(line, quit_cmd) || ns_str_starts_with(line, q_cmd)) {
        cmd.type = NS_DEBUG_REPL_QUIT;
    } else if (ns_str_starts_with(line, ast_cmd)) {
        cmd.type = NS_DEBUG_REPL_AST;
    } else if (ns_str_starts_with(line, b_cmd) || ns_str_starts_with(line, b_short_cmd)) {
        cmd.type = NS_DEBUG_REPL_BREAK;
        cmd.line = ns_str_to_i32(ns_str_sub_expr(line));
    } else  {
        cmd.type = NS_DEBUG_REPL_UNKNOWN;
    }
    return cmd;
}

ns_return_void ns_debug_repl_step_hook(ns_vm *vm, ns_ast_ctx *ctx, i32 i) {
    ns_debug_session *sess = &_debug_repl_sess;
    ns_ast_t *n = &ctx->nodes[i];
    ns_str f = ctx->filename;
    if (!ns_debug_should_pause(sess, vm, n, f)) return ns_return_ok_void;
    sess->state = NS_DEBUG_STATE_PAUSED;
    sess->step_mode = NS_DEBUG_STEP_NONE;
    sess->pause_line = n->state.l;
    sess->pause_file = f;
    
    ns_info("ns_debug", "hit breakpoint at %.*s:%d:%d\n", f.len, f.data, n->state.l, n->state.o);
    ns_return_void ret = ns_debug_repl_loop();
    if (ns_return_is_error(ret)) {
        ns_error("ns_debug", "repl error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
    }
    return ret;
}

ns_bool ns_debug_should_pause(ns_debug_session *sess, ns_vm *vm, ns_ast_t *n, ns_str f) {
    i32 depth = ns_array_length(vm->call_stack);
    switch (sess->step_mode) {
    case NS_DEBUG_STEP_INTO:
        return true;
    case NS_DEBUG_STEP_OVER:
        if (depth < sess->step_depth) return true;
        if (depth == sess->step_depth && n->state.l != sess->pause_line) return true;
        return false;
    case NS_DEBUG_STEP_OUT:
        if (depth < sess->step_depth) return true;
        return false;
    case NS_DEBUG_STEP_NONE:
    default:
        return ns_debug_hit_breakpoint(sess, f, n->state.l);
    }
}

void ns_debug_repl_list_breakpoints(ns_debug_session *sess) {
    i32 len = ns_array_length(sess->breakpoints);
    ns_info("ns_debug", "breakpoints: %d\n", len);
    for (i32 i = 0, l = len; i < l; i++) {
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
        if (b.l == l) return;
        if (b.l > l) {
            ns_array_insert(sess->breakpoints, i, bp);
            return;
        }
    }
    ns_array_push(sess->breakpoints, bp);
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

ns_bool ns_debug_hit_breakpoint(ns_debug_session *sess, ns_str f, i32 l) {
    for (i32 i = 0, len = ns_array_length(sess->breakpoints); i < len; i++) {
        ns_debug_breakpoint bp = sess->breakpoints[i];
        if (bp.l == l && ns_str_equals(bp.f, f)) return true;
    }
    return false;
}

void ns_debug_repl_print(ns_vm *vm, ns_str expr) {
    vm->repl = true;
    ns_ast_ctx *ctx = &_debug_repl_ctx;

    ns_return_bool ret_p = ns_ast_parse(ctx, expr, ns_str_cstr("<repl>"));
    if (ns_return_is_error(ret_p)) {
        ns_warn("ast", "parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
        return;
    }

    for (i32 i = ctx->section_begin, l = ctx->section_end; i < l; ++i) {
        i32 s_i = ctx->sections[i];
        ns_ast_t *n = &ctx->nodes[s_i];
        if (n->type >= NS_AST_EXPR && n->type <= NS_AST_ARRAY_EXPR) {
            ns_return_value ret = ns_eval_expr(vm, ctx, s_i);
            if (ns_return_is_error(ret)) {
                ns_warn("eval", "eval error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
            }
            ns_str s = ns_fmt_value(vm, ret.r);
            printf("%.*s\n", s.len, s.data);
        } else {
            ns_warn("eval", "invalid expr type: %d\n", n->type);
        }
    }

    ns_array_set_length(ctx->sections, 0);
    ns_array_set_length(ctx->nodes, 0);
    ctx->section_begin = ctx->section_end = 0;
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
        case NS_DEBUG_REPL_HELP:
            ns_debug_repl_help();
            break;
        case NS_DEBUG_REPL_LOAD:
            if (sess->state != NS_DEBUG_STATE_INIT) {
                ns_warn("ns_debug", "invalid state\n");
                break;
            }

            sess->options.filename = cmd.expr;
            sess->source = ns_fs_read_file(cmd.expr);
            if (sess->source.len == 0) {
                ns_warn("ns_debug", "file not found: %.*s\n", cmd.expr.len, cmd.expr.data);
                break;
            }

            ns_return_bool ret_ast = ns_ast_parse(&_debug_ast_ctx, sess->source, sess->options.filename);
            if (ns_return_is_error(ret_ast)) return ns_return_change_type(void, ret_ast);

            sess->state = NS_DEBUG_STATE_READY;
            break;
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
            case NS_DEBUG_STATE_RUNNING:
                return ns_return_ok_void;
            case NS_DEBUG_STATE_INIT:
                return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "no program loaded");
            default:
                return ns_return_error(void, ns_code_loc_nil, NS_ERR_EVAL, "invalid state");
                break;
            }
            break;
        case NS_DEBUG_REPL_STEP_INTO:
            ns_info("ns_debug", "step into\n");
            if (sess->state == NS_DEBUG_STATE_PAUSED) {
                sess->step_mode = NS_DEBUG_STEP_INTO;
                sess->step_depth = ns_array_length(vm->call_stack);
                sess->state = NS_DEBUG_STATE_RUNNING;
                return ns_return_ok_void;
            } else if (sess->state == NS_DEBUG_STATE_READY) {
                sess->step_mode = NS_DEBUG_STEP_INTO;
                sess->step_depth = 0;
                sess->pause_line = -1;
                sess->state = NS_DEBUG_STATE_RUNNING;
                ns_return_value ret = ns_eval(vm, sess->source, sess->options.filename);
                if (ns_return_is_error(ret)) return ns_return_change_type(void, ret);
            } else {
                ns_warn("ns_debug", "step into requires paused or ready state\n");
            }
            break;
        case NS_DEBUG_REPL_STEP_NEXT:
            ns_info("ns_debug", "step next\n");
            if (sess->state != NS_DEBUG_STATE_PAUSED) {
                ns_warn("ns_debug", "step over requires paused state\n");
                break;
            }
            sess->step_mode = NS_DEBUG_STEP_OVER;
            sess->step_depth = ns_array_length(vm->call_stack);
            sess->state = NS_DEBUG_STATE_RUNNING;
            return ns_return_ok_void;
            break;
        case NS_DEBUG_REPL_STEP_OUT:
            ns_info("ns_debug", "step out\n");
            if (sess->state != NS_DEBUG_STATE_PAUSED) {
                ns_warn("ns_debug", "step out requires paused state\n");
                break;
            }
            sess->step_mode = NS_DEBUG_STEP_OUT;
            sess->step_depth = ns_array_length(vm->call_stack);
            sess->state = NS_DEBUG_STATE_RUNNING;
            return ns_return_ok_void;
            break;
        case NS_DEBUG_REPL_BREAK:
            ns_info("ns_debug", "break %d\n", cmd.line);
            ns_debug_repl_set_breakpoint(sess, sess->options.filename, cmd.line);
            break;
        case NS_DEBUG_REPL_BREAK_LIST:
            ns_debug_repl_list_breakpoints(sess);
            break;
        case NS_DEBUG_REPL_BREAK_DELETE:
            ns_info("ns_debug", "break delete %d\n", cmd.line);
            ns_debug_repl_del_breakpoint(sess, sess->options.filename, cmd.line);
            break;
        case NS_DEBUG_REPL_BREAK_ERROR:
            ns_info("ns_debug", "break on error\n");
            break;
        case NS_DEBUG_REPL_BREAK_CLEAR:
            ns_array_set_length(sess->breakpoints, 0);
            break;
        case NS_DEBUG_REPL_PRINT:
            ns_debug_repl_print(vm, cmd.expr);
            break;
        case NS_DEBUG_REPL_QUIT:
            ns_info("ns_debug", "quit\n");
            exit(0);
            return ns_return_ok_void;
        case NS_DEBUG_REPL_UNKNOWN:
            ns_warn("ns_debug", "unknown command: %.*s\n", line.len, line.data);
            break;
        case NS_DEBUG_REPL_AST:
            ns_ast_ctx_print(&_debug_ast_ctx, false);
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
    sess->options = options;
    ns_vm *vm = &_debug_repl_vm;
    sess->vm = vm;
    sess->state = NS_DEBUG_STATE_INIT;
    sess->step_mode = NS_DEBUG_STEP_NONE;
    sess->pause_line = -1;
    vm->step_hook = ns_debug_repl_step_hook;

    _debug_repl_history = ns_path_join(ns_path_home(), ns_str_cstr(NS_DEBUG_REPL_HISTORY_FILE));
    ns_os_mkdir(ns_path_dirname(_debug_repl_history));
    read_history(_debug_repl_history.data);

    ns_return_void ret = ns_debug_repl_loop();
    if (ns_return_is_error(ret)) {
        ns_code_loc loc = ret.e.loc;
        ns_error("ns_debug", "[%.*s:%d:%d] repl error: %.*s\n", loc.f.len, loc.f.data, loc.l, loc.o, ret.e.msg.len, ret.e.msg.data);
        return 1;
    }

    return 0;
}
