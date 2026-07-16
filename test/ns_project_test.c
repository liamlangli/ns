#include "ns_project.h"
#include "ns_test.h"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return ns_null;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return ns_null;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return ns_null;
    }
    size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read] = '\0';
    return text;
}

static ns_bool text_has(const char *path, const char *needle) {
    char *text = read_text(path);
    ns_bool found = text && strstr(text, needle) != ns_null;
    free(text);
    return found;
}

static ns_bool append_text(const char *path, const char *text) {
    FILE *file = fopen(path, "ab");
    if (!file) return false;
    ns_bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    return fclose(file) == 0 && ok;
}

static void path(char out[PATH_MAX], const char *root, const char *suffix) {
    snprintf(out, PATH_MAX, "%s/%s", root, suffix);
}

static ns_project_spec app_spec(const char *root, const char *runtime, const char *linked) {
    return (ns_project_spec){
        .kind = NS_PROJECT_APP,
        .root = ns_str_cstr((i8*)root),
        .manifest = ns_str_cstr((i8*)"ns.mod"),
        .source_dir = ns_str_cstr((i8*)root),
        .name = ns_str_cstr((i8*)"Demo & App"),
        .safe_name = ns_str_cstr((i8*)"demo-app"),
        .version = ns_str_cstr((i8*)"1.2.3"),
        .linked_source = ns_str_cstr((i8*)linked),
        .ns_executable = ns_str_cstr((i8*)"/tmp/ns tools/bin/ns"),
        .runtime_root = ns_str_cstr((i8*)runtime),
    };
}

