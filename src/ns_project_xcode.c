#include "ns_project.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define ns_xcode_mkdir(path) _mkdir(path)
#else
#include <unistd.h>
#define ns_xcode_mkdir(path) mkdir(path, 0755)
#endif

typedef struct ns_xcode_buffer {
    char *data;
    size_t len;
    size_t cap;
} ns_xcode_buffer;

static void ns_xcode_buffer_free(ns_xcode_buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static ns_bool ns_xcode_buffer_reserve(ns_xcode_buffer *buffer, size_t extra) {
    if (extra > (size_t)-1 - buffer->len - 1) return false;
    size_t required = buffer->len + extra + 1;
    if (required <= buffer->cap) return true;
    size_t cap = buffer->cap ? buffer->cap : 1024;
    while (cap < required) {
        if (cap > (size_t)-1 / 2) {
            cap = required;
            break;
        }
        cap *= 2;
    }
    char *data = (char *)realloc(buffer->data, cap);
    if (!data) return false;
    buffer->data = data;
    buffer->cap = cap;
    return true;
}

static ns_bool ns_xcode_buffer_append_len(ns_xcode_buffer *buffer, const char *text, size_t len) {
    if (!ns_xcode_buffer_reserve(buffer, len)) return false;
    if (len) memcpy(buffer->data + buffer->len, text, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return true;
}

static ns_bool ns_xcode_buffer_append(ns_xcode_buffer *buffer, const char *text) {
    return ns_xcode_buffer_append_len(buffer, text, strlen(text));
}

static ns_bool ns_xcode_buffer_appendf(ns_xcode_buffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || !ns_xcode_buffer_reserve(buffer, (size_t)count)) {
        va_end(args);
        return false;
    }
    vsnprintf(buffer->data + buffer->len, (size_t)count + 1, format, args);
    va_end(args);
    buffer->len += (size_t)count;
    return true;
}

static char *ns_xcode_str_dup(ns_str value) {
    if (!value.data || value.len < 0) return NULL;
    char *copy = (char *)malloc((size_t)value.len + 1);
    if (!copy) return NULL;
    memcpy(copy, value.data, (size_t)value.len);
    copy[value.len] = '\0';
    return copy;
}

static char *ns_xcode_path_join(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    ns_bool slash = left_len > 0 && left[left_len - 1] != '/';
    char *path = (char *)malloc(left_len + (size_t)slash + right_len + 1);
    if (!path) return NULL;
    memcpy(path, left, left_len);
    if (slash) path[left_len++] = '/';
    memcpy(path + left_len, right, right_len);
    path[left_len + right_len] = '\0';
    return path;
}

static ns_bool ns_xcode_file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static ns_bool ns_xcode_dir_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static ns_bool ns_xcode_make_dirs(const char *path) {
    if (!path || !path[0]) return false;
    char *copy = (char *)malloc(strlen(path) + 1);
    if (!copy) return false;
    strcpy(copy, path);

    for (char *cursor = copy + 1; *cursor; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (copy[0] && !ns_xcode_dir_exists(copy) && ns_xcode_mkdir(copy) != 0 && errno != EEXIST) {
            fprintf(stderr, "project: cannot create directory %s: %s\n", copy, strerror(errno));
            free(copy);
            return false;
        }
        *cursor = '/';
    }
    if (!ns_xcode_dir_exists(copy) && ns_xcode_mkdir(copy) != 0 && errno != EEXIST) {
        fprintf(stderr, "project: cannot create directory %s: %s\n", copy, strerror(errno));
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static ns_bool ns_xcode_ensure_parent(const char *path) {
    char *copy = (char *)malloc(strlen(path) + 1);
    if (!copy) return false;
    strcpy(copy, path);
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return true;
    }
    *slash = '\0';
    ns_bool result = ns_xcode_make_dirs(copy);
    free(copy);
    return result;
}

static ns_bool ns_xcode_write(const char *path, const char *data, size_t len, ns_bool overwrite) {
    if (!overwrite && ns_xcode_file_exists(path)) return true;
    if (!ns_xcode_ensure_parent(path)) return false;
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "project: cannot write %s: %s\n", path, strerror(errno));
        return false;
    }
    ns_bool ok = len == 0 || fwrite(data, 1, len, file) == len;
    if (fclose(file) != 0) ok = false;
    if (!ok) fprintf(stderr, "project: failed writing %s\n", path);
    return ok;
}

static ns_bool ns_xcode_copy(const char *source, const char *destination) {
    FILE *input = fopen(source, "rb");
    if (!input) {
        fprintf(stderr, "project: required runtime SDK file is missing: %s\n", source);
        return false;
    }
    if (!ns_xcode_ensure_parent(destination)) {
        fclose(input);
        return false;
    }
    FILE *output = fopen(destination, "wb");
    if (!output) {
        fprintf(stderr, "project: cannot write %s: %s\n", destination, strerror(errno));
        fclose(input);
        return false;
    }
    char chunk[16384];
    size_t count;
    ns_bool ok = true;
    while ((count = fread(chunk, 1, sizeof(chunk), input)) != 0) {
        if (fwrite(chunk, 1, count, output) != count) {
            ok = false;
            break;
        }
    }
    if (ferror(input)) ok = false;
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (!ok) fprintf(stderr, "project: failed copying %s to %s\n", source, destination);
    return ok;
}

static char *ns_xcode_escape(const char *text) {
    ns_xcode_buffer escaped = {0};
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        switch (*p) {
        case '\\':
            if (!ns_xcode_buffer_append(&escaped, "\\\\")) goto fail;
            break;
        case '"':
            if (!ns_xcode_buffer_append(&escaped, "\\\"")) goto fail;
            break;
        case '\n':
            if (!ns_xcode_buffer_append(&escaped, "\\n")) goto fail;
            break;
        case '\r':
            if (!ns_xcode_buffer_append(&escaped, "\\r")) goto fail;
            break;
        case '\t':
            if (!ns_xcode_buffer_append(&escaped, "\\t")) goto fail;
            break;
        default:
            if (!ns_xcode_buffer_append_len(&escaped, (const char *)p, 1)) goto fail;
            break;
        }
    }
    if (!escaped.data) {
        escaped.data = (char *)calloc(1, 1);
        if (!escaped.data) return NULL;
    }
    return escaped.data;
fail:
    ns_xcode_buffer_free(&escaped);
    return NULL;
}

static char *ns_xcode_xcconfig_quote(const char *text) {
    ns_xcode_buffer quoted = {0};
    for (const char *p = text; *p; ++p) {
        if (*p == '\\' || *p == '"' || *p == ' ' || *p == '\t' || *p == '#') {
            if (!ns_xcode_buffer_append(&quoted, "\\")) goto fail;
        }
        if (*p == '$' && !ns_xcode_buffer_append(&quoted, "$")) goto fail;
        if (!ns_xcode_buffer_append_len(&quoted, p, 1)) goto fail;
    }
    if (!quoted.data) {
        quoted.data = (char *)calloc(1, 1);
        if (!quoted.data) return NULL;
    }
    return quoted.data;
fail:
    ns_xcode_buffer_free(&quoted);
    return NULL;
}

static char *ns_xcode_xml_escape(const char *text) {
    ns_xcode_buffer escaped = {0};
    for (const char *p = text; *p; ++p) {
        const char *replacement = NULL;
        switch (*p) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&apos;"; break;
        default: break;
        }
        if (replacement) {
            if (!ns_xcode_buffer_append(&escaped, replacement)) goto fail;
        } else if (!ns_xcode_buffer_append_len(&escaped, p, 1)) {
            goto fail;
        }
    }
    if (!escaped.data) {
        escaped.data = (char *)calloc(1, 1);
        if (!escaped.data) return NULL;
    }
    return escaped.data;
fail:
    ns_xcode_buffer_free(&escaped);
    return NULL;
}

static void ns_xcode_id(char out[25], unsigned group, unsigned index) {
    snprintf(out, 25, "%08X%08X%08X", 0x4E535052u, group, index);
}

static const char *const ns_xcode_runtime_sources[] = {
    "ns_fmt.c",
    "ns_type.c",
    "ns_profile.c",
    "ns_os.c",
    "ns_token.c",
    "ns_ast.c",
    "ns_ast_stmt.c",
    "ns_ast_expr.c",
    "ns_ast_print.c",
    "ns_vm_parse.c",
    "ns_vm_eval.c",
    "ns_task.c",
    "ns_vm_lib.c",
    "ns_embedded_ffi.c",
    "ns_vm_print.c",
    "ns_json.c",
    "ns_shader.c",
};

static const char *const ns_xcode_feature_sources[] = {
    "io.c",
    "os.c",
    "os.osx.m",
    "os.ios.m",
    "os.haptic.apple.m",
    "view.c",
    "view.osx.m",
    "view.ios.m",
    "gpu.c",
    "gpu.metal.m",
    "ui.c",
};

static const char *const ns_xcode_feature_headers[] = {
    "os.h",
    "view.h",
    "gpu.h",
    "gpu_const.h",
    "stb_image.h",
    "stb_image_resize2.h",
    "stb_image_write.h",
};

