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
#include "ns_agents_md.h"

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(NS_DARWIN)
#include <mach-o/dyld.h>
#endif

#define STB_DS_IMPLEMENTATION
#define NS_MANIFEST_SCHEMA_CURRENT "ns.mod/v1"
#define NS_MANIFEST_SCHEMA_LEGACY "ns.mod/v0"

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
    ns_bool profile: 2; // write ns.profile with elapsed wall-clock time
    ns_bool run: 2;     // `ns run <file>`  - compile project scope and execute
    ns_bool test: 2;    // `ns test <path>` - compile and run test entries
    ns_bool build: 2;   // `ns build <path>` - compile project scope to an artifact
    ns_bool project: 2; // `ns project <path>` - generate a native IDE project
    ns_bool init: 2;    // `ns init [path]` - scaffold an ns project in place
    ns_bool create: 2;  // `ns create <name>` - scaffold an ns project in a new folder
    ns_bool update: 2;  // `ns update [path]` - migrate project metadata to the current format
    ns_bool shader_only: 2; // `ns --shader <target> <file>` - transpile shader fns
    ns_bool shader_bin: 2;  // also compile the emitted source with the platform toolchain
    u8 build_kind;      // 0 auto, 1 executable, 2 library
    i32 positional_count;
    ns_str shader_target;
    ns_str entry;
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
        } else if (i == 1 && strcmp(argv[i], "build") == 0) {
            option.build = true;
        } else if (i == 1 && strcmp(argv[i], "project") == 0) {
            option.project = true;
        } else if (i == 1 && strcmp(argv[i], "init") == 0) {
            option.init = true;
        } else if (i == 1 && strcmp(argv[i], "create") == 0) {
            option.create = true;
        } else if (i == 1 && strcmp(argv[i], "update") == 0) {
            option.update = true;
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
            option.output = ns_str_cstr(argv[i + 1]);
            i++;
        } else {
            if (option.filename.len == 0) {
                option.filename = ns_str_cstr(argv[i]); // unmatched argument is treated as filename
            }
            option.positional_count++;
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
    printf("  --shader <target> transpile shader fns to msl | glsl | hlsl source\n");
    printf("  --entry <name>    shader entry fn (default: every vs_*/fs_*/ps_*/cs_* fn)\n");
    printf("  --shader-bin      also compile the shader source when the platform\n");
    printf("                    toolchain is installed (xcrun metal / glslc / dxc)\n");
    printf("  -s --symbol       print symbol table\n");
    printf("  -v --version      show version\n");
    printf("  -h --help         show this help\n");
    printf("  --profile         write ns.profile: elapsed time plus a per-symbol ffi breakdown\n");
    printf("  -o --output       output path\n");
    printf("\ncommands:\n");
    printf("  init [path]       scaffold an ns project in place (default: cwd)\n");
    printf("                    writes ns.mod, main.ns, README.md, AGENTS.md and .gitignore\n");
    printf("  create <name>     scaffold an ns project in a new <name> folder\n");
    printf("  update [path]     migrate ns.mod and refresh project support files\n");
    printf("  run  [file.ns]    run cwd/ns.mod, otherwise cwd/main.ns, or an explicit input\n");
    printf("  test [path]       run <project>/test/*_test.ns, a test file, or a test dir\n");
    printf("  build [path]      compile and link a script/module to an executable or static lib\n");
    printf("                    uses ns.mod type when path is omitted or a module dir\n");
    printf("                    --exe/--app or --lib/--library can force artifact type\n");
    printf("                    app manifests may set icon = \"path/to/image.png\"\n");
    printf("  project [path]    generate a native IDE project from ns.mod\n");
    printf("                    Darwin: bin/<name>.xcodeproj; Windows: bin/<name>.sln\n");

}

void ns_version() {
    ns_info("nanoscript", "v%d.%d\n", (int)VERSION_MAJOR, (int)VERSION_MINOR);
}

