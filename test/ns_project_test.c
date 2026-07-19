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

static ns_bool replace_text_after(const char *path, const char *anchor, const char *needle, const char *replacement) {
    char *source = read_text(path);
    if (!source) return false;
    char *start = strstr(source, anchor);
    char *found = start ? strstr(start, needle) : ns_null;
    if (!found) {
        free(source);
        return false;
    }
    size_t prefix_len = (size_t)(found - source);
    size_t needle_len = strlen(needle);
    size_t replacement_len = strlen(replacement);
    size_t suffix_len = strlen(found + needle_len);
    char *updated = malloc(prefix_len + replacement_len + suffix_len + 1);
    if (!updated) {
        free(source);
        return false;
    }
    memcpy(updated, source, prefix_len);
    memcpy(updated + prefix_len, replacement, replacement_len);
    memcpy(updated + prefix_len + replacement_len, found + needle_len, suffix_len + 1);
    FILE *file = fopen(path, "wb");
    ns_bool ok = file && fwrite(updated, 1, prefix_len + replacement_len + suffix_len, file) ==
                                  prefix_len + replacement_len + suffix_len;
    if (file && fclose(file) != 0) ok = false;
    free(updated);
    free(source);
    return ok;
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
    char app_assets[PATH_MAX], app_asset_file[PATH_MAX];
    path(app_assets, app_root, "assets");
    path(app_asset_file, app_root, "assets/project-asset.txt");
    ns_expect(mkdir(app_assets, 0755) == 0 && append_text(app_asset_file, "bundled\n"),
              "project test creates a project asset fixture.");
    ns_project_spec app = app_spec(app_root, runtime,
                                   "use std\nfn main() { print(`generated`) }\n");
#if defined(__APPLE__)
    char app_icon_source[PATH_MAX];
    path(app_icon_source, runtime, "lib/assets/latin_mono.png");
    app.icon = ns_str_cstr((i8*)app_icon_source);
#endif

    ns_expect(ns_project_generate_xcode(&app), "Xcode app project generation succeeds.");
    ns_expect(ns_project_generate_visual_studio(&app), "Visual Studio app project generation succeeds.");

    char pbx[PATH_MAX], linked[PATH_MAX], xlocal[PATH_MAX], xgenerated[PATH_MAX], bridge_header[PATH_MAX];
    char view_osx[PATH_MAX], view_ios[PATH_MAX], os_ios[PATH_MAX], gpu_metal[PATH_MAX], ui_native[PATH_MAX], ui_asset[PATH_MAX], ios_plist[PATH_MAX];
    char app_icon_json[PATH_MAX], app_icon_png[PATH_MAX], vision_icon_json[PATH_MAX], vision_icon_image[PATH_MAX];
    char sln[PATH_MAX], vcx[PATH_MAX], vlocal[PATH_MAX], vgenerated[PATH_MAX];
    path(pbx, app_root, "bin/demo-app.xcodeproj/project.pbxproj");
    path(linked, app_root, "bin/demo-app.nsproject/Generated/LinkedProject.ns");
    path(xlocal, app_root, "bin/demo-app.nsproject/Config/NS.Local.xcconfig");
    path(xgenerated, app_root, "bin/demo-app.nsproject/Config/NS.Generated.xcconfig");
    path(bridge_header, app_root, "bin/demo-app.nsproject/Sources/NSBridge.h");
    path(view_osx, app_root, "bin/demo-app.nsproject/Native/src/view.osx.m");
    path(view_ios, app_root, "bin/demo-app.nsproject/Native/src/view.ios.m");
    path(os_ios, app_root, "bin/demo-app.nsproject/Native/src/os.ios.m");
    path(gpu_metal, app_root, "bin/demo-app.nsproject/Native/src/gpu.metal.m");
    path(ui_native, app_root, "bin/demo-app.nsproject/Native/src/ui.c");
    path(ui_asset, app_root, "bin/demo-app.nsproject/Resources/latin_mono.json");
    path(ios_plist, app_root, "bin/demo-app.nsproject/Info/iOS-Info.plist");
    path(app_icon_json, app_root, "bin/demo-app.nsproject/Resources/Assets.xcassets/AppIcon.appiconset/Contents.json");
    path(app_icon_png, app_root, "bin/demo-app.nsproject/Resources/Assets.xcassets/AppIcon.appiconset/AppIcon-mac-16.png");
    path(vision_icon_json, app_root, "bin/demo-app.nsproject/Resources/Assets.xcassets/AppIcon.solidimagestack/Contents.json");
    path(vision_icon_image, app_root,
         "bin/demo-app.nsproject/Resources/Assets.xcassets/AppIcon.solidimagestack/Back.solidimagestacklayer/Content.imageset/AppIcon-vision-1024.jpg");
    path(sln, app_root, "bin/demo-app.sln");
    path(vcx, app_root, "bin/demo-app.vcxproj");
    path(vlocal, app_root, "bin/demo-app.nsproject/Config/NS.Local.props");
    path(vgenerated, app_root, "bin/demo-app.nsproject/Config/NS.Generated.props");

    ns_expect(text_has(pbx, "SDKROOT = macosx"), "Xcode project contains a macOS target.");
    ns_expect(text_has(pbx, "SDKROOT = iphoneos"), "Xcode project contains an iOS target.");
    ns_expect(text_has(pbx, "SDKROOT = xros"), "Xcode project contains a visionOS target.");
    ns_expect(text_has(pbx, "path = \"demo-app.nsproject\"") && !text_has(pbx, "../demo-app.nsproject"),
              "Xcode project resolves its managed sibling directory from SRCROOT.");
    ns_expect(text_has(pbx, "name = \"Project Sources\"") && text_has(pbx, app_root),
              "Xcode project navigator exposes the real Nano Script source directory.");
    ns_expect(text_has(pbx, "name = \"Project Assets\"") && text_has(pbx, "Project Assets in Resources") &&
                  text_has(pbx, app_assets),
              "Xcode app targets bundle a project's assets directory as a folder resource.");
    ns_expect(text_has(pbx, "Native/src/view.ios.m") && text_has(pbx, "Native/src/os.ios.m") &&
                  text_has(pbx, "Native/src/ui.c"),
              "Xcode native targets compile the embedded view, UI, and OS forwarders.");
    ns_expect(access(view_ios, R_OK) == 0 && access(os_ios, R_OK) == 0 && access(gpu_metal, R_OK) == 0 && access(ui_native, R_OK) == 0,
              "Xcode project copies Apple feature sources into the managed project.");
    ns_expect(text_has(view_osx, "dispatch_sync(dispatch_get_main_queue(), create_view)") &&
                  text_has(view_osx, "if (view_osx_hosted)") &&
                  text_has(view_osx, "dispatch_semaphore_wait(view_osx_done"),
              "embedded macOS view creation and lifecycle stay on the AppKit main thread.");
    ns_expect(text_has(gpu_metal, "if ([NSThread isMainThread]) attach_view()") &&
                  text_has(gpu_metal, "dispatch_sync(dispatch_get_main_queue(), attach_view)"),
              "embedded Metal setup keeps NSWindow and MTKView access on the AppKit main thread.");
    ns_expect(text_has(gpu_metal, "_mtl_buffer_resource_options(desc->usage)") &&
                  text_has(gpu_metal, "usage & ~USAGE_MEMORYLESS"),
              "embedded Metal strips texture-only memoryless storage from buffers.");
    ns_expect(text_has(gpu_metal, "desc->color_count > 0 ? shader.fragment_func : nil"),
              "embedded Metal omits fragment functions from depth-only pipelines.");
    ns_expect(text_has(gpu_metal, "gpu_pipeline_mtl _pipeline = {0}") &&
                  text_has(gpu_metal, "_pipeline.reflection = [reflection retain]"),
              "embedded Metal pipelines own initialized reflection state through renderer shutdown.");
    ns_expect(text_has(view_ios, "CGSize drawable = metal_view.drawableSize") &&
                  text_has(view_ios, "framebuffer_width = (i32)(drawable.width + 0.5)"),
              "embedded iOS view metrics use the exact Metal drawable extent.");
    ns_expect(text_has(ui_native, "gpu_set_viewport(0, 0, framebuffer_width, framebuffer_height)") &&
                  text_has(ui_native, "if (x1 > framebuffer_width) x1 = framebuffer_width") &&
                  text_has(ui_native, "gpu_set_scissor(x0, y0, x1 - x0, y1 - y0)"),
              "embedded UI scissors are clamped to the render-pass extent.");
    ns_expect(text_has(ui_native, "\"../Resources\""),
              "embedded UI resolves its font atlas from a generated macOS application bundle.");
    char embedded_ffi[PATH_MAX];
    path(embedded_ffi, app_root, "bin/demo-app.nsproject/Runtime/src/ns_embedded_ffi.c");
    ns_expect(text_has(embedded_ffi, "extern void ui_flush(void *, ui_color_rgba *);") &&
                  text_has(embedded_ffi, "gpu_begin_render_pass(*(gpu_render_pass *)") &&
                  text_has(embedded_ffi, "extern u32 gpu_create_pipeline_layout_indexed_ex") &&
                  text_has(embedded_ffi, "extern u32 gpu_create_buffer_texture_binding") &&
                  text_has(embedded_ffi, "extern i32 os_platform(void);") &&
                  text_has(embedded_ffi, "i32 result = os_platform();") &&
                  text_has(embedded_ffi, "embedded native function is not forwarded: %.*s.%.*s") &&
                  !text_has(embedded_ffi, "ui_scene_"),
              "embedded forwarding preserves native ABIs and names missing functions precisely.");
    ns_expect(access(ui_asset, R_OK) == 0 && text_has(pbx, "latin_mono.json in Resources"),
              "Xcode app targets bundle the UI runtime assets.");
#if defined(__APPLE__)
    ns_expect(text_has(pbx, "App Icon Assets in Resources") && text_has(pbx, "ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon"),
              "Xcode app targets compile the generated AppIcon asset catalog.");
    ns_expect(text_has(app_icon_json, "AppIcon-ios-1024.png") && text_has(app_icon_json, "AppIcon-mac-512@2x.png") &&
                  access(app_icon_png, R_OK) == 0,
              "Xcode project generation creates the iOS and macOS AppIcon set from the manifest icon.");
    ns_expect(text_has(vision_icon_json, "Back.solidimagestacklayer") && access(vision_icon_image, R_OK) == 0,
              "Xcode project generation creates a visionOS AppIcon image stack from the manifest icon.");
#endif
    ns_expect(text_has(ios_plist, "UIInterfaceOrientationPortraitUpsideDown") &&
                  text_has(ios_plist, "UISupportedInterfaceOrientations~ipad"),
              "Xcode iOS app declares every supported phone and tablet orientation.");
    ns_expect(text_has(xgenerated, "NS_BUNDLE_IDENTIFIER = ns.demo-app"), "Xcode project uses the sanitized bundle identifier.");
    ns_expect(text_has(xgenerated, "NS_EXECUTABLE = /tmp/ns\\ tools/bin/ns") &&
                  !text_has(xgenerated, "NS_EXECUTABLE = \""),
              "Xcode configuration escapes executable paths without embedding shell-breaking quotes.");
    ns_expect(text_has(xgenerated, "-Wno-shorten-64-to-32") &&
                  !text_has(pbx, "\"-framework\", AppIntents") &&
                  text_has(pbx, "NSProjectGeneratorVersion = 7"),
              "Xcode configuration keeps intentional embedded ABI narrowing quiet without linking unused AppIntents services.");
    ns_expect(text_has(bridge_header, "#ifndef NS_BRIDGE_H") && !text_has(bridge_header, "#pragma once"),
              "Xcode bridging header uses an include guard without main-file pragma warnings.");
    char bridge_source[PATH_MAX];
    path(bridge_source, app_root, "bin/demo-app.nsproject/Sources/NSBridge.c");
    ns_expect(text_has(bridge_source, "chdir(resource_root)"),
              "Xcode app execution resolves relative project assets from the application bundle.");
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

    ns_expect(replace_text_after(pbx, "4E5350520000004800000015 /* Debug */", "DEVELOPMENT_TEAM = \"\";",
                                 "DEVELOPMENT_TEAM = IOSDEBUG1;") &&
                  replace_text_after(pbx, "4E5350520000004800000016 /* Release */", "DEVELOPMENT_TEAM = \"\";",
                                     "DEVELOPMENT_TEAM = IOSRELSE2;") &&
                  replace_text_after(pbx, "NSProjectGeneratorVersion = 7;", "NSProjectGeneratorVersion = 7;",
                                     "NSProjectGeneratorVersion = 6;"),
              "project test simulates iOS signing choices before a structural refresh.");
    ns_expect(ns_project_generate_xcode(&app), "Xcode structural project refresh succeeds.");
    ns_expect(text_has(pbx, "DEVELOPMENT_TEAM = \"IOSDEBUG1\";") &&
                  text_has(pbx, "DEVELOPMENT_TEAM = \"IOSRELSE2\";"),
              "Xcode structural refresh preserves iOS development teams by configuration.");

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
    hosted.host_build = false;
    ns_expect(ns_project_generate_xcode(&hosted), "Xcode generated legacy app upgrade succeeds.");
    ns_expect(text_has(hosted_pbx, "SDKROOT = iphoneos") && text_has(hosted_pbx, "isa = PBXNativeTarget;") &&
                  !text_has(hosted_pbx, "isa = PBXLegacyTarget;") && text_has(hosted_pbx, "Project Sources"),
              "Xcode upgrades an ns-generated legacy app to native multi-platform targets.");

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