static const char *const ns_xcode_resource_modules[] = {
    "std.ns", "shader.ns", "simd.ns", "view.ns", "ui.ns", "os.ns", "gpu.ns", "io.ns",
};

static const char *const ns_xcode_ui_assets[] = {
    "latin_mono.json", "latin_mono.webp", "latin_mono.png",
};

static const char *const ns_xcode_runtime_headers[] = {
    "ns_type.h",
    "ns_fmt.h",
    "ns_token.h",
    "ns_ast.h",
    "ns_vm.h",
    "os/ns_json.h",
    "ns_shader.h",
    "ns_profile.h",
    "os/ns_os.h",
};

static const size_t ns_xcode_runtime_source_count = sizeof(ns_xcode_runtime_sources) / sizeof(ns_xcode_runtime_sources[0]);
static const size_t ns_xcode_runtime_header_count = sizeof(ns_xcode_runtime_headers) / sizeof(ns_xcode_runtime_headers[0]);
static const size_t ns_xcode_feature_source_count = sizeof(ns_xcode_feature_sources) / sizeof(ns_xcode_feature_sources[0]);
static const size_t ns_xcode_feature_header_count = sizeof(ns_xcode_feature_headers) / sizeof(ns_xcode_feature_headers[0]);
static const size_t ns_xcode_resource_module_count = sizeof(ns_xcode_resource_modules) / sizeof(ns_xcode_resource_modules[0]);
static const size_t ns_xcode_ui_asset_count = sizeof(ns_xcode_ui_assets) / sizeof(ns_xcode_ui_assets[0]);

#define NS_XCODE_RUNTIME_SOURCE_BASE 10u
#define NS_XCODE_FEATURE_SOURCE_BASE (NS_XCODE_RUNTIME_SOURCE_BASE + (unsigned)ns_xcode_runtime_source_count)

static unsigned ns_xcode_resource_file_id(size_t index) {
    return index < 3 ? 5u + (unsigned)index : 50u + (unsigned)index - 3u;
}

static unsigned ns_xcode_asset_file_id(size_t index) {
    return 50u + (unsigned)ns_xcode_resource_module_count - 3u + (unsigned)index;
}

static ns_bool ns_xcode_copy_relative(const char *runtime_root, const char *managed_root, const char *from_dir,
                                      const char *to_dir, const char *relative) {
    char *from_root = ns_xcode_path_join(runtime_root, from_dir);
    char *to_root = ns_xcode_path_join(managed_root, to_dir);
    char *source = from_root ? ns_xcode_path_join(from_root, relative) : NULL;
    char *destination = to_root ? ns_xcode_path_join(to_root, relative) : NULL;
    ns_bool ok = source && destination && ns_xcode_copy(source, destination);
    free(from_root);
    free(to_root);
    free(source);
    free(destination);
    return ok;
}

static ns_bool ns_xcode_copy_ref(const char *runtime_root, const char *managed_root, const char *name) {
    char *ref_root = ns_xcode_path_join(runtime_root, "ref");
    char *source = ref_root ? ns_xcode_path_join(ref_root, name) : NULL;
    if (!source || !ns_xcode_file_exists(source)) {
        free(ref_root);
        free(source);
        ref_root = ns_xcode_path_join(runtime_root, "lib");
        source = ref_root ? ns_xcode_path_join(ref_root, name) : NULL;
    }
    char *resources = ns_xcode_path_join(managed_root, "Resources");
    char *destination = resources ? ns_xcode_path_join(resources, name) : NULL;
    ns_bool ok = source && destination && ns_xcode_copy(source, destination);
    free(ref_root);
    free(source);
    free(resources);
    free(destination);
    return ok;
}

static ns_bool ns_xcode_copy_feature(const char *runtime_root, const char *managed_root, const char *from_dir,
                                     const char *to_dir, const char *name) {
    char *feature_root = ns_xcode_path_join(runtime_root, "feature");
    char *source_root = feature_root ? ns_xcode_path_join(feature_root, from_dir) : NULL;
    char *source = source_root ? ns_xcode_path_join(source_root, name) : NULL;
    if (!source || !ns_xcode_file_exists(source)) {
        free(source_root);
        free(source);
        source_root = ns_xcode_path_join(runtime_root, "lib");
        char *nested = source_root ? ns_xcode_path_join(source_root, from_dir) : NULL;
        free(source_root);
        source_root = nested;
        source = source_root ? ns_xcode_path_join(source_root, name) : NULL;
    }
    char *destination_root = ns_xcode_path_join(managed_root, to_dir);
    char *destination = destination_root ? ns_xcode_path_join(destination_root, name) : NULL;
    ns_bool ok = source && destination && ns_xcode_copy(source, destination);
    free(feature_root);
    free(source_root);
    free(source);
    free(destination_root);
    free(destination);
    return ok;
}

static ns_bool ns_xcode_copy_ui_asset(const char *runtime_root, const char *managed_root, const char *name) {
    char *ref = ns_xcode_path_join(runtime_root, "feature/assets");
    char *source = ref ? ns_xcode_path_join(ref, name) : NULL;
    if (!source || !ns_xcode_file_exists(source)) {
        free(ref);
        free(source);
        ref = ns_xcode_path_join(runtime_root, "ref/assets");
        source = ref ? ns_xcode_path_join(ref, name) : NULL;
    }
    if (!source || !ns_xcode_file_exists(source)) {
        free(ref);
        free(source);
        ref = ns_xcode_path_join(runtime_root, "lib/assets");
        source = ref ? ns_xcode_path_join(ref, name) : NULL;
    }
    char *resources = ns_xcode_path_join(managed_root, "Resources");
    char *destination = resources ? ns_xcode_path_join(resources, name) : NULL;
    ns_bool ok = source && destination && ns_xcode_copy(source, destination);
    free(ref);
    free(source);
    free(resources);
    free(destination);
    return ok;
}

static ns_bool ns_xcode_validate_modules(const char *linked_source) {
    const char *line = linked_source;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if ((size_t)(end - p) < 4 || strncmp(p, "use", 3) != 0 || (p[3] != ' ' && p[3] != '\t')) {
            line = *end ? end + 1 : end;
            continue;
        }
        p += 3;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        const char *start = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) ++p;
        size_t len = (size_t)(p - start);
        if ((len == 3 && strncmp(start, "std", len) == 0) || (len == 6 && strncmp(start, "shader", len) == 0) ||
            (len == 4 && strncmp(start, "simd", len) == 0) || (len == 4 && strncmp(start, "view", len) == 0) ||
            (len == 2 && strncmp(start, "ui", len) == 0) || (len == 2 && strncmp(start, "os", len) == 0) ||
            (len == 3 && strncmp(start, "gpu", len) == 0) || (len == 2 && strncmp(start, "io", len) == 0)) {
            line = *end ? end + 1 : end;
            continue;
        }
        fprintf(stderr,
                "project: module '%.*s' requires external FFI, which generated Apple apps do not support; "
                "use only language modules plus embedded Apple modules std, shader, simd, view, ui, os, gpu, and io\n",
                (int)len, start);
        return false;
    }
    return true;
}