// Order the per-symbol FFI table by descending time so the hottest native
// calls sort to the top of the emitted profile.
static int ns_profile_fn_cmp(const void *a, const void *b) {
    const ns_profile_fn_stat *x = a;
    const ns_profile_fn_stat *y = b;
    if (x->total_ms < y->total_ms) return 1;
    if (x->total_ms > y->total_ms) return -1;
    if (x->calls < y->calls) return 1;
    if (x->calls > y->calls) return -1;
    return 0;
}

static int ns_profile_event_cmp(const void *a, const void *b) {
    const ns_profile_event *x = a;
    const ns_profile_event *y = b;
    if (x->start_ms < y->start_ms) return -1;
    if (x->start_ms > y->start_ms) return 1;
    if (x->kind == NS_PROFILE_EVENT_SCOPE && y->kind != NS_PROFILE_EVENT_SCOPE) return -1;
    if (x->kind != NS_PROFILE_EVENT_SCOPE && y->kind == NS_PROFILE_EVENT_SCOPE) return 1;
    if (x->depth < y->depth) return -1;
    if (x->depth > y->depth) return 1;
    return 0;
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

    f64 ffi_ms = ns_profile.ffi_total_ms;
    f64 ffi_pct = elapsed_ms > 0.0 ? (ffi_ms / elapsed_ms) * 100.0 : 0.0;
    i32 ffi_event_count = 0;
    i32 scope_event_count = 0;
    for (i32 i = 0; i < ns_profile.event_count; i++) {
        ns_profile_event *e = &ns_profile.events[i];
        if (e->kind == NS_PROFILE_EVENT_SCOPE) {
            scope_event_count++;
        } else {
            ffi_event_count++;
        }
    }

    fprintf(f, "format: ns-profile-v3\n");
    fprintf(f, "elapsed_ms: %.3f\n", elapsed_ms);
    fprintf(f, "ffi_calls: %llu\n", (unsigned long long)ns_profile.ffi_calls);
    fprintf(f, "ffi_ms: %.3f\n", ffi_ms);
    fprintf(f, "ffi_pct: %.1f\n", ffi_pct);
    fprintf(f, "ffi_symbols: %d\n", ns_profile.fn_count);
    fprintf(f, "ffi_events: %d\n", ffi_event_count);
    fprintf(f, "scope_calls: %llu\n", (unsigned long long)ns_profile.scope_calls);
    fprintf(f, "scope_events: %d\n", scope_event_count);
    fprintf(f, "timeline_events: %d\n", ns_profile.event_count);
    fprintf(f, "argv:");
    for (i32 i = 0; i < argc; i++) fprintf(f, " %s", argv[i]);
    fprintf(f, "\n");

    qsort(ns_profile.events, ns_profile.event_count, sizeof(ns_profile_event), ns_profile_event_cmp);

    fprintf(f, "timeline: kind depth start_ms duration_ms symbol\n");
    for (i32 i = 0; i < ns_profile.event_count; i++) {
        ns_profile_event *e = &ns_profile.events[i];
        if (e->kind == NS_PROFILE_EVENT_SCOPE) {
            fprintf(f, "scope_event: %d %.3f %.3f ", e->depth, e->start_ms, e->elapsed_ms);
        } else {
            fprintf(f, "ffi_event: %.3f %.3f ", e->start_ms, e->elapsed_ms);
        }
        if (e->lib.len > 0) fprintf(f, "%.*s::", e->lib.len, e->lib.data);
        fprintf(f, "%.*s\n", e->name.len, e->name.data);
    }
    if (ns_profile.events_dropped > 0) {
        fprintf(f, "timeline_events_dropped: %llu\n", (unsigned long long)ns_profile.events_dropped);
    }

    // Per-symbol breakdown, hottest first. Columns:
    //   calls  total_ms  avg_ms  min_ms  max_ms  lib::name
    qsort(ns_profile.fns, ns_profile.fn_count, sizeof(ns_profile_fn_stat), ns_profile_fn_cmp);
    fprintf(f, "ffi_table: calls total_ms avg_ms min_ms max_ms symbol\n");
    for (i32 i = 0; i < ns_profile.fn_count; i++) {
        ns_profile_fn_stat *s = &ns_profile.fns[i];
        f64 avg_ms = s->calls ? s->total_ms / (f64)s->calls : 0.0;
        fprintf(f, "ffi: %llu %.3f %.4f %.4f %.4f ", (unsigned long long)s->calls, s->total_ms, avg_ms, s->min_ms, s->max_ms);
        if (s->lib.len > 0) fprintf(f, "%.*s::", s->lib.len, s->lib.data);
        fprintf(f, "%.*s\n", s->name.len, s->name.data);
    }
    if (ns_profile.fns_dropped > 0) {
        fprintf(f, "ffi_dropped: %llu\n", (unsigned long long)ns_profile.fns_dropped);
    }
    fclose(f);

    ns_info("profile", "wrote ns.profile (%.3f ms total, %llu vm scopes, %llu ffi calls, %.3f ms / %.1f%% in ffi)\n",
            elapsed_ms, (unsigned long long)ns_profile.scope_calls, (unsigned long long)ns_profile.ffi_calls, ffi_ms, ffi_pct);
}

