#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "gpu.h"
#include "ns_type.h"
#include "view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define UI_VERTEX_STRIDE 36
#define UI_INITIAL_VERTEX_CAP 131072
#define UI_MAX_COMMANDS 4096
#define UI_MAX_CLIPS 32
#define UI_MAX_GPU_CLIPS 4096
#define UI_MAX_TEXTURES 32
#define UI_MAX_RECT_BATCHES 16
#define UI_FONT_MAIN 0
#define UI_FONT_MONO 1
#define UI_FONT_ZH 2
#define UI_FONT_BITMAP 3
#define UI_FONT_BITMAP_ZH 4
#define UI_FONT_COUNT 5
#define UI_WHITE_TEXTURE 1
#define UI_FONT_TEXTURE 2
#define UI_FONT_ZH_TEXTURE (UI_MAX_TEXTURES + 3)
#define UI_FONT_BITMAP_TEXTURE (UI_MAX_TEXTURES + 4)
#define UI_FONT_BITMAP_ZH_TEXTURE (UI_MAX_TEXTURES + 5)
#define UI_KIND_IMAGE 0
#define UI_KIND_MSDF 1
#define UI_KIND_ARC_SDF 2
#define UI_KIND_BITMAP 3
#define UI_DEFAULT_FEATHER 0.5
#define UI_BITMAP_FONT_SIZE 10.0
// Head-locked HUD distance in tracking metres. Same NDC overlay at z=0 is
// optical infinity and ghosts against the stereo world; this plane sits in
// front of the 3D camera so each eye gets matching disparity and depth.
#define UI_HUD_DISTANCE_METRES 1.5f

typedef struct io_image {
    i32 width;
    i32 height;
    i32 channels;
    u8 *data;
} io_image;

extern io_image *io_load_image(const char *path);

#define UI_PATH_MAX 4096

static const char ui_module_anchor = 0;

static ns_bool ui_file_readable(const char *path) {
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return false;
    fclose(file);
    return true;
}

static ns_bool ui_module_directory(char out[UI_PATH_MAX]) {
#if defined(_WIN32)
    HMODULE module = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            &ui_module_anchor, &module)) return false;
    DWORD len = GetModuleFileNameA(module, out, UI_PATH_MAX);
    if (len == 0 || len >= UI_PATH_MAX) return false;
#else
    Dl_info info = {0};
    if (dladdr(&ui_module_anchor, &info) == 0 || !info.dli_fname) return false;
    i32 len = snprintf(out, UI_PATH_MAX, "%s", info.dli_fname);
    if (len <= 0 || len >= UI_PATH_MAX) return false;
#endif
    char *slash = strrchr(out, '/');
#if defined(_WIN32)
    char *backslash = strrchr(out, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (!slash) return false;
    *slash = '\0';
    return true;
}

// Resolve bundled UI resources independently of the process working
// directory. Installed dylibs live under <root>/lib with assets under
// <root>/ref/assets; source-tree dylibs live in <repo>/bin with assets under
// <repo>/lib/assets; generated macOS apps keep them in Contents/Resources.
// NS_UI_ASSET_ROOT remains available for custom packaging.
static ns_bool ui_resolve_asset(const char *name, char out[UI_PATH_MAX]) {
    const char *override = getenv("NS_UI_ASSET_ROOT");
    if (override && override[0]) {
        i32 len = snprintf(out, UI_PATH_MAX, "%s/%s", override, name);
        if (len > 0 && len < UI_PATH_MAX && ui_file_readable(out)) return true;
    }

    char module_dir[UI_PATH_MAX];
    if (ui_module_directory(module_dir)) {
        const char *layouts[] = {".", "assets", "../Resources", "../ref/assets", "../lib/assets"};
        for (u32 i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
            i32 len = snprintf(out, UI_PATH_MAX, "%s/%s/%s", module_dir, layouts[i], name);
            if (len > 0 && len < UI_PATH_MAX && ui_file_readable(out)) return true;
        }
    }

    i32 len = snprintf(out, UI_PATH_MAX, "lib/assets/%s", name);
    return len > 0 && len < UI_PATH_MAX && ui_file_readable(out);
}

typedef struct ui_color_rgba {
    f64 r;
    f64 g;
    f64 b;
    f64 a;
} ui_color_rgba;

typedef struct ui_rect {
    f64 x;
    f64 y;
    f64 w;
    f64 h;
} ui_rect;

typedef struct ui_text_size {
    f64 w;
    f64 h;
} ui_text_size;

enum {
    UI_ALIGN_LEFT = 1,
    UI_ALIGN_RIGHT = 2,
    UI_ALIGN_TOP = 4,
    UI_ALIGN_BOTTOM = 8,
    UI_ALIGN_CENTER_HORIZONTAL = 16,
    UI_ALIGN_CENTER_VERTICAL = 32,
};

typedef struct ui_vertex {
    f32 x, y;
    f32 u, v;
    u32 color;
    f32 range, weight, softness, clip;
} ui_vertex;

typedef struct ui_clip {
    f64 x, y, w, h;
} ui_clip;

typedef struct ui_gpu_clip {
    f32 x0, y0, x1, y1;
} ui_gpu_clip;

typedef struct ui_command {
    i32 vertex_offset;
    i32 vertex_count;
    i32 texture_id;
    i32 kind;
    i32 rect_batch_id;
    f64 offset_x, offset_y;
    i32 clip_x, clip_y, clip_w, clip_h;
} ui_command;

typedef struct ui_rect_batch {
    ui_vertex *vertices;
    i32 vertex_count;
    i32 vertex_capacity;
    u64 gpu_offset;
    ns_bool used;
} ui_rect_batch;

typedef struct ui_glyph {
    i32 code;
    f64 width;
    f64 height;
    f64 x_offset;
    f64 y_offset;
    f64 x_advance;
    f64 atlas_x;
    f64 atlas_y;
} ui_glyph;

typedef struct ui_font {
    ui_glyph *glyphs;
    i32 glyph_count;
    i32 texture_width;
    i32 texture_height;
    f64 font_size;
    f64 line_height;
    f64 baseline;
    f64 cap_top;
} ui_font;

// Margins of the drawable that platform chrome may cover, in logical points.
typedef struct ui_insets {
    f64 top;
    f64 right;
    f64 bottom;
    f64 left;
} ui_insets;

typedef struct ui_renderer {
    void *handle;
    view *v;
    // The two rectangles the renderer lays out against, both in logical points
    // and both in drawable space. `rect` is the whole screen, device chrome
    // included; `safe_rect` is the part of it the notch, the status bar and the
    // home indicator leave alone. Vertices reach the GPU in drawable space,
    // while the public API draws relative to the safe rect's origin: layout
    // lands inside the safe area by default, and a pass that wants the whole
    // screen asks for ui_surface_rect instead.
    ui_rect rect;
    ui_rect safe_rect;
    f64 content_scale;

    // Device safe area. `insets` mirrors the view unless an application
    // overrides it; `safe_rect` is resolved from them by ui_resolve_safe_area
    // whenever any input changes.
    ui_insets insets;
    ns_bool insets_overridden;
    ns_bool safe_area_enabled;

    ui_vertex *vertices;
    i32 vertex_count;
    i32 vertex_capacity;
    ui_command commands[UI_MAX_COMMANDS];
    i32 command_count;
    ui_clip clips[UI_MAX_CLIPS];
    i32 clip_count;
    ui_gpu_clip gpu_clips[UI_MAX_GPU_CLIPS];
    i32 gpu_clip_count;
    i32 current_texture_id;

    ui_font fonts[UI_FONT_COUNT];
    u32 white_texture;
    u32 font_texture;
    u32 font_zh_texture;
    u32 font_bitmap_texture;
    u32 font_bitmap_zh_texture;
    u32 shader_image;
    u32 shader_msdf;
    u32 shader_bitmap;
    u32 shader_arc_sdf;
    u32 render_state;
    u32 render_state_hud;
    u32 textures[UI_MAX_TEXTURES];
    i32 texture_widths[UI_MAX_TEXTURES];
    i32 texture_heights[UI_MAX_TEXTURES];
    ui_rect_batch rect_batches[UI_MAX_RECT_BATCHES];
    gpu_addr storage;
    u64 storage_capacity;
    ns_bool gpu_ready;
} ui_renderer;

typedef struct ui_gpu_root {
    f32 texture_id;
    f32 unused_texture_id;
    f32 screen_width;
    f32 screen_height;
    f32 offset_x;
    f32 offset_y;
    u32 vertex_offset;
    u32 clip_offset;
    f32 hud_center_enable[4];
    f32 hud_right_hw[4];
    f32 hud_up_hh[4];
    f32 hud_proj[4];
    f32 hud_depth[4];
} ui_gpu_root;

typedef struct ui_theme { void *handle; } ui_theme;
typedef struct ui_hit { ns_bool hovered; ns_bool pressed; } ui_hit;
typedef struct ui_text_sel {
    i32 active;
    i32 anchor;
    i32 head;
    ns_bool dragging;
} ui_text_sel;
typedef struct ui_input {
    f64 mouse_x, mouse_y;
    ns_bool mouse_down, mouse_pressed, mouse_released;
    ns_bool mouse_middle_down, mouse_right_pressed, mouse_right_down;
    f64 pan_dx, pan_dy, zoom_factor, wheel_y;
    const char *typed_text, *ime_composition;
    ns_bool key_backspace, key_delete, key_enter, key_escape;
    ns_bool key_left, key_right, key_up, key_down, key_home, key_end;
    ns_bool key_page_up, key_page_down, key_a, key_c;
    ns_bool shift, ctrl, meta, alt, gizmo_manipulating;
} ui_input;

typedef struct ui_widgets {
    void *handle;
    ui_renderer *renderer;
    ui_input input;
    u32 active_id;
    ns_bool light;
} ui_widgets;

ui_input *ui_input_empty(void) {
    static ui_input input;
    memset(&input, 0, sizeof(input));
    input.zoom_factor = 1.0;
    return &input;
}

ui_theme *ui_theme_empty(void) {
    static ui_theme theme;
    memset(&theme, 0, sizeof(theme));
    return &theme;
}

void ui_renderer_destroy(ui_renderer *r);
void ui_fill_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, u32 rgba, f64 feather);
void ui_fill_gradient_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h,
                           u32 rgba_top_left, u32 rgba_top_right,
                           u32 rgba_bottom_right, u32 rgba_bottom_left);
void ui_fill_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, u32 rgba, f64 feather);
void ui_fill_arc(ui_renderer *r, f64 cx, f64 cy, f64 radius, f64 thickness, f64 angle_start, f64 angle_end, u32 rgba, f64 feather);
void ui_stroke_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, f64 thickness, u32 rgba, f64 feather);
void ui_draw_text(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type);
void ui_draw_text_arc(ui_renderer *r, f64 cx, f64 cy, f64 radius, f64 center_angle, const char *text, f64 font_px, u32 rgba, i32 font_type);
void ui_draw_text_vertical(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type);
f64 ui_text_vertical_column_width(ui_renderer *r, f64 font_px, i32 font_type);
i32 ui_text_vertical_column_count(const char *text);
i32 ui_text_vertical_max_run(const char *text);
static void ui_round_rect_points(f64 *pts, i32 *out_n, f64 x, f64 y, f64 w, f64 h, f64 radius);
static void ui_draw_round_ring(ui_renderer *r, const f64 *outer, const f64 *inner, i32 n, u32 outer_color, u32 inner_color);

static const char *ui_shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct UiRoot { float texture_id; float unused_texture_id; float screen_width; float screen_height; float offset_x; float offset_y; uint vertex_offset; uint clip_offset; float4 hud_center_enable; float4 hud_right_hw; float4 hud_up_hh; float4 hud_proj; float4 hud_depth; };\n"
"struct VOut { float4 pos [[position]]; float2 pixel; float2 uv; float4 col; float4 params; };\n"
"vertex VOut ui_vs(uint vertex_id [[vertex_id]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]]) {\n"
"  uint base = ns_root.vertex_offset / 4u + vertex_id * 9u;\n"
"  float2 pixel = float2(as_type<float>(ns_storage_buffer[base]), as_type<float>(ns_storage_buffer[base + 1u])) + float2(ns_root.offset_x, ns_root.offset_y);\n"
"  float2 uv = float2(as_type<float>(ns_storage_buffer[base + 2u]), as_type<float>(ns_storage_buffer[base + 3u]));\n"
"  uint color = ns_storage_buffer[base + 4u];\n"
"  float4 params = float4(as_type<float>(ns_storage_buffer[base + 5u]), as_type<float>(ns_storage_buffer[base + 6u]), as_type<float>(ns_storage_buffer[base + 7u]), as_type<float>(ns_storage_buffer[base + 8u]));\n"
"  float2 screen = float2(ns_root.screen_width, ns_root.screen_height);\n"
"  float2 ndc = float2((pixel.x / screen.x) * 2.0 - 1.0, 1.0 - (pixel.y / screen.y) * 2.0);\n"
"  float4 pos = float4(ndc, 0.0, 1.0);\n"
"  if (ns_root.hud_center_enable.w > 0.5) {\n"
"    float3 view = ns_root.hud_center_enable.xyz + ns_root.hud_right_hw.xyz * (ndc.x * ns_root.hud_right_hw.w) + ns_root.hud_up_hh.xyz * (ndc.y * ns_root.hud_up_hh.w);\n"
"    float z = max(view.z, 0.001);\n"
"    float clip_x = view.x * ns_root.hud_proj.x - z * ns_root.hud_proj.z;\n"
"    float clip_y = view.y * ns_root.hud_proj.y - z * ns_root.hud_proj.w;\n"
"    float ndc_z = clamp(0.0 - ns_root.hud_depth.x + ns_root.hud_depth.y / z, 0.0, 1.0);\n"
"    pos = float4(clip_x, clip_y, ndc_z * z, z);\n"
"  }\n"
"  VOut o; o.pos = pos; o.pixel = pixel; o.uv = uv;\n"
"  o.col = float4(float((color >> 0u) & 255u), float((color >> 8u) & 255u), float((color >> 16u) & 255u), float((color >> 24u) & 255u)) / 255.0;\n"
"  o.params = params; return o;\n"
"}\n"
"static inline half ui_median3(half r, half g, half b) { return max(min(r, g), min(max(r, g), b)); }\n"
"static inline bool ui_clip_discard(VOut in, constant UiRoot &ns_root, device const uint *ns_storage_buffer) {\n"
"  uint clip_idx = uint(round(max(in.params.w, 0.0)));\n"
"  if (clip_idx == 0u) { return false; }\n"
"  uint base = ns_root.clip_offset / 4u + (clip_idx - 1u) * 4u;\n"
"  float4 c = float4(as_type<float>(ns_storage_buffer[base]), as_type<float>(ns_storage_buffer[base + 1u]), as_type<float>(ns_storage_buffer[base + 2u]), as_type<float>(ns_storage_buffer[base + 3u]));\n"
"  return in.pixel.x < c.x || in.pixel.y < c.y || in.pixel.x >= c.z || in.pixel.y >= c.w;\n"
"}\n"
"fragment float4 ui_fs_image(VOut in [[stage_in]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]], texture2d<float> ns_texture_map [[texture(1)]]) {\n"
"  if (ui_clip_discard(in, ns_root, ns_storage_buffer)) { discard_fragment(); }\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
"  return ns_texture_map.sample(samp, in.uv) * in.col;\n"
"}\n"
"fragment float4 ui_fs_msdf(VOut in [[stage_in]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]], texture2d<float> ns_texture_map [[texture(1)]]) {\n"
"  if (ui_clip_discard(in, ns_root, ns_storage_buffer)) { discard_fragment(); }\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
"  float4 s = ns_texture_map.sample(samp, in.uv); half sd = ui_median3(half(s.r), half(s.g), half(s.b));\n"
"  float2 tex_size = float2(ns_texture_map.get_width(), ns_texture_map.get_height()); float range = max(in.params.x, 0.5);\n"
"  float2 unit_range = float2(range) / tex_size; float2 screen_texel = max(fwidth(in.uv), float2(1e-6));\n"
"  float px_range = max(0.5 * dot(unit_range, 1.0 / screen_texel), 1.0);\n"
"  float opacity = clamp(((float(sd) - 0.5) * px_range + in.params.y) / max(in.params.z, 1.0) + 0.5, 0.0, 1.0);\n"
"  return float4(in.col.rgb, in.col.a * opacity);\n"
"}\n"
"fragment float4 ui_fs_bitmap(VOut in [[stage_in]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]], texture2d<float> ns_texture_map [[texture(1)]]) {\n"
"  if (ui_clip_discard(in, ns_root, ns_storage_buffer)) { discard_fragment(); }\n"
"  constexpr sampler samp(mag_filter::nearest, min_filter::nearest, mip_filter::none, address::clamp_to_edge);\n"
"  float4 sample = ns_texture_map.sample(samp, in.uv); float coverage = min(sample.r, sample.a);\n"
"  return float4(in.col.rgb, in.col.a * coverage);\n"
"}\n"
"fragment float4 ui_fs_arc_sdf(VOut in [[stage_in]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]]) {\n"
"  if (ui_clip_discard(in, ns_root, ns_storage_buffer)) { discard_fragment(); }\n"
"  float radius = max(in.params.x, 0.0001); float half_width = max(in.params.y, 0.0);\n"
"  float half_angle = clamp(in.params.z, 0.0, 3.14159265); float radial = length(in.uv);\n"
"  float angle = atan2(in.uv.y, in.uv.x); float half_arc = radius * half_angle;\n"
"  float corner = min(half_width * 0.44, half_arc * 0.48);\n"
"  float2 extent = max(float2(half_arc, half_width) - float2(corner), float2(0.0));\n"
"  float2 q = abs(float2(angle * radius, radial - radius)) - extent;\n"
"  float distance = length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - corner;\n"
"  float aa = max(fwidth(distance), 0.35); float opacity = 1.0 - smoothstep(-aa, aa, distance);\n"
"  return float4(in.col.rgb, in.col.a * opacity);\n"
"}\n";

