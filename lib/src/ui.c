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
#define UI_WHITE_TEXTURE 1
#define UI_FONT_TEXTURE 2
#define UI_FONT_ZH_TEXTURE (UI_MAX_TEXTURES + 3)
#define UI_KIND_IMAGE 0
#define UI_KIND_MSDF 1
#define UI_KIND_SCENE_GRID 2
#define UI_KIND_ARC_SDF 3
#define UI_DEFAULT_FEATHER 0.5
#define UI_SCENE_MAX_MESHES 64
#define UI_SCENE_VERTEX_FLOATS 10

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

typedef struct ui_scene_grid_uniforms {
    f32 inverse_view_projection[16];
    f32 viewport[4];
    f32 params[4];
} ui_scene_grid_uniforms;

typedef struct ui_rect_batch {
    ui_vertex *vertices;
    i32 vertex_count;
    i32 vertex_capacity;
    i32 gpu_vertex_capacity;
    gpu_buffer vertex_buffer;
    gpu_buffer offset_buffer;
    gpu_binding binding;
    gpu_mesh mesh;
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

typedef struct ui_renderer {
    void *handle;
    view *v;
    i32 width;
    i32 height;
    f64 content_scale;

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

    ui_font fonts[3];
    gpu_texture white_texture;
    gpu_texture font_texture;
    gpu_texture font_zh_texture;
    gpu_buffer screen_buffer;
    gpu_buffer clip_buffer;
    gpu_buffer vertex_buffer;
    gpu_shader shader_image;
    gpu_shader shader_batch;
    gpu_shader shader_msdf;
    gpu_shader shader_scene_grid;
    gpu_shader shader_arc_sdf;
    gpu_pipeline pipeline_image;
    gpu_pipeline pipeline_batch;
    gpu_pipeline pipeline_msdf;
    gpu_pipeline pipeline_scene_grid;
    gpu_pipeline pipeline_arc_sdf;
    gpu_binding binding_white_image;
    gpu_binding binding_font_msdf;
    gpu_binding binding_font_zh_msdf;
    gpu_buffer scene_grid_buffer;
    gpu_binding binding_scene_grid;
    gpu_binding binding_arc_sdf;
    gpu_texture textures[UI_MAX_TEXTURES];
    gpu_binding texture_bindings[UI_MAX_TEXTURES];
    i32 texture_widths[UI_MAX_TEXTURES];
    i32 texture_heights[UI_MAX_TEXTURES];
    ui_rect_batch rect_batches[UI_MAX_RECT_BATCHES];
    gpu_mesh mesh;
    gpu_render_pass screen_pass;
    ns_bool gpu_ready;
} ui_renderer;

typedef struct ui_scene_mesh {
    f32 *vertices;
    u32 *indices;
    i32 vertex_count;
    i32 index_count;
    ns_bool used;
} ui_scene_mesh;

typedef struct ui_gizmo_result {
    ns_bool active;
    ns_bool changed;
    i32 axis;
    f64 tx, ty, tz;
    f64 rx, ry, rz;
    f64 sx, sy, sz;
} ui_gizmo_result;

typedef struct ui_scene {
    void *handle;
    ui_renderer *renderer;
    ui_scene_mesh meshes[UI_SCENE_MAX_MESHES];
    f32 view_projection[16];
    f64 x, y, width, height;
    i32 active_axis;
    f64 last_pointer_x, last_pointer_y;
    ui_gizmo_result gizmo_result;
    ns_bool begun;
} ui_scene;

typedef struct ui_scene_projected {
    f64 x, y, z, w;
    u32 color;
    f64 nx, ny, nz;
} ui_scene_projected;

typedef struct ui_scene_triangle {
    ui_scene_projected a, b, c;
    f64 depth;
} ui_scene_triangle;

typedef struct ui_theme { void *handle; } ui_theme;
typedef struct ui_hit { ns_bool hovered; ns_bool pressed; } ui_hit;
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
void ui_fill_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, u32 rgba, f64 feather);
void ui_fill_arc(ui_renderer *r, f64 cx, f64 cy, f64 radius, f64 thickness, f64 angle_start, f64 angle_end, u32 rgba, f64 feather);
void ui_stroke_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, f64 thickness, u32 rgba, f64 feather);
void ui_draw_text(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type);
static void ui_round_rect_points(f64 *pts, i32 *out_n, f64 x, f64 y, f64 w, f64 h, f64 radius);
static void ui_draw_round_ring(ui_renderer *r, const f64 *outer, const f64 *inner, i32 n, u32 outer_color, u32 inner_color);

static const char *ui_shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; uchar4 col [[attribute(2)]]; float4 params [[attribute(3)]]; };\n"
"struct VOut { float4 pos [[position]]; float2 pixel; float2 uv; float4 col; float4 params; };\n"
"vertex VOut ui_vs(VIn in [[stage_in]], constant float2 &screen [[buffer(1)]]) {\n"
"  VOut o; float2 ndc = float2((in.pos.x / screen.x) * 2.0 - 1.0, 1.0 - (in.pos.y / screen.y) * 2.0);\n"
"  o.pos = float4(ndc, 0.0, 1.0); o.pixel = in.pos; o.uv = in.uv; o.col = float4(in.col) / 255.0; o.params = in.params; return o;\n"
"}\n"
"vertex VOut ui_vs_batch(VIn in [[stage_in]], constant float2 &screen [[buffer(1)]], constant float2 &offset [[buffer(2)]]) {\n"
"  VOut o; float2 pixel = in.pos + offset; float2 ndc = float2((pixel.x / screen.x) * 2.0 - 1.0, 1.0 - (pixel.y / screen.y) * 2.0);\n"
"  o.pos = float4(ndc, 0.0, 1.0); o.pixel = pixel; o.uv = in.uv; o.col = float4(in.col) / 255.0; o.params = in.params; return o;\n"
"}\n"
"static inline half ui_median3(half r, half g, half b) { return max(min(r, g), min(max(r, g), b)); }\n"
"static inline bool ui_clip_discard(VOut in, constant float4 *clip_rects) {\n"
"  uint clip_idx = uint(round(max(in.params.w, 0.0)));\n"
"  if (clip_idx == 0u) { return false; }\n"
"  float4 c = clip_rects[clip_idx - 1u];\n"
"  return in.pixel.x < c.x || in.pixel.y < c.y || in.pixel.x >= c.z || in.pixel.y >= c.w;\n"
"}\n"
"fragment float4 ui_fs_image(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], constant float4 *clip_rects [[buffer(0)]]) {\n"
"  if (ui_clip_discard(in, clip_rects)) { discard_fragment(); }\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
"  return tex.sample(samp, in.uv) * in.col;\n"
"}\n"
"fragment float4 ui_fs_msdf(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], constant float4 *clip_rects [[buffer(0)]]) {\n"
"  if (ui_clip_discard(in, clip_rects)) { discard_fragment(); }\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
"  float4 s = tex.sample(samp, in.uv); half sd = ui_median3(half(s.r), half(s.g), half(s.b));\n"
"  float2 tex_size = float2(tex.get_width(), tex.get_height()); float range = max(in.params.x, 0.5);\n"
"  float2 unit_range = float2(range) / tex_size; float2 screen_texel = max(fwidth(in.uv), float2(1e-6));\n"
"  float px_range = max(0.5 * dot(unit_range, 1.0 / screen_texel), 1.0);\n"
"  float opacity = clamp(((float(sd) - 0.5) * px_range + in.params.y) / max(in.params.z, 1.0) + 0.5, 0.0, 1.0);\n"
"  return float4(in.col.rgb, in.col.a * opacity);\n"
"}\n"
"fragment float4 ui_fs_arc_sdf(VOut in [[stage_in]], constant float4 *clip_rects [[buffer(0)]]) {\n"
"  if (ui_clip_discard(in, clip_rects)) { discard_fragment(); }\n"
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

