#include "ns_ast.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_fmt.h"

#include <readline/readline.h>
#include <readline/history.h>

static ns_vm _ns_repl_vm = {0};
static ns_ast_ctx _ns_repl_ctx = {0};

ns_str ns_repl_read_line(char *prompt) {
    char *line = readline(prompt);
    if (!line) return ns_str_null;
    add_history(line);
    ns_str s = ns_str_cstr(line);
    return s;
}

void ns_repl_free_line(ns_str s) {
    ns_str_free(s);
}

void ns_repl(ns_vm* vm) {
    vm->repl = true;

    // read eval pri32loop
    ns_str filename = ns_str_cstr("<repl>");
    while(1) {
        ns_str line = ns_repl_read_line(ns_color_log "ns" ns_color_nil "> ");
        if (line.len == 0) continue;
        // if get exit command
        if (ns_str_equals_STR(line, "q") || ns_str_equals_STR(line, "exit")) {
            ns_repl_free_line(line);
            break;
        }

        ns_return_bool ret_p = ns_ast_parse(&_ns_repl_ctx, line, filename);
        if (ns_return_is_error(ret_p)) {
            ns_warn("ast", "parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
        }

        ret_p = ns_vm_parse(vm, &_ns_repl_ctx);
        if (ns_return_is_error(ret_p)) {
            ns_warn("parse", "vm parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
        }

        for (i32 i = _ns_repl_ctx.section_begin, l = _ns_repl_ctx.section_end; i < l; ++i) {
            i32 s_i = _ns_repl_ctx.sections[i];
            ns_ast_t *n = &_ns_repl_ctx.nodes[s_i];
            ns_return_value ret = ns_return_ok(value, ns_nil);
            if (n->type >= NS_AST_EXPR && n->type <= NS_AST_ARRAY_EXPR) {
                ret = ns_eval_expr(vm, &_ns_repl_ctx, s_i);
            } else if (n->type == NS_AST_VAR_DEF) {
                ret = ns_eval_var_def(vm, &_ns_repl_ctx, s_i);
            } else {
                ns_warn("eval", "invalid expr type: %d\n", n->type);
                continue;
            }
            if (ns_return_is_error(ret)) {
                ns_warn("eval", "eval error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
            }
            ns_str s = ns_fmt_value(vm, ret.r);
            printf("%.*s\n", s.len, s.data);
        }
        ns_repl_free_line(line);

        _ns_repl_ctx.section_begin = _ns_repl_ctx.section_end;
    }
    ne_exit_safe("ns", "exit repl\n");
}