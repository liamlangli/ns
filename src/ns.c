#include "ns.h"
#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"
#include "ns_asm.h"
#include "ns_pe.h"
#include "ns_shader.h"
#include "ns_profile.h"
#include "ns_project.h"
#include "ns_build_cache.h"
#include "ns_lint.h"
#include "ns_ssa.h"
#include "ns_agents_md.h"

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(NS_DARWIN)
#include <mach-o/dyld.h>
#endif

#define NS_MANIFEST_SCHEMA_CURRENT "ns.mod/v1"
#define NS_MANIFEST_SCHEMA_LEGACY "ns.mod/v0"
#define NS_WASM_DEFAULT_PORT 9001

static ns_vm vm = {0};
static ns_ast_ctx ctx = {0};

static ns_return_ptr ns_ssa_build_for_cli(ns_ast_ctx *ast) {
    return ns_ssa_build_with_runtime_paths(ast, vm.ref_path, vm.lib_path, vm.lib_fallback_path);
}

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
    ns_bool profile: 2; // write ns.profile and print a colored hot-path summary
    ns_bool profile_cmd: 2; // `ns profile [path]` - CLI run with profiling
    ns_bool profile_view: 2; // `ns profiler [file]` - open the GUI viewer
    ns_bool run: 2;     // `ns run <file>`  - compile project scope and execute
    ns_bool test: 2;    // `ns test <path>` - compile and run test entries
    ns_bool build: 2;   // `ns build <path>` - compile project scope to an artifact
    ns_bool force: 2;   // `--force` - rebuild even when the recorded inputs are unchanged
    ns_bool clean: 2;   // `ns clean [path]` - remove the files a build generates
    ns_bool project: 2; // `ns project <path>` - generate a native IDE project
    ns_bool init: 2;    // `ns init [path]` - scaffold an ns project in place
    ns_bool create: 2;  // `ns create <name>` - scaffold an ns project in a new folder
    ns_bool update: 2;  // `ns update [path]` - migrate project metadata to the current format
    ns_bool lint: 2;    // `ns lint [path]` - report style findings
    ns_bool lint_fix: 2; // `ns lint_fix [path]` - rewrite the fixable findings
    ns_bool shader_only: 2; // `ns --shader <target> <file>` - transpile shader fns
    ns_bool shader_bin: 2;  // also compile the emitted source with the platform toolchain
    u8 build_kind;      // 0 auto, 1 executable, 2 library
    i32 positional_count;
    i32 program_argc;
    i8 **program_argv;
    ns_str shader_target;
    ns_str entry;
    ns_str output;
    ns_str filename;
    i32 port;
    ns_bool port_set;
} ns_compile_option_t;

ns_compile_option_t parse_options(i32 argc, i8** argv) {
    ns_compile_option_t option = {0};
    for (i32 i = 1; i < argc; i++) {
        if (i == 1 && strcmp(argv[i], "run") == 0) {
            option.run = true;
        } else if (i == 1 && strcmp(argv[i], "test") == 0) {
            option.test = true;
        } else if (i == 1 && strcmp(argv[i], "build") == 0) {
            option.build = true;
        } else if (i == 1 && strcmp(argv[i], "clean") == 0) {
            option.clean = true;
        } else if (i == 1 && strcmp(argv[i], "project") == 0) {
            option.project = true;
        } else if (i == 1 && strcmp(argv[i], "init") == 0) {
            option.init = true;
        } else if (i == 1 && strcmp(argv[i], "create") == 0) {
            option.create = true;
        } else if (i == 1 && strcmp(argv[i], "update") == 0) {
            option.update = true;
        } else if (i == 1 && strcmp(argv[i], "profile") == 0) {
            option.profile_cmd = true;
            option.profile = true;
        } else if (i == 1 && strcmp(argv[i], "profiler") == 0) {
            option.profile_view = true;
        } else if (i == 1 && strcmp(argv[i], "lint") == 0) {
            option.lint = true;
        } else if (i == 1 && (strcmp(argv[i], "lint_fix") == 0 || strcmp(argv[i], "lint-fix") == 0)) {
            option.lint = true;
            option.lint_fix = true;
        } else if (strcmp(argv[i], "--fix") == 0) {
            option.lint_fix = true;
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
        } else if (strcmp(argv[i], "--profile") == 0) {
            option.profile = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            option.force = true;
        } else if (strcmp(argv[i], "--exe") == 0) {
            option.build_kind = 1;
        } else if (strcmp(argv[i], "--app") == 0) {
            option.build_kind = 3;
        } else if (strcmp(argv[i], "--lib") == 0 || strcmp(argv[i], "--library") == 0) {
            option.build_kind = 2;
        } else if (strcmp(argv[i], "--shader") == 0) {
            option.shader_only = true;
            if (i + 1 < argc) { i++; option.shader_target = ns_str_cstr(argv[i]); } // ns_str_cstr is a macro: no argv[++i]
        } else if (strcmp(argv[i], "--entry") == 0) {
            if (i + 1 < argc) { i++; option.entry = ns_str_cstr(argv[i]); }
        } else if (strcmp(argv[i], "--shader-bin") == 0) {
            option.shader_bin = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) ns_exit(1, "usage", "%s needs a path.\n", argv[i]);
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) ns_exit(1, "usage", "--port needs a number.\n");
            char *end = ns_null;
            long port = strtol(argv[++i], &end, 10);
            if (!end || *end || port < 0 || port > 65535) ns_exit(1, "usage", "invalid port %s.\n", argv[i]);
            option.port = (i32)port;
            option.port_set = true;
        } else if (strcmp(argv[i], "--") == 0 && (option.run || option.profile_cmd)) {
            // Everything after `--` is for the program, including a missing file.
            option.program_argv = &argv[i + 1];
            option.program_argc = argc - i - 1;
            break;
        } else {
            if (option.filename.len == 0) {
                option.filename = ns_str_cstr(argv[i]); // unmatched argument is treated as filename
                option.positional_count++;
                // `ns run` / `ns profile` take one file or target; the rest is
                // published as NS_ARGC / NS_ARG0... for the program to read.
                if (option.run || option.profile_cmd) {
                    option.program_argv = &argv[i + 1];
                    option.program_argc = argc - i - 1;
                    if (option.program_argc > 0 && strcmp(option.program_argv[0], "--") == 0) {
                        option.program_argv++;
                        option.program_argc--;
                    }
                    break;
                }
            } else {
                option.positional_count++;
            }
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
    printf("  --shader <target> transpile shader fns to msl | glsl | hlsl | wgsl source\n");
    printf("  --entry <name>    shader entry fn (default: every vs_*/fs_*/ps_*/cs_* fn)\n");
    printf("  --shader-bin      also compile the shader source when the platform\n");
    printf("                    toolchain is installed (xcrun metal / glslc / dxc)\n");
    printf("  -s --symbol       print symbol table\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  --profile         profile execution or build phases; write ns.profile and print a hot-path summary\n");
    printf("  -o --output       output path\n");
    printf("\ncommands:\n");
    printf("  init [path]       scaffold an ns project in place (default: cwd)\n");
    printf("                    writes ns.mod, src/main.ns, README.md, AGENTS.md and .gitignore\n");
    printf("  create <name>     scaffold an ns project in a new <name> folder\n");
    printf("  update [path]     migrate ns.mod and refresh project support files\n");
    printf("  run  [file|target] [args...] run native source; link = true builds and launches\n");
    printf("                    wasm projects build and serve bin/\n");
    printf("                    a bare name selects a [[targets]] entry of ns.mod\n");
    printf("                    arguments after the file or target become NS_ARG0...\n");
    printf("       --port <n>    wasm server port (default 9001; 0 chooses an available port)\n");
    printf("  profile [path]    run like `ns run` with profiling; print a CLI hot-path summary\n");
    printf("  profiler [file]   open the GUI viewer for ns.profile (or file)\n");
    printf("  test [path]       run <project>/test/*_test.ns, a test file, or a test dir\n");
    printf("  build [path|target] compile and link a script/module to an executable or static lib\n");
    printf("                    uses ns.mod type when path is omitted or a module dir\n");
    printf("                    type app | cli | library picks bundle | executable | .a\n");
    printf("                    builds every [[targets]] table; a bare name builds one\n");
    printf("                    --exe/--app or --lib/--library can force artifact type\n");
    printf("                    app manifests may set icon = \"path/to/image.png\"\n");
    printf("                    keeps the artifact when no input changed; --force rebuilds\n");
    printf("  clean [path]      remove bin/ and the profiles a build generates\n");
    printf("  project [path]    generate a native IDE project from ns.mod\n");
    printf("                    Darwin: bin/<name>.xcodeproj; Windows: bin/<name>.sln\n");
    printf("  lint [path]       check the style of a file, a directory or the whole project\n");
    printf("  lint_fix [path]   rewrite the fixable findings in place (`ns lint --fix`)\n");
    printf("                    rules come from the [lint] table of ns.mod\n");

}

void ns_version() {
    ns_info("nanoscript", "v%d.%d\n", (int)VERSION_MAJOR, (int)VERSION_MINOR);
}

static f64 ns_profile_start_ms = 0.0;
static i32 ns_profile_argc = 0;
static i8 **ns_profile_argv = ns_null;
static ns_bool ns_profile_registered = false;
static ns_bool ns_profile_written = false;

static void ns_profile_emit(f64 start_ms, i32 argc, i8 **argv) {
    if (ns_profile_written) return;
    ns_profile_written = true;

    f64 elapsed_ms = ns_profile_now_ms() - start_ms;
    FILE *f = fopen("ns.profile", "w");
    if (!f) {
        ns_warn("profile", "failed to write ns.profile.\n");
        return;
    }
    ns_profile_write_text(f, elapsed_ms, argc, argv);
    fclose(f);

    f64 ffi_ms = ns_profile.ffi_total_ms;
    f64 ffi_pct = elapsed_ms > 0.0 ? (ffi_ms / elapsed_ms) * 100.0 : 0.0;
    ns_info("profile", "wrote ns.profile (%.3f ms total, %llu scopes, %llu ffi calls, %.3f ms / %.1f%% in ffi)\n",
            elapsed_ms, (unsigned long long)ns_profile.scope_calls, (unsigned long long)ns_profile.ffi_calls, ffi_ms, ffi_pct);
    ns_profile_print_summary(stdout, elapsed_ms);
}

static void ns_profile_emit_at_exit(void) {
    if (!ns_profile.enabled) return;
    ns_profile_emit(ns_profile_start_ms, ns_profile_argc, ns_profile_argv);
}

static void ns_profile_begin(i32 argc, i8 **argv) {
    ns_profile_reset();
    ns_profile_start_ms = ns_profile_now_ms();
    ns_profile_enable(ns_profile_start_ms);
    ns_profile_argc = argc;
    ns_profile_argv = argv;

    if (!ns_profile_registered) {
        if (atexit(ns_profile_emit_at_exit) != 0) {
            ns_warn("profile", "failed to register exit hook; profile only writes on normal return.\n");
        } else {
            ns_profile_registered = true;
        }
    }
}

// Build profiling uses the same nested scope model as interpreted functions,
// with a module label that keeps compiler phases distinct in the tables and
// viewer. Literal phase names remain valid until the report is written.
static f64 ns_build_profile_begin(const char *name) {
    if (!ns_profile.enabled) return 0.0;
    f64 start_ms = ns_profile_now_ms();
    ns_profile_scope_enter(ns_str_cstr((char *)name), ns_str_cstr("compiler"));
    return start_ms;
}

static void ns_build_profile_end(const char *name, f64 start_ms) {
    if (!ns_profile.enabled || start_ms == 0.0) return;
    f64 elapsed_ms = ns_profile_now_ms() - start_ms;
    ns_profile_record_scope(ns_str_cstr((char *)name), ns_str_cstr("compiler"), 0, start_ms, elapsed_ms);
}

static void ns_exec_profile_view(ns_str filename);
void ns_exec_run(ns_str filename, i32 port, ns_bool port_set, ns_bool honor_link);

void ns_exec_tokenize(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) { 
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }
    ns_token(source, filename);
}

void ns_exec_ast(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
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
    ns_str source = ns_os_read_file(filename);
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
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *m = ssa_ret.r;
    ns_ssa_print(m);
    ns_ssa_module_free(m);
}

void ns_exec_aarch(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
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
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.out");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_macho_emit(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("macho", "output %.*s\n", output.len, output.data);
}

void ns_exec_macho_object(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.out.o");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_macho_emit_object(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("macho", "object %.*s\n", output.len, output.data);
}

void ns_exec_wasm(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.wasm");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_ssa_module *ssa = ssa_ret.r;

    ns_return_bool emit_ret = ns_wasm_emit(ssa, output);
    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_info("wasm", "output %.*s\n", output.len, output.data);
}

void ns_exec_pe(ns_str filename, ns_str output) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) {
        ns_warn("ns", "empty file %.*s.\n", filename.len, filename.data);
        return;
    }

    if (output.len == 0) {
        output = ns_str_cstr("bin/a.exe");
    }

    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
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
// A manifest project is one translation unit: every .ns file below its source
// directory is linked recursively unless `exclude` in ns.mod removes it.
// Project-local `use` lines are therefore optional and are stripped when
// present; external imports (for example `use std`) are hoisted and deduped.
// A standalone file keeps the older sibling-module import behavior.

typedef struct ns_project_source {
    ns_str path;
    ns_str relative;
} ns_project_source;