static f64 ui_clamp_f64(f64 v, f64 lo, f64 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static f64 ui_resolve_feather(f64 feather) {
    return feather > 0.0 ? feather : UI_DEFAULT_FEATHER;
}

static i32 ui_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static u32 ui_hex_byte(const char *hex, i32 i) {
    return (u32)((ui_hex_digit(hex[i]) << 4) | ui_hex_digit(hex[i + 1]));
}

u32 ui_pack_color(const char *hex) {
    if (!hex || hex[0] != '#') return 0xff000000u;
    u32 r = ui_hex_byte(hex, 1);
    u32 g = ui_hex_byte(hex, 3);
    u32 b = ui_hex_byte(hex, 5);
    u32 a = 0xffu;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static char *ui_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    char *data = (char*)malloc((size_t)len + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)len, f);
    fclose(f);
    data[got] = '\0';
    if (out_len) *out_len = got;
    return data;
}

static char *ui_find_key(char *p, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(p, needle);
}

static f64 ui_parse_number(char **p) {
    while (**p && (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t' || **p == ':' || **p == ',' || **p == '[')) (*p)++;
    char *end = *p;
    f64 v = strtod(*p, &end);
    *p = end;
    return v;
}

static f64 ui_json_key_number(char *base, const char *key, f64 fallback) {
    char *p = ui_find_key(base, key);
    if (!p) return fallback;
    p = strchr(p, ':');
    if (!p) return fallback;
    p++;
    return ui_parse_number(&p);
}

static i32 ui_glyph_cmp(const void *a, const void *b) {
    const ui_glyph *ga = (const ui_glyph*)a;
    const ui_glyph *gb = (const ui_glyph*)b;
    return (ga->code > gb->code) - (ga->code < gb->code);
}

static ui_glyph *ui_font_glyph(ui_font *font, i32 code) {
    i32 lo = 0;
    i32 hi = font->glyph_count - 1;
    while (lo <= hi) {
        i32 mid = lo + (hi - lo) / 2;
        i32 c = font->glyphs[mid].code;
        if (c == code) return &font->glyphs[mid];
        if (c < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static void ui_detect_cap_metrics(ui_font *font) {
    static const i32 refs[] = {72, 77, 78, 73, 76, 69, 88, 84};
    for (u32 i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        ui_glyph *g = ui_font_glyph(font, refs[i]);
        if (g && g->height > 0) {
            font->cap_top = g->y_offset;
            font->baseline = g->y_offset + g->height;
            return;
        }
    }
    font->cap_top = round(font->font_size * 0.1);
    font->baseline = round(font->font_size * 0.8);
}

static ns_bool ui_load_font_face(char *json, const char *face_name, i32 tex_w, i32 tex_h, ui_font *font) {
    char *face = (face_name && face_name[0]) ? ui_find_key(json, face_name) : json;
    if (!face) return false;
    char *chars_key = ui_find_key(face, "chars");
    if (!chars_key) return false;
    char *p = strchr(chars_key, '[');
    if (!p) return false;
    p++;

    i32 cap = 256;
    font->glyphs = (ui_glyph*)calloc((size_t)cap, sizeof(ui_glyph));
    if (!font->glyphs) return false;
    font->glyph_count = 0;
    font->texture_width = tex_w;
    font->texture_height = tex_h;
    font->font_size = ui_json_key_number(face, "size", 42);
    font->line_height = ui_json_key_number(face, "line_height", font->font_size * 1.4);

    while (*p) {
        while (*p && *p != '[' && *p != ']') p++;
        if (*p == ']') break;
        p++;
        ui_glyph g = {0};
        g.code = (i32)ui_parse_number(&p);
        g.width = ui_parse_number(&p);
        g.height = ui_parse_number(&p);
        g.x_offset = ui_parse_number(&p);
        g.y_offset = ui_parse_number(&p);
        g.x_advance = ui_parse_number(&p);
        g.atlas_x = ui_parse_number(&p);
        g.atlas_y = ui_parse_number(&p);
        while (*p && *p != ']') p++;
        if (*p == ']') p++;
        if (font->glyph_count >= cap) {
            cap *= 2;
            ui_glyph *next = (ui_glyph*)realloc(font->glyphs, (size_t)cap * sizeof(ui_glyph));
            if (!next) return false;
            font->glyphs = next;
        }
        font->glyphs[font->glyph_count++] = g;
    }

    qsort(font->glyphs, (size_t)font->glyph_count, sizeof(ui_glyph), ui_glyph_cmp);
    ui_detect_cap_metrics(font);
    return true;
}

static ns_bool ui_font_append_glyph(ui_font *font, i32 *capacity, ui_glyph glyph) {
    if (font->glyph_count >= *capacity) {
        *capacity *= 2;
        ui_glyph *next = (ui_glyph*)realloc(font->glyphs, (size_t)*capacity * sizeof(ui_glyph));
        if (!next) return false;
        font->glyphs = next;
    }
    font->glyphs[font->glyph_count++] = glyph;
    return true;
}

// Construct bitmap Latin format:
// [{"c":65,"xo":0,"yo":2,"xa":6,"uv":[62,18,6,7]}, ...]
static ns_bool ui_load_bitmap_latin_face(char *json, i32 tex_w, i32 tex_h, ui_font *font) {
    i32 capacity = 128;
    font->glyphs = (ui_glyph*)calloc((size_t)capacity, sizeof(ui_glyph));
    if (!font->glyphs) return false;
    font->texture_width = tex_w;
    font->texture_height = tex_h;
    font->font_size = UI_BITMAP_FONT_SIZE;
    font->line_height = UI_BITMAP_FONT_SIZE;

    char *cursor = json;
    while ((cursor = strchr(cursor, '{')) != NULL) {
        char *end = strchr(cursor, '}');
        if (!end) break;
        char *uv = ui_find_key(cursor, "uv");
        if (!uv || uv > end) {
            cursor = end + 1;
            continue;
        }
        uv = strchr(uv, '[');
        if (!uv || uv > end) return false;
        uv++;
        ui_glyph glyph = {0};
        glyph.code = (i32)ui_json_key_number(cursor, "c", -1);
        glyph.x_offset = ui_json_key_number(cursor, "xo", 0);
        glyph.y_offset = ui_json_key_number(cursor, "yo", 0);
        glyph.x_advance = ui_json_key_number(cursor, "xa", 6);
        glyph.atlas_x = ui_parse_number(&uv);
        glyph.atlas_y = ui_parse_number(&uv);
        glyph.width = ui_parse_number(&uv);
        glyph.height = ui_parse_number(&uv);
        if (glyph.code >= 0 && !ui_font_append_glyph(font, &capacity, glyph)) return false;
        cursor = end + 1;
    }
    if (font->glyph_count <= 0) return false;
    qsort(font->glyphs, (size_t)font->glyph_count, sizeof(ui_glyph), ui_glyph_cmp);
    ui_detect_cap_metrics(font);
    return true;
}

// Construct bitmap CJK format:
// {"19968":[0,0,10,10], ...}
static ns_bool ui_load_bitmap_chinese_face(char *json, i32 tex_w, i32 tex_h, ui_font *font) {
    i32 capacity = 8192;
    font->glyphs = (ui_glyph*)calloc((size_t)capacity, sizeof(ui_glyph));
    if (!font->glyphs) return false;
    font->texture_width = tex_w;
    font->texture_height = tex_h;
    font->font_size = UI_BITMAP_FONT_SIZE;
    font->line_height = UI_BITMAP_FONT_SIZE;
    font->cap_top = 0.0;
    font->baseline = UI_BITMAP_FONT_SIZE;

    char *cursor = json;
    while ((cursor = strchr(cursor, '"')) != NULL) {
        char *key_end = strchr(cursor + 1, '"');
        if (!key_end) break;
        char *number_end = NULL;
        long code = strtol(cursor + 1, &number_end, 10);
        if (number_end != key_end) {
            cursor = key_end + 1;
            continue;
        }
        char *values = strchr(key_end, '[');
        if (!values) break;
        values++;
        ui_glyph glyph = {0};
        glyph.code = (i32)code;
        glyph.atlas_x = ui_parse_number(&values);
        glyph.atlas_y = ui_parse_number(&values);
        glyph.width = ui_parse_number(&values);
        glyph.height = ui_parse_number(&values);
        glyph.x_advance = glyph.width;
        if (!ui_font_append_glyph(font, &capacity, glyph)) return false;
        cursor = values;
    }
    if (font->glyph_count <= 0) return false;
    qsort(font->glyphs, (size_t)font->glyph_count, sizeof(ui_glyph), ui_glyph_cmp);
    return true;
}

static u32 ui_create_rgba_texture(const void *data, i32 width, i32 height) {
    if (!data || width <= 0 || height <= 0) return 0;
    u32 texture = gpu_texture_create(width, height, 1, PIXELFORMAT_RGBA8,
                                     TEXTURE_USAGE_READ, 1, TEXTURE_2D);
    if (texture) {
        gpu_texture_upload(texture, 0, 0, data,
                           (u64)(size_t)width * (u64)(size_t)height * 4u);
    }
    return texture;
}

static u32 ui_create_font_texture(io_image *image) {
    if (!image || !image->data || image->width <= 0 || image->height <= 0 || image->channels <= 0) {
        return 0;
    }
    const size_t pixel_count = (size_t)image->width * (size_t)image->height;
    u8 *rgba = image->data;
    if (image->channels != 4) {
        rgba = (u8*)malloc(pixel_count * 4);
        if (!rgba) return 0;
        for (size_t i = 0; i < pixel_count; i++) {
            const u8 value = image->data[i * (size_t)image->channels];
            rgba[i * 4 + 0] = value;
            rgba[i * 4 + 1] = value;
            rgba[i * 4 + 2] = value;
            rgba[i * 4 + 3] = 255;
        }
    }
    u32 texture = ui_create_rgba_texture(rgba, image->width, image->height);
    if (rgba != image->data) free(rgba);
    return texture;
}

static ns_bool ui_load_fonts(ui_renderer *r) {
    char json_path[UI_PATH_MAX];
    if (!ui_resolve_asset("latin_mono.json", json_path)) {
        fprintf(stderr, "ui: cannot locate latin_mono.json\n");
        return false;
    }
    size_t len = 0;
    char *json = ui_read_file(json_path, &len);
    ns_unused(len);
    if (!json) return false;
    i32 tex_w = (i32)ui_json_key_number(json, "width", 512);
    i32 tex_h = (i32)ui_json_key_number(json, "height", 512);
    ns_bool ok = ui_load_font_face(json, "FONT_MAIN", tex_w, tex_h, &r->fonts[UI_FONT_MAIN]) &&
                 ui_load_font_face(json, "FONT_MONO", tex_w, tex_h, &r->fonts[UI_FONT_MONO]);
    free(json);
    return ok;
}

// The drawable in drawing coordinates. The insets sit at negative coordinates
// because the origin is the safe rect's top-left, so this is what a background
// fills to reach under the device's chrome.
static ui_clip ui_surface_clip(ui_renderer *r) {
    return (ui_clip){r->rect.x - r->safe_rect.x, r->rect.y - r->safe_rect.y,
                     r->rect.w, r->rect.h};
}

// Nothing is clipped to the safe area on its own: the base clip is the whole
// screen and an application narrows it with ui_push_clip where it wants to.
static ui_clip ui_current_clip(ui_renderer *r) {
    if (r->clip_count <= 0) return ui_surface_clip(r);
    return r->clips[r->clip_count - 1];
}

static f64 ui_clip_param(ui_renderer *r, ui_clip c) {
    if (!r || c.w <= 0.0 || c.h <= 0.0) return 0.0;
    // Clips are authored in content space; the shader tests them against the
    // drawable-space pixel it shades.
    const f64 x0 = c.x + r->safe_rect.x;
    const f64 y0 = c.y + r->safe_rect.y;
    if (x0 <= 0.0 && y0 <= 0.0 && x0 + c.w >= r->rect.w && y0 + c.h >= r->rect.h) {
        return 0.0;
    }

    ui_gpu_clip gpu_clip = {
        .x0 = (f32)x0,
        .y0 = (f32)y0,
        .x1 = (f32)(x0 + c.w),
        .y1 = (f32)(y0 + c.h),
    };
    for (i32 i = 0; i < r->gpu_clip_count; i++) {
        ui_gpu_clip *existing = &r->gpu_clips[i];
        if (existing->x0 == gpu_clip.x0 && existing->y0 == gpu_clip.y0 &&
            existing->x1 == gpu_clip.x1 && existing->y1 == gpu_clip.y1) {
            return (f64)(i + 1);
        }
    }
    if (r->gpu_clip_count >= UI_MAX_GPU_CLIPS) return 0.0;
    r->gpu_clips[r->gpu_clip_count] = gpu_clip;
    r->gpu_clip_count++;
    return (f64)r->gpu_clip_count;
}

static void ui_emit_command(ui_renderer *r, i32 base, i32 count, i32 kind) {
    if (r->command_count >= UI_MAX_COMMANDS) return;
    ui_clip c = ui_current_clip(r);
    if (c.w <= 0 || c.h <= 0) return;
    // The scissor is a drawable-space rectangle.
    c.x += r->safe_rect.x;
    c.y += r->safe_rect.y;
    ui_command *cmd = NULL;
    if (r->command_count > 0) {
        cmd = &r->commands[r->command_count - 1];
        if (cmd->rect_batch_id == 0 &&
            cmd->vertex_offset + cmd->vertex_count == base &&
            cmd->texture_id == r->current_texture_id &&
            cmd->kind == kind &&
            cmd->clip_x == (i32)floor(c.x) &&
            cmd->clip_y == (i32)floor(c.y) &&
            cmd->clip_w == (i32)ceil(c.w) &&
            cmd->clip_h == (i32)ceil(c.h)) {
            cmd->vertex_count += count;
            return;
        }
    }

    cmd = &r->commands[r->command_count++];
    *cmd = (ui_command){
        .vertex_offset = base,
        .vertex_count = count,
        .texture_id = r->current_texture_id,
        .kind = kind,
        .rect_batch_id = 0,
        .clip_x = (i32)floor(c.x),
        .clip_y = (i32)floor(c.y),
        .clip_w = (i32)ceil(c.w),
        .clip_h = (i32)ceil(c.h),
    };
}

static ns_bool ui_push_vertex(ui_renderer *r, f64 x, f64 y, f64 u, f64 v, u32 color, f64 range, f64 weight, f64 softness, f64 clip) {
    if (r->vertex_count >= r->vertex_capacity) return false;
    // Geometry is authored in the safe content space; the safe-area origin is
    // the single translation into drawable space.
    x += r->safe_rect.x;
    y += r->safe_rect.y;
    r->vertices[r->vertex_count++] = (ui_vertex){
        .x = (f32)x, .y = (f32)y, .u = (f32)u, .v = (f32)v, .color = color,
        .range = (f32)range, .weight = (f32)weight, .softness = (f32)softness, .clip = (f32)clip,
    };
    return true;
}

static void ui_push_quad_ex(ui_renderer *r, f64 x0, f64 y0, f64 x1, f64 y1, f64 u0, f64 v0, f64 u1, f64 v1, u32 color, i32 kind, f64 range, f64 weight, f64 softness) {
    ui_clip clip = ui_current_clip(r);
    f64 cx0 = fmax(x0, clip.x);
    f64 cy0 = fmax(y0, clip.y);
    f64 cx1 = fmin(x1, clip.x + clip.w);
    f64 cy1 = fmin(y1, clip.y + clip.h);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    f64 inv_w = 1.0 / fmax(0.000001, x1 - x0);
    f64 inv_h = 1.0 / fmax(0.000001, y1 - y0);
    f64 cu0 = u0 + (u1 - u0) * ((cx0 - x0) * inv_w);
    f64 cv0 = v0 + (v1 - v0) * ((cy0 - y0) * inv_h);
    f64 cu1 = u0 + (u1 - u0) * ((cx1 - x0) * inv_w);
    f64 cv1 = v0 + (v1 - v0) * ((cy1 - y0) * inv_h);
    const f64 clip_param = ui_clip_param(r, clip);
    i32 base = r->vertex_count;
    if (!ui_push_vertex(r, cx0, cy0, cu0, cv0, color, range, weight, softness, clip_param) ||
        !ui_push_vertex(r, cx1, cy0, cu1, cv0, color, range, weight, softness, clip_param) ||
        !ui_push_vertex(r, cx1, cy1, cu1, cv1, color, range, weight, softness, clip_param) ||
        !ui_push_vertex(r, cx0, cy0, cu0, cv0, color, range, weight, softness, clip_param) ||
        !ui_push_vertex(r, cx1, cy1, cu1, cv1, color, range, weight, softness, clip_param) ||
        !ui_push_vertex(r, cx0, cy1, cu0, cv1, color, range, weight, softness, clip_param)) {
        r->vertex_count = base;
        return;
    }
    ui_emit_command(r, base, 6, kind);
}

static void ui_push_quad_rotated(ui_renderer *r, f64 origin_x, f64 origin_y, f64 cosine, f64 sine,
                                 f64 x0, f64 y0, f64 x1, f64 y1,
                                 f64 u0, f64 v0, f64 u1, f64 v1,
                                 u32 color, i32 kind, f64 range, f64 weight, f64 softness) {
    const f64 local_x[4] = {x0, x1, x1, x0};
    const f64 local_y[4] = {y0, y0, y1, y1};
    const f64 uv_x[4] = {u0, u1, u1, u0};
    const f64 uv_y[4] = {v0, v0, v1, v1};
    f64 px[4], py[4];
    f64 min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
    for (i32 i = 0; i < 4; i++) {
        px[i] = origin_x + cosine * local_x[i] - sine * local_y[i];
        py[i] = origin_y + sine * local_x[i] + cosine * local_y[i];
        min_x = fmin(min_x, px[i]);
        min_y = fmin(min_y, py[i]);
        max_x = fmax(max_x, px[i]);
        max_y = fmax(max_y, py[i]);
    }
    ui_clip clip = ui_current_clip(r);
    if (max_x <= clip.x || max_y <= clip.y || min_x >= clip.x + clip.w || min_y >= clip.y + clip.h) return;
    const i32 order[6] = {0, 1, 2, 0, 2, 3};
    const f64 clip_param = ui_clip_param(r, clip);
    const i32 base = r->vertex_count;
    for (i32 i = 0; i < 6; i++) {
        const i32 vertex = order[i];
        if (!ui_push_vertex(r, px[vertex], py[vertex], uv_x[vertex], uv_y[vertex],
                            color, range, weight, softness, clip_param)) {
            r->vertex_count = base;
            return;
        }
    }
    ui_emit_command(r, base, 6, kind);
}

static void ui_push_tri(ui_renderer *r, f64 x0, f64 y0, f64 x1, f64 y1, f64 x2, f64 y2, f64 u, f64 v, u32 color) {
    ui_clip clip = ui_current_clip(r);
    f64 min_x = fmin(x0, fmin(x1, x2));
    f64 min_y = fmin(y0, fmin(y1, y2));
    f64 max_x = fmax(x0, fmax(x1, x2));
    f64 max_y = fmax(y0, fmax(y1, y2));
    if (max_x <= clip.x || max_y <= clip.y || min_x >= clip.x + clip.w || min_y >= clip.y + clip.h) return;
    const f64 clip_param = ui_clip_param(r, clip);
    i32 base = r->vertex_count;
    if (!ui_push_vertex(r, x0, y0, u, v, color, 0, 0, 0, clip_param) ||
        !ui_push_vertex(r, x1, y1, u, v, color, 0, 0, 0, clip_param) ||
        !ui_push_vertex(r, x2, y2, u, v, color, 0, 0, 0, clip_param)) {
        r->vertex_count = base;
        return;
    }
    ui_emit_command(r, base, 3, UI_KIND_IMAGE);
}

static u32 ui_color_alpha_mul(u32 color, f64 alpha) {
    u32 a = (color >> 24) & 0xffu;
    a = (u32)ui_clamp_f64((f64)a * alpha, 0.0, 255.0);
    return (color & 0x00ffffffu) | (a << 24);
}

static void ui_push_tri_colors(ui_renderer *r, f64 x0, f64 y0, u32 c0, f64 x1, f64 y1, u32 c1, f64 x2, f64 y2, u32 c2) {
    ui_clip clip = ui_current_clip(r);
    f64 min_x = fmin(x0, fmin(x1, x2));
    f64 min_y = fmin(y0, fmin(y1, y2));
    f64 max_x = fmax(x0, fmax(x1, x2));
    f64 max_y = fmax(y0, fmax(y1, y2));
    if (max_x <= clip.x || max_y <= clip.y || min_x >= clip.x + clip.w || min_y >= clip.y + clip.h) return;
    const f64 clip_param = ui_clip_param(r, clip);
    i32 base = r->vertex_count;
    if (!ui_push_vertex(r, x0, y0, 0, 0, c0, 0, 0, 0, clip_param) ||
        !ui_push_vertex(r, x1, y1, 0, 0, c1, 0, 0, 0, clip_param) ||
        !ui_push_vertex(r, x2, y2, 0, 0, c2, 0, 0, 0, clip_param)) {
        r->vertex_count = base;
        return;
    }
    ui_emit_command(r, base, 3, UI_KIND_IMAGE);
}

void ui_fill_circle(ui_renderer *r, f64 cx, f64 cy, f64 radius, u32 rgba, f64 feather) {
    if (!r || radius <= 0.0) return;
    r->current_texture_id = UI_WHITE_TEXTURE;

    const i32 seg = 40;
    f64 f = ui_clamp_f64(ui_resolve_feather(feather), 0.0, radius);
    f64 inner = radius - f;
    u32 transparent = ui_color_alpha_mul(rgba, 0.0);

    if (inner <= 0.0) {
        for (i32 i = 0; i < seg; i++) {
            f64 a0 = (f64)i / (f64)seg * M_PI * 2.0;
            f64 a1 = (f64)(i + 1) / (f64)seg * M_PI * 2.0;
            ui_push_tri_colors(r, cx, cy, rgba,
                               cx + cos(a0) * radius, cy + sin(a0) * radius, transparent,
                               cx + cos(a1) * radius, cy + sin(a1) * radius, transparent);
        }
        return;
    }

    for (i32 i = 0; i < seg; i++) {
        f64 a0 = (f64)i / (f64)seg * M_PI * 2.0;
        f64 a1 = (f64)(i + 1) / (f64)seg * M_PI * 2.0;
        f64 ix0 = cx + cos(a0) * inner;
        f64 iy0 = cy + sin(a0) * inner;
        f64 ix1 = cx + cos(a1) * inner;
        f64 iy1 = cy + sin(a1) * inner;
        f64 ox0 = cx + cos(a0) * radius;
        f64 oy0 = cy + sin(a0) * radius;
        f64 ox1 = cx + cos(a1) * radius;
        f64 oy1 = cy + sin(a1) * radius;

        ui_push_tri_colors(r, cx, cy, rgba, ix0, iy0, rgba, ix1, iy1, rgba);
        if (f > 0.0) {
            ui_push_tri_colors(r, ix0, iy0, rgba, ox0, oy0, transparent, ox1, oy1, transparent);
            ui_push_tri_colors(r, ix0, iy0, rgba, ox1, oy1, transparent, ix1, iy1, rgba);
        }
    }
}

void ui_fill_triangle(ui_renderer *r, f64 x0, f64 y0, f64 x1, f64 y1, f64 x2, f64 y2, u32 rgba, f64 feather) {
    if (!r) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    f64 f = ui_resolve_feather(feather);
    if (f <= 0.0) {
        ui_push_tri(r, x0, y0, x1, y1, x2, y2, 0, 0, rgba);
        return;
    }
    f64 cx = (x0 + x1 + x2) / 3.0;
    f64 cy = (y0 + y1 + y2) / 3.0;
    f64 r0 = hypot(x0 - cx, y0 - cy);
    f64 r1 = hypot(x1 - cx, y1 - cy);
    f64 r2 = hypot(x2 - cx, y2 - cy);
    f64 radius = fmax(0.000001, fmin(r0, fmin(r1, r2)));
    f64 inset = ui_clamp_f64(f / radius, 0.0, 0.9);
    f64 ix0 = x0 + (cx - x0) * inset, iy0 = y0 + (cy - y0) * inset;
    f64 ix1 = x1 + (cx - x1) * inset, iy1 = y1 + (cy - y1) * inset;
    f64 ix2 = x2 + (cx - x2) * inset, iy2 = y2 + (cy - y2) * inset;
    u32 transparent = ui_color_alpha_mul(rgba, 0.0);
    ui_push_tri_colors(r, ix0, iy0, rgba, ix1, iy1, rgba, ix2, iy2, rgba);
    ui_push_tri_colors(r, x0, y0, transparent, x1, y1, transparent, ix1, iy1, rgba);
    ui_push_tri_colors(r, x0, y0, transparent, ix1, iy1, rgba, ix0, iy0, rgba);
    ui_push_tri_colors(r, x1, y1, transparent, x2, y2, transparent, ix2, iy2, rgba);
    ui_push_tri_colors(r, x1, y1, transparent, ix2, iy2, rgba, ix1, iy1, rgba);
    ui_push_tri_colors(r, x2, y2, transparent, x0, y0, transparent, ix0, iy0, rgba);
    ui_push_tri_colors(r, x2, y2, transparent, ix0, iy0, rgba, ix2, iy2, rgba);
}

void ui_fill_triangle_colors(ui_renderer *r,
                             f64 x0, f64 y0, u32 rgba0,
                             f64 x1, f64 y1, u32 rgba1,
                             f64 x2, f64 y2, u32 rgba2) {
    if (!r) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    ui_push_tri_colors(r, x0, y0, rgba0, x1, y1, rgba1, x2, y2, rgba2);
}

void ui_fill_arc(ui_renderer *r, f64 cx, f64 cy, f64 radius, f64 thickness,
                 f64 angle_start, f64 angle_end, u32 rgba, f64 feather) {
    if (!r || radius <= 0.0 || thickness <= 0.0 || angle_start == angle_end) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    const f64 half_width = thickness * 0.5;
    const f64 half_angle = fmin(fabs(angle_end - angle_start) * 0.5, M_PI);
    const f64 mid_angle = (angle_start + angle_end) * 0.5;
    const f64 padding = fmax(1.0, ui_resolve_feather(feather) * 2.0);
    const f64 bound = radius + half_width + padding;
    const f64 x0 = cx - bound, y0 = cy - bound, x1 = cx + bound, y1 = cy + bound;
    const ui_clip clip = ui_current_clip(r);
    if (x1 <= clip.x || y1 <= clip.y || x0 >= clip.x + clip.w || y0 >= clip.y + clip.h) return;

    const f64 cosine = cos(mid_angle), sine = sin(mid_angle);
    const f64 px[4] = {x0, x1, x1, x0};
    const f64 py[4] = {y0, y0, y1, y1};
    f64 local_x[4], local_y[4];
    for (i32 i = 0; i < 4; i++) {
        const f64 dx = px[i] - cx, dy = py[i] - cy;
        local_x[i] = cosine * dx + sine * dy;
        local_y[i] = -sine * dx + cosine * dy;
    }
    const i32 order[6] = {0, 1, 2, 0, 2, 3};
    const f64 clip_param = ui_clip_param(r, clip);
    const i32 base = r->vertex_count;
    for (i32 i = 0; i < 6; i++) {
        const i32 vertex = order[i];
        if (!ui_push_vertex(r, px[vertex], py[vertex], local_x[vertex], local_y[vertex], rgba,
                            radius, half_width, half_angle, clip_param)) {
            r->vertex_count = base;
            return;
        }
    }
    ui_emit_command(r, base, 6, UI_KIND_ARC_SDF);
}

void ui_stroke_line(ui_renderer *r, f64 x0, f64 y0, f64 x1, f64 y1, f64 thickness, u32 rgba, f64 feather) {
    ns_unused(feather);
    if (!r || thickness <= 0.0) return;
    f64 dx = x1 - x0;
    f64 dy = y1 - y0;
    f64 len = sqrt(dx * dx + dy * dy);
    if (len <= 0.000001) return;
    f64 nx = -dy / len * thickness * 0.5;
    f64 ny = dx / len * thickness * 0.5;
    r->current_texture_id = UI_WHITE_TEXTURE;
    ui_push_tri(r, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, 0, 0, rgba);
    ui_push_tri(r, x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, 0, 0, rgba);
}

void ui_stroke_polyline(ui_renderer *r, f64 *points, i32 point_count, f64 thickness, u32 rgba, f64 feather) {
    if (!r || !points || point_count < 2) return;
    for (i32 i = 0; i < point_count - 1; i++) {
        ui_stroke_line(r, points[i * 2], points[i * 2 + 1], points[(i + 1) * 2], points[(i + 1) * 2 + 1], thickness, rgba, feather);
    }
}

void ui_stroke_circle(ui_renderer *r, f64 cx, f64 cy, f64 radius, f64 thickness, u32 rgba, f64 feather) {
    if (!r || radius <= 0.0 || thickness <= 0.0) return;
    r->current_texture_id = UI_WHITE_TEXTURE;

    const i32 seg = 48;
    f64 half = thickness * 0.5;
    f64 f = ui_clamp_f64(ui_resolve_feather(feather), 0.0, radius);
    f64 outer = radius + half;
    f64 outer_solid = fmax(radius, outer - f);
    f64 inner = fmax(0.0, radius - half);
    f64 inner_solid = fmin(radius, inner + f);
    u32 transparent = ui_color_alpha_mul(rgba, 0.0);

    for (i32 i = 0; i < seg; i++) {
        f64 a0 = (f64)i / (f64)seg * M_PI * 2.0;
        f64 a1 = (f64)(i + 1) / (f64)seg * M_PI * 2.0;
        f64 c0 = cos(a0), s0 = sin(a0);
        f64 c1 = cos(a1), s1 = sin(a1);

        f64 os0x = cx + c0 * outer_solid, os0y = cy + s0 * outer_solid;
        f64 os1x = cx + c1 * outer_solid, os1y = cy + s1 * outer_solid;
        f64 is0x = cx + c0 * inner_solid, is0y = cy + s0 * inner_solid;
        f64 is1x = cx + c1 * inner_solid, is1y = cy + s1 * inner_solid;
        ui_push_tri_colors(r, is0x, is0y, rgba, os0x, os0y, rgba, os1x, os1y, rgba);
        ui_push_tri_colors(r, is0x, is0y, rgba, os1x, os1y, rgba, is1x, is1y, rgba);

        if (f > 0.0) {
            f64 o0x = cx + c0 * outer, o0y = cy + s0 * outer;
            f64 o1x = cx + c1 * outer, o1y = cy + s1 * outer;
            ui_push_tri_colors(r, os0x, os0y, rgba, o0x, o0y, transparent, o1x, o1y, transparent);
            ui_push_tri_colors(r, os0x, os0y, rgba, o1x, o1y, transparent, os1x, os1y, rgba);

            if (inner > 0.0) {
                f64 i0x = cx + c0 * inner, i0y = cy + s0 * inner;
                f64 i1x = cx + c1 * inner, i1y = cy + s1 * inner;
                ui_push_tri_colors(r, i0x, i0y, transparent, is0x, is0y, rgba, is1x, is1y, rgba);
                ui_push_tri_colors(r, i0x, i0y, transparent, is1x, is1y, rgba, i1x, i1y, transparent);
            }
        }
    }
}

void ui_stroke_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 thickness, u32 rgba, f64 feather) {
    ns_unused(feather);
    if (!r || w <= 0.0 || h <= 0.0 || thickness <= 0.0) return;
    ui_fill_rect(r, x, y, w, thickness, rgba, 0.0);
    ui_fill_rect(r, x, y + h - thickness, w, thickness, rgba, 0.0);
    ui_fill_rect(r, x, y, thickness, h, rgba, 0.0);
    ui_fill_rect(r, x + w - thickness, y, thickness, h, rgba, 0.0);
}

void ui_stroke_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, f64 thickness, u32 rgba, f64 feather) {
    if (!r || w <= 0.0 || h <= 0.0 || thickness <= 0.0) return;
    const f64 f = ui_clamp_f64(ui_resolve_feather(feather), 0.0, thickness * 0.5);
    const f64 half = thickness * 0.5;
    f64 outer[4 * 9 * 2], outer_solid[4 * 9 * 2];
    f64 inner_solid[4 * 9 * 2], inner[4 * 9 * 2];
    i32 n = 0, n2 = 0;
    ui_round_rect_points(outer, &n, x - half, y - half, w + thickness, h + thickness, radius + half);
    ui_round_rect_points(outer_solid, &n2, x - half + f, y - half + f,
                         w + thickness - f * 2.0, h + thickness - f * 2.0, fmax(0.01, radius + half - f));
    ui_round_rect_points(inner_solid, &n2, x + half - f, y + half - f,
                         w - thickness + f * 2.0, h - thickness + f * 2.0, fmax(0.01, radius - half + f));
    ui_round_rect_points(inner, &n2, x + half, y + half,
                         w - thickness, h - thickness, fmax(0.01, radius - half));
    if (n < 3 || n2 != n) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    const u32 transparent = ui_color_alpha_mul(rgba, 0.0);
    ui_draw_round_ring(r, outer, outer_solid, n, transparent, rgba);
    ui_draw_round_ring(r, outer_solid, inner_solid, n, rgba, rgba);
    ui_draw_round_ring(r, inner_solid, inner, n, rgba, transparent);
}

void ui_stroke_round_rect_per_corner(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 rtl, f64 rtr, f64 rbl, f64 rbr, f64 thickness, u32 rgba, f64 feather) {
    ns_unused(rtl);
    ns_unused(rtr);
    ns_unused(rbl);
    ui_stroke_round_rect(r, x, y, w, h, rbr, thickness, rgba, feather);
}

static void ui_create_gpu_resources(ui_renderer *r) {
    u32 white = 0xffffffffu;
    r->white_texture = ui_create_rgba_texture(&white, 1, 1);

    char image_path[UI_PATH_MAX];
    io_image *img = ui_resolve_asset("latin_mono.png", image_path)
                        ? io_load_image(image_path)
                        : NULL;
    if (!img) fprintf(stderr, "ui: cannot locate or load latin_mono.png\n");
    if (img && img->data && img->channels == 4) {
        r->font_texture = ui_create_rgba_texture(img->data, img->width, img->height);
    }
    if (img) {
        free(img->data);
        free(img);
    }

    r->shader_image = gpu_shader_graphics_create(ui_shader_src, ui_shader_src, "ui_vs", "ui_fs_image");
    r->shader_msdf = gpu_shader_graphics_create(ui_shader_src, ui_shader_src, "ui_vs", "ui_fs_msdf");
    r->shader_bitmap = gpu_shader_graphics_create(ui_shader_src, ui_shader_src, "ui_vs", "ui_fs_bitmap");
    r->shader_arc_sdf = gpu_shader_graphics_create(ui_shader_src, ui_shader_src, "ui_vs", "ui_fs_arc_sdf");
    r->render_state = gpu_state_create(PRIMITIVE_TRIANGLES, CULL_NONE, FACE_WINDING_CCW,
                                       COMPARE_ALWAYS, false, GPU_BLEND_ALPHA, COLOR_MASK_ALL);
    // Reverse-Z compositor: write HUD depth so timewarp does not smear overlay
    // pixels as if they belonged to the world behind them.
    r->render_state_hud = gpu_state_create(PRIMITIVE_TRIANGLES, CULL_NONE, FACE_WINDING_CCW,
                                           COMPARE_ALWAYS, true, GPU_BLEND_ALPHA, COLOR_MASK_ALL);
    r->gpu_ready = r->white_texture && r->font_texture && r->shader_image &&
                   r->shader_msdf && r->shader_bitmap && r->shader_arc_sdf && r->render_state &&
                   r->render_state_hud;
}

static f64 ui_view_content_scale(view *v) {
    if (!v) return 1.0;
    if (v->display_ratio > 0.0) return v->display_ratio;
    if (v->ui_scale > 0.0) return v->ui_scale;
    return 1.0;
}

// Resolve the safe content space from the drawable extent and the active
// insets. Insets are dropped when they would leave nothing to draw in, so a
// bogus platform report can never collapse an application's canvas.
static void ui_resolve_safe_area(ui_renderer *r) {
    if (!r) return;
    ui_insets in = r->safe_area_enabled ? r->insets : (ui_insets){0.0, 0.0, 0.0, 0.0};
    if (in.top < 0.0) in.top = 0.0;
    if (in.right < 0.0) in.right = 0.0;
    if (in.bottom < 0.0) in.bottom = 0.0;
    if (in.left < 0.0) in.left = 0.0;
    if (in.left + in.right >= r->rect.w) in.left = in.right = 0.0;
    if (in.top + in.bottom >= r->rect.h) in.top = in.bottom = 0.0;
    r->safe_rect = (ui_rect){
        r->rect.x + in.left,
        r->rect.y + in.top,
        r->rect.w - in.left - in.right,
        r->rect.h - in.top - in.bottom,
    };
}

// The renderer works in logical points; the display scale converts to physical
// framebuffer pixels only at the GPU viewport/scissor (see ui_flush).
static void ui_sync_view_metrics(ui_renderer *r) {
    if (!r) return;
    view *v = r->v;
    r->content_scale = ui_view_content_scale(v);
    i32 lw = 0, lh = 0;
    if (v) {
        lw = v->width;
        lh = v->height;
        if (lw <= 0 && v->framebuffer_width > 0) lw = (i32)(v->framebuffer_width / r->content_scale + 0.5);
        if (lh <= 0 && v->framebuffer_height > 0) lh = (i32)(v->framebuffer_height / r->content_scale + 0.5);
        if (!r->insets_overridden) {
            r->insets = (ui_insets){v->safe_area_top, v->safe_area_right,
                                    v->safe_area_bottom, v->safe_area_left};
        }
    }
    r->rect = (ui_rect){0.0, 0.0, (f64)(lw > 0 ? lw : 1), (f64)(lh > 0 ? lh : 1)};
    ui_resolve_safe_area(r);
}

ui_renderer *ui_renderer_create(view *v) {
    ui_renderer *r = (ui_renderer*)calloc(1, sizeof(ui_renderer));
    if (!r) return NULL;
    r->handle = r;
    r->v = v;
    // Device safe areas are honoured unless an application opts out, so UI laid
    // out inside the reported canvas never lands under native chrome.
    r->safe_area_enabled = true;
    // ui owns its GPU dependency; view + ui applications need no direct gpu
    // import. Backends keep repeated requests for the same view idempotent.
    gpu_request_device(v);
    ui_sync_view_metrics(r);
    r->vertex_capacity = UI_INITIAL_VERTEX_CAP;
    r->vertices = (ui_vertex*)calloc((size_t)r->vertex_capacity, sizeof(ui_vertex));
    r->current_texture_id = UI_WHITE_TEXTURE;
    if (!r->vertices || !ui_load_fonts(r)) {
        ui_renderer_destroy(r);
        return NULL;
    }
    ui_create_gpu_resources(r);
    return r;
}

ns_bool ui_load_font(ui_renderer *r, const char *json_path, const char *image_path) {
    if (!r || !json_path || !image_path) return false;
    size_t json_len = 0;
    char *json = ui_read_file(json_path, &json_len);
    ns_unused(json_len);
    io_image *image = io_load_image(image_path);
    if (!json || !image || !image->data || image->channels != 4) {
        free(json);
        if (image) { free(image->data); free(image); }
        return false;
    }

    const i32 tex_w = (i32)ui_json_key_number(json, "width", image->width);
    const i32 tex_h = (i32)ui_json_key_number(json, "height", image->height);
    ui_font main_font = {0};
    ui_font mono_font = {0};
    ns_bool loaded = ui_load_font_face(json, "FONT_MAIN", tex_w, tex_h, &main_font) &&
                     ui_load_font_face(json, "FONT_MONO", tex_w, tex_h, &mono_font);
    free(json);
    if (!loaded) {
        free(main_font.glyphs);
        free(mono_font.glyphs);
        free(image->data);
        free(image);
        return false;
    }

    u32 texture = ui_create_rgba_texture(image->data, image->width, image->height);
    free(image->data);
    free(image);
    if (!texture) {
        free(main_font.glyphs);
        free(mono_font.glyphs);
        return false;
    }
    free(r->fonts[UI_FONT_MAIN].glyphs);
    free(r->fonts[UI_FONT_MONO].glyphs);
    if (r->font_texture) gpu_texture_destroy(r->font_texture);
    r->fonts[UI_FONT_MAIN] = main_font;
    r->fonts[UI_FONT_MONO] = mono_font;
    r->font_texture = texture;
    return true;
}

ns_bool ui_load_chinese_font(ui_renderer *r, const char *json_path, const char *image_path) {
    if (!r || !json_path || !image_path) return false;
    size_t json_len = 0;
    char *json = ui_read_file(json_path, &json_len);
    ns_unused(json_len);
    io_image *image = io_load_image(image_path);
    if (!json || !image || !image->data || image->channels != 4) {
        free(json);
        if (image) { free(image->data); free(image); }
        return false;
    }

    const i32 tex_w = (i32)ui_json_key_number(json, "width", image->width);
    const i32 tex_h = (i32)ui_json_key_number(json, "height", image->height);
    ui_font zh_font = {0};
    ns_bool loaded = ui_load_font_face(json, NULL, tex_w, tex_h, &zh_font);
    free(json);
    if (!loaded) {
        free(zh_font.glyphs);
        free(image->data);
        free(image);
        return false;
    }

    u32 texture = ui_create_rgba_texture(image->data, image->width, image->height);
    free(image->data);
    free(image);
    if (!texture) {
        free(zh_font.glyphs);
        return false;
    }
    free(r->fonts[UI_FONT_ZH].glyphs);
    if (r->font_zh_texture) gpu_texture_destroy(r->font_zh_texture);
    r->fonts[UI_FONT_ZH] = zh_font;
    r->font_zh_texture = texture;
    return true;
}

typedef ns_bool (*ui_bitmap_face_loader)(char*, i32, i32, ui_font*);

static ns_bool ui_load_bitmap_face(ui_renderer *r, const char *json_path, const char *image_path,
                                   i32 font_index, ui_bitmap_face_loader load_face) {
    if (!r || !json_path || !image_path || !load_face || !r->shader_bitmap) return false;
    size_t json_len = 0;
    char *json = ui_read_file(json_path, &json_len);
    ns_unused(json_len);
    io_image *image = io_load_image(image_path);
    if (!json || !image || !image->data) {
        free(json);
        if (image) { free(image->data); free(image); }
        return false;
    }

    ui_font font = {0};
    const ns_bool loaded = load_face(json, image->width, image->height, &font);
    free(json);
    if (!loaded) {
        free(font.glyphs);
        free(image->data);
        free(image);
        return false;
    }

    u32 texture = ui_create_font_texture(image);
    free(image->data);
    free(image);
    if (!texture) {
        free(font.glyphs);
        return false;
    }
    u32 *target_texture = font_index == UI_FONT_BITMAP_ZH
                              ? &r->font_bitmap_zh_texture
                              : &r->font_bitmap_texture;
    free(r->fonts[font_index].glyphs);
    if (*target_texture) gpu_texture_destroy(*target_texture);
    r->fonts[font_index] = font;
    *target_texture = texture;
    return true;
}

ns_bool ui_load_bitmap_font(ui_renderer *r, const char *json_path, const char *image_path) {
    return ui_load_bitmap_face(r, json_path, image_path, UI_FONT_BITMAP, ui_load_bitmap_latin_face);
}

ns_bool ui_load_bitmap_chinese_font(ui_renderer *r, const char *json_path, const char *image_path) {
    return ui_load_bitmap_face(r, json_path, image_path, UI_FONT_BITMAP_ZH, ui_load_bitmap_chinese_face);
}

ns_bool ui_load_builtin_bitmap_font(ui_renderer *r) {
    char latin_json[UI_PATH_MAX];
    char latin_image[UI_PATH_MAX];
    char chinese_json[UI_PATH_MAX];
    char chinese_image[UI_PATH_MAX];
    if (!ui_resolve_asset("bitmap_font.json", latin_json) ||
        !ui_resolve_asset("bitmap_font.png", latin_image) ||
        !ui_resolve_asset("bitmap_zh_cn.json", chinese_json) ||
        !ui_resolve_asset("bitmap_zh_cn.png", chinese_image)) {
        return false;
    }
    if (!ui_load_bitmap_font(r, latin_json, latin_image)) return false;
    return ui_load_bitmap_chinese_font(r, chinese_json, chinese_image);
}

void ui_renderer_destroy(ui_renderer *r) {
    if (!r) return;
    for (i32 i = 0; i < UI_MAX_RECT_BATCHES; i++) {
        free(r->rect_batches[i].vertices);
    }
    for (i32 i = 0; i < UI_MAX_TEXTURES; i++) {
        if (r->textures[i]) gpu_texture_destroy(r->textures[i]);
    }
    if (r->white_texture) gpu_texture_destroy(r->white_texture);
    if (r->font_texture) gpu_texture_destroy(r->font_texture);
    if (r->font_zh_texture) gpu_texture_destroy(r->font_zh_texture);
    if (r->font_bitmap_texture) gpu_texture_destroy(r->font_bitmap_texture);
    if (r->font_bitmap_zh_texture) gpu_texture_destroy(r->font_bitmap_zh_texture);
    if (r->shader_image) gpu_shader_destroy(r->shader_image);
    if (r->shader_msdf) gpu_shader_destroy(r->shader_msdf);
    if (r->shader_bitmap) gpu_shader_destroy(r->shader_bitmap);
    if (r->shader_arc_sdf) gpu_shader_destroy(r->shader_arc_sdf);
    if (r->storage) gpu_free(r->storage);
    for (i32 i = 0; i < UI_FONT_COUNT; i++) free(r->fonts[i].glyphs);
    free(r->vertices);
    free(r);
}

static ui_rect_batch *ui_rect_batch_get(ui_renderer *r, i32 batch_id) {
    if (!r || batch_id <= 0 || batch_id > UI_MAX_RECT_BATCHES) return NULL;
    ui_rect_batch *batch = &r->rect_batches[batch_id - 1];
    return batch->used ? batch : NULL;
}

i32 ui_rect_batch_create(ui_renderer *r) {
    if (!r) return 0;
    for (i32 i = 0; i < UI_MAX_RECT_BATCHES; i++) {
        ui_rect_batch *batch = &r->rect_batches[i];
        if (batch->used) continue;
        memset(batch, 0, sizeof(*batch));
        batch->used = true;
        return i + 1;
    }
    return 0;
}

void ui_rect_batch_destroy(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch) return;
    free(batch->vertices);
    memset(batch, 0, sizeof(*batch));
}

void ui_rect_batch_begin(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch) return;
    batch->vertex_count = 0;
}

static ns_bool ui_rect_batch_reserve(ui_rect_batch *batch, i32 additional) {
    if (!batch || additional <= 0) return false;
    i32 required = batch->vertex_count + additional;
    if (required <= batch->vertex_capacity) return true;
    i32 capacity = batch->vertex_capacity > 0 ? batch->vertex_capacity : 4096;
    while (capacity < required) {
        if (capacity > 1073741823) return false;
        capacity *= 2;
    }
    ui_vertex *vertices = (ui_vertex*)realloc(batch->vertices, (size_t)capacity * sizeof(ui_vertex));
    if (!vertices) return false;
    batch->vertices = vertices;
    batch->vertex_capacity = capacity;
    return true;
}

void ui_rect_batch_add(ui_renderer *r, i32 batch_id, f64 x, f64 y, f64 w, f64 h, u32 rgba) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch || w <= 0.0 || h <= 0.0 || !ui_rect_batch_reserve(batch, 6)) return;
    f32 x0 = (f32)x, y0 = (f32)y, x1 = (f32)(x + w), y1 = (f32)(y + h);
    ui_vertex quad[6] = {
        {.x = x0, .y = y0, .color = rgba},
        {.x = x1, .y = y0, .color = rgba},
        {.x = x1, .y = y1, .color = rgba},
        {.x = x0, .y = y0, .color = rgba},
        {.x = x1, .y = y1, .color = rgba},
        {.x = x0, .y = y1, .color = rgba},
    };
    memcpy(batch->vertices + batch->vertex_count, quad, sizeof(quad));
    batch->vertex_count += 6;
}

