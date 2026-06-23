#include "ns.h"
#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"
#include "ns_asm.h"
#include "ns_pe.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define STB_DS_IMPLEMENTATION

static ns_vm vm = {0};
static ns_ast_ctx ctx = {0};

typedef struct ns_compile_option_t {
    ns_bool tokenize_only: 2;
    ns_bool ast_only: 2;
    ns_bool ssa_only: 2;
    ns_bool aarch_only: 2;
    ns_bool macho_only: 2;
    ns_bool macho_obj_only: 2;
    ns_bool wasm_only: 2;
    ns_bool pe_only: 2;
    ns_bool symbol_only: 2;
    ns_bool show_version: 2;
    ns_bool show_help: 2;
    ns_bool run: 2;     // `ns run <file>`  - compile project scope and execute
    ns_bool test: 2;    // `ns test <path>` - compile and run test entries
    ns_str output;
    ns_str filename;
} ns_compile_option_t;

ns_compile_option_t parse_options(i32 argc, i8** argv) {
    ns_compile_option_t option = {0};
    for (i32 i = 1; i < argc; i++) {
        if (i == 1 && strcmp(argv[i], "run") == 0) {
            option.run = true;
        } else if (i == 1 && strcmp(argv[i], "test") == 0) {
            option.test = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--token") == 0) {
            option.tokenize_only = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--ast") == 0) {
            option.ast_only = true;
        } else if (strcmp(argv[i], "--ssa") == 0) {
            option.ssa_only = true;
        } else if (strcmp(argv[i], "--aarch") == 0) {
            option.aarch_only = true;
        } else if (strcmp(argv[i], "--macho") == 0) {
            option.macho_only = true;
        } else if (strcmp(argv[i], "--macho-o") == 0 || strcmp(argv[i], "--macho-obj") == 0) {
            option.macho_obj_only = true;
        } else if (strcmp(argv[i], "--wasm") == 0) {
            option.wasm_only = true;
        } else if (strcmp(argv[i], "--pe") == 0) {
            option.pe_only = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--symbol") == 0) {
            option.symbol_only = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            option.show_version = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            option.show_help = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else {
            option.filename = ns_str_cstr(argv[i]); // unmatched argument is treated as filename
        }
    }
    return option;
}

void ns_help() {
    ns_asm_target target;
    ns_asm_get_current_target(&target);
    ns_str arch = ns_arch_str(target.arch);
    ns_str os = ns_os_str(target.os);

    ns_info("usage", "ns [option] [file.ns]\n");
    printf("target: %.*s-%.*s\n", arch.len, arch.data, os.len, os.data);
    printf("  -t --token        tokenize only\n");
    printf("  -a --ast          parse ast only\n");
    printf("  --ssa             lower ast to ssa blocks\n");
    printf("  --aarch           lower ssa to aarch64 machine words\n");
    printf("  --macho           emit mach-o executable (arm64)\n");
    printf("  --macho-o         emit mach-o object file (.o, arm64)\n");
    printf("  --wasm            emit webassembly module (.wasm)\n");
    printf("  --pe              emit windows pe executable (.exe, amd64)\n");
    printf("  -s --symbol       print symbol table\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  -o --output       output path\n");
    printf("\ncommands:\n");
    printf("  run  <file.ns>    compile the project scope and run it\n");
    printf("  test <path>       run a test entry, or every *_test.ns under a dir\n");

}

void ns_version() {
    ns_info("nanoscript", "v%d.%d\n", (int)VERSION_MAJOR, (int)VERSION_MINOR);
}

void ns_exec_tokenize(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_token(source, filename);
}

void ns_exec_ast(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);
    ns_ast_ctx_print(&ctx, true);
}

void ns_exec_symbol(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    if (ns_return_is_error(ret)) ns_return_assert(ret);

    ret = ns_vm_parse(&vm, &ctx);
    if (ns_return_is_error(ret)) ns_return_assert(ret);
    ns_vm_symbol_print(&vm);
}

void ns_exec_ssa(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *m = ssa_ret.r;
    ns_ssa_print(m);
    ns_ssa_module_free(m);
}