static const char *ui_scene_grid_shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; uchar4 col [[attribute(2)]]; float4 params [[attribute(3)]]; };\n"
"struct VOut { float4 pos [[position]]; float2 pixel; };\n"
"struct GridUniforms { float4x4 inverse_view_projection; float4 viewport; float4 params; };\n"
"vertex VOut ui_scene_grid_vs(VIn in [[stage_in]], constant float2 &screen [[buffer(1)]]) {\n"
"  VOut o; float2 ndc = float2((in.pos.x / screen.x) * 2.0 - 1.0, 1.0 - (in.pos.y / screen.y) * 2.0);\n"
"  o.pos = float4(ndc, 0.0, 1.0); o.pixel = in.pos; return o;\n"
"}\n"
"static inline float grid_coverage(float2 p, float spacing, float radius_pixels) {\n"
"  float2 deriv = max(abs(dfdx(p)) + abs(dfdy(p)), float2(1e-6));\n"
"  float2 cell = abs(fract(p / spacing - 0.5) - 0.5) * spacing;\n"
"  float2 pixel_distance = cell / deriv;\n"
"  float2 line = 1.0 - smoothstep(float2(max(radius_pixels - 0.5, 0.0)), float2(radius_pixels + 0.5), pixel_distance);\n"
"  float frequency_fade = 1.0 - smoothstep(spacing * 0.45, spacing * 0.95, max(deriv.x, deriv.y));\n"
"  return max(line.x, line.y) * frequency_fade;\n"
"}\n"
"fragment float4 ui_scene_grid_fs(VOut in [[stage_in]], constant GridUniforms &grid [[buffer(2)]]) {\n"
"  float2 uv = (in.pixel - grid.viewport.xy) / grid.viewport.zw;\n"
"  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
"  float4 near_h = grid.inverse_view_projection * float4(ndc, -1.0, 1.0);\n"
"  float4 far_h = grid.inverse_view_projection * float4(ndc, 1.0, 1.0);\n"
"  float3 near_p = near_h.xyz / max(abs(near_h.w), 1e-6) * sign(near_h.w);\n"
"  float3 far_p = far_h.xyz / max(abs(far_h.w), 1e-6) * sign(far_h.w);\n"
"  float denom = far_p.y - near_p.y;\n"
"  if (abs(denom) < 1e-6) return float4(0.0);\n"
"  float ray_t = -near_p.y / denom;\n"
"  if (ray_t < 0.0 || ray_t > 1.0) return float4(0.0);\n"
"  float2 ground = mix(near_p, far_p, ray_t).xz;\n"
"  float extent = grid.params.x;\n"
"  float radial = length(ground);\n"
"  float edge_width = max(fwidth(radial) * 1.5, extent * 0.008);\n"
"  float edge = 1.0 - smoothstep(extent - edge_width, extent, radial);\n"
"  if (edge <= 0.0) return float4(0.0);\n"
"  float minor = grid_coverage(ground, grid.params.y, 0.52);\n"
"  float major = grid_coverage(ground, grid.params.z, 1.15);\n"
"  float line = max(minor, major);\n"
"  return float4(0.0, 0.0, 0.0, line * edge);\n"
"}\n";

static ns_bool ui_mat4_inverse(const f32 m[16], f32 out[16]) {
    f32 inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    f32 determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(determinant) <= 1e-8f) return false;
    determinant = 1.0f / determinant;
    for (i32 i = 0; i < 16; i++) out[i] = inv[i] * determinant;
    return true;
}

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

static ui_clip ui_current_clip(ui_renderer *r) {
    if (r->clip_count <= 0) return (ui_clip){0, 0, r->width, r->height};
    return r->clips[r->clip_count - 1];
}