static ns_bool ns_xcode_write_app_sources(const char *managed_root) {
    static const char swift_source[] =
        "import SwiftUI\n"
        "\n"
        "@main\n"
        "struct NSGeneratedApp: App {\n"
        "    @State private var status = \"Ready\"\n"
        "\n"
        "    var body: some Scene {\n"
        "        WindowGroup {\n"
        "            VStack(spacing: 12) {\n"
        "                Text(\"NS\").font(.largeTitle).bold()\n"
        "                Text(status).multilineTextAlignment(.center)\n"
        "            }\n"
        "            .padding(24)\n"
        "            .task {\n"
        "                status = \"Running…\"\n"
        "                let resourceRoot = Bundle.main.resourceURL?.path ?? Bundle.main.bundlePath\n"
        "                status = await Task.detached(priority: .userInitiated) {\n"
        "                    resourceRoot.withCString { root in\n"
        "                        String(cString: ns_run_linked_project(root))\n"
        "                    }\n"
        "                }.value\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";
    static const char bridge_header[] =
        "#pragma once\n"
        "\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n"
        "const char * _Nonnull ns_run_linked_project(const char * _Nonnull resource_root);\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n";
    static const char bridge_source[] =
        "#include \"NSBridge.h\"\n"
        "#include \"ns_vm.h\"\n"
        "#include \"ns_os.h\"\n"
        "\n"
        "#include <stdio.h>\n"
        "\n"
        "static char ns_app_status[1024];\n"
        "\n"
        "const char *ns_run_linked_project(const char *resource_root) {\n"
        "    ns_vm vm = {0};\n"
        "    ns_vm_set_ref_path(&vm, ns_str_cstr((char *)resource_root));\n"
        "    ns_str root = ns_str_cstr((char *)resource_root);\n"
        "    ns_str filename = ns_path_join(root, ns_str_cstr(\"LinkedProject.ns\"));\n"
        "    ns_str source = ns_os_read_file(filename);\n"
        "    if (!source.data) {\n"
        "        snprintf(ns_app_status, sizeof(ns_app_status), \"Could not read %s\", filename.data);\n"
        "        fprintf(stderr, \"ns: %s\\n\", ns_app_status);\n"
        "        ns_str_free(filename);\n"
        "        return ns_app_status;\n"
        "    }\n"
        "    ns_return_value result = ns_eval(&vm, source, filename);\n"
        "    if (ns_return_is_error(result)) {\n"
        "        ns_str state = ns_return_state_str(result.s);\n"
        "        snprintf(ns_app_status, sizeof(ns_app_status), \"%.*s: %.*s\", state.len, state.data, result.e.msg.len, result.e.msg.data);\n"
        "        ns_return_print_error(result.s, result.e);\n"
        "    } else {\n"
        "        snprintf(ns_app_status, sizeof(ns_app_status), \"Finished\");\n"
        "        fprintf(stdout, \"ns: finished\\n\");\n"
        "    }\n"
        "    ns_str_free(source);\n"
        "    ns_str_free(filename);\n"
        "    return ns_app_status;\n"
        "}\n";

    char *sources = ns_xcode_path_join(managed_root, "Sources");
    char *swift = sources ? ns_xcode_path_join(sources, "NSApp.swift") : NULL;
    char *header = sources ? ns_xcode_path_join(sources, "NSBridge.h") : NULL;
    char *bridge = sources ? ns_xcode_path_join(sources, "NSBridge.c") : NULL;
    ns_bool ok = swift && header && bridge && ns_xcode_write(swift, swift_source, sizeof(swift_source) - 1, true) &&
                 ns_xcode_write(header, bridge_header, sizeof(bridge_header) - 1, true) &&
                 ns_xcode_write(bridge, bridge_source, sizeof(bridge_source) - 1, true);
    free(sources);
    free(swift);
    free(header);
    free(bridge);
    return ok;
}

static ns_bool ns_xcode_write_plist(const char *managed_root, const char *platform, const char *safe_name, const char *version) {
    ns_xcode_buffer plist = {0};
    ns_bool mobile = strcmp(platform, "macOS") != 0;
    char *escaped_name = ns_xcode_xml_escape(safe_name);
    char *escaped_version = ns_xcode_xml_escape(version && version[0] ? version : "1.0");
    if (!escaped_name || !escaped_version || !ns_xcode_buffer_appendf(
            &plist,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "  <key>CFBundleDevelopmentRegion</key><string>$(DEVELOPMENT_LANGUAGE)</string>\n"
            "  <key>CFBundleDisplayName</key><string>%s</string>\n"
            "  <key>CFBundleExecutable</key><string>$(EXECUTABLE_NAME)</string>\n"
            "  <key>CFBundleIdentifier</key><string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>\n"
            "  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
            "  <key>CFBundleName</key><string>$(PRODUCT_NAME)</string>\n"
            "  <key>CFBundlePackageType</key><string>APPL</string>\n"
            "  <key>CFBundleShortVersionString</key><string>%s</string>\n"
            "  <key>CFBundleVersion</key><string>1</string>\n"
            "  <key>NSHumanReadableCopyright</key><string></string>\n",
            escaped_name, escaped_version)) {
        free(escaped_name);
        free(escaped_version);
        ns_xcode_buffer_free(&plist);
        return false;
    }
    if (mobile && !ns_xcode_buffer_append(&plist, "  <key>UILaunchScreen</key><dict/>\n")) {
        free(escaped_name);
        free(escaped_version);
        ns_xcode_buffer_free(&plist);
        return false;
    }
    if (!ns_xcode_buffer_append(&plist, "</dict>\n</plist>\n")) {
        free(escaped_name);
        free(escaped_version);
        ns_xcode_buffer_free(&plist);
        return false;
    }
    char filename[64];
    snprintf(filename, sizeof(filename), "%s-Info.plist", platform);
    char *info = ns_xcode_path_join(managed_root, "Info");
    char *path = info ? ns_xcode_path_join(info, filename) : NULL;
    ns_bool ok = path && ns_xcode_write(path, plist.data, plist.len, true);
    free(info);
    free(path);
    free(escaped_name);
    free(escaped_version);
    ns_xcode_buffer_free(&plist);
    return ok;
}

static ns_bool ns_xcode_write_config(const ns_project_spec *spec, const char *managed_root, const char *safe_name,
                                     const char *root, const char *executable) {
    char *config = ns_xcode_path_join(managed_root, "Config");
    char *generated = config ? ns_xcode_path_join(config, "NS.Generated.xcconfig") : NULL;
    char *local = config ? ns_xcode_path_join(config, "NS.Local.xcconfig") : NULL;
    char *quoted_root = ns_xcode_xcconfig_quote(root);
    char *quoted_executable = ns_xcode_xcconfig_quote(executable);
    ns_xcode_buffer contents = {0};
    ns_bool ok = config && generated && local && quoted_root && quoted_executable &&
                 ns_xcode_buffer_appendf(&contents,
                                         "// Generated by ns project. Changes to this file are overwritten.\n"
                                         "NS_PROJECT_ROOT = %s\n"
                                         "NS_EXECUTABLE = %s\n"
                                         "NS_BUNDLE_IDENTIFIER = ns.%s\n"
                                         "SWIFT_VERSION = 5.0\n"
                                         "CLANG_C_LANGUAGE_STANDARD = gnu17\n"
                                         "GCC_PREPROCESSOR_DEFINITIONS = $(inherited) NS_XCLIB=1 NS_DARWIN=1\n"
                                         "HEADER_SEARCH_PATHS = $(inherited) \"$(SRCROOT)/%s.nsproject/Runtime/include\" "
                                         "\"$(SRCROOT)/%s.nsproject/Runtime/include/os\" "
                                         "\"$(SRCROOT)/%s.nsproject/Native/include\"\n"
                                         "#include? \"NS.Local.xcconfig\"\n",
                                         quoted_root, quoted_executable, safe_name, safe_name, safe_name, safe_name) &&
                 ns_xcode_write(generated, contents.data, contents.len, true);
    static const char local_contents[] =
        "// User overrides for the generated NS Xcode project.\n"
        "// This file is created once and is never overwritten by `ns project`.\n";
    if (ok) ok = ns_xcode_write(local, local_contents, sizeof(local_contents) - 1, false);
    ns_unused(spec);
    free(config);
    free(generated);
    free(local);
    free(quoted_root);
    free(quoted_executable);
    ns_xcode_buffer_free(&contents);
    return ok;
}