void ns_exec_aarch(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_ptr bin_ret = ns_aarch_from_ssa(ssa);
    if (ns_return_is_error(bin_ret)) {
        ns_ssa_module_free(ssa);
        ns_return_assert(bin_ret);
    }

    ns_aarch_module_bin *bin = bin_ret.r;
    ns_aarch_print(bin);
    ns_aarch_free(bin);
    ns_ssa_module_free(ssa);
}

void ns_exec_macho(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.out");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_macho_emit(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("macho", "output %.*s\n", output.len, output.data);
}

void ns_exec_macho_object(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.out.o");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_macho_emit_object(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("macho", "object %.*s\n", output.len, output.data);
}

void ns_exec_wasm(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.wasm");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_wasm_emit(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("wasm", "output %.*s\n", output.len, output.data);
}

void ns_exec_pe(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.exe");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_pe_emit(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("pe", "output %.*s\n", output.len, output.data);
}

// ---------------------------------------------------------------------------
// Scope-based project compile
// ---------------------------------------------------------------------------
// Nano Script's `use` only imports modules from the interpreter's ref path,
// so a multi-file program in a working directory ("project scope") cannot
// import its own local modules. The linker below resolves `use <name>` against
// the nearest manifest root first: when `<root>/<name>.ns` exists it is
// inlined (its body merged into a single translation unit, after stripping its
// own `mod`/local-`use` lines), otherwise the `use` is kept as an external
// library import (e.g. `use std`). This lets the self-hosted `ns-in-ns` project
// be compiled, run and tested directly by `ns`, without a Makefile to
// concatenate sources by hand. Scope projects must have an `ns.mod` TOML
// manifest at their root; for now the runtime only requires its presence.

typedef struct ns_linker {
    ns_str scope;       // directory used to resolve local modules
    ns_str *seen;       // module names already inlined (dedup)
    ns_str *ext_seen;   // external lib names already emitted (dedup)
    ns_str body;        // accumulated module + entry bodies
    ns_str ext;         // accumulated external `use` lines
} ns_linker;

static ns_bool ns_name_in(ns_str *names, ns_str name) {
    for (i32 i = 0, l = ns_array_length(names); i < l; i++) {
        if (ns_str_equals(names[i], name)) return true;
    }
    return false;
}