static f64 ui_clip_param(ui_renderer *r, ui_clip c) {
    if (!r || c.w <= 0.0 || c.h <= 0.0) return 0.0;
    if (c.x <= 0.0 && c.y <= 0.0 && c.x + c.w >= (f64)r->width && c.y + c.h >= (f64)r->height) {
        return 0.0;
    }

    ui_gpu_clip gpu_clip = {
        .x0 = (f32)c.x,
        .y0 = (f32)c.y,
        .x1 = (f32)(c.x + c.w),
        .y1 = (f32)(c.y + c.h),
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
    f32 screen[2] = {(f32)r->width, (f32)r->height};
    r->screen_buffer = gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = sizeof(screen),
        .data = (ns_data){screen, sizeof(screen)},
        .type = BUFFER_UNIFORM,
        .usage = USAGE_DEFAULT,
    });
    r->clip_buffer = gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = (int)sizeof(r->gpu_clips),
        .type = BUFFER_UNIFORM,
        .usage = USAGE_DEFAULT,
    });
    r->vertex_buffer = gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = r->vertex_capacity * UI_VERTEX_STRIDE,
        .type = BUFFER_VERTEX,
        .usage = USAGE_DEFAULT,
    });
    r->scene_grid_buffer = gpu_create_buffer_desc(&(gpu_buffer_desc){
        .size = (int)sizeof(ui_scene_grid_uniforms),
        .type = BUFFER_UNIFORM,
        .usage = USAGE_DEFAULT,
    });

    u32 white = 0xffffffffu;
    r->white_texture = gpu_create_texture(&(gpu_texture_desc){
        .width = 1, .height = 1, .depth = 1,
        .data = (ns_data){&white, sizeof(white)},
        .format = PIXELFORMAT_RGBA8,
        .type = TEXTURE_2D,
        .usage = TEXTURE_USAGE_READ,
        .resource_usage = USAGE_DEFAULT,
    });

    char image_path[UI_PATH_MAX];
    io_image *img = ui_resolve_asset("latin_mono.png", image_path)
                        ? io_load_image(image_path)
                        : NULL;
    if (!img) fprintf(stderr, "ui: cannot locate or load latin_mono.png\n");
    if (img && img->data && img->channels == 4) {
        r->font_texture = gpu_create_texture(&(gpu_texture_desc){
            .width = img->width, .height = img->height, .depth = 1,
            .data = (ns_data){img->data, (size_t)(img->width * img->height * img->channels)},
            .format = PIXELFORMAT_RGBA8,
            .type = TEXTURE_2D,
            .usage = TEXTURE_USAGE_READ,
            .resource_usage = USAGE_DEFAULT,
        });
    }

    gpu_shader_desc shader_desc = {0};
    shader_desc.vertex.source = ns_str_cstr(ui_shader_src);
    shader_desc.vertex.entry = ns_str_cstr("ui_vs");
    shader_desc.fragment.source = ns_str_cstr(ui_shader_src);
    shader_desc.fragment.entry = ns_str_cstr("ui_fs_image");
    r->shader_image = gpu_create_shader(&shader_desc);
    shader_desc.vertex.entry = ns_str_cstr("ui_vs_batch");
    r->shader_batch = gpu_create_shader(&shader_desc);
    shader_desc.vertex.entry = ns_str_cstr("ui_vs");
    shader_desc.fragment.entry = ns_str_cstr("ui_fs_msdf");
    r->shader_msdf = gpu_create_shader(&shader_desc);
    shader_desc.fragment.entry = ns_str_cstr("ui_fs_arc_sdf");
    r->shader_arc_sdf = gpu_create_shader(&shader_desc);
    shader_desc.vertex.source = ns_str_cstr(ui_scene_grid_shader_src);
    shader_desc.vertex.entry = ns_str_cstr("ui_scene_grid_vs");
    shader_desc.fragment.source = ns_str_cstr(ui_scene_grid_shader_src);
    shader_desc.fragment.entry = ns_str_cstr("ui_scene_grid_fs");
    r->shader_scene_grid = gpu_create_shader(&shader_desc);

    gpu_pipeline_desc pipe = {0};
    pipe.layout.buffers[0] = (gpu_vertex_buffer_layout_state){.stride = UI_VERTEX_STRIDE, .step_func = VERTEX_STEP_PER_VERTEX, .step_rate = 1};
    pipe.layout.attributes[0] = (gpu_vertex_attribute_state){.buffer_index = 0, .offset = 0, .size = 2, .format = ATTRIBUTE_FORMAT_FLOAT};
    pipe.layout.attributes[1] = (gpu_vertex_attribute_state){.buffer_index = 0, .offset = 8, .size = 2, .format = ATTRIBUTE_FORMAT_FLOAT};
    pipe.layout.attributes[2] = (gpu_vertex_attribute_state){.buffer_index = 0, .offset = 16, .size = 4, .format = ATTRIBUTE_FORMAT_UBYTE};
    pipe.layout.attributes[3] = (gpu_vertex_attribute_state){.buffer_index = 0, .offset = 20, .size = 4, .format = ATTRIBUTE_FORMAT_FLOAT};
    pipe.color_count = 1;
    pipe.colors[0].format = PIXELFORMAT_BGRA8;
    pipe.colors[0].blend = (gpu_blend_state){
        .enabled = true,
        .src_factor = BLEND_FACTOR_SRC_ALPHA,
        .dst_factor = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .op = BLEND_OP_ADD,
        .src_factor_alpha = BLEND_FACTOR_ONE,
        .dst_factor_alpha = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .op_alpha = BLEND_OP_ADD,
    };
    // Apple views expose a Depth32Float attachment on their screen render
    // pass. Metal requires every pipeline used by that pass to declare the
    // same attachment format, even when the draw does not use depth testing.
    // Keep UI ordering unchanged by accepting every fragment without writing
    // depth.
    pipe.depth.format = PIXELFORMAT_DEPTH;
    pipe.depth.compare_func = COMPARE_ALWAYS;
    pipe.depth.write_enabled = false;
    pipe.primitive_type = PRIMITIVE_TRIANGLES;
    pipe.index_type = INDEX_NONE;
    pipe.cull_mode = CULL_NONE;
    pipe.face_winding = FACE_WINDING_CCW;
    pipe.sample_count = 1;
    pipe.shader = r->shader_image;
    r->pipeline_image = gpu_create_pipeline(&pipe);
    pipe.shader = r->shader_batch;
    r->pipeline_batch = gpu_create_pipeline(&pipe);
    pipe.shader = r->shader_msdf;
    r->pipeline_msdf = gpu_create_pipeline(&pipe);
    pipe.shader = r->shader_arc_sdf;
    r->pipeline_arc_sdf = gpu_create_pipeline(&pipe);
    pipe.shader = r->shader_scene_grid;
    r->pipeline_scene_grid = gpu_create_pipeline(&pipe);

    r->binding_white_image = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_image,
        .buffers = {
            {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
            {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
        },
        .textures = {{.texture = r->white_texture, .name = ns_str_cstr("tex")}},
    });
    r->binding_font_msdf = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_msdf,
        .buffers = {
            {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
            {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
        },
        .textures = {{.texture = r->font_texture, .name = ns_str_cstr("tex")}},
    });
    r->binding_scene_grid = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_scene_grid,
        .buffers = {
            {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
            {.buffer = r->scene_grid_buffer, .name = ns_str_cstr("grid")},
        },
    });
    r->binding_arc_sdf = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_arc_sdf,
        .buffers = {
            {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
            {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
        },
    });
    r->mesh = gpu_create_mesh(&(gpu_mesh_desc){
        .buffers = {r->vertex_buffer},
        .pipeline = r->pipeline_image,
    });
    r->screen_pass = (gpu_render_pass){.id = 0};
    r->gpu_ready = r->screen_buffer.id && r->clip_buffer.id && r->vertex_buffer.id && r->white_texture.id &&
                   r->font_texture.id && r->scene_grid_buffer.id && r->pipeline_image.id &&
                   r->pipeline_batch.id && r->pipeline_msdf.id && r->pipeline_scene_grid.id &&
                   r->pipeline_arc_sdf.id && r->binding_white_image.id && r->binding_font_msdf.id &&
                   r->binding_scene_grid.id && r->binding_arc_sdf.id && r->mesh.id;
}