typedef struct ns_linker {
    ns_str scope;          // directory used to resolve local modules
    ns_bool project_all;   // source set is complete; never pull excluded siblings
    ns_str *seen;          // module names already inlined (dedup)
    ns_str *local_names;   // module names supplied by the project source set
    ns_str *ext_seen;      // external lib names already emitted (dedup)
    ns_str body;           // accumulated module + entry bodies
    ns_str ext;            // accumulated external `use` lines
    ns_line_loc *body_map; // origin (file, line) for each emitted body line
    ns_line_loc *ext_map;  // origin (file, line) for each emitted external line
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

static ns_bool ns_str_has_suffix(ns_str s, const char *suffix) {
    i32 n = (i32)strlen(suffix);
    return s.len >= n && strncmp(s.data + s.len - n, suffix, n) == 0;
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
    ns_str data = ns_os_read_file(path);
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

static void ns_link_source(ns_linker *lk, ns_str src, ns_str file);

// Inline the local module `name` (resolved as `<scope>/<name>.ns`) once.
static void ns_link_module(ns_linker *lk, ns_str name, ns_str src, ns_str file) {
    if (ns_name_in(lk->seen, name)) return;
    ns_array_push(lk->seen, name);
    ns_link_source(lk, src, file);
}

// Append `src` to the merged body, recording each emitted line's origin
// (`file` + its 1-based line in that file) into the linker's source map so the
// merged translation unit can be mapped back for diagnostics. `line` counts
// every physical line in `src`, including the `use`/`mod` lines that are
// stripped, so it always matches the source file's real line numbers.
static void ns_link_source(ns_linker *lk, ns_str src, ns_str file) {
    i32 i = 0;
    i32 line = 0;
    while (i < src.len) {
        i32 ls = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 le = i;            // line end (exclusive of '\n')
        if (i < src.len) i++;  // consume newline
        line++;                // 1-based physical line in `file`

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

            if (ns_name_in(lk->local_names, name)) {
                // Every local source is already part of the project
                // translation unit, so the import is redundant.
            } else if (!ns_name_in(lk->ext_seen, name)) {
                ns_str path = ns_str_null;
                ns_str mod_src = ns_str_null;
                if (!lk->project_all) {
                    path = ns_path_join(lk->scope, ns_str_concat(name, ns_str_cstr(".ns")));
                    mod_src = ns_os_read_file(path);
                }
                if (mod_src.data != ns_null) {
                    ns_link_module(lk, name, mod_src, path); // standalone sibling module
                } else {
                    ns_array_push(lk->ext_seen, name);   // external lib -> keep `use`
                    ns_str_append_len(&lk->ext, "use ", 4);
                    ns_str_append_len(&lk->ext, name.data, name.len);
                    ns_str_append_len(&lk->ext, "\n", 1);
                    ns_array_push(lk->ext_map, ((ns_line_loc){.f = file, .l = line}));
                }
            }
            continue; // the `use` line itself never reaches the merged body
        }

        if (is_mod) continue; // module markers are meaningless once inlined

        ns_str_append_len(&lk->body, src.data + ls, le - ls);
        ns_str_append_len(&lk->body, "\n", 1);
        ns_array_push(lk->body_map, ((ns_line_loc){.f = file, .l = line}));
    }
}

// Finish a linked translation unit (external `use`s hoisted to the top). The
// result is null-terminated and owns its buffer. When `out_map` is non-NULL it
// receives one source location per output line for diagnostic remapping.
static ns_str ns_link_finish(ns_linker *lk, ns_line_loc **out_map, ns_str **out_external_modules) {
    ns_str out = ns_str_null;
    if (lk->ext.len > 0) ns_str_append_len(&out, lk->ext.data, lk->ext.len);
    if (lk->body.len > 0) ns_str_append_len(&out, lk->body.data, lk->body.len);
    ns_array_push(out.data, '\0');
    out.dynamic = false; // backed by ns_array, freed at process exit

    // The map mirrors `out`: external `use` lines first, then the body lines.
    if (out_map != ns_null) {
        ns_line_loc *map = ns_null;
        for (i32 i = 0, l = ns_array_length(lk->ext_map); i < l; i++) ns_array_push(map, lk->ext_map[i]);
        for (i32 i = 0, l = ns_array_length(lk->body_map); i < l; i++) ns_array_push(map, lk->body_map[i]);
        *out_map = map;
    }

    if (out_external_modules != ns_null) {
        ns_str *modules = ns_null;
        for (i32 i = 0, l = ns_array_length(lk->ext_seen); i < l; i++) {
            ns_array_push(modules, ns_str_concat(lk->ext_seen[i], ns_str_cstr("")));
        }
        *out_external_modules = modules;
    }

    ns_str_free(lk->ext);
    ns_str_free(lk->body);
    ns_array_free(lk->seen);
    ns_array_free(lk->local_names);
    ns_array_free(lk->ext_seen);
    ns_array_free(lk->ext_map);
    ns_array_free(lk->body_map);
    return out;
}

static ns_str ns_project_link(ns_str scope, ns_str entry_src, ns_str entry_file,
                              ns_line_loc **out_map, ns_str **out_external_modules) {
    ns_linker lk = {0};
    lk.scope = scope.len > 0 ? scope : ns_str_cstr(".");
    ns_link_source(&lk, entry_src, entry_file);
    return ns_link_finish(&lk, out_map, out_external_modules);
}

// Absolute current working directory (heap-owned), or "." if it can't be read.
static ns_str ns_getcwd(void) {
    char buf[4096];
#if defined(_WIN32)
    if (_getcwd(buf, sizeof(buf)) == ns_null) return ns_str_cstr(".");
#else
    if (getcwd(buf, sizeof(buf)) == ns_null) return ns_str_cstr(".");
#endif
    return ns_str_concat(ns_str_cstr(buf), ns_str_cstr(""));
}

static ns_bool ns_path_is_absolute(ns_str path) {
    if (path.len == 0) return false;
#if defined(_WIN32)
    return path.data[0] == '\\' || path.data[0] == '/' ||
           (path.len >= 3 && path.data[1] == ':' && (path.data[2] == '\\' || path.data[2] == '/'));
#else
    return path.data[0] == '/';
#endif
}

static ns_str ns_path_resolve(ns_str root, ns_str path) {
    if (path.data == ns_null || path.len == 0) return ns_str_null;
    if (ns_path_is_absolute(path)) return ns_str_concat(path, ns_str_cstr(""));
    return ns_path_join(root, path);
}

// The top-level keys of a manifest: everything before its first `[table]` or
// `[[table]]` header. Restricting lookups to that slice keeps a key such as
// `name` or `target` inside a `[[dependencies.runtime]]` or `[[targets]]`
// table from being read as the project-wide value.
static ns_str ns_manifest_head(ns_str src) {
    for (i32 i = 0; i < src.len;) {
        i32 ls = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 t = ls;
        while (t < i && (src.data[t] == ' ' || src.data[t] == '\t')) t++;
        if (t < i && src.data[t] == '[') return (ns_str){.data = src.data, .len = ls};
        if (i < src.len) i++;
    }
    return src;
}

// Extract the first quoted string that follows `key =` in a TOML manifest.
// Handles both `key = "value"` and `key = ["value", ...]`; the key must be the
// first token on its line so `entry` never matches `entries`. Returns a
// heap-owned ns_str, or ns_str_null when the key is absent.
static ns_str ns_manifest_value(ns_str src, const char *key) {
    src = ns_manifest_head(src);
    i32 klen = (i32)strlen(key);
    i32 i = 0;
    while (i < src.len) {
        i32 ls = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 le = i;
        if (i < src.len) i++;

        i32 t = ls;
        while (t < le && (src.data[t] == ' ' || src.data[t] == '\t' || src.data[t] == '\r')) t++;
        if (le - t < klen || strncmp(src.data + t, key, klen) != 0) continue;

        i32 p = t + klen;
        while (p < le && (src.data[p] == ' ' || src.data[p] == '\t')) p++;
        if (p >= le || src.data[p] != '=') continue; // not `key = ...`
        p++;

        while (p < le && src.data[p] != '"') p++; // first quote on the line
        if (p >= le) continue;
        i32 vs = ++p;
        while (p < le && src.data[p] != '"') p++;
        if (p >= le) continue;
        return ns_str_concat(ns_str_slice(src, vs, p), ns_str_cstr(""));
    }
    return ns_str_null;
}

// Return every quoted value assigned to a top-level TOML key. This is used for
// string arrays such as `exclude = ["generated.ns", "vendor/**"]`.
static ns_str *ns_manifest_values(ns_str src, const char *key) {
    src = ns_manifest_head(src);
    ns_str *values = ns_null;
    i32 klen = (i32)strlen(key);
    for (i32 i = 0; i < src.len;) {
        i32 ls = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 le = i;
        if (i < src.len) i++;

        i32 p = ls;
        while (p < le && (src.data[p] == ' ' || src.data[p] == '\t' || src.data[p] == '\r')) p++;
        if (le - p < klen || strncmp(src.data + p, key, klen) != 0) continue;
        p += klen;
        while (p < le && (src.data[p] == ' ' || src.data[p] == '\t')) p++;
        if (p >= le || src.data[p++] != '=') continue;
        while (p < le && src.data[p] != '[' && src.data[p] != '"') p++;
        ns_bool array = p < le && src.data[p] == '[';
        if (array) p++;
        i32 end = array ? src.len : le;
        while (p < end) {
            if (array && src.data[p] == ']') break;
            if (!array && src.data[p] == '\n') break;
            if (src.data[p] != '"') { p++; continue; }
            i32 start = ++p;
            while (p < end && src.data[p] != '"') p++;
            if (p >= end) break;
            ns_array_push(values, ns_str_slice(src, start, p));
            p++;
        }
        break;
    }
    return values;
}

// ---------------------------------------------------------------------------
// `[[targets]]` manifest tables
// ---------------------------------------------------------------------------
// A manifest may declare several runnable targets, each with its own entry:
//
//   [[targets]]
//   name = "editor"
//   entry = "main.ns"
//   default = true
//
//   [[targets]]
//   name = "editor-web"
//   entry = "web_main.ns"
//   platform = "wasm"
//
// `ns run <name>` and `ns build <name>` select one by name; without a name the
// target marked `default = true`, otherwise the first declared one, is used.
// Every target keeps the project source set, minus the entries owned by the
// targets that were not selected, so each target declares its own `main`.
typedef struct ns_manifest_target {
    ns_str name;        // selector, and the default artifact name
    ns_str entry;       // entry source, relative to the manifest `source` dir
    ns_str type;        // app | library; inherits the top-level `type`
    ns_str platform;    // wasm; inherits the top-level `target`
    ns_str icon;        // inherits the top-level `icon`
    ns_str shell;       // inherits the top-level `shell`
    ns_str output;      // artifact and display name; defaults to `name`
    ns_str *exclude;    // sources removed for this target only
    ns_bool is_default; // `default = true`
    ns_bool link;       // `ns run` builds and launches the native artifact
    ns_bool has_link;   // distinguishes an inherited value from `link = false`
} ns_manifest_target;

// Offset just past `key =` on `line`, or -1 when the line assigns another key.
// The key must be the first token so `entry` never matches `entries`.
static i32 ns_manifest_line_assign(ns_str line, const char *key) {
    i32 klen = (i32)strlen(key);
    i32 p = 0;
    while (p < line.len && (line.data[p] == ' ' || line.data[p] == '\t')) p++;
    if (line.len - p < klen || strncmp(line.data + p, key, klen) != 0) return -1;
    p += klen;
    while (p < line.len && (line.data[p] == ' ' || line.data[p] == '\t')) p++;
    if (p >= line.len || line.data[p] != '=') return -1;
    return p + 1;
}

// First quoted value assigned to `key` on `line`, heap-owned, or ns_str_null.
static ns_str ns_manifest_line_str(ns_str line, const char *key) {
    i32 p = ns_manifest_line_assign(line, key);
    if (p < 0) return ns_str_null;
    while (p < line.len && line.data[p] != '"') p++;
    if (p >= line.len) return ns_str_null;
    i32 start = ++p;
    while (p < line.len && line.data[p] != '"') p++;
    if (p >= line.len) return ns_str_null;
    return ns_str_slice(line, start, p);
}

// Every quoted value of a single-line array such as `exclude = ["a", "b"]`.
static ns_bool ns_manifest_line_strs(ns_str line, const char *key, ns_str **out) {
    i32 p = ns_manifest_line_assign(line, key);
    if (p < 0) return false;
    while (p < line.len) {
        if (line.data[p] == ']') break;
        if (line.data[p] != '"') { p++; continue; }
        i32 start = ++p;
        while (p < line.len && line.data[p] != '"') p++;
        if (p >= line.len) break;
        ns_array_push(*out, ns_str_slice(line, start, p));
        p++;
    }
    return true;
}

static ns_bool ns_manifest_line_bool(ns_str line, const char *key, ns_bool *out) {
    i32 p = ns_manifest_line_assign(line, key);
    if (p < 0) return false;
    while (p < line.len && (line.data[p] == ' ' || line.data[p] == '\t')) p++;
    *out = line.len - p >= 4 && strncmp(line.data + p, "true", 4) == 0;
    return true;
}

// Read a boolean from the top-level manifest table. The return value says
// whether the key was present; `out` holds its value when it was.
static ns_bool ns_manifest_bool(ns_str src, const char *key, ns_bool *out) {
    src = ns_manifest_head(src);
    for (i32 i = 0; i < src.len;) {
        i32 start = i;
        while (i < src.len && src.data[i] != '\n') i++;
        ns_str line = ns_str_slice(src, start, i);
        if (i < src.len) i++;
        if (ns_manifest_line_bool(line, key, out)) return true;
    }
    return false;
}

// Match a `[name]` or `[[name]]` table header.
static ns_bool ns_manifest_table_is(ns_str line, const char *name) {
    i32 p = 0;
    while (p < line.len && (line.data[p] == ' ' || line.data[p] == '\t')) p++;
    i32 depth = 0;
    while (p < line.len && line.data[p] == '[') { depth++; p++; }
    if (depth == 0) return false;
    i32 nlen = (i32)strlen(name);
    if (line.len - p < nlen || strncmp(line.data + p, name, nlen) != 0) return false;
    p += nlen;
    while (p < line.len && (line.data[p] == ' ' || line.data[p] == '\t')) p++;
    return p < line.len && line.data[p] == ']';
}

// Parse every `[[targets]]` table of a manifest, in declaration order.
static ns_manifest_target *ns_manifest_targets(ns_str src) {
    ns_manifest_target *targets = ns_null;
    ns_bool inside = false;
    for (i32 i = 0; i < src.len;) {
        i32 ls = i;
        while (i < src.len && src.data[i] != '\n') i++;
        ns_str line = (ns_str){.data = src.data + ls, .len = i - ls};
        if (i < src.len) i++;
        while (line.len > 0 && (line.data[line.len - 1] == '\r' || line.data[line.len - 1] == ' ')) line.len--;

        i32 t = 0;
        while (t < line.len && (line.data[t] == ' ' || line.data[t] == '\t')) t++;
        if (t >= line.len || line.data[t] == '#') continue;
        if (line.data[t] == '[') {
            inside = ns_manifest_table_is(line, "targets") || ns_manifest_table_is(line, "target");
            if (inside) ns_array_push(targets, ((ns_manifest_target){0}));
            continue;
        }
        if (!inside) continue;

        ns_manifest_target *target = &targets[ns_array_length(targets) - 1];
        ns_str value;
        if ((value = ns_manifest_line_str(line, "name")).data != ns_null) { target->name = value; continue; }
        if ((value = ns_manifest_line_str(line, "entry")).data != ns_null) { target->entry = value; continue; }
        if ((value = ns_manifest_line_str(line, "type")).data != ns_null) { target->type = value; continue; }
        // `platform` is the canonical key; `target` inside the table is the
        // same value spelled like the top-level key it inherits from.
        if ((value = ns_manifest_line_str(line, "platform")).data != ns_null) { target->platform = value; continue; }
        if ((value = ns_manifest_line_str(line, "target")).data != ns_null) { target->platform = value; continue; }
        if ((value = ns_manifest_line_str(line, "icon")).data != ns_null) { target->icon = value; continue; }
        if ((value = ns_manifest_line_str(line, "shell")).data != ns_null) { target->shell = value; continue; }
        if ((value = ns_manifest_line_str(line, "output")).data != ns_null) { target->output = value; continue; }
        if (ns_manifest_line_bool(line, "default", &target->is_default)) continue;
        if (ns_manifest_line_bool(line, "link", &target->link)) { target->has_link = true; continue; }
        if (ns_manifest_line_strs(line, "exclude", &target->exclude)) continue;
    }
    return targets;
}

static void ns_manifest_targets_free(ns_manifest_target *targets) {
    for (i32 i = 0, count = ns_array_length(targets); i < count; i++) {
        ns_str_free(targets[i].name);
        ns_str_free(targets[i].entry);
        ns_str_free(targets[i].type);
        ns_str_free(targets[i].platform);
        ns_str_free(targets[i].icon);
        ns_str_free(targets[i].shell);
        ns_str_free(targets[i].output);
        for (i32 e = 0, l = ns_array_length(targets[i].exclude); e < l; e++) ns_str_free(targets[i].exclude[e]);
        ns_array_free(targets[i].exclude);
    }
    ns_array_free(targets);
}

// Index of the target named `name`, or -1. An empty name selects the default:
// the target marked `default = true`, otherwise the first declared one.
static i32 ns_manifest_target_index(ns_manifest_target *targets, ns_str name) {
    i32 count = ns_array_length(targets);
    if (count == 0) return -1;
    if (name.len == 0) {
        for (i32 i = 0; i < count; i++) {
            if (targets[i].is_default) return i;
        }
        return 0;
    }
    for (i32 i = 0; i < count; i++) {
        if (ns_str_equals(targets[i].name, name)) return i;
    }
    return -1;
}

static void ns_str_append_cstr(ns_str *s, const char *cstr);

// `a, b, c` over the declared target names, for diagnostics.
static ns_str ns_manifest_target_names(ns_manifest_target *targets) {
    ns_str list = ns_str_null;
    for (i32 i = 0, count = ns_array_length(targets); i < count; i++) {
        if (i > 0) ns_str_append_cstr(&list, ", ");
        ns_str_append(&list, targets[i].name);
    }
    ns_array_push(list.data, '\0');
    return list;
}

// Heap-owned copy of a manifest value, keeping an absent value absent.
static ns_str ns_str_dup(ns_str s) {
    return s.data == ns_null ? ns_str_null : ns_str_concat(s, ns_str_cstr(""));
}

// The directory manifest entries are relative to: `<root>/<source>`.
static ns_str ns_manifest_source_base(ns_str root, ns_str mod) {
    ns_str source = ns_manifest_value(mod, "source");
    ns_str base = (source.data != ns_null && !ns_str_equals(source, ns_str_cstr(".")))
                      ? ns_path_join(root, source)
                      : ns_str_concat(root, ns_str_cstr(""));
    ns_str_free(source);
    return base;
}

// The top-level `entry = "..."` or first of `entries = ["...", ...]`.
static ns_str ns_manifest_top_entry(ns_str mod) {
    ns_str entry = ns_manifest_value(mod, "entry");
    if (entry.data == ns_null) entry = ns_manifest_value(mod, "entries");
    return entry;
}

static ns_str ns_project_source_dir(ns_str root) {
    ns_str manifest_path = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str manifest = ns_os_read_file(manifest_path);
    ns_str source = ns_manifest_value(manifest, "source");
    ns_str result;
    if (source.data == ns_null || ns_str_equals(source, ns_str_cstr("."))) {
        result = ns_str_concat(root, ns_str_cstr(""));
    } else {
        result = ns_path_join(root, source);
    }
    ns_str_free(source);
    ns_str_free(manifest);
    ns_str_free(manifest_path);
    return result;
}

static ns_bool ns_glob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            ns_bool recursive = pattern[1] == '*';
            pattern += recursive ? 2 : 1;
            if (recursive && *pattern == '/') pattern++;
            do {
                if (ns_glob_match(pattern, text)) return true;
                if (!*text || (!recursive && *text == '/')) break;
                text++;
            } while (true);
            return false;
        }
        if (!*text || (*pattern != '?' && *pattern != *text)) return false;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static ns_str ns_path_normalized(ns_str path) {
    i32 start = path.len >= 2 && path.data[0] == '.' &&
                (path.data[1] == '/' || path.data[1] == '\\') ? 2 : 0;
    i8 *data = ns_malloc(path.len - start + 1);
    for (i32 i = start; i < path.len; i++) {
        data[i - start] = path.data[i] == '\\' ? '/' : path.data[i];
    }
    data[path.len - start] = '\0';
    return ns_str_range(data, path.len - start);
}

static ns_bool ns_project_path_excluded(ns_str *patterns, ns_str relative, ns_str source_relative) {
    ns_str paths[2] = {ns_path_normalized(relative), ns_path_normalized(source_relative)};
    ns_bool excluded = false;
    for (i32 p = 0, count = ns_array_length(patterns); p < count && !excluded; p++) {
        ns_str normalized = ns_path_normalized(patterns[p]);
        while (normalized.len > 0 && normalized.data[normalized.len - 1] == '/') normalized.len--;
        normalized.data[normalized.len] = '\0';
        for (i32 i = 0; i < 2 && !excluded; i++) {
            ns_str candidate = paths[i];
            ns_bool directory = patterns[p].len > 0 &&
                                (patterns[p].data[patterns[p].len - 1] == '/' ||
                                 patterns[p].data[patterns[p].len - 1] == '\\');
            if (directory) {
                excluded = candidate.len >= normalized.len &&
                           strncmp(candidate.data, normalized.data, normalized.len) == 0 &&
                           (candidate.len == normalized.len || candidate.data[normalized.len] == '/');
            } else {
                excluded = ns_glob_match(normalized.data, candidate.data);
            }
        }
        ns_str_free(normalized);
    }
    ns_str_free(paths[0]);
    ns_str_free(paths[1]);
    return excluded;
}

static ns_str ns_relative_join(ns_str lhs, const char *rhs) {
    i32 rhs_len = (i32)strlen(rhs);
    i32 slash = lhs.len > 0 ? 1 : 0;
    i8 *data = ns_malloc(lhs.len + slash + rhs_len + 1);
    if (lhs.len > 0) memcpy(data, lhs.data, lhs.len);
    if (slash) data[lhs.len] = '/';
    memcpy(data + lhs.len + slash, rhs, rhs_len);
    data[lhs.len + slash + rhs_len] = '\0';
    return ns_str_range(data, lhs.len + slash + rhs_len);
}

static void ns_project_sources_scan(ns_str dir, ns_str project_relative, ns_str source_relative,
                                    ns_str *excludes, ns_project_source **out) {
#if defined(_WIN32)
    ns_str pattern = ns_path_join(dir, ns_str_cstr("*"));
    WIN32_FIND_DATAA fd;
    HANDLE handle = FindFirstFileA(pattern.data, &fd);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
#else
    DIR *handle = opendir(dir.data);
    if (!handle) return;
    struct dirent *entry;
    while ((entry = readdir(handle)) != ns_null) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
#endif
        ns_str path = ns_path_join(dir, ns_str_cstr((char*)name));
        ns_str rel = ns_relative_join(project_relative, name);
        ns_str source_rel = ns_relative_join(source_relative, name);
        ns_bool directory = ns_is_dir(path);
        // `bin` is generated output, never project source even when source=".".
        ns_bool generated = project_relative.len == 0 && strcmp(name, "bin") == 0;
        ns_bool excluded = generated || ns_project_path_excluded(excludes, rel, source_rel);
        if (!excluded && directory) {
            ns_project_sources_scan(path, rel, source_rel, excludes, out);
        } else if (!excluded && ns_str_has_suffix(path, ".ns")) {
            ns_array_push(*out, ((ns_project_source){.path = path, .relative = rel}));
            continue;
        }
        ns_str_free(path);
        ns_str_free(rel);
        ns_str_free(source_rel);
#if defined(_WIN32)
    } while (FindNextFileA(handle, &fd));
    FindClose(handle);
#else
    }
    closedir(handle);
#endif
}

static int ns_project_source_cmp(const void *a, const void *b) {
    const ns_project_source *lhs = a;
    const ns_project_source *rhs = b;
    i32 n = lhs->relative.len < rhs->relative.len ? lhs->relative.len : rhs->relative.len;
    i32 order = strncmp(lhs->relative.data, rhs->relative.data, n);
    return order != 0 ? order : lhs->relative.len - rhs->relative.len;
}

static ns_str ns_project_module_name(ns_str path) {
    i32 start = 0;
    for (i32 i = 0; i < path.len; i++) {
        if (path.data[i] == '/' || path.data[i] == '\\') start = i + 1;
    }
    i32 end = path.len;
    if (end - start >= 3 && strncmp(path.data + end - 3, ".ns", 3) == 0) end -= 3;
    return ns_str_slice(path, start, end);
}

static ns_str ns_manifest_entry_file_for_root(ns_str root);

// Sources under test/ and files named *_test.ns are not part of an ordinary
// project build; `ns test` adds back only the selected test entry.
static ns_bool ns_project_source_is_test(ns_project_source source) {
    return ns_str_has_suffix(source.relative, "_test.ns") ||
           strncmp(source.relative.data, "test/", 5) == 0 ||
           strstr(source.relative.data, "/test/") != ns_null;
}

static void ns_project_target_excludes(ns_str root, ns_str manifest, ns_str entry_file, ns_str **excludes);

// Collect the manifest source set of `root` without linking it. The scan is
// deterministic and reads no source file, so a build can compare the set
// against its recorded inputs before deciding to compile. `entry_file` is the
// entry being built, so the set matches what linking that target compiles.
static ns_project_source *ns_project_sources(ns_str root, ns_str entry_file) {
    ns_str manifest_path = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str manifest = ns_os_read_file(manifest_path);
    ns_str *excludes = ns_manifest_values(manifest, "exclude");
    ns_project_target_excludes(root, manifest, entry_file, &excludes);
    ns_str source_dir = ns_project_source_dir(root);
    ns_str source_value = ns_manifest_value(manifest, "source");
    ns_str project_relative = (source_value.data == ns_null || ns_str_equals(source_value, ns_str_cstr(".")))
                                  ? ns_str_cstr("") : source_value;
    ns_project_source *sources = ns_null;
    ns_project_sources_scan(source_dir, project_relative, ns_str_cstr(""), excludes, &sources);
    qsort(sources, ns_array_length(sources), sizeof(ns_project_source), ns_project_source_cmp);

    for (i32 i = 0, count = ns_array_length(excludes); i < count; i++) ns_str_free(excludes[i]);
    ns_array_free(excludes);
    ns_str_free(manifest);
    ns_str_free(manifest_path);
    ns_str_free(source_value);
    ns_str_free(source_dir);
    return sources;
}

static void ns_project_sources_free(ns_project_source *sources) {
    for (i32 i = 0, count = ns_array_length(sources); i < count; i++) {
        ns_str_free(sources[i].path);
        ns_str_free(sources[i].relative);
    }
    ns_array_free(sources);
}

// True when a manifest entry declaration names the file being linked. Both
// paths are built the same way for a manifest-driven run, but an explicit
// `ns run path/to/entry.ns` can spell the same file differently.
static ns_bool ns_manifest_entry_matches(ns_str entry_path, ns_str entry_file, ns_str entry_relative) {
    if (ns_str_equals(entry_path, entry_file)) return true;
    ns_str declared = ns_path_normalized(entry_path);
    ns_str selected = ns_path_normalized(entry_file);
    ns_str relative = ns_path_normalized(entry_relative);
    ns_bool same = ns_str_equals(declared, selected);
    if (!same && selected.len > relative.len) {
        i32 at = selected.len - relative.len;
        same = selected.data[at - 1] == '/' &&
               strncmp(selected.data + at, relative.data, relative.len) == 0;
    }
    ns_str_free(declared);
    ns_str_free(selected);
    ns_str_free(relative);
    return same;
}

// The entries a manifest declares that the selected target does not own. Each
// target keeps its own `main`, so linking one removes the others' entries from
// the source set; the selected target adds its own `exclude` list.
static void ns_project_target_excludes(ns_str root, ns_str manifest, ns_str entry_file, ns_str **excludes) {
    ns_manifest_target *targets = ns_manifest_targets(manifest);
    if (ns_array_length(targets) == 0) {
        ns_manifest_targets_free(targets);
        return;
    }

    ns_str base = ns_manifest_source_base(root, manifest);
    ns_bool matched = false;
    for (i32 i = 0, count = ns_array_length(targets); i < count; i++) {
        ns_str entry = targets[i].entry;
        if (entry.data == ns_null) continue;
        ns_str path = ns_path_join(base, entry);
        if (!matched && ns_manifest_entry_matches(path, entry_file, entry)) {
            matched = true;
            for (i32 e = 0, l = ns_array_length(targets[i].exclude); e < l; e++) {
                ns_array_push(*excludes, ns_str_dup(targets[i].exclude[e]));
            }
        } else {
            ns_array_push(*excludes, ns_str_dup(entry));
        }
        ns_str_free(path);
    }

    // A top-level `entry` beside `[[targets]]` is one more competing main.
    ns_str top = ns_manifest_top_entry(manifest);
    if (top.data != ns_null) {
        ns_str path = ns_path_join(base, top);
        if (!ns_manifest_entry_matches(path, entry_file, top)) ns_array_push(*excludes, ns_str_dup(top));
        ns_str_free(path);
        ns_str_free(top);
    }
    ns_str_free(base);
    ns_manifest_targets_free(targets);
}

// Link all project sources. Files under test/ and other *_test.ns entries are
// excluded by default. Test execution adds only the selected test entry.
static ns_str ns_project_link_all(ns_str root, ns_str entry_src, ns_str entry_file, ns_bool test_entry,
                                  ns_line_loc **out_map, ns_str **out_external_modules) {
    ns_str manifest_path = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str manifest = ns_os_read_file(manifest_path);
    ns_str *excludes = ns_manifest_values(manifest, "exclude");
    ns_project_target_excludes(root, manifest, entry_file, &excludes);
    ns_str source_dir = ns_project_source_dir(root);
    ns_str source_value = ns_manifest_value(manifest, "source");
    ns_str project_relative = (source_value.data == ns_null || ns_str_equals(source_value, ns_str_cstr(".")))
                                  ? ns_str_cstr("") : source_value;
    ns_str project_type = ns_manifest_value(manifest, "type");
    ns_bool app_project = ns_str_equals(project_type, ns_str_cstr("app")) ||
                          ns_str_equals(project_type, ns_str_cstr("application"));
    // An app's manifest entry owns its own main and must not shadow the main
    // in a selected test. Library entries remain part of their testable API.
    ns_str project_entry = test_entry && app_project ? ns_manifest_entry_file_for_root(root) : ns_str_null;
    ns_project_source *sources = ns_null;
    ns_project_sources_scan(source_dir, project_relative, ns_str_cstr(""), excludes, &sources);
    qsort(sources, ns_array_length(sources), sizeof(ns_project_source), ns_project_source_cmp);

    ns_linker lk = {0};
    lk.scope = source_dir;
    lk.project_all = true;
    for (i32 i = 0, count = ns_array_length(sources); i < count; i++) {
        ns_project_source source = sources[i];
        ns_bool selected = ns_str_equals(source.path, entry_file);
        ns_bool app_entry = test_entry && !selected && ns_str_equals(source.path, project_entry);
        if (app_entry) continue;
        if (ns_project_source_is_test(source) && !(test_entry && selected)) continue;
        ns_str name = ns_project_module_name(source.path);
        if (!ns_name_in(lk.local_names, name)) ns_array_push(lk.local_names, name);
        else ns_str_free(name);
    }
    ns_str selected_name = ns_project_module_name(entry_file);
    if (!ns_name_in(lk.local_names, selected_name)) ns_array_push(lk.local_names, selected_name);
    else ns_str_free(selected_name);

    // Put the selected entry last so project declarations are available before
    // its executable top-level code, while preserving deterministic ordering.
    for (i32 i = 0, count = ns_array_length(sources); i < count; i++) {
        ns_project_source source = sources[i];
        ns_bool selected = ns_str_equals(source.path, entry_file);
        ns_bool app_entry = test_entry && !selected && ns_str_equals(source.path, project_entry);
        if (selected) continue;
        if (app_entry) continue;
        if (ns_project_source_is_test(source)) continue;
        ns_str text = ns_os_read_file(source.path);
        if (text.data != ns_null) ns_link_source(&lk, text, source.path);
        ns_str_free(text);
    }
    ns_link_source(&lk, entry_src, entry_file);

    for (i32 i = 0, count = ns_array_length(sources); i < count; i++) {
        // Source-map entries retain file names through parsing/evaluation.
        // Keep those path buffers alive when a map was requested.
        if (out_map == ns_null) ns_str_free(sources[i].path);
        ns_str_free(sources[i].relative);
    }
    for (i32 i = 0, count = ns_array_length(excludes); i < count; i++) ns_str_free(excludes[i]);
    ns_array_free(sources);
    ns_array_free(excludes);
    ns_str_free(manifest);
    ns_str_free(manifest_path);
    ns_str_free(source_value);
    ns_str_free(project_type);
    ns_str_free(project_entry);
    return ns_link_finish(&lk, out_map, out_external_modules);
}

