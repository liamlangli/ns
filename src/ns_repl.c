#include "ns_ast.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_fmt.h"
#include "ns_os.h"

#include <readline/readline.h>
#include <readline/history.h>

typedef void(*ns_repl_cmd_fn)(ns_vm *vm, ns_str arg);
typedef struct ns_repl_cmd {
    ns_str cmd;
    ns_str s;
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

void ns_repl_mem(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_mem_status();
}

void ns_repl_load(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_info("ns_repl", "load file: %.*s\n", arg.len, arg.data);
    ns_str filename = ns_str_slice(arg, 0, arg.len);

    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_ast_parse(&_ast, source, filename);
    ns_vm_parse(vm, &_ast);
}

void ns_repl_symbols(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_vm_symbol_print(vm);
}

void ns_repl_add_cmd(ns_str cmd, ns_str shortcut, ns_repl_cmd_fn fn, ns_str desc) {
    ns_repl_cmd c = (ns_repl_cmd){.cmd = cmd, .fn = fn, .desc = desc, .s = shortcut};
    ns_array_push(_ctx.cmds, c);
}

void ns_repl_exit(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_array_free(_ctx.cmds);
    _ctx.cmds = NULL;
    ns_exit_safe("ns", "exit repl\n");
}

void ns_repl_help(ns_vm *vm, ns_str arg) {
    ns_unused(vm); ns_unused(arg);
    ns_info("ns_repl", "available commands:\n");
    for (szt i = 0; i < ns_array_length(_ctx.cmds); ++i) {
        ns_repl_cmd *c = &_ctx.cmds[i];
        printf("  [%.*s]%8.*s:"ns_color_cmt" %.*s\n" ns_color_nil, c->s.len, c->s.data, c->cmd.len, c->cmd.data, c->desc.len, c->desc.data);
    }
}

void ns_repl_init(void) {
    ns_repl_add_cmd(ns_str_cstr("help"), ns_str_cstr("h"), ns_repl_help, ns_str_cstr("show help"));;
    ns_repl_add_cmd(ns_str_cstr("mem"), ns_str_cstr("m"), ns_repl_mem, ns_str_cstr("show memory status"));
    ns_repl_add_cmd(ns_str_cstr("exit"), ns_str_cstr("q"), ns_repl_exit, ns_str_cstr("exit repl"));
    ns_repl_add_cmd(ns_str_cstr("symbols"), ns_str_cstr("s"), ns_repl_symbols, ns_str_cstr("show symbols"));
    ns_repl_add_cmd(ns_str_cstr("load"), ns_str_cstr("l"), ns_repl_load, ns_str_cstr("load file"));
}

ns_bool ns_repl_invoke_cmd(ns_vm *vm, ns_str line) {
    if (ns_str_is_empty(line)) return false;

    szt len = ns_array_length(_ctx.cmds);
    if (len == 0) return false;

    // find first space to split cmd
    i32 space = ns_str_index_of(line, ns_str_cstr(" "));

    ns_str cmd;
    ns_str arg;
    if (space == -1) {
        space = cmd.len;
        cmd = line;
        arg = ns_str_nil;
    } else {
        cmd = ns_str_range(line.data, space);
        arg = ns_str_sub_expr(ns_str_range(line.data + space, line.len - space));
    }
    if (ns_str_is_empty(arg)) {
        arg = ns_str_cstr("");
    }

    for (szt i = 0; i < len; ++i) {
        ns_repl_cmd *c = &_ctx.cmds[i];
        if (ns_str_equals(c->s, cmd) || ns_str_equals(c->cmd, cmd)) {
            c->fn(vm, arg);
            return true;
        }
    }
    return false;
}

void ns_repl(ns_vm* vm) {
    vm->repl = true;
    setenv("NS_REPL_RECOVER", "1", 1);
    ns_repl_init();

    ns_lib_import(vm, ns_str_cstr("std"));
    // read eval pri32loop
    ns_str filename = ns_str_cstr("<repl>");
    while(1) {
        ns_str line = ns_repl_read_line(ns_color_log "ns" ns_color_nil "> ");
        if (!line.data) {
            printf("\n");
            break;
        }
        if (line.len == 0) {
            ns_repl_free_line(line);
            continue;
        }

        if (ns_repl_invoke_cmd(vm, line)) {
            ns_repl_free_line(line);
            continue;
        }

        ns_return_bool ret_p = ns_ast_parse(&_ast, line, filename);
        if (ns_return_is_error(ret_p)) {
            ns_warn("ast", "parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
            ns_repl_free_line(line);
            _ast.section_begin = _ast.section_end;
            continue;
        }

        ret_p = ns_vm_parse(vm, &_ast);
        if (ns_return_is_error(ret_p)) {
            ns_warn("parse", "vm parse error: %.*s\n", ret_p.e.msg.len, ret_p.e.msg.data);
            ns_repl_free_line(line);
            _ast.section_begin = _ast.section_end;
            continue;
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
                case NS_AST_USE_STMT: { 
                    ret = ns_return_ok(value, ns_nil);
                    ns_lib_import(vm, n->use_stmt.lib.val);
                } break;
                default: ns_warn("eval", "invalid expr type: %d\n", n->type); break;
                }
            }
            if (ns_return_is_error(ret)) {
                ns_warn("eval", "eval error: %.*s\n", ret.e.msg.len, ret.e.msg.data);
                continue;
            }
            ns_str s = ns_fmt_value(vm, ret.r);
            printf("%.*s\n", s.len, s.data);
        }
        ns_repl_free_line(line);
        _ast.section_begin = _ast.section_end;
    }
    unsetenv("NS_REPL_RECOVER");
}