static f64 ui_view_content_scale(view *v) {
    if (!v) return 1.0;
    if (v->display_ratio > 0.0) return v->display_ratio;
    if (v->ui_scale > 0.0) return v->ui_scale;
    return 1.0;
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
    }
    r->width = lw > 0 ? lw : 1;
    r->height = lh > 0 ? lh : 1;
}

ui_renderer *ui_renderer_create(view *v) {
    ui_renderer *r = (ui_renderer*)calloc(1, sizeof(ui_renderer));
    if (!r) return NULL;
    r->handle = r;
    r->v = v;
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

    gpu_texture texture = gpu_create_texture(&(gpu_texture_desc){
        .width = image->width, .height = image->height, .depth = 1,
        .data = (ns_data){image->data, (size_t)(image->width * image->height * image->channels)},
        .format = PIXELFORMAT_RGBA8,
        .type = TEXTURE_2D,
        .usage = TEXTURE_USAGE_READ,
        .resource_usage = USAGE_DEFAULT,
    });
    free(image->data);
    free(image);
    if (!texture.id) {
        free(main_font.glyphs);
        free(mono_font.glyphs);
        return false;
    }
    gpu_binding binding = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_msdf,
        .buffers = {
            {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
            {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
        },
        .textures = {{.texture = texture, .name = ns_str_cstr("tex")}},
    });
    if (!binding.id) {
        gpu_destroy_texture(texture);
        free(main_font.glyphs);
        free(mono_font.glyphs);
        return false;
    }

    free(r->fonts[UI_FONT_MAIN].glyphs);
    free(r->fonts[UI_FONT_MONO].glyphs);
    if (r->binding_font_msdf.id) gpu_destroy_binding(r->binding_font_msdf);
    if (r->font_texture.id) gpu_destroy_texture(r->font_texture);
    r->fonts[UI_FONT_MAIN] = main_font;
    r->fonts[UI_FONT_MONO] = mono_font;
    r->font_texture = texture;
    r->binding_font_msdf = binding;
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

    gpu_texture texture = gpu_create_texture(&(gpu_texture_desc){
        .width = image->width, .height = image->height, .depth = 1,
        .data = (ns_data){image->data, (size_t)(image->width * image->height * image->channels)},
        .format = PIXELFORMAT_RGBA8,
        .type = TEXTURE_2D,
        .usage = TEXTURE_USAGE_READ,
        .resource_usage = USAGE_DEFAULT,
    });
    free(image->data);
    free(image);
    if (!texture.id) {
        free(zh_font.glyphs);
        return false;
    }
    gpu_binding binding = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_msdf,
        .buffers = {
            {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
            {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
        },
        .textures = {{.texture = texture, .name = ns_str_cstr("tex")}},
    });
    if (!binding.id) {
        gpu_destroy_texture(texture);
        free(zh_font.glyphs);
        return false;
    }

    free(r->fonts[UI_FONT_ZH].glyphs);
    if (r->binding_font_zh_msdf.id) gpu_destroy_binding(r->binding_font_zh_msdf);
    if (r->font_zh_texture.id) gpu_destroy_texture(r->font_zh_texture);
    r->fonts[UI_FONT_ZH] = zh_font;
    r->font_zh_texture = texture;
    r->binding_font_zh_msdf = binding;
    return true;
}