int main(void) {
    ns_str safe = ns_project_safe_name(ns_str_cstr(" 42 Weird_Name!? "));
    ns_expect(ns_str_equals(safe, ns_str_cstr("project-42-weird-name")),
              "project names normalize deterministically for IDE and bundle identifiers.");

    char runtime[PATH_MAX];
    ns_expect(getcwd(runtime, sizeof(runtime)) != ns_null, "project test resolves runtime fixture root.");

    char app_root[] = "/tmp/ns-project-app-XXXXXX";
    ns_expect(mkdtemp(app_root) != ns_null, "project test creates app fixture directory.");
    ns_project_spec app = app_spec(app_root, runtime,
                                   "use std\nfn main() { print(`generated`) }\n");

    ns_expect(ns_project_generate_xcode(&app), "Xcode app project generation succeeds.");
    ns_expect(ns_project_generate_visual_studio(&app), "Visual Studio app project generation succeeds.");

    char pbx[PATH_MAX], linked[PATH_MAX], xlocal[PATH_MAX], xgenerated[PATH_MAX];
    char view_ios[PATH_MAX], os_ios[PATH_MAX], ui_native[PATH_MAX], ui_asset[PATH_MAX];
    char sln[PATH_MAX], vcx[PATH_MAX], vlocal[PATH_MAX], vgenerated[PATH_MAX];
    path(pbx, app_root, "bin/demo-app.xcodeproj/project.pbxproj");
    path(linked, app_root, "bin/demo-app.nsproject/Generated/LinkedProject.ns");
    path(xlocal, app_root, "bin/demo-app.nsproject/Config/NS.Local.xcconfig");
    path(xgenerated, app_root, "bin/demo-app.nsproject/Config/NS.Generated.xcconfig");
    path(view_ios, app_root, "bin/demo-app.nsproject/Native/src/view.ios.m");
    path(os_ios, app_root, "bin/demo-app.nsproject/Native/src/os.ios.m");
    path(ui_native, app_root, "bin/demo-app.nsproject/Native/src/ui.c");
    path(ui_asset, app_root, "bin/demo-app.nsproject/Resources/latin_mono.json");
    path(sln, app_root, "bin/demo-app.sln");
    path(vcx, app_root, "bin/demo-app.vcxproj");
    path(vlocal, app_root, "bin/demo-app.nsproject/Config/NS.Local.props");
    path(vgenerated, app_root, "bin/demo-app.nsproject/Config/NS.Generated.props");

    ns_expect(text_has(pbx, "SDKROOT = macosx"), "Xcode project contains a macOS target.");
    ns_expect(text_has(pbx, "SDKROOT = iphoneos"), "Xcode project contains an iOS target.");
    ns_expect(text_has(pbx, "SDKROOT = xros"), "Xcode project contains a visionOS target.");
    ns_expect(text_has(pbx, "path = \"demo-app.nsproject\"") && !text_has(pbx, "../demo-app.nsproject"),
              "Xcode project resolves its managed sibling directory from SRCROOT.");
    ns_expect(text_has(pbx, "Native/src/view.ios.m") && text_has(pbx, "Native/src/os.ios.m") &&
                  text_has(pbx, "Native/src/ui.c"),
              "Xcode native targets compile the embedded view, UI, and OS forwarders.");
    ns_expect(access(view_ios, R_OK) == 0 && access(os_ios, R_OK) == 0 && access(ui_native, R_OK) == 0,
              "Xcode project copies Apple feature sources into the managed project.");
    char embedded_ffi[PATH_MAX];
    path(embedded_ffi, app_root, "bin/demo-app.nsproject/Runtime/src/ns_embedded_ffi.c");
    ns_expect(text_has(embedded_ffi, "extern void ui_flush(void *, ui_color_rgba *);") &&
                  text_has(embedded_ffi, "gpu_begin_render_pass(*(gpu_render_pass *)"),
              "embedded forwarding preserves pointer and value native struct ABIs.");
    ns_expect(access(ui_asset, R_OK) == 0 && text_has(pbx, "latin_mono.json in Resources"),
              "Xcode app targets bundle the UI runtime assets.");
    ns_expect(text_has(xgenerated, "NS_BUNDLE_IDENTIFIER = ns.demo-app"), "Xcode project uses the sanitized bundle identifier.");
    ns_expect(text_has(xgenerated, "NS_EXECUTABLE = /tmp/ns\\ tools/bin/ns") &&
                  !text_has(xgenerated, "NS_EXECUTABLE = \""),
              "Xcode configuration escapes executable paths without embedding shell-breaking quotes.");
    ns_expect(text_has(pbx, "NS.Generated.xcconfig"), "Xcode project imports generated configuration.");
    ns_expect(text_has(linked, "print(`generated`)"), "Xcode project writes linked NS source.");
    ns_expect(text_has(sln, "Visual Studio Version 17"), "Visual Studio 2022 solution is generated.");
    ns_expect(text_has(vcx, "<ConfigurationType>Makefile</ConfigurationType>"), "Visual Studio project is NMake-based.");
    ns_expect(text_has(vcx, "$(NSExecutable)&quot; build"), "Visual Studio app delegates builds to ns build.");
    ns_expect(text_has(vcx, "Exclude=\"$(NSProjectRoot)\\bin\\**\\*\""), "Visual Studio source wildcard excludes bin.");

    ns_expect(append_text(pbx, "\n// PBX USER SENTINEL\n") && append_text(xlocal, "\n// XCODE LOCAL SENTINEL\n") &&
                  append_text(sln, "\r\n# SLN USER SENTINEL\r\n") && append_text(vcx, "\r\n<!-- VCX USER SENTINEL -->\r\n") &&
                  append_text(vlocal, "\r\n<!-- VS LOCAL SENTINEL -->\r\n"),
              "project test adds simulated user edits.");

    app.linked_source = ns_str_cstr((i8*)"use std\nfn main() { print(`refreshed`) }\n");
    app.ns_executable = ns_str_cstr((i8*)"C:\\Program Files\\ns\\ns.exe");
    ns_expect(ns_project_generate_xcode(&app) && ns_project_generate_visual_studio(&app),
              "project regeneration succeeds.");
    ns_expect(text_has(pbx, "PBX USER SENTINEL") && text_has(xlocal, "XCODE LOCAL SENTINEL"),
              "Xcode regeneration preserves project and local configuration edits.");
    ns_expect(text_has(sln, "SLN USER SENTINEL") && text_has(vcx, "VCX USER SENTINEL") && text_has(vlocal, "VS LOCAL SENTINEL"),
              "Visual Studio regeneration preserves solution, project, and local property edits.");
    ns_expect(text_has(linked, "print(`refreshed`)") && !text_has(linked, "print(`generated`)"),
              "Xcode regeneration refreshes linked source.");
    ns_expect(text_has(vgenerated, "C:\\Program Files\\ns\\ns.exe"),
              "Visual Studio regeneration refreshes generated properties.");

    char hosted_root[] = "/tmp/ns-project-hosted-app-XXXXXX";
    ns_expect(mkdtemp(hosted_root) != ns_null, "project test creates hosted app fixture directory.");
    ns_project_spec hosted = app_spec(hosted_root, runtime, "use view\nfn main() {}\n");
    hosted.host_build = true;
    ns_expect(ns_project_generate_xcode(&hosted), "Xcode hosted app project generation succeeds.");
    char hosted_pbx[PATH_MAX];
    path(hosted_pbx, hosted_root, "bin/demo-app.xcodeproj/project.pbxproj");
    ns_expect(text_has(hosted_pbx, "NS Build") && text_has(hosted_pbx, "NS Test"),
              "Xcode hosted app delegates build and test to the host ns toolchain.");
    ns_expect(!text_has(hosted_pbx, "SDKROOT = iphoneos"),
              "Xcode hosted app does not claim unsupported portable Apple targets.");

    char library_root[] = "/tmp/ns-project-library-XXXXXX";
    ns_expect(mkdtemp(library_root) != ns_null, "project test creates library fixture directory.");
    ns_project_spec library = app_spec(library_root, runtime, "");
    library.kind = NS_PROJECT_LIBRARY;
    ns_expect(ns_project_generate_xcode(&library), "Xcode library utility project generation succeeds.");
    ns_expect(ns_project_generate_visual_studio(&library), "Visual Studio library utility project generation succeeds.");
    char library_pbx[PATH_MAX], library_vcx[PATH_MAX];
    path(library_pbx, library_root, "bin/demo-app.xcodeproj/project.pbxproj");
    path(library_vcx, library_root, "bin/demo-app.vcxproj");
    ns_expect(text_has(library_pbx, "NS Build") && text_has(library_pbx, "NS Test"),
              "Xcode library project exposes host build and test utility targets.");
    ns_expect(text_has(library_vcx, "$(NSExecutable)&quot; test"),
              "Visual Studio library Build action runs ns test.");

    return 0;
}