ns_bool ui_rect_batch_end(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    return batch != NULL;
}

void ui_rect_batch_draw_at(ui_renderer *r, i32 batch_id, f64 dx, f64 dy) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch || batch->vertex_count <= 0 || r->command_count >= UI_MAX_COMMANDS) return;
    ui_clip c = ui_current_clip(r);
    if (c.w <= 0.0 || c.h <= 0.0) return;
    ui_command *cmd = &r->commands[r->command_count++];
    *cmd = (ui_command){
        .vertex_offset = 0,
        .vertex_count = batch->vertex_count,
        .texture_id = UI_WHITE_TEXTURE,
        .kind = UI_KIND_IMAGE,
        .rect_batch_id = batch_id,
        // Batch vertices are recorded once in content space and translated at
        // draw time, so the safe-area origin rides along with the draw offset.
        .offset_x = dx + r->safe_rect.x,
        .offset_y = dy + r->safe_rect.y,
        .clip_x = (i32)floor(c.x + r->safe_rect.x),
        .clip_y = (i32)floor(c.y + r->safe_rect.y),
        .clip_w = (i32)ceil(c.w),
        .clip_h = (i32)ceil(c.h),
    };
}

void ui_rect_batch_draw(ui_renderer *r, i32 batch_id) {
    ui_rect_batch_draw_at(r, batch_id, 0.0, 0.0);
}

static i32 ui_register_rgba_texture(ui_renderer *r, const u8 *data, i32 width, i32 height) {
    if (!r || !data || width <= 0 || height <= 0) return 0;
    for (i32 slot = 0; slot < UI_MAX_TEXTURES; slot++) {
        if (r->textures[slot]) continue;
        u32 texture = ui_create_rgba_texture(data, width, height);
        if (!texture) return 0;
        r->textures[slot] = texture;
        r->texture_widths[slot] = width;
        r->texture_heights[slot] = height;
        return slot + 3;
    }
    return 0;
}

i32 ui_atlas_load(ui_renderer *r, const char *path) {
    if (!r || !path || !path[0]) return 0;
    io_image *image = io_load_image(path);
    if (!image || !image->data || image->width <= 0 || image->height <= 0) return 0;
    size_t pixels = (size_t)image->width * (size_t)image->height;
    u8 *rgba = (u8*)malloc(pixels * 4);
    if (!rgba) {
        free(image->data);
        free(image);
        return 0;
    }
    for (size_t i = 0; i < pixels; i++) {
        const i32 c = image->channels;
        rgba[i * 4 + 0] = image->data[i * c + 0];
        rgba[i * 4 + 1] = c > 1 ? image->data[i * c + 1] : image->data[i * c + 0];
        rgba[i * 4 + 2] = c > 2 ? image->data[i * c + 2] : image->data[i * c + 0];
        rgba[i * 4 + 3] = c > 3 ? image->data[i * c + 3] : 255;
    }
    i32 texture_id = ui_register_rgba_texture(r, rgba, image->width, image->height);
    free(rgba);
    free(image->data);
    free(image);
    return texture_id;
}