// What a manifest declares for one selected target: its entry plus the fields
// a build or a run reads, each already resolved against the top-level value it
// inherits when the target leaves it out.
typedef struct ns_manifest_selection {
    ns_bool has_target; // a [[targets]] table was selected
    ns_str target_name; // selected target name, empty without targets
    ns_str entry_file;  // entry source path, joined onto the `source` dir
    ns_str name;        // artifact and display name
    ns_str type;        // app | library
    ns_str platform;    // wasm, or empty for a native target
    ns_str icon;        // resolved against the project root
    ns_str shell;       // resolved against the project root
    ns_bool link;       // `ns run` builds and launches instead of evaluating
} ns_manifest_selection;

static ns_str ns_manifest_read(ns_str root) {
    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str mod = ns_os_read_file(manifest);
    if (mod.data == ns_null)
        ns_exit(1, "ns", "cannot read manifest %.*s.\n", manifest.len, manifest.data);
    ns_str_free(manifest);
    return mod;
}

// Resolve what `root/ns.mod` declares for `target_name`. An empty name selects
// the default target, or the top-level `entry` when no `[[targets]]` table is
// declared. An unknown name is a usage error that lists what is declared.
static ns_manifest_selection ns_manifest_select(ns_str root, ns_str target_name) {
    ns_str mod = ns_manifest_read(root);
    ns_manifest_target *targets = ns_manifest_targets(mod);
    ns_manifest_selection sel = {0};
    ns_bool top_link = false;
    ns_manifest_bool(mod, "link", &top_link);

    i32 index = ns_manifest_target_index(targets, target_name);
    if (index < 0 && target_name.len > 0) {
        ns_str names = ns_manifest_target_names(targets);
        if (names.len == 0) {
            ns_exit(1, "ns", "ns.mod at %.*s declares no targets; `%.*s` is not a file either.\n",
                    root.len, root.data, target_name.len, target_name.data);
        }
        ns_exit(1, "ns", "unknown target `%.*s`; ns.mod at %.*s declares %.*s.\n",
                target_name.len, target_name.data, root.len, root.data, names.len, names.data);
    }

    ns_str entry = ns_str_null;
    if (index >= 0) {
        ns_manifest_target *target = &targets[index];
        sel.has_target = true;
        sel.target_name = ns_str_dup(target->name);
        entry = ns_str_dup(target->entry);
        sel.name = ns_str_dup(target->output.data != ns_null ? target->output : target->name);
        sel.type = ns_str_dup(target->type);
        sel.platform = ns_str_dup(target->platform);
        sel.icon = ns_path_resolve(root, target->icon);
        sel.shell = ns_path_resolve(root, target->shell);
        sel.link = target->has_link ? target->link : top_link;
        if (entry.data == ns_null || entry.len == 0) {
            ns_exit(1, "ns", "target `%.*s` of ns.mod at %.*s declares no `entry`.\n",
                    target->name.len, target->name.data, root.len, root.data);
        }
    } else {
        // No targets: the top-level entry. It stays optional here, so reading a
        // manifest for its other fields never depends on a declared entry.
        entry = ns_manifest_top_entry(mod);
        sel.link = top_link;
    }

    // Anything the selected target leaves out comes from the top-level key.
    if (sel.name.len == 0) { ns_str_free(sel.name); sel.name = ns_manifest_value(mod, "name"); }
    if (sel.type.len == 0) { ns_str_free(sel.type); sel.type = ns_manifest_value(mod, "type"); }
    if (sel.platform.len == 0) { ns_str_free(sel.platform); sel.platform = ns_manifest_value(mod, "target"); }
    if (sel.icon.data == ns_null) sel.icon = ns_path_resolve(root, ns_manifest_value(mod, "icon"));
    if (sel.shell.data == ns_null) sel.shell = ns_path_resolve(root, ns_manifest_value(mod, "shell"));

    if (entry.data != ns_null) {
        ns_str base = ns_manifest_source_base(root, mod);
        sel.entry_file = ns_path_join(base, entry);
        ns_str_free(base);
    }
    ns_str_free(entry);
    ns_manifest_targets_free(targets);
    ns_str_free(mod);
    return sel;
}

// True when `root/ns.mod` declares a `[[targets]]` table named `name`.
static ns_bool ns_manifest_has_target(ns_str root, ns_str name) {
    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str mod = ns_os_read_file(manifest);
    ns_str_free(manifest);
    if (mod.data == ns_null) return false;
    ns_manifest_target *targets = ns_manifest_targets(mod);
    ns_bool found = false;
    for (i32 i = 0, count = ns_array_length(targets); i < count && !found; i++) {
        found = ns_str_equals(targets[i].name, name);
    }
    ns_manifest_targets_free(targets);
    ns_str_free(mod);
    return found;
}

// Every `[[targets]]` name declared by `root/ns.mod`, in declaration order.
static ns_str *ns_manifest_target_list(ns_str root) {
    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str mod = ns_os_read_file(manifest);
    ns_str_free(manifest);
    if (mod.data == ns_null) return ns_null;

    ns_manifest_target *targets = ns_manifest_targets(mod);
    ns_str *names = ns_null;
    for (i32 i = 0, count = ns_array_length(targets); i < count; i++) {
        if (targets[i].name.data != ns_null) ns_array_push(names, ns_str_dup(targets[i].name));
    }
    ns_manifest_targets_free(targets);
    ns_str_free(mod);
    return names;
}

// The target that owns `entry_file`, so an explicit entry path selects the same
// target the manifest declares for it. Empty when no declared target matches.
static ns_str ns_manifest_target_for_entry(ns_str root, ns_str entry_file) {
    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str mod = ns_os_read_file(manifest);
    ns_str_free(manifest);
    if (mod.data == ns_null) return ns_str_null;

    ns_manifest_target *targets = ns_manifest_targets(mod);
    ns_str base = ns_manifest_source_base(root, mod);
    ns_str name = ns_str_null;
    for (i32 i = 0, count = ns_array_length(targets); i < count && name.data == ns_null; i++) {
        if (targets[i].entry.data == ns_null) continue;
        ns_str path = ns_path_join(base, targets[i].entry);
        if (ns_manifest_entry_matches(path, entry_file, targets[i].entry)) name = ns_str_dup(targets[i].name);
        ns_str_free(path);
    }
    ns_str_free(base);
    ns_manifest_targets_free(targets);
    ns_str_free(mod);
    return name;
}

// Resolve the entry source file declared by `root/ns.mod`, relative to its
// `source` dir. Supports `entry = "..."`, `entries = ["...", ...]`, and the
// default `[[targets]]` table.
static ns_str ns_manifest_entry_file_for_root(ns_str root) {
    ns_str entry = ns_manifest_select(root, ns_str_null).entry_file;
    if (entry.data == ns_null)
        ns_exit(1, "ns", "ns.mod at %.*s declares no `entry` to run.\n", root.len, root.data);
    return entry;
}

// Resolve the entry source file `ns run` should execute when no file is given.
// A manifest in the current directory takes precedence over main.ns. Unlike
// project/build discovery, bare `ns run` deliberately does not walk upward:
// its implicit input is always selected from the current directory.
static ns_str ns_default_run_entry(void) {
    ns_str cwd = ns_getcwd();
    ns_str manifest = ns_path_join(cwd, ns_str_cstr("ns.mod"));
    if (ns_file_exists(manifest)) return ns_manifest_entry_file_for_root(cwd);

    ns_str main = ns_path_join(cwd, ns_str_cstr("main.ns"));
    if (ns_file_exists(main)) return main;

    ns_exit(1, "ns", "cannot run %.*s: neither ns.mod nor main.ns was found.\n",
            cwd.len, cwd.data);
    return ns_str_null;
}

typedef enum {
    NS_BUILD_AUTO = 0,
    NS_BUILD_EXE = 1,
    NS_BUILD_LIB = 2,
    NS_BUILD_APP = 3,
} ns_build_kind;

static ns_bool ns_build_target_is_wasm(ns_str target);
static ns_str ns_path_last_component(ns_str path);

typedef struct ns_build_input {
    ns_str filename;
    ns_str source;
    ns_str scope;
    ns_str name;
    ns_str module_type;
    ns_str target;
    ns_str icon;
    ns_str shell;
    ns_str target_name; // selected [[targets]] name, empty when none
    ns_bool has_manifest;
} ns_build_input;

static ns_bool ns_find_project_root(ns_str path, ns_str *out) {
    ns_str dir = ns_is_dir(path) ? path : ns_path_dirname_safe(path);
    while (true) {
        ns_str manifest = ns_path_join(dir, ns_str_cstr("ns.mod"));
        ns_bool found = ns_file_exists(manifest);
        ns_str_free(manifest);
        if (found) {
            if (out) *out = dir;
            return true;
        }

        ns_str parent = ns_path_parent(dir);
        if (ns_str_equals(parent, dir)) break;
        dir = parent;
    }
    return false;
}

static ns_str ns_build_manifest_value(ns_str root, const char *key) {
    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str mod = ns_os_read_file(manifest);
    if (mod.data == ns_null) return ns_str_null;
    return ns_manifest_value(mod, key);
}

// A bare word (no path separator, no `.ns` suffix) that names a `[[targets]]`
// table of the nearest project selects that target for `ns run` and
// `ns build`. Anything shaped like a path stays a path, so `./web` and
// `web.ns` still name files even when a target `web` is declared.
static ns_bool ns_target_arg_select(ns_str arg, ns_str *out_root, ns_str *out_target) {
    if (arg.len == 0) return false;
    for (i32 i = 0; i < arg.len; i++) {
        if (arg.data[i] == '/' || arg.data[i] == '\\') return false;
    }
    if (ns_str_has_suffix(arg, ".ns")) return false;

    ns_str root = ns_str_null;
    if (!ns_find_project_root(ns_getcwd(), &root)) return false;
    if (!ns_manifest_has_target(root, arg)) return false;
    *out_root = root;
    *out_target = arg;
    return true;
}

static ns_str ns_build_default_name(ns_build_input *in) {
    if (in->has_manifest) {
        ns_str name = ns_build_manifest_value(in->scope, "name");
        if (name.data != ns_null) return name;
    }
    return ns_path_filename(in->filename);
}

static ns_str ns_build_default_output(ns_build_input *in, ns_build_kind kind) {
    ns_str name = in->name.data != ns_null ? in->name : ns_build_default_name(in);
    ns_str artifact;
    if (ns_build_target_is_wasm(in->target)) {
        ns_str safe = ns_project_safe_name(name);
        artifact = ns_str_concat(safe, ns_str_cstr(".wasm"));
    } else if (kind == NS_BUILD_LIB) {
        artifact = ns_str_concat(ns_str_cstr("lib"), name);
        artifact = ns_str_concat(artifact, ns_str_cstr(".a"));
    } else if (kind == NS_BUILD_APP) {
#if defined(NS_DARWIN)
        artifact = ns_str_concat(name, ns_str_cstr(".app"));
#else
        artifact = ns_str_concat(name, ns_str_cstr(""));
#if defined(_WIN32)
        artifact = ns_str_concat(artifact, ns_str_cstr(".exe"));
#endif
#endif
    } else {
        artifact = ns_str_concat(name, ns_str_cstr(""));
#if defined(_WIN32)
        artifact = ns_str_concat(artifact, ns_str_cstr(".exe"));
#endif
    }

    ns_str bin = ns_path_join(in->scope, ns_str_cstr("bin"));
    return ns_path_join(bin, artifact);
}

static ns_str ns_wasm_safe_name(ns_str name) {
    ns_str safe = ns_str_null;
    for (i32 i = 0; i < name.len; ++i) {
        i8 c = name.data[i];
        ns_bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        i8 value = ok ? c : '-';
        ns_str_append_len(&safe, &value, 1);
    }
    if (safe.len == 0) ns_str_append_len(&safe, "app", 3);
    ns_array_push(safe.data, '\0');
    return safe;
}

static ns_str ns_path_basename_with_extension(ns_str path) {
    i32 start = 0;
    for (i32 i = 0; i < path.len; ++i) {
        if (path.data[i] == '/' || path.data[i] == '\\') start = i + 1;
    }
    return ns_str_slice(path, start, path.len);
}

static void ns_mkdir_one(ns_str dir) {
    if (dir.len == 0) return;
#if defined(_WIN32)
    CreateDirectoryA(dir.data, NULL);
#else
    mkdir(dir.data, 0755);
#endif
}

static void ns_mkdir_p(ns_str dir) {
    if (dir.len == 0 || ns_str_equals(dir, ns_str_cstr("."))) return;
    ns_str tmp = ns_str_concat(dir, ns_str_cstr(""));
    for (i32 i = 1; i < tmp.len; i++) {
        if (tmp.data[i] == NS_PATH_SEPARATOR) {
            tmp.data[i] = '\0';
            ns_mkdir_one(ns_str_cstr(tmp.data));
            tmp.data[i] = NS_PATH_SEPARATOR;
        }
    }
    ns_mkdir_one(tmp);
    ns_str_free(tmp);
}

static void ns_build_ensure_output_dir(ns_str output) {
    ns_str dir = ns_path_dirname_safe(output);
    ns_mkdir_p(dir);
    ns_str_free(dir);
}

static void ns_write_text_file(ns_str path, ns_str text) {
    FILE *file = fopen(path.data, "wb");
    if (!file) ns_exit(1, "build", "failed to write %.*s.\n", path.len, path.data);
    if (text.len > 0 && fwrite(text.data, 1, text.len, file) != (size_t)text.len) {
        fclose(file);
        ns_exit(1, "build", "failed to write %.*s.\n", path.len, path.data);
    }
    fclose(file);
}

static void ns_copy_file_contents(ns_str src, ns_str dst) {
    ns_str data = ns_os_read_file(src);
    if (data.data == ns_null) ns_exit(1, "build", "failed to read %.*s.\n", src.len, src.data);
    ns_build_ensure_output_dir(dst);
    FILE *file = fopen(dst.data, "wb");
    if (!file || (data.len > 0 && fwrite(data.data, 1, data.len, file) != (size_t)data.len)) {
        if (file) fclose(file);
        ns_str_free(data);
        ns_exit(1, "build", "failed to copy %.*s to %.*s.\n", src.len, src.data, dst.len, dst.data);
    }
    fclose(file);
    ns_str_free(data);
}

static void ns_copy_tree(ns_str src, ns_str dst) {
    if (!ns_is_dir(src)) return;
    ns_mkdir_p(dst);
#if defined(_WIN32)
    ns_str pattern = ns_path_join(src, ns_str_cstr("*"));
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.data, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        ns_str from = ns_path_join(src, ns_str_cstr(fd.cFileName));
        ns_str to = ns_path_join(dst, ns_str_cstr(fd.cFileName));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ns_copy_tree(from, to);
        else ns_copy_file_contents(from, to);
        ns_str_free(from); ns_str_free(to);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(src.data);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != ns_null) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        ns_str from = ns_path_join(src, ns_str_cstr(entry->d_name));
        ns_str to = ns_path_join(dst, ns_str_cstr(entry->d_name));
        if (ns_is_dir(from)) ns_copy_tree(from, to);
        else ns_copy_file_contents(from, to);
        ns_str_free(from); ns_str_free(to);
    }
    closedir(dir);
#endif
}

static void ns_remove_tree(ns_str path) {
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path.data);
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ns_str pattern = ns_path_join(path, ns_str_cstr("*"));
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.data, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                ns_str child = ns_path_join(path, ns_str_cstr(fd.cFileName));
                ns_remove_tree(child);
                ns_str_free(child);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        ns_str_free(pattern);
        RemoveDirectoryA(path.data);
    } else {
        DeleteFileA(path.data);
    }
#else
    struct stat st;
    if (lstat(path.data, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path.data);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != ns_null) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                ns_str child = ns_path_join(path, ns_str_cstr(entry->d_name));
                ns_remove_tree(child);
                ns_str_free(child);
            }
            closedir(dir);
        }
        rmdir(path.data);
    } else {
        remove(path.data);
    }
#endif
}

static void ns_str_append_cstr(ns_str *s, const char *cstr) {
    ns_str_append_len(s, cstr, (i32)strlen(cstr));
}

static ns_str ns_html_escape(ns_str s) {
    ns_str out = ns_str_null;
    for (i32 i = 0; i < s.len; i++) {
        switch (s.data[i]) {
            case '&': ns_str_append_len(&out, "&amp;", 5); break;
            case '<': ns_str_append_len(&out, "&lt;", 4); break;
            case '>': ns_str_append_len(&out, "&gt;", 4); break;
            case '"': ns_str_append_len(&out, "&quot;", 6); break;
            default: ns_str_append_len(&out, s.data + i, 1); break;
        }
    }
    ns_array_push(out.data, '\0');
    return out;
}

#if defined(NS_DARWIN)
static ns_bool ns_str_ends_with(ns_str s, ns_str suffix) {
    return s.len >= suffix.len && strncmp(s.data + s.len - suffix.len, suffix.data, suffix.len) == 0;
}

static ns_str ns_build_app_output(ns_str output) {
    if (ns_str_ends_with(output, ns_str_cstr(".app"))) return output;
    return ns_str_concat(output, ns_str_cstr(".app"));
}

static void ns_copy_file_or_exit(ns_str src, ns_str dst) {
    ns_str data = ns_os_read_file(src);
    if (data.data == ns_null) ns_exit(1, "build", "failed to read icon %.*s.\n", src.len, src.data);

    FILE *file = fopen(dst.data, "wb");
    if (!file) ns_exit(1, "build", "failed to copy icon to %.*s.\n", dst.len, dst.data);
    if (data.len > 0 && fwrite(data.data, 1, data.len, file) != (size_t)data.len) {
        fclose(file);
        ns_exit(1, "build", "failed to copy icon to %.*s.\n", dst.len, dst.data);
    }
    fclose(file);
    ns_str_free(data);
}

static ns_str ns_xml_escape(ns_str s) {
    ns_str out = ns_str_null;
    for (i32 i = 0; i < s.len; i++) {
        switch (s.data[i]) {
            case '&': ns_str_append_len(&out, "&amp;", 5); break;
            case '<': ns_str_append_len(&out, "&lt;", 4); break;
            case '>': ns_str_append_len(&out, "&gt;", 4); break;
            case '"': ns_str_append_len(&out, "&quot;", 6); break;
            default: ns_str_append_len(&out, s.data + i, 1); break;
        }
    }
    ns_array_push(out.data, '\0');
    return out;
}
#endif

static ns_str ns_shell_quote(ns_str s) {
    ns_str q = ns_str_null;
    ns_str_append_len(&q, "'", 1);
    for (i32 i = 0; i < s.len; i++) {
        if (s.data[i] == '\'') {
            ns_str_append_len(&q, "'\\''", 4);
        } else {
            ns_str_append_len(&q, s.data + i, 1);
        }
    }
    ns_str_append_len(&q, "'", 1);
    ns_array_push(q.data, '\0');
    return q;
}

static void ns_archive_object(ns_str output, ns_str object) {
    ns_str q_output = ns_shell_quote(output);
    ns_str q_object = ns_shell_quote(object);
    ns_str cmd = ns_str_null;
    ns_str_append_len(&cmd, "ar rcs ", 7);
    ns_str_append_len(&cmd, q_output.data, q_output.len);
    ns_str_append_len(&cmd, " ", 1);
    ns_str_append_len(&cmd, q_object.data, q_object.len);
    ns_array_push(cmd.data, '\0');

    f64 archive_start = ns_build_profile_begin("archive_library");
    i32 ret = system(cmd.data);
    ns_build_profile_end("archive_library", archive_start);
    if (ret != 0) {
        ns_exit(1, "build", "failed to archive %.*s.\n", output.len, output.data);
    }

    remove(object.data);
}

static ns_ssa_module *ns_compile_source_to_ssa(ns_str source, ns_str filename, ns_line_loc *line_map) {
    ctx.line_map = line_map;
    f64 parse_start = ns_build_profile_begin("parse");
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);
    ns_build_profile_end("parse", parse_start);

    f64 ssa_start = ns_build_profile_begin("lower_ssa");
    ns_return_ptr ssa_ret = ns_ssa_build_for_cli(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    ns_build_profile_end("lower_ssa", ssa_start);
    return ssa_ret.r;
}

// Installed declaration file of an external `use` module, resolved the same
// way the SSA builder resolves it. The path is returned even when the file is
// absent so that installing the module later invalidates a recorded build.
static ns_str ns_build_module_ref_path(ns_str name) {
    if (vm.ref_path.data == ns_null || vm.ref_path.len == 0) return ns_str_null;
    ns_str file = ns_str_concat(name, ns_str_cstr(".ns"));
    ns_str path = ns_path_join(vm.ref_path, file);
    ns_str_free(file);
    return path;
}

// Stamp what the linker read: every file that contributed a line, plus the
// declaration of each external module the merged unit still imports. Sibling
// modules pulled in by `use` are only known here, after linking.
static void ns_build_cache_add_linked(ns_build_cache *cache, ns_line_loc *map, ns_str *external) {
    ns_str previous = ns_str_null;
    for (i32 i = 0, l = (i32)ns_array_length(map); i < l; i++) {
        // Lines of one file are contiguous, so this skips the repeat lookups.
        if (previous.data != ns_null && ns_str_equals(previous, map[i].f)) continue;
        previous = map[i].f;
        ns_build_cache_add(cache, map[i].f);
    }
    for (i32 i = 0, l = (i32)ns_array_length(external); i < l; i++) {
        ns_str path = ns_build_module_ref_path(external[i]);
        ns_build_cache_add(cache, path);
        ns_str_free(path);
    }
}

