#include "ns_ast.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_fmt.h"

#include <readline/readline.h>
#include <readline/history.h>

typedef void(*ns_repl_cmd_fn)(ns_vm *vm, ns_str arg);
typedef struct ns_repl_cmd {
    ns_str cmd;
    ns_str desc;
    ns_repl_cmd_fn fn;
} ns_repl_cmd;

typedef struct ns_repl_ctx {
    ns_repl_cmd *cmds;
} ns_repl_ctx;

static ns_repl_ctx _ctx = {0};
static ns_ast_ctx _ast = {0};

ns_str ns_repl_read_line(i8 *prompt) {
    i8 *line = readline(prompt);
    if (!line) return ns_str_null;
    add_history(line);
    ns_str s = ns_str_cstr(line);
    return s;
}

void ns_repl_free_line(ns_str s) {
    ns_str_free(s);
}

void ns_repl_print_ast(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_ast_ctx_print(&_ast, false);
}

void ns_repl_mem(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_mem_status();
}

void ns_repl_add_cmd(ns_str cmd, ns_repl_cmd_fn fn) {
    ns_repl_cmd c = (ns_repl_cmd){.cmd = cmd, .fn = fn};
    ns_array_push(_ctx.cmds, c);
}

void ns_repl_exit(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_array_free(_ctx.cmds);
    _ctx.cmds = NULL;
    ns_exit_safe("ns", "exit repl\n");
}

void ns_repl_init(void) {
    ns_repl_add_cmd(ns_str_cstr("ast"), ns_repl_print_ast);
    ns_repl_add_cmd(ns_str_cstr("mem"), ns_repl_mem);
    ns_repl_add_cmd(ns_str_cstr("exit"), ns_repl_exit);
    ns_repl_add_cmd(ns_str_cstr("q"), ns_repl_exit);
}

ns_bool ns_repl_invoke_cmd(ns_vm *vm, ns_str cmd) {
    if (ns_str_empty(cmd)) return false;
    szt len = ns_array_length(_ctx.cmds);
    if (len == 0) return false;
    for (szt i = 0; i < len; ++i) {
        ns_repl_cmd *c = &_ctx.cmds[i];
        if (ns_str_equals(c->cmd, cmd)) {
            c->fn(vm, cmd);
            return true;
        }
    }
    return false;
}

void ns_repl(ns_vm* vm) {
    vm->repl = true;
    ns_repl_init();

    ns_lib_import(vm, ns_str_cstr("std"));
    // read eval pri32loop
    ns_str filename = ns_str_cstr("<repl>");
    while(1) {
        ns_str line = ns_repl_read_line(ns_color_log "ns" ns_color_nil "> ");
        if (line.len == 0) continue;

        if (ns_repl_invoke_cmd(vm, line)) {
            ns_repl_free_line(line);
            continue;
        }

        ns_return_bool ret_p = ns_ast_parse(&_ast, line, filename);
        if (ns_return_is_error(ret_p)) {
            ns_warn("ast", "parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
        }

        ret_p = ns_vm_parse(vm, &_ast);
        if (ns_return_is_error(ret_p)) {
            ns_warn("parse", "vm parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
        }

        for (i32 i = _ast.section_begin, l = _ast.section_end; i < l; ++i) {
            i32 s_i = _ast.sections[i];
            ns_ast_t *n = &_ast.nodes[s_i];
            ns_return_value ret = ns_return_ok(value, ns_nil);
            if (n->type >= NS_AST_EXPR && n->type <= NS_AST_ARRAY_EXPR) {
                ret = ns_eval_expr(vm, &_ast, s_i);
            } else {
                switch (n->type)
                {
                case NS_AST_VAR_DEF: ret = ns_eval_var_def(vm, &_ast, s_i); break;
                case NS_AST_IMPORT_STMT: { 
                    ret = ns_return_ok(value, ns_nil);
                    ns_lib_import(vm, n->import_stmt.lib.val);
                } break;
                default: ns_warn("eval", "invalid expr type: %d\n", n->type); break;
                }
            }
            if (ns_return_is_error(ret)) {
                ns_warn("eval", "eval error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
            }
            ns_str s = ns_fmt_value(vm, ret.r);
            printf("%.*s\n", s.len, s.data);
        }
        ns_repl_free_line(line);
        _ast.section_begin = _ast.section_end;
    }
}