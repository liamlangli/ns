#include "ns_ast.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_fmt.h"

#include <readline/readline.h>
#include <readline/history.h>

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
    ns_ast_ctx ctx = {0};
    ns_array_set_capacity(ctx.nodes, 4);

    ns_vm_import_std_symbols(vm);
    vm->repl = true;

    // read eval pri32loop
    ns_str filename = ns_str_cstr("<repl>");
    while(1) {
        ns_str line = ns_repl_read_line(ns_color_log "ns" ns_color_nil "> ");
        if (line.len == 0) continue;
        // if get exit command
        if (ns_str_equals_STR(line, "exit")) {
            ns_repl_free_line(line);
            break;
        }

        ns_ast_parse(&ctx, line, filename);
        ns_vm_parse(vm, &ctx);

        for (i32 i = ctx.section_begin; i < ctx.section_end; ++i) {
            ns_ast_t n = ctx.nodes[ctx.sections[i++]];
            switch (n.type) {
                case NS_AST_PRIMARY_EXPR: {
                    ns_value v = ns_eval_primary_expr(vm, n);
                    if (v.t.type != NS_TYPE_NIL) {
                        ns_str s = ns_fmt_value(vm, v);
                        printf("   [" ns_color_log "%s" ns_color_nil "]\n", s.data);
                        ns_str_free(s);
                    }
                } break;
                case NS_AST_VAR_DEF:
                    ns_eval_var_def(vm, &ctx, n);
                    break;
                case NS_AST_CALL_EXPR:
                case NS_AST_EXPR:
                    ns_eval_expr(vm, &ctx, n);
                    break;
                default:
                    break;
            }
        }
        ns_repl_free_line(line);
    }
    ne_exit_safe("ns", "exit repl\n");
}