void ui_atlas_destroy(ui_renderer *r, i32 atlas) {
    if (!r || atlas < 3 || atlas >= UI_MAX_TEXTURES + 3) return;
    i32 slot = atlas - 3;
    if (r->textures[slot]) gpu_texture_destroy(r->textures[slot]);
    r->textures[slot] = 0;
    r->texture_widths[slot] = 0;
    r->texture_heights[slot] = 0;
}

i32 ui_atlas_width(ui_renderer *r, i32 atlas) {
    return r && atlas >= 3 && atlas < UI_MAX_TEXTURES + 3 ? r->texture_widths[atlas - 3] : 0;
}
i32 ui_atlas_height(ui_renderer *r, i32 atlas) {
    return r && atlas >= 3 && atlas < UI_MAX_TEXTURES + 3 ? r->texture_heights[atlas - 3] : 0;
}

void ui_atlas_draw_region(ui_renderer *r, i32 atlas, f64 x, f64 y, f64 w, f64 h,
                          f64 atlas_x, f64 atlas_y, f64 atlas_w, f64 atlas_h, u32 rgba) {
    i32 width = ui_atlas_width(r, atlas);
    i32 height = ui_atlas_height(r, atlas);
    if (!r || width <= 0 || height <= 0 || w <= 0 || h <= 0) return;
    r->current_texture_id = atlas;
    ui_push_quad_ex(r, x, y, x + w, y + h,
                    atlas_x / width, atlas_y / height,
                    (atlas_x + atlas_w) / width, (atlas_y + atlas_h) / height,
                    rgba, UI_KIND_IMAGE, 0, 0, 0);
}

void ui_atlas_draw(ui_renderer *r, i32 atlas, f64 x, f64 y, f64 w, f64 h) {
    ui_atlas_draw_region(r, atlas, x, y, w, h, 0, 0, ui_atlas_width(r, atlas), ui_atlas_height(r, atlas), 0xffffffffu);
}

void ui_resize(ui_renderer *r) {
    if (!r) return;
    ui_sync_view_metrics(r);
}

void ui_resize_to(ui_renderer *r, i32 width, i32 height) {
    if (!r) return;
    r->rect = (ui_rect){0.0, 0.0, (f64)(width > 0 ? width : 1), (f64)(height > 0 ? height : 1)};
    ui_resolve_safe_area(r);
}

void ui_request_render(ui_renderer *r, i32 frames) {
    if (!r) return;
    view_request_frame(r->v, frames);
}

void ui_request_render_after(ui_renderer *r, i32 milliseconds) {
    if (!r) return;
    view_request_frame_after(r->v, milliseconds);
}

void ui_begin_frame(ui_renderer *r) {
    if (!r) return;
    r->vertex_count = 0;
    r->command_count = 0;
    r->clip_count = 1;
    r->gpu_clip_count = 0;
    r->clips[0] = ui_surface_clip(r);
    r->current_texture_id = UI_WHITE_TEXTURE;
}

