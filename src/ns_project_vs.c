#include "ns_project.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

typedef struct ns_vs_buffer {
    char *data;
    size_t len;
    size_t capacity;
    ns_bool ok;
} ns_vs_buffer;

static void ns_vs_buffer_init(ns_vs_buffer *buffer) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->ok = true;
}

static void ns_vs_buffer_free(ns_vs_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void ns_vs_buffer_reserve(ns_vs_buffer *buffer, size_t additional) {
    if (!buffer->ok) return;
    if (additional > SIZE_MAX - buffer->len - 1) {
        buffer->ok = false;
        return;
    }

    size_t needed = buffer->len + additional + 1;
    if (needed <= buffer->capacity) return;

    size_t capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    char *data = (char *)realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->ok = false;
        return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
}

static void ns_vs_buffer_append_len(ns_vs_buffer *buffer, const char *text, size_t len) {
    ns_vs_buffer_reserve(buffer, len);
    if (!buffer->ok) return;
    memcpy(buffer->data + buffer->len, text, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
}

static void ns_vs_buffer_append(ns_vs_buffer *buffer, const char *text) {
    ns_vs_buffer_append_len(buffer, text, strlen(text));
}

static void ns_vs_buffer_append_ns_str(ns_vs_buffer *buffer, ns_str text) {
    ns_vs_buffer_append_len(buffer, text.data, (size_t)text.len);
}

static void ns_vs_buffer_append_xml_len(ns_vs_buffer *buffer, const char *text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        switch (text[i]) {
        case '&': ns_vs_buffer_append(buffer, "&amp;"); break;
        case '<': ns_vs_buffer_append(buffer, "&lt;"); break;
        case '>': ns_vs_buffer_append(buffer, "&gt;"); break;
        case '\"': ns_vs_buffer_append(buffer, "&quot;"); break;
        case '\'': ns_vs_buffer_append(buffer, "&apos;"); break;
        default: ns_vs_buffer_append_len(buffer, text + i, 1); break;
        }
    }
}

static void ns_vs_buffer_append_xml_ns_str(ns_vs_buffer *buffer, ns_str text) {
    ns_vs_buffer_append_xml_len(buffer, text.data, (size_t)text.len);
}

// MSBuild expands properties/items and treats several otherwise valid path
// characters as syntax. Encode those first, then apply XML escaping to the
// remaining bytes. MSBuild restores the encoded characters when a property is
// consumed by NMake, the debugger, or an item wildcard.
static void ns_vs_buffer_append_msbuild_xml_len(ns_vs_buffer *buffer, const char *text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const char *escape = NULL;
        switch (text[i]) {
        case '%': escape = "%25"; break;
        case '$': escape = "%24"; break;
        case '@': escape = "%40"; break;
        case '\'': escape = "%27"; break;
        case '(': escape = "%28"; break;
        case ')': escape = "%29"; break;
        case ';': escape = "%3B"; break;
        case '?': escape = "%3F"; break;
        case '*': escape = "%2A"; break;
        default: break;
        }
        if (escape != NULL) ns_vs_buffer_append(buffer, escape);
        else ns_vs_buffer_append_xml_len(buffer, text + i, 1);
    }
}

static void ns_vs_buffer_append_msbuild_xml(ns_vs_buffer *buffer, const char *text) {
    ns_vs_buffer_append_msbuild_xml_len(buffer, text, strlen(text));
}

static void ns_vs_buffer_append_msbuild_xml_ns_str(ns_vs_buffer *buffer, ns_str text) {
    ns_vs_buffer_append_msbuild_xml_len(buffer, text.data, (size_t)text.len);
}

static char *ns_vs_copy_ns_str(ns_str value) {
    char *copy = (char *)malloc((size_t)value.len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, value.data, (size_t)value.len);
    copy[value.len] = '\0';
    return copy;
}