static ns_bool ns_is_ident_char(i8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static ns_bool ns_is_dir(ns_str path);

static ns_str ns_path_dirname_safe(ns_str path) {
    for (i32 i = path.len - 1; i >= 0; i--) {
        if (path.data[i] == NS_PATH_SEPARATOR) {
            if (i == 0) return ns_str_cstr("/");
            return ns_str_slice(path, 0, i);
        }
    }
    return ns_str_cstr(".");
}

static ns_str ns_path_parent(ns_str dir) {
    if (dir.len == 0 || ns_str_equals(dir, ns_str_cstr(".")) || ns_str_equals(dir, ns_str_cstr("/"))) {
        return dir;
    }
    return ns_path_dirname_safe(dir);
}

static ns_bool ns_file_exists(ns_str path) {
    ns_str data = ns_fs_read_file(path);
    if (data.data == ns_null) return false;
    ns_str_free(data);
    return true;
}

static ns_str ns_project_root(ns_str path) {
    ns_str dir = ns_is_dir(path) ? path : ns_path_dirname_safe(path);
    while (true) {
        ns_str manifest = ns_path_join(dir, ns_str_cstr("ns.mod"));
        ns_bool found = ns_file_exists(manifest);
        ns_str_free(manifest);
        if (found) return dir;

        ns_str parent = ns_path_parent(dir);
        if (ns_str_equals(parent, dir)) break;
        dir = parent;
    }
    ns_exit(1, "ns", "scope project requires ns.mod at project root for %.*s.\n", path.len, path.data);
    return ns_str_null;
}

static void ns_link_source(ns_linker *lk, ns_str src);

// Inline the local module `name` (resolved as `<scope>/<name>.ns`) once.
static void ns_link_module(ns_linker *lk, ns_str name, ns_str src) {
    if (ns_name_in(lk->seen, name)) return;
    ns_array_push(lk->seen, name);
    ns_link_source(lk, src);
}

static void ns_link_source(ns_linker *lk, ns_str src) {
    i32 i = 0;
    while (i < src.len) {
        i32 ls = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 le = i;            // line end (exclusive of '\n')
        if (i < src.len) i++;  // consume newline

        // first non-blank column of the line
        i32 t = ls;
        while (t < le && (src.data[t] == ' ' || src.data[t] == '\t' || src.data[t] == '\r')) t++;

        ns_bool is_use = (le - t >= 4) && strncmp(src.data + t, "use ", 4) == 0;
        ns_bool is_mod = (le - t >= 4) && strncmp(src.data + t, "mod ", 4) == 0;

        if (is_use) {
            i32 ns_s = t + 3;
            while (ns_s < le && (src.data[ns_s] == ' ' || src.data[ns_s] == '\t')) ns_s++;
            i32 ns_e = ns_s;
            while (ns_e < le && ns_is_ident_char(src.data[ns_e])) ns_e++;
            ns_str name = ns_str_slice(src, ns_s, ns_e);

            ns_str path = ns_path_join(lk->scope, ns_str_concat(name, ns_str_cstr(".ns")));
            ns_str mod_src = ns_fs_read_file(path);
            if (mod_src.data != ns_null) {
                ns_link_module(lk, name, mod_src);   // local module -> inline
            } else if (!ns_name_in(lk->ext_seen, name)) {
                ns_array_push(lk->ext_seen, name);   // external lib -> keep `use`
                ns_str_append_len(&lk->ext, "use ", 4);
                ns_str_append_len(&lk->ext, name.data, name.len);
                ns_str_append_len(&lk->ext, "\n", 1);
            }
            continue; // the `use` line itself never reaches the merged body
        }

        if (is_mod) continue; // module markers are meaningless once inlined

        ns_str_append_len(&lk->body, src.data + ls, le - ls);
        ns_str_append_len(&lk->body, "\n", 1);
    }
}

// Merge the entry source plus every local module it transitively `use`s into a
// single source string (external `use`s hoisted to the top). The result is
// null-terminated and owns its buffer.
static ns_str ns_project_link(ns_str scope, ns_str entry_src) {
    ns_linker lk = {0};
    lk.scope = scope.len > 0 ? scope : ns_str_cstr(".");
    ns_link_source(&lk, entry_src);

    ns_str out = ns_str_null;
    if (lk.ext.len > 0) ns_str_append_len(&out, lk.ext.data, lk.ext.len);
    if (lk.body.len > 0) ns_str_append_len(&out, lk.body.data, lk.body.len);
    ns_array_push(out.data, '\0');
    out.dynamic = false; // backed by ns_array, freed at process exit

    ns_str_free(lk.ext);
    ns_str_free(lk.body);
    ns_array_free(lk.seen);
    ns_array_free(lk.ext_seen);
    return out;
}

void ns_exec_run(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.data == ns_null || source.len == 0)
        ns_exit(1, "ns", "invalid input file %.*s.\n", filename.len, filename.data);

    ns_str scope = ns_project_root(filename);
    ns_str merged = ns_project_link(scope, source);

    ns_return_value ret_v = ns_eval(&vm, merged, filename);
    if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
}

// Run a single test entry in a fresh VM; returns the entry's i32 exit status
// (0 == all assertions passed).
static i32 ns_run_test_file(ns_str filename) {
    ns_vm tvm = {0};
    ns_str source = ns_fs_read_file(filename);
    if (source.data == ns_null || source.len == 0) {
        ns_warn("test", "skip empty/unreadable file %.*s.\n", filename.len, filename.data);
        return 1;
    }

    ns_str scope = ns_project_root(filename);
    ns_str merged = ns_project_link(scope, source);

    ns_return_value ret_v = ns_eval(&tvm, merged, filename);
    if (ns_return_is_error(ret_v)) {
        ns_warn("test", "%.*s errored.\n", filename.len, filename.data);
        return 1;
    }
    return ns_eval_number_i32(&tvm, ret_v.r);
}