static ns_ssa_module *ns_compile_build_input(ns_build_input *in, ns_build_cache *cache) {
    ns_line_loc *map = ns_null;
    ns_str *external = ns_null;
    f64 link_start = ns_build_profile_begin("link_sources");
    ns_str merged = in->has_manifest
                        ? ns_project_link_all(in->scope, in->source, in->filename, false, &map, &external)
                        : ns_project_link(in->scope, in->source, in->filename, &map, &external);
    ns_build_profile_end("link_sources", link_start);

    f64 dependency_start = ns_build_profile_begin("track_dependencies");
    if (cache != ns_null) ns_build_cache_add_linked(cache, map, external);
    for (i32 i = 0, l = (i32)ns_array_length(external); i < l; i++) ns_str_free(external[i]);
    ns_array_free(external);
    ns_build_profile_end("track_dependencies", dependency_start);
    return ns_compile_source_to_ssa(merged, in->filename, map);
}

// Resolve what a build compiles. `target_name` selects one `[[targets]]` table
// of the manifest; an empty name uses the default target, or the top-level
// `entry` of a manifest that declares no targets. An explicit file path keeps
// its own entry, but still inherits the manifest fields of its project.
static ns_build_input ns_build_input_resolve(ns_str path, ns_str target_name) {
    ns_build_input in = {0};
    ns_bool manifest_entry = false;

    if (path.len == 0) {
        ns_str cwd = ns_getcwd();
        in.scope = ns_project_root(cwd);
        in.has_manifest = true;
        manifest_entry = true;
    } else if (ns_is_dir(path)) {
        in.scope = ns_project_root(path);
        in.has_manifest = true;
        manifest_entry = true;
    } else {
        in.filename = path;
        if (!ns_find_project_root(path, &in.scope)) in.scope = ns_path_dirname_safe(path);
        in.has_manifest = ns_file_exists(ns_path_join(in.scope, ns_str_cstr("ns.mod")));
        // An explicit path to a declared entry builds that target.
        if (in.has_manifest && target_name.len == 0) {
            target_name = ns_manifest_target_for_entry(in.scope, path);
        }
    }

    if (in.has_manifest) {
        ns_manifest_selection sel = ns_manifest_select(in.scope, target_name);
        if (manifest_entry) {
            if (sel.entry_file.data == ns_null)
                ns_exit(1, "build", "ns.mod at %.*s declares no `entry` to build.\n",
                        in.scope.len, in.scope.data);
            in.filename = sel.entry_file;
        }
        in.name = sel.name;
        in.module_type = sel.type;
        in.target = sel.platform;
        in.icon = sel.icon;
        in.shell = sel.shell;
        in.target_name = sel.target_name;
    }

    in.source = ns_os_read_file(in.filename);
    if (in.source.data == ns_null || in.source.len == 0) {
        ns_exit(1, "build", "empty file or folder %.*s.\n", in.filename.len, in.filename.data);
    }
    if (in.name.data == ns_null) in.name = ns_path_filename(in.filename);
    return in;
}

static ns_str ns_project_current_executable(void);
static ns_bool ns_build_type_is_app(ns_str t);

// The file or bundle directory a build produces. `ns build` keeps it when
// every recorded input is unchanged, so the artifact must be checked by the
// same path the emitters write.
static ns_str ns_build_artifact_path(ns_build_kind kind, ns_str output) {
    if (kind == NS_BUILD_APP && !ns_str_has_suffix(output, ".app")) {
        return ns_str_concat(output, ns_str_cstr(".app"));
    }
    return ns_str_concat(output, ns_str_cstr(""));
}

// Everything besides the input files that decides what the artifact contains:
// the toolchain version, the artifact kind, the host and project targets, and
// the output path. A change here rebuilds even when no source changed.
static ns_str ns_build_config(ns_build_input *in, ns_build_kind kind, ns_str output) {
    ns_asm_target target;
    ns_asm_get_current_target(&target);
    ns_str arch = ns_arch_str(target.arch);
    ns_str os = ns_os_str(target.os);

    ns_str config = ns_str_null;
    ns_str_append_cstr(&config, "ns ");
    ns_str_append_i32(&config, (i32)VERSION_MAJOR);
    ns_str_append_cstr(&config, ".");
    ns_str_append_i32(&config, (i32)VERSION_MINOR);
    ns_str_append_cstr(&config, " kind ");
    ns_str_append_i32(&config, (i32)kind);
    ns_str_append_cstr(&config, " host ");
    ns_str_append(&config, arch);
    ns_str_append_cstr(&config, "-");
    ns_str_append(&config, os);
    ns_str_append_cstr(&config, " target ");
    ns_str_append(&config, in->target.len > 0 ? in->target : ns_str_cstr("native"));
    ns_str_append_cstr(&config, " output ");
    ns_str_append(&config, output);
    ns_array_push(config.data, '\0');
    return config;
}

// Stamp the inputs a build reads before it links: the running toolchain, the
// entry, the manifest and its source set, and the resources a bundle packages.
// Files the linker discovers are added afterwards by ns_compile_build_input,
// and remain guarded by the recorded stamps of the previous build.
static void ns_build_cache_collect(ns_build_cache *cache, ns_build_input *in) {
    // A rebuilt ns emits different code from the same sources.
    ns_str executable = ns_project_current_executable();
    ns_build_cache_add(cache, executable);
    ns_str_free(executable);

    ns_build_cache_add(cache, in->filename);
    if (!in->has_manifest) return;

    ns_str manifest = ns_path_join(in->scope, ns_str_cstr("ns.mod"));
    ns_build_cache_add(cache, manifest);
    ns_str_free(manifest);
    ns_build_cache_add(cache, in->icon);
    ns_build_cache_add(cache, in->shell);

    ns_project_source *sources = ns_project_sources(in->scope, in->filename);
    for (i32 i = 0, count = ns_array_length(sources); i < count; i++) {
        if (!ns_project_source_is_test(sources[i])) ns_build_cache_add(cache, sources[i].path);
    }
    ns_project_sources_free(sources);

    // Assets are copied into app and browser bundles, so they are inputs too.
    ns_str source_dir = ns_project_source_dir(in->scope);
    ns_str source_assets = ns_path_join(source_dir, ns_str_cstr("assets"));
    ns_str scope_assets = ns_path_join(in->scope, ns_str_cstr("assets"));
    ns_build_cache_add_tree(cache, source_assets);
    ns_build_cache_add_tree(cache, scope_assets);
    ns_str_free(source_assets);
    ns_str_free(scope_assets);
    ns_str_free(source_dir);
}

static ns_str ns_wasm_runtime_source(void) {
    ns_str executable = ns_project_current_executable();
    ns_str bin = ns_path_dirname_safe(executable);
    ns_str root = ns_path_parent(bin);
    ns_str source = ns_path_join(root, ns_str_cstr("lib/ns-wasm.js"));
    if (ns_file_exists(source)) return source;
    ns_str_free(source);
    source = ns_path_join(root, ns_str_cstr("ref/ns-wasm.js"));
    if (ns_file_exists(source)) return source;
    ns_exit(1, "build", "installed wasm middleware ns-wasm.js was not found.\n");
    return ns_str_null;
}

static ns_str ns_wasm_default_icon_source(void) {
    ns_str executable = ns_project_current_executable();
    ns_str bin = ns_path_dirname_safe(executable);
    ns_str root = ns_path_parent(bin);
    ns_str sample = ns_path_join(root, ns_str_cstr("sample"));
    ns_str source = ns_path_join(sample, ns_str_cstr("ns.svg"));
    if (ns_file_exists(source)) return source;
    ns_str_free(source);
    source = ns_path_join(root, ns_str_cstr("ref/ns.svg"));
    if (ns_file_exists(source)) return source;
    ns_exit(1, "build", "installed default wasm icon ns.svg was not found.\n");
    return ns_str_null;
}

static ns_str ns_wasm_favicon_filename(ns_str icon) {
    ns_str basename = ns_path_basename_with_extension(icon);
    i32 dot = -1;
    for (i32 i = 0; i < basename.len; ++i) {
        if (basename.data[i] == '.') dot = i;
    }
    ns_str filename = ns_str_null;
    ns_str_append_cstr(&filename, "favicon");
    if (dot >= 0) {
        ns_bool safe = true;
        for (i32 i = dot + 1; i < basename.len; ++i) {
            i8 c = basename.data[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                safe = false;
                break;
            }
        }
        if (safe && dot + 1 < basename.len) {
            ns_str_append_len(&filename, ".", 1);
            for (i32 i = dot + 1; i < basename.len; ++i) {
                i8 c = basename.data[i];
                if (c >= 'A' && c <= 'Z') c = (i8)(c - 'A' + 'a');
                ns_str_append_len(&filename, &c, 1);
            }
        }
    }
    ns_array_push(filename.data, '\0');
    return filename;
}

// Expand the three stable placeholders accepted by a custom Wasm HTML shell.
// Keeping this deliberately small makes the shell a static asset rather than
// introducing a template language into the project manifest.
static ns_str ns_wasm_shell_replace(ns_str source, const char *token, ns_str value) {
    ns_str out = ns_str_null;
    i32 token_len = (i32)strlen(token);
    for (i32 i = 0; i < source.len;) {
        if (i + token_len <= source.len && strncmp(source.data + i, token, (size_t)token_len) == 0) {
            ns_str_append_len(&out, value.data, value.len);
            i += token_len;
        } else {
            ns_str_append_len(&out, source.data + i, 1);
            i++;
        }
    }
    return out;
}

static ns_ssa_fn *ns_wasm_find_function(ns_ssa_module *ssa, const char *name) {
    for (i32 i = 0, l = (i32)ns_array_length(ssa->fns); i < l; ++i) {
        if (ns_str_equals(ssa->fns[i].name, ns_str_cstr((char *)name))) return &ssa->fns[i];
    }
    return ns_null;
}

static void ns_wasm_validate_browser_entry(ns_ssa_module *ssa) {
    ns_ssa_fn *main_fn = ns_wasm_find_function(ssa, "main");
    if (!main_fn) ns_exit(1, "build", "wasm app requires fn main() or fn main() i32.\n");
    if (ns_array_length(main_fn->params) != 0 ||
        (!ns_type_is(main_fn->ret, NS_TYPE_VOID) && !ns_type_is(main_fn->ret, NS_TYPE_I32))) {
        ns_exit(1, "build", "wasm main must have signature fn main() or fn main() i32.\n");
    }
    ns_ssa_fn *frame = ns_wasm_find_function(ssa, "frame");
    if (frame && (ns_array_length(frame->params) != 3 ||
                  !ns_type_is(frame->params[0], NS_TYPE_F64) ||
                  !ns_type_is(frame->params[1], NS_TYPE_I32) ||
                  !ns_type_is(frame->params[2], NS_TYPE_I32) ||
                  !ns_type_is(frame->ret, NS_TYPE_VOID))) {
        ns_exit(1, "build", "optional wasm frame must be fn frame(time_ms: f64, width: i32, height: i32).\n");
    }
}

static void ns_exec_build_wasm(ns_build_input *in, ns_str output, u8 requested_kind, ns_bool force) {
    if (!in->has_manifest) ns_exit(1, "build", "target = \"wasm\" requires ns.mod.\n");
    if (!ns_build_type_is_app(in->module_type) || requested_kind == NS_BUILD_LIB) {
        ns_exit(1, "build", "wasm v1 supports type = \"app\" only.\n");
    }
    ns_str safe = ns_wasm_safe_name(in->name);
    if (output.len == 0) {
        ns_str filename = ns_str_concat(safe, ns_str_cstr(".wasm"));
        output = ns_path_join(ns_path_join(in->scope, ns_str_cstr("bin")), filename);
    }
    ns_build_ensure_output_dir(output);

    // The module guards the whole bundle: everything else in bin/ is copied or
    // generated from the same inputs.
    f64 cache_start = ns_build_profile_begin("check_cache");
    ns_build_cache cache;
    ns_str config = ns_build_config(in, NS_BUILD_APP, output);
    ns_build_cache_open(&cache, output, config);
    ns_array_free(config.data);
    ns_build_cache_collect(&cache, in);
    ns_bool fresh = !force && ns_build_cache_fresh(&cache);
    ns_build_profile_end("check_cache", cache_start);
    if (fresh) {
        ns_str current = ns_path_dirname_safe(output);
        ns_info("build", "up to date %.*s\n", current.len, current.data);
        ns_build_cache_free(&cache);
        ns_str_free(current);
        return;
    }

    f64 compile_start = ns_build_profile_begin("compile");
    ns_ssa_module *ssa = ns_compile_build_input(in, &cache);
    ns_wasm_validate_browser_entry(ssa);
    ns_build_profile_end("compile", compile_start);

    ns_str artifact = ns_path_basename_with_extension(output);
    ns_str map_url = ns_str_concat(artifact, ns_str_cstr(".map"));
    ns_str map_output = ns_str_concat(output, ns_str_cstr(".map"));
    ns_str temporary = ns_str_concat(output, ns_str_cstr(".tmp"));
    ns_str map_temporary = ns_str_concat(map_output, ns_str_cstr(".tmp"));
    f64 emit_start = ns_build_profile_begin("emit_wasm");
    ns_return_bool emitted = ns_wasm_emit_source_mapped(ssa, temporary, map_temporary,
                                                        map_url, in->scope);
    ns_ssa_module_free(ssa);
    ns_build_profile_end("emit_wasm", emit_start);
    if (ns_return_is_error(emitted)) {
        remove(temporary.data);
        remove(map_temporary.data);
        ns_return_assert(emitted);
    }
    f64 package_start = ns_build_profile_begin("package_wasm");
#if defined(_WIN32)
    remove(output.data);
    remove(map_output.data);
#endif
    if (rename(temporary.data, output.data) != 0) {
        remove(temporary.data);
        remove(map_temporary.data);
        ns_exit(1, "build", "failed to replace %.*s.\n", output.len, output.data);
    }
    if (rename(map_temporary.data, map_output.data) != 0) {
        remove(map_temporary.data);
        ns_exit(1, "build", "failed to replace %.*s.\n", map_output.len, map_output.data);
    }

    ns_str out_dir = ns_path_dirname_safe(output);
    ns_str runtime_dst = ns_path_join(out_dir, ns_str_cstr("ns-wasm.js"));
    ns_str runtime_src = ns_wasm_runtime_source();
    ns_copy_file_contents(runtime_src, runtime_dst);
    ns_build_cache_add(&cache, runtime_src); // installed middleware, copied into the bundle

    ns_bool uses_default_icon = in->icon.data == ns_null || in->icon.len == 0;
    ns_str icon_src = uses_default_icon ? ns_wasm_default_icon_source() : in->icon;
    ns_str favicon = uses_default_icon ? ns_str_cstr("ns.svg") : ns_wasm_favicon_filename(icon_src);
    ns_copy_file_contents(icon_src, ns_path_join(out_dir, favicon));
    ns_build_cache_add(&cache, icon_src);

    ns_str title = ns_html_escape(in->name);
    ns_str html = ns_str_null;
    if (in->shell.data != ns_null && in->shell.len > 0) {
        ns_str custom = ns_os_read_file(in->shell);
        if (custom.data == ns_null) {
            ns_exit(1, "build", "wasm shell file not found: %.*s.\n", in->shell.len, in->shell.data);
        }
        ns_str expanded = ns_wasm_shell_replace(custom, "{{wasm}}", artifact);
        ns_str_free(custom);
        custom = ns_wasm_shell_replace(expanded, "{{title}}", title);
        ns_str_free(expanded);
        expanded = ns_wasm_shell_replace(custom, "{{favicon}}", favicon);
        ns_str_free(custom);
        html = expanded;
    } else {
        ns_str_append_cstr(&html, "<!doctype html>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n");
        ns_str_append_cstr(&html, "<title>");
        ns_str_append_len(&html, title.data, title.len);
        ns_str_append_cstr(&html, "</title>\n<link rel=\"icon\" href=\"./");
        ns_str_append_len(&html, favicon.data, favicon.len);
        ns_str_append_cstr(&html, "\">\n<style>html,body,canvas{width:100%;height:100%;margin:0;display:block;background:#111}canvas{outline:none}</style>\n");
        ns_str_append_cstr(&html, "<canvas id=\"ns-canvas\"></canvas>\n<script type=\"module\">import { boot } from './ns-wasm.js'; boot('./");
        ns_str_append_len(&html, artifact.data, artifact.len);
        ns_str_append_cstr(&html, "');</script>\n");
    }
    ns_write_text_file(ns_path_join(out_dir, ns_str_cstr("index.html")), html);

    ns_str source_dir = ns_project_source_dir(in->scope);
    ns_str assets = ns_path_join(source_dir, ns_str_cstr("assets"));
    ns_str output_assets = ns_path_join(out_dir, ns_str_cstr("assets"));
    if (!ns_str_equals(assets, output_assets)) {
        ns_remove_tree(output_assets);
        ns_copy_tree(assets, output_assets);
    }
    ns_build_profile_end("package_wasm", package_start);
    ns_info("build", "wasm bundle %.*s\n", out_dir.len, out_dir.data);
    f64 write_cache_start = ns_build_profile_begin("write_cache");
    ns_build_cache_write(&cache);
    ns_build_profile_end("write_cache", write_cache_start);
    ns_build_cache_free(&cache);
}

static ns_bool ns_build_type_is_library(ns_str t) {
    return ns_str_equals(t, ns_str_cstr("library")) || ns_str_equals(t, ns_str_cstr("lib"));
}

static ns_bool ns_build_type_is_app(ns_str t) {
    return ns_str_equals(t, ns_str_cstr("app")) || ns_str_equals(t, ns_str_cstr("application"));
}

// A command-line program: a plain executable, never a host app bundle.
static ns_bool ns_build_type_is_cli(ns_str t) {
    return ns_str_equals(t, ns_str_cstr("cli")) || ns_str_equals(t, ns_str_cstr("exe")) ||
           ns_str_equals(t, ns_str_cstr("executable"));
}

static ns_bool ns_build_target_is_wasm(ns_str target) {
    return ns_str_equals(target, ns_str_cstr("wasm"));
}

static ns_build_kind ns_build_resolve_kind(ns_build_input *in, u8 requested) {
    if (requested == NS_BUILD_EXE) return NS_BUILD_EXE;
    if (requested == NS_BUILD_LIB) return NS_BUILD_LIB;
    if (requested == NS_BUILD_APP) return NS_BUILD_APP;
    if (ns_build_type_is_library(in->module_type)) return NS_BUILD_LIB;
    if (ns_build_type_is_app(in->module_type)) return NS_BUILD_APP;
    if (ns_build_type_is_cli(in->module_type)) return NS_BUILD_EXE;
    return NS_BUILD_EXE;
}

#if defined(NS_DARWIN)
static ns_bool ns_build_darwin_make_icns(ns_str icon, ns_str resources_dir, ns_str *out_icon_file) {
    if (icon.data == ns_null) return false;
    if (!ns_file_exists(icon)) ns_exit(1, "build", "icon file not found: %.*s.\n", icon.len, icon.data);

    ns_str icon_file = ns_str_cstr("AppIcon.icns");
    ns_str icns_path = ns_path_join(resources_dir, icon_file);
    if (ns_str_ends_with(icon, ns_str_cstr(".icns"))) {
        ns_copy_file_or_exit(icon, icns_path);
        *out_icon_file = icon_file;
        return true;
    }

    ns_str iconset_dir = ns_path_join(resources_dir, ns_str_cstr("AppIcon.iconset"));
    ns_mkdir_p(iconset_dir);

    const char *names[] = {
        "icon_16x16.png", "icon_16x16@2x.png",
        "icon_32x32.png", "icon_32x32@2x.png",
        "icon_128x128.png", "icon_128x128@2x.png",
        "icon_256x256.png", "icon_256x256@2x.png",
        "icon_512x512.png", "icon_512x512@2x.png",
    };
    const i32 pixels[] = {16, 32, 32, 64, 128, 256, 256, 512, 512, 1024};

    ns_str q_icon = ns_shell_quote(icon);
    for (i32 i = 0; i < 10; i++) {
        ns_str out = ns_path_join(iconset_dir, ns_str_cstr((char*)names[i]));
        ns_str q_out = ns_shell_quote(out);
        ns_str px = ns_str_from_i32(pixels[i]);
        ns_str cmd = ns_str_null;
        ns_str_append_cstr(&cmd, "/usr/bin/sips -z ");
        ns_str_append(&cmd, px);
        ns_str_append_len(&cmd, " ", 1);
        ns_str_append(&cmd, px);
        ns_str_append_len(&cmd, " ", 1);
        ns_str_append(&cmd, q_icon);
        ns_str_append_len(&cmd, " --out ", 7);
        ns_str_append(&cmd, q_out);
        ns_str_append_cstr(&cmd, " >/dev/null");
        ns_array_push(cmd.data, '\0');
        i32 ret = system(cmd.data);
        if (ret != 0) ns_exit(1, "build", "failed to resize icon %.*s.\n", icon.len, icon.data);
        ns_str_free(out);
        ns_str_free(q_out);
        ns_str_free(cmd);
    }
    ns_str_free(q_icon);

    ns_str q_iconset = ns_shell_quote(iconset_dir);
    ns_str q_icns = ns_shell_quote(icns_path);
    ns_str iconutil = ns_str_null;
    ns_str_append_cstr(&iconutil, "/usr/bin/iconutil -c icns ");
    ns_str_append(&iconutil, q_iconset);
    ns_str_append_len(&iconutil, " -o ", 4);
    ns_str_append(&iconutil, q_icns);
    ns_array_push(iconutil.data, '\0');
    i32 iconutil_ret = system(iconutil.data);
    if (iconutil_ret != 0) ns_exit(1, "build", "failed to create icon %.*s.\n", icns_path.len, icns_path.data);

    ns_str rm = ns_str_null;
    ns_str_append_len(&rm, "rm -rf ", 7);
    ns_str_append(&rm, q_iconset);
    ns_array_push(rm.data, '\0');
    system(rm.data);

    ns_str_free(iconset_dir);
    ns_str_free(q_iconset);
    ns_str_free(q_icns);
    ns_str_free(iconutil);
    ns_str_free(rm);
    *out_icon_file = icon_file;
    return true;
}