static ns_bool ns_xcode_refresh_app(const ns_project_spec *spec, const char *managed_root, const char *runtime_root,
                                    const char *linked_source, const char *safe_name, const char *version) {
    if (!ns_xcode_validate_modules(linked_source)) return false;
    if (!ns_xcode_write_app_sources(managed_root)) return false;
    for (size_t i = 0; i < ns_xcode_runtime_source_count; ++i) {
        if (!ns_xcode_copy_relative(runtime_root, managed_root, "src", "Runtime/src", ns_xcode_runtime_sources[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_runtime_header_count; ++i) {
        if (!ns_xcode_copy_relative(runtime_root, managed_root, "include", "Runtime/include", ns_xcode_runtime_headers[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_feature_source_count; ++i) {
        if (!ns_xcode_copy_feature(runtime_root, managed_root, "src", "Native/src", ns_xcode_feature_sources[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_feature_header_count; ++i) {
        if (!ns_xcode_copy_feature(runtime_root, managed_root, "include", "Native/include", ns_xcode_feature_headers[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_resource_module_count; ++i) {
        if (!ns_xcode_copy_ref(runtime_root, managed_root, ns_xcode_resource_modules[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_ui_asset_count; ++i) {
        if (!ns_xcode_copy_ui_asset(runtime_root, managed_root, ns_xcode_ui_assets[i])) return false;
    }
    char *generated = ns_xcode_path_join(managed_root, "Generated");
    char *linked = generated ? ns_xcode_path_join(generated, "LinkedProject.ns") : NULL;
    ns_bool ok = linked && ns_xcode_write(linked, linked_source, strlen(linked_source), true) &&
                 ns_xcode_write_plist(managed_root, "macOS", safe_name, version) &&
                 ns_xcode_write_plist(managed_root, "iOS", safe_name, version) &&
                 ns_xcode_write_plist(managed_root, "visionOS", safe_name, version);
    free(generated);
    free(linked);
    ns_unused(spec);
    return ok;
}

static ns_bool ns_xcode_append_file_reference(ns_xcode_buffer *pbx, unsigned index, const char *file_type, const char *name,
                                               const char *path) {
    char id[25];
    ns_xcode_id(id, 40, index);
    char *escaped_name = ns_xcode_escape(name);
    char *escaped_path = ns_xcode_escape(path);
    ns_bool ok = escaped_name && escaped_path &&
                 ns_xcode_buffer_appendf(pbx, "\t\t%s /* %s */ = {isa = PBXFileReference; lastKnownFileType = %s; name = \"%s\"; "
                                              "path = \"%s\"; sourceTree = \"<group>\"; };\n",
                                         id, escaped_name, file_type, escaped_name, escaped_path);
    free(escaped_name);
    free(escaped_path);
    return ok;
}

static ns_bool ns_xcode_append_build_file(ns_xcode_buffer *pbx, unsigned target, unsigned file, const char *name,
                                          const char *phase) {
    char build_id[25];
    char file_id[25];
    ns_xcode_id(build_id, 50, target * 100 + file);
    ns_xcode_id(file_id, 40, file);
    return ns_xcode_buffer_appendf(pbx,
                                   "\t\t%s /* %s in %s */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };\n",
                                   build_id, name, phase, file_id, name);
}

static ns_bool ns_xcode_append_app_target_config(ns_xcode_buffer *pbx, unsigned target, unsigned variant, const char *target_name,
                                                 const char *safe_name, const char *platform, const char *sdk,
                                                 const char *supported, const char *deployment_key, const char *deployment_value,
                                                 const char *device_family) {
    char config_id[25];
    char generated_id[25];
    ns_xcode_id(config_id, 72, target * 10 + variant);
    ns_xcode_id(generated_id, 40, 8);
    char *escaped_target = ns_xcode_escape(target_name);
    char *escaped_safe = ns_xcode_escape(safe_name);
    const char *frameworks = strcmp(platform, "macOS") == 0
        ? "(\"$(inherited)\", \"-framework\", AppKit, \"-framework\", CoreHaptics, \"-framework\", CoreServices, \"-framework\", Foundation, \"-framework\", Metal, \"-framework\", MetalKit, \"-framework\", QuartzCore)"
        : "(\"$(inherited)\", \"-framework\", CoreHaptics, \"-framework\", Foundation, \"-framework\", Metal, \"-framework\", MetalKit, \"-framework\", QuartzCore, \"-framework\", UIKit)";
    ns_xcode_buffer plist_path = {0};
    ns_xcode_buffer bridge_path = {0};
    ns_bool ok = escaped_target && escaped_safe &&
                 ns_xcode_buffer_appendf(&plist_path, "%s.nsproject/Info/%s-Info.plist", safe_name, platform) &&
                 ns_xcode_buffer_appendf(&bridge_path, "%s.nsproject/Sources/NSBridge.h", safe_name) &&
                 ns_xcode_buffer_appendf(
                     pbx,
                     "\t\t%s /* %s */ = {\n"
                     "\t\t\tisa = XCBuildConfiguration;\n"
                     "\t\t\tbaseConfigurationReference = %s /* NS.Generated.xcconfig */;\n"
                     "\t\t\tbuildSettings = {\n"
                     "\t\t\t\tCODE_SIGN_STYLE = Automatic;\n"
                     "\t\t\t\tDEVELOPMENT_TEAM = \"\";\n"
                     "\t\t\t\tENABLE_USER_SCRIPT_SANDBOXING = NO;\n"
                     "\t\t\t\tGENERATE_INFOPLIST_FILE = NO;\n"
                     "\t\t\t\tINFOPLIST_FILE = \"%s\";\n"
                     "\t\t\t\tOTHER_LDFLAGS = %s;\n"
                     "\t\t\t\tPRODUCT_BUNDLE_IDENTIFIER = \"$(NS_BUNDLE_IDENTIFIER)\";\n"
                     "\t\t\t\tPRODUCT_NAME = \"%s\";\n"
                     "\t\t\t\tSDKROOT = %s;\n"
                     "\t\t\t\tSUPPORTED_PLATFORMS = \"%s\";\n"
                     "\t\t\t\tSWIFT_OBJC_BRIDGING_HEADER = \"%s\";\n"
                     "\t\t\t\tTARGETED_DEVICE_FAMILY = \"%s\";\n"
                     "\t\t\t\t%s = %s;\n"
                     "\t\t\t};\n"
                     "\t\t\tname = %s;\n"
                     "\t\t};\n",
                     config_id, variant == 1 ? "Debug" : "Release", generated_id, plist_path.data, frameworks, escaped_safe, sdk, supported,
                     bridge_path.data, device_family, deployment_key, deployment_value, variant == 1 ? "Debug" : "Release");
    free(escaped_target);
    free(escaped_safe);
    ns_xcode_buffer_free(&plist_path);
    ns_xcode_buffer_free(&bridge_path);
    return ok;
}

static ns_bool ns_xcode_append_project_config(ns_xcode_buffer *pbx, unsigned variant) {
    char config_id[25];
    char generated_id[25];
    ns_xcode_id(config_id, 70, variant);
    ns_xcode_id(generated_id, 40, 8);
    return ns_xcode_buffer_appendf(
        pbx,
        "\t\t%s /* %s */ = {\n"
        "\t\t\tisa = XCBuildConfiguration;\n"
        "\t\t\tbaseConfigurationReference = %s /* NS.Generated.xcconfig */;\n"
        "\t\t\tbuildSettings = {\n"
        "\t\t\t\tALWAYS_SEARCH_USER_PATHS = NO;\n"
        "\t\t\t\tCLANG_ENABLE_MODULES = YES;\n"
        "\t\t\t\tCLANG_ENABLE_OBJC_ARC = NO;\n"
        "\t\t\t\tCLANG_WARN_BOOL_CONVERSION = YES;\n"
        "\t\t\t\tCLANG_WARN_CONSTANT_CONVERSION = YES;\n"
        "\t\t\t\tCLANG_WARN_UNREACHABLE_CODE = YES;\n"
        "\t\t\t\tCOPY_PHASE_STRIP = NO;\n"
        "\t\t\t\tDEBUG_INFORMATION_FORMAT = dwarf;\n"
        "\t\t\t\tGCC_NO_COMMON_BLOCKS = YES;\n"
        "\t\t\t\tGCC_OPTIMIZATION_LEVEL = %s;\n"
        "\t\t\t\tONLY_ACTIVE_ARCH = %s;\n"
        "\t\t\t};\n"
        "\t\t\tname = %s;\n"
        "\t\t};\n",
        config_id, variant == 1 ? "Debug" : "Release", generated_id, variant == 1 ? "0" : "s",
        variant == 1 ? "YES" : "NO", variant == 1 ? "Debug" : "Release");
}

static ns_bool ns_xcode_append_sources_phase(ns_xcode_buffer *pbx, unsigned target) {
    char phase_id[25];
    ns_xcode_id(phase_id, 11, target);
    if (!ns_xcode_buffer_appendf(pbx,
                                 "\t\t%s /* Sources */ = {\n"
                                 "\t\t\tisa = PBXSourcesBuildPhase;\n"
                                 "\t\t\tbuildActionMask = 2147483647;\n"
                                 "\t\t\tfiles = (\n",
                                 phase_id)) {
        return false;
    }
    const unsigned fixed_sources[] = {1, 2};
    const char *const fixed_names[] = {"NSApp.swift", "NSBridge.c"};
    for (size_t i = 0; i < sizeof(fixed_sources) / sizeof(fixed_sources[0]); ++i) {
        char id[25];
        ns_xcode_id(id, 50, target * 100 + fixed_sources[i]);
        if (!ns_xcode_buffer_appendf(pbx, "\t\t\t\t%s /* %s in Sources */,\n", id, fixed_names[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_runtime_source_count; ++i) {
        unsigned file = NS_XCODE_RUNTIME_SOURCE_BASE + (unsigned)i;
        char id[25];
        ns_xcode_id(id, 50, target * 100 + file);
        if (!ns_xcode_buffer_appendf(pbx, "\t\t\t\t%s /* %s in Sources */,\n", id, ns_xcode_runtime_sources[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_feature_source_count; ++i) {
        unsigned file = NS_XCODE_FEATURE_SOURCE_BASE + (unsigned)i;
        char id[25];
        ns_xcode_id(id, 50, target * 100 + file);
        if (!ns_xcode_buffer_appendf(pbx, "\t\t\t\t%s /* %s in Sources */,\n", id, ns_xcode_feature_sources[i])) return false;
    }
    return ns_xcode_buffer_append(pbx,
                                  "\t\t\t);\n"
                                  "\t\t\trunOnlyForDeploymentPostprocessing = 0;\n"
                                  "\t\t};\n");
}

static ns_bool ns_xcode_append_resources_phase(ns_xcode_buffer *pbx, unsigned target) {
    char phase_id[25];
    ns_xcode_id(phase_id, 12, target);
    if (!ns_xcode_buffer_appendf(pbx,
                                 "\t\t%s /* Resources */ = {\n"
                                 "\t\t\tisa = PBXResourcesBuildPhase;\n"
                                 "\t\t\tbuildActionMask = 2147483647;\n"
                                 "\t\t\tfiles = (\n",
                                 phase_id)) {
        return false;
    }
    char linked_id[25];
    ns_xcode_id(linked_id, 50, target * 100 + 4u);
    if (!ns_xcode_buffer_appendf(pbx, "\t\t\t\t%s /* LinkedProject.ns in Resources */,\n", linked_id)) return false;
    for (size_t i = 0; i < ns_xcode_resource_module_count; ++i) {
        char id[25];
        ns_xcode_id(id, 50, target * 100 + ns_xcode_resource_file_id(i));
        if (!ns_xcode_buffer_appendf(pbx, "\t\t\t\t%s /* %s in Resources */,\n", id, ns_xcode_resource_modules[i])) return false;
    }
    for (size_t i = 0; i < ns_xcode_ui_asset_count; ++i) {
        char id[25];
        ns_xcode_id(id, 50, target * 100 + ns_xcode_asset_file_id(i));
        if (!ns_xcode_buffer_appendf(pbx, "\t\t\t\t%s /* %s in Resources */,\n", id, ns_xcode_ui_assets[i])) return false;
    }
    return ns_xcode_buffer_append(pbx,
                                  "\t\t\t);\n"
                                  "\t\t\trunOnlyForDeploymentPostprocessing = 0;\n"
                                  "\t\t};\n");
}

static ns_bool ns_xcode_append_shell_phase(ns_xcode_buffer *pbx, unsigned target, const char *escaped_script) {
    char phase_id[25];
    ns_xcode_id(phase_id, 10, target);
    return ns_xcode_buffer_appendf(
        pbx,
        "\t\t%s /* Refresh NS Project */ = {\n"
        "\t\t\tisa = PBXShellScriptBuildPhase;\n"
        "\t\t\talwaysOutOfDate = 1;\n"
        "\t\t\tbuildActionMask = 2147483647;\n"
        "\t\t\tfiles = ();\n"
        "\t\t\tinputFileListPaths = ();\n"
        "\t\t\tinputPaths = ();\n"
        "\t\t\tname = \"Refresh NS Project\";\n"
        "\t\t\toutputFileListPaths = ();\n"
        "\t\t\toutputPaths = ();\n"
        "\t\t\trunOnlyForDeploymentPostprocessing = 0;\n"
        "\t\t\tshellPath = /bin/sh;\n"
        "\t\t\tshellScript = \"%s\";\n"
        "\t\t};\n",
        phase_id, escaped_script);
}

static ns_bool ns_xcode_append_native_target(ns_xcode_buffer *pbx, unsigned target, const char *target_name,
                                             const char *safe_name) {
    char target_id[25];
    char config_list_id[25];
    char shell_id[25];
    char sources_id[25];
    char resources_id[25];
    char product_id[25];
    ns_xcode_id(target_id, 20, target);
    ns_xcode_id(config_list_id, 73, target);
    ns_xcode_id(shell_id, 10, target);
    ns_xcode_id(sources_id, 11, target);
    ns_xcode_id(resources_id, 12, target);
    ns_xcode_id(product_id, 60, target);
    char *escaped_target = ns_xcode_escape(target_name);
    char *escaped_safe = ns_xcode_escape(safe_name);
    ns_bool ok = escaped_target && escaped_safe &&
                 ns_xcode_buffer_appendf(
                     pbx,
                     "\t\t%s /* %s */ = {\n"
                     "\t\t\tisa = PBXNativeTarget;\n"
                     "\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXNativeTarget \"%s\" */;\n"
                     "\t\t\tbuildPhases = (\n"
                     "\t\t\t\t%s /* Refresh NS Project */,\n"
                     "\t\t\t\t%s /* Sources */,\n"
                     "\t\t\t\t%s /* Resources */,\n"
                     "\t\t\t);\n"
                     "\t\t\tbuildRules = ();\n"
                     "\t\t\tdependencies = ();\n"
                     "\t\t\tname = \"%s\";\n"
                     "\t\t\tproductName = \"%s\";\n"
                     "\t\t\tproductReference = %s /* %s.app */;\n"
                     "\t\t\tproductType = \"com.apple.product-type.application\";\n"
                     "\t\t};\n",
                     target_id, escaped_target, config_list_id, escaped_target, shell_id, sources_id, resources_id, escaped_target,
                     escaped_safe, product_id, escaped_safe);
    free(escaped_target);
    free(escaped_safe);
    return ok;
}

static ns_bool ns_xcode_append_target_config_list(ns_xcode_buffer *pbx, unsigned target, const char *target_name) {
    char list_id[25];
    char debug_id[25];
    char release_id[25];
    ns_xcode_id(list_id, 73, target);
    ns_xcode_id(debug_id, 72, target * 10 + 1);
    ns_xcode_id(release_id, 72, target * 10 + 2);
    char *escaped_target = ns_xcode_escape(target_name);
    ns_bool ok = escaped_target &&
                 ns_xcode_buffer_appendf(
                     pbx,
                     "\t\t%s /* Build configuration list for PBXNativeTarget \"%s\" */ = {\n"
                     "\t\t\tisa = XCConfigurationList;\n"
                     "\t\t\tbuildConfigurations = (\n"
                     "\t\t\t\t%s /* Debug */,\n"
                     "\t\t\t\t%s /* Release */,\n"
                     "\t\t\t);\n"
                     "\t\t\tdefaultConfigurationIsVisible = 0;\n"
                     "\t\t\tdefaultConfigurationName = Release;\n"
                     "\t\t};\n",
                     list_id, escaped_target, debug_id, release_id);
    free(escaped_target);
    return ok;
}

static ns_bool ns_xcode_generate_app_pbx(const ns_project_spec *spec, const char *project_file, const char *safe_name) {
    if (ns_xcode_file_exists(project_file)) return true;

    ns_xcode_buffer script = {0};
    ns_xcode_buffer pbx = {0};
    char *escaped_script = NULL;
    char *escaped_safe = ns_xcode_escape(safe_name);
    if (!escaped_safe ||
        !ns_xcode_buffer_append(&script, "set -e\n\"$NS_EXECUTABLE\" project \"$NS_PROJECT_ROOT\"\n")) {
        goto fail;
    }
    escaped_script = ns_xcode_escape(script.data);
    if (!escaped_script) goto fail;

    if (!ns_xcode_buffer_append(&pbx,
                                "// !$*UTF8*$!\n"
                                "{\n"
                                "\tarchiveVersion = 1;\n"
                                "\tclasses = {};\n"
                                "\tobjectVersion = 56;\n"
                                "\tobjects = {\n\n"
                                "/* Begin PBXBuildFile section */\n")) {
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        if (!ns_xcode_append_build_file(&pbx, target, 1, "NSApp.swift", "Sources") ||
            !ns_xcode_append_build_file(&pbx, target, 2, "NSBridge.c", "Sources") ||
            !ns_xcode_append_build_file(&pbx, target, 4, "LinkedProject.ns", "Resources")) {
            goto fail;
        }
        for (size_t i = 0; i < ns_xcode_resource_module_count; ++i) {
            if (!ns_xcode_append_build_file(&pbx, target, ns_xcode_resource_file_id(i), ns_xcode_resource_modules[i], "Resources")) goto fail;
        }
        for (size_t i = 0; i < ns_xcode_ui_asset_count; ++i) {
            if (!ns_xcode_append_build_file(&pbx, target, ns_xcode_asset_file_id(i), ns_xcode_ui_assets[i], "Resources")) goto fail;
        }
        for (size_t i = 0; i < ns_xcode_runtime_source_count; ++i) {
            if (!ns_xcode_append_build_file(&pbx, target, NS_XCODE_RUNTIME_SOURCE_BASE + (unsigned)i, ns_xcode_runtime_sources[i], "Sources")) goto fail;
        }
        for (size_t i = 0; i < ns_xcode_feature_source_count; ++i) {
            if (!ns_xcode_append_build_file(&pbx, target, NS_XCODE_FEATURE_SOURCE_BASE + (unsigned)i, ns_xcode_feature_sources[i], "Sources")) goto fail;
        }
    }
    if (!ns_xcode_buffer_append(&pbx, "/* End PBXBuildFile section */\n\n/* Begin PBXFileReference section */\n") ||
        !ns_xcode_append_file_reference(&pbx, 1, "sourcecode.swift", "NSApp.swift", "Sources/NSApp.swift") ||
        !ns_xcode_append_file_reference(&pbx, 2, "sourcecode.c.c", "NSBridge.c", "Sources/NSBridge.c") ||
        !ns_xcode_append_file_reference(&pbx, 3, "sourcecode.c.h", "NSBridge.h", "Sources/NSBridge.h") ||
        !ns_xcode_append_file_reference(&pbx, 4, "text", "LinkedProject.ns", "Generated/LinkedProject.ns") ||
        !ns_xcode_append_file_reference(&pbx, 5, "text", "std.ns", "Resources/std.ns") ||
        !ns_xcode_append_file_reference(&pbx, 6, "text", "shader.ns", "Resources/shader.ns") ||
        !ns_xcode_append_file_reference(&pbx, 7, "text", "simd.ns", "Resources/simd.ns") ||
        !ns_xcode_append_file_reference(&pbx, 8, "text.xcconfig", "NS.Generated.xcconfig", "Config/NS.Generated.xcconfig") ||
        !ns_xcode_append_file_reference(&pbx, 9, "text.xcconfig", "NS.Local.xcconfig", "Config/NS.Local.xcconfig")) {
        goto fail;
    }
    for (size_t i = 3; i < ns_xcode_resource_module_count; ++i) {
        ns_xcode_buffer path = {0};
        if (!ns_xcode_buffer_appendf(&path, "Resources/%s", ns_xcode_resource_modules[i]) ||
            !ns_xcode_append_file_reference(&pbx, ns_xcode_resource_file_id(i), "text", ns_xcode_resource_modules[i], path.data)) {
            ns_xcode_buffer_free(&path);
            goto fail;
        }
        ns_xcode_buffer_free(&path);
    }
    for (size_t i = 0; i < ns_xcode_ui_asset_count; ++i) {
        ns_xcode_buffer path = {0};
        const char *type = strstr(ns_xcode_ui_assets[i], ".json") ? "text.json" : "file";
        if (!ns_xcode_buffer_appendf(&path, "Resources/%s", ns_xcode_ui_assets[i]) ||
            !ns_xcode_append_file_reference(&pbx, ns_xcode_asset_file_id(i), type, ns_xcode_ui_assets[i], path.data)) {
            ns_xcode_buffer_free(&path);
            goto fail;
        }
        ns_xcode_buffer_free(&path);
    }
    for (size_t i = 0; i < ns_xcode_runtime_source_count; ++i) {
        ns_xcode_buffer path = {0};
        if (!ns_xcode_buffer_appendf(&path, "Runtime/src/%s", ns_xcode_runtime_sources[i]) ||
            !ns_xcode_append_file_reference(&pbx, NS_XCODE_RUNTIME_SOURCE_BASE + (unsigned)i, "sourcecode.c.c", ns_xcode_runtime_sources[i], path.data)) {
            ns_xcode_buffer_free(&path);
            goto fail;
        }
        ns_xcode_buffer_free(&path);
    }
    for (size_t i = 0; i < ns_xcode_feature_source_count; ++i) {
        ns_xcode_buffer path = {0};
        const char *type = strstr(ns_xcode_feature_sources[i], ".m") ? "sourcecode.c.objc" : "sourcecode.c.c";
        if (!ns_xcode_buffer_appendf(&path, "Native/src/%s", ns_xcode_feature_sources[i]) ||
            !ns_xcode_append_file_reference(&pbx, NS_XCODE_FEATURE_SOURCE_BASE + (unsigned)i, type, ns_xcode_feature_sources[i], path.data)) {
            ns_xcode_buffer_free(&path);
            goto fail;
        }
        ns_xcode_buffer_free(&path);
    }
    for (unsigned target = 1; target <= 3; ++target) {
        char product_id[25];
        ns_xcode_id(product_id, 60, target);
        if (!ns_xcode_buffer_appendf(&pbx,
                                     "\t\t%s /* %s.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; "
                                     "includeInIndex = 0; path = \"%s.app\"; sourceTree = BUILT_PRODUCTS_DIR; };\n",
                                     product_id, escaped_safe, escaped_safe)) {
            goto fail;
        }
    }
    if (!ns_xcode_buffer_append(&pbx, "/* End PBXFileReference section */\n\n/* Begin PBXGroup section */\n")) goto fail;
    char main_group[25], products_group[25], managed_group[25];
    ns_xcode_id(main_group, 2, 1);
    ns_xcode_id(products_group, 2, 2);
    ns_xcode_id(managed_group, 2, 3);
    if (!ns_xcode_buffer_appendf(&pbx,
                                 "\t\t%s = {isa = PBXGroup; children = (%s /* %s.nsproject */, %s /* Products */); "
                                 "sourceTree = \"<group>\"; };\n"
                                 "\t\t%s /* Products */ = {isa = PBXGroup; children = (",
                                 main_group, managed_group, escaped_safe, products_group, products_group)) {
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        char product_id[25];
        ns_xcode_id(product_id, 60, target);
        if (!ns_xcode_buffer_appendf(&pbx, "%s%s /* %s.app */", target == 1 ? "" : ", ", product_id, escaped_safe)) goto fail;
    }
    if (!ns_xcode_buffer_appendf(&pbx,
                                 "); name = Products; sourceTree = \"<group>\"; };\n"
                                 "\t\t%s /* %s.nsproject */ = {\n"
                                 "\t\t\tisa = PBXGroup;\n"
                                 "\t\t\tchildren = (\n",
                                 managed_group, escaped_safe)) {
        goto fail;
    }
    const unsigned managed_fixed[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const char *const managed_names[] = {"NSApp.swift", "NSBridge.c", "NSBridge.h", "LinkedProject.ns", "std.ns",
                                         "shader.ns", "simd.ns", "NS.Generated.xcconfig", "NS.Local.xcconfig"};
    for (size_t i = 0; i < sizeof(managed_fixed) / sizeof(managed_fixed[0]); ++i) {
        char id[25];
        ns_xcode_id(id, 40, managed_fixed[i]);
        if (!ns_xcode_buffer_appendf(&pbx, "\t\t\t\t%s /* %s */,\n", id, managed_names[i])) goto fail;
    }
    for (size_t i = 3; i < ns_xcode_resource_module_count; ++i) {
        char id[25];
        ns_xcode_id(id, 40, ns_xcode_resource_file_id(i));
        if (!ns_xcode_buffer_appendf(&pbx, "\t\t\t\t%s /* %s */,\n", id, ns_xcode_resource_modules[i])) goto fail;
    }
    for (size_t i = 0; i < ns_xcode_ui_asset_count; ++i) {
        char id[25];
        ns_xcode_id(id, 40, ns_xcode_asset_file_id(i));
        if (!ns_xcode_buffer_appendf(&pbx, "\t\t\t\t%s /* %s */,\n", id, ns_xcode_ui_assets[i])) goto fail;
    }
    for (size_t i = 0; i < ns_xcode_runtime_source_count; ++i) {
        char id[25];
        ns_xcode_id(id, 40, NS_XCODE_RUNTIME_SOURCE_BASE + (unsigned)i);
        if (!ns_xcode_buffer_appendf(&pbx, "\t\t\t\t%s /* %s */,\n", id, ns_xcode_runtime_sources[i])) goto fail;
    }
    for (size_t i = 0; i < ns_xcode_feature_source_count; ++i) {
        char id[25];
        ns_xcode_id(id, 40, NS_XCODE_FEATURE_SOURCE_BASE + (unsigned)i);
        if (!ns_xcode_buffer_appendf(&pbx, "\t\t\t\t%s /* %s */,\n", id, ns_xcode_feature_sources[i])) goto fail;
    }
    if (!ns_xcode_buffer_appendf(&pbx,
                                 "\t\t\t);\n"
                                 "\t\t\tname = \"%s.nsproject\";\n"
                                 "\t\t\tpath = \"%s.nsproject\";\n"
                                 "\t\t\tsourceTree = \"<group>\";\n"
                                 "\t\t};\n"
                                 "/* End PBXGroup section */\n\n"
                                 "/* Begin PBXNativeTarget section */\n",
                                 escaped_safe, escaped_safe)) {
        goto fail;
    }
    ns_xcode_buffer target_names[3] = {{0}, {0}, {0}};
    if (!ns_xcode_buffer_appendf(&target_names[0], "%s macOS", safe_name) ||
        !ns_xcode_buffer_appendf(&target_names[1], "%s iOS", safe_name) ||
        !ns_xcode_buffer_appendf(&target_names[2], "%s visionOS", safe_name)) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        if (!ns_xcode_append_native_target(&pbx, target, target_names[target - 1].data, safe_name)) {
            for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
            goto fail;
        }
    }
    if (!ns_xcode_buffer_append(&pbx, "/* End PBXNativeTarget section */\n\n/* Begin PBXProject section */\n")) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    char project_id[25], project_config_list[25];
    ns_xcode_id(project_id, 1, 1);
    ns_xcode_id(project_config_list, 71, 1);
    char target_ids[3][25];
    for (unsigned i = 0; i < 3; ++i) ns_xcode_id(target_ids[i], 20, i + 1);
    if (!ns_xcode_buffer_appendf(
            &pbx,
            "\t\t%s /* Project object */ = {\n"
            "\t\t\tisa = PBXProject;\n"
            "\t\t\tattributes = {\n"
            "\t\t\t\tBuildIndependentTargetsInParallel = YES;\n"
            "\t\t\t\tLastSwiftUpdateCheck = 1600;\n"
            "\t\t\t\tLastUpgradeCheck = 1600;\n"
            "\t\t\t\tTargetAttributes = {\n"
            "\t\t\t\t\t%s = {CreatedOnToolsVersion = 16.0; ProvisioningStyle = Automatic;};\n"
            "\t\t\t\t\t%s = {CreatedOnToolsVersion = 16.0; ProvisioningStyle = Automatic;};\n"
            "\t\t\t\t\t%s = {CreatedOnToolsVersion = 16.0; ProvisioningStyle = Automatic;};\n"
            "\t\t\t\t};\n"
            "\t\t\t};\n"
            "\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXProject */;\n"
            "\t\t\tcompatibilityVersion = \"Xcode 14.0\";\n"
            "\t\t\tdevelopmentRegion = en;\n"
            "\t\t\thasScannedForEncodings = 0;\n"
            "\t\t\tknownRegions = (en, Base);\n"
            "\t\t\tmainGroup = %s;\n"
            "\t\t\tproductRefGroup = %s /* Products */;\n"
            "\t\t\tprojectDirPath = \"\";\n"
            "\t\t\tprojectRoot = \"\";\n"
            "\t\t\ttargets = (%s /* %s */, %s /* %s */, %s /* %s */);\n"
            "\t\t};\n"
            "/* End PBXProject section */\n\n"
            "/* Begin PBXResourcesBuildPhase section */\n",
            project_id, target_ids[0], target_ids[1], target_ids[2], project_config_list, main_group, products_group, target_ids[0],
            target_names[0].data, target_ids[1], target_names[1].data, target_ids[2], target_names[2].data)) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        if (!ns_xcode_append_resources_phase(&pbx, target)) {
            for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
            goto fail;
        }
    }
    if (!ns_xcode_buffer_append(&pbx,
                                "/* End PBXResourcesBuildPhase section */\n\n"
                                "/* Begin PBXShellScriptBuildPhase section */\n")) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        if (!ns_xcode_append_shell_phase(&pbx, target, escaped_script)) {
            for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
            goto fail;
        }
    }
    if (!ns_xcode_buffer_append(&pbx,
                                "/* End PBXShellScriptBuildPhase section */\n\n"
                                "/* Begin PBXSourcesBuildPhase section */\n")) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        if (!ns_xcode_append_sources_phase(&pbx, target)) {
            for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
            goto fail;
        }
    }
    if (!ns_xcode_buffer_append(&pbx,
                                "/* End PBXSourcesBuildPhase section */\n\n"
                                "/* Begin XCBuildConfiguration section */\n") ||
        !ns_xcode_append_project_config(&pbx, 1) || !ns_xcode_append_project_config(&pbx, 2) ||
        !ns_xcode_append_app_target_config(&pbx, 1, 1, target_names[0].data, safe_name, "macOS", "macosx", "macosx",
                                           "MACOSX_DEPLOYMENT_TARGET", "12.0", "6") ||
        !ns_xcode_append_app_target_config(&pbx, 1, 2, target_names[0].data, safe_name, "macOS", "macosx", "macosx",
                                           "MACOSX_DEPLOYMENT_TARGET", "12.0", "6") ||
        !ns_xcode_append_app_target_config(&pbx, 2, 1, target_names[1].data, safe_name, "iOS", "iphoneos",
                                           "iphoneos iphonesimulator", "IPHONEOS_DEPLOYMENT_TARGET", "16.0", "1,2") ||
        !ns_xcode_append_app_target_config(&pbx, 2, 2, target_names[1].data, safe_name, "iOS", "iphoneos",
                                           "iphoneos iphonesimulator", "IPHONEOS_DEPLOYMENT_TARGET", "16.0", "1,2") ||
        !ns_xcode_append_app_target_config(&pbx, 3, 1, target_names[2].data, safe_name, "visionOS", "xros", "xros xrsimulator",
                                           "XROS_DEPLOYMENT_TARGET", "1.0", "7") ||
        !ns_xcode_append_app_target_config(&pbx, 3, 2, target_names[2].data, safe_name, "visionOS", "xros", "xros xrsimulator",
                                           "XROS_DEPLOYMENT_TARGET", "1.0", "7")) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    if (!ns_xcode_buffer_append(&pbx,
                                "/* End XCBuildConfiguration section */\n\n"
                                "/* Begin XCConfigurationList section */\n")) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    char project_debug[25], project_release[25];
    ns_xcode_id(project_debug, 70, 1);
    ns_xcode_id(project_release, 70, 2);
    if (!ns_xcode_buffer_appendf(&pbx,
                                 "\t\t%s /* Build configuration list for PBXProject */ = {\n"
                                 "\t\t\tisa = XCConfigurationList;\n"
                                 "\t\t\tbuildConfigurations = (%s /* Debug */, %s /* Release */);\n"
                                 "\t\t\tdefaultConfigurationIsVisible = 0;\n"
                                 "\t\t\tdefaultConfigurationName = Release;\n"
                                 "\t\t};\n",
                                 project_config_list, project_debug, project_release)) {
        for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
        goto fail;
    }
    for (unsigned target = 1; target <= 3; ++target) {
        if (!ns_xcode_append_target_config_list(&pbx, target, target_names[target - 1].data)) {
            for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
            goto fail;
        }
    }
    for (unsigned i = 0; i < 3; ++i) ns_xcode_buffer_free(&target_names[i]);
    if (!ns_xcode_buffer_appendf(&pbx,
                                 "/* End XCConfigurationList section */\n"
                                 "\t};\n"
                                 "\trootObject = %s /* Project object */;\n"
                                 "}\n",
                                 project_id) ||
        !ns_xcode_write(project_file, pbx.data, pbx.len, false)) {
        goto fail;
    }
    free(escaped_script);
    free(escaped_safe);
    ns_xcode_buffer_free(&script);
    ns_xcode_buffer_free(&pbx);
    ns_unused(spec);
    return true;

fail:
    fprintf(stderr, "project: failed to generate Xcode app project\n");
    free(escaped_script);
    free(escaped_safe);
    ns_xcode_buffer_free(&script);
    ns_xcode_buffer_free(&pbx);
    return false;
}

static ns_bool ns_xcode_append_legacy_config(ns_xcode_buffer *pbx, unsigned target, unsigned variant) {
    char config_id[25];
    char generated_id[25];
    ns_xcode_id(config_id, 74, target * 10 + variant);
    ns_xcode_id(generated_id, 40, 8);
    return ns_xcode_buffer_appendf(
        pbx,
        "\t\t%s /* %s */ = {\n"
        "\t\t\tisa = XCBuildConfiguration;\n"
        "\t\t\tbaseConfigurationReference = %s /* NS.Generated.xcconfig */;\n"
        "\t\t\tbuildSettings = {};\n"
        "\t\t\tname = %s;\n"
        "\t\t};\n",
        config_id, variant == 1 ? "Debug" : "Release", generated_id, variant == 1 ? "Debug" : "Release");
}

static ns_bool ns_xcode_append_legacy_config_list(ns_xcode_buffer *pbx, unsigned target, const char *target_name) {
    char list_id[25], debug_id[25], release_id[25];
    ns_xcode_id(list_id, 75, target);
    ns_xcode_id(debug_id, 74, target * 10 + 1);
    ns_xcode_id(release_id, 74, target * 10 + 2);
    char *escaped_target = ns_xcode_escape(target_name);
    ns_bool ok = escaped_target &&
                 ns_xcode_buffer_appendf(
                     pbx,
                     "\t\t%s /* Build configuration list for PBXLegacyTarget \"%s\" */ = {\n"
                     "\t\t\tisa = XCConfigurationList;\n"
                     "\t\t\tbuildConfigurations = (%s /* Debug */, %s /* Release */);\n"
                     "\t\t\tdefaultConfigurationIsVisible = 0;\n"
                     "\t\t\tdefaultConfigurationName = Release;\n"
                     "\t\t};\n",
                     list_id, escaped_target, debug_id, release_id);
    free(escaped_target);
    return ok;
}

static ns_bool ns_xcode_append_legacy_target(ns_xcode_buffer *pbx, unsigned target, const char *target_name,
                                             const char *command) {
    char target_id[25], config_list_id[25];
    ns_xcode_id(target_id, 21, target);
    ns_xcode_id(config_list_id, 75, target);
    char *escaped_target = ns_xcode_escape(target_name);
    ns_bool ok = escaped_target &&
                 ns_xcode_buffer_appendf(
                     pbx,
                     "\t\t%s /* %s */ = {\n"
                     "\t\t\tisa = PBXLegacyTarget;\n"
                     "\t\t\tbuildArgumentsString = \"%s\";\n"
                     "\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXLegacyTarget \"%s\" */;\n"
                     "\t\t\tbuildPhases = ();\n"
                     "\t\t\tbuildToolPath = \"%s\";\n"
                     "\t\t\tbuildWorkingDirectory = \"%s\";\n"
                     "\t\t\tdependencies = ();\n"
                     "\t\t\tname = \"%s\";\n"
                     "\t\t\tpassBuildSettingsInEnvironment = 1;\n"
                     "\t\t\tproductName = \"%s\";\n"
                     "\t\t};\n",
                     target_id, escaped_target, command, config_list_id, escaped_target, "$(NS_EXECUTABLE)", "$(NS_PROJECT_ROOT)",
                     escaped_target, escaped_target);
    free(escaped_target);
    return ok;
}

static ns_bool ns_xcode_generate_library_pbx(const char *project_file, const char *safe_name) {
    if (ns_xcode_file_exists(project_file)) return true;
    ns_xcode_buffer pbx = {0};
    char *escaped_safe = ns_xcode_escape(safe_name);
    if (!escaped_safe ||
        !ns_xcode_buffer_append(&pbx,
                                "// !$*UTF8*$!\n"
                                "{\n"
                                "\tarchiveVersion = 1;\n"
                                "\tclasses = {};\n"
                                "\tobjectVersion = 56;\n"
                                "\tobjects = {\n\n"
                                "/* Begin PBXFileReference section */\n") ||
        !ns_xcode_append_file_reference(&pbx, 8, "text.xcconfig", "NS.Generated.xcconfig", "Config/NS.Generated.xcconfig") ||
        !ns_xcode_append_file_reference(&pbx, 9, "text.xcconfig", "NS.Local.xcconfig", "Config/NS.Local.xcconfig")) {
        goto fail;
    }
    char main_group[25], managed_group[25], generated_ref[25], local_ref[25];
    ns_xcode_id(main_group, 2, 1);
    ns_xcode_id(managed_group, 2, 3);
    ns_xcode_id(generated_ref, 40, 8);
    ns_xcode_id(local_ref, 40, 9);
    if (!ns_xcode_buffer_appendf(
            &pbx,
            "/* End PBXFileReference section */\n\n"
            "/* Begin PBXGroup section */\n"
            "\t\t%s = {isa = PBXGroup; children = (%s /* %s.nsproject */); sourceTree = \"<group>\";};\n"
            "\t\t%s /* %s.nsproject */ = {isa = PBXGroup; children = (%s /* NS.Generated.xcconfig */, "
            "%s /* NS.Local.xcconfig */); name = \"%s.nsproject\"; path = \"%s.nsproject\"; sourceTree = \"<group>\";};\n"
            "/* End PBXGroup section */\n\n"
            "/* Begin PBXLegacyTarget section */\n",
            main_group, managed_group, escaped_safe, managed_group, escaped_safe, generated_ref, local_ref, escaped_safe,
            escaped_safe) ||
        !ns_xcode_append_legacy_target(&pbx, 1, "NS Build", "build \\\"$(NS_PROJECT_ROOT)\\\"") ||
        !ns_xcode_append_legacy_target(&pbx, 2, "NS Test", "test \\\"$(NS_PROJECT_ROOT)\\\"") ||
        !ns_xcode_buffer_append(&pbx,
                                "/* End PBXLegacyTarget section */\n\n"
                                "/* Begin PBXProject section */\n")) {
        goto fail;
    }
    char project_id[25], project_list[25], build_target[25], test_target[25];
    ns_xcode_id(project_id, 1, 1);
    ns_xcode_id(project_list, 71, 1);
    ns_xcode_id(build_target, 21, 1);
    ns_xcode_id(test_target, 21, 2);
    if (!ns_xcode_buffer_appendf(
            &pbx,
            "\t\t%s /* Project object */ = {\n"
            "\t\t\tisa = PBXProject;\n"
            "\t\t\tattributes = {BuildIndependentTargetsInParallel = YES; LastUpgradeCheck = 1600;};\n"
            "\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXProject */;\n"
            "\t\t\tcompatibilityVersion = \"Xcode 14.0\";\n"
            "\t\t\tdevelopmentRegion = en;\n"
            "\t\t\thasScannedForEncodings = 0;\n"
            "\t\t\tknownRegions = (en, Base);\n"
            "\t\t\tmainGroup = %s;\n"
            "\t\t\tprojectDirPath = \"\";\n"
            "\t\t\tprojectRoot = \"\";\n"
            "\t\t\ttargets = (%s /* NS Build */, %s /* NS Test */);\n"
            "\t\t};\n"
            "/* End PBXProject section */\n\n"
            "/* Begin XCBuildConfiguration section */\n",
            project_id, project_list, main_group, build_target, test_target) ||
        !ns_xcode_append_project_config(&pbx, 1) || !ns_xcode_append_project_config(&pbx, 2) ||
        !ns_xcode_append_legacy_config(&pbx, 1, 1) || !ns_xcode_append_legacy_config(&pbx, 1, 2) ||
        !ns_xcode_append_legacy_config(&pbx, 2, 1) || !ns_xcode_append_legacy_config(&pbx, 2, 2) ||
        !ns_xcode_buffer_append(&pbx,
                                "/* End XCBuildConfiguration section */\n\n"
                                "/* Begin XCConfigurationList section */\n")) {
        goto fail;
    }
    char debug_id[25], release_id[25];
    ns_xcode_id(debug_id, 70, 1);
    ns_xcode_id(release_id, 70, 2);
    if (!ns_xcode_buffer_appendf(
            &pbx,
            "\t\t%s /* Build configuration list for PBXProject */ = {isa = XCConfigurationList; "
            "buildConfigurations = (%s /* Debug */, %s /* Release */); defaultConfigurationIsVisible = 0; "
            "defaultConfigurationName = Release;};\n",
            project_list, debug_id, release_id) ||
        !ns_xcode_append_legacy_config_list(&pbx, 1, "NS Build") ||
        !ns_xcode_append_legacy_config_list(&pbx, 2, "NS Test") ||
        !ns_xcode_buffer_appendf(&pbx,
                                 "/* End XCConfigurationList section */\n"
                                 "\t};\n"
                                 "\trootObject = %s /* Project object */;\n"
                                 "}\n",
                                 project_id) ||
        !ns_xcode_write(project_file, pbx.data, pbx.len, false)) {
        goto fail;
    }
    free(escaped_safe);
    ns_xcode_buffer_free(&pbx);
    return true;

fail:
    fprintf(stderr, "project: failed to generate Xcode library project\n");
    free(escaped_safe);
    ns_xcode_buffer_free(&pbx);
    return false;
}

ns_bool ns_project_generate_xcode(const ns_project_spec *spec) {
    if (!spec || !spec->root.data || !spec->safe_name.data || !spec->ns_executable.data) {
        fprintf(stderr, "project: incomplete Xcode project specification\n");
        return false;
    }

    char *root = ns_xcode_str_dup(spec->root);
    char *safe_name = ns_xcode_str_dup(spec->safe_name);
    char *version = ns_xcode_str_dup(spec->version);
    char *executable = ns_xcode_str_dup(spec->ns_executable);
    char *runtime_root = ns_xcode_str_dup(spec->runtime_root);
    char *linked_source = ns_xcode_str_dup(spec->linked_source);
    char *bin = root ? ns_xcode_path_join(root, "bin") : NULL;
    ns_xcode_buffer project_basename = {0};
    ns_xcode_buffer managed_basename = {0};
    char *project_dir = NULL;
    char *project_file = NULL;
    char *managed_root = NULL;
    ns_bool ok = root && safe_name && version && executable && bin && safe_name[0] && strchr(safe_name, '/') == NULL &&
                 strchr(safe_name, '\\') == NULL && strcmp(safe_name, ".") != 0 && strcmp(safe_name, "..") != 0 &&
                 ns_xcode_buffer_appendf(&project_basename, "%s.xcodeproj", safe_name) &&
                 ns_xcode_buffer_appendf(&managed_basename, "%s.nsproject", safe_name);
    if (!ok) {
        fprintf(stderr, "project: invalid or out-of-memory Xcode project specification\n");
        goto cleanup;
    }
    project_dir = ns_xcode_path_join(bin, project_basename.data);
    project_file = project_dir ? ns_xcode_path_join(project_dir, "project.pbxproj") : NULL;
    managed_root = ns_xcode_path_join(bin, managed_basename.data);
    if (!project_dir || !project_file || !managed_root || !ns_xcode_make_dirs(project_dir) || !ns_xcode_make_dirs(managed_root) ||
        !ns_xcode_write_config(spec, managed_root, safe_name, root, executable)) {
        ok = false;
        goto cleanup;
    }

    if (spec->kind == NS_PROJECT_APP && !spec->host_build) {
        if (!runtime_root || !runtime_root[0] || !linked_source) {
            fprintf(stderr, "project: app generation requires the embedded runtime SDK and linked NS source\n");
            ok = false;
            goto cleanup;
        }
        ok = ns_xcode_refresh_app(spec, managed_root, runtime_root, linked_source, safe_name, version) &&
             ns_xcode_generate_app_pbx(spec, project_file, safe_name);
    } else if (spec->kind == NS_PROJECT_LIBRARY || (spec->kind == NS_PROJECT_APP && spec->host_build)) {
        ok = ns_xcode_generate_library_pbx(project_file, safe_name);
    } else {
        fprintf(stderr, "project: unknown project kind for Xcode generation\n");
        ok = false;
    }

cleanup:
    free(root);
    free(safe_name);
    free(version);
    free(executable);
    free(runtime_root);
    free(linked_source);
    free(bin);
    free(project_dir);
    free(project_file);
    free(managed_root);
    ns_xcode_buffer_free(&project_basename);
    ns_xcode_buffer_free(&managed_basename);
    return ok;
}