static u64 ui_align_u64(u64 value, u64 alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static ns_bool ui_upload_storage(ui_renderer *r, u32 *clip_offset) {
    if (!r || !clip_offset) return false;
    u64 cursor = (u64)(u32)r->vertex_count * UI_VERTEX_STRIDE;
    cursor = ui_align_u64(cursor, 16);
    *clip_offset = (u32)cursor;
    const u64 clip_size = r->gpu_clip_count > 0
                              ? (u64)(u32)r->gpu_clip_count * sizeof(ui_gpu_clip)
                              : sizeof(ui_gpu_clip);
    cursor = ui_align_u64(cursor + clip_size, 16);
    for (i32 i = 0; i < UI_MAX_RECT_BATCHES; i++) {
        ui_rect_batch *batch = &r->rect_batches[i];
        batch->gpu_offset = cursor;
        if (batch->used && batch->vertex_count > 0) {
            cursor = ui_align_u64(cursor + (u64)(u32)batch->vertex_count * UI_VERTEX_STRIDE, 16);
        }
    }
    const u64 required = cursor > 0 ? cursor : 16;
    if (!r->storage || required > r->storage_capacity) {
        u64 capacity = r->storage_capacity > 0 ? r->storage_capacity : 4096;
        while (capacity < required) capacity *= 2;
        gpu_addr storage = gpu_malloc(capacity, GPU_MEM_SHARED, "ui renderer storage");
        if (!storage) return false;
        if (r->storage) gpu_free(r->storage);
        r->storage = storage;
        r->storage_capacity = capacity;
    }
    if (r->vertex_count > 0) {
        gpu_write(r->storage, r->vertices, (u64)(u32)r->vertex_count * UI_VERTEX_STRIDE);
    }
    ui_gpu_clip empty_clip = {0};
    const void *clips = r->gpu_clip_count > 0 ? (const void *)r->gpu_clips : (const void *)&empty_clip;
    gpu_write(r->storage + *clip_offset, clips, clip_size);
    for (i32 i = 0; i < UI_MAX_RECT_BATCHES; i++) {
        ui_rect_batch *batch = &r->rect_batches[i];
        if (batch->used && batch->vertex_count > 0) {
            gpu_write(r->storage + batch->gpu_offset, batch->vertices,
                      (u64)(u32)batch->vertex_count * UI_VERTEX_STRIDE);
        }
    }
    return true;
}

static u32 ui_command_texture(ui_renderer *r, const ui_command *cmd) {
    if (cmd->rect_batch_id > 0 || cmd->kind == UI_KIND_ARC_SDF) return r->white_texture;
    if (cmd->kind == UI_KIND_MSDF) {
        return cmd->texture_id == UI_FONT_ZH_TEXTURE && r->font_zh_texture
                   ? r->font_zh_texture
                   : r->font_texture;
    }
    if (cmd->kind == UI_KIND_BITMAP) {
        return cmd->texture_id == UI_FONT_BITMAP_ZH_TEXTURE
                   ? r->font_bitmap_zh_texture
                   : r->font_bitmap_texture;
    }
    if (cmd->texture_id >= 3 && cmd->texture_id < UI_MAX_TEXTURES + 3) {
        u32 texture = r->textures[cmd->texture_id - 3];
        if (texture) return texture;
    }
    return r->white_texture;
}

static f32 ui_dot3(const f32 a[3], const f32 b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static ns_bool ui_fill_hud_root(ui_gpu_root *root) {
    if (!root || view_immersive_status() != 2 || view_immersive_eye() < 0) return false;
    f32 eye_right[3] = { (f32)view_immersive_value(0), (f32)view_immersive_value(1), (f32)view_immersive_value(2) };
    f32 eye_up[3] = { (f32)view_immersive_value(4), (f32)view_immersive_value(5), (f32)view_immersive_value(6) };
    f32 eye_forward[3] = { (f32)-view_immersive_value(8), (f32)-view_immersive_value(9), (f32)-view_immersive_value(10) };
    f32 eye_pos[3] = { (f32)view_immersive_value(12), (f32)view_immersive_value(13), (f32)view_immersive_value(14) };
    f32 head_right[3] = { (f32)view_immersive_value(32), (f32)view_immersive_value(33), (f32)view_immersive_value(34) };
    f32 head_up[3] = { (f32)view_immersive_value(36), (f32)view_immersive_value(37), (f32)view_immersive_value(38) };
    f32 head_forward[3] = { (f32)-view_immersive_value(40), (f32)-view_immersive_value(41), (f32)-view_immersive_value(42) };
    f32 head_pos[3] = { (f32)view_immersive_value(44), (f32)view_immersive_value(45), (f32)view_immersive_value(46) };
    f32 proj_x = (f32)view_immersive_value(16);
    f32 proj_y = (f32)view_immersive_value(21);
    f32 proj_zx = (f32)view_immersive_value(24);
    f32 proj_zy = (f32)view_immersive_value(25);
    if (fabsf(proj_x) < 1e-5f || fabsf(proj_y) < 1e-5f) return false;
    if (ui_dot3(eye_forward, eye_forward) < 1e-6f || ui_dot3(head_forward, head_forward) < 1e-6f) return false;
    f32 distance = UI_HUD_DISTANCE_METRES;
    f32 center_world[3] = {
        head_pos[0] + head_forward[0] * distance,
        head_pos[1] + head_forward[1] * distance,
        head_pos[2] + head_forward[2] * distance
    };
    f32 rel[3] = { center_world[0] - eye_pos[0], center_world[1] - eye_pos[1], center_world[2] - eye_pos[2] };
    root->hud_center_enable[0] = ui_dot3(rel, eye_right);
    root->hud_center_enable[1] = ui_dot3(rel, eye_up);
    root->hud_center_enable[2] = ui_dot3(rel, eye_forward);
    root->hud_center_enable[3] = 1.0f;
    root->hud_right_hw[0] = ui_dot3(head_right, eye_right);
    root->hud_right_hw[1] = ui_dot3(head_right, eye_up);
    root->hud_right_hw[2] = ui_dot3(head_right, eye_forward);
    root->hud_right_hw[3] = fabsf(distance / proj_x);
    root->hud_up_hh[0] = ui_dot3(head_up, eye_right);
    root->hud_up_hh[1] = ui_dot3(head_up, eye_up);
    root->hud_up_hh[2] = ui_dot3(head_up, eye_forward);
    root->hud_up_hh[3] = fabsf(distance / proj_y);
    if (root->hud_right_hw[3] < 1e-4f || root->hud_up_hh[3] < 1e-4f) {
        root->hud_center_enable[3] = 0.0f;
        return false;
    }
    root->hud_proj[0] = proj_x;
    root->hud_proj[1] = proj_y;
    root->hud_proj[2] = proj_zx;
    root->hud_proj[3] = proj_zy;
    root->hud_depth[0] = (f32)view_immersive_value(26);
    root->hud_depth[1] = (f32)view_immersive_value(30);
    return true;
}

void ui_flush(ui_renderer *r, ui_color_rgba *clear) {
    if (!r || !r->gpu_ready) return;
    u32 clip_offset = 0;
    if (!ui_upload_storage(r, &clip_offset)) return;
    const f64 fallback_scale = r->content_scale > 0.0 ? r->content_scale : 1.0;
    const i32 framebuffer_width = r->v && r->v->framebuffer_width > 0
                                      ? r->v->framebuffer_width
                                      : (i32)(r->rect.w * fallback_scale + 0.5);
    const i32 framebuffer_height = r->v && r->v->framebuffer_height > 0
                                       ? r->v->framebuffer_height
                                       : (i32)(r->rect.h * fallback_scale + 0.5);
    const f64 sx = (f64)framebuffer_width / r->rect.w;
    const f64 sy = (f64)framebuffer_height / r->rect.h;
    // ui_color_rgba is passed by value at the ns surface and the native FFI
    // adapter retains the historical opaque pointer ABI. The old renderer did
    // not dereference it either; keep that ABI and use the established default.
    ns_unused(clear);
    gpu_screen_pass_begin("ui", 0.0, 0.0, 0.0, 1.0);
    gpu_set_viewport(0, 0, framebuffer_width, framebuffer_height);
    ui_gpu_root hud_probe = {0};
    ns_bool hud = ui_fill_hud_root(&hud_probe);
    gpu_set_state(hud ? r->render_state_hud : r->render_state);
    gpu_set_storage(r->storage);
    for (i32 i = 0; i < r->command_count; i++) {
        ui_command *cmd = &r->commands[i];
        if (cmd->clip_w <= 0 || cmd->clip_h <= 0) continue;
        i32 x0 = (i32)floor(cmd->clip_x * sx);
        i32 y0 = (i32)floor(cmd->clip_y * sy);
        i32 x1 = (i32)ceil((cmd->clip_x + cmd->clip_w) * sx);
        i32 y1 = (i32)ceil((cmd->clip_y + cmd->clip_h) * sy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > framebuffer_width) x1 = framebuffer_width;
        if (y1 > framebuffer_height) y1 = framebuffer_height;
        if (x1 <= x0 || y1 <= y0) continue;
        // 3D HUD projection moves triangles off the 2D clip rect; pixel clips
        // still run in the fragment shader from canvas coordinates.
        if (hud) gpu_set_scissor(0, 0, framebuffer_width, framebuffer_height);
        else gpu_set_scissor(x0, y0, x1 - x0, y1 - y0);
        ui_rect_batch *batch = NULL;
        u32 shader = r->shader_image;
        if (cmd->rect_batch_id > 0) {
            batch = ui_rect_batch_get(r, cmd->rect_batch_id);
            if (!batch || batch->vertex_count <= 0) continue;
        } else if (cmd->kind == UI_KIND_ARC_SDF) {
            shader = r->shader_arc_sdf;
        } else if (cmd->kind == UI_KIND_MSDF) {
            shader = r->shader_msdf;
        } else if (cmd->kind == UI_KIND_BITMAP) {
            shader = r->shader_bitmap;
        }
        u32 texture = ui_command_texture(r, cmd);
        if (!texture) continue;
        ui_gpu_root root = {
            .texture_id = (f32)texture,
            .screen_width = (f32)r->rect.w,
            .screen_height = (f32)r->rect.h,
            .offset_x = (f32)cmd->offset_x,
            .offset_y = (f32)cmd->offset_y,
            .vertex_offset = batch ? (u32)batch->gpu_offset : 0,
            .clip_offset = clip_offset,
        };
        ui_fill_hud_root(&root);
        gpu_set_shader(shader);
        gpu_set_root_data(&root, sizeof(root));
        gpu_draw_vertices(batch ? 0 : cmd->vertex_offset, cmd->vertex_count, 1);
    }
    gpu_pass_end();
    gpu_commit();
    r->vertex_count = 0;
    r->command_count = 0;
}

// The canvas is the safe content area: laying out inside it keeps an
// application clear of the notch, the status bar and the home indicator.
i32 ui_canvas_width(ui_renderer *r) {
    return r ? (i32)(r->safe_rect.w + 0.5) : 0;
}

i32 ui_canvas_height(ui_renderer *r) {
    return r ? (i32)(r->safe_rect.h + 0.5) : 0;
}

// The full drawable, insets included. Use it for full-bleed backgrounds.
i32 ui_surface_width(ui_renderer *r) {
    return r ? (i32)(r->rect.w + 0.5) : 0;
}

i32 ui_surface_height(ui_renderer *r) {
    return r ? (i32)(r->rect.h + 0.5) : 0;
}

// The same two rectangles as whole rects, in drawing coordinates, for layout
// that takes a rect rather than a width and a height. The safe rect starts at
// the origin; the drawable rect starts at negative coordinates by exactly the
// insets, so a panel laid out in it covers the chrome as well.
ui_rect *ui_safe_rect(ui_renderer *r) {
    ui_rect *out = (ui_rect *)ns_malloc(sizeof(ui_rect));
    if (!out) return NULL;
    *out = r ? (ui_rect){0.0, 0.0, r->safe_rect.w, r->safe_rect.h}
             : (ui_rect){0.0, 0.0, 0.0, 0.0};
    return out;
}

ui_rect *ui_surface_rect(ui_renderer *r) {
    ui_rect *out = (ui_rect *)ns_malloc(sizeof(ui_rect));
    if (!out) return NULL;
    if (!r) {
        *out = (ui_rect){0.0, 0.0, 0.0, 0.0};
        return out;
    }
    const ui_clip c = ui_surface_clip(r);
    *out = (ui_rect){c.x, c.y, c.w, c.h};
    return out;
}

// Insets currently applied: the device values, an application override, or
// zeroes while the safe area is switched off.
ui_insets *ui_safe_area(ui_renderer *r) {
    ui_insets *out = (ui_insets *)ns_malloc(sizeof(ui_insets));
    if (!out) return NULL;
    if (!r) {
        *out = (ui_insets){0.0, 0.0, 0.0, 0.0};
        return out;
    }
    *out = (ui_insets){
        .top = r->safe_rect.y - r->rect.y,
        .right = (r->rect.x + r->rect.w) - (r->safe_rect.x + r->safe_rect.w),
        .bottom = (r->rect.y + r->rect.h) - (r->safe_rect.y + r->safe_rect.h),
        .left = r->safe_rect.x - r->rect.x,
    };
    return out;
}

ns_bool ui_safe_area_enabled(ui_renderer *r) {
    return r ? r->safe_area_enabled : false;
}

// Opt out when the application draws its own full-screen chrome and takes
// responsibility for keeping controls clear of the device's.
void ui_set_safe_area_enabled(ui_renderer *r, ns_bool enabled) {
    if (!r) return;
    r->safe_area_enabled = enabled;
    ui_resolve_safe_area(r);
}

// Replace the device insets, e.g. to reserve room for an application title bar
// or to test a device layout on the desktop.
void ui_set_safe_area_insets(ui_renderer *r, f64 top, f64 right, f64 bottom, f64 left) {
    if (!r) return;
    r->insets = (ui_insets){top, right, bottom, left};
    r->insets_overridden = true;
    ui_resolve_safe_area(r);
}

// Drop an override and follow the view again.
void ui_reset_safe_area_insets(ui_renderer *r) {
    if (!r) return;
    r->insets_overridden = false;
    ui_sync_view_metrics(r);
}

// Drawable point -> content point. View input (view.mouse_x, pointer events)
// arrives in drawable space; widgets hit-test in content space.
f64 ui_content_x(ui_renderer *r, f64 x) { return r ? x - r->safe_rect.x : x; }
f64 ui_content_y(ui_renderer *r, f64 y) { return r ? y - r->safe_rect.y : y; }

// Content point -> drawable point.
f64 ui_surface_x(ui_renderer *r, f64 x) { return r ? x + r->safe_rect.x : x; }
f64 ui_surface_y(ui_renderer *r, f64 y) { return r ? y + r->safe_rect.y : y; }

// Fill the whole drawable, insets included, ignoring the current clip. The
// background of a full-screen application reaches under the native chrome
// while its controls stay inside the safe area.
void ui_fill_surface(ui_renderer *r, u32 rgba) {
    if (!r) return;
    const ui_clip surface = ui_surface_clip(r);
    const i32 clip_count = r->clip_count;
    r->clip_count = 1;
    r->clips[0] = surface;
    ui_fill_rect(r, surface.x, surface.y, surface.w, surface.h, rgba, 0.0);
    r->clip_count = clip_count;
}

ui_rect *ui_layout(f64 x, f64 y, f64 w, f64 h, f64 child_w, f64 child_h, i32 align) {
    ui_rect *rect = (ui_rect *)ns_malloc(sizeof(ui_rect));
    if (!rect) return NULL;
    rect->x = x;
    rect->y = y;
    rect->w = child_w;
    rect->h = child_h;

    if (align & UI_ALIGN_CENTER_HORIZONTAL) {
        rect->x = x + (w - child_w) * 0.5;
    } else if (align & UI_ALIGN_RIGHT) {
        rect->x = x + w - child_w;
    }

    if (align & UI_ALIGN_CENTER_VERTICAL) {
        rect->y = y + (h - child_h) * 0.5;
    } else if (align & UI_ALIGN_BOTTOM) {
        rect->y = y + h - child_h;
    }

    return rect;
}

void ui_push_clip(ui_renderer *r, f64 x, f64 y, f64 w, f64 h) {
    if (!r || r->clip_count >= UI_MAX_CLIPS) return;
    ui_clip a = ui_current_clip(r);
    f64 x0 = fmax(a.x, x);
    f64 y0 = fmax(a.y, y);
    f64 x1 = fmin(a.x + a.w, x + w);
    f64 y1 = fmin(a.y + a.h, y + h);
    r->clips[r->clip_count++] = (ui_clip){x0, y0, fmax(0, x1 - x0), fmax(0, y1 - y0)};
}

void ui_push_clip_round(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius) {
    ns_unused(radius);
    ui_push_clip(r, x, y, w, h);
}

void ui_pop_clip(ui_renderer *r) {
    if (r && r->clip_count > 1) r->clip_count--;
}

void ui_flush_overlay(ui_renderer *r, ui_color_rgba *clear) {
    ui_flush(r, clear);
}

static u32 ui_pack_rgba01(f64 r, f64 g, f64 b, f64 a) {
    r = ui_clamp_f64(r, 0.0, 1.0); g = ui_clamp_f64(g, 0.0, 1.0);
    b = ui_clamp_f64(b, 0.0, 1.0); a = ui_clamp_f64(a, 0.0, 1.0);
    return ((u32)(a * 255.0 + 0.5) << 24) | ((u32)(b * 255.0 + 0.5) << 16) |
           ((u32)(g * 255.0 + 0.5) << 8) | (u32)(r * 255.0 + 0.5);
}

static u32 ui_widget_hash(const char *text) {
    u32 h = 2166136261u;
    if (!text) return h;
    while (*text) { h ^= (u8)*text++; h *= 16777619u; }
    return h ? h : 1u;
}

static ns_bool ui_widget_hover(ui_widgets *w, f64 x, f64 y, f64 width, f64 height) {
    return w && w->input.mouse_x >= x && w->input.mouse_y >= y && w->input.mouse_x < x + width && w->input.mouse_y < y + height;
}

ui_widgets *ui_widgets_create(ui_renderer *r) {
    if (!r) return NULL;
    ui_widgets *w = (ui_widgets *)calloc(1, sizeof(ui_widgets));
    if (w) { w->handle = w; w->renderer = r; }
    return w;
}

void ui_widgets_destroy(ui_widgets *w) { free(w); }

void ui_widgets_set_light(ui_widgets *w, ns_bool enabled) { if (w) w->light = enabled; }

void ui_widgets_begin_frame(ui_widgets *w, ui_theme *theme, ui_input *input) {
    ns_unused(theme);
    if (!w || !input) return;
    w->input = *input;
    // Pointer positions are collected from the view in drawable space; widgets
    // are laid out in the safe content space.
    w->input.mouse_x = ui_content_x(w->renderer, w->input.mouse_x);
    w->input.mouse_y = ui_content_y(w->renderer, w->input.mouse_y);
}

void ui_widgets_begin_view(ui_widgets *w, ui_theme *theme, view *v, ns_bool gizmo_manipulating) {
    ns_unused(theme);
    if (!w || !v) return;
    memset(&w->input, 0, sizeof(w->input));
    w->input.mouse_x = ui_content_x(w->renderer, v->mouse_x);
    w->input.mouse_y = ui_content_y(w->renderer, v->mouse_y);
    w->input.mouse_down = v->mouse_down;
    w->input.mouse_pressed = v->mouse_pressed;
    w->input.mouse_released = v->mouse_released;
    w->input.mouse_middle_down = v->mouse_middle_down;
    w->input.mouse_right_pressed = v->mouse_right_pressed;
    w->input.mouse_right_down = v->mouse_right_down;
    w->input.zoom_factor = 1.0;
    w->input.wheel_y = v->scroll_y;
    w->input.gizmo_manipulating = gizmo_manipulating;
}

void ui_widgets_end_frame(ui_widgets *w) { ns_unused(w); }

ns_bool ui_button(ui_widgets *w, const char *id, f64 x, f64 y, f64 width, f64 height, const char *label, ns_bool active) {
    if (!w || !w->renderer) return false;
    ns_bool hover = ui_widget_hover(w, x, y, width, height);
    u32 bg = w->light ? (active ? 0xffffe4dbu : (hover ? 0xfffff5e7u : 0xfffaf9f8u))
                      : (active ? 0xff4b805fu : (hover ? 0xff343b45u : 0xff262c34u));
    u32 border = w->light ? (active ? 0xfff56e4cu : 0xffe6e2deu)
                          : (active ? 0xff61d394u : 0xff48515du);
    u32 text_color = w->light ? 0xff292521u : 0xffedf2f7u;
    ui_fill_round_rect(w->renderer, x, y, width, height, 7.0, bg, 0.0);
    ui_stroke_round_rect(w->renderer, x, y, width, height, 7.0, 1.0, border, 0.0);
    if (label) ui_draw_text(w->renderer, x + 9.0, y + (height - 13.0) * 0.5, label, 13.0, text_color, UI_FONT_MAIN);
    u32 hash = ui_widget_hash(id);
    if (hover && w->input.mouse_pressed) w->active_id = hash;
    ns_bool clicked = hover && w->input.mouse_released && w->active_id == hash;
    // Only the button the press belongs to lets go of it, the way the slider
    // and the colour picker below do. Releasing it for whichever button was
    // drawn first would take it away from every later one in the same frame,
    // so only the first button of a panel could ever be clicked.
    if (w->input.mouse_released && w->active_id == hash) w->active_id = 0;
    return clicked;
}

f64 ui_slider(ui_widgets *w, const char *id, f64 x, f64 y, f64 width, f64 height,
              f64 value, f64 min, f64 max, ns_bool show_value) {
    ns_unused(show_value);
    if (!w || !w->renderer || max <= min) return value;
    ns_bool hover = ui_widget_hover(w, x, y, width, height);
    u32 hash = ui_widget_hash(id);
    if (hover && w->input.mouse_pressed) w->active_id = hash;
    if (w->active_id == hash && w->input.mouse_down) value = min + ui_clamp_f64((w->input.mouse_x - x) / width, 0.0, 1.0) * (max - min);
    if (w->input.mouse_released && w->active_id == hash) w->active_id = 0;
    f64 t = ui_clamp_f64((value - min) / (max - min), 0.0, 1.0);
    ui_fill_round_rect(w->renderer, x, y + height * 0.4, width, height * 0.2, height * 0.1, 0xff414a55u, 0.0);
    ui_fill_round_rect(w->renderer, x, y + height * 0.4, width * t, height * 0.2, height * 0.1, 0xff61d394u, 0.0);
    ui_fill_circle(w->renderer, x + width * t, y + height * 0.5, height * 0.28, 0xffedf2f7u, 0.0);
    return value;
}

f64 ui_slider_rect(ui_widgets *w, const char *id, ui_rect *rect, f64 value, f64 min, f64 max) {
    if (!rect) return value;
    return ui_slider(w, id, rect->x, rect->y, rect->w, rect->h, value, min, max, false);
}

f64 ui_slider_id(ui_widgets *w, i32 id, ui_rect *rect, f64 value, f64 min, f64 max) {
    char name[32];
    snprintf(name, sizeof(name), "slider-%d", id);
    return ui_slider_rect(w, name, rect, value, min, max);
}

ui_color_rgba *ui_color_picker(ui_widgets *w, const char *id, f64 x, f64 y, f64 width, f64 height, ui_color_rgba *value) {
    static ui_color_rgba result;
    result = value ? *value : (ui_color_rgba){1.0, 1.0, 1.0, 1.0};
    if (!w || !w->renderer) return &result;
    u32 hash = ui_widget_hash(id);
    ns_bool hover = ui_widget_hover(w, x, y, width, height);
    if (hover && w->input.mouse_pressed) w->active_id = hash;
    if (w->active_id == hash && w->input.mouse_down) {
        result.r = ui_clamp_f64((w->input.mouse_x - x) / width, 0.0, 1.0);
        result.g = ui_clamp_f64(1.0 - (w->input.mouse_y - y) / height, 0.0, 1.0);
        result.b = ui_clamp_f64(1.0 - fabs(result.r - result.g), 0.0, 1.0);
    }
    if (w->input.mouse_released && w->active_id == hash) w->active_id = 0;
    const i32 cells = 12;
    for (i32 iy = 0; iy < cells; iy++) for (i32 ix = 0; ix < cells; ix++) {
        f64 rr = (f64)ix / (cells - 1), gg = 1.0 - (f64)iy / (cells - 1);
        ui_fill_rect(w->renderer, x + width * ix / cells, y + height * iy / cells, width / cells + 1.0, height / cells + 1.0,
                     ui_pack_rgba01(rr, gg, 1.0 - fabs(rr - gg), 1.0), 0.0);
    }
    ui_stroke_circle(w->renderer, x + result.r * width, y + (1.0 - result.g) * height, 5.0, 2.0, 0xffffffffu, 0.0);
    return &result;
}

ui_color_rgba *ui_color_picker_rect(ui_widgets *w, const char *id, ui_rect *rect, ui_color_rgba *value) {
    static ui_color_rgba fallback;
    if (!rect) {
        fallback = value ? *value : (ui_color_rgba){1.0, 1.0, 1.0, 1.0};
        return &fallback;
    }
    return ui_color_picker(w, id, rect->x, rect->y, rect->w, rect->h, value);
}

ui_color_rgba *ui_color_picker_id(ui_widgets *w, i32 id, ui_rect *rect, ui_color_rgba *value) {
    char name[32];
    snprintf(name, sizeof(name), "color-%d", id);
    return ui_color_picker_rect(w, name, rect, value);
}

ui_hit *ui_hit_region(ui_widgets *w, f64 x, f64 y, f64 width, f64 height) {
    static ui_hit hit;
    hit.hovered = ui_widget_hover(w, x, y, width, height);
    hit.pressed = hit.hovered && w && w->input.mouse_pressed;
    return &hit;
}

ns_bool ui_is_mouse_down(ui_widgets *w) { return w ? w->input.mouse_down : false; }
ns_bool ui_is_mouse_pressed(ui_widgets *w) { return w ? w->input.mouse_pressed : false; }
ns_bool ui_is_escape_pressed(ui_widgets *w) { return w ? w->input.key_escape : false; }
ns_bool ui_is_enter_pressed(ui_widgets *w) { return w ? w->input.key_enter : false; }
ns_bool ui_has_keyboard_focus(ui_widgets *w) { ns_unused(w); return false; }
f64 ui_widgets_mouse_x(ui_widgets *w) { return w ? w->input.mouse_x : 0.0; }
f64 ui_widgets_mouse_y(ui_widgets *w) { return w ? w->input.mouse_y : 0.0; }

ns_bool ui_rect_clipped(ui_renderer *r, f64 x, f64 y, f64 w, f64 h) {
    if (!r) return true;
    ui_clip c = ui_current_clip(r);
    return x + w <= c.x || y + h <= c.y || x >= c.x + c.w || y >= c.y + c.h;
}

void ui_fill_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, u32 rgba, f64 feather) {
    feather = ui_resolve_feather(feather);
    ns_unused(feather);
    if (!r || w <= 0 || h <= 0) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    ui_push_quad_ex(r, x, y, x + w, y + h, 0, 0, 0, 0, rgba, UI_KIND_IMAGE, 0, 0, 0);
}

void ui_fill_gradient_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h,
                           u32 rgba_top_left, u32 rgba_top_right,
                           u32 rgba_bottom_right, u32 rgba_bottom_left) {
    if (!r || w <= 0 || h <= 0) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    ui_push_tri_colors(r,
        x, y, rgba_top_left,
        x + w, y, rgba_top_right,
        x + w, y + h, rgba_bottom_right);
    ui_push_tri_colors(r,
        x, y, rgba_top_left,
        x + w, y + h, rgba_bottom_right,
        x, y + h, rgba_bottom_left);
}

