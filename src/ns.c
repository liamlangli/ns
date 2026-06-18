#include "ns.h"
#include "ns_ast.h"
#include "ns_token.h"
#include "ns_type.h"
#include "ns_vm.h"
#include "ns_os.h"
#include "ns_toml.h"
#include "ns_asm.h"
#include "ns_pe.h"

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
    ns_bool project_mode: 2; // `ns compile [dir]`: compile a whole folder
    ns_str output;
    ns_str filename;
    ns_str project_dir;
} ns_compile_option_t;

ns_compile_option_t parse_options(i32 argc, i8** argv) {
    ns_compile_option_t option = {0};
    for (i32 i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--token") == 0) {
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
        } else if (strcmp(argv[i], "compile") == 0) {
            option.project_mode = true; // `ns compile [dir]`
        } else if (option.project_mode && option.project_dir.len == 0) {
            option.project_dir = ns_str_cstr(argv[i]); // optional folder after `compile`
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
    printf("       ns compile [dir]   compile every .ns file under dir (default '.') into one binary/lib\n");
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

// ===== project scope compile =====

typedef enum {
    NS_PROJECT_FMT_MACHO,
    NS_PROJECT_FMT_PE,
    NS_PROJECT_FMT_WASM,
} ns_project_fmt;

typedef struct ns_project_config {
    ns_str name;
    ns_bool is_lib;       // emit a library/object instead of an executable
    ns_project_fmt fmt;
    ns_str output;        // explicit output path, or empty to derive one
} ns_project_config;

static ns_project_fmt ns_project_fmt_from_os(ns_os os) {
    switch (os) {
    case NS_OS_DARWIN:  return NS_PROJECT_FMT_MACHO;
    case NS_OS_WINDOWS: return NS_PROJECT_FMT_PE;
    // linux has no native object backend yet; wasm is the portable default
    default:            return NS_PROJECT_FMT_WASM;
    }
}

static ns_str ns_project_fmt_ext(ns_project_fmt fmt, ns_bool is_lib) {
    switch (fmt) {
    case NS_PROJECT_FMT_MACHO: return is_lib ? ns_str_cstr(".o") : ns_str_cstr("");
    case NS_PROJECT_FMT_PE:    return ns_str_cstr(".exe");
    case NS_PROJECT_FMT_WASM:  return ns_str_cstr(".wasm");
    default:                   return ns_str_cstr("");
    }
}

// skip generated artifacts living in a `bin/` directory
static ns_bool ns_project_skip_path(ns_str path) {
    return ns_str_starts_with(path, ns_str_cstr("bin/"))
        || ns_str_index_of(path, ns_str_cstr("/bin/")) >= 0;
}

// resolve build configuration from ns.toml (if present) and CLI overrides
static ns_project_config ns_project_load_config(ns_str dir, ns_compile_option_t option) {
    ns_project_config cfg = {0};
    cfg.name = ns_str_cstr("app");
    cfg.fmt = ns_project_fmt_from_os(NS_OS_UNKNOWN); // overwritten below
    cfg.is_lib = false;

    ns_asm_target host;
    ns_asm_get_current_target(&host);
    cfg.fmt = ns_project_fmt_from_os(host.os);

    ns_str toml_path = ns_path_join(dir, ns_str_cstr("ns.toml"));
    ns_str toml_src = ns_str_null;
    if (ns_fs_exists(toml_path)) {
        toml_src = ns_fs_read_file(toml_path);
        ns_toml toml = ns_toml_parse(toml_src);

        ns_str name = ns_toml_get(&toml, ns_str_cstr("project"), ns_str_cstr("name"));
        if (name.len) cfg.name = name;

        ns_str type = ns_toml_get(&toml, ns_str_cstr("project"), ns_str_cstr("type"));
        if (type.len && (ns_str_equals(type, ns_str_cstr("lib")) || ns_str_equals(type, ns_str_cstr("library")))) {
            cfg.is_lib = true;
        }

        ns_str os = ns_toml_get(&toml, ns_str_cstr("target"), ns_str_cstr("os"));
        if (os.len) {
            if (ns_str_equals(os, ns_str_cstr("darwin")))  cfg.fmt = NS_PROJECT_FMT_MACHO;
            else if (ns_str_equals(os, ns_str_cstr("windows"))) cfg.fmt = NS_PROJECT_FMT_PE;
            else if (ns_str_equals(os, ns_str_cstr("linux")))   cfg.fmt = NS_PROJECT_FMT_WASM;
        }

        ns_str fmt = ns_toml_get(&toml, ns_str_cstr("target"), ns_str_cstr("format"));
        if (fmt.len) {
            if (ns_str_equals(fmt, ns_str_cstr("macho")))     cfg.fmt = NS_PROJECT_FMT_MACHO;
            else if (ns_str_equals(fmt, ns_str_cstr("pe")))   cfg.fmt = NS_PROJECT_FMT_PE;
            else if (ns_str_equals(fmt, ns_str_cstr("wasm"))) cfg.fmt = NS_PROJECT_FMT_WASM;
        }

        ns_str out = ns_toml_get(&toml, ns_str_cstr("build"), ns_str_cstr("output"));
        if (out.len) cfg.output = out;

        ns_toml_free(&toml);
    }
    ns_str_free(toml_path);

    // CLI format flags override the toml target
    if (option.macho_only || option.macho_obj_only) cfg.fmt = NS_PROJECT_FMT_MACHO;
    else if (option.pe_only) cfg.fmt = NS_PROJECT_FMT_PE;
    else if (option.wasm_only) cfg.fmt = NS_PROJECT_FMT_WASM;
    if (option.macho_obj_only) cfg.is_lib = true;

    // -o overrides any toml output
    if (option.output.len) cfg.output = option.output;

    return cfg;
}

void ns_exec_project_compile(ns_str dir, ns_compile_option_t option) {
    if (dir.len == 0) dir = ns_str_cstr(".");

    if (!ns_fs_is_dir(dir)) {
        ns_exit(1, "ns", "compile target '%.*s' is not a directory.\n", dir.len, dir.data);
    }

    ns_project_config cfg = ns_project_load_config(dir, option);

    ns_str *files = ns_fs_list_ext(dir, ns_str_cstr(".ns"));
    if (ns_array_length(files) == 0) {
        ns_fs_list_free(files);
        ns_exit(1, "ns", "no .ns source files found under '%.*s'.\n", dir.len, dir.data);
    }

    // parse every source file into a single ast context so symbols link together
    i32 parsed = 0;
    for (i32 i = 0, l = (i32)ns_array_length(files); i < l; i++) {
        ns_str path = files[i];
        if (ns_project_skip_path(path)) continue;
        ns_str source = ns_fs_read_file(path);
        if (source.len == 0) {
            ns_warn("ns", "skip empty file %.*s.\n", path.len, path.data);
            continue;
        }
        ns_info("compile", "%.*s\n", path.len, path.data);
        ns_return_bool ret = ns_ast_parse(&ctx, source, path);
        if (ns_return_is_error(ret)) {
            ns_fs_list_free(files);
            ns_return_assert(ret);
            return;
        }
        parsed++;
    }

    if (parsed == 0) {
        ns_fs_list_free(files);
        ns_exit(1, "ns", "no compilable .ns source files under '%.*s'.\n", dir.len, dir.data);
    }

    // process the full accumulated section range as one linked unit
    ctx.section_begin = 0;

    ns_return_ptr ssa_ret = ns_ssa_build(&ctx);
    if (ns_return_is_error(ssa_ret)) {
        ns_fs_list_free(files);
        ns_return_assert(ssa_ret);
        return;
    }
    ns_ssa_module *ssa = ssa_ret.r;

    // derive an output path when none was configured
    ns_str output = cfg.output;
    ns_bool output_owned = false;
    if (output.len == 0) {
        ns_str ext = ns_project_fmt_ext(cfg.fmt, cfg.is_lib);
        ns_str bindir = ns_path_join(dir, ns_str_cstr("bin"));
        ns_str base = ns_path_join(bindir, cfg.name);
        output = ns_str_concat(base, ext);
        ns_str_free(bindir);
        ns_str_free(base);
        output_owned = true;
    }
    // create the output directory only when the path actually has one
    // (ns_path_dirname returns the whole string when no separator is present)
    ns_str out_dir = ns_path_dirname(output);
    if (out_dir.len > 0 && out_dir.len < output.len) ns_os_mkdir(out_dir);
    ns_str_free(out_dir);

    ns_return_bool emit_ret;
    const char *kind;
    switch (cfg.fmt) {
    case NS_PROJECT_FMT_MACHO:
        if (cfg.is_lib) { emit_ret = ns_macho_emit_object(ssa, output); kind = "macho-lib"; }
        else { emit_ret = ns_macho_emit(ssa, output); kind = "macho"; }
        break;
    case NS_PROJECT_FMT_PE:
        if (cfg.is_lib) ns_warn("ns", "pe target has no library format; emitting an executable.\n");
        emit_ret = ns_pe_emit(ssa, output); kind = "pe";
        break;
    case NS_PROJECT_FMT_WASM:
        emit_ret = ns_wasm_emit(ssa, output); kind = "wasm";
        break;
    default:
        emit_ret = ns_return_error(bool, ns_code_loc_nil, NS_ERR, "unsupported target format");
        kind = "unknown";
        break;
    }

    ns_ssa_module_free(ssa);
    ns_fs_list_free(files);

    if (ns_return_is_error(emit_ret)) {
        if (output_owned) ns_str_free(output);
        ns_return_assert(emit_ret);
        return;
    }

    ns_info(kind, "linked %d file(s) -> %.*s\n", parsed, output.len, output.data);
    if (output_owned) ns_str_free(output);
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

    if (option.project_mode) {
        ns_exec_project_compile(option.project_dir, option);
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