void ui_renderer_destroy(ui_renderer *r) {
    if (!r) return;
    for (i32 i = 0; i < UI_MAX_RECT_BATCHES; i++) {
        ui_rect_batch *batch = &r->rect_batches[i];
        if (batch->binding.id) gpu_destroy_binding(batch->binding);
        if (batch->mesh.id) gpu_destroy_mesh(batch->mesh);
        if (batch->vertex_buffer.id) gpu_destroy_buffer(batch->vertex_buffer);
        if (batch->offset_buffer.id) gpu_destroy_buffer(batch->offset_buffer);
        free(batch->vertices);
    }
    for (i32 i = 0; i < UI_MAX_TEXTURES; i++) {
        if (r->texture_bindings[i].id) gpu_destroy_binding(r->texture_bindings[i]);
        if (r->textures[i].id) gpu_destroy_texture(r->textures[i]);
    }
    if (r->binding_font_zh_msdf.id) gpu_destroy_binding(r->binding_font_zh_msdf);
    if (r->font_zh_texture.id) gpu_destroy_texture(r->font_zh_texture);
    if (r->binding_scene_grid.id) gpu_destroy_binding(r->binding_scene_grid);
    if (r->binding_arc_sdf.id) gpu_destroy_binding(r->binding_arc_sdf);
    if (r->scene_grid_buffer.id) gpu_destroy_buffer(r->scene_grid_buffer);
    if (r->pipeline_scene_grid.id) gpu_destroy_pipeline(r->pipeline_scene_grid);
    if (r->pipeline_arc_sdf.id) gpu_destroy_pipeline(r->pipeline_arc_sdf);
    if (r->shader_scene_grid.id) gpu_destroy_shader(r->shader_scene_grid);
    if (r->shader_arc_sdf.id) gpu_destroy_shader(r->shader_arc_sdf);
    for (i32 i = 0; i < 3; i++) free(r->fonts[i].glyphs);
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
    if (batch->binding.id) gpu_destroy_binding(batch->binding);
    if (batch->mesh.id) gpu_destroy_mesh(batch->mesh);
    if (batch->vertex_buffer.id) gpu_destroy_buffer(batch->vertex_buffer);
    if (batch->offset_buffer.id) gpu_destroy_buffer(batch->offset_buffer);
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
    if (!batch) return false;
    if (batch->vertex_count <= 0) return true;
    if (!batch->offset_buffer.id) {
        f32 offset[2] = {0.0f, 0.0f};
        batch->offset_buffer = gpu_create_buffer_desc(&(gpu_buffer_desc){
            .size = sizeof(offset),
            .data = (ns_data){offset, sizeof(offset)},
            .type = BUFFER_UNIFORM,
            .usage = USAGE_DEFAULT,
        });
    }
    if (!batch->binding.id && batch->offset_buffer.id) {
        batch->binding = gpu_create_binding(&(gpu_binding_desc){
            .pipeline = r->pipeline_batch,
            .buffers = {
                {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
                {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
                {.buffer = batch->offset_buffer, .name = ns_str_cstr("offset")},
            },
            .textures = {{.texture = r->white_texture, .name = ns_str_cstr("tex")}},
        });
    }
    if (!batch->offset_buffer.id || !batch->binding.id) return false;
    if (!batch->vertex_buffer.id || !batch->mesh.id || batch->vertex_count > batch->gpu_vertex_capacity) {
        if (batch->mesh.id) gpu_destroy_mesh(batch->mesh);
        if (batch->vertex_buffer.id) gpu_destroy_buffer(batch->vertex_buffer);
        batch->mesh = (gpu_mesh){0};
        batch->vertex_buffer = gpu_create_buffer_desc(&(gpu_buffer_desc){
            .size = batch->vertex_capacity * UI_VERTEX_STRIDE,
            .data = (ns_data){batch->vertices, (size_t)batch->vertex_count * UI_VERTEX_STRIDE},
            .type = BUFFER_VERTEX,
            .usage = USAGE_DEFAULT,
        });
        if (!batch->vertex_buffer.id) {
            batch->gpu_vertex_capacity = 0;
            return false;
        }
        gpu_update_buffer_desc(batch->vertex_buffer, (ns_data){
            batch->vertices, (size_t)batch->vertex_count * UI_VERTEX_STRIDE
        });
        batch->gpu_vertex_capacity = batch->vertex_capacity;
        batch->mesh = gpu_create_mesh(&(gpu_mesh_desc){
            .buffers = {batch->vertex_buffer},
            .pipeline = r->pipeline_batch,
        });
    } else {
        gpu_update_buffer_desc(batch->vertex_buffer, (ns_data){
            batch->vertices, (size_t)batch->vertex_count * UI_VERTEX_STRIDE
        });
    }
    return batch->mesh.id != 0;
}

void ui_rect_batch_draw_at(ui_renderer *r, i32 batch_id, f64 dx, f64 dy) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch || !batch->mesh.id || batch->vertex_count <= 0 || r->command_count >= UI_MAX_COMMANDS) return;
    ui_clip c = ui_current_clip(r);
    if (c.w <= 0.0 || c.h <= 0.0) return;
    ui_command *cmd = &r->commands[r->command_count++];
    *cmd = (ui_command){
        .vertex_offset = 0,
        .vertex_count = batch->vertex_count,
        .texture_id = UI_WHITE_TEXTURE,
        .kind = UI_KIND_IMAGE,
        .rect_batch_id = batch_id,
        .offset_x = dx,
        .offset_y = dy,
        .clip_x = (i32)floor(c.x),
        .clip_y = (i32)floor(c.y),
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
        if (r->textures[slot].id) continue;
        gpu_texture texture = gpu_create_texture(&(gpu_texture_desc){
            .width = width, .height = height, .depth = 1,
            .data = (ns_data){(void*)data, (size_t)width * (size_t)height * 4},
            .format = PIXELFORMAT_RGBA8,
            .type = TEXTURE_2D,
            .usage = TEXTURE_USAGE_READ,
            .resource_usage = USAGE_DEFAULT,
        });
        if (!texture.id) return 0;
        gpu_binding binding = gpu_create_binding(&(gpu_binding_desc){
            .pipeline = r->pipeline_image,
            .buffers = {
                {.buffer = r->screen_buffer, .name = ns_str_cstr("screen")},
                {.buffer = r->clip_buffer, .name = ns_str_cstr("clip_rects")},
            },
            .textures = {{.texture = texture, .name = ns_str_cstr("tex")}},
        });
        if (!binding.id) {
            gpu_destroy_texture(texture);
            return 0;
        }
        r->textures[slot] = texture;
        r->texture_bindings[slot] = binding;
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
    if (r->texture_bindings[slot].id) gpu_destroy_binding(r->texture_bindings[slot]);
    if (r->textures[slot].id) gpu_destroy_texture(r->textures[slot]);
    r->texture_bindings[slot] = (gpu_binding){0};
    r->textures[slot] = (gpu_texture){0};
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
    f32 screen[2] = {(f32)r->width, (f32)r->height};
    if (r->screen_buffer.id) gpu_update_buffer_desc(r->screen_buffer, (ns_data){screen, sizeof(screen)});
}

void ui_resize_to(ui_renderer *r, i32 width, i32 height) {
    if (!r) return;
    r->width = width > 0 ? width : 1;
    r->height = height > 0 ? height : 1;
    f32 screen[2] = {(f32)r->width, (f32)r->height};
    if (r->screen_buffer.id) gpu_update_buffer_desc(r->screen_buffer, (ns_data){screen, sizeof(screen)});
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
    r->clips[0] = (ui_clip){0, 0, r->width, r->height};
    r->current_texture_id = UI_WHITE_TEXTURE;
}

void ui_flush(ui_renderer *r, ui_color_rgba *clear) {
    if (!r || !r->gpu_ready) return;
    gpu_update_buffer_desc(r->vertex_buffer, (ns_data){r->vertices, (size_t)r->vertex_count * UI_VERTEX_STRIDE});
    if (r->clip_buffer.id) {
        ui_gpu_clip empty_clip = {0};
        const void *clip_data = r->gpu_clip_count > 0 ? (const void*)r->gpu_clips : (const void*)&empty_clip;
        const size_t clip_len = r->gpu_clip_count > 0 ? (size_t)r->gpu_clip_count * sizeof(ui_gpu_clip) : sizeof(empty_clip);
        gpu_update_buffer_desc(r->clip_buffer, (ns_data){(void*)clip_data, clip_len});
    }
    const f64 s = r->content_scale > 0.0 ? r->content_scale : 1.0;
    gpu_begin_render_pass(r->screen_pass);
    gpu_set_viewport(0, 0, (i32)(r->width * s + 0.5), (i32)(r->height * s + 0.5));
    ns_unused(clear);
    for (i32 i = 0; i < r->command_count; i++) {
        ui_command *cmd = &r->commands[i];
        if (cmd->clip_w <= 0 || cmd->clip_h <= 0) continue;
        gpu_set_scissor((i32)floor(cmd->clip_x * s), (i32)floor(cmd->clip_y * s),
                        (i32)ceil(cmd->clip_w * s), (i32)ceil(cmd->clip_h * s));
        ui_rect_batch *batch = NULL;
        if (cmd->rect_batch_id > 0) {
            batch = ui_rect_batch_get(r, cmd->rect_batch_id);
            if (!batch || !batch->mesh.id || !batch->binding.id || !batch->offset_buffer.id) continue;
            f32 offset[2] = {(f32)cmd->offset_x, (f32)cmd->offset_y};
            gpu_update_buffer_desc(batch->offset_buffer, (ns_data){offset, sizeof(offset)});
            gpu_set_pipeline(r->pipeline_batch);
            gpu_set_binding(batch->binding);
        } else if (cmd->kind == UI_KIND_SCENE_GRID) {
            gpu_set_pipeline(r->pipeline_scene_grid);
            gpu_set_binding(r->binding_scene_grid);
        } else if (cmd->kind == UI_KIND_ARC_SDF) {
            gpu_set_pipeline(r->pipeline_arc_sdf);
            gpu_set_binding(r->binding_arc_sdf);
        } else if (cmd->kind == UI_KIND_MSDF) {
            gpu_set_pipeline(r->pipeline_msdf);
            if (cmd->texture_id == UI_FONT_ZH_TEXTURE && r->binding_font_zh_msdf.id) {
                gpu_set_binding(r->binding_font_zh_msdf);
            } else {
                gpu_set_binding(r->binding_font_msdf);
            }
        } else if (cmd->texture_id >= 3 && cmd->texture_id < UI_MAX_TEXTURES + 3 &&
                   r->texture_bindings[cmd->texture_id - 3].id) {
            gpu_set_pipeline(r->pipeline_image);
            gpu_set_binding(r->texture_bindings[cmd->texture_id - 3]);
        } else {
            gpu_set_pipeline(r->pipeline_image);
            gpu_set_binding(r->binding_white_image);
        }
        if (batch) {
            gpu_set_mesh(batch->mesh);
        } else {
            gpu_set_mesh(r->mesh);
        }
        gpu_draw(cmd->vertex_offset, cmd->vertex_count, 1);
    }
    gpu_end_pass();
    gpu_commit();
    r->vertex_count = 0;
    r->command_count = 0;
}

i32 ui_canvas_width(ui_renderer *r) {
    return r ? r->width : 0;
}

i32 ui_canvas_height(ui_renderer *r) {
    return r ? r->height : 0;
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

static u32 ui_scene_pack(f64 r, f64 g, f64 b, f64 a) {
    r = ui_clamp_f64(r, 0.0, 1.0); g = ui_clamp_f64(g, 0.0, 1.0);
    b = ui_clamp_f64(b, 0.0, 1.0); a = ui_clamp_f64(a, 0.0, 1.0);
    return ((u32)(a * 255.0 + 0.5) << 24) | ((u32)(b * 255.0 + 0.5) << 16) |
           ((u32)(g * 255.0 + 0.5) << 8) | (u32)(r * 255.0 + 0.5);
}

static u32 ui_scene_shade(u32 color, f64 light, ns_bool selected) {
    f64 r = (f64)(color & 0xffu) / 255.0;
    f64 g = (f64)((color >> 8) & 0xffu) / 255.0;
    f64 b = (f64)((color >> 16) & 0xffu) / 255.0;
    f64 a = (f64)((color >> 24) & 0xffu) / 255.0;
    light = ui_clamp_f64(light, 0.18, 1.0);
    if (selected) { r = r * light * 0.75 + 0.12; g = g * light * 0.75 + 0.22; b = b * light * 0.75 + 0.15; }
    else { r *= light; g *= light; b *= light; }
    return ui_scene_pack(r, g, b, a);
}

static void ui_scene_mul_point(const f32 *m, f64 x, f64 y, f64 z, f64 *ox, f64 *oy, f64 *oz, f64 *ow) {
    *ox = m[0] * x + m[4] * y + m[8] * z + m[12];
    *oy = m[1] * x + m[5] * y + m[9] * z + m[13];
    *oz = m[2] * x + m[6] * y + m[10] * z + m[14];
    *ow = m[3] * x + m[7] * y + m[11] * z + m[15];
}

static ui_scene_projected ui_scene_project(ui_scene *scene, const f32 *model, f64 x, f64 y, f64 z,
                                            f64 nx, f64 ny, f64 nz, u32 color) {
    f64 wx, wy, wz, ww, cx, cy, cz, cw;
    ui_scene_mul_point(model, x, y, z, &wx, &wy, &wz, &ww);
    if (fabs(ww) > 0.000001 && fabs(ww - 1.0) > 0.000001) { wx /= ww; wy /= ww; wz /= ww; }
    ui_scene_mul_point(scene->view_projection, wx, wy, wz, &cx, &cy, &cz, &cw);
    f64 nnx = model[0] * nx + model[4] * ny + model[8] * nz;
    f64 nny = model[1] * nx + model[5] * ny + model[9] * nz;
    f64 nnz = model[2] * nx + model[6] * ny + model[10] * nz;
    f64 nl = sqrt(nnx * nnx + nny * nny + nnz * nnz);
    if (nl > 0.000001) { nnx /= nl; nny /= nl; nnz /= nl; }
    ui_scene_projected out = {.w = cw, .color = color, .nx = nnx, .ny = nny, .nz = nnz};
    if (fabs(cw) > 0.000001) {
        f64 ndc_x = cx / cw, ndc_y = cy / cw;
        out.z = cz / cw;
        out.x = scene->x + (ndc_x * 0.5 + 0.5) * scene->width;
        out.y = scene->y + (0.5 - ndc_y * 0.5) * scene->height;
    }
    return out;
}

static int ui_scene_triangle_compare(const void *a, const void *b) {
    const ui_scene_triangle *ta = (const ui_scene_triangle *)a, *tb = (const ui_scene_triangle *)b;
    return ta->depth < tb->depth ? 1 : (ta->depth > tb->depth ? -1 : 0);
}

ui_scene *ui_scene_create(ui_renderer *r) {
    if (!r) return NULL;
    ui_scene *scene = (ui_scene *)calloc(1, sizeof(ui_scene));
    if (scene) { scene->handle = scene; scene->renderer = r; scene->active_axis = -1; }
    return scene;
}

void ui_scene_destroy(ui_scene *scene) {
    if (!scene) return;
    for (i32 i = 1; i < UI_SCENE_MAX_MESHES; i++) { free(scene->meshes[i].vertices); free(scene->meshes[i].indices); }
    free(scene);
}

i32 ui_scene_mesh_create(ui_scene *scene) {
    if (!scene) return 0;
    for (i32 i = 1; i < UI_SCENE_MAX_MESHES; i++) if (!scene->meshes[i].used) { scene->meshes[i].used = true; return i; }
    return 0;
}

ns_bool ui_scene_mesh_update(ui_scene *scene, i32 mesh_id, f32 *vertices, i32 vertex_count, u32 *indices, i32 index_count) {
    if (!scene || mesh_id <= 0 || mesh_id >= UI_SCENE_MAX_MESHES || !scene->meshes[mesh_id].used ||
        !vertices || vertex_count < 0 || !indices || index_count < 0) return false;
    ui_scene_mesh *mesh = &scene->meshes[mesh_id];
    size_t vb = (size_t)vertex_count * UI_SCENE_VERTEX_FLOATS * sizeof(f32), ib = (size_t)index_count * sizeof(u32);
    f32 *nv = vb ? (f32 *)malloc(vb) : NULL; u32 *ni = ib ? (u32 *)malloc(ib) : NULL;
    if ((vb && !nv) || (ib && !ni)) { free(nv); free(ni); return false; }
    if (vb) memcpy(nv, vertices, vb); if (ib) memcpy(ni, indices, ib);
    free(mesh->vertices); free(mesh->indices);
    mesh->vertices = nv; mesh->indices = ni; mesh->vertex_count = vertex_count; mesh->index_count = index_count;
    return true;
}

void ui_scene_mesh_destroy(ui_scene *scene, i32 mesh_id) {
    if (!scene || mesh_id <= 0 || mesh_id >= UI_SCENE_MAX_MESHES) return;
    free(scene->meshes[mesh_id].vertices); free(scene->meshes[mesh_id].indices);
    memset(&scene->meshes[mesh_id], 0, sizeof(scene->meshes[mesh_id]));
}

void ui_scene_begin(ui_scene *scene, f64 x, f64 y, f64 width, f64 height, f32 *view_projection, u32 background) {
    if (!scene || !scene->renderer || !view_projection || width <= 0.0 || height <= 0.0) return;
    scene->x = x; scene->y = y; scene->width = width; scene->height = height;
    memcpy(scene->view_projection, view_projection, sizeof(scene->view_projection));
    ui_fill_rect(scene->renderer, x, y, width, height, background, 0.0);
    ui_push_clip(scene->renderer, x, y, width, height); scene->begun = true;
}

void ui_scene_draw_grid(ui_scene *scene, f64 extent, f64 minor_spacing, f64 major_spacing) {
    if (!scene || !scene->begun || extent <= 0.0 || minor_spacing <= 0.0 || major_spacing <= 0.0) return;
    ui_renderer *r = scene->renderer;
    ui_scene_grid_uniforms grid = {0};
    if (!r || !ui_mat4_inverse(scene->view_projection, grid.inverse_view_projection)) return;
    grid.viewport[0] = (f32)scene->x;
    grid.viewport[1] = (f32)scene->y;
    grid.viewport[2] = (f32)scene->width;
    grid.viewport[3] = (f32)scene->height;
    grid.params[0] = (f32)extent;
    grid.params[1] = (f32)minor_spacing;
    grid.params[2] = (f32)major_spacing;
    gpu_update_buffer_desc(r->scene_grid_buffer, (ns_data){&grid, sizeof(grid)});
    r->current_texture_id = UI_WHITE_TEXTURE;
    ui_push_quad_ex(r, scene->x, scene->y, scene->x + scene->width, scene->y + scene->height,
                    0.0, 0.0, 1.0, 1.0, 0xffffffffu, UI_KIND_SCENE_GRID, 0.0, 0.0, 0.0);
}

void ui_scene_draw_mesh(ui_scene *scene, i32 mesh_id, f32 *model, i32 flags) {
    if (!scene || !scene->begun || !model || mesh_id <= 0 || mesh_id >= UI_SCENE_MAX_MESHES) return;
    ui_scene_mesh *mesh = &scene->meshes[mesh_id];
    if (!mesh->used || !mesh->vertices || !mesh->indices || mesh->index_count < 3) return;
    i32 count = mesh->index_count / 3, valid = 0;
    ui_scene_triangle *triangles = (ui_scene_triangle *)malloc((size_t)count * sizeof(ui_scene_triangle));
    if (!triangles) return;
    for (i32 i = 0; i < count; i++) {
        u32 ids[3] = {mesh->indices[i * 3], mesh->indices[i * 3 + 1], mesh->indices[i * 3 + 2]};
        if (ids[0] >= (u32)mesh->vertex_count || ids[1] >= (u32)mesh->vertex_count || ids[2] >= (u32)mesh->vertex_count) continue;
        ui_scene_projected p[3];
        for (i32 j = 0; j < 3; j++) {
            f32 *v = &mesh->vertices[ids[j] * UI_SCENE_VERTEX_FLOATS];
            p[j] = ui_scene_project(scene, model, v[0], v[1], v[2], v[3], v[4], v[5], ui_scene_pack(v[6], v[7], v[8], v[9]));
        }
        if (p[0].w <= 0.0001 || p[1].w <= 0.0001 || p[2].w <= 0.0001) continue;
        f64 area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
        if (!(flags & 4) && area >= 0.0) continue;
        triangles[valid++] = (ui_scene_triangle){p[0], p[1], p[2], (p[0].z + p[1].z + p[2].z) / 3.0};
    }
    qsort(triangles, (size_t)valid, sizeof(ui_scene_triangle), ui_scene_triangle_compare);
    for (i32 i = 0; i < valid; i++) {
        ui_scene_triangle *t = &triangles[i];
        f64 nx = (t->a.nx + t->b.nx + t->c.nx) / 3.0, ny = (t->a.ny + t->b.ny + t->c.ny) / 3.0;
        f64 nz = (t->a.nz + t->b.nz + t->c.nz) / 3.0;
        f64 light = 0.32 + 0.68 * fmax(0.0, nx * -0.32 + ny * 0.72 + nz * 0.62);
        if (flags & 1) ui_push_tri_colors(scene->renderer,
            t->a.x, t->a.y, ui_scene_shade(t->a.color, light, flags & 8),
            t->b.x, t->b.y, ui_scene_shade(t->b.color, light, flags & 8),
            t->c.x, t->c.y, ui_scene_shade(t->c.color, light, flags & 8));
        if (flags & 2) {
            u32 edge = flags & 8 ? 0xfff56e4cu : 0x996c757du;
            ui_stroke_line(scene->renderer, t->a.x, t->a.y, t->b.x, t->b.y, 1.0, edge, 0.0);
            ui_stroke_line(scene->renderer, t->b.x, t->b.y, t->c.x, t->c.y, 1.0, edge, 0.0);
            ui_stroke_line(scene->renderer, t->c.x, t->c.y, t->a.x, t->a.y, 1.0, edge, 0.0);
        }
    }
    free(triangles);
}

static ns_bool ui_scene_project_position(ui_scene *scene, f32 *p, f64 *x, f64 *y) {
    static const f32 identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    ui_scene_projected out = ui_scene_project(scene, identity, p[0], p[1], p[2], 0, 0, 1, 0xffffffffu);
    if (out.w <= 0.0001) return false; *x = out.x; *y = out.y; return true;
}

void ui_scene_draw_lines(ui_scene *scene, f32 *positions, i32 point_count, u32 rgba, f64 thickness) {
    if (!scene || !scene->begun || !positions) return;
    for (i32 i = 0; i + 1 < point_count; i += 2) { f64 x0,y0,x1,y1;
        if (ui_scene_project_position(scene, &positions[i*3], &x0, &y0) && ui_scene_project_position(scene, &positions[(i+1)*3], &x1, &y1))
            ui_stroke_line(scene->renderer, x0, y0, x1, y1, thickness, rgba, 0.0); }
}

void ui_scene_draw_points(ui_scene *scene, f32 *positions, i32 point_count, u32 rgba, f64 radius) {
    if (!scene || !scene->begun || !positions) return;
    for (i32 i = 0; i < point_count; i++) { f64 x,y; if (ui_scene_project_position(scene, &positions[i*3], &x, &y)) ui_fill_circle(scene->renderer, x, y, radius, rgba, 0.0); }
}

void ui_scene_end(ui_scene *scene) { if (scene && scene->begun) { ui_pop_clip(scene->renderer); scene->begun = false; } }

static f64 ui_scene_distance_segment(f64 px, f64 py, f64 ax, f64 ay, f64 bx, f64 by) {
    f64 vx = bx-ax, vy = by-ay, vv = vx*vx+vy*vy;
    f64 t = vv > 0.000001 ? ((px-ax)*vx+(py-ay)*vy)/vv : 0.0; t = ui_clamp_f64(t,0.0,1.0);
    return hypot(px-(ax+vx*t), py-(ay+vy*t));
}

ui_gizmo_result *ui_gizmo(ui_scene *scene, i32 mode, f64 x, f64 y, f64 z, f64 px, f64 py,
                           ns_bool down, ns_bool pressed, ns_bool released) {
    if (!scene || !scene->renderer) return NULL;
    memset(&scene->gizmo_result, 0, sizeof(scene->gizmo_result)); scene->gizmo_result.axis = scene->active_axis;
    scene->gizmo_result.sx = scene->gizmo_result.sy = scene->gizmo_result.sz = 1.0;
    f32 lines[18] = {(f32)x,(f32)y,(f32)z,(f32)(x+1),(f32)y,(f32)z,
                     (f32)x,(f32)y,(f32)z,(f32)x,(f32)(y+1),(f32)z,
                     (f32)x,(f32)y,(f32)z,(f32)x,(f32)y,(f32)(z+1)};
    f64 cx,cy,ex[3],ey[3]; if (!ui_scene_project_position(scene, lines, &cx, &cy)) return &scene->gizmo_result;
    for (i32 a=0;a<3;a++) ui_scene_project_position(scene,&lines[(a*2+1)*3],&ex[a],&ey[a]);
    u32 colors[3]={0xff5c68f2u,0xff63d46au,0xffff985cu};
    for(i32 a=0;a<3;a++){ui_stroke_line(scene->renderer,cx,cy,ex[a],ey[a],scene->active_axis==a?5.0:3.0,colors[a],0.0);ui_fill_circle(scene->renderer,ex[a],ey[a],mode==1?5.0:6.0,colors[a],0.0);}
    if(pressed){f64 best=12.0;scene->active_axis=-1;for(i32 a=0;a<3;a++){f64 d=ui_scene_distance_segment(px,py,cx,cy,ex[a],ey[a]);if(d<best){best=d;scene->active_axis=a;}}scene->last_pointer_x=px;scene->last_pointer_y=py;}
    if(down&&scene->active_axis>=0){i32 a=scene->active_axis;f64 vx=ex[a]-cx,vy=ey[a]-cy,l=hypot(vx,vy);if(l<.0001)l=1;vx/=l;vy/=l;f64 delta=(px-scene->last_pointer_x)*vx+(py-scene->last_pointer_y)*vy;
        scene->gizmo_result.active=true;scene->gizmo_result.changed=fabs(delta)>.000001;scene->gizmo_result.axis=a;
        if(mode==0){f64 q=delta/80.0;if(a==0)scene->gizmo_result.tx=q;if(a==1)scene->gizmo_result.ty=q;if(a==2)scene->gizmo_result.tz=q;}
        else if(mode==1){f64 q=delta*.012;if(a==0)scene->gizmo_result.rx=q;if(a==1)scene->gizmo_result.ry=q;if(a==2)scene->gizmo_result.rz=q;}
        else{f64 q=fmax(.05,1.0+delta/100.0);if(a==0)scene->gizmo_result.sx=q;if(a==1)scene->gizmo_result.sy=q;if(a==2)scene->gizmo_result.sz=q;}
        scene->last_pointer_x=px;scene->last_pointer_y=py;}
    if(released)scene->active_axis=-1; return &scene->gizmo_result;
}

ui_gizmo_result *ui_gizmo_compact(ui_scene *scene, i32 mode, f64 *input) {
    if (!input) return ui_gizmo(scene, mode, 0.0, 0.0, 0.0, 0.0, 0.0, false, false, false);
    return ui_gizmo(scene, mode, input[0], input[1], input[2], input[3], input[4],
                    input[5] != 0.0, input[6] != 0.0, input[7] != 0.0);
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
}

void ui_widgets_begin_view(ui_widgets *w, ui_theme *theme, view *v, ns_bool gizmo_manipulating) {
    ns_unused(theme);
    if (!w || !v) return;
    memset(&w->input, 0, sizeof(w->input));
    w->input.mouse_x = v->mouse_x;
    w->input.mouse_y = v->mouse_y;
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
    if (w->input.mouse_released) w->active_id = 0;
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
                     ui_scene_pack(rr, gg, 1.0 - fabs(rr - gg), 1.0), 0.0);
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
    return &r->fonts[UI_FONT_MAIN];
}

static ui_font *ui_font_for_code(ui_renderer *r, i32 font_type, i32 code, ui_glyph **glyph) {
    ui_font *font = ui_primary_font(r, font_type);
    *glyph = ui_font_glyph(font, code);
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
        if (!g) continue;
        f64 scale = font_px / font->font_size;
        if (g->width > 0 && g->height > 0) {
            f64 x0 = cx + g->x_offset * scale;
            f64 y0 = cy + g->y_offset * scale;
            f64 x1 = x0 + g->width * scale;
            f64 y1 = y0 + g->height * scale;
            r->current_texture_id = (font == &r->fonts[UI_FONT_ZH]) ? UI_FONT_ZH_TEXTURE : UI_FONT_TEXTURE;
            ui_push_quad_ex(r, x0, y0, x1, y1,
                            g->atlas_x / font->texture_width,
                            g->atlas_y / font->texture_height,
                            (g->atlas_x + g->width) / font->texture_width,
                            (g->atlas_y + g->height) / font->texture_height,
                            rgba, UI_KIND_MSDF, 5.0, 0.0, 1.0);
        }
        cx += g->x_advance * scale;
    }
}

static f64 ui_text_char_advance(ui_renderer *r, i32 font_type, i32 code, f64 font_px) {
    ui_glyph *g = NULL;
    ui_font *font = ui_font_for_code(r, font_type, code, &g);
    return g && font->font_size > 0.0 ? g->x_advance * (font_px / font->font_size) : font_px * 0.55;
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