static void ui_round_rect_points(f64 *pts, i32 *out_n, f64 x, f64 y, f64 w, f64 h, f64 radius) {
    f64 r = ui_clamp_f64(radius, 0, fmin(w, h) * 0.5);
    i32 n = 0;
    if (r <= 0) {
        pts[n++] = x; pts[n++] = y;
        pts[n++] = x + w; pts[n++] = y;
        pts[n++] = x + w; pts[n++] = y + h;
        pts[n++] = x; pts[n++] = y + h;
        *out_n = 4;
        return;
    }
    const i32 seg = 8;
    const f64 corners[4][3] = {
        {x + w - r, y + r, -M_PI_2},
        {x + w - r, y + h - r, 0},
        {x + r, y + h - r, M_PI_2},
        {x + r, y + r, M_PI},
    };
    for (i32 c = 0; c < 4; c++) {
        for (i32 i = 0; i <= seg; i++) {
            f64 a = corners[c][2] + (f64)i / (f64)seg * M_PI_2;
            pts[n * 2 + 0] = corners[c][0] + cos(a) * r;
            pts[n * 2 + 1] = corners[c][1] + sin(a) * r;
            n++;
        }
    }
    *out_n = n;
}

static void ui_draw_round_ring(ui_renderer *r, const f64 *outer, const f64 *inner, i32 n, u32 outer_color, u32 inner_color) {
    for (i32 i = 0; i < n; i++) {
        const i32 j = (i + 1) % n;
        ui_push_tri_colors(r,
            outer[i * 2], outer[i * 2 + 1], outer_color,
            inner[i * 2], inner[i * 2 + 1], inner_color,
            inner[j * 2], inner[j * 2 + 1], inner_color);
        ui_push_tri_colors(r,
            outer[i * 2], outer[i * 2 + 1], outer_color,
            inner[j * 2], inner[j * 2 + 1], inner_color,
            outer[j * 2], outer[j * 2 + 1], outer_color);
    }
}

void ui_fill_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, u32 rgba, f64 feather) {
    if (!r || w <= 0 || h <= 0) return;
    const f64 f = ui_clamp_f64(ui_resolve_feather(feather), 0.0, fmin(w, h) * 0.5);
    f64 outer[4 * 9 * 2], inner[4 * 9 * 2];
    i32 n = 0, inner_n = 0;
    ui_round_rect_points(outer, &n, x, y, w, h, radius);
    ui_round_rect_points(inner, &inner_n, x + f, y + f, w - f * 2.0, h - f * 2.0, fmax(0.01, radius - f));
    if (n < 3 || inner_n != n) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    const f64 cx = x + w * 0.5;
    const f64 cy = y + h * 0.5;
    for (i32 i = 0; i < n; i++) {
        const i32 j = (i + 1) % n;
        ui_push_tri(r, cx, cy, inner[i * 2], inner[i * 2 + 1], inner[j * 2], inner[j * 2 + 1], 0, 0, rgba);
    }
    ui_draw_round_ring(r, outer, inner, n, ui_color_alpha_mul(rgba, 0.0), rgba);
}

void ui_fill_round_rect_per_corner(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 rtl, f64 rtr, f64 rbl, f64 rbr, u32 rgba, f64 feather) {
    ns_unused(rtl);
    ns_unused(rtr);
    ns_unused(rbl);
    ui_fill_round_rect(r, x, y, w, h, rbr, rgba, feather);
}