static void ns_build_darwin_write_plist(ns_build_input *in, ns_str app_dir, ns_str executable_name, ns_str icon_file) {
    ns_str contents_dir = ns_path_join(app_dir, ns_str_cstr("Contents"));
    ns_str plist_path = ns_path_join(contents_dir, ns_str_cstr("Info.plist"));
    ns_str name = ns_xml_escape(in->name);
    ns_str executable = ns_xml_escape(executable_name);
    ns_str identifier_src = ns_str_concat(ns_str_cstr("ns."), in->name);
    ns_str identifier = ns_xml_escape(identifier_src);
    ns_str icon = icon_file.data != ns_null ? ns_xml_escape(icon_file) : ns_str_null;

    ns_str plist = ns_str_null;
    ns_str_append_cstr(&plist, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    ns_str_append_cstr(&plist, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
    ns_str_append_cstr(&plist, "<plist version=\"1.0\">\n<dict>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundleName</key>\n  <string>");
    ns_str_append(&plist, name);
    ns_str_append_cstr(&plist, "</string>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundleDisplayName</key>\n  <string>");
    ns_str_append(&plist, name);
    ns_str_append_cstr(&plist, "</string>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundleExecutable</key>\n  <string>");
    ns_str_append(&plist, executable);
    ns_str_append_cstr(&plist, "</string>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundleIdentifier</key>\n  <string>");
    ns_str_append(&plist, identifier);
    ns_str_append_cstr(&plist, "</string>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundlePackageType</key>\n  <string>APPL</string>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundleVersion</key>\n  <string>1</string>\n");
    ns_str_append_cstr(&plist, "  <key>CFBundleShortVersionString</key>\n  <string>1.0</string>\n");
    if (icon_file.data != ns_null) {
        ns_str_append_cstr(&plist, "  <key>CFBundleIconFile</key>\n  <string>");
        ns_str_append(&plist, icon);
        ns_str_append_cstr(&plist, "</string>\n");
    }
    ns_str_append_cstr(&plist, "</dict>\n</plist>\n");
    ns_array_push(plist.data, '\0');
    ns_write_text_file(plist_path, plist);

    ns_str_free(contents_dir);
    ns_str_free(plist_path);
    ns_str_free(name);
    ns_str_free(executable);
    ns_str_free(identifier_src);
    ns_str_free(identifier);
    ns_str_free(icon);
    ns_str_free(plist);
}

static void ns_build_darwin_codesign_app(ns_str app_dir) {
    ns_str q_app_dir = ns_shell_quote(app_dir);
    ns_str cmd = ns_str_null;
    ns_str_append_cstr(&cmd, "/usr/bin/codesign --force --sign - --timestamp=none ");
    ns_str_append(&cmd, q_app_dir);
    ns_array_push(cmd.data, '\0');

    i32 ret = system(cmd.data);
    if (ret != 0) {
        ns_exit(1, "build", "failed to codesign app %.*s.\n", app_dir.len, app_dir.data);
    }

    ns_str_free(q_app_dir);
    ns_str_free(cmd);
}

static ns_str ns_native_rt_c_path(void) {
    ns_str exe = ns_project_current_executable();
    if (exe.data == ns_null) return ns_str_null;
    ns_str bin = ns_path_dirname_safe(exe);
    ns_str root = ns_path_parent(bin);
    ns_str src = ns_path_join(root, ns_str_cstr("src/ns_native_rt.c"));
    if (ns_file_exists(src)) return src;
    ns_str_free(src);
    return ns_path_join(root, ns_str_cstr("share/ns-runtime/src/ns_native_rt.c"));
}

static void ns_build_write_strtab_c(ns_ssa_module *ssa, ns_str path) {
    FILE *f = fopen(path.data, "w");
    if (!f) ns_exit(1, "build", "failed to write runtime string table %.*s.\n", path.len, path.data);
    ns_str *tab = ns_null;
    for (i32 fi = 0, fl = (i32)ns_array_length(ssa->fns); fi < fl; ++fi) {
        ns_ssa_fn *fn = &ssa->fns[fi];
        for (i32 ii = 0, il = (i32)ns_array_length(fn->insts); ii < il; ++ii) {
            ns_ssa_inst *inst = &fn->insts[ii];
            if (inst->op != NS_SSA_OP_CONST || !ns_type_is(inst->type, NS_TYPE_STRING)) continue;
            ns_str value = ns_str_unescape(inst->name);
            ns_bool seen = false;
            for (i32 s = 0, sl = (i32)ns_array_length(tab); s < sl; ++s) {
                if (ns_str_equals(tab[s], value)) { seen = true; break; }
            }
            if (!seen) ns_array_push(tab, value);
            else ns_str_free(value);
        }
    }
    i32 n = (i32)ns_array_length(tab);
    fprintf(f, "void ns_rt_set_strtab(const char **tab, const int *lens, int n);\n");
    fprintf(f, "static const char *ns_rt_strtab_data[] = {\n");
    if (n == 0) fprintf(f, "    \"\"\n");
    for (i32 i = 0; i < n; ++i) {
        fprintf(f, "    \"");
        for (i32 k = 0; k < tab[i].len; ++k) {
            unsigned char ch = (unsigned char)tab[i].data[k];
            if (ch == '\\' || ch == '"') fprintf(f, "\\%c", ch);
            else if (ch == '\n') fputs("\\n", f);
            else if (ch == '\t') fputs("\\t", f);
            else if (ch < 32 || ch > 126) fprintf(f, "\\x%02x", ch);
            else fputc(ch, f);
        }
        fprintf(f, "\",\n");
    }
    fprintf(f, "};\nstatic const int ns_rt_strlen_data[] = {");
    if (n == 0) fprintf(f, "0");
    for (i32 i = 0; i < n; ++i) fprintf(f, "%s%d", i ? ", " : "", tab[i].len);
    fprintf(f, "};\n");
    fprintf(f, "static void ns_rt_strtab_ctor(void) __attribute__((constructor));\n");
    fprintf(f, "static void ns_rt_strtab_ctor(void) { ns_rt_set_strtab(ns_rt_strtab_data, ns_rt_strlen_data, %d); }\n", n);
    fclose(f);
    for (i32 i = 0; i < n; ++i) ns_str_free(tab[i]);
    ns_array_free(tab);
}

static void ns_build_darwin_link_executable(ns_ssa_module *ssa, ns_str executable_path) {
    ns_str object_path = ns_str_concat(executable_path, ns_str_cstr(".o"));
    f64 object_start = ns_build_profile_begin("emit_object");
    ns_return_bool emit_ret = ns_macho_emit_object(ssa, object_path);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);
    ns_build_profile_end("emit_object", object_start);

    f64 runtime_start = ns_build_profile_begin("prepare_native_runtime");
    ns_str rt_path = ns_native_rt_c_path();
    if (rt_path.data == ns_null || !ns_file_exists(rt_path)) {
        ns_exit(1, "build", "cannot find ns_native_rt.c next to the ns toolchain.\n");
    }
    ns_str strtab_path = ns_str_concat(executable_path, ns_str_cstr(".strtab.c"));
    ns_build_write_strtab_c(ssa, strtab_path);

    ns_str rt_dir = ns_path_dirname_safe(rt_path);
    ns_str inc_dir = ns_path_join(ns_path_parent(rt_dir), ns_str_cstr("include"));
    ns_str q_object = ns_shell_quote(object_path);
    ns_str q_executable = ns_shell_quote(executable_path);
    ns_str q_rt = ns_shell_quote(rt_path);
    ns_str q_strtab = ns_shell_quote(strtab_path);
    ns_str q_inc = ns_shell_quote(inc_dir);
    ns_str cmd = ns_str_null;
    ns_str_append_cstr(&cmd, "/usr/bin/clang -I");
    ns_str_append(&cmd, q_inc);
    ns_str_append_cstr(&cmd, " ");
    ns_str_append(&cmd, q_object);
    ns_str_append_cstr(&cmd, " ");
    ns_str_append(&cmd, q_rt);
    ns_str_append_cstr(&cmd, " ");
    ns_str_append(&cmd, q_strtab);

    ns_str exe = ns_project_current_executable();
    ns_str bin = ns_path_dirname_safe(exe);
    ns_str root = ns_path_parent(bin);
    ns_str lib_dirs[3];
    i32 nlib_dirs = 0;
    ns_str installed_lib = ns_path_join(root, ns_str_cstr("lib"));
    ns_str home = ns_path_home();
    ns_str home_lib = ns_path_join(home, ns_str_cstr("ns/lib"));
    lib_dirs[nlib_dirs++] = bin;
    lib_dirs[nlib_dirs++] = installed_lib;
    lib_dirs[nlib_dirs++] = home_lib;
    ns_str *linked = ns_null;
    ns_str rpath_dir = ns_str_null;
    for (i32 i = 0, l = (i32)ns_array_length(ssa->imports); i < l; ++i) {
        ns_str module = ssa->imports[i].module;
        if (module.len == 0) continue;
        if (ns_str_equals(module, ns_str_cstr("std")) ||
            ns_str_equals(module, ns_str_cstr("task")) ||
            ns_str_equals(module, ns_str_cstr("simd")) ||
            ns_str_equals(module, ns_str_cstr("shader"))) continue;
        ns_bool already = false;
        for (i32 k = 0, kl = (i32)ns_array_length(linked); k < kl; ++k) {
            if (ns_str_equals(linked[k], module)) { already = true; break; }
        }
        if (already) continue;
        ns_str dylib = ns_str_null;
        for (i32 d = 0; d < nlib_dirs; ++d) {
            ns_str cand = ns_path_join(lib_dirs[d], ns_str_concat(module, ns_str_cstr(".dylib")));
            if (ns_file_exists(cand)) {
                dylib = cand;
                if (rpath_dir.data == ns_null) rpath_dir = ns_str_concat(lib_dirs[d], ns_str_cstr(""));
                break;
            }
            ns_str_free(cand);
        }
        if (dylib.data == ns_null) continue;
        ns_str q_dylib = ns_shell_quote(dylib);
        ns_str_append_cstr(&cmd, " ");
        ns_str_append(&cmd, q_dylib);
        ns_str_free(q_dylib);
        ns_str_free(dylib);
        ns_array_push(linked, module);
    }
    if (rpath_dir.data) {
        ns_str_append_cstr(&cmd, " -Wl,-rpath,");
        ns_str_append(&cmd, rpath_dir);
    }
    ns_str_append_cstr(&cmd, " -Wl,-rpath,@executable_path");
    ns_str_append_cstr(&cmd, " -Wl,-rpath,@executable_path/../../../");
    ns_str_append_cstr(&cmd, " -Wl,-rpath,@executable_path/../../../../lib");
    ns_str_append_cstr(&cmd, " -Wl,-rpath,@executable_path/../../../../bin");
    ns_array_free(linked);
    ns_str_free(exe);
    ns_str_free(bin);
    ns_str_free(root);
    ns_str_free(installed_lib);
    ns_str_free(home);
    ns_str_free(home_lib);
    ns_str_free(rpath_dir);

    ns_str_append_cstr(&cmd, " -o ");
    ns_str_append(&cmd, q_executable);
    ns_array_push(cmd.data, '\0');
    ns_build_profile_end("prepare_native_runtime", runtime_start);

    f64 link_start = ns_build_profile_begin("system_link");
    i32 ret = system(cmd.data);
    ns_build_profile_end("system_link", link_start);
    if (ret != 0) {
        ns_exit(1, "build", "failed to link executable %.*s.\n", executable_path.len, executable_path.data);
    }

    remove(object_path.data);
    remove(strtab_path.data);
    ns_str_free(object_path);
    ns_str_free(rt_path);
    ns_str_free(strtab_path);
    ns_str_free(inc_dir);
    ns_str_free(q_object);
    ns_str_free(q_executable);
    ns_str_free(q_rt);
    ns_str_free(q_strtab);
    ns_str_free(q_inc);
    ns_str_free(cmd);
}

static ns_str ns_build_darwin_current_executable(void) {
    u32 size = 0;
    _NSGetExecutablePath(ns_null, &size);
    if (size == 0) return ns_str_null;

    char *buf = (char*)ns_malloc(size + 1);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        ns_free(buf);
        return ns_str_null;
    }
    buf[size] = '\0';

    char resolved[4096];
    if (realpath(buf, resolved)) {
        ns_free(buf);
        return ns_str_concat(ns_str_cstr(resolved), ns_str_cstr(""));
    }

    ns_str path = ns_str_concat(ns_str_cstr(buf), ns_str_cstr(""));
    ns_free(buf);
    if (ns_path_is_absolute(path)) return path;

    ns_str cwd = ns_getcwd();
    ns_str absolute = ns_path_join(cwd, path);
    ns_str_free(cwd);
    ns_str_free(path);
    return absolute;
}

static void ns_build_darwin_app(ns_build_input *in, ns_str output, ns_ssa_module *ssa) {
    f64 bundle_start = ns_build_profile_begin("prepare_app_bundle");
    ns_str app_dir = ns_build_app_output(output);
    ns_str contents_dir = ns_path_join(app_dir, ns_str_cstr("Contents"));
    ns_str macos_dir = ns_path_join(contents_dir, ns_str_cstr("MacOS"));
    ns_str resources_dir = ns_path_join(contents_dir, ns_str_cstr("Resources"));
    ns_mkdir_p(macos_dir);
    ns_mkdir_p(resources_dir);

    ns_str executable_name = ns_str_concat(in->name, ns_str_cstr(""));
    ns_str executable_path = ns_path_join(macos_dir, executable_name);
    ns_build_profile_end("prepare_app_bundle", bundle_start);
    ns_build_darwin_link_executable(ssa, executable_path);

    f64 icon_start = ns_build_profile_begin("package_icon");
    ns_str icon_file = ns_str_null;
    ns_build_darwin_make_icns(in->icon, resources_dir, &icon_file);
    ns_build_profile_end("package_icon", icon_start);
    f64 metadata_start = ns_build_profile_begin("write_app_metadata");
    ns_build_darwin_write_plist(in, app_dir, executable_name, icon_file);
    ns_build_profile_end("write_app_metadata", metadata_start);
    f64 sign_start = ns_build_profile_begin("codesign_app");
    ns_build_darwin_codesign_app(app_dir);
    ns_build_profile_end("codesign_app", sign_start);

    ns_info("build", "app %.*s\n", app_dir.len, app_dir.data);
    ns_str_free(app_dir);
    ns_str_free(contents_dir);
    ns_str_free(macos_dir);
    ns_str_free(resources_dir);
    ns_str_free(executable_name);
    ns_str_free(executable_path);
    ns_str_free(icon_file);
}
#endif

static ns_bool ns_wasm_bundle_reserved(ns_str relative, ns_str wasm_name) {
    i32 first_end = 0;
    while (first_end < relative.len && relative.data[first_end] != '/' && relative.data[first_end] != '\\') first_end++;
    ns_str first = ns_str_slice(relative, 0, first_end);
    return ns_str_equals(first, wasm_name) ||
           ns_str_equals(first, ns_str_cstr("ns.runtime.js")) ||
           ns_str_equals(first, ns_str_cstr("__ns")) ||
           ns_str_equals(first, ns_str_cstr(".ns-wasm-files"));
}

static void ns_copy_binary_file(ns_str source, ns_str destination) {
    ns_str data = ns_os_read_file(source);
    if (data.data == ns_null) ns_exit(1, "wasm", "failed to read %.*s.\n", source.len, source.data);
    ns_build_ensure_output_dir(destination);
    FILE *file = fopen(destination.data, "wb");
    if (!file) ns_exit(1, "wasm", "failed to write %.*s.\n", destination.len, destination.data);
    if (data.len > 0 && fwrite(data.data, 1, (size_t)data.len, file) != (size_t)data.len) {
        fclose(file);
        ns_exit(1, "wasm", "failed to write %.*s.\n", destination.len, destination.data);
    }
    fclose(file);
    ns_str_free(data);
}

static ns_str ns_wasm_relative_join(ns_str lhs, ns_str rhs) {
    if (lhs.len == 0) return ns_str_concat(rhs, ns_str_cstr(""));
    ns_str out = ns_str_concat(lhs, ns_str_cstr("/"));
    return ns_str_concat(out, rhs);
}

static void ns_wasm_copy_tree(ns_str source, ns_str destination, ns_str relative,
                              ns_str wasm_name, ns_bool reserved_check, ns_bool validate_only,
                              ns_str **owned) {
    if (!ns_is_dir(source)) return;
#if defined(_WIN32)
    ns_str pattern = ns_path_join(source, ns_str_cstr("*"));
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern.data, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const char *name_c = fd.cFileName;
        if (strcmp(name_c, ".") == 0 || strcmp(name_c, "..") == 0) continue;
        ns_str name = ns_str_cstr((char*)name_c);
        ns_str child_source = ns_path_join(source, name);
        ns_str child_relative = ns_wasm_relative_join(relative, name);
        ns_str child_destination = ns_path_join(destination, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ns_wasm_copy_tree(child_source, child_destination, child_relative, wasm_name,
                              reserved_check, validate_only, owned);
        } else {
            if (reserved_check && ns_wasm_bundle_reserved(child_relative, wasm_name))
                ns_exit(1, "wasm", "web overlay cannot replace reserved file `%.*s`.\n",
                        child_relative.len, child_relative.data);
            if (!validate_only) {
                ns_copy_binary_file(child_source, child_destination);
                ns_array_push(*owned, ns_str_concat(child_relative, ns_str_cstr("")));
            }
        }
        ns_str_free(child_source);
        ns_str_free(child_relative);
        ns_str_free(child_destination);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    ns_str_free(pattern);
#else
    DIR *dir = opendir(source.data);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != ns_null) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        ns_str name = ns_str_cstr(entry->d_name);
        ns_str child_source = ns_path_join(source, name);
        ns_str child_relative = ns_wasm_relative_join(relative, name);
        ns_str child_destination = ns_path_join(destination, name);
        struct stat st;
        if (lstat(child_source.data, &st) != 0 || S_ISLNK(st.st_mode)) {
            ns_str_free(child_source);
            ns_str_free(child_relative);
            ns_str_free(child_destination);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ns_wasm_copy_tree(child_source, child_destination, child_relative, wasm_name,
                              reserved_check, validate_only, owned);
        } else if (S_ISREG(st.st_mode)) {
            if (reserved_check && ns_wasm_bundle_reserved(child_relative, wasm_name))
                ns_exit(1, "wasm", "web overlay cannot replace reserved file `%.*s`.\n",
                        child_relative.len, child_relative.data);
            if (!validate_only) {
                ns_copy_binary_file(child_source, child_destination);
                ns_array_push(*owned, ns_str_concat(child_relative, ns_str_cstr("")));
            }
        }
        ns_str_free(child_source);
        ns_str_free(child_relative);
        ns_str_free(child_destination);
    }
    closedir(dir);
#endif
}

static void ns_wasm_remove_old_owned(ns_str bundle_dir) {
    ns_str manifest_path = ns_path_join(bundle_dir, ns_str_cstr(".ns-wasm-files"));
    ns_str manifest = ns_os_read_file(manifest_path);
    for (i32 i = 0; manifest.data != ns_null && i < manifest.len;) {
        i32 start = i;
        while (i < manifest.len && manifest.data[i] != '\n' && manifest.data[i] != '\r') i++;
        ns_str rel = ns_str_slice(manifest, start, i);
        while (i < manifest.len && (manifest.data[i] == '\n' || manifest.data[i] == '\r')) i++;
        if (rel.len == 0 || rel.data[0] == '/' || rel.data[0] == '\\') continue;
        ns_bool safe = true;
        for (i32 p = 0; p + 1 < rel.len; p++) {
            if (rel.data[p] == '.' && rel.data[p + 1] == '.') { safe = false; break; }
        }
        if (!safe) continue;
        ns_str path = ns_path_join(bundle_dir, rel);
        remove(path.data);
        ns_str_free(path);
    }
    remove(manifest_path.data);
    ns_str_free(manifest);
    ns_str_free(manifest_path);
}

static const char *ns_wasm_embedded_runtime_source =
"const decoder = new TextDecoder();\n"
"function text(memory, ptr, len = -1) {\n"
"  if (!ptr) return ''; const bytes = new Uint8Array(memory.buffer);\n"
"  if (len < 0) { len = 0; while (bytes[ptr + len]) len++; }\n"
"  return decoder.decode(bytes.subarray(ptr, ptr + len));\n"
"}\n"
"function reloadSocket() {\n"
"  if (location.hostname !== '127.0.0.1' && location.hostname !== 'localhost') return null;\n"
"  let stopped = false, socket = null;\n"
"  const connect = () => {\n"
"    if (stopped) return; const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';\n"
"    socket = new WebSocket(`${protocol}//${location.host}/__ns/reload`);\n"
"    socket.onmessage = e => { if (e.data === 'reload') location.reload(); else if (e.data === 'build-error') console.error('Nano Script rebuild failed; keeping the last good app.'); };\n"
"    socket.onclose = () => { if (!stopped) setTimeout(connect, 500); };\n"
"  }; connect(); return () => { stopped = true; if (socket) socket.close(); };\n"
"}\n"
"export async function start(options = {}) {\n"
"  if (!navigator.gpu) throw new Error('WebGPU is required by this Nano Script application.');\n"
"  const adapter = await navigator.gpu.requestAdapter();\n"
"  if (!adapter) throw new Error('No WebGPU adapter is available.');\n"
"  const device = await adapter.requestDevice();\n"
"  const canvas = options.canvas || document.querySelector('canvas');\n"
"  const url = options.wasmURL || './app.wasm';\n"
"  const bytes = await (await fetch(url)).arrayBuffer();\n"
"  const module = await WebAssembly.compile(bytes);\n"
"  let exports = null;\n"
"  const host = {};\n"
"  for (const item of WebAssembly.Module.imports(module)) {\n"
"    if (item.module !== 'ns' || item.kind !== 'function') continue;\n"
"    host[item.name] = (...args) => {\n"
"      if (item.name === 'print') { console.log(text(exports.memory, args[0])); return 0; }\n"
"      if (item.name === 'gpu_shader_target') return 0;\n"
"      throw new Error(`Unsupported Nano Script browser import: ${item.name}`);\n"
"    };\n"
"  }\n"
"  const instance = await WebAssembly.instantiate(module, { ns: host }); exports = instance.exports;\n"
"  if (exports.__ns_init) exports.__ns_init(); if (exports.main) exports.main();\n"
"  const stopReload = reloadSocket();\n"
"  return { exports, memory: exports.memory, device, canvas, stop() { if (stopReload) stopReload(); } };\n"
"}\n";

static ns_str ns_wasm_default_index(ns_str wasm_name) {
    ns_str html = ns_str_null;
    ns_str_append_cstr(&html, "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n");
    ns_str_append_cstr(&html, "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n");
    ns_str_append_cstr(&html, "<style>html,body,canvas{margin:0;width:100%;height:100%;display:block;background:#1e1e24}</style></head>\n");
    ns_str_append_cstr(&html, "<body><canvas></canvas><script type=\"module\">\nimport { start } from './ns.runtime.js';\n");
    ns_str_append_cstr(&html, "start({ wasmURL: './");
    ns_str_append(&html, wasm_name);
    ns_str_append_cstr(&html, "', canvas: document.querySelector('canvas') }).catch(error => { console.error(error); document.body.textContent = error.message; });\n");
    ns_str_append_cstr(&html, "</script></body></html>\n");
    return html;
}

static void ns_wasm_write_owned_manifest(ns_str bundle_dir, ns_str *owned) {
    ns_str body = ns_str_null;
    for (i32 i = 0, count = (i32)ns_array_length(owned); i < count; i++) {
        ns_str_append(&body, owned[i]);
        ns_str_append_cstr(&body, "\n");
    }
    ns_str path = ns_path_join(bundle_dir, ns_str_cstr(".ns-wasm-files"));
    ns_write_text_file(path, body);
    ns_str_free(path);
    ns_str_free(body);
}