static char *ns_vs_concat(const char *lhs, const char *rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    if (lhs_len > SIZE_MAX - rhs_len - 1) return NULL;
    char *result = (char *)malloc(lhs_len + rhs_len + 1);
    if (result == NULL) return NULL;
    memcpy(result, lhs, lhs_len);
    memcpy(result + lhs_len, rhs, rhs_len + 1);
    return result;
}

static ns_bool ns_vs_is_separator(char c) {
#if defined(_WIN32)
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

static char *ns_vs_path_join(const char *lhs, const char *rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    ns_bool needs_separator = lhs_len > 0 && !ns_vs_is_separator(lhs[lhs_len - 1]);
    if (lhs_len > SIZE_MAX - rhs_len - (needs_separator ? 2 : 1)) return NULL;

    char *result = (char *)malloc(lhs_len + rhs_len + (needs_separator ? 2 : 1));
    if (result == NULL) return NULL;
    memcpy(result, lhs, lhs_len);
    if (needs_separator) {
#if defined(_WIN32)
        result[lhs_len++] = '\\';
#else
        result[lhs_len++] = '/';
#endif
    }
    memcpy(result + lhs_len, rhs, rhs_len + 1);
    return result;
}

enum ns_vs_path_kind {
    NS_VS_PATH_MISSING,
    NS_VS_PATH_FILE,
    NS_VS_PATH_DIRECTORY,
    NS_VS_PATH_OTHER,
};

static enum ns_vs_path_kind ns_vs_path_kind(const char *path) {
#if defined(_WIN32)
    struct _stat info;
    if (_stat(path, &info) != 0) return NS_VS_PATH_MISSING;
    if ((info.st_mode & _S_IFMT) == _S_IFREG) return NS_VS_PATH_FILE;
    if ((info.st_mode & _S_IFMT) == _S_IFDIR) return NS_VS_PATH_DIRECTORY;
#else
    struct stat info;
    if (stat(path, &info) != 0) return NS_VS_PATH_MISSING;
    if (S_ISREG(info.st_mode)) return NS_VS_PATH_FILE;
    if (S_ISDIR(info.st_mode)) return NS_VS_PATH_DIRECTORY;
#endif
    return NS_VS_PATH_OTHER;
}

static ns_bool ns_vs_mkdir_one(const char *path) {
    if (path[0] == '\0') return true;
#if defined(_WIN32)
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST && ns_vs_path_kind(path) == NS_VS_PATH_DIRECTORY;
}

static size_t ns_vs_mkdir_start(const char *path) {
    size_t len = strlen(path);
#if defined(_WIN32)
    if (len >= 3 && isalpha((unsigned char)path[0]) && path[1] == ':' && ns_vs_is_separator(path[2])) return 3;
    if (len >= 2 && ns_vs_is_separator(path[0]) && ns_vs_is_separator(path[1])) {
        size_t i = 2;
        while (i < len && !ns_vs_is_separator(path[i])) ++i;
        while (i < len && ns_vs_is_separator(path[i])) ++i;
        while (i < len && !ns_vs_is_separator(path[i])) ++i;
        while (i < len && ns_vs_is_separator(path[i])) ++i;
        return i;
    }
#endif
    return len > 0 && ns_vs_is_separator(path[0]) ? 1 : 0;
}

static ns_bool ns_vs_mkdir_p(const char *path) {
    char *copy = ns_vs_concat(path, "");
    if (copy == NULL) return false;

    size_t len = strlen(copy);
    while (len > 1 && ns_vs_is_separator(copy[len - 1])) copy[--len] = '\0';
    size_t start = ns_vs_mkdir_start(copy);

    for (size_t i = start; i < len; ++i) {
        if (!ns_vs_is_separator(copy[i])) continue;
        char separator = copy[i];
        copy[i] = '\0';
        if (!ns_vs_mkdir_one(copy)) {
            free(copy);
            return false;
        }
        copy[i] = separator;
        while (i + 1 < len && ns_vs_is_separator(copy[i + 1])) ++i;
    }

    ns_bool result = ns_vs_mkdir_one(copy);
    free(copy);
    return result;
}

static ns_bool ns_vs_write_buffer(const char *path, const ns_vs_buffer *buffer, ns_bool overwrite) {
    if (!buffer->ok) {
        fprintf(stderr, "ns project: out of memory while generating %s.\n", path);
        return false;
    }

    enum ns_vs_path_kind kind = ns_vs_path_kind(path);
    if (!overwrite && kind == NS_VS_PATH_FILE) return true;
    if (kind == NS_VS_PATH_DIRECTORY || kind == NS_VS_PATH_OTHER) {
        fprintf(stderr, "ns project: cannot write %s because a non-file already exists there.\n", path);
        return false;
    }

    char *write_path = overwrite ? ns_vs_concat(path, ".tmp") : ns_vs_concat(path, "");
    if (write_path == NULL) {
        fprintf(stderr, "ns project: out of memory while preparing %s.\n", path);
        return false;
    }

    FILE *file = fopen(write_path, "wb");
    if (file == NULL) {
        fprintf(stderr, "ns project: cannot open %s for writing: %s.\n", write_path, strerror(errno));
        free(write_path);
        return false;
    }

    ns_bool ok = fwrite(buffer->data, 1, buffer->len, file) == buffer->len;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "ns project: cannot write %s: %s.\n", write_path, strerror(errno));
        remove(write_path);
        free(write_path);
        return false;
    }

    if (overwrite) {
#if defined(_WIN32)
        if (kind == NS_VS_PATH_FILE && remove(path) != 0) {
            fprintf(stderr, "ns project: cannot replace %s: %s.\n", path, strerror(errno));
            goto replace_failure;
        }
#endif
        if (rename(write_path, path) != 0) {
            fprintf(stderr, "ns project: cannot replace %s: %s.\n", path, strerror(errno));
            goto replace_failure;
        }
    }

    free(write_path);
    return true;