static i32 ui_utf8_next(const unsigned char **cursor) {
    const unsigned char *p = *cursor;
    if (!p || !*p) return 0;
    i32 code = *p++;
    if (code < 0x80) { *cursor = p; return code; }
    if ((code & 0xe0) == 0xc0 && (p[0] & 0xc0) == 0x80) {
        code = ((code & 0x1f) << 6) | (p[0] & 0x3f);
        p += 1;
    } else if ((code & 0xf0) == 0xe0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
        code = ((code & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
        p += 2;
    } else if ((code & 0xf8) == 0xf0 && (p[0] & 0xc0) == 0x80 &&
               (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        code = ((code & 0x07) << 18) | ((p[0] & 0x3f) << 12) |
               ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        p += 3;
    } else {
        code = 0xfffd;
    }
    *cursor = p;
    return code;
}

static ui_font *ui_primary_font(ui_renderer *r, i32 font_type) {
    if (font_type == UI_FONT_MONO) return &r->fonts[UI_FONT_MONO];
    if (font_type == UI_FONT_ZH && r->fonts[UI_FONT_ZH].glyph_count > 0) return &r->fonts[UI_FONT_ZH];
    if (font_type == UI_FONT_BITMAP && r->fonts[UI_FONT_BITMAP].glyph_count > 0) return &r->fonts[UI_FONT_BITMAP];
    return &r->fonts[UI_FONT_MAIN];
}

static ui_font *ui_font_for_code(ui_renderer *r, i32 font_type, i32 code, ui_glyph **glyph) {
    ui_font *font = ui_primary_font(r, font_type);
    *glyph = ui_font_glyph(font, code);
    if (font_type == UI_FONT_BITMAP && font == &r->fonts[UI_FONT_BITMAP]) {
        if (!*glyph && r->fonts[UI_FONT_BITMAP_ZH].glyph_count > 0) {
            ui_glyph *chinese = ui_font_glyph(&r->fonts[UI_FONT_BITMAP_ZH], code);
            if (chinese) { font = &r->fonts[UI_FONT_BITMAP_ZH]; *glyph = chinese; }
        }
        return font;
    }
    if (!*glyph && font != &r->fonts[UI_FONT_ZH] && r->fonts[UI_FONT_ZH].glyph_count > 0) {
        ui_glyph *zh_glyph = ui_font_glyph(&r->fonts[UI_FONT_ZH], code);
        if (zh_glyph) { font = &r->fonts[UI_FONT_ZH]; *glyph = zh_glyph; }
    }
    if (!*glyph && font != &r->fonts[UI_FONT_MAIN]) {
        ui_glyph *main_glyph = ui_font_glyph(&r->fonts[UI_FONT_MAIN], code);
        if (main_glyph) { font = &r->fonts[UI_FONT_MAIN]; *glyph = main_glyph; }
    }
    if (!*glyph) *glyph = ui_font_glyph(font, 32);
    return font;
}

static ns_bool ui_font_is_bitmap(ui_renderer *r, ui_font *font) {
    return font == &r->fonts[UI_FONT_BITMAP] || font == &r->fonts[UI_FONT_BITMAP_ZH];
}

static i32 ui_font_texture_id(ui_renderer *r, ui_font *font) {
    if (font == &r->fonts[UI_FONT_BITMAP]) return UI_FONT_BITMAP_TEXTURE;
    if (font == &r->fonts[UI_FONT_BITMAP_ZH]) return UI_FONT_BITMAP_ZH_TEXTURE;
    if (font == &r->fonts[UI_FONT_ZH]) return UI_FONT_ZH_TEXTURE;
    return UI_FONT_TEXTURE;
}

static f64 ui_missing_glyph_advance(i32 font_type, f64 font_px) {
    return font_type == UI_FONT_BITMAP ? font_px * 0.6 : font_px * 0.55;
}

void ui_draw_text(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type) {
    if (!r || !text || font_px <= 0) return;
    ui_font *primary = ui_primary_font(r, font_type);
    f64 cx = x;
    f64 cy = y;
    const unsigned char *p = (const unsigned char*)text;
    while (*p) {
        i32 code = ui_utf8_next(&p);
        if (code == '\n') {
            cx = x;
            cy += primary->line_height * (font_px / primary->font_size);
            continue;
        }
        ui_glyph *g = NULL;
        ui_font *font = ui_font_for_code(r, font_type, code, &g);
        if (!g) {
            cx += ui_missing_glyph_advance(font_type, font_px);
            continue;
        }
        f64 scale = font_px / font->font_size;
        if (g->width > 0 && g->height > 0) {
            f64 x0 = cx + g->x_offset * scale;
            f64 y0 = cy + g->y_offset * scale;
            const ns_bool bitmap = ui_font_is_bitmap(r, font);
            if (bitmap) {
                x0 = floor(x0);
                y0 = floor(y0);
            }
            f64 x1 = x0 + g->width * scale;
            f64 y1 = y0 + g->height * scale;
            r->current_texture_id = ui_font_texture_id(r, font);
            ui_push_quad_ex(r, x0, y0, x1, y1,
                            g->atlas_x / font->texture_width,
                            g->atlas_y / font->texture_height,
                            (g->atlas_x + g->width) / font->texture_width,
                            (g->atlas_y + g->height) / font->texture_height,
                            rgba, bitmap ? UI_KIND_BITMAP : UI_KIND_MSDF, 5.0, 0.0, 1.0);
        }
        cx += g->x_advance * scale;
    }
}

static f64 ui_text_char_advance(ui_renderer *r, i32 font_type, i32 code, f64 font_px) {
    ui_glyph *g = NULL;
    ui_font *font = ui_font_for_code(r, font_type, code, &g);
    return g && font->font_size > 0.0
               ? g->x_advance * (font_px / font->font_size)
               : ui_missing_glyph_advance(font_type, font_px);
}

void ui_draw_text_arc(ui_renderer *r, f64 cx, f64 cy, f64 radius, f64 center_angle,
                      const char *text, f64 font_px, u32 rgba, i32 font_type) {
    if (!r || !text || radius <= 0.0 || font_px <= 0.0) return;
    f64 total_width = 0.0;
    const unsigned char *measure = (const unsigned char*)text;
    while (*measure) {
        const i32 code = ui_utf8_next(&measure);
        if (code == '\n') break;
        total_width += ui_text_char_advance(r, font_type, code, font_px);
    }

    f64 cursor = -total_width * 0.5;
    const unsigned char *p = (const unsigned char*)text;
    while (*p) {
        const i32 code = ui_utf8_next(&p);
        if (code == '\n') break;
        ui_glyph *g = NULL;
        ui_font *font = ui_font_for_code(r, font_type, code, &g);
        if (!g) {
            cursor += ui_missing_glyph_advance(font_type, font_px);
            continue;
        }
        const f64 scale = font_px / font->font_size;
        const f64 advance = g->x_advance * scale;
        const f64 angle = center_angle + (cursor + advance * 0.5) / radius;
        const f64 rotation = angle + M_PI * 0.5;
        const f64 origin_x = cx + cos(angle) * radius;
        const f64 origin_y = cy + sin(angle) * radius;
        if (g->width > 0 && g->height > 0) {
            const f64 line_top = -(font->cap_top + font->baseline) * 0.5 * scale;
            const f64 x0 = g->x_offset * scale - advance * 0.5;
            const f64 y0 = line_top + g->y_offset * scale;
            const f64 x1 = x0 + g->width * scale;
            const f64 y1 = y0 + g->height * scale;
            const ns_bool bitmap = ui_font_is_bitmap(r, font);
            r->current_texture_id = ui_font_texture_id(r, font);
            ui_push_quad_rotated(r, origin_x, origin_y, cos(rotation), sin(rotation),
                                 x0, y0, x1, y1,
                                 g->atlas_x / font->texture_width,
                                 g->atlas_y / font->texture_height,
                                 (g->atlas_x + g->width) / font->texture_width,
                                 (g->atlas_y + g->height) / font->texture_height,
                                 rgba, bitmap ? UI_KIND_BITMAP : UI_KIND_MSDF, 5.0, 0.0, 1.0);
        }
        cursor += advance;
    }
}

static void ui_draw_text_range(ui_renderer *r, f64 x, f64 y, const char *text, i32 len, f64 font_px, u32 rgba, i32 font_type) {
    if (!r || !text || len <= 0) return;
    char *line = (char*)malloc((size_t)len + 1);
    if (!line) return;
    memcpy(line, text, (size_t)len);
    line[len] = '\0';
    ui_draw_text(r, x, y, line, font_px, rgba, font_type);
    free(line);
}

f64 ui_draw_text_wrapped(ui_renderer *r, f64 x, f64 y, f64 w, const char *text, f64 font_px, u32 rgba, i32 font_type) {
    if (!r || !text || font_px <= 0.0) return 0.0;
    if (w <= 0.0) return 0.0;

    ui_font *font = ui_primary_font(r, font_type);
    const f64 line_h = font->font_size > 0.0 ? font->line_height * (font_px / font->font_size) : font_px;
    const char *line_start = text;
    const char *p = text;
    const char *last_space = ns_null;
    f64 line_w = 0.0;
    f64 cy = y;

    while (*p) {
        if (*p == '\n') {
            ui_draw_text_range(r, x, cy, line_start, (i32)(p - line_start), font_px, rgba, font_type);
            cy += line_h;
            p++;
            line_start = p;
            last_space = ns_null;
            line_w = 0.0;
            continue;
        }

        const char *char_start = p;
        const unsigned char *next = (const unsigned char*)p;
        i32 code = ui_utf8_next(&next);
        if (code == ' ' || code == '\t') last_space = char_start;
        line_w += ui_text_char_advance(r, font_type, code, font_px);

        if (line_w > w && char_start > line_start) {
            const char *break_at = last_space && last_space >= line_start ? last_space : char_start;
            ui_draw_text_range(r, x, cy, line_start, (i32)(break_at - line_start), font_px, rgba, font_type);
            cy += line_h;

            p = break_at;
            while (*p == ' ' || *p == '\t') p++;
            line_start = p;
            last_space = ns_null;
            line_w = 0.0;
            continue;
        }

        p = (const char*)next;
    }

    if (p > line_start) {
        ui_draw_text_range(r, x, cy, line_start, (i32)(p - line_start), font_px, rgba, font_type);
        cy += line_h;
    }

    return cy - y;
}

f64 ui_text_line_height(ui_renderer *r, f64 font_px, i32 font_type) {
    if (!r) return font_px;
    ui_font *font = ui_primary_font(r, font_type);
    return font->font_size > 0 ? font->line_height * (font_px / font->font_size) : font_px;
}

f64 ui_text_v_center_y(ui_renderer *r, f64 y, f64 h, f64 font_px, i32 font_type) {
    if (!r) return y + (h - font_px) * 0.5;
    ui_font *font = ui_primary_font(r, font_type);
    if (font->font_size <= 0.0) return y + (h - font_px) * 0.5;
    // ui_draw_text takes the top of the line box, whose glyphs sit below
    // center (the box reserves room for descenders and line gap). Return the
    // top that centers the cap band (cap top .. baseline) in the rect instead,
    // so the visible ink is what ends up in the middle.
    return y + h * 0.5 - (font->cap_top + font->baseline) * 0.5 * (font_px / font->font_size);
}

f64 ui_text_width(ui_renderer *r, const char *text, f64 font_px, i32 font_type) {
    if (!r || !text) return 0;
    f64 width = 0;
    const unsigned char *p = (const unsigned char*)text;
    while (*p) {
        i32 code = ui_utf8_next(&p);
        if (code == '\n') break;
        width += ui_text_char_advance(r, font_type, code, font_px);
    }
    return width;
}

// Byte offset of the caret nearest to local x inside a single-line string.
// Offsets are UTF-8 bytes so they compose with substr; x past the end clamps
// to strlen, and x before the first glyph returns 0.
i32 ui_text_index_at_x(ui_renderer *r, const char *text, f64 font_px, i32 font_type, f64 x) {
    if (!r || !text || font_px <= 0.0) return 0;
    if (x <= 0.0) return 0;
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *p = start;
    f64 cursor = 0.0;
    while (*p) {
        const unsigned char *glyph = p;
        i32 code = ui_utf8_next(&p);
        if (code == '\n') break;
        f64 advance = ui_text_char_advance(r, font_type, code, font_px);
        if (x < cursor + advance * 0.5) {
            return (i32)(glyph - start);
        }
        cursor += advance;
    }
    return (i32)(p - start);
}

ui_text_sel *ui_text_sel_create(void) {
    ui_text_sel *s = (ui_text_sel *)calloc(1, sizeof(ui_text_sel));
    if (s) s->active = -1;
    return s;
}

void ui_text_sel_clear(ui_text_sel *s) {
    if (!s) return;
    s->active = -1;
    s->anchor = 0;
    s->head = 0;
    s->dragging = false;
}

ns_bool ui_text_sel_has(ui_text_sel *s) {
    return s && s->active >= 0 && s->anchor != s->head;
}

i32 ui_text_sel_lo(ui_text_sel *s) {
    if (!s) return 0;
    return s->anchor <= s->head ? s->anchor : s->head;
}

i32 ui_text_sel_hi(ui_text_sel *s) {
    if (!s) return 0;
    return s->anchor <= s->head ? s->head : s->anchor;
}

static i32 ui_text_sel_clamp(i32 idx, i32 len) {
    if (idx < 0) return 0;
    if (idx > len) return len;
    return idx;
}

const char *ui_text_sel_slice(const char *text, ui_text_sel *s) {
    static char *buf = NULL;
    static size_t cap = 0;
    if (!text || !ui_text_sel_has(s)) return "";
    i32 len = (i32)strlen(text);
    i32 lo = ui_text_sel_clamp(ui_text_sel_lo(s), len);
    i32 hi = ui_text_sel_clamp(ui_text_sel_hi(s), len);
    if (hi <= lo) return "";
    size_t n = (size_t)(hi - lo);
    if (n + 1 > cap) {
        size_t next = n + 1;
        if (next < 64) next = 64;
        char *grown = (char *)realloc(buf, next);
        if (!grown) return "";
        buf = grown;
        cap = next;
    }
    memcpy(buf, text + lo, n);
    buf[n] = 0;
    return buf;
}

f64 ui_text_prefix_width(ui_renderer *r, const char *text, i32 end, f64 font_px, i32 font_type) {
    if (!r || !text || end <= 0 || font_px <= 0.0) return 0.0;
    i32 len = (i32)strlen(text);
    if (end >= len) return ui_text_width(r, text, font_px, font_type);
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *p = start;
    f64 width = 0.0;
    while (*p) {
        const unsigned char *glyph = p;
        i32 code = ui_utf8_next(&p);
        if (code == '\n') break;
        if ((i32)(glyph - start) >= end) break;
        width += ui_text_char_advance(r, font_type, code, font_px);
    }
    return width;
}

ns_bool ui_text_sel_interact(ui_renderer *r, ui_text_sel *s, i32 field, const char *text,
                             f64 x, f64 y, f64 w, f64 h, f64 font_px, i32 font_type,
                             f64 mx, f64 my, ns_bool pressed, ns_bool down, ns_bool released) {
    if (!s) return false;
    ns_bool inside = mx >= x && mx < x + w && my >= y && my < y + h;
    if (pressed) {
        if (inside && text) {
            i32 idx = ui_text_index_at_x(r, text, font_px, font_type, mx - x);
            s->active = field;
            s->anchor = idx;
            s->head = idx;
            s->dragging = true;
        } else if (s->active == field && !s->dragging) {
            ui_text_sel_clear(s);
        }
    }
    if (s->dragging && s->active == field) {
        if (down && text) {
            f64 local = mx - x;
            if (local < 0.0) local = 0.0;
            if (local > w) local = w;
            s->head = ui_text_index_at_x(r, text, font_px, font_type, local);
        }
        if (released) s->dragging = false;
    }
    if (released && s->active == field) s->dragging = false;
    return s->active == field;
}

void ui_draw_text_sel(ui_renderer *r, f64 x, f64 y, f64 h, const char *text, f64 font_px, u32 rgba,
                      i32 font_type, ui_text_sel *s, i32 field, u32 sel_rgba) {
    if (!r || !text) return;
    if (s && s->active == field && s->anchor != s->head) {
        i32 lo = ui_text_sel_lo(s);
        i32 hi = ui_text_sel_hi(s);
        f64 x0 = x + ui_text_prefix_width(r, text, lo, font_px, font_type);
        f64 x1 = x + ui_text_prefix_width(r, text, hi, font_px, font_type);
        f64 sw = x1 - x0;
        if (sw > 0.0) ui_fill_rect(r, x0, y, sw, h, sel_rgba, 0.0);
    }
    f64 ty = ui_text_v_center_y(r, y, h, font_px, font_type);
    ui_draw_text(r, x, ty, text, font_px, rgba, font_type);
}

ns_bool ui_text_sel_copy(view *v, ui_text_sel *s, const char *text) {
    if (!v || !s || !text || !ui_text_sel_has(s)) return false;
    i32 mods = view_take_key_press(v, VIEW_KEY_C);
    if (mods < 0) return false;
    if ((mods & VIEW_KEY_MOD_CONTROL) == 0 && (mods & VIEW_KEY_MOD_SUPER) == 0) return false;
    const char *slice = ui_text_sel_slice(text, s);
    if (!slice || !slice[0]) return false;
    view_set_clipboard(v, slice);
    return true;
}

static ns_bool ui_text_is_vertical_space(i32 code) {
    return code == 32 || code == 9;
}

static ns_bool ui_text_is_vertical_break(i32 code) {
    return code == 10 || code == 13;
}

static void ui_utf8_encode(i32 code, char *buf) {
    if (code < 0x80) {
        buf[0] = (char)code;
        buf[1] = 0;
        return;
    }
    if (code < 0x800) {
        buf[0] = (char)(0xc0 | (code >> 6));
        buf[1] = (char)(0x80 | (code & 0x3f));
        buf[2] = 0;
        return;
    }
    if (code < 0x10000) {
        buf[0] = (char)(0xe0 | (code >> 12));
        buf[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (code & 0x3f));
        buf[3] = 0;
        return;
    }
    buf[0] = (char)(0xf0 | (code >> 18));
    buf[1] = (char)(0x80 | ((code >> 12) & 0x3f));
    buf[2] = (char)(0x80 | ((code >> 6) & 0x3f));
    buf[3] = (char)(0x80 | (code & 0x3f));
    buf[4] = 0;
}

i32 ui_text_vertical_column_count(const char *text) {
    if (!text || !text[0]) return 0;
    i32 columns = 1;
    ns_bool saw_glyph = false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        i32 code = ui_utf8_next(&p);
        if (ui_text_is_vertical_break(code)) {
            if (saw_glyph) columns = columns + 1;
            saw_glyph = false;
        } else if (!ui_text_is_vertical_space(code)) {
            saw_glyph = true;
        }
    }
    if (!saw_glyph && columns > 1) columns = columns - 1;
    return columns;
}

i32 ui_text_vertical_max_run(const char *text) {
    if (!text) return 0;
    i32 longest = 0;
    i32 run = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        i32 code = ui_utf8_next(&p);
        if (ui_text_is_vertical_break(code)) {
            if (run > longest) longest = run;
            run = 0;
        } else if (!ui_text_is_vertical_space(code)) {
            run = run + 1;
        }
    }
    if (run > longest) longest = run;
    return longest;
}

f64 ui_text_vertical_column_width(ui_renderer *r, f64 font_px, i32 font_type) {
    f64 wide = ui_text_width(r, "国", font_px, font_type);
    if (wide > 0.0) return wide;
    f64 em = ui_text_width(r, "M", font_px, font_type);
    if (em > 0.0) return em;
    return font_px;
}

ui_text_size *ui_text_vertical_size(ui_renderer *r, const char *text, f64 font_px, i32 font_type) {
    ui_text_size *size = (ui_text_size *)malloc(sizeof(ui_text_size));
    if (!size) return NULL;
    f64 col_w = ui_text_vertical_column_width(r, font_px, font_type);
    f64 step_y = ui_text_line_height(r, font_px, font_type);
    size->w = col_w * (f64)ui_text_vertical_column_count(text);
    size->h = step_y * (f64)ui_text_vertical_max_run(text);
    return size;
}

void ui_draw_text_vertical(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type) {
    if (!r || !text || font_px <= 0.0) return;
    f64 col_w = ui_text_vertical_column_width(r, font_px, font_type);
    f64 step_y = ui_text_line_height(r, font_px, font_type);
    f64 col_x = x - col_w;
    f64 cy = y;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        i32 code = ui_utf8_next(&p);
        if (ui_text_is_vertical_break(code)) {
            col_x = col_x - col_w;
            cy = y;
            continue;
        }
        if (ui_text_is_vertical_space(code)) continue;
        char buf[8];
        ui_utf8_encode(code, buf);
        f64 gw = ui_text_width(r, buf, font_px, font_type);
        ui_draw_text(r, col_x + (col_w - gw) * 0.5, cy, buf, font_px, rgba, font_type);
        cy = cy + step_y;
    }
}

ui_text_size *ui_measure_text(ui_renderer *r, const char *text, f64 font_px, i32 font_type) {
    ui_text_size *size = (ui_text_size*)malloc(sizeof(ui_text_size));
    if (!size) return NULL;
    size->w = ui_text_width(r, text, font_px, font_type);
    size->h = ui_text_line_height(r, font_px, font_type);
    return size;
}

f64 ui_mono_char_width(ui_renderer *r, f64 font_px, i32 font_type) {
    ns_unused(font_type);
    return ui_text_width(r, "0", font_px, UI_FONT_MONO);
}

u32 ui_pack_rgba_floats(f64 r, f64 g, f64 b, f64 a) {
    u32 rr = (u32)ui_clamp_f64(r * 255.0, 0.0, 255.0);
    u32 gg = (u32)ui_clamp_f64(g * 255.0, 0.0, 255.0);
    u32 bb = (u32)ui_clamp_f64(b * 255.0, 0.0, 255.0);
    u32 aa = (u32)ui_clamp_f64(a * 255.0, 0.0, 255.0);
    return (aa << 24) | (bb << 16) | (gg << 8) | rr;
}