static void ns_build_wasm_app(ns_build_input *in, ns_str output, ns_ssa_module *ssa) {
    if (!ns_build_type_is_app(in->module_type)) {
        ns_ssa_module_free(ssa);
        ns_exit(1, "build", "target = \"wasm\" currently supports type = \"app\" only.\n");
    }
    ns_str bundle_dir = ns_path_dirname_safe(output);
    ns_str wasm_name = ns_path_last_component(output);
    ns_mkdir_p(bundle_dir);

    ns_str web = ns_path_join(in->scope, ns_str_cstr("web"));
    ns_wasm_copy_tree(web, bundle_dir, ns_str_cstr(""), wasm_name, true, true, ns_null);

    ns_str temporary = ns_str_concat(output, ns_str_cstr(".tmp"));
    f64 emit_start = ns_build_profile_begin("emit_wasm");
    ns_return_bool emitted = ns_wasm_emit(ssa, temporary);
    ns_ssa_module_free(ssa);
    ns_build_profile_end("emit_wasm", emit_start);
    if (ns_return_is_error(emitted)) ns_return_assert(emitted);

    f64 package_start = ns_build_profile_begin("package_wasm");
    ns_wasm_remove_old_owned(bundle_dir);
    if (rename(temporary.data, output.data) != 0) {
        remove(temporary.data);
        ns_exit(1, "build", "failed to replace %.*s.\n", output.len, output.data);
    }

    ns_str *owned = ns_null;
    ns_array_push(owned, ns_str_concat(wasm_name, ns_str_cstr("")));
    ns_str runtime_path = ns_path_join(bundle_dir, ns_str_cstr("ns.runtime.js"));
    ns_write_text_file(runtime_path, ns_str_cstr((char*)ns_wasm_embedded_runtime_source));
    ns_array_push(owned, ns_str_cstr("ns.runtime.js"));
    ns_str index_path = ns_path_join(bundle_dir, ns_str_cstr("index.html"));
    ns_write_text_file(index_path, ns_wasm_default_index(wasm_name));
    ns_array_push(owned, ns_str_cstr("index.html"));

    ns_str assets = ns_path_join(in->scope, ns_str_cstr("assets"));
    ns_str assets_out = ns_path_join(bundle_dir, ns_str_cstr("assets"));
    ns_wasm_copy_tree(assets, assets_out, ns_str_cstr("assets"), wasm_name, false, false, &owned);
    ns_wasm_copy_tree(web, bundle_dir, ns_str_cstr(""), wasm_name, true, false, &owned);

    ns_wasm_write_owned_manifest(bundle_dir, owned);
    for (i32 i = 0, count = (i32)ns_array_length(owned); i < count; i++) {
        if (owned[i].dynamic) ns_str_free(owned[i]);
    }
    ns_array_free(owned);
    ns_str_free(web);
    ns_str_free(assets);
    ns_str_free(assets_out);
    ns_str_free(runtime_path);
    ns_str_free(index_path);
    ns_str_free(temporary);
    ns_build_profile_end("package_wasm", package_start);
    ns_info("build", "wasm app %.*s\n", bundle_dir.len, bundle_dir.data);
    ns_str_free(bundle_dir);
}

// Compile `in` and write its artifact. Every branch either emits the artifact
// or exits, so the caller records the build inputs only after a real build.
static void ns_build_emit(ns_build_input *in, ns_build_kind kind, ns_str output, ns_ssa_module *ssa) {
    if (ns_build_target_is_wasm(in->target)) {
        ns_build_wasm_app(in, output, ssa);
        return;
    }

    if (kind == NS_BUILD_LIB) {
        ns_asm_target target;
        ns_asm_get_current_target(&target);
        if (target.os != NS_OS_DARWIN) {
            ns_ssa_module_free(ssa);
            ns_exit(1, "build", "static library output is currently supported for mach-o targets only.\n");
        }

        ns_str object = ns_str_concat(output, ns_str_cstr(".o"));
        f64 object_start = ns_build_profile_begin("emit_object");
        ns_return_bool emit_ret = ns_macho_emit_object(ssa, object);
        ns_ssa_module_free(ssa);
        if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);
        ns_build_profile_end("emit_object", object_start);

        ns_archive_object(output, object);
        ns_info("build", "library %.*s\n", output.len, output.data);
        return;
    }

    if (kind == NS_BUILD_APP) {
#if defined(NS_DARWIN)
        ns_str app_output = ns_build_app_output(output);
        ns_build_darwin_app(in, app_output, ssa);
        ns_ssa_module_free(ssa);
        return;
#else
        ns_ssa_module_free(ssa);
        ns_exit(1, "build", "app bundle output is only available on Darwin builds.\n");
#endif
    }

    ns_asm_target target;
    ns_asm_get_current_target(&target);
    ns_return_bool emit_ret;
    if (target.os == NS_OS_DARWIN) {
#if defined(NS_DARWIN)
        ns_build_darwin_link_executable(ssa, output);
        ns_ssa_module_free(ssa);
        ns_info("build", "executable %.*s\n", output.len, output.data);
        return;
#else
        f64 executable_start = ns_build_profile_begin("emit_executable");
        emit_ret = ns_macho_emit(ssa, output);
        ns_build_profile_end("emit_executable", executable_start);
#endif
    } else if (target.os == NS_OS_WINDOWS) {
        f64 executable_start = ns_build_profile_begin("emit_executable");
        emit_ret = ns_pe_emit(ssa, output);
        ns_build_profile_end("emit_executable", executable_start);
    } else {
        ns_ssa_module_free(ssa);
        ns_exit(1, "build", "executable output is not yet supported for this target OS.\n");
        return;
    }

    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);
    ns_info("build", "executable %.*s\n", output.len, output.data);
}

// `ns build [path|target]`. `target_name` is the manifest target to compile;
// pass ns_str_null to let the positional argument decide.
void ns_exec_build_target(ns_str path, ns_str output, u8 requested_kind, ns_bool force, ns_str target_name) {
    f64 target_start = ns_build_profile_begin("build_target");
    f64 resolve_start = ns_build_profile_begin("resolve_input");
    ns_build_input in = ns_build_input_resolve(path, target_name);
    ns_build_profile_end("resolve_input", resolve_start);
    if (in.target.len > 0 && !ns_build_target_is_wasm(in.target)) {
        ns_exit(1, "build", "unsupported project target `%.*s`; expected `wasm` or omit target for a native build.\n",
                in.target.len, in.target.data);
    }
    if (ns_build_target_is_wasm(in.target)) {
        ns_exec_build_wasm(&in, output, requested_kind, force);
        ns_build_profile_end("build_target", target_start);
        return;
    }

    ns_build_kind kind = ns_build_resolve_kind(&in, requested_kind);
    ns_asm_target target;
    ns_asm_get_current_target(&target);
    if (kind == NS_BUILD_APP && target.os != NS_OS_DARWIN) {
        ns_warn("build", "app bundle icon packaging is currently supported for mach-o targets only; emitting executable.\n");
        kind = NS_BUILD_EXE;
    }
    if (output.len == 0) output = ns_build_default_output(&in, kind);
    ns_build_ensure_output_dir(output);

    ns_str artifact = ns_build_artifact_path(kind, output);
    f64 cache_start = ns_build_profile_begin("check_cache");
    ns_build_cache cache;
    ns_str config = ns_build_config(&in, kind, output);
    ns_build_cache_open(&cache, artifact, config);
    ns_array_free(config.data);
    ns_build_cache_collect(&cache, &in);
    ns_bool fresh = !force && ns_build_cache_fresh(&cache);
    ns_build_profile_end("check_cache", cache_start);
    if (fresh) {
        ns_info("build", "up to date %.*s\n", artifact.len, artifact.data);
        ns_build_cache_free(&cache);
        ns_str_free(artifact);
        ns_build_profile_end("build_target", target_start);
        return;
    }

    f64 compile_start = ns_build_profile_begin("compile");
    ns_ssa_module *ssa = ns_compile_build_input(&in, &cache);
    ns_build_profile_end("compile", compile_start);
    f64 emit_start = ns_build_profile_begin("emit_artifact");
    ns_build_emit(&in, kind, output, ssa);
    ns_build_profile_end("emit_artifact", emit_start);
    f64 write_cache_start = ns_build_profile_begin("write_cache");
    ns_build_cache_write(&cache);
    ns_build_profile_end("write_cache", write_cache_start);
    ns_build_cache_free(&cache);
    ns_str_free(artifact);
    ns_build_profile_end("build_target", target_start);
}

// `ns build [path|target]`. A target name builds that target alone; a project
// (an explicit directory, or none at all) builds every target it declares, so
// each one emits its own artifact kind under `bin/`.
void ns_exec_build(ns_str path, ns_str output, u8 requested_kind, ns_bool force) {
    ns_str target_name = ns_str_null;
    ns_str root = ns_str_null;
    if (ns_target_arg_select(path, &root, &target_name)) {
        ns_exec_build_target(root, output, requested_kind, force, target_name);
        return;
    }

    ns_str project = ns_str_null;
    if (path.len == 0) project = ns_project_root(ns_getcwd());
    else if (ns_is_dir(path)) project = ns_project_root(path);

    ns_str *names = project.data != ns_null ? ns_manifest_target_list(project) : ns_null;
    i32 count = ns_array_length(names);
    if (count > 1 && output.len > 0) {
        ns_exit(1, "build", "-o takes a single target; %.*s declares %d. Name one, or drop -o.\n",
                project.len, project.data, count);
    }
    for (i32 i = 0; i < count; i++) {
        ns_exec_build_target(project, output, requested_kind, force, names[i]);
        ns_str_free(names[i]);
    }
    ns_array_free(names);
    if (count > 0) return;

    ns_exec_build_target(path, output, requested_kind, force, ns_str_null);
}

static ns_str ns_project_absolute_path(ns_str path) {
    if (ns_path_is_absolute(path)) return ns_str_concat(path, ns_str_cstr(""));
    ns_str cwd = ns_getcwd();
    if (path.len == 0 || ns_str_equals(path, ns_str_cstr("."))) return cwd;
    ns_str absolute = ns_path_join(cwd, path);
    ns_str_free(cwd);
    return absolute;
}

// Everything `ns` generates for a project: the bin/ output directory, which
// also holds generated IDE projects and the build cache, and the profile
// written beside the manifest. This is the `bin`/`ns.profile` part of the
// scaffolded .gitignore; source and user files are never touched.
// `ns.profile.json` is leftover Chrome-trace output from older ns versions.
static const char *ns_clean_generated[] = {"bin", "ns.profile", "ns.profile.json"};

// `ns clean [path]` - remove the generated files of the nearest project.
void ns_exec_clean(ns_str path) {
    ns_str base = ns_project_absolute_path(path);
    if (!ns_is_dir(base)) base = ns_path_dirname_safe(base);

    // A directory outside any project is cleaned on its own.
    ns_str root = base;
    ns_find_project_root(base, &root);

    i32 removed = 0;
    for (i32 i = 0; i < (i32)(sizeof(ns_clean_generated) / sizeof(ns_clean_generated[0])); i++) {
        ns_str target = ns_path_join(root, ns_str_cstr(ns_clean_generated[i]));
        if (ns_is_dir(target) || ns_file_exists(target)) {
            ns_remove_tree(target);
            ns_info("clean", "removed %.*s\n", target.len, target.data);
            removed++;
        }
        ns_str_free(target);
    }
    if (removed == 0) ns_info("clean", "nothing to remove in %.*s\n", root.len, root.data);
}

static ns_bool ns_project_runtime_root_valid(ns_str root) {
    if (root.data == ns_null || root.len == 0) return false;
    ns_str source = ns_path_join(root, ns_str_cstr("src/ns_vm_eval.c"));
    ns_str header = ns_path_join(root, ns_str_cstr("include/ns_vm.h"));
    ns_str std_ref = ns_path_join(root, ns_str_cstr("ref/std.ns"));
    ns_str std_lib = ns_path_join(root, ns_str_cstr("lib/std.ns"));
    ns_bool valid = ns_file_exists(source) && ns_file_exists(header) &&
                    (ns_file_exists(std_ref) || ns_file_exists(std_lib));
    ns_str_free(source);
    ns_str_free(header);
    ns_str_free(std_ref);
    ns_str_free(std_lib);
    return valid;
}

static ns_str ns_project_current_executable(void) {
#if defined(NS_DARWIN)
    return ns_build_darwin_current_executable();
#elif defined(_WIN32)
    char buf[4096];
    DWORD len = GetModuleFileNameA(ns_null, buf, (DWORD)sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) return ns_str_null;
    return ns_str_concat(ns_str_range(buf, (i32)len), ns_str_cstr(""));
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ns_str_null;
    buf[len] = '\0';
    return ns_str_concat(ns_str_range(buf, (i32)len), ns_str_cstr(""));
#endif
}

// Configure module declarations and FFI libraries relative to the running ns
// executable, so debug builds keep working after `make install` and commands
// can be launched from any working directory. Supported layouts are:
//   installed: <root>/bin/ns, <root>/ref/*.ns, <root>/lib/*.{dylib,so,dll}
//   source:    <repo>/bin/ns, <repo>/lib/*.ns, preferring matching FFI libs
//              in <repo>/bin and falling back to the installed runtime
static void ns_configure_vm_runtime_paths(ns_vm *target) {
    ns_str executable = ns_project_current_executable();
    if (executable.data == ns_null) return;

    ns_str bin = ns_path_dirname_safe(executable);
    ns_str root = ns_path_parent(bin);
    ns_str installed_ref = ns_path_join(root, ns_str_cstr("ref"));
    ns_str installed_std = ns_path_join(installed_ref, ns_str_cstr("std.ns"));
    if (ns_file_exists(installed_std)) {
        ns_vm_set_ref_path(target, installed_ref);
        ns_vm_set_lib_path(target, ns_path_join(root, ns_str_cstr("lib")));
        return;
    }

    ns_str source_ref = ns_path_join(root, ns_str_cstr("lib"));
    ns_str source_std = ns_path_join(source_ref, ns_str_cstr("std.ns"));
    if (ns_file_exists(source_std)) {
        ns_vm_set_ref_path(target, source_ref);
        // Source declarations and native modules must stay in sync. Prefer
        // this build's bin directory, while remaining usable before the first
        // native-module build when an installed runtime is available.
        ns_vm_set_lib_path(target, ns_str_concat(bin, ns_str_cstr("")));
        ns_str home = ns_path_home();
        ns_vm_set_lib_fallback_path(target, ns_path_join(home, ns_str_cstr("ns/lib")));
    }
}

static ns_str ns_project_runtime_root(ns_str executable) {
    const char *override = getenv("NS_RUNTIME_ROOT");
    if (override && override[0]) {
        ns_str root = ns_project_absolute_path(ns_str_cstr((char*)override));
        if (ns_project_runtime_root_valid(root)) return root;
        ns_exit(1, "project", "NS_RUNTIME_ROOT is not a valid ns runtime SDK: %s\n", override);
    }

    ns_str bin = ns_path_dirname_safe(executable);
    ns_str install = ns_path_parent(bin);
    ns_str shared = ns_path_join(install, ns_str_cstr("share/ns-runtime"));
    if (ns_project_runtime_root_valid(shared)) return shared;
    ns_str_free(shared);

    // Development-tree layout: <repo>/bin/ns with src/, include/, and lib/ at
    // the repository root.
    if (ns_project_runtime_root_valid(install)) {
        return ns_str_concat(install, ns_str_cstr(""));
    }

    ns_str cwd = ns_getcwd();
    if (ns_project_runtime_root_valid(cwd)) return cwd;
    ns_str_free(cwd);
    return ns_str_null;
}

static ns_bool ns_project_module_embeddable(ns_str module) {
    return ns_str_equals(module, ns_str_cstr("std")) ||
           ns_str_equals(module, ns_str_cstr("task")) ||
           ns_str_equals(module, ns_str_cstr("shader")) ||
           ns_str_equals(module, ns_str_cstr("simd")) ||
           ns_str_equals(module, ns_str_cstr("view")) ||
           ns_str_equals(module, ns_str_cstr("ui")) ||
           ns_str_equals(module, ns_str_cstr("os")) ||
           ns_str_equals(module, ns_str_cstr("gpu")) ||
           ns_str_equals(module, ns_str_cstr("net")) ||
           ns_str_equals(module, ns_str_cstr("io"));
}

void ns_exec_project(ns_str path) {
    ns_str start = path;
    if (start.len == 0) start = ns_getcwd();
    ns_str root = ns_project_root(start);
    root = ns_project_absolute_path(root);

    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    // A multi-target manifest generates the IDE project of its default target.
    ns_manifest_selection selection = ns_manifest_select(root, ns_str_null);
    ns_str schema = ns_build_manifest_value(root, "schema");
    ns_str name = ns_build_manifest_value(root, "name");
    ns_str module_type = selection.type;
    ns_str project_target = selection.platform;
    ns_str version = ns_build_manifest_value(root, "version");
    ns_str icon = selection.icon;
    if (!ns_str_equals(schema, ns_str_cstr(NS_MANIFEST_SCHEMA_CURRENT))) {
        ns_exit(1, "project", "%.*s must declare schema = \"" NS_MANIFEST_SCHEMA_CURRENT "\".\n",
                manifest.len, manifest.data);
    }
    if (name.data == ns_null || name.len == 0) {
        ns_exit(1, "project", "%.*s must declare a non-empty project name.\n", manifest.len, manifest.data);
    }

    if (ns_build_target_is_wasm(project_target)) {
        if (!ns_build_type_is_app(module_type))
            ns_exit(1, "project", "target = \"wasm\" currently supports type = \"app\" only.\n");
        ns_exec_build_target(root, ns_str_null, NS_BUILD_AUTO, false, selection.target_name);
        return;
    }

    // A `cli` target has an entry and an executable artifact like an app, but
    // no host app bundle, so it generates the host build/test targets instead
    // of the platform application targets.
    ns_bool cli_project = ns_build_type_is_cli(module_type);
    ns_project_kind kind;
    if (ns_build_type_is_app(module_type) || cli_project) kind = NS_PROJECT_APP;
    else if (ns_build_type_is_library(module_type)) kind = NS_PROJECT_LIBRARY;
    else {
        ns_exit(1, "project", "%.*s has unsupported type `%.*s`; expected app, cli or library.\n",
                manifest.len, manifest.data, module_type.len, module_type.data);
    }
    if (version.data == ns_null || version.len == 0) version = ns_str_cstr("0.1.0");

    ns_str source_value = ns_build_manifest_value(root, "source");
    ns_str source_dir = (source_value.data != ns_null && !ns_str_equals(source_value, ns_str_cstr(".")))
                            ? ns_path_join(root, source_value)
                            : ns_str_concat(root, ns_str_cstr(""));
    ns_str *external_modules = ns_null;
    ns_str linked = ns_str_null;
    if (kind == NS_PROJECT_APP) {
        ns_build_input in = ns_build_input_resolve(root, selection.target_name);
        linked = ns_project_link_all(root, in.source, in.filename, false, ns_null, &external_modules);
    }

    ns_bool host_build = cli_project;
    if (kind == NS_PROJECT_APP) {
        for (i32 i = 0, l = ns_array_length(external_modules); i < l; i++) {
            if (!ns_project_module_embeddable(external_modules[i])) host_build = true;
        }
    }

    ns_str executable = ns_project_current_executable();
    if (executable.data == ns_null) ns_exit(1, "project", "failed to locate the ns executable.\n");
    ns_str runtime_root = kind == NS_PROJECT_APP ? ns_project_runtime_root(executable) : ns_str_null;
#if defined(NS_DARWIN)
    if (kind == NS_PROJECT_APP && runtime_root.data == ns_null) {
        ns_exit(1, "project", "language runtime SDK not found; reinstall ns or set NS_RUNTIME_ROOT.\n");
    }
#endif

    ns_project_spec spec = {
        .kind = kind,
        .host_build = host_build,
        .root = root,
        .manifest = manifest,
        .source_dir = source_dir,
        .name = name,
        .safe_name = ns_project_safe_name(name),
        .version = version,
        .icon = icon,
        .linked_source = linked,
        .ns_executable = executable,
        .runtime_root = runtime_root,
    };

    ns_bool generated = false;
#if defined(NS_DARWIN)
    generated = ns_project_generate_xcode(&spec);
#elif defined(_WIN32) || defined(NS_WIN)
    generated = ns_project_generate_visual_studio(&spec);
#else
    ns_exit(1, "project", "IDE project generation is supported on Darwin and Windows hosts only.\n");
#endif
    if (!generated) ns_exit(1, "project", "failed to generate IDE project.\n");

#if defined(NS_DARWIN)
    ns_info("project", "Xcode project %.*s/bin/%.*s.xcodeproj\n",
            root.len, root.data, spec.safe_name.len, spec.safe_name.data);
#else
    ns_info("project", "Visual Studio solution %.*s/bin/%.*s.sln\n",
            root.len, root.data, spec.safe_name.len, spec.safe_name.data);
#endif
}

// ---------------------------------------------------------------------------
// `ns init [path]` / `ns create <name>` project scaffolding
// ---------------------------------------------------------------------------
// Both commands write the same skeleton (ns.mod, src/main.ns, README.md,
// AGENTS.md, .gitignore). `init` targets an existing directory (default: cwd)
// and keeps any file already present; `create` requires a fresh directory so
// it never collides with user content.

// Script files live in their own directory, so the project root keeps only the
// manifest and support files. The manifest `source` field points at it and
// `entry` stays relative to it.
#define NS_SCAFFOLD_SOURCE_DIR "src"

// Last path component, trailing separators ignored. Unlike ns_path_filename
// this keeps dots, so a directory like `my.app` yields its full name.
static ns_str ns_path_last_component(ns_str path) {
    i32 end = path.len;
    while (end > 0 && path.data[end - 1] == NS_PATH_SEPARATOR) end--;
    i32 start = end;
    while (start > 0 && path.data[start - 1] != NS_PATH_SEPARATOR) start--;
    return ns_str_slice(path, start, end);
}

// Scaffold names are embedded in TOML strings and generated source, so keep
// them to characters that need no escaping in either.
static ns_bool ns_scaffold_name_valid(ns_str name) {
    if (name.len == 0) return false;
    for (i32 i = 0; i < name.len; i++) {
        i8 c = name.data[i];
        if (!(ns_is_ident_char(c) || c == '-' || c == '.')) return false;
    }
    return true;
}