static void ns_profile_emit_at_exit(void) {
    if (!ns_profile.enabled) return;
    ns_profile_emit(ns_profile_start_ms, ns_profile_argc, ns_profile_argv);
}

static void ns_profile_begin(i32 argc, i8 **argv) {
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

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
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

// Extract the first quoted string that follows `key =` in a TOML manifest.
// Handles both `key = "value"` and `key = ["value", ...]`; the key must be the
// first token on its line so `entry` never matches `entries`. Returns a
// heap-owned ns_str, or ns_str_null when the key is absent.
static ns_str ns_manifest_value(ns_str src, const char *key) {
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

// Link all project sources. Files under test/ and other *_test.ns entries are
// excluded by default. Test execution adds only the selected test entry.
static ns_str ns_project_link_all(ns_str root, ns_str entry_src, ns_str entry_file, ns_bool test_entry,
                                  ns_line_loc **out_map, ns_str **out_external_modules) {
    ns_str manifest_path = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str manifest = ns_os_read_file(manifest_path);
    ns_str *excludes = ns_manifest_values(manifest, "exclude");
    ns_str source_dir = ns_project_source_dir(root);
    ns_str source_value = ns_manifest_value(manifest, "source");
    ns_str project_relative = (source_value.data == ns_null || ns_str_equals(source_value, ns_str_cstr(".")))
                                  ? ns_str_cstr("") : source_value;
    ns_project_source *sources = ns_null;
    ns_project_sources_scan(source_dir, project_relative, ns_str_cstr(""), excludes, &sources);
    qsort(sources, ns_array_length(sources), sizeof(ns_project_source), ns_project_source_cmp);

    ns_linker lk = {0};
    lk.scope = source_dir;
    lk.project_all = true;
    for (i32 i = 0, count = ns_array_length(sources); i < count; i++) {
        ns_project_source source = sources[i];
        ns_bool selected = ns_str_equals(source.path, entry_file);
        ns_bool is_test = ns_str_has_suffix(source.relative, "_test.ns");
        ns_bool in_test_dir = strncmp(source.relative.data, "test/", 5) == 0 ||
                              strstr(source.relative.data, "/test/") != ns_null;
        if ((is_test || in_test_dir) && !(test_entry && selected)) continue;
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
        ns_bool is_test = ns_str_has_suffix(source.relative, "_test.ns");
        ns_bool in_test_dir = strncmp(source.relative.data, "test/", 5) == 0 ||
                              strstr(source.relative.data, "/test/") != ns_null;
        if (selected) continue;
        if (is_test || in_test_dir) continue;
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
    return ns_link_finish(&lk, out_map, out_external_modules);
}

// Resolve the entry source file declared by `root/ns.mod`, relative to its
// `source` dir. Supports both `entry = "..."` and `entries = ["...", ...]`.
static ns_str ns_manifest_entry_file_for_root(ns_str root) {
    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str mod = ns_os_read_file(manifest);
    if (mod.data == ns_null)
        ns_exit(1, "ns", "cannot read manifest %.*s.\n", manifest.len, manifest.data);

    ns_str entry = ns_manifest_value(mod, "entry");
    if (entry.data == ns_null) entry = ns_manifest_value(mod, "entries");
    if (entry.data == ns_null)
        ns_exit(1, "ns", "ns.mod at %.*s declares no `entry` to run.\n", root.len, root.data);

    ns_str src_dir = ns_manifest_value(mod, "source");
    ns_str base = (src_dir.data != ns_null && !ns_str_equals(src_dir, ns_str_cstr(".")))
                      ? ns_path_join(root, src_dir)
                      : root;
    return ns_path_join(base, entry);
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

typedef struct ns_build_input {
    ns_str filename;
    ns_str source;
    ns_str scope;
    ns_str name;
    ns_str module_type;
    ns_str icon;
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
    if (kind == NS_BUILD_LIB) {
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

static void ns_str_append_cstr(ns_str *s, const char *cstr) {
    ns_str_append_len(s, cstr, (i32)strlen(cstr));
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

#if defined(NS_DARWIN)
static ns_str ns_c_string_escape(ns_str s) {
    ns_str out = ns_str_null;
    for (i32 i = 0; i < s.len; i++) {
        switch (s.data[i]) {
            case '\\': ns_str_append_len(&out, "\\\\", 2); break;
            case '"': ns_str_append_len(&out, "\\\"", 2); break;
            case '\n': ns_str_append_len(&out, "\\n", 2); break;
            case '\r': ns_str_append_len(&out, "\\r", 2); break;
            case '\t': ns_str_append_len(&out, "\\t", 2); break;
            default: ns_str_append_len(&out, s.data + i, 1); break;
        }
    }
    ns_array_push(out.data, '\0');
    return out;
}
#endif

static void ns_archive_object(ns_str output, ns_str object) {
    ns_str q_output = ns_shell_quote(output);
    ns_str q_object = ns_shell_quote(object);
    ns_str cmd = ns_str_null;
    ns_str_append_len(&cmd, "ar rcs ", 7);
    ns_str_append_len(&cmd, q_output.data, q_output.len);
    ns_str_append_len(&cmd, " ", 1);
    ns_str_append_len(&cmd, q_object.data, q_object.len);
    ns_array_push(cmd.data, '\0');

    i32 ret = system(cmd.data);
    if (ret != 0) {
        ns_exit(1, "build", "failed to archive %.*s.\n", output.len, output.data);
    }

    remove(object.data);
}

static ns_ssa_module *ns_compile_source_to_ssa(ns_str source, ns_str filename, ns_line_loc *line_map) {
    ctx.line_map = line_map;
    ns_return_bool ret = ns_ast_parse(&ctx, source, filename);
    ns_return_assert(ret);

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) ns_return_assert(ssa_ret);
    return ssa_ret.r;
}

static ns_ssa_module *ns_compile_build_input(ns_build_input *in) {
    ns_line_loc *map = ns_null;
    ns_str merged = in->has_manifest
                        ? ns_project_link_all(in->scope, in->source, in->filename, false, &map, ns_null)
                        : ns_project_link(in->scope, in->source, in->filename, &map, ns_null);
    return ns_compile_source_to_ssa(merged, in->filename, map);
}

static ns_build_input ns_build_input_resolve(ns_str path) {
    ns_build_input in = {0};

    if (path.len == 0) {
        ns_str cwd = ns_getcwd();
        in.scope = ns_project_root(cwd);
        in.filename = ns_manifest_entry_file_for_root(in.scope);
        in.has_manifest = true;
    } else if (ns_is_dir(path)) {
        in.scope = ns_project_root(path);
        in.filename = ns_manifest_entry_file_for_root(in.scope);
        in.has_manifest = true;
    } else {
        in.filename = path;
        if (!ns_find_project_root(path, &in.scope)) in.scope = ns_path_dirname_safe(path);
        in.has_manifest = ns_file_exists(ns_path_join(in.scope, ns_str_cstr("ns.mod")));
    }

    in.source = ns_os_read_file(in.filename);
    if (in.source.data == ns_null || in.source.len == 0) {
        ns_exit(1, "build", "empty file or folder %.*s.\n", in.filename.len, in.filename.data);
    }

    if (in.has_manifest) {
        in.name = ns_build_manifest_value(in.scope, "name");
        in.module_type = ns_build_manifest_value(in.scope, "type");
        in.icon = ns_path_resolve(in.scope, ns_build_manifest_value(in.scope, "icon"));
    }
    if (in.name.data == ns_null) in.name = ns_path_filename(in.filename);
    return in;
}

static ns_bool ns_build_type_is_library(ns_str t) {
    return ns_str_equals(t, ns_str_cstr("library")) || ns_str_equals(t, ns_str_cstr("lib"));
}

static ns_bool ns_build_type_is_app(ns_str t) {
    return ns_str_equals(t, ns_str_cstr("app")) || ns_str_equals(t, ns_str_cstr("application"));
}

static ns_build_kind ns_build_resolve_kind(ns_build_input *in, u8 requested) {
    if (requested == NS_BUILD_EXE) return NS_BUILD_EXE;
    if (requested == NS_BUILD_LIB) return NS_BUILD_LIB;
    if (requested == NS_BUILD_APP) return NS_BUILD_APP;
    if (ns_build_type_is_library(in->module_type)) return NS_BUILD_LIB;
    if (ns_build_type_is_app(in->module_type)) return NS_BUILD_APP;
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

static void ns_build_darwin_link_executable(ns_ssa_module *ssa, ns_str executable_path) {
    ns_str object_path = ns_str_concat(executable_path, ns_str_cstr(".o"));
    ns_return_bool emit_ret = ns_macho_emit_object(ssa, object_path);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

    ns_str q_object = ns_shell_quote(object_path);
    ns_str q_executable = ns_shell_quote(executable_path);
    ns_str cmd = ns_str_null;
    ns_str_append_cstr(&cmd, "/usr/bin/clang ");
    ns_str_append(&cmd, q_object);
    ns_str_append_cstr(&cmd, " -o ");
    ns_str_append(&cmd, q_executable);
    ns_array_push(cmd.data, '\0');

    i32 ret = system(cmd.data);
    if (ret != 0) {
        ns_exit(1, "build", "failed to link executable %.*s.\n", executable_path.len, executable_path.data);
    }

    remove(object_path.data);
    ns_str_free(object_path);
    ns_str_free(q_object);
    ns_str_free(q_executable);
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

static void ns_build_darwin_launcher_executable(ns_build_input *in, ns_str executable_path) {
    ns_str ns_path = ns_build_darwin_current_executable();
    if (ns_path.data == ns_null) ns_exit(1, "build", "failed to locate ns executable for app launcher.\n");

    ns_str cwd = ns_getcwd();
    ns_str root_path = ns_str_concat(cwd, ns_str_cstr(""));
    ns_str entry_path = ns_path_resolve(cwd, in->filename);
    ns_str source_path = ns_str_concat(executable_path, ns_str_cstr(".launcher.c"));
    ns_str q_source = ns_shell_quote(source_path);
    ns_str q_executable = ns_shell_quote(executable_path);
    ns_str c_ns_path = ns_c_string_escape(ns_path);
    ns_str c_root_path = ns_c_string_escape(root_path);
    ns_str c_entry_path = ns_c_string_escape(entry_path);

    ns_str source = ns_str_null;
    ns_str_append_cstr(&source, "#include <errno.h>\n");
    ns_str_append_cstr(&source, "#include <stdio.h>\n");
    ns_str_append_cstr(&source, "#include <sys/wait.h>\n");
    ns_str_append_cstr(&source, "#include <unistd.h>\n\n");
    ns_str_append_cstr(&source, "int main(void) {\n");
    ns_str_append_cstr(&source, "    const char *ns = \"");
    ns_str_append(&source, c_ns_path);
    ns_str_append_cstr(&source, "\";\n");
    ns_str_append_cstr(&source, "    const char *root = \"");
    ns_str_append(&source, c_root_path);
    ns_str_append_cstr(&source, "\";\n");
    ns_str_append_cstr(&source, "    const char *entry = \"");
    ns_str_append(&source, c_entry_path);
    ns_str_append_cstr(&source, "\";\n");
    ns_str_append_cstr(&source, "    if (chdir(root) != 0) {\n");
    ns_str_append_cstr(&source, "        perror(\"nscode-native launcher failed to enter project root\");\n");
    ns_str_append_cstr(&source, "        return errno ? errno : 127;\n");
    ns_str_append_cstr(&source, "    }\n");
    ns_str_append_cstr(&source, "    pid_t pid = fork();\n");
    ns_str_append_cstr(&source, "    if (pid == 0) {\n");
    ns_str_append_cstr(&source, "        execl(ns, ns, \"run\", entry, (char*)0);\n");
    ns_str_append_cstr(&source, "        perror(\"nscode-native launcher failed\");\n");
    ns_str_append_cstr(&source, "        _exit(errno ? errno : 127);\n");
    ns_str_append_cstr(&source, "    }\n");
    ns_str_append_cstr(&source, "    if (pid < 0) {\n");
    ns_str_append_cstr(&source, "        perror(\"nscode-native launcher failed to fork\");\n");
    ns_str_append_cstr(&source, "        return errno ? errno : 127;\n");
    ns_str_append_cstr(&source, "    }\n");
    ns_str_append_cstr(&source, "    int status = 0;\n");
    ns_str_append_cstr(&source, "    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}\n");
    ns_str_append_cstr(&source, "    if (WIFEXITED(status)) return WEXITSTATUS(status);\n");
    ns_str_append_cstr(&source, "    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);\n");
    ns_str_append_cstr(&source, "    return status;\n");
    ns_str_append_cstr(&source, "}\n");
    ns_array_push(source.data, '\0');
    ns_write_text_file(source_path, source);

    ns_str cmd = ns_str_null;
    ns_str_append_cstr(&cmd, "/usr/bin/clang ");
    ns_str_append(&cmd, q_source);
    ns_str_append_cstr(&cmd, " -o ");
    ns_str_append(&cmd, q_executable);
    ns_array_push(cmd.data, '\0');

    i32 ret = system(cmd.data);
    if (ret != 0) ns_exit(1, "build", "failed to build app launcher %.*s.\n", executable_path.len, executable_path.data);

    remove(source_path.data);
    ns_str_free(ns_path);
    ns_str_free(cwd);
    ns_str_free(root_path);
    ns_str_free(entry_path);
    ns_str_free(source_path);
    ns_str_free(q_source);
    ns_str_free(q_executable);
    ns_str_free(c_ns_path);
    ns_str_free(c_root_path);
    ns_str_free(c_entry_path);
    ns_str_free(source);
    ns_str_free(cmd);
}

static void ns_build_darwin_app(ns_build_input *in, ns_str output, ns_ssa_module *ssa) {
    ns_str app_dir = ns_build_app_output(output);
    ns_str contents_dir = ns_path_join(app_dir, ns_str_cstr("Contents"));
    ns_str macos_dir = ns_path_join(contents_dir, ns_str_cstr("MacOS"));
    ns_str resources_dir = ns_path_join(contents_dir, ns_str_cstr("Resources"));
    ns_mkdir_p(macos_dir);
    ns_mkdir_p(resources_dir);

    ns_str executable_name = ns_str_concat(in->name, ns_str_cstr(""));
    ns_str executable_path = ns_path_join(macos_dir, executable_name);
    (void)ssa;
    ns_build_darwin_launcher_executable(in, executable_path);

    ns_str icon_file = ns_str_null;
    ns_build_darwin_make_icns(in->icon, resources_dir, &icon_file);
    ns_build_darwin_write_plist(in, app_dir, executable_name, icon_file);
    ns_build_darwin_codesign_app(app_dir);

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

void ns_exec_build(ns_str path, ns_str output, u8 requested_kind) {
    ns_build_input in = ns_build_input_resolve(path);
    ns_build_kind kind = ns_build_resolve_kind(&in, requested_kind);
    if (output.len == 0) output = ns_build_default_output(&in, kind);
    ns_build_ensure_output_dir(output);

    ns_ssa_module *ssa = ns_compile_build_input(&in);

    if (kind == NS_BUILD_LIB) {
        ns_asm_target target;
        ns_asm_get_current_target(&target);
        if (target.os != NS_OS_DARWIN) {
            ns_ssa_module_free(ssa);
            ns_exit(1, "build", "static library output is currently supported for mach-o targets only.\n");
        }

        ns_str object = ns_str_concat(output, ns_str_cstr(".o"));
        ns_return_bool emit_ret = ns_macho_emit_object(ssa, object);
        ns_ssa_module_free(ssa);
        if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);

        ns_archive_object(output, object);
        ns_info("build", "library %.*s\n", output.len, output.data);
        return;
    }

    if (kind == NS_BUILD_APP) {
        ns_asm_target target;
        ns_asm_get_current_target(&target);
        if (target.os != NS_OS_DARWIN) {
            ns_warn("build", "app bundle icon packaging is currently supported for mach-o targets only; emitting executable.\n");
            kind = NS_BUILD_EXE;
        } else {
#if defined(NS_DARWIN)
            ns_str app_output = ns_build_app_output(output);
            ns_build_darwin_app(&in, app_output, ssa);
            ns_ssa_module_free(ssa);
            return;
#else
            ns_ssa_module_free(ssa);
            ns_exit(1, "build", "app bundle output is only available on Darwin builds.\n");
#endif
        }
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
        emit_ret = ns_macho_emit(ssa, output);
#endif
    } else if (target.os == NS_OS_WINDOWS) {
        emit_ret = ns_pe_emit(ssa, output);
    } else {
        ns_ssa_module_free(ssa);
        ns_exit(1, "build", "executable output is not yet supported for this target OS.\n");
        return;
    }

    ns_ssa_module_free(ssa);
    if (ns_return_is_error(emit_ret)) ns_return_assert(emit_ret);
    ns_info("build", "executable %.*s\n", output.len, output.data);
}

static ns_str ns_project_absolute_path(ns_str path) {
    if (ns_path_is_absolute(path)) return ns_str_concat(path, ns_str_cstr(""));
    ns_str cwd = ns_getcwd();
    if (path.len == 0 || ns_str_equals(path, ns_str_cstr("."))) return cwd;
    ns_str absolute = ns_path_join(cwd, path);
    ns_str_free(cwd);
    return absolute;
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
           ns_str_equals(module, ns_str_cstr("shader")) ||
           ns_str_equals(module, ns_str_cstr("simd")) ||
           ns_str_equals(module, ns_str_cstr("view")) ||
           ns_str_equals(module, ns_str_cstr("ui")) ||
           ns_str_equals(module, ns_str_cstr("os")) ||
           ns_str_equals(module, ns_str_cstr("gpu")) ||
           ns_str_equals(module, ns_str_cstr("io"));
}

void ns_exec_project(ns_str path) {
    ns_str start = path;
    if (start.len == 0) start = ns_getcwd();
    ns_str root = ns_project_root(start);
    root = ns_project_absolute_path(root);

    ns_str manifest = ns_path_join(root, ns_str_cstr("ns.mod"));
    ns_str schema = ns_build_manifest_value(root, "schema");
    ns_str name = ns_build_manifest_value(root, "name");
    ns_str module_type = ns_build_manifest_value(root, "type");
    ns_str version = ns_build_manifest_value(root, "version");
    ns_str icon = ns_path_resolve(root, ns_build_manifest_value(root, "icon"));
    if (!ns_str_equals(schema, ns_str_cstr(NS_MANIFEST_SCHEMA_CURRENT))) {
        ns_exit(1, "project", "%.*s must declare schema = \"" NS_MANIFEST_SCHEMA_CURRENT "\".\n",
                manifest.len, manifest.data);
    }
    if (name.data == ns_null || name.len == 0) {
        ns_exit(1, "project", "%.*s must declare a non-empty project name.\n", manifest.len, manifest.data);
    }

    ns_project_kind kind;
    if (ns_build_type_is_app(module_type)) kind = NS_PROJECT_APP;
    else if (ns_build_type_is_library(module_type)) kind = NS_PROJECT_LIBRARY;
    else {
        ns_exit(1, "project", "%.*s has unsupported type `%.*s`; expected app or library.\n",
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
        ns_build_input in = ns_build_input_resolve(root);
        linked = ns_project_link_all(root, in.source, in.filename, false, ns_null, &external_modules);
    }

    ns_bool host_build = false;
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
// Both commands write the same skeleton (ns.mod, main.ns, README.md,
// AGENTS.md, .gitignore). `init` targets an existing directory (default: cwd)
// and keeps any file already present; `create` requires a fresh directory so
// it never collides with user content.

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
    ns_str_append_cstr(&s, "source = \".\"\n");
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
// so `ns init` can fill in the gaps of a partially set up directory.
static void ns_scaffold_write(ns_str root, const char *filename, ns_str text, const char *tag) {
    ns_str path = ns_path_join(root, ns_str_cstr((char*)filename));
    if (ns_file_exists(path)) {
        ns_warn(tag, "skip existing %.*s.\n", path.len, path.data);
    } else {
        ns_write_text_file(path, text);
        ns_info(tag, "wrote %.*s\n", path.len, path.data);
    }
    ns_str_free(path);
    ns_str_free(text);
}

static void ns_scaffold_project(ns_str root, ns_str name, const char *tag) {
    if (!ns_scaffold_name_valid(name)) {
        ns_exit(1, tag, "invalid project name `%.*s`; use letters, digits, `_`, `-` or `.`.\n", name.len, name.data);
    }
    ns_scaffold_write(root, "ns.mod", ns_scaffold_manifest_text(name), tag);
    ns_scaffold_write(root, "main.ns", ns_scaffold_main_text(name), tag);
    ns_scaffold_write(root, "README.md", ns_scaffold_readme_text(name), tag);
    ns_scaffold_write(root, "AGENTS.md", ns_scaffold_agents_text(), tag);
    ns_scaffold_write(root, ".gitignore", ns_scaffold_gitignore_text(), tag);
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

void ns_exec_run(ns_str filename) {
    // No file argument: prefer cwd/ns.mod, then fall back to cwd/main.ns.
    ns_bool implicit = filename.len == 0;
    if (implicit) filename = ns_default_run_entry();
    if (ns_is_dir(filename)) filename = ns_manifest_entry_file_for_root(ns_project_root(filename));

    ns_str source = ns_os_read_file(filename);
    if (source.data == ns_null || source.len == 0)
        ns_exit(1, "ns", "empty file or folder %.*s.\n", filename.len, filename.data);

    ns_str scope = ns_str_null;
    ns_bool project = false;
    if (implicit) {
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

    ns_str icon = ns_path_resolve(scope, ns_build_manifest_value(scope, "icon"));
    if (icon.data != ns_null) {
#if defined(_WIN32)
        _putenv_s("NS_APP_ICON", icon.data);
#else
        setenv("NS_APP_ICON", icon.data, 1);
#endif
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
        ns_exit(1, "shader", "unknown target %.*s (expected msl | glsl | hlsl).\n", target_s.len, target_s.data);
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

    if (option.profile) ns_profile_begin(argc, argv);

    ns_configure_vm_runtime_paths(&vm);

    if (option.run) {
        ns_exec_run(option.filename);
    } else if (option.test) {
        ns_exec_test(option.filename);
    } else if (option.build) {
        ns_exec_build(option.filename, option.output, option.build_kind);
    } else if (option.project) {
        ns_exec_project(option.filename);
    } else if (option.init) {
        ns_exec_init(option.filename);
    } else if (option.create) {
        ns_exec_create(option.filename);
    } else if (option.update) {
        ns_exec_update(option.filename);
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