replace_failure:
    remove(write_path);
    free(write_path);
    return false;
}

static uint64_t ns_vs_hash_byte(uint64_t hash, unsigned char byte) {
    return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t ns_vs_hash_str(uint64_t hash, ns_str value) {
    for (i32 i = 0; i < value.len; ++i) hash = ns_vs_hash_byte(hash, (unsigned char)value.data[i]);
    return hash;
}

static void ns_vs_make_guid(const ns_project_spec *spec, unsigned char discriminator, char guid[39]) {
    uint64_t first = UINT64_C(14695981039346656037);
    uint64_t second = UINT64_C(7809847782465536322);
    first = ns_vs_hash_str(first, spec->root);
    first = ns_vs_hash_byte(first, 0);
    first = ns_vs_hash_str(first, spec->safe_name);
    first = ns_vs_hash_byte(first, discriminator);
    second = ns_vs_hash_byte(second, discriminator);
    second = ns_vs_hash_str(second, spec->safe_name);
    second = ns_vs_hash_byte(second, 0);
    second = ns_vs_hash_str(second, spec->root);

    unsigned char bytes[16];
    for (i32 i = 0; i < 8; ++i) {
        bytes[i] = (unsigned char)(first >> (i * 8));
        bytes[i + 8] = (unsigned char)(second >> (i * 8));
    }
    bytes[6] = (unsigned char)((bytes[6] & 0x0fU) | 0x50U);
    bytes[8] = (unsigned char)((bytes[8] & 0x3fU) | 0x80U);

    snprintf(guid, 39, "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned)bytes[0],
             (unsigned)bytes[1], (unsigned)bytes[2], (unsigned)bytes[3], (unsigned)bytes[4], (unsigned)bytes[5],
             (unsigned)bytes[6], (unsigned)bytes[7], (unsigned)bytes[8], (unsigned)bytes[9], (unsigned)bytes[10],
             (unsigned)bytes[11], (unsigned)bytes[12], (unsigned)bytes[13], (unsigned)bytes[14], (unsigned)bytes[15]);
}

static ns_bool ns_vs_valid_input_string(ns_str value, ns_bool allow_empty) {
    if (value.len < 0 || (value.len > 0 && value.data == NULL)) return false;
    if (!allow_empty && value.len == 0) return false;
    for (i32 i = 0; i < value.len; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if (c == 0 || c < 0x20U) return false;
    }
    return true;
}

static ns_bool ns_vs_valid_safe_name(ns_str name) {
    if (!ns_vs_valid_input_string(name, false)) return false;
    if ((name.len == 1 && name.data[0] == '.') || (name.len == 2 && name.data[0] == '.' && name.data[1] == '.')) return false;
    for (i32 i = 0; i < name.len; ++i) {
        unsigned char c = (unsigned char)name.data[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

static void ns_vs_generate_props(ns_vs_buffer *buffer, const ns_project_spec *spec, const char *output_path) {
    ns_vs_buffer_append(buffer, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
    ns_vs_buffer_append(buffer, "<!-- Managed by ns project. This file is refreshed on every generation. -->\r\n");
    ns_vs_buffer_append(buffer, "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n");
    ns_vs_buffer_append(buffer, "  <PropertyGroup Label=\"NSGenerated\">\r\n");
    ns_vs_buffer_append(buffer, "    <NSExecutable>");
    ns_vs_buffer_append_msbuild_xml_ns_str(buffer, spec->ns_executable);
    ns_vs_buffer_append(buffer, "</NSExecutable>\r\n");
    ns_vs_buffer_append(buffer, "    <NSProjectRoot>");
    ns_vs_buffer_append_msbuild_xml_ns_str(buffer, spec->root);
    ns_vs_buffer_append(buffer, "</NSProjectRoot>\r\n");
    ns_vs_buffer_append(buffer, "    <NSProjectName>");
    ns_vs_buffer_append_xml_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, "</NSProjectName>\r\n");
    ns_vs_buffer_append(buffer, "    <NSProjectKind>");
    ns_vs_buffer_append(buffer, spec->kind == NS_PROJECT_APP ? "Application" : "Library");
    ns_vs_buffer_append(buffer, "</NSProjectKind>\r\n");
    if (spec->kind == NS_PROJECT_APP) {
        ns_vs_buffer_append(buffer, "    <NSOutput>");
        ns_vs_buffer_append_msbuild_xml(buffer, output_path);
        ns_vs_buffer_append(buffer, "</NSOutput>\r\n");
    }
    ns_vs_buffer_append(buffer, "  </PropertyGroup>\r\n");
    ns_vs_buffer_append(buffer, "</Project>\r\n");
}

static void ns_vs_generate_local_props(ns_vs_buffer *buffer) {
    ns_vs_buffer_append(buffer, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
    ns_vs_buffer_append(buffer, "<!-- User-owned settings. ns project never overwrites this file. -->\r\n");
    ns_vs_buffer_append(buffer, "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n");
    ns_vs_buffer_append(buffer, "  <!-- Add persistent overrides such as NSExecutable inside this group. -->\r\n");
    ns_vs_buffer_append(buffer, "  <PropertyGroup Label=\"NSLocal\" />\r\n");
    ns_vs_buffer_append(buffer, "</Project>\r\n");
}

static void ns_vs_generate_solution(ns_vs_buffer *buffer, const ns_project_spec *spec, const char project_guid[39],
                                    const char solution_guid[39]) {
    ns_vs_buffer_append(buffer, "Microsoft Visual Studio Solution File, Format Version 12.00\r\n");
    ns_vs_buffer_append(buffer, "# Visual Studio Version 17\r\n");
    ns_vs_buffer_append(buffer, "VisualStudioVersion = 17.0.31903.59\r\n");
    ns_vs_buffer_append(buffer, "MinimumVisualStudioVersion = 10.0.40219.1\r\n");
    ns_vs_buffer_append(buffer, "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"");
    ns_vs_buffer_append_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, "\", \"");
    ns_vs_buffer_append_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, ".vcxproj\", \"");
    ns_vs_buffer_append(buffer, project_guid);
    ns_vs_buffer_append(buffer, "\"\r\nEndProject\r\n");
    ns_vs_buffer_append(buffer, "Global\r\n");
    ns_vs_buffer_append(buffer, "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n");
    ns_vs_buffer_append(buffer, "\t\tDebug|x64 = Debug|x64\r\n");
    ns_vs_buffer_append(buffer, "\t\tRelease|x64 = Release|x64\r\n");
    ns_vs_buffer_append(buffer, "\tEndGlobalSection\r\n");
    ns_vs_buffer_append(buffer, "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n");
    ns_vs_buffer_append(buffer, "\t\t");
    ns_vs_buffer_append(buffer, project_guid);
    ns_vs_buffer_append(buffer, ".Debug|x64.ActiveCfg = Debug|x64\r\n\t\t");
    ns_vs_buffer_append(buffer, project_guid);
    ns_vs_buffer_append(buffer, ".Debug|x64.Build.0 = Debug|x64\r\n\t\t");
    ns_vs_buffer_append(buffer, project_guid);
    ns_vs_buffer_append(buffer, ".Release|x64.ActiveCfg = Release|x64\r\n\t\t");
    ns_vs_buffer_append(buffer, project_guid);
    ns_vs_buffer_append(buffer, ".Release|x64.Build.0 = Release|x64\r\n");
    ns_vs_buffer_append(buffer, "\tEndGlobalSection\r\n");
    ns_vs_buffer_append(buffer, "\tGlobalSection(SolutionProperties) = preSolution\r\n");
    ns_vs_buffer_append(buffer, "\t\tHideSolutionNode = FALSE\r\n");
    ns_vs_buffer_append(buffer, "\tEndGlobalSection\r\n");
    ns_vs_buffer_append(buffer, "\tGlobalSection(ExtensibilityGlobals) = postSolution\r\n");
    ns_vs_buffer_append(buffer, "\t\tSolutionGuid = ");
    ns_vs_buffer_append(buffer, solution_guid);
    ns_vs_buffer_append(buffer, "\r\n\tEndGlobalSection\r\n");
    ns_vs_buffer_append(buffer, "EndGlobal\r\n");
}

static void ns_vs_append_nmake_settings(ns_vs_buffer *buffer, ns_project_kind kind, const char *configuration) {
    ns_vs_buffer_append(buffer, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='");
    ns_vs_buffer_append(buffer, configuration);
    ns_vs_buffer_append(buffer, "|x64'\">\r\n");
    if (kind == NS_PROJECT_APP) {
        ns_vs_buffer_append(
            buffer, "    <NMakeBuildCommandLine>&quot;$(NSExecutable)&quot; build &quot;$(NSProjectRoot)&quot; --exe -o "
                    "&quot;$(NSOutput)&quot;</NMakeBuildCommandLine>\r\n");
        ns_vs_buffer_append(buffer,
                            "    <NMakeReBuildCommandLine>if exist &quot;$(NSOutput)&quot; del /F /Q "
                            "&quot;$(NSOutput)&quot; &amp; "
                            "&quot;$(NSExecutable)&quot; build &quot;$(NSProjectRoot)&quot; --exe -o &quot;$(NSOutput)&quot;"
                            "</NMakeReBuildCommandLine>\r\n");
        ns_vs_buffer_append(buffer,
                            "    <NMakeCleanCommandLine>if exist &quot;$(NSOutput)&quot; del /F /Q "
                            "&quot;$(NSOutput)&quot;</NMakeCleanCommandLine>\r\n");
        ns_vs_buffer_append(buffer, "    <NMakeOutput>$(NSOutput)</NMakeOutput>\r\n");
        ns_vs_buffer_append(buffer, "    <LocalDebuggerCommand>$(NSOutput)</LocalDebuggerCommand>\r\n");
        ns_vs_buffer_append(buffer, "    <LocalDebuggerWorkingDirectory>$(NSProjectRoot)</LocalDebuggerWorkingDirectory>\r\n");
        ns_vs_buffer_append(buffer, "    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>\r\n");
    } else {
        ns_vs_buffer_append(buffer,
                            "    <NMakeBuildCommandLine>&quot;$(NSExecutable)&quot; test "
                            "&quot;$(NSProjectRoot)&quot;</NMakeBuildCommandLine>\r\n");
        ns_vs_buffer_append(buffer,
                            "    <NMakeReBuildCommandLine>&quot;$(NSExecutable)&quot; test "
                            "&quot;$(NSProjectRoot)&quot;</NMakeReBuildCommandLine>\r\n");
        ns_vs_buffer_append(buffer, "    <NMakeCleanCommandLine>cmd /c exit 0</NMakeCleanCommandLine>\r\n");
    }
    ns_vs_buffer_append(buffer, "  </PropertyGroup>\r\n");
}

static void ns_vs_generate_vcxproj(ns_vs_buffer *buffer, const ns_project_spec *spec, const char project_guid[39]) {
    ns_vs_buffer_append(buffer, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
    ns_vs_buffer_append(buffer,
                        "<Project DefaultTargets=\"Build\" ToolsVersion=\"17.0\" "
                        "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\r\n");
    ns_vs_buffer_append(buffer, "  <ItemGroup Label=\"ProjectConfigurations\">\r\n");
    ns_vs_buffer_append(buffer,
                        "    <ProjectConfiguration Include=\"Debug|x64\">\r\n"
                        "      <Configuration>Debug</Configuration>\r\n"
                        "      <Platform>x64</Platform>\r\n"
                        "    </ProjectConfiguration>\r\n"
                        "    <ProjectConfiguration Include=\"Release|x64\">\r\n"
                        "      <Configuration>Release</Configuration>\r\n"
                        "      <Platform>x64</Platform>\r\n"
                        "    </ProjectConfiguration>\r\n"
                        "  </ItemGroup>\r\n");
    ns_vs_buffer_append(buffer, "  <PropertyGroup Label=\"Globals\">\r\n");
    ns_vs_buffer_append(buffer, "    <VCProjectVersion>17.0</VCProjectVersion>\r\n");
    ns_vs_buffer_append(buffer, "    <Keyword>MakeFileProj</Keyword>\r\n");
    ns_vs_buffer_append(buffer, "    <ProjectGuid>");
    ns_vs_buffer_append(buffer, project_guid);
    ns_vs_buffer_append(buffer, "</ProjectGuid>\r\n");
    ns_vs_buffer_append(buffer, "    <RootNamespace>");
    ns_vs_buffer_append_xml_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, "</RootNamespace>\r\n");
    ns_vs_buffer_append(buffer, "    <ProjectName>");
    ns_vs_buffer_append_xml_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, "</ProjectName>\r\n");
    ns_vs_buffer_append(buffer, "    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>\r\n");
    ns_vs_buffer_append(buffer, "  </PropertyGroup>\r\n");
    ns_vs_buffer_append(buffer, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\r\n");
    ns_vs_buffer_append(buffer,
                        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\" "
                        "Label=\"Configuration\">\r\n"
                        "    <ConfigurationType>Makefile</ConfigurationType>\r\n"
                        "    <UseDebugLibraries>true</UseDebugLibraries>\r\n"
                        "    <PlatformToolset>v143</PlatformToolset>\r\n"
                        "  </PropertyGroup>\r\n"
                        "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\" "
                        "Label=\"Configuration\">\r\n"
                        "    <ConfigurationType>Makefile</ConfigurationType>\r\n"
                        "    <UseDebugLibraries>false</UseDebugLibraries>\r\n"
                        "    <PlatformToolset>v143</PlatformToolset>\r\n"
                        "  </PropertyGroup>\r\n");
    ns_vs_buffer_append(buffer, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\r\n");
    ns_vs_buffer_append(buffer, "  <ImportGroup Label=\"ExtensionSettings\" />\r\n");
    ns_vs_buffer_append(buffer, "  <ImportGroup Label=\"Shared\" />\r\n");
    ns_vs_buffer_append(buffer, "  <ImportGroup Label=\"PropertySheets\">\r\n");
    ns_vs_buffer_append(buffer, "    <Import Project=\"$(MSBuildProjectDirectory)\\");
    ns_vs_buffer_append_xml_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, ".nsproject\\Config\\NS.Generated.props\" />\r\n");
    ns_vs_buffer_append(buffer, "    <Import Project=\"$(MSBuildProjectDirectory)\\");
    ns_vs_buffer_append_xml_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer,
                        ".nsproject\\Config\\NS.Local.props\" "
                        "Condition=\"Exists('$(MSBuildProjectDirectory)\\");
    ns_vs_buffer_append_xml_ns_str(buffer, spec->safe_name);
    ns_vs_buffer_append(buffer, ".nsproject\\Config\\NS.Local.props')\" />\r\n");
    ns_vs_buffer_append(buffer, "  </ImportGroup>\r\n");
    ns_vs_buffer_append(buffer, "  <PropertyGroup Label=\"UserMacros\" />\r\n");
    ns_vs_append_nmake_settings(buffer, spec->kind, "Debug");
    ns_vs_append_nmake_settings(buffer, spec->kind, "Release");
    ns_vs_buffer_append(buffer, "  <ItemGroup>\r\n");
    ns_vs_buffer_append(buffer,
                        "    <None Include=\"$(NSProjectRoot)\\**\\*.ns\" "
                        "Exclude=\"$(NSProjectRoot)\\bin\\**\\*\">\r\n"
                        "      <Link>NS Sources\\%(RecursiveDir)%(Filename)%(Extension)</Link>\r\n"
                        "    </None>\r\n");
    ns_vs_buffer_append(buffer, "  </ItemGroup>\r\n");
    ns_vs_buffer_append(buffer, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\r\n");
    ns_vs_buffer_append(buffer, "  <ImportGroup Label=\"ExtensionTargets\" />\r\n");
    ns_vs_buffer_append(buffer, "</Project>\r\n");
}

ns_bool ns_project_generate_visual_studio(const ns_project_spec *spec) {
    if (spec == NULL) {
        fprintf(stderr, "ns project: missing Visual Studio project specification.\n");
        return false;
    }
    if (spec->kind != NS_PROJECT_APP && spec->kind != NS_PROJECT_LIBRARY) {
        fprintf(stderr, "ns project: invalid Visual Studio project kind.\n");
        return false;
    }
    if (!ns_vs_valid_input_string(spec->root, false)) {
        fprintf(stderr, "ns project: invalid project root for Visual Studio generation.\n");
        return false;
    }
    if (!ns_vs_valid_safe_name(spec->safe_name)) {
        fprintf(stderr, "ns project: invalid sanitized project name for Visual Studio generation.\n");
        return false;
    }
    if (!ns_vs_valid_input_string(spec->ns_executable, false)) {
        fprintf(stderr, "ns project: invalid ns executable path for Visual Studio generation.\n");
        return false;
    }

    char *root = ns_vs_copy_ns_str(spec->root);
    char *safe_name = ns_vs_copy_ns_str(spec->safe_name);
    if (root == NULL || safe_name == NULL) {
        fprintf(stderr, "ns project: out of memory while preparing Visual Studio paths.\n");
        free(root);
        free(safe_name);
        return false;
    }
    if (ns_vs_path_kind(root) != NS_VS_PATH_DIRECTORY) {
        fprintf(stderr, "ns project: project root is not a directory: %s.\n", root);
        free(root);
        free(safe_name);
        return false;
    }

    char *bin = ns_vs_path_join(root, "bin");
    char *support_leaf = ns_vs_concat(safe_name, ".nsproject");
    char *solution_leaf = ns_vs_concat(safe_name, ".sln");
    char *project_leaf = ns_vs_concat(safe_name, ".vcxproj");
    char *output_leaf = ns_vs_concat(safe_name, ".exe");
    char *support = bin != NULL && support_leaf != NULL ? ns_vs_path_join(bin, support_leaf) : NULL;
    char *config = support != NULL ? ns_vs_path_join(support, "Config") : NULL;
    char *solution_path = bin != NULL && solution_leaf != NULL ? ns_vs_path_join(bin, solution_leaf) : NULL;
    char *project_path = bin != NULL && project_leaf != NULL ? ns_vs_path_join(bin, project_leaf) : NULL;
    char *output_path = bin != NULL && output_leaf != NULL ? ns_vs_path_join(bin, output_leaf) : NULL;
    char *generated_path = config != NULL ? ns_vs_path_join(config, "NS.Generated.props") : NULL;
    char *local_path = config != NULL ? ns_vs_path_join(config, "NS.Local.props") : NULL;

    ns_bool paths_ok = bin != NULL && support_leaf != NULL && solution_leaf != NULL && project_leaf != NULL &&
                       output_leaf != NULL && support != NULL && config != NULL && solution_path != NULL &&
                       project_path != NULL && output_path != NULL && generated_path != NULL && local_path != NULL;
    if (!paths_ok) {
        fprintf(stderr, "ns project: out of memory while preparing Visual Studio paths.\n");
        goto cleanup_failure;
    }
    if (!ns_vs_mkdir_p(config)) {
        fprintf(stderr, "ns project: cannot create managed Visual Studio directory %s: %s.\n", config, strerror(errno));
        goto cleanup_failure;
    }

    char project_guid[39];
    char solution_guid[39];
    ns_vs_make_guid(spec, 0x50U, project_guid);
    ns_vs_make_guid(spec, 0x53U, solution_guid);

    ns_vs_buffer generated;
    ns_vs_buffer local;
    ns_vs_buffer solution;
    ns_vs_buffer project;
    ns_vs_buffer_init(&generated);
    ns_vs_buffer_init(&local);
    ns_vs_buffer_init(&solution);
    ns_vs_buffer_init(&project);
    ns_vs_generate_props(&generated, spec, output_path);
    ns_vs_generate_local_props(&local);
    ns_vs_generate_solution(&solution, spec, project_guid, solution_guid);
    ns_vs_generate_vcxproj(&project, spec, project_guid);

    ns_bool write_ok = ns_vs_write_buffer(generated_path, &generated, true) && ns_vs_write_buffer(local_path, &local, false) &&
                       ns_vs_write_buffer(project_path, &project, false) && ns_vs_write_buffer(solution_path, &solution, false);
    ns_vs_buffer_free(&generated);
    ns_vs_buffer_free(&local);
    ns_vs_buffer_free(&solution);
    ns_vs_buffer_free(&project);
    if (!write_ok) goto cleanup_failure;

    free(local_path);
    free(generated_path);
    free(output_path);
    free(project_path);
    free(solution_path);
    free(config);
    free(support);
    free(output_leaf);
    free(project_leaf);
    free(solution_leaf);
    free(support_leaf);
    free(bin);
    free(safe_name);
    free(root);
    return true;

cleanup_failure:
    free(local_path);
    free(generated_path);
    free(output_path);
    free(project_path);
    free(solution_path);
    free(config);
    free(support);
    free(output_leaf);
    free(project_leaf);
    free(solution_leaf);
    free(support_leaf);
    free(bin);
    free(safe_name);
    free(root);
    return false;
}