static ns_bool ns_is_dir(ns_str path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.data);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(path.data, &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
#endif
}

static ns_bool ns_is_test_name(const char *name) {
    size_t n = strlen(name);
    const char *suffix = "_test.ns";
    size_t s = strlen(suffix);
    return n > s && strcmp(name + (n - s), suffix) == 0;
}

static int ns_cstr_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Discover and run every `*_test.ns` file directly under `dir`.
static void ns_exec_test_dir(ns_str dir) {
    char **names = ns_null;
#if defined(_WIN32)
    ns_str pattern = ns_path_join(dir, ns_str_cstr("*_test.ns"));
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.data, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (ns_is_test_name(fd.cFileName)) ns_array_push(names, strdup(fd.cFileName));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir.data);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != ns_null) {
            if (ns_is_test_name(e->d_name)) ns_array_push(names, strdup(e->d_name));
        }
        closedir(d);
    }
#endif

    i32 count = (i32)ns_array_length(names);
    if (count == 0) {
        ns_warn("test", "no *_test.ns files under %.*s\n", dir.len, dir.data);
        return;
    }
    qsort(names, count, sizeof(char *), ns_cstr_cmp);

    i32 passed = 0, failed = 0;
    for (i32 i = 0; i < count; i++) {
        ns_str file = ns_path_join(dir, ns_str_cstr(names[i]));
        printf("== %s ==\n", names[i]);
        i32 code = ns_run_test_file(file);
        if (code == 0) { passed++; } else { failed++; }
        printf("-- %s: %s --\n", names[i], code == 0 ? "PASS" : "FAIL");
        free(names[i]);
    }
    ns_array_free(names);

    printf("\nsuites: %d passed, %d failed\n", passed, failed);
    if (failed != 0) exit(1);
}

void ns_exec_test(ns_str path) {
    if (path.len == 0) ns_error("ns", "no test file or directory.\n");
    if (ns_is_dir(path)) {
        ns_exec_test_dir(path);
        return;
    }
    i32 code = ns_run_test_file(path);
    if (code != 0) exit(1);
}

void ns_exec_eval(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_fs_read_file(filename);
    if (source.len == 0) ns_exit(1, "ns", "invalid input file %.*s.\n", filename.len, filename.data);
    ns_return_value ret_v = ns_eval(&vm, source, filename);
    if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
    ns_str ret = ns_fmt_value(&vm, ret_v.r);
    printf("%.*s\n", ret.len, ret.data);
    ns_str_free(ret);
}

void ns_exec_repl() {
    ns_repl(&vm);
}

i32 main(i32 argc, i8** argv) {
    ns_compile_option_t option = parse_options(argc, argv);

    if (option.show_help) {
        ns_help(); return 0;
    }

    if (option.show_version) {
        ns_version(); return 0;
    }

    if (option.run) {
        ns_exec_run(option.filename);
    } else if (option.test) {
        ns_exec_test(option.filename);
    } else if (option.tokenize_only) {
        ns_exec_tokenize(option.filename);
    } else if (option.ast_only) {
        ns_exec_ast(option.filename);
        ns_mem_status();
    } else if (option.ssa_only) {
        ns_exec_ssa(option.filename);
    } else if (option.aarch_only) {
        ns_exec_aarch(option.filename);
    } else if (option.macho_only) {
        ns_exec_macho(option.filename, option.output);
    } else if (option.macho_obj_only) {
        ns_exec_macho_object(option.filename, option.output);
    } else if (option.wasm_only) {
        ns_exec_wasm(option.filename, option.output);
    } else if (option.pe_only) {
        ns_exec_pe(option.filename, option.output);
    } else if (option.symbol_only) {
        ns_exec_symbol(option.filename);
    } else {
        if (option.filename.len == 0) {
            ns_version();
            ns_exec_repl();
        } else {
            ns_exec_eval(option.filename);
        }
    }
    return 0;
}