static ns_str ns_scaffold_manifest_text(ns_str name) {
    ns_str s = ns_str_null;
    ns_str_append_cstr(&s, "schema = \"" NS_MANIFEST_SCHEMA_CURRENT "\"\n");
    ns_str_append_cstr(&s, "name = \"");
    ns_str_append(&s, name);
    ns_str_append_cstr(&s, "\"\n");
    ns_str_append_cstr(&s, "version = \"0.1.0\"\n");
    ns_str_append_cstr(&s, "type = \"app\"\n");
    ns_str_append_cstr(&s, "description = \"A Nano Script project.\"\n");
    ns_str_append_cstr(&s, "source = \"" NS_SCAFFOLD_SOURCE_DIR "\"\n");
    ns_str_append_cstr(&s, "entry = \"main.ns\"\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "[[dependencies.runtime]]\n");
    ns_str_append_cstr(&s, "name = \"std\"\n");
    ns_str_append_cstr(&s, "version = \">=0.1.0\"\n");
    return s;
}

static ns_str ns_scaffold_main_text(ns_str name) {
    ns_str s = ns_str_null;
    ns_str_append_cstr(&s, "use std\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "fn main() {\n");
    ns_str_append_cstr(&s, "    print(\"hello, ");
    ns_str_append(&s, name);
    ns_str_append_cstr(&s, "!\\n\")\n");
    ns_str_append_cstr(&s, "}\n");
    return s;
}

static ns_str ns_scaffold_readme_text(ns_str name) {
    ns_str s = ns_str_null;
    ns_str_append_cstr(&s, "# ");
    ns_str_append(&s, name);
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "A Nano Script project.\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "## Run\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "```bash\n");
    ns_str_append_cstr(&s, "ns run\n");
    ns_str_append_cstr(&s, "```\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "## Build\n");
    ns_str_append_cstr(&s, "\n");
    ns_str_append_cstr(&s, "```bash\n");
    ns_str_append_cstr(&s, "ns build\n");
    ns_str_append_cstr(&s, "```\n");
    return s;
}

static const char *ns_scaffold_gitignore_rules[] = {"bin", ".DS_Store", "*.log", "ns.profile"};

static ns_str ns_scaffold_gitignore_text(void) {
    ns_str s = ns_str_null;
    for (i32 i = 0; i < (i32)(sizeof(ns_scaffold_gitignore_rules) / sizeof(ns_scaffold_gitignore_rules[0])); i++) {
        ns_str_append_cstr(&s, ns_scaffold_gitignore_rules[i]);
        ns_str_append_cstr(&s, "\n");
    }
    return s;
}

static ns_str ns_scaffold_agents_text(void) {
    ns_str s = ns_str_null;
    ns_str_append_cstr(&s, ns_scaffold_agents_markdown);
    return s;
}

// Write one scaffold file unless it already exists; existing files are kept
// so `ns init` can fill in the gaps of a partially set up directory. `dir` is
// a project-relative directory, or ns_null for the project root; it is created
// on demand so a scaffold may nest files.
static void ns_scaffold_write(ns_str root, const char *dir, const char *filename, ns_str text, const char *tag) {
    ns_str parent = dir == ns_null ? ns_str_concat(root, ns_str_cstr("")) : ns_path_join(root, ns_str_cstr((char*)dir));
    if (!ns_is_dir(parent)) {
        ns_mkdir_p(parent);
        if (!ns_is_dir(parent)) ns_exit(1, tag, "failed to create directory %.*s.\n", parent.len, parent.data);
    }

    ns_str path = ns_path_join(parent, ns_str_cstr((char*)filename));
    if (ns_file_exists(path)) {
        ns_warn(tag, "skip existing %.*s.\n", path.len, path.data);
    } else {
        ns_write_text_file(path, text);
        ns_info(tag, "wrote %.*s\n", path.len, path.data);
    }
    ns_str_free(path);
    ns_str_free(parent);
    ns_str_free(text);
}

static void ns_scaffold_project(ns_str root, ns_str name, const char *tag) {
    if (!ns_scaffold_name_valid(name)) {
        ns_exit(1, tag, "invalid project name `%.*s`; use letters, digits, `_`, `-` or `.`.\n", name.len, name.data);
    }
    ns_scaffold_write(root, ns_null, "ns.mod", ns_scaffold_manifest_text(name), tag);
    ns_scaffold_write(root, NS_SCAFFOLD_SOURCE_DIR, "main.ns", ns_scaffold_main_text(name), tag);
    ns_scaffold_write(root, ns_null, "README.md", ns_scaffold_readme_text(name), tag);
    ns_scaffold_write(root, ns_null, "AGENTS.md", ns_scaffold_agents_text(), tag);
    ns_scaffold_write(root, ns_null, ".gitignore", ns_scaffold_gitignore_text(), tag);
}

void ns_exec_init(ns_str path) {
    ns_str root = path.len == 0 ? ns_getcwd() : ns_str_concat(path, ns_str_cstr(""));
    if (!ns_is_dir(root)) {
        ns_mkdir_p(root);
        if (!ns_is_dir(root)) ns_exit(1, "init", "failed to create directory %.*s.\n", root.len, root.data);
    }

    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    if (ns_file_exists(manifest)) {
        ns_exit(1, "init", "%.*s already exists; already an ns project.\n", manifest.len, manifest.data);
    }
    ns_str_free(manifest);

    // resolve to an absolute path so `.` still yields a meaningful name
    ns_str name = ns_path_last_component(ns_project_absolute_path(root));
    ns_scaffold_project(root, name, "init");
    ns_info("init", "project %.*s ready; run it with `ns run`.\n", name.len, name.data);
}

void ns_exec_create(ns_str path) {
    if (path.len == 0) ns_exit(1, "create", "no project name; usage: ns create <name>.\n");
    if (ns_is_dir(path) || ns_file_exists(path)) {
        ns_exit(1, "create", "%.*s already exists; use `ns init %.*s` to scaffold in place.\n",
                path.len, path.data, path.len, path.data);
    }

    ns_str name = ns_path_last_component(path);
    if (!ns_scaffold_name_valid(name)) {
        ns_exit(1, "create", "invalid project name `%.*s`; use letters, digits, `_`, `-` or `.`.\n", name.len, name.data);
    }

    ns_mkdir_p(path);
    if (!ns_is_dir(path)) ns_exit(1, "create", "failed to create directory %.*s.\n", path.len, path.data);

    ns_scaffold_project(path, name, "create");
    ns_info("create", "project %.*s created; `cd %.*s` then `ns run`.\n", name.len, name.data, path.len, path.data);
}

// ---------------------------------------------------------------------------
// `ns update [path]` project metadata migration
// ---------------------------------------------------------------------------
// Application source and README content are never rewritten. The command owns
// the manifest schema marker, the canonical agent guide, and additive ignore
// rules. Original changed files are retained below bin/ns-update-backup.

static ns_bool ns_update_line_equals(ns_str text, const char *wanted) {
    i32 wanted_len = (i32)strlen(wanted);
    for (i32 i = 0; i < text.len;) {
        i32 start = i;
        while (i < text.len && text.data[i] != '\n') i++;
        i32 end = i;
        if (end > start && text.data[end - 1] == '\r') end--;
        if (end - start == wanted_len && strncmp(text.data + start, wanted, wanted_len) == 0) return true;
        if (i < text.len) i++;
    }
    return false;
}

static void ns_update_write_text(ns_str path, ns_str text) {
    FILE *file = fopen(path.data, "wb");
    if (!file) ns_exit(1, "update", "failed to write %.*s.\n", path.len, path.data);
    if (text.len > 0 && fwrite(text.data, 1, text.len, file) != (size_t)text.len) {
        fclose(file);
        ns_exit(1, "update", "failed to write %.*s.\n", path.len, path.data);
    }
    fclose(file);
}

static void ns_update_backup(ns_str root, const char *filename, ns_str text) {
    ns_str bin = ns_path_join(root, ns_str_cstr("bin"));
    ns_str backup_dir = ns_path_join(bin, ns_str_cstr("ns-update-backup"));
    ns_mkdir_p(backup_dir);
    ns_str backup = ns_path_join(backup_dir, ns_str_cstr((char*)filename));
    i32 revision = 0;
    while (ns_file_exists(backup)) {
        ns_str previous = ns_os_read_file(backup);
        ns_bool already_saved = previous.data != ns_null && ns_str_equals(previous, text);
        ns_str_free(previous);
        if (already_saved) {
            ns_str_free(backup);
            ns_str_free(backup_dir);
            ns_str_free(bin);
            return;
        }
        ns_str_free(backup);
        revision++;
        char backup_name[4096];
        snprintf(backup_name, sizeof(backup_name), "%s.%d", filename, revision);
        backup = ns_path_join(backup_dir, ns_str_cstr(backup_name));
    }
    ns_update_write_text(backup, text);
    ns_info("update", "backed up %s to %.*s\n", filename, backup.len, backup.data);
    ns_str_free(backup);
    ns_str_free(backup_dir);
    ns_str_free(bin);
}

// Return the byte span inside the quoted value assigned to a top-level key.
static ns_bool ns_update_manifest_value_span(ns_str src, const char *key, i32 *start, i32 *end) {
    i32 key_len = (i32)strlen(key);
    for (i32 i = 0; i < src.len;) {
        i32 line_start = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 line_end = i;
        if (i < src.len) i++;

        i32 p = line_start;
        while (p < line_end && (src.data[p] == ' ' || src.data[p] == '\t' || src.data[p] == '\r')) p++;
        if (line_end - p < key_len || strncmp(src.data + p, key, key_len) != 0) continue;
        p += key_len;
        while (p < line_end && (src.data[p] == ' ' || src.data[p] == '\t')) p++;
        if (p >= line_end || src.data[p++] != '=') continue;
        while (p < line_end && src.data[p] != '"') p++;
        if (p >= line_end) continue;
        *start = ++p;
        while (p < line_end && src.data[p] != '"') p++;
        if (p >= line_end) continue;
        *end = p;
        return true;
    }
    return false;
}

static ns_bool ns_update_manifest_has_assignment(ns_str src, const char *key) {
    i32 key_len = (i32)strlen(key);
    for (i32 i = 0; i < src.len;) {
        i32 line_start = i;
        while (i < src.len && src.data[i] != '\n') i++;
        i32 line_end = i;
        if (i < src.len) i++;

        i32 p = line_start;
        while (p < line_end && (src.data[p] == ' ' || src.data[p] == '\t' || src.data[p] == '\r')) p++;
        if (line_end - p < key_len || strncmp(src.data + p, key, key_len) != 0) continue;
        p += key_len;
        while (p < line_end && (src.data[p] == ' ' || src.data[p] == '\t')) p++;
        if (p < line_end && src.data[p] == '=') return true;
    }
    return false;
}

static ns_str ns_update_manifest_text(ns_str src, ns_bool *changed) {
    ns_str schema = ns_manifest_value(src, "schema");
    *changed = false;
    if (schema.data == ns_null) {
        if (ns_update_manifest_has_assignment(src, "schema")) {
            ns_exit(1, "update", "cannot parse schema; use schema = \"" NS_MANIFEST_SCHEMA_CURRENT "\".\n");
        }
        ns_str out = ns_str_null;
        ns_str_append_cstr(&out, "schema = \"" NS_MANIFEST_SCHEMA_CURRENT "\"\n");
        ns_str_append(&out, src);
        *changed = true;
        return out;
    }
    if (ns_str_equals(schema, ns_str_cstr(NS_MANIFEST_SCHEMA_CURRENT))) {
        ns_str_free(schema);
        return ns_str_null;
    }
    if (!ns_str_equals(schema, ns_str_cstr(NS_MANIFEST_SCHEMA_LEGACY))) {
        ns_exit(1, "update", "unsupported manifest schema `%.*s`; this ns supports " NS_MANIFEST_SCHEMA_CURRENT ".\n",
                schema.len, schema.data);
    }

    i32 start = 0;
    i32 end = 0;
    if (!ns_update_manifest_value_span(src, "schema", &start, &end)) {
        ns_exit(1, "update", "cannot parse the schema assignment in ns.mod.\n");
    }
    ns_str out = ns_str_null;
    ns_str_append_len(&out, src.data, start);
    ns_str_append_cstr(&out, NS_MANIFEST_SCHEMA_CURRENT);
    ns_str_append_len(&out, src.data + end, src.len - end);
    ns_str_free(schema);
    *changed = true;
    return out;
}

static ns_str ns_update_gitignore_text(ns_str current, ns_bool *changed) {
    ns_str out = ns_str_null;
    if (current.data != ns_null) ns_str_append(&out, current);
    *changed = false;
    for (i32 i = 0; i < (i32)(sizeof(ns_scaffold_gitignore_rules) / sizeof(ns_scaffold_gitignore_rules[0])); i++) {
        const char *rule = ns_scaffold_gitignore_rules[i];
        ns_bool present = current.data != ns_null && ns_update_line_equals(current, rule);
        if (!present && strcmp(rule, "bin") == 0 && current.data != ns_null) {
            present = ns_update_line_equals(current, "bin/");
        }
        if (present) continue;
        if (out.len > 0 && out.data[out.len - 1] != '\n') ns_str_append_cstr(&out, "\n");
        ns_str_append_cstr(&out, rule);
        ns_str_append_cstr(&out, "\n");
        *changed = true;
    }
    return out;
}

static void ns_update_replace(ns_str root, const char *filename, ns_str current, ns_str replacement) {
    ns_str path = ns_path_join(root, ns_str_cstr((char*)filename));
    if (current.data != ns_null) ns_update_backup(root, filename, current);
    ns_update_write_text(path, replacement);
    ns_info("update", "%s %.*s\n", current.data == ns_null ? "created" : "updated", path.len, path.data);
    ns_str_free(path);
}

void ns_exec_update(ns_str path) {
    ns_str start = path.len == 0 ? ns_getcwd() : path;
    ns_str found_root = ns_str_null;
    if (!ns_find_project_root(start, &found_root)) {
        ns_exit(1, "update", "no ns.mod found at or above %.*s.\n", start.len, start.data);
    }
    ns_str root = ns_project_absolute_path(found_root);
    ns_str_free(found_root);
    ns_str manifest_path = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str manifest = ns_os_read_file(manifest_path);
    if (manifest.data == ns_null) ns_exit(1, "update", "cannot read %.*s.\n", manifest_path.len, manifest_path.data);

    i32 changed_count = 0;
    ns_bool changed = false;
    ns_str migrated = ns_update_manifest_text(manifest, &changed);
    if (changed) {
        ns_update_replace(root, "ns.mod", manifest, migrated);
        changed_count++;
    }

    ns_str agents_path = ns_path_join(root, ns_str_cstr("AGENTS.md"));
    ns_str agents = ns_os_read_file(agents_path);
    ns_str canonical_agents = ns_scaffold_agents_text();
    if (agents.data == ns_null || !ns_str_equals(agents, canonical_agents)) {
        ns_update_replace(root, "AGENTS.md", agents, canonical_agents);
        changed_count++;
    }

    ns_str ignore_path = ns_path_join(root, ns_str_cstr(".gitignore"));
    ns_str ignore = ns_os_read_file(ignore_path);
    ns_str merged_ignore = ns_update_gitignore_text(ignore, &changed);
    if (changed) {
        ns_update_replace(root, ".gitignore", ignore, merged_ignore);
        changed_count++;
    }

    if (changed_count == 0) {
        ns_info("update", "%.*s is already up to date (" NS_MANIFEST_SCHEMA_CURRENT ").\n", root.len, root.data);
    } else {
        ns_info("update", "migrated %.*s to " NS_MANIFEST_SCHEMA_CURRENT "; changed %d file%s.\n",
                root.len, root.data, changed_count, changed_count == 1 ? "" : "s");
    }

    ns_str_free(merged_ignore);
    ns_str_free(ignore);
    ns_str_free(ignore_path);
    ns_str_free(canonical_agents);
    ns_str_free(agents);
    ns_str_free(agents_path);
    ns_str_free(migrated);
    ns_str_free(manifest);
    ns_str_free(manifest_path);
    ns_str_free(root);
}

static ns_bool ns_profile_viewer_at(ns_str app) {
    ns_str mod = ns_path_join(app, ns_str_cstr("ns.mod"));
    ns_str entry = ns_path_join(app, ns_str_cstr("main.ns"));
    ns_bool ok = ns_file_exists(mod) && ns_file_exists(entry);
    ns_str_free(entry);
    ns_str_free(mod);
    return ok;
}

static ns_str ns_profile_app_executable(ns_str bundle) {
    if (bundle.data == ns_null || bundle.len == 0) return ns_str_null;
#if defined(NS_DARWIN)
    ns_str macos = ns_path_join(bundle, ns_str_cstr("Contents/MacOS/nscode-profile"));
    if (ns_file_exists(macos)) return macos;
    ns_str_free(macos);
#endif
    if (ns_file_exists(bundle) && !ns_is_dir(bundle)) {
        return ns_str_concat(bundle, ns_str_cstr(""));
    }
    return ns_str_null;
}

static ns_bool ns_profile_compiled_at(ns_str bundle) {
    ns_str exe = ns_profile_app_executable(bundle);
    ns_bool ok = exe.data != ns_null;
    ns_str_free(exe);
    return ok;
}

static ns_str ns_profile_find_compiled(void) {
    ns_str executable = ns_project_current_executable();
    ns_str home = ns_path_home();
    ns_str candidates[6];
    i32 n = 0;
    if (executable.data != ns_null) {
        ns_str bin = ns_path_dirname_safe(executable);
        ns_str root = ns_path_parent(bin);
        candidates[n++] = ns_path_join(bin, ns_str_cstr("nscode-profile.app"));
        candidates[n++] = ns_path_join(root, ns_str_cstr("nscode/profile/bin/nscode-profile.app"));
        candidates[n++] = ns_path_join(root, ns_str_cstr("share/nscode/profile/nscode-profile.app"));
        ns_str_free(bin);
        ns_str_free(root);
    }
    if (home.data != ns_null) {
        candidates[n++] = ns_path_join(home, ns_str_cstr("ns/share/nscode/profile/nscode-profile.app"));
        candidates[n++] = ns_path_join(home, ns_str_cstr("ns/bin/nscode-profile.app"));
    }
    ns_str found = ns_str_null;
    for (i32 i = 0; i < n; i++) {
        if (found.data == ns_null && ns_profile_compiled_at(candidates[i])) {
            found = candidates[i];
        } else {
            ns_str_free(candidates[i]);
        }
    }
    ns_str_free(executable);
    ns_str_free(home);
    return found;
}

static ns_str ns_profile_find_viewer(void) {
    ns_str executable = ns_project_current_executable();
    ns_str home = ns_path_home();
    ns_str candidates[4];
    i32 n = 0;
    if (executable.data != ns_null) {
        ns_str bin = ns_path_dirname_safe(executable);
        ns_str root = ns_path_parent(bin);
        // Source tree: <repo>/nscode/profile
        candidates[n++] = ns_path_join(root, ns_str_cstr("nscode/profile"));
        // `make install`: <prefix>/share/nscode/profile
        candidates[n++] = ns_path_join(root, ns_str_cstr("share/nscode/profile"));
        ns_str_free(bin);
        ns_str_free(root);
    }
    if (home.data != ns_null) {
        candidates[n++] = ns_path_join(home, ns_str_cstr("ns/share/nscode/profile"));
        candidates[n++] = ns_path_join(home, ns_str_cstr("ns/nscode/profile"));
    }
    ns_str found = ns_str_null;
    for (i32 i = 0; i < n; i++) {
        if (found.data == ns_null && ns_profile_viewer_at(candidates[i])) {
            found = candidates[i];
        } else {
            ns_str_free(candidates[i]);
        }
    }
    ns_str_free(executable);
    ns_str_free(home);
    return found;
}

static ns_bool ns_profile_launch_compiled(ns_str bundle) {
    ns_str exe = ns_profile_app_executable(bundle);
    if (exe.data == ns_null) return false;
#if defined(_WIN32)
    ns_str_free(exe);
    return false;
#else
    pid_t pid = fork();
    if (pid == 0) {
        execl(exe.data, exe.data, (char *)0);
        _exit(127);
    }
    ns_str_free(exe);
    if (pid < 0) return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}

static void ns_exec_profile_view(ns_str filename) {
    const char *path = "ns.profile";
    char path_buf[1024];
    if (filename.len > 0) {
        i32 n = filename.len < (i32)sizeof(path_buf) - 1 ? filename.len : (i32)sizeof(path_buf) - 1;
        memcpy(path_buf, filename.data, (size_t)n);
        path_buf[n] = 0;
        path = path_buf;
    }
#if defined(_WIN32)
    _putenv_s("NS_PROFILE_FILE", path);
#else
    setenv("NS_PROFILE_FILE", path, 1);
#endif

    ns_str compiled = ns_profile_find_compiled();
    if (compiled.data != ns_null) {
        ns_info("profile", "opening compiled viewer for %s\n", path);
        if (!ns_profile_launch_compiled(compiled)) {
            ns_warn("profile", "failed to launch compiled viewer %.*s\n", compiled.len, compiled.data);
        }
        ns_str_free(compiled);
        return;
    }

    ns_str app = ns_profile_find_viewer();
    if (app.data == ns_null) {
        ns_warn("profile", "native viewer not found; build it with `ns build nscode/profile`\n");
        return;
    }
    ns_info("profile", "opening interpreted viewer for %s\n", path);
    ns_exec_run(app, 0, false, false);
    ns_str_free(app);
}

// Build the same native artifact as `ns build`, then return the executable
// within it. The caller owns the returned path.
static ns_str ns_linked_run_executable(ns_str filename, ns_str target_name) {
    ns_build_input in = ns_build_input_resolve(filename, target_name);
    ns_build_kind kind = ns_build_resolve_kind(&in, NS_BUILD_AUTO);
    if (kind == NS_BUILD_LIB) {
        ns_exit(1, "run", "cannot run linked library target `%.*s`.\n",
                in.name.len, in.name.data);
    }

    ns_asm_target target;
    ns_asm_get_current_target(&target);
    if (kind == NS_BUILD_APP && target.os != NS_OS_DARWIN) kind = NS_BUILD_EXE;

    ns_str output = ns_build_default_output(&in, kind);
    ns_exec_build_target(filename, ns_str_null, NS_BUILD_AUTO, false, target_name);

#if defined(NS_DARWIN)
    if (kind == NS_BUILD_APP) {
        ns_str macos = ns_path_join(output, ns_str_cstr("Contents/MacOS"));
        ns_str executable = ns_path_join(macos, in.name);
        ns_str_free(macos);
        ns_str_free(output);
        return executable;
    }
#endif
    return output;
}

// Launch a linked run synchronously so `ns run` keeps the program's console,
// environment, working directory and exit status.
static void ns_exec_linked(ns_str executable) {
    if (!ns_file_exists(executable)) {
        ns_exit(1, "run", "linked executable was not produced at %.*s.\n",
                executable.len, executable.data);
    }
    fflush(NULL);
#if defined(_WIN32)
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessA(executable.data, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        ns_exit(1, "run", "failed to launch linked executable %.*s.\n",
                executable.len, executable.data);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    exit((i32)status);
#else
    pid_t pid = fork();
    if (pid < 0) {
        ns_exit(1, "run", "failed to launch linked executable %.*s.\n",
                executable.len, executable.data);
    }
    if (pid == 0) {
        execl(executable.data, executable.data, (char *)0);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        ns_exit(1, "run", "failed while waiting for linked executable %.*s.\n",
                executable.len, executable.data);
    }
    if (WIFEXITED(status)) exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) exit(128 + WTERMSIG(status));
    exit(1);
#endif
}

// `ns run [file.ns | target]`. A bare word naming a `[[targets]]` table of the
// nearest project runs that target; otherwise the argument is a path, and no
// argument at all runs the default target of the current directory.
void ns_exec_run(ns_str argument, i32 port, ns_bool port_set, ns_bool honor_link) {
    ns_str filename = argument;
    ns_str scope = ns_str_null;
    ns_str target_name = ns_str_null;
    ns_bool project = false;

    if (ns_target_arg_select(argument, &scope, &target_name)) {
        filename = ns_manifest_select(scope, target_name).entry_file;
        project = true;
    } else if (argument.len > 0 && !ns_file_exists(argument) && !ns_is_dir(argument)) {
        // A bare word that is neither a path nor a declared target is most
        // likely a typo, so report the targets the project does declare.
        ns_str root = ns_str_null;
        if (!ns_str_has_suffix(argument, ".ns") && ns_find_project_root(ns_getcwd(), &root)) {
            ns_manifest_select(root, argument); // exits with the declared targets
        }
    }

    // No file argument: prefer cwd/ns.mod, then fall back to cwd/main.ns.
    ns_bool implicit = !project && filename.len == 0;
    if (implicit) filename = ns_default_run_entry();
    if (!project && ns_is_dir(filename)) filename = ns_manifest_entry_file_for_root(ns_project_root(filename));

    ns_str source = ns_os_read_file(filename);
    if (source.data == ns_null || source.len == 0)
        ns_exit(1, "ns", "empty file or folder %.*s.\n", filename.len, filename.data);

    if (project) {
        // The selected target already fixed the project scope.
    } else if (implicit) {
        ns_str cwd = ns_getcwd();
        ns_str local_manifest = ns_path_join(cwd, ns_str_cstr("ns.mod"));
        project = ns_file_exists(local_manifest);
        if (project) scope = cwd;
        ns_str_free(local_manifest);
        if (!project) ns_str_free(cwd);
    } else {
        project = ns_find_project_root(filename, &scope);
    }
    if (!project) {
        ns_return_value ret_v = ns_eval(&vm, source, filename);
        if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
        return;
    }

    // An explicit path to a declared entry runs under that target's settings.
    if (target_name.len == 0) target_name = ns_manifest_target_for_entry(scope, filename);
    ns_manifest_selection selection = ns_manifest_select(scope, target_name);
    ns_str target = selection.platform;
    if (target.len > 0 && !ns_str_equals(target, ns_str_cstr("wasm"))) {
        ns_exit(1, "run", "unsupported project target `%.*s`; expected `wasm` or omit target for a native run.\n",
                target.len, target.data);
    }
    if (ns_str_equals(target, ns_str_cstr("wasm"))) {
        setvbuf(stdout, ns_null, _IOLBF, 0);
        ns_exec_build_target(scope, ns_str_null, NS_BUILD_AUTO, false, target_name);
        ns_str output_root = ns_path_join(scope, ns_str_cstr("bin"));
        ns_str executable = ns_project_current_executable();
        char port_text[16];
        snprintf(port_text, sizeof(port_text), "%d", port_set ? port : NS_WASM_DEFAULT_PORT);
#if defined(_WIN32)
        _putenv_s("NS_WASM_ROOT", scope.data);
        _putenv_s("NS_WASM_BIN", output_root.data);
        _putenv_s("NS_WASM_PORT", port_text);
        _putenv_s("NS_EXECUTABLE", executable.data);
#else
        setenv("NS_WASM_ROOT", scope.data, 1);
        setenv("NS_WASM_BIN", output_root.data, 1);
        setenv("NS_WASM_PORT", port_text, 1);
        setenv("NS_EXECUTABLE", executable.data, 1);
#endif
        ns_return_value served = ns_eval(&vm,
            ns_str_cstr("use wasm_dev\nfn main() { wasm_dev_serve() }\n"),
            ns_str_cstr("<wasm-dev-server>"));
        if (ns_return_is_error(served)) ns_return_assert(served);
        return;
    }

    ns_str icon = selection.icon;
    if (icon.data != ns_null) {
#if defined(_WIN32)
        _putenv_s("NS_APP_ICON", icon.data);
#else
        setenv("NS_APP_ICON", icon.data, 1);
#endif
    }
    if (honor_link && selection.link) {
        ns_str executable = ns_linked_run_executable(filename, target_name);
        ns_exec_linked(executable);
        return;
    }
    ns_line_loc *map = ns_null;
    ns_str merged = ns_project_link_all(scope, source, filename, false, &map, ns_null);

    ns_return_value ret_v = ns_eval_with_map(&vm, merged, filename, map);
    if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
}

// Run a single test entry in a fresh VM; returns the entry's i32 exit status
// (0 == all assertions passed).
static i32 ns_run_test_file(ns_str filename) {
    ns_vm tvm = {0};
    ns_configure_vm_runtime_paths(&tvm);
    ns_str source = ns_os_read_file(filename);
    if (source.data == ns_null || source.len == 0) {
        ns_warn("test", "skip empty/unreadable file %.*s.\n", filename.len, filename.data);
        return 1;
    }

    ns_str scope = ns_project_root(filename);
    ns_line_loc *map = ns_null;
    ns_str merged = ns_project_link_all(scope, source, filename, true, &map, ns_null);

    ns_return_value ret_v = ns_eval_with_map(&tvm, merged, filename, map);
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

// Resolve a project directory to the conventional test directory beside its
// ns.mod. An omitted path starts at cwd and may find a manifest in a parent;
// an explicit project directory must contain ns.mod itself so arbitrary test
// directories keep their existing meaning.
static ns_str ns_project_test_dir(ns_str path) {
    ns_str root = path;
    if (root.len == 0) root = ns_project_root(ns_getcwd());

    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_bool is_project = ns_file_exists(manifest);
    ns_str_free(manifest);
    if (!is_project) return ns_str_null;

    return ns_path_join(root, ns_str_cstr("test"));
}

void ns_exec_test(ns_str path) {
    ns_str test_dir = ns_project_test_dir(path);
    if (test_dir.data != ns_null) {
        if (!ns_is_dir(test_dir)) {
            ns_warn("test", "no test directory at %.*s\n", test_dir.len, test_dir.data);
            return;
        }
        ns_exec_test_dir(test_dir);
        return;
    }
    if (ns_is_dir(path)) {
        ns_exec_test_dir(path);
        return;
    }
    i32 code = ns_run_test_file(path);
    if (code != 0) exit(1);
}

// ---------------------------------------------------------------------------
// `ns lint [path]` / `ns lint_fix [path]`
// ---------------------------------------------------------------------------
// Rules come from the `[lint]` table of the nearest ns.mod, so a project owns
// its own style. A file argument lints that file; a directory - or no argument,
// meaning the current directory - lints every `.ns` source below it, honoring
// the manifest's `exclude`. Test sources are ordinary sources here.

static void ns_exec_lint(ns_str path, ns_bool fix) {
    const char *tag = fix ? "lint_fix" : "lint";
    ns_str target = path.len > 0 ? path : ns_getcwd();
    ns_bool directory = ns_is_dir(target);
    if (!directory && !ns_file_exists(target)) ns_exit(1, tag, "cannot read %.*s.\n", target.len, target.data);

    ns_str root = ns_str_null;
    ns_bool has_root = ns_find_project_root(target, &root);
    ns_lint_config cfg;
    ns_lint_config_default(&cfg);
    ns_str *excludes = ns_null;
    if (has_root) {
        ns_str manifest_path = ns_path_join(root, ns_str_cstr("ns.mod"));
        ns_str manifest = ns_os_read_file(manifest_path);
        ns_lint_config_from_manifest(&cfg, manifest);
        excludes = ns_manifest_values(manifest, "exclude");
        ns_str_free(manifest);
        ns_str_free(manifest_path);
    }

    ns_project_source *sources = ns_null;
    if (!directory) {
        ns_array_push(sources, ((ns_project_source){.path = target, .relative = ns_str_null}));
    } else {
        // Linting a project root follows the manifest's source directory; any
        // other directory is scanned as given.
        ns_str scan = target;
        ns_str relative = ns_str_cstr("");
        if (has_root && ns_str_equals(root, target)) {
            scan = ns_project_source_dir(root);
            ns_str source_value = ns_build_manifest_value(root, "source");
            if (source_value.data != ns_null && !ns_str_equals(source_value, ns_str_cstr("."))) relative = source_value;
        }
        ns_project_sources_scan(scan, relative, ns_str_cstr(""), excludes, &sources);
        qsort(sources, ns_array_length(sources), sizeof(ns_project_source), ns_project_source_cmp);
    }

    if (ns_array_length(sources) == 0) {
        ns_warn(tag, "no .ns sources under %.*s\n", target.len, target.data);
        ns_array_free(sources);
        return;
    }

    ns_lint_result result = {0};
    i32 rewritten = 0;
    for (i32 i = 0, count = (i32)ns_array_length(sources); i < count; i++) {
        ns_str file = sources[i].path;
        ns_str source = ns_os_read_file(file);
        if (source.data == ns_null) {
            ns_warn(tag, "cannot read %.*s\n", file.len, file.data);
            continue;
        }
        i32 first = (i32)ns_array_length(result.diagnostics);
        if (fix) {
            ns_str fixed = ns_lint_fix(&cfg, source, file, &result);
            if (fixed.data != ns_null) {
                ns_write_text_file(file, fixed);
                ns_array_free(fixed.data);
                rewritten++;
            }
        } else {
            ns_lint_source(&cfg, source, file, &result);
        }
        for (i32 d = first, n = (i32)ns_array_length(result.diagnostics); d < n; d++) {
            ns_lint_diagnostic_print(&result.diagnostics[d]);
        }
        ns_str_free(source);
    }

    i32 errors = 0, warnings = 0;
    for (i32 d = 0, n = (i32)ns_array_length(result.diagnostics); d < n; d++) {
        ns_lint_diagnostic *diagnostic = &result.diagnostics[d];
        if (diagnostic->fixed && fix) continue;
        if (diagnostic->severity == NS_LINT_ERROR) errors++;
        else warnings++;
    }

    if (fix) {
        ns_info(tag, "%d fixes in %d files; %d errors, %d warnings left in %d files.\n", result.fixed, rewritten, errors, warnings, result.files);
    } else if (errors == 0 && warnings == 0) {
        ns_info(tag, "%d files clean.\n", result.files);
    } else {
        ns_info(tag, "%d errors, %d warnings in %d files (%d fixable, run `ns lint_fix`).\n", errors, warnings, result.files, result.fixable);
    }

    ns_lint_result_free(&result);
    if (directory) {
        for (i32 i = 0, count = (i32)ns_array_length(sources); i < count; i++) {
            ns_str_free(sources[i].path);
            ns_str_free(sources[i].relative);
        }
    }
    for (i32 i = 0, count = (i32)ns_array_length(excludes); i < count; i++) ns_str_free(excludes[i]);
    ns_array_free(excludes);
    ns_array_free(sources);
    if (errors > 0) exit(1);
}

void ns_exec_eval(ns_str filename) {
    if (filename.len == 0) ns_error("ns", "no input file.\n");
    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) ns_exit(1, "ns", "empty file or folder %.*s.\n", filename.len, filename.data);
    ns_return_value ret_v = ns_eval(&vm, source, filename);
    if (ns_return_is_error(ret_v)) ns_return_assert(ret_v);
    ns_str ret = ns_fmt_value(&vm, ret_v.r);
    printf("%.*s\n", ret.len, ret.data);
    ns_str_free(ret);
}

// ---------------------------------------------------------------------------
// `ns --shader <target> file.ns [-o out] [--entry name] [--shader-bin]`
// ---------------------------------------------------------------------------

static ns_bool ns_shader_tool_exists(const char *tool) {
    char probe[256];
#if defined(NS_WIN)
    snprintf(probe, sizeof(probe), "where %s >NUL 2>&1", tool);
#else
    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", tool);
#endif
    return system(probe) == 0;
}

// Best-effort shader binary compilation via the platform toolchain. Never turns
// a successful source emission into a failure: a missing tool only warns.
static void ns_shader_compile_binary(ns_shader_target target, ns_str src_path, ns_str entry_name, ns_shader_stage stage) {
    char cmd[2048];
    ns_str q = ns_shell_quote(src_path);
    switch (target) {
    case NS_SHADER_MSL: {
#if defined(NS_DARWIN)
        if (!ns_shader_tool_exists("xcrun")) {
            ns_warn("shader", "xcrun not found; wrote source only.\n");
            break;
        }
        snprintf(cmd, sizeof(cmd), "xcrun -sdk macosx metal -c %s -o %s.air && xcrun -sdk macosx metallib %s.air -o %s.metallib", q.data, q.data, q.data,
                 q.data);
        if (system(cmd) != 0) ns_warn("shader", "metal compile failed for %.*s.\n", src_path.len, src_path.data);
        else ns_info("shader", "binary %.*s.metallib\n", src_path.len, src_path.data);
#else
        ns_warn("shader", "metallib compilation needs the Apple toolchain; wrote source only.\n");
#endif
    } break;
    case NS_SHADER_GLSL_VULKAN: {
        const char *stage_name = stage == NS_SHADER_STAGE_VERTEX ? "vertex" : stage == NS_SHADER_STAGE_COMPUTE ? "compute" : "fragment";
        if (ns_shader_tool_exists("glslc")) {
            snprintf(cmd, sizeof(cmd), "glslc -fshader-stage=%s %s -o %s.spv", stage_name, q.data, q.data);
        } else if (ns_shader_tool_exists("glslangValidator")) {
            snprintf(cmd, sizeof(cmd), "glslangValidator -V %s -o %s.spv", q.data, q.data);
        } else {
            ns_warn("shader", "glslc / glslangValidator not found; wrote source only.\n");
            break;
        }
        if (system(cmd) != 0) ns_warn("shader", "spir-v compile failed for %.*s.\n", src_path.len, src_path.data);
        else ns_info("shader", "binary %.*s.spv\n", src_path.len, src_path.data);
    } break;
    case NS_SHADER_HLSL: {
        if (!ns_shader_tool_exists("dxc")) {
            ns_warn("shader", "dxc not found; wrote source only.\n");
            break;
        }
        const char *profile = stage == NS_SHADER_STAGE_VERTEX ? "vs_6_0" : stage == NS_SHADER_STAGE_COMPUTE ? "cs_6_0" : "ps_6_0";
        snprintf(cmd, sizeof(cmd), "dxc -T %s -E %.*s %s -Fo %s.%.*s.dxil", profile, entry_name.len, entry_name.data, q.data, q.data, entry_name.len,
                 entry_name.data);
        if (system(cmd) != 0) ns_warn("shader", "dxil compile failed for %.*s.\n", src_path.len, src_path.data);
        else ns_info("shader", "binary %.*s.%.*s.dxil\n", src_path.len, src_path.data, entry_name.len, entry_name.data);
    } break;
    default: break;
    }
    ns_str_free(q);
}

static void ns_shader_emit_output(ns_str output, ns_str suffix, ns_str src, ns_bool bin, ns_shader_target target, ns_str entry_name, ns_shader_stage stage) {
    if (output.len == 0) {
        printf("%.*s", src.len, src.data);
        if (bin) ns_warn("shader", "--shader-bin needs -o <path>; printed source only.\n");
        return;
    }
    ns_str path = suffix.len > 0 ? ns_str_concat(output, suffix) : output;
    ns_build_ensure_output_dir(path);
    ns_write_text_file(path, src);
    ns_info("shader", "wrote %.*s\n", path.len, path.data);
    if (bin) ns_shader_compile_binary(target, path, entry_name, stage);
}

void ns_exec_shader(ns_str filename, ns_str target_s, ns_str entry, ns_str output, ns_bool bin) {
    if (filename.len == 0) ns_exit(1, "shader", "no input file.\n");
    ns_shader_target target = ns_shader_target_from_str(target_s);
    if (target == NS_SHADER_TARGET_UNKNOWN) {
        ns_exit(1, "shader", "unknown target %.*s (expected msl | glsl | hlsl | wgsl).\n", target_s.len, target_s.data);
    }

    ns_str source = ns_os_read_file(filename);
    if (source.len == 0) ns_exit(1, "shader", "empty file or folder %.*s.\n", filename.len, filename.data);
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    if (ns_return_is_error(ret)) ns_return_assert(ret);
    ret = ns_vm_parse(&vm, &ctx); // runs `use` imports so simd structs resolve
    if (ns_return_is_error(ret)) ns_return_assert(ret);

    // entries: --entry name, otherwise every main-TU vs_*/fs_*/ps_*/cs_* fn
    ns_shader_entry_desc *entries = ns_null;
    for (i32 i = 0, l = (i32)ns_array_length(vm.symbols); i < l; ++i) {
        ns_symbol *s = &vm.symbols[i];
        if (s->type != NS_SYMBOL_FN || s->fn.fn.t.ref || s->fn.body == 0) continue;
        if (!(s->lib.len == 0 || ns_str_equals(s->lib, ns_str_cstr("main")))) continue;
        if (entry.len > 0) {
            if (!ns_str_equals(s->name, entry)) continue;
        } else if (!(ns_str_starts_with(s->name, ns_str_cstr("vs_")) || ns_str_starts_with(s->name, ns_str_cstr("fs_")) ||
                     ns_str_starts_with(s->name, ns_str_cstr("ps_")) || ns_str_starts_with(s->name, ns_str_cstr("cs_")))) {
            continue;
        }
        ns_shader_stage stage = ns_shader_stage_infer(&vm, &ctx, i);
        if (stage == NS_SHADER_STAGE_AUTO) {
            ns_exit(1, "shader", "cannot infer the stage of %.*s; name it vs_*/fs_*/cs_* or adjust its signature.\n", s->name.len, s->name.data);
        }
        ns_array_push(entries, ((ns_shader_entry_desc){.fn_index = i, .stage = stage}));
    }
    if (ns_array_length(entries) == 0) {
        if (entry.len > 0) ns_exit(1, "shader", "entry fn %.*s not found.\n", entry.len, entry.data);
        ns_exit(1, "shader", "no shader entry fns found (name them vs_*/fs_*/ps_*/cs_* or pass --entry).\n");
    }

    if (target == NS_SHADER_GLSL_VULKAN) {
        // Vulkan GLSL is one source per stage
        for (i32 i = 0, l = (i32)ns_array_length(entries); i < l; ++i) {
            ns_symbol *s = &vm.symbols[entries[i].fn_index];
            ns_return_str src = ns_shader_transpile_program(&vm, &ctx, &entries[i], 1, target);
            if (ns_return_is_error(src)) ns_return_assert(src);
            ns_str suffix = entries[i].stage == NS_SHADER_STAGE_VERTEX ? ns_str_cstr(".vert")
                            : entries[i].stage == NS_SHADER_STAGE_COMPUTE ? ns_str_cstr(".comp")
                                                                         : ns_str_cstr(".frag");
            if (output.len == 0 && l > 1) printf("// ---- %.*s ----\n", s->name.len, s->name.data);
            ns_shader_emit_output(output, suffix, src.r, bin, target, ns_shader_entry_name(target, s->name), entries[i].stage);
            ns_array_free(src.r.data);
        }
    } else {
        ns_return_str src = ns_shader_transpile_program(&vm, &ctx, entries, (i32)ns_array_length(entries), target);
        if (ns_return_is_error(src)) ns_return_assert(src);
        ns_shader_emit_output(output, ns_str_null, src.r, bin && target == NS_SHADER_MSL, target, ns_str_null, NS_SHADER_STAGE_AUTO);
        if (bin && target == NS_SHADER_HLSL) {
            if (output.len == 0) {
                ns_warn("shader", "--shader-bin needs -o <path>; printed source only.\n");
            } else {
                // dxc compiles per entry point
                for (i32 i = 0, l = (i32)ns_array_length(entries); i < l; ++i) {
                    ns_symbol *s = &vm.symbols[entries[i].fn_index];
                    ns_shader_compile_binary(target, output, ns_shader_entry_name(target, s->name), entries[i].stage);
                }
            }
        }
        ns_array_free(src.r.data);
    }
    ns_array_free(entries);
}

void ns_exec_repl() {
    ns_repl(&vm);
}

static void ns_setenv(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// `os` has no argv, so `ns run` / `ns profile` publish program arguments as
// NS_ARGC plus NS_ARG0, NS_ARG1, ... which a script reads through `os_env`.
static void ns_publish_program_args(i32 argc, i8 **argv) {
    char count_buf[16];
    snprintf(count_buf, sizeof(count_buf), "%d", argc);
    ns_setenv("NS_ARGC", count_buf);
    for (i32 i = 0; i < argc; i++) {
        char name[32];
        snprintf(name, sizeof(name), "NS_ARG%d", i);
        ns_setenv(name, argv && argv[i] ? argv[i] : "");
    }
}

i32 main(i32 argc, i8** argv) {
    ns_compile_option_t option = parse_options(argc, argv);

    if (option.show_help) {
        ns_help(); return 0;
    }

    if (option.show_version) {
        ns_version(); return 0;
    }

    if (option.positional_count > 1) {
        ns_exit(1, "usage", "too many input paths; expected at most one. See `ns --help`.\n");
    }

    if (option.run || option.profile_cmd) {
        ns_publish_program_args(option.program_argc, option.program_argv);
    }

    if (option.profile) ns_profile_begin(argc, argv);

    ns_configure_vm_runtime_paths(&vm);

    if (option.profile_view) {
        ns_exec_profile_view(option.filename);
    } else if (option.profile_cmd) {
        ns_exec_run(option.filename, option.port, option.port_set, false);
    } else if (option.run) {
        ns_exec_run(option.filename, option.port, option.port_set, true);
    } else if (option.test) {
        ns_exec_test(option.filename);
    } else if (option.build) {
        f64 build_start = ns_build_profile_begin("build");
        ns_exec_build(option.filename, option.output, option.build_kind, option.force);
        ns_build_profile_end("build", build_start);
    } else if (option.clean) {
        ns_exec_clean(option.filename);
    } else if (option.project) {
        ns_exec_project(option.filename);
    } else if (option.init) {
        ns_exec_init(option.filename);
    } else if (option.create) {
        ns_exec_create(option.filename);
    } else if (option.update) {
        ns_exec_update(option.filename);
    } else if (option.lint) {
        ns_exec_lint(option.filename, option.lint_fix);
    } else if (option.tokenize_only) {
        ns_exec_tokenize(option.filename);
    } else if (option.ast_only) {
        ns_exec_ast(option.filename);
        ns_mem_status();
    } else if (option.shader_only) {
        ns_exec_shader(option.filename, option.shader_target, option.entry, option.output, option.shader_bin);
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
    if (option.profile) ns_profile_emit(ns_profile_start_ms, argc, argv);
    return 0;
}
