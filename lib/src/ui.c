#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "gpu.h"
#include "ns_type.h"
#include "view.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define UI_VERTEX_STRIDE 40
#define UI_INITIAL_VERTEX_CAP 131072
#define UI_MAX_COMMANDS 16384
#define UI_MAX_CLIPS 32
#define UI_MAX_GPU_CLIPS 4096
#define UI_MAX_TEXTURES 32
#define UI_MAX_RECT_BATCHES 8192
#define UI_MAX_PLANES 8
#define UI_FONT_MAIN 0
#define UI_FONT_MONO 1
#define UI_FONT_ZH 2
#define UI_FONT_BITMAP 3
#define UI_FONT_BITMAP_ZH 4
#define UI_FONT_COUNT 5
// A command's texture_id: a solid shape samples nothing, the main/mono and
// Chinese faces live in the MSDF font pack, the bitmap faces and atlases are
// RGBA textures of their own.
#define UI_WHITE_TEXTURE 1
#define UI_FONT_TEXTURE 2
#define UI_FONT_ZH_TEXTURE (UI_MAX_TEXTURES + 3)
#define UI_FONT_BITMAP_TEXTURE (UI_MAX_TEXTURES + 4)
#define UI_FONT_BITMAP_ZH_TEXTURE (UI_MAX_TEXTURES + 5)
// The shared preview texture (ui_preview_load): every preview lives in one
// cell of it, so any number of previews draw in one ubershader run.
#define UI_PREVIEW_TEXTURE (UI_MAX_TEXTURES + 6)
#define UI_PREVIEW_SIZE 2048
#define UI_PREVIEW_CELL 256
#define UI_PREVIEW_COLUMNS (UI_PREVIEW_SIZE / UI_PREVIEW_CELL)
#define UI_PREVIEW_SLOTS (UI_PREVIEW_COLUMNS * UI_PREVIEW_COLUMNS)
#define UI_PREVIEW_RETIRED 4
#define UI_KIND_IMAGE 0
#define UI_KIND_MSDF 1
#define UI_KIND_ARC_SDF 2
#define UI_KIND_BITMAP 3
// Only in ui_vertex.meta: an image command with no texture of its own, drawn
// in its vertex colour.
#define UI_KIND_SOLID 4
// Only in a retained draw's meta: a ui_rect_batch_add rectangle batch. The
// batch scales freely, so a rectangle can shrink below a framebuffer pixel;
// the vertex stage widens such an axis to one pixel and the fragment stage
// box-filters its coverage, keeping thin rows from aliasing into moire. A
// ui_rect_batch_add_smooth rectangle is filtered on both axes, so a repeating
// pattern a few pixels tall does not beat against the pixel grid either;
// plain rectangles keep hard edges there so abutting fills show no seam.
#define UI_KIND_COVERAGE 5
// ui_vertex.meta, written when the frame is flushed: the kind (bits 0-3), the
// MSDF face in the font pack (bits 4-7) and the projection (bits 8-11) of the
// command the vertex belongs to. Projection 0 is the flat screen,
// 1..UI_MAX_PLANES a ui_begin_plane plane, and UI_PROJECTION_PANEL the 3D
// panel. The shader sources spell these numbers out.
#define UI_META_FACE_SHIFT 4
// The MSDF faces in the font pack: the main/mono atlas, then the Chinese one.
#define UI_FACE_MAIN 0
#define UI_FACE_ZH 1
#define UI_FACES 2
// Empty texels between packed faces, so linear sampling at a glyph's edge
// never reaches the next face.
#define UI_FONT_PACK_GUTTER 8
// The ui draws with two textures: the MSDF font pack and one RGBA texture.
#define UI_TEXTURE_MSDF 0
#define UI_TEXTURE_RGBA 1
#define UI_META_PROJECTION_SHIFT 8
#define UI_PROJECTION_PANEL (UI_MAX_PLANES + 1)
// Per projection in frame storage: a column-major clip matrix, then the
// framebuffer-pixel rectangle (x0, y0, x1, y1) that bounds a plane.
#define UI_PROJECTION_WORDS 20
#define UI_DEFAULT_FEATHER 0.5
#define UI_BITMAP_FONT_SIZE 10.0
// Head-locked HUD distance in tracking metres. Same NDC overlay at z=0 is
// optical infinity and ghosts against the stereo world; this plane sits in
// front of the 3D camera so each eye gets matching disparity and depth.
#define UI_HUD_DISTANCE_METRES 1.5f
// How far out on the panel the gaze may travel before the panel follows, as a
// share of the panel's half extent. A panel rigidly locked to the head carries
// every control along with the gaze, so the reticle would sit on whatever is in
// the middle of the overlay and could never address anything else. Keeping the
// panel still until the gaze leaves this band is what turns the head into a
// pointer; past the band the panel comes along and the band is what stops it
// ever being left behind.
#define UI_HUD_GAZE_LIMIT 0.82f
// Straight up and straight down are where a yaw/pitch panel degenerates.
#define UI_HUD_PITCH_LIMIT 1.45f

typedef struct io_image {
    i32 width;
    i32 height;
    i32 channels;
    u8 *data;
} io_image;

extern io_image *io_load_image(const char *path);
extern io_image *io_try_load_image(const char *path);
extern void io_image_destroy(io_image *img);

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
    u32 meta;
} ui_vertex;

_Static_assert(sizeof(ui_vertex) == UI_VERTEX_STRIDE, "the ui shader reads ten words per vertex");
_Static_assert(UI_PROJECTION_PANEL == 9 && UI_KIND_SOLID == 4 && UI_KIND_COVERAGE == 5 && UI_FACE_ZH == 1,
               "the ui shader sources hard-code the panel projection, the solid and coverage kinds and the faces");

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
    ns_bool retained;
    f64 offset_x, offset_y;
    f64 scale_x, scale_y;
    // 1-based ui_begin_plane projection, 0 for the flat screen.
    i32 plane;
    // Recorded between ui_begin_overlay and ui_end_overlay: flat on the
    // screen even while the renderer has a 3D panel.
    ns_bool overlay;
    // Content-space clip of a retained batch draw, applied per fragment.
    ui_clip batch_clip;
    i32 clip_x, clip_y, clip_w, clip_h;
} ui_command;

// A retained batch holds either solid rectangles or glyphs of one font
// texture; `kind` is fixed by the first add after ui_rect_batch_begin.
typedef struct ui_rect_batch {
    ui_vertex *vertices;
    i32 vertex_count;
    i32 vertex_capacity;
    i32 kind;
    i32 texture_id;
    // The batch's own GPU copy, rewritten only when the vertices changed and
    // the batch is drawn, so a large retained batch costs no per-frame upload.
    gpu_addr buffer;
    u64 buffer_size;
    ns_bool dirty;
    ns_bool used;
    u64 last_draw_frame;
} ui_rect_batch;

// A replaced batch buffer may still be read by a frame in flight.
#define UI_RETIRED_BUFFERS 32
#define UI_BATCH_GPU_CACHE 64
typedef struct ui_retired_buffer {
    gpu_addr addr;
    i32 frames;
} ui_retired_buffer;

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
    // Where a line of this face is centred, down from the line top: the
    // middle of 中 for a face that has it, else of the capitals.
    f64 middle;
} ui_font;

// Margins of the drawable that platform chrome may cover, in logical points.
typedef struct ui_insets {
    f64 top;
    f64 right;
    f64 bottom;
    f64 left;
} ui_insets;

// A fingertip against the 3D panel (ui_panel_touch_update), in canvas units.
typedef struct ui_panel_touch {
    f64 x;
    f64 y;
    f64 depth;
    ns_bool inside;
    ns_bool hover;
    ns_bool down;
    ns_bool pressed;
    ns_bool released;
} ui_panel_touch;

#define UI_PANEL_TOUCHES 4
// Touch distances as fractions of the panel's smaller half extent: how far in
// front a fingertip hovers, and how far back out a pressed one must come
// before it releases, so tracking jitter at the plane cannot double-click.
#define UI_PANEL_HOVER_RANGE 0.5
#define UI_PANEL_RELEASE_MARGIN 0.04
// How far past an edge, in panel half extents, a pressed touch may slide.
#define UI_PANEL_DRAG_SLACK 1.25

typedef struct ui_panel_touch_state {
    ns_bool tracked;
    ns_bool down;
    f64 depth;
} ui_panel_touch_state;

// One draw of the flush: a run of immediate commands whose vertices are
// contiguous and that sample at most one RGBA texture, or a single retained
// command. Every draw samples the one MSDF font pack.
typedef struct ui_draw {
    i32 command;          // the retained command, -1 for an immediate run
    i32 vertex_offset;
    i32 vertex_count;
    u32 batch_meta;       // a retained command's meta; its vertices carry none
    u32 rgba;             // the RGBA texture its images and bitmap glyphs sample, 0 for none
    ns_bool msdf;         // it draws MSDF text from the font pack
    ns_bool focus;        // it draws on a plane with depth of field
    u32 planes;           // those planes, one bit per plane index
} ui_draw;

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
    // The MSDF faces share one texture, the font pack (ui_font_pack): the
    // main/mono atlas on top and the Chinese one under it. Glyph vertices keep
    // face-local UVs and the shader moves them into the pack through
    // font_faces, each face's (u, v) origin and extent, so a repack leaves
    // retained text valid. The pack goes up the first time a draw samples
    // it, so a program that only draws bitmap text never holds it.
    u32 font_pack;
    ns_bool font_pack_zh;
    f32 font_faces[UI_FACES][4];
    // A replaced pack may still be read by a frame in flight.
    u32 font_pack_retired;
    i32 font_pack_retired_frames;
    // The main/mono atlas image; empty for the bundled latin_mono.png. A
    // failed load is not retried.
    char font_main_image[UI_PATH_MAX];
    ns_bool font_texture_failed;
    // The Chinese atlas is large and English text never samples it, so
    // ui_load_chinese_font keeps only its glyph table and image path; it joins
    // the pack the first time a draw uses a Chinese glyph.
    char font_zh_image[UI_PATH_MAX];
    ns_bool font_zh_texture_failed;
    // Bound only where a draw has neither the pack nor an RGBA texture to
    // give a slot it never reads.
    u32 placeholder_texture;
    // What the planned frame samples from the pack (ui_plan_draws).
    ns_bool plan_msdf;
    ns_bool plan_zh;
    u32 font_bitmap_texture;
    u32 font_bitmap_zh_texture;
    // The one shader and state every ui draw uses. The state tests the screen
    // depth (GREATER_EQUAL, no write): the 3D panel hides behind scene geometry
    // in front of it, while flat ui sits at the near plane and always passes.
    u32 shader;
    u32 render_state;
    u32 render_state_hud;
    u32 textures[UI_MAX_TEXTURES];
    i32 texture_widths[UI_MAX_TEXTURES];
    // Previews are composed on the CPU into preview_pixels, one cell each,
    // and the whole image goes up as a fresh texture on the next flush after
    // a change; the texture it replaces may still be read by a frame in
    // flight, so it is retired a few frames later.
    u8 *preview_pixels;
    ns_bool preview_dirty;
    u32 preview_texture;
    u32 preview_retired[UI_PREVIEW_RETIRED];
    i32 preview_retired_frames[UI_PREVIEW_RETIRED];
    ns_bool preview_used[UI_PREVIEW_SLOTS];
    // The source image's size, and the size of its scaled copy in the cell.
    i32 preview_widths[UI_PREVIEW_SLOTS];
    i32 preview_heights[UI_PREVIEW_SLOTS];
    i32 preview_cell_widths[UI_PREVIEW_SLOTS];
    i32 preview_cell_heights[UI_PREVIEW_SLOTS];
    i32 texture_heights[UI_MAX_TEXTURES];
    ui_rect_batch rect_batches[UI_MAX_RECT_BATCHES];
    ui_retired_buffer retired[UI_RETIRED_BUFFERS];
    u64 frame_serial;
    // Tilted-plane projections for this frame (ui_begin_plane) and the
    // screen rectangle each one is scissored to.
    f32 planes[UI_MAX_PLANES][16];
    ui_clip plane_scissors[UI_MAX_PLANES];
    i32 plane_count;
    i32 current_plane;
    // Depth of field (ui_set_plane_focus): the blur radius in ui units and the
    // sharp band the next ui_begin_plane takes, and per plane its blur, band,
    // pivot line (drawable y) and defocus per ui unit away from that line.
    f32 plane_focus[2];
    f32 plane_dof[UI_MAX_PLANES][4];
    // A plane with depth of field draws into dof_scene, and a screen-space
    // pass gathers it back onto the screen through a blue-noise lens. A
    // resized scene may still be read by a frame in flight.
    u32 dof_shader;
    u32 dof_state;
    u32 dof_scene;
    i32 dof_width;
    i32 dof_height;
    u32 dof_noise;
    ns_bool dof_failed;
    u32 dof_retired;
    i32 dof_retired_frames;
    // 3D panel (ui_set_panel). panel_clip maps the canvas's (u, v, 0, 1), with
    // u and v in -1..1, to clip space; the world frame serves touch tests.
    ns_bool panel;
    // ui_begin_overlay..ui_end_overlay is being recorded.
    ns_bool overlay;
    f32 panel_clip[16];
    f64 panel_center[3];
    f64 panel_right[3];
    f64 panel_up[3];
    f64 panel_normal[3];
    f64 panel_half_w;
    f64 panel_half_h;
    ui_panel_touch_state touches[UI_PANEL_TOUCHES];
    gpu_addr storage;
    u64 storage_capacity;
    // Where this frame's projections sit in `storage` (ui_upload_storage).
    u32 projection_offset;
    ui_draw *draws;
    i32 draw_count;
    i32 draw_capacity;
    ns_bool gpu_ready;
} ui_renderer;

typedef struct ui_gpu_root {
    f32 screen_width;
    f32 screen_height;
    f32 offset_x;
    f32 offset_y;
    u32 clip_offset;
    u32 projection_offset;
    // A retained draw's vertices carry no meta; this is theirs.
    u32 batch_meta;
    // A retained draw's first vertex. Its quads start there, wherever the
    // draw's vertices sit: an overflow batch is copied into frame storage
    // behind vertices of any count.
    u32 quad_origin;
    f32 hud_center_enable[4];
    f32 hud_right_hw[4];
    f32 hud_up_hh[4];
    f32 hud_proj[4];
    f32 hud_depth[4];
    // Draw-time x/y scale of the recorded vertices before the offset; z set
    // means vertices without a clip of their own use batch_clip; w is
    // framebuffer pixels per ui unit.
    f32 vertex_scale[4];
    // Drawable-space clip (x0, y0, x1, y1) of a retained batch draw. A batch
    // binds its own vertex buffer, so it cannot index the frame's clip list.
    f32 batch_clip[4];
    // Per MSDF face its (u, v) origin and (u, v) extent in the font pack.
    f32 font_faces[UI_FACES][4];
    // A retained draw's projection words: the matrix and, for a plane, its
    // framebuffer rectangle. Its own vertex buffer is bound in place of the
    // frame storage that holds them for every other draw.
    f32 projection[UI_PROJECTION_WORDS];
} ui_gpu_root;

_Static_assert(sizeof(ui_gpu_root) <= GPU_V2_ALLOC_ALIGN, "UI root must fit its frame-storage stride");

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

// One shader draws all of it. Every vertex carries its own meta word (kind,
// MSDF face, projection), so a frame's ui is one draw whatever mix of
// rectangles, text, arcs, planes and panel it holds; a retained batch, which
// keeps its own vertex buffer, and a change of RGBA texture start another.
// It samples two textures at fixed bindings: the MSDF font pack and the
// draw's RGBA texture (an atlas, an image or a bitmap face).
static const char *ui_shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct UiRoot { float screen_width; float screen_height; float offset_x; float offset_y; uint clip_offset; uint projection_offset; uint batch_meta; uint quad_origin; float4 hud_center_enable; float4 hud_right_hw; float4 hud_up_hh; float4 hud_proj; float4 hud_depth; float4 vertex_scale; float4 batch_clip; float4 font_faces[2]; float4 projection[5]; };\n"
"struct VOut { float4 pos [[position]]; float2 pixel; float2 uv; float4 col; float4 params; uint meta [[flat]]; };\n"
"static inline float ui_word(device const uint *s, uint i) { return as_type<float>(s[i]); }\n"
"vertex VOut ui_vs(uint vertex_id [[vertex_id]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]]) {\n"
"  device const uint *s = ns_storage_buffer;\n"
"  uint base = vertex_id * 10u;\n"
"  float2 pixel = float2(ui_word(s, base), ui_word(s, base + 1u)) * ns_root.vertex_scale.xy + float2(ns_root.offset_x, ns_root.offset_y);\n"
"  float2 uv = float2(ui_word(s, base + 2u), ui_word(s, base + 3u));\n"
"  uint color = s[base + 4u];\n"
"  float4 params = float4(ui_word(s, base + 5u), ui_word(s, base + 6u), ui_word(s, base + 7u), ui_word(s, base + 8u));\n"
"  uint meta = s[base + 9u];\n"
"  if (ns_root.vertex_scale.z > 0.5) { meta = ns_root.batch_meta; if (params.w <= 0.0) { params.w = -1.0; } }\n"
"  if ((meta & 15u) == 5u) {\n"
"    // A rectangle's corners are its quad's vertices 0 and 2. An axis under one\n"
"    // framebuffer pixel becomes a one-pixel box carrying the lost area as\n"
"    // alpha. Every filtered axis (all of a smooth rectangle's) is padded so\n"
"    // each pixel it touches is shaded; uv and params.xy hand the box to the\n"
"    // fragment stage, which filters only those axes.\n"
"    uint q = (ns_root.quad_origin + (vertex_id - ns_root.quad_origin) / 6u * 6u) * 10u;\n"
"    float2 lo = float2(ui_word(s, q), ui_word(s, q + 1u)) * ns_root.vertex_scale.xy + float2(ns_root.offset_x, ns_root.offset_y);\n"
"    float2 hi = float2(ui_word(s, q + 20u), ui_word(s, q + 21u)) * ns_root.vertex_scale.xy + float2(ns_root.offset_x, ns_root.offset_y);\n"
"    float2 unit = float2(1.0 / max(ns_root.vertex_scale.w, 0.001));\n"
"    float2 center = (lo + hi) * 0.5; float2 size = hi - lo; bool2 thin = size < unit;\n"
"    bool smooth = params.x > 0.5; bool2 soft = bool2(thin.x || smooth, thin.y || smooth);\n"
"    float2 box = select(size, unit, thin);\n"
"    float2 side = select(float2(-1.0), float2(1.0), pixel > center);\n"
"    pixel = center + side * (box * 0.5 + select(float2(0.0), unit, soft));\n"
"    params.xy = select(float2(-1e30), center - box * 0.5, soft); uv = select(float2(1e30), center + box * 0.5, soft);\n"
"    params.z = (size.x / box.x) * (size.y / box.y);\n"
"    // A smooth rectangle shrinking under two pixels tall averages into the gap\n"
"    // beside it and dims; up to double its coverage keeps the fill's brightness.\n"
"    if (smooth) { params.z *= 1.0 + clamp((2.0 - size.y / unit.y) / 1.5, 0.0, 1.0); }\n"
"  }\n"
"  float2 screen = float2(ns_root.screen_width, ns_root.screen_height);\n"
"  float2 ndc = float2((pixel.x / screen.x) * 2.0 - 1.0, 1.0 - (pixel.y / screen.y) * 2.0);\n"
"  float4 pos = float4(ndc, 1.0, 1.0);\n"
"  uint projection = (meta >> 8u) & 15u;\n"
"  if (projection > 0u) {\n"
"    uint p = ns_root.projection_offset / 4u + (projection - 1u) * 20u;\n"
"    float4x4 m = float4x4(float4(ui_word(s, p), ui_word(s, p + 1u), ui_word(s, p + 2u), ui_word(s, p + 3u)),\n"
"                          float4(ui_word(s, p + 4u), ui_word(s, p + 5u), ui_word(s, p + 6u), ui_word(s, p + 7u)),\n"
"                          float4(ui_word(s, p + 8u), ui_word(s, p + 9u), ui_word(s, p + 10u), ui_word(s, p + 11u)),\n"
"                          float4(ui_word(s, p + 12u), ui_word(s, p + 13u), ui_word(s, p + 14u), ui_word(s, p + 15u)));\n"
"    // A retained draw's storage is its own vertices; the root carries its projection.\n"
"    if (ns_root.vertex_scale.z > 0.5) { m = float4x4(ns_root.projection[0], ns_root.projection[1], ns_root.projection[2], ns_root.projection[3]); }\n"
"    pos = m * float4(ndc, 0.0, 1.0);\n"
"    if (projection != 9u) { pos.z = pos.w; }\n"
"  }\n"
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
"  o.params = params; o.meta = meta; return o;\n"
"}\n"
"static inline half ui_median3(half r, half g, half b) { return max(min(r, g), min(max(r, g), b)); }\n"
"static inline bool ui_clip_discard(VOut in, constant UiRoot &ns_root, device const uint *s) {\n"
"  uint projection = (in.meta >> 8u) & 15u;\n"
"  if (projection > 0u && projection < 9u) {\n"
"    uint p = ns_root.projection_offset / 4u + (projection - 1u) * 20u + 16u;\n"
"    float4 b = ns_root.vertex_scale.z > 0.5 ? ns_root.projection[4] : float4(ui_word(s, p), ui_word(s, p + 1u), ui_word(s, p + 2u), ui_word(s, p + 3u));\n"
"    if (in.pos.x < b.x || in.pos.y < b.y || in.pos.x >= b.z || in.pos.y >= b.w) { return true; }\n"
"  }\n"
"  if (in.params.w < -0.5) { float4 b = ns_root.batch_clip; return in.pixel.x < b.x || in.pixel.y < b.y || in.pixel.x >= b.z || in.pixel.y >= b.w; }\n"
"  uint clip_idx = uint(round(max(in.params.w, 0.0)));\n"
"  if (clip_idx == 0u) { return false; }\n"
"  uint base = ns_root.clip_offset / 4u + (clip_idx - 1u) * 4u;\n"
"  float4 c = float4(ui_word(s, base), ui_word(s, base + 1u), ui_word(s, base + 2u), ui_word(s, base + 3u));\n"
"  return in.pixel.x < c.x || in.pixel.y < c.y || in.pixel.x >= c.z || in.pixel.y >= c.w;\n"
"}\n"
"static inline float ui_arc_distance(VOut in) {\n"
"  float radius = max(abs(in.params.x), 0.0001); float half_width = max(in.params.y, 0.0);\n"
"  float half_angle = clamp(in.params.z, 0.0, 3.14159265); float radial = length(in.uv);\n"
"  float angle = atan2(in.uv.y, in.uv.x); float half_arc = radius * half_angle;\n"
"  float corner = min(half_width * 0.44, half_arc * 0.48);\n"
"  float2 extent = max(float2(half_arc, half_width) - float2(corner), float2(0.0));\n"
"  float2 q = abs(float2(angle * radius, radial - radius)) - extent;\n"
"  return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - corner;\n"
"}\n"
"fragment float4 ui_fs(VOut in [[stage_in]], constant UiRoot &ns_root [[buffer(0)]], device const uint *ns_storage_buffer [[buffer(3)]], texture2d<float> ns_texture_table_msdf [[texture(3)]], texture2d<float> ns_texture_table_rgba [[texture(4)]]) {\n"
"  // Derivatives first, outside every branch: the fragments of one 2x2 quad\n"
"  // can belong to primitives of different kinds in the same draw.\n"
"  float2 uv_dx = dfdx(in.uv); float2 uv_dy = dfdy(in.uv);\n"
"  float arc_distance = ui_arc_distance(in); float arc_aa = max(fwidth(arc_distance), 0.35);\n"
"  float2 pixel_fw = max(fwidth(in.pixel), float2(1e-6));\n"
"  if (ui_clip_discard(in, ns_root, ns_storage_buffer)) { discard_fragment(); }\n"
"  uint kind = in.meta & 15u;\n"
"  if (kind == 5u) {\n"
"    // The share of this pixel's footprint the box covers, per filtered axis.\n"
"    float2 cover = clamp((min(in.pixel + pixel_fw * 0.5, in.uv) - max(in.pixel - pixel_fw * 0.5, in.params.xy)) / pixel_fw, 0.0, 1.0);\n"
"    return float4(in.col.rgb, in.col.a * min(in.params.z * cover.x * cover.y, 1.0));\n"
"  }\n"
"  if (kind == 2u) {\n"
"    float opacity = 1.0 - smoothstep(-arc_aa, arc_aa, arc_distance);\n"
"    if (in.params.x < 0.0) { opacity = arc_distance <= 0.0 ? 1.0 : 0.0; }\n"
"    return float4(in.col.rgb, in.col.a * opacity);\n"
"  }\n"
"  if (kind == 4u) { return in.col; }\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
"  if (kind == 1u) {\n"
"    // The glyph's face-local uv, moved into the font pack.\n"
"    float4 face = ns_root.font_faces[((in.meta >> 4u) & 15u) == 1u ? 1 : 0];\n"
"    float4 sample = ns_texture_table_msdf.sample(samp, face.xy + in.uv * face.zw, gradient2d(uv_dx * face.zw, uv_dy * face.zw));\n"
"    half sd = ui_median3(half(sample.r), half(sample.g), half(sample.b));\n"
"    float2 tex_size = float2(ns_texture_table_msdf.get_width(), ns_texture_table_msdf.get_height()) * face.zw; float range = max(in.params.x, 0.5);\n"
"    float2 unit_range = float2(range) / tex_size; float2 screen_texel = max(abs(uv_dx) + abs(uv_dy), float2(1e-6));\n"
"    float px_range = max(0.5 * dot(unit_range, 1.0 / screen_texel), 1.0);\n"
"    float opacity = clamp(((float(sd) - 0.5) * px_range + in.params.y) / max(in.params.z, 1.0) + 0.5, 0.0, 1.0);\n"
"    return float4(in.col.rgb, in.col.a * opacity);\n"
"  }\n"
"  if (kind == 3u) {\n"
"    constexpr sampler point_samp(mag_filter::nearest, min_filter::nearest, mip_filter::none, address::clamp_to_edge);\n"
"    float4 texel = ns_texture_table_rgba.sample(point_samp, in.uv, level(0.0)); float coverage = min(texel.r, texel.a);\n"
"    return float4(in.col.rgb, in.col.a * coverage);\n"
"  }\n"
"  return ns_texture_table_rgba.sample(samp, in.uv, gradient2d(uv_dx, uv_dy)) * in.col;\n"
"}\n";

// Vulkan/GLSL 450 form of the same renderer. The backend compiles each stage
// with entry `main`, so the vertex and the fragment stage each get a source of
// their own, built from the shared root and storage declarations. The two
// textures are the first two entries of the backend's texture table.
#define UI_SHADER_GLSL_CAPACITY 16384

static const char *ui_shader_glsl_common =
    "#version 450\n"
    "layout(set = 0, binding = 2, std140) uniform UiRoot {\n"
    "    float screen_width;\n"
    "    float screen_height;\n"
    "    float offset_x;\n"
    "    float offset_y;\n"
    "    uint clip_offset;\n"
    "    uint projection_offset;\n"
    "    uint batch_meta;\n"
    "    uint quad_origin;\n"
    "    vec4 hud_center_enable;\n"
    "    vec4 hud_right_hw;\n"
    "    vec4 hud_up_hh;\n"
    "    vec4 hud_proj;\n"
    "    vec4 hud_depth;\n"
    "    vec4 vertex_scale;\n"
    "    vec4 batch_clip;\n"
    "    vec4 font_faces[2];\n"
    "    vec4 projection[5];\n"
    "} ns_root;\n"
    "layout(set = 0, binding = 8, std430) readonly buffer UiStorage { uint values[]; } ns_storage_buffer_0_block;\n"
    "#define ns_storage_buffer_0 ns_storage_buffer_0_block.values\n"
    "float ui_word(uint i) { return uintBitsToFloat(ns_storage_buffer_0[i]); }\n";

static const char *ui_shader_glsl_vs_body =
    "layout(location = 0) out vec2 ns_pixel;\n"
    "layout(location = 1) out vec2 ns_uv;\n"
    "layout(location = 2) out vec4 ns_col;\n"
    "layout(location = 3) out vec4 ns_params;\n"
    "layout(location = 4) flat out uint ns_meta;\n"
    "void main() {\n"
    "    uint base = uint(gl_VertexIndex) * 10u;\n"
    "    vec2 pixel = vec2(ui_word(base), ui_word(base + 1u)) * ns_root.vertex_scale.xy + vec2(ns_root.offset_x, ns_root.offset_y);\n"
    "    vec2 uv = vec2(ui_word(base + 2u), ui_word(base + 3u));\n"
    "    uint color = ns_storage_buffer_0[base + 4u];\n"
    "    vec4 params = vec4(ui_word(base + 5u), ui_word(base + 6u), ui_word(base + 7u), ui_word(base + 8u));\n"
    "    uint meta = ns_storage_buffer_0[base + 9u];\n"
    "    if (ns_root.vertex_scale.z > 0.5) { meta = ns_root.batch_meta; if (params.w <= 0.0) { params.w = -1.0; } }\n"
    "    if ((meta & 15u) == 5u) {\n"
    "        // A coverage rectangle: see the Metal source.\n"
    "        uint q = (ns_root.quad_origin + (uint(gl_VertexIndex) - ns_root.quad_origin) / 6u * 6u) * 10u;\n"
    "        vec2 lo = vec2(ui_word(q), ui_word(q + 1u)) * ns_root.vertex_scale.xy + vec2(ns_root.offset_x, ns_root.offset_y);\n"
    "        vec2 hi = vec2(ui_word(q + 20u), ui_word(q + 21u)) * ns_root.vertex_scale.xy + vec2(ns_root.offset_x, ns_root.offset_y);\n"
    "        vec2 unit = vec2(1.0 / max(ns_root.vertex_scale.w, 0.001));\n"
    "        vec2 center = (lo + hi) * 0.5;\n"
    "        vec2 size = hi - lo;\n"
    "        bvec2 thin = lessThan(size, unit);\n"
    "        bool smooth_rect = params.x > 0.5;\n"
    "        bvec2 soft = bvec2(thin.x || smooth_rect, thin.y || smooth_rect);\n"
    "        vec2 box = mix(size, unit, thin);\n"
    "        vec2 side = mix(vec2(-1.0), vec2(1.0), greaterThan(pixel, center));\n"
    "        pixel = center + side * (box * 0.5 + mix(vec2(0.0), unit, soft));\n"
    "        params.xy = mix(vec2(-1e30), center - box * 0.5, soft);\n"
    "        uv = mix(vec2(1e30), center + box * 0.5, soft);\n"
    "        params.z = (size.x / box.x) * (size.y / box.y);\n"
    "        if (smooth_rect) { params.z *= 1.0 + clamp((2.0 - size.y / unit.y) / 1.5, 0.0, 1.0); }\n"
    "    }\n"
    "    vec2 screen = vec2(ns_root.screen_width, ns_root.screen_height);\n"
    "    vec2 ndc = vec2((pixel.x / screen.x) * 2.0 - 1.0, 1.0 - (pixel.y / screen.y) * 2.0);\n"
    "    vec4 pos = vec4(ndc, 1.0, 1.0);\n"
    "    uint projection = (meta >> 8u) & 15u;\n"
    "    if (projection > 0u) {\n"
    "        uint p = ns_root.projection_offset / 4u + (projection - 1u) * 20u;\n"
    "        mat4 m = mat4(vec4(ui_word(p), ui_word(p + 1u), ui_word(p + 2u), ui_word(p + 3u)),\n"
    "                      vec4(ui_word(p + 4u), ui_word(p + 5u), ui_word(p + 6u), ui_word(p + 7u)),\n"
    "                      vec4(ui_word(p + 8u), ui_word(p + 9u), ui_word(p + 10u), ui_word(p + 11u)),\n"
    "                      vec4(ui_word(p + 12u), ui_word(p + 13u), ui_word(p + 14u), ui_word(p + 15u)));\n"
    "        if (ns_root.vertex_scale.z > 0.5) { m = mat4(ns_root.projection[0], ns_root.projection[1], ns_root.projection[2], ns_root.projection[3]); }\n"
    "        pos = m * vec4(ndc, 0.0, 1.0);\n"
    "        if (projection != 9u) { pos.z = pos.w; }\n"
    "    }\n"
    "    if (ns_root.hud_center_enable.w > 0.5) {\n"
    "        vec3 view = ns_root.hud_center_enable.xyz + ns_root.hud_right_hw.xyz * (ndc.x * ns_root.hud_right_hw.w) + ns_root.hud_up_hh.xyz * (ndc.y * ns_root.hud_up_hh.w);\n"
    "        float z = max(view.z, 0.001);\n"
    "        float clip_x = view.x * ns_root.hud_proj.x - z * ns_root.hud_proj.z;\n"
    "        float clip_y = view.y * ns_root.hud_proj.y - z * ns_root.hud_proj.w;\n"
    "        float ndc_z = clamp(0.0 - ns_root.hud_depth.x + ns_root.hud_depth.y / z, 0.0, 1.0);\n"
    "        pos = vec4(clip_x, clip_y, ndc_z * z, z);\n"
    "    }\n"
    "    ns_pixel = pixel;\n"
    "    ns_uv = uv;\n"
    "    ns_col = vec4(float((color >> 0u) & 255u), float((color >> 8u) & 255u), float((color >> 16u) & 255u), float((color >> 24u) & 255u)) / 255.0;\n"
    "    ns_params = params;\n"
    "    ns_meta = meta;\n"
    "    gl_Position = pos;\n"
    "}\n";

static const char *ui_shader_glsl_fs_prelude =
    "layout(set = 0, binding = 5) uniform sampler2D ns_texture_table[2];\n"
    "layout(location = 0) in vec2 ns_pixel;\n"
    "layout(location = 1) in vec2 ns_uv;\n"
    "layout(location = 2) in vec4 ns_col;\n"
    "layout(location = 3) in vec4 ns_params;\n"
    "layout(location = 4) flat in uint ns_meta;\n"
    "layout(location = 0) out vec4 ns_frag_color;\n";

static const char *ui_shader_glsl_fs_body =
    "bool ui_clip_discard() {\n"
    "    uint projection = (ns_meta >> 8u) & 15u;\n"
    "    if (projection > 0u && projection < 9u) {\n"
    "        uint p = ns_root.projection_offset / 4u + (projection - 1u) * 20u + 16u;\n"
    "        vec4 b = ns_root.vertex_scale.z > 0.5 ? ns_root.projection[4] : vec4(ui_word(p), ui_word(p + 1u), ui_word(p + 2u), ui_word(p + 3u));\n"
    "        if (gl_FragCoord.x < b.x || gl_FragCoord.y < b.y || gl_FragCoord.x >= b.z || gl_FragCoord.y >= b.w) { return true; }\n"
    "    }\n"
    "    if (ns_params.w < -0.5) { vec4 b = ns_root.batch_clip; return ns_pixel.x < b.x || ns_pixel.y < b.y || ns_pixel.x >= b.z || ns_pixel.y >= b.w; }\n"
    "    uint clip_idx = uint(round(max(ns_params.w, 0.0)));\n"
    "    if (clip_idx == 0u) { return false; }\n"
    "    uint base = ns_root.clip_offset / 4u + (clip_idx - 1u) * 4u;\n"
    "    vec4 c = vec4(ui_word(base), ui_word(base + 1u), ui_word(base + 2u), ui_word(base + 3u));\n"
    "    return ns_pixel.x < c.x || ns_pixel.y < c.y || ns_pixel.x >= c.z || ns_pixel.y >= c.w;\n"
    "}\n"
    "float ui_arc_distance() {\n"
    "    float radius = max(abs(ns_params.x), 0.0001);\n"
    "    float half_width = max(ns_params.y, 0.0);\n"
    "    float half_angle = clamp(ns_params.z, 0.0, 3.14159265);\n"
    "    float radial = length(ns_uv);\n"
    "    float angle = atan(ns_uv.y, ns_uv.x);\n"
    "    float half_arc = radius * half_angle;\n"
    "    float corner = min(half_width * 0.44, half_arc * 0.48);\n"
    "    vec2 extent = max(vec2(half_arc, half_width) - vec2(corner), vec2(0.0));\n"
    "    vec2 q = abs(vec2(angle * radius, radial - radius)) - extent;\n"
    "    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - corner;\n"
    "}\n"
    "void main() {\n"
    "    // Derivatives first, outside every branch: the fragments of one 2x2\n"
    "    // quad can belong to primitives of different kinds in the same draw.\n"
    "    vec2 uv_dx = dFdx(ns_uv);\n"
    "    vec2 uv_dy = dFdy(ns_uv);\n"
    "    float arc_distance = ui_arc_distance();\n"
    "    float arc_aa = max(fwidth(arc_distance), 0.35);\n"
    "    vec2 pixel_fw = max(fwidth(ns_pixel), vec2(1e-6));\n"
    "    if (ui_clip_discard()) { discard; }\n"
    "    uint kind = ns_meta & 15u;\n"
    "    if (kind == 5u) {\n"
    "        vec2 cover = clamp((min(ns_pixel + pixel_fw * 0.5, ns_uv) - max(ns_pixel - pixel_fw * 0.5, ns_params.xy)) / pixel_fw, 0.0, 1.0);\n"
    "        ns_frag_color = vec4(ns_col.rgb, ns_col.a * min(ns_params.z * cover.x * cover.y, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    if (kind == 2u) {\n"
    "        float opacity = 1.0 - smoothstep(-arc_aa, arc_aa, arc_distance);\n"
    "        if (ns_params.x < 0.0) { opacity = arc_distance <= 0.0 ? 1.0 : 0.0; }\n"
    "        ns_frag_color = vec4(ns_col.rgb, ns_col.a * opacity);\n"
    "        return;\n"
    "    }\n"
    "    if (kind == 4u) {\n"
    "        ns_frag_color = ns_col;\n"
    "        return;\n"
    "    }\n"
    "    if (kind == 1u) {\n"
    "        // The glyph's face-local uv, moved into the font pack.\n"
    "        vec4 face = ns_root.font_faces[((ns_meta >> 4u) & 15u) == 1u ? 1 : 0];\n"
    "        vec4 s = textureGrad(ns_texture_table[0], face.xy + ns_uv * face.zw, uv_dx * face.zw, uv_dy * face.zw);\n"
    "        float sd = max(min(s.r, s.g), min(max(s.r, s.g), s.b));\n"
    "        float range = max(ns_params.x, 0.5);\n"
    "        vec2 unit_range = vec2(range) / (vec2(textureSize(ns_texture_table[0], 0)) * face.zw);\n"
    "        vec2 screen_texel = max(abs(uv_dx) + abs(uv_dy), vec2(1e-6));\n"
    "        float px_range = max(0.5 * dot(unit_range, 1.0 / screen_texel), 1.0);\n"
    "        float opacity = clamp(((sd - 0.5) * px_range + ns_params.y) / max(ns_params.z, 1.0) + 0.5, 0.0, 1.0);\n"
    "        ns_frag_color = vec4(ns_col.rgb, ns_col.a * opacity);\n"
    "        return;\n"
    "    }\n"
    "    if (kind == 3u) {\n"
    "        ivec2 size = textureSize(ns_texture_table[1], 0);\n"
    "        ivec2 pixel = clamp(ivec2(ns_uv * vec2(size)), ivec2(0), size - ivec2(1));\n"
    "        vec4 texel = texelFetch(ns_texture_table[1], pixel, 0);\n"
    "        ns_frag_color = vec4(ns_col.rgb, ns_col.a * min(texel.r, texel.a));\n"
    "        return;\n"
    "    }\n"
    "    ns_frag_color = textureGrad(ns_texture_table[1], ns_uv, uv_dx, uv_dy) * ns_col;\n"
    "}\n";

static void ui_glsl_append(char *out, size_t capacity, size_t *used, const char *format, ...) {
    if (*used >= capacity) return;
    va_list args;
    va_start(args, format);
    const i32 written = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (written > 0) *used += (size_t)written;
}

static ns_bool ui_shader_glsl_vertex(char *out, size_t capacity) {
    size_t used = 0;
    ui_glsl_append(out, capacity, &used, "%s%s", ui_shader_glsl_common, ui_shader_glsl_vs_body);
    return used < capacity;
}

static ns_bool ui_shader_glsl_fragment(char *out, size_t capacity) {
    size_t used = 0;
    ui_glsl_append(out, capacity, &used, "%s%s%s", ui_shader_glsl_common, ui_shader_glsl_fs_prelude, ui_shader_glsl_fs_body);
    return used < capacity;
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
    font->cap_top = round(font->font_size * 0.1);
    font->baseline = round(font->font_size * 0.8);
    for (u32 i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        ui_glyph *g = ui_font_glyph(font, refs[i]);
        if (g && g->height > 0) {
            font->cap_top = g->y_offset;
            font->baseline = g->y_offset + g->height;
            break;
        }
    }
    font->middle = (font->cap_top + font->baseline) * 0.5;
    ui_glyph *ideograph = ui_font_glyph(font, 0x4E2D);
    if (ideograph && ideograph->height > 0) font->middle = ideograph->y_offset + ideograph->height * 0.5;
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

static u32 ui_create_rgba_texture(const void *data, i32 width, i32 height, const char *name) {
    if (!data || width <= 0 || height <= 0) return 0;
    u32 texture = gpu_texture_create(width, height, 1, PIXELFORMAT_RGBA8,
                                     TEXTURE_USAGE_READ, 1, TEXTURE_2D, name);
    if (texture) {
        gpu_texture_upload(texture, 0, 0, data,
                           (u64)(size_t)width * (u64)(size_t)height * 4u);
    }
    return texture;
}

// Drop the font pack so the next draw that samples it builds it again; a
// frame in flight keeps the old one a few frames longer.
static void ui_font_pack_retire(ui_renderer *r) {
    if (!r->font_pack) return;
    if (r->font_pack_retired) gpu_texture_destroy(r->font_pack_retired);
    r->font_pack_retired = r->font_pack;
    r->font_pack_retired_frames = 4;
    r->font_pack = 0;
    r->font_pack_zh = false;
}

static u32 ui_create_font_texture(io_image *image, const char *name) {
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
    u32 texture = ui_create_rgba_texture(rgba, image->width, image->height, name);
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

// Every immediate vertex belongs to a command: a flush draws whole runs of
// them, so vertices no command takes are taken back.
static void ui_drop_vertices(ui_renderer *r, i32 base, i32 count) {
    if (base + count == r->vertex_count) r->vertex_count = base;
}

static void ui_emit_command(ui_renderer *r, i32 base, i32 count, i32 kind) {
    ui_clip c = ui_current_clip(r);
    if (c.w <= 0 || c.h <= 0) {
        ui_drop_vertices(r, base, count);
        return;
    }
    // The scissor is a drawable-space rectangle.
    c.x += r->safe_rect.x;
    c.y += r->safe_rect.y;
    ui_command *cmd = NULL;
    if (r->command_count > 0) {
        cmd = &r->commands[r->command_count - 1];
        // Clips travel per vertex, so they never split a command.
        if (cmd->rect_batch_id == 0 && !cmd->retained &&
            cmd->plane == r->current_plane &&
            cmd->overlay == r->overlay &&
            cmd->vertex_offset + cmd->vertex_count == base &&
            cmd->texture_id == r->current_texture_id &&
            cmd->kind == kind) {
            cmd->vertex_count += count;
            return;
        }
    }
    if (r->command_count >= UI_MAX_COMMANDS) {
        ui_drop_vertices(r, base, count);
        return;
    }

    cmd = &r->commands[r->command_count++];
    *cmd = (ui_command){
        .vertex_offset = base,
        .vertex_count = count,
        .texture_id = r->current_texture_id,
        .kind = kind,
        .rect_batch_id = 0,
        .plane = r->current_plane,
        .overlay = r->overlay,
        .clip_x = (i32)floor(c.x),
        .clip_y = (i32)floor(c.y),
        .clip_w = (i32)ceil(c.w),
        .clip_h = (i32)ceil(c.h),
    };
}

static ns_bool ui_push_vertex(ui_renderer *r, f64 x, f64 y, f64 u, f64 v, u32 color, f64 range, f64 weight, f64 softness, f64 clip) {
    if (r->vertex_count >= r->vertex_capacity) {
        // A dense frame grows the CPU vertex array instead of silently
        // dropping every draw after the limit; the GPU storage follows it.
        if (r->vertex_capacity > 268435455) return false;
        i32 capacity = r->vertex_capacity > 0 ? r->vertex_capacity * 2 : UI_INITIAL_VERTEX_CAP;
        ui_vertex *vertices = (ui_vertex*)realloc(r->vertices, (size_t)capacity * sizeof(ui_vertex));
        if (!vertices) return false;
        r->vertices = vertices;
        r->vertex_capacity = capacity;
    }
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
    // Shared edges in a triangle mesh must stay fully covered. In particular,
    // zero feather must not turn into the default fade on every internal edge.
    f64 f = fmax(0.0, feather);
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
    // The radius sign selects hard coverage without changing the vertex ABI.
    const f64 shader_radius = feather > 0.0 ? radius : -radius;
    for (i32 i = 0; i < 6; i++) {
        const i32 vertex = order[i];
        if (!ui_push_vertex(r, px[vertex], py[vertex], local_x[vertex], local_y[vertex], rgba,
                            shader_radius, half_width, half_angle, clip_param)) {
            r->vertex_count = base;
            return;
        }
    }
    ui_emit_command(r, base, 6, UI_KIND_ARC_SDF);
}

void ui_stroke_line(ui_renderer *r, f64 x0, f64 y0, f64 x1, f64 y1, f64 thickness, u32 rgba, f64 feather) {
    if (!r || thickness <= 0.0) return;
    f64 dx = x1 - x0;
    f64 dy = y1 - y0;
    f64 len = sqrt(dx * dx + dy * dy);
    if (len <= 0.000001) return;
    const f64 half = thickness * 0.5;
    const f64 fade = fmin(fmax(feather, 0.0) * 0.5, half);
    const f64 nx = -dy / len;
    const f64 ny = dx / len;
    const f64 inner = half - fade;
    const f64 outer = half + fade;
    r->current_texture_id = UI_WHITE_TEXTURE;
    if (inner > 0.0) {
        ui_push_tri(r, x0 + nx * inner, y0 + ny * inner, x1 + nx * inner, y1 + ny * inner,
                    x1 - nx * inner, y1 - ny * inner, 0, 0, rgba);
        ui_push_tri(r, x0 + nx * inner, y0 + ny * inner, x1 - nx * inner, y1 - ny * inner,
                    x0 - nx * inner, y0 - ny * inner, 0, 0, rgba);
    }
    if (fade <= 0.0) return;
    // Fade only the two long edges. Keep butt endpoints fully covered so a
    // straight segment can meet a rounded corner without a transparent seam.
    const u32 transparent = ui_color_alpha_mul(rgba, 0.0);
    for (i32 side = -1; side <= 1; side += 2) {
        const f64 ix = nx * inner * side, iy = ny * inner * side;
        const f64 ox = nx * outer * side, oy = ny * outer * side;
        ui_push_tri_colors(r, x0 + ix, y0 + iy, rgba, x0 + ox, y0 + oy, transparent,
                           x1 + ox, y1 + oy, transparent);
        ui_push_tri_colors(r, x0 + ix, y0 + iy, rgba, x1 + ox, y1 + oy, transparent,
                           x1 + ix, y1 + iy, rgba);
    }
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
    if (strcmp(gpu_shader_target(), "glsl") == 0) {
        static char vs_src[UI_SHADER_GLSL_CAPACITY];
        static char fs_src[UI_SHADER_GLSL_CAPACITY];
        if (ui_shader_glsl_vertex(vs_src, sizeof(vs_src)) && ui_shader_glsl_fragment(fs_src, sizeof(fs_src))) {
            r->shader = gpu_shader_graphics_create(vs_src, fs_src, "ui_vs", "ui_fs");
        }
    } else {
        r->shader = gpu_shader_graphics_create(ui_shader_src, ui_shader_src, "ui_vs", "ui_fs");
    }
    // Flat ui sits at depth 1 and the panel at its own depth, so one
    // GREATER_EQUAL test serves both: a hand in front of the panel hides it
    // and nothing hides the screen. Nothing writes depth, so layers that share
    // a plane keep painter's order.
    r->render_state = gpu_state_create(PRIMITIVE_TRIANGLES, CULL_NONE, FACE_WINDING_CCW,
                                       COMPARE_GREATER_EQUAL, false, GPU_BLEND_ALPHA, COLOR_MASK_ALL);
    // Reverse-Z compositor: write HUD depth so timewarp does not smear overlay
    // pixels as if they belonged to the world behind them.
    r->render_state_hud = gpu_state_create(PRIMITIVE_TRIANGLES, CULL_NONE, FACE_WINDING_CCW,
                                           COMPARE_ALWAYS, true, GPU_BLEND_ALPHA, COLOR_MASK_ALL);
    r->gpu_ready = r->shader && r->render_state && r->render_state_hud;
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
    // An atlas of one face (chars at the top level, no FONT_MAIN) serves as
    // both the main and the mono face.
    const ns_bool one_face = ui_find_key(json, "FONT_MAIN") == NULL;
    ns_bool loaded = one_face
                         ? ui_load_font_face(json, NULL, tex_w, tex_h, &main_font) &&
                               ui_load_font_face(json, NULL, tex_w, tex_h, &mono_font)
                         : ui_load_font_face(json, "FONT_MAIN", tex_w, tex_h, &main_font) &&
                               ui_load_font_face(json, "FONT_MONO", tex_w, tex_h, &mono_font);
    free(json);
    if (!loaded) {
        free(main_font.glyphs);
        free(mono_font.glyphs);
        free(image->data);
        free(image);
        return false;
    }

    free(image->data);
    free(image);
    if (snprintf(r->font_main_image, UI_PATH_MAX, "%s", image_path) >= UI_PATH_MAX) {
        r->font_main_image[0] = '\0';
        free(main_font.glyphs);
        free(mono_font.glyphs);
        return false;
    }
    free(r->fonts[UI_FONT_MAIN].glyphs);
    free(r->fonts[UI_FONT_MONO].glyphs);
    r->fonts[UI_FONT_MAIN] = main_font;
    r->fonts[UI_FONT_MONO] = mono_font;
    r->font_texture_failed = false;
    ui_font_pack_retire(r);
    return true;
}

ns_bool ui_load_chinese_font(ui_renderer *r, const char *json_path, const char *image_path) {
    if (!r || !json_path || !image_path) return false;
    size_t json_len = 0;
    i32 path_len = snprintf(NULL, 0, "%s", image_path);
    if (path_len <= 0 || path_len >= UI_PATH_MAX || !ui_file_readable(image_path)) return false;
    char *json = ui_read_file(json_path, &json_len);
    ns_unused(json_len);
    if (!json) return false;

    i32 tex_w = (i32)ui_json_key_number(json, "width", 0);
    i32 tex_h = (i32)ui_json_key_number(json, "height", 0);
    if (tex_w <= 0 || tex_h <= 0) {
        // No atlas size in the json: read it off the image itself.
        io_image *image = io_load_image(image_path);
        if (image) {
            tex_w = image->width;
            tex_h = image->height;
            free(image->data);
            free(image);
        }
    }
    ui_font zh_font = {0};
    ns_bool loaded = tex_w > 0 && tex_h > 0 && ui_load_font_face(json, NULL, tex_w, tex_h, &zh_font);
    free(json);
    if (!loaded) {
        free(zh_font.glyphs);
        return false;
    }

    free(r->fonts[UI_FONT_ZH].glyphs);
    r->fonts[UI_FONT_ZH] = zh_font;
    r->font_zh_texture_failed = false;
    snprintf(r->font_zh_image, UI_PATH_MAX, "%s", image_path);
    if (r->font_pack_zh) ui_font_pack_retire(r);
    return true;
}

typedef ns_bool (*ui_bitmap_face_loader)(char*, i32, i32, ui_font*);

static ns_bool ui_load_bitmap_face(ui_renderer *r, const char *json_path, const char *image_path,
                                   i32 font_index, ui_bitmap_face_loader load_face) {
    if (!r || !json_path || !image_path || !load_face || !r->shader) return false;
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

    u32 texture = ui_create_font_texture(image, gpu_texture_file_name(image_path));
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
        if (r->rect_batches[i].buffer) gpu_free(r->rect_batches[i].buffer);
    }
    for (i32 i = 0; i < UI_RETIRED_BUFFERS; i++) {
        if (r->retired[i].addr) gpu_free(r->retired[i].addr);
    }
    for (i32 i = 0; i < UI_MAX_TEXTURES; i++) {
        if (r->textures[i]) gpu_texture_destroy(r->textures[i]);
    }
    if (r->font_pack) gpu_texture_destroy(r->font_pack);
    if (r->font_pack_retired) gpu_texture_destroy(r->font_pack_retired);
    if (r->placeholder_texture) gpu_texture_destroy(r->placeholder_texture);
    if (r->preview_texture) gpu_texture_destroy(r->preview_texture);
    for (i32 i = 0; i < UI_PREVIEW_RETIRED; i++) {
        if (r->preview_retired[i]) gpu_texture_destroy(r->preview_retired[i]);
    }
    free(r->preview_pixels);
    if (r->font_bitmap_texture) gpu_texture_destroy(r->font_bitmap_texture);
    if (r->font_bitmap_zh_texture) gpu_texture_destroy(r->font_bitmap_zh_texture);
    if (r->shader) gpu_shader_destroy(r->shader);
    if (r->dof_shader) gpu_shader_destroy(r->dof_shader);
    if (r->dof_scene) gpu_texture_destroy(r->dof_scene);
    if (r->dof_retired) gpu_texture_destroy(r->dof_retired);
    if (r->dof_noise) gpu_texture_destroy(r->dof_noise);
    if (r->storage) gpu_free(r->storage);
    for (i32 i = 0; i < UI_FONT_COUNT; i++) free(r->fonts[i].glyphs);
    free(r->vertices);
    free(r->draws);
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

static void ui_retire_buffer(ui_renderer *r, gpu_addr addr) {
    if (!addr) return;
    for (i32 i = 0; i < UI_RETIRED_BUFFERS; i++) {
        if (r->retired[i].addr) continue;
        r->retired[i].addr = addr;
        r->retired[i].frames = 4;
        return;
    }
    // Every slot is waiting: the oldest frames have long retired by now.
    gpu_free(r->retired[0].addr);
    r->retired[0].addr = addr;
    r->retired[0].frames = 4;
}

static void ui_age_retired_buffers(ui_renderer *r) {
    for (i32 i = 0; i < UI_RETIRED_BUFFERS; i++) {
        if (r->retired[i].addr && --r->retired[i].frames <= 0) {
            gpu_free(r->retired[i].addr);
            r->retired[i].addr = 0;
        }
    }
    if (r->dof_retired && --r->dof_retired_frames <= 0) {
        gpu_texture_destroy(r->dof_retired);
        r->dof_retired = 0;
    }
    if (r->font_pack_retired && --r->font_pack_retired_frames <= 0) {
        gpu_texture_destroy(r->font_pack_retired);
        r->font_pack_retired = 0;
    }
    for (i32 i = 0; i < UI_PREVIEW_RETIRED; i++) {
        if (r->preview_retired[i] && --r->preview_retired_frames[i] <= 0) {
            gpu_texture_destroy(r->preview_retired[i]);
            r->preview_retired[i] = 0;
        }
    }
}

void ui_rect_batch_destroy(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch) return;
    ui_retire_buffer(r, batch->buffer);
    free(batch->vertices);
    memset(batch, 0, sizeof(*batch));
}

void ui_rect_batch_begin(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch) return;
    batch->vertex_count = 0;
    batch->kind = -1;
    batch->texture_id = UI_WHITE_TEXTURE;
    batch->dirty = true;
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

// A rectangle's `range` word marks a smooth one: the shader filters its
// coverage on both axes instead of only on an axis under one pixel.
static void ui_rect_batch_push(ui_renderer *r, i32 batch_id, f64 x, f64 y, f64 w, f64 h, u32 rgba, f32 smooth) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch || w <= 0.0 || h <= 0.0) return;
    if (batch->kind >= 0 && batch->kind != UI_KIND_IMAGE) return;
    if (!ui_rect_batch_reserve(batch, 6)) return;
    batch->kind = UI_KIND_IMAGE;
    batch->texture_id = UI_WHITE_TEXTURE;
    batch->dirty = true;
    f32 x0 = (f32)x, y0 = (f32)y, x1 = (f32)(x + w), y1 = (f32)(y + h);
    ui_vertex quad[6] = {
        {.x = x0, .y = y0, .color = rgba, .range = smooth},
        {.x = x1, .y = y0, .color = rgba, .range = smooth},
        {.x = x1, .y = y1, .color = rgba, .range = smooth},
        {.x = x0, .y = y0, .color = rgba, .range = smooth},
        {.x = x1, .y = y1, .color = rgba, .range = smooth},
        {.x = x0, .y = y1, .color = rgba, .range = smooth},
    };
    memcpy(batch->vertices + batch->vertex_count, quad, sizeof(quad));
    batch->vertex_count += 6;
}

void ui_rect_batch_add(ui_renderer *r, i32 batch_id, f64 x, f64 y, f64 w, f64 h, u32 rgba) {
    ui_rect_batch_push(r, batch_id, x, y, w, h, rgba, 0.0f);
}

void ui_rect_batch_add_smooth(ui_renderer *r, i32 batch_id, f64 x, f64 y, f64 w, f64 h, u32 rgba) {
    ui_rect_batch_push(r, batch_id, x, y, w, h, rgba, 1.0f);
}

ns_bool ui_rect_batch_append(ui_renderer *r, i32 batch_id, i32 source_id) {
    ui_rect_batch *dst = ui_rect_batch_get(r, batch_id);
    ui_rect_batch *src = ui_rect_batch_get(r, source_id);
    if (!dst || !src || dst == src) return false;
    if (!src->vertex_count) return true;
    if (dst->kind >= 0 && (dst->kind != src->kind || dst->texture_id != src->texture_id)) return false;
    if (src->vertex_count > INT32_MAX - dst->vertex_count) return false;
    if (!ui_rect_batch_reserve(dst, src->vertex_count)) return false;
    memcpy(dst->vertices + dst->vertex_count, src->vertices, (size_t)src->vertex_count * sizeof(ui_vertex));
    dst->vertex_count += src->vertex_count;
    dst->kind = src->kind;
    dst->texture_id = src->texture_id;
    dst->dirty = true;
    return true;
}

ns_bool ui_rect_batch_end(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    return batch != NULL;
}

i32 ui_rect_batch_vertex_count(ui_renderer *r, i32 batch_id) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    return batch ? batch->vertex_count : 0;
}

void ui_rect_batch_draw_range(ui_renderer *r, i32 batch_id, i32 first, i32 count,
                              f64 dx, f64 dy, f64 scale_x, f64 scale_y) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch || batch->kind < 0 || scale_x <= 0.0 || scale_y <= 0.0 ||
        r->command_count >= UI_MAX_COMMANDS) return;
    if (first < 0) first = 0;
    if (count > batch->vertex_count - first) count = batch->vertex_count - first;
    // Whole quads only, so a range never splits a glyph.
    first -= first % 6;
    count -= count % 6;
    if (count <= 0) return;
    ui_clip c = ui_current_clip(r);
    if (c.w <= 0.0 || c.h <= 0.0) return;
    batch->last_draw_frame = r->frame_serial;
    ui_command *cmd = &r->commands[r->command_count++];
    *cmd = (ui_command){
        .vertex_offset = first,
        .vertex_count = count,
        .texture_id = batch->texture_id,
        .kind = batch->kind,
        .rect_batch_id = batch_id,
        .retained = true,
        // Batch vertices are recorded once in content space, then scaled and
        // translated at draw time, so the safe-area origin rides along with
        // the draw offset.
        .offset_x = dx + r->safe_rect.x,
        .offset_y = dy + r->safe_rect.y,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .plane = r->current_plane,
        .overlay = r->overlay,
        .batch_clip = c,
        .clip_x = (i32)floor(c.x + r->safe_rect.x),
        .clip_y = (i32)floor(c.y + r->safe_rect.y),
        .clip_w = (i32)ceil(c.w),
        .clip_h = (i32)ceil(c.h),
    };
}

void ui_rect_batch_draw_scaled(ui_renderer *r, i32 batch_id, f64 dx, f64 dy, f64 scale) {
    ui_rect_batch_draw_range(r, batch_id, 0, ui_rect_batch_vertex_count(r, batch_id), dx, dy, scale, scale);
}

void ui_rect_batch_draw_at(ui_renderer *r, i32 batch_id, f64 dx, f64 dy) {
    ui_rect_batch_draw_scaled(r, batch_id, dx, dy, 1.0);
}

void ui_rect_batch_draw(ui_renderer *r, i32 batch_id) {
    ui_rect_batch_draw_scaled(r, batch_id, 0.0, 0.0, 1.0);
}

static i32 ui_register_rgba_texture(ui_renderer *r, const u8 *data, i32 width, i32 height, const char *name) {
    if (!r || !data || width <= 0 || height <= 0) return 0;
    for (i32 slot = 0; slot < UI_MAX_TEXTURES; slot++) {
        if (r->textures[slot]) continue;
        u32 texture = ui_create_rgba_texture(data, width, height, name);
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
    for (i32 slot = 0; slot < UI_MAX_TEXTURES; slot++) {
        if (r->textures[slot]) continue;
        i32 width = 0, height = 0;
        u32 texture = gpu_texture_load_cooked_info(path, &width, &height);
        if (texture) {
            r->textures[slot] = texture;
            r->texture_widths[slot] = width;
            r->texture_heights[slot] = height;
            return slot + 3;
        }
        break;
    }
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
    i32 texture_id = ui_register_rgba_texture(r, rgba, image->width, image->height, gpu_texture_file_name(path));
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

// ---- previews -----------------------------------------------------------

static ns_bool ui_preview_valid(const ui_renderer *r, i32 preview) {
    return r && preview >= 1 && preview <= UI_PREVIEW_SLOTS && r->preview_used[preview - 1];
}

// Shrink `image` into the cell of `slot`, keeping its aspect, inside a
// one-texel transparent border so linear sampling never reaches a neighbour.
// Colours are averaged by coverage so transparent texels do not darken edges.
static void ui_preview_compose(ui_renderer *r, i32 slot, const io_image *image) {
    const i32 inner = UI_PREVIEW_CELL - 2;
    const i32 sw = image->width;
    const i32 sh = image->height;
    const f64 scale = fmin(1.0, fmin((f64)inner / sw, (f64)inner / sh));
    const i32 dw = (i32)fmax(1.0, floor(sw * scale + 0.5));
    const i32 dh = (i32)fmax(1.0, floor(sh * scale + 0.5));
    const i32 cell_x = (slot % UI_PREVIEW_COLUMNS) * UI_PREVIEW_CELL;
    const i32 cell_y = (slot / UI_PREVIEW_COLUMNS) * UI_PREVIEW_CELL;
    const i32 c = image->channels;
    for (i32 y = 0; y < UI_PREVIEW_CELL; y++) {
        memset(r->preview_pixels + ((size_t)(cell_y + y) * UI_PREVIEW_SIZE + cell_x) * 4, 0, UI_PREVIEW_CELL * 4);
    }
    for (i32 y = 0; y < dh; y++) {
        const i32 sy0 = (i32)((i64)y * sh / dh);
        i32 sy1 = (i32)((i64)(y + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        u8 *row = r->preview_pixels + ((size_t)(cell_y + 1 + y) * UI_PREVIEW_SIZE + cell_x + 1) * 4;
        for (i32 x = 0; x < dw; x++) {
            const i32 sx0 = (i32)((i64)x * sw / dw);
            i32 sx1 = (i32)((i64)(x + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            f64 sum[3] = {0.0, 0.0, 0.0};
            f64 alpha = 0.0;
            for (i32 yy = sy0; yy < sy1; yy++) {
                const u8 *src = image->data + ((size_t)yy * sw + sx0) * c;
                for (i32 xx = sx0; xx < sx1; xx++, src += c) {
                    const f64 a = c == 4 ? src[3] : c == 2 ? src[1] : 255.0;
                    const u8 g = src[0];
                    sum[0] += a * (c >= 3 ? src[0] : g);
                    sum[1] += a * (c >= 3 ? src[1] : g);
                    sum[2] += a * (c >= 3 ? src[2] : g);
                    alpha += a;
                }
            }
            const f64 count = (f64)(sx1 - sx0) * (f64)(sy1 - sy0);
            u8 *out = row + (size_t)x * 4;
            if (alpha > 0.0) {
                out[0] = (u8)(sum[0] / alpha + 0.5);
                out[1] = (u8)(sum[1] / alpha + 0.5);
                out[2] = (u8)(sum[2] / alpha + 0.5);
            }
            out[3] = (u8)(alpha / count + 0.5);
        }
    }
    r->preview_widths[slot] = sw;
    r->preview_heights[slot] = sh;
    r->preview_cell_widths[slot] = dw;
    r->preview_cell_heights[slot] = dh;
}

i32 ui_preview_load(ui_renderer *r, const char *path) {
    if (!r || !path || !path[0]) return 0;
    i32 slot = -1;
    for (i32 i = 0; i < UI_PREVIEW_SLOTS; i++) {
        if (!r->preview_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;
    if (!r->preview_pixels) {
        r->preview_pixels = (u8 *)calloc((size_t)UI_PREVIEW_SIZE * UI_PREVIEW_SIZE, 4);
        if (!r->preview_pixels) return 0;
    }
    io_image *image = io_try_load_image(path);
    if (!image) return 0;
    if (image->width <= 0 || image->height <= 0 || image->channels < 1 || image->channels > 4) {
        io_image_destroy(image);
        return 0;
    }
    ui_preview_compose(r, slot, image);
    io_image_destroy(image);
    r->preview_used[slot] = true;
    r->preview_dirty = true;
    return slot + 1;
}

void ui_preview_release(ui_renderer *r, i32 preview) {
    if (!ui_preview_valid(r, preview)) return;
    // The cell keeps its pixels until a later load reuses it; nothing draws
    // a released preview, so the texture need not change now.
    r->preview_used[preview - 1] = false;
}

i32 ui_preview_width(ui_renderer *r, i32 preview) {
    return ui_preview_valid(r, preview) ? r->preview_widths[preview - 1] : 0;
}

i32 ui_preview_height(ui_renderer *r, i32 preview) {
    return ui_preview_valid(r, preview) ? r->preview_heights[preview - 1] : 0;
}

void ui_preview_draw(ui_renderer *r, i32 preview, f64 x, f64 y, f64 w, f64 h, u32 rgba) {
    if (!ui_preview_valid(r, preview) || w <= 0 || h <= 0) return;
    const i32 slot = preview - 1;
    const f64 size = UI_PREVIEW_SIZE;
    const f64 u0 = ((slot % UI_PREVIEW_COLUMNS) * UI_PREVIEW_CELL + 1) / size;
    const f64 v0 = ((slot / UI_PREVIEW_COLUMNS) * UI_PREVIEW_CELL + 1) / size;
    r->current_texture_id = UI_PREVIEW_TEXTURE;
    ui_push_quad_ex(r, x, y, x + w, y + h, u0, v0,
                    u0 + r->preview_cell_widths[slot] / size,
                    v0 + r->preview_cell_heights[slot] / size,
                    rgba, UI_KIND_IMAGE, 0, 0, 0);
}

// Upload the composed previews as a new texture after a load changed them.
static void ui_preview_sync(ui_renderer *r) {
    if (!r->preview_dirty || !r->preview_pixels) return;
    u32 texture = ui_create_rgba_texture(r->preview_pixels, UI_PREVIEW_SIZE, UI_PREVIEW_SIZE, "ui preview cells");
    if (!texture) return;
    r->preview_dirty = false;
    if (r->preview_texture) {
        i32 spare = 0;
        for (i32 i = 0; i < UI_PREVIEW_RETIRED; i++) {
            if (!r->preview_retired[i]) {
                spare = i;
                break;
            }
            if (r->preview_retired_frames[i] < r->preview_retired_frames[spare]) spare = i;
        }
        // Every slot is waiting: the oldest one's frames have retired by now.
        if (r->preview_retired[spare]) gpu_texture_destroy(r->preview_retired[spare]);
        r->preview_retired[spare] = r->preview_texture;
        r->preview_retired_frames[spare] = 4;
    }
    r->preview_texture = texture;
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
    r->frame_serial++;
    r->vertex_count = 0;
    r->command_count = 0;
    r->clip_count = 1;
    r->gpu_clip_count = 0;
    r->clips[0] = ui_surface_clip(r);
    r->current_texture_id = UI_WHITE_TEXTURE;
    r->plane_count = 0;
    r->current_plane = 0;
    r->plane_focus[0] = r->plane_focus[1] = 0.0f;
    r->overlay = false;
}

static u64 ui_align_u64(u64 value, u64 alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static ns_bool ui_upload_storage(ui_renderer *r, u32 *clip_offset, u32 *root_offset) {
    if (!r || !clip_offset || !root_offset) return false;
    // Keep a bounded cache of GPU copies. CPU batches remain retained even
    // when a project has more files than the backend's buffer-slot limit.
    i32 cached = 0;
    for (i32 i = 0; i < UI_MAX_RECT_BATCHES; i++) {
        ui_rect_batch *batch = &r->rect_batches[i];
        if (!batch->buffer) continue;
        if (r->frame_serial - batch->last_draw_frame > GPU_SWAP_BUFFER_COUNT) {
            gpu_free(batch->buffer);
            batch->buffer = 0;
            batch->buffer_size = 0;
            batch->dirty = true;
        } else {
            cached++;
        }
    }
    for (i32 i = 0; i < r->command_count; i++) {
        ui_command *cmd = &r->commands[i];
        ui_rect_batch *batch = ui_rect_batch_get(r, cmd->rect_batch_id);
        if (!batch) continue;
        const u64 size = (u64)(u32)batch->vertex_count * UI_VERTEX_STRIDE;
        if (size > batch->buffer_size && (batch->buffer || cached < UI_BATCH_GPU_CACHE)) {
            u64 capacity = batch->buffer_size > 0 ? batch->buffer_size : 4096;
            while (capacity < size) capacity *= 2;
            gpu_addr buffer = gpu_malloc(capacity, GPU_MEM_SHARED, "ui retained batch vertices");
            if (buffer) {
                if (!batch->buffer) cached++;
                ui_retire_buffer(r, batch->buffer);
                batch->buffer = buffer;
                batch->buffer_size = capacity;
            }
        }
        if (batch->buffer && size <= batch->buffer_size) {
            if (batch->dirty) {
                gpu_write(batch->buffer, batch->vertices, size);
                batch->dirty = false;
            }
            continue;
        }
        // Overflow draws share the renderer's triple-buffered frame storage.
        // Preserve local coordinates, transform and fragment clip for glyphs
        // and tilted planes; only the vertex source changes.
        if (cmd->vertex_count > INT32_MAX - r->vertex_count) return false;
        const i32 base = r->vertex_count;
        const i32 required = base + cmd->vertex_count;
        if (required > r->vertex_capacity) {
            i32 capacity = r->vertex_capacity > 0 ? r->vertex_capacity : UI_INITIAL_VERTEX_CAP;
            while (capacity < required) {
                if (capacity > INT32_MAX / 2) { capacity = required; break; }
                capacity *= 2;
            }
            ui_vertex *vertices = realloc(r->vertices, (size_t)capacity * sizeof(ui_vertex));
            if (!vertices) return false;
            r->vertices = vertices;
            r->vertex_capacity = capacity;
        }
        memcpy(r->vertices + base, batch->vertices + cmd->vertex_offset,
               (size_t)cmd->vertex_count * sizeof(ui_vertex));
        r->vertex_count += cmd->vertex_count;
        cmd->vertex_offset = base;
        cmd->rect_batch_id = 0;
    }
    u64 cursor = (u64)(u32)r->vertex_count * UI_VERTEX_STRIDE;
    cursor = ui_align_u64(cursor, 16);
    *clip_offset = (u32)cursor;
    const u64 clip_size = r->gpu_clip_count > 0
                              ? (u64)(u32)r->gpu_clip_count * sizeof(ui_gpu_clip)
                              : sizeof(ui_gpu_clip);
    cursor = ui_align_u64(cursor + clip_size, 16);
    r->projection_offset = (u32)cursor;
    cursor += (u64)UI_PROJECTION_PANEL * UI_PROJECTION_WORDS * sizeof(f32);
    *root_offset = (u32)ui_align_u64(cursor, GPU_V2_ALLOC_ALIGN);
    cursor = *root_offset + (u64)(u32)r->command_count * GPU_V2_ALLOC_ALIGN;
    const u64 required = cursor > 0 ? cursor : 16;
    if (!r->storage || required > r->storage_capacity) {
        u64 capacity = r->storage_capacity > 0 ? r->storage_capacity : 4096;
        while (capacity < required) capacity *= 2;
        gpu_addr storage = gpu_malloc(capacity, GPU_MEM_SHARED | GPU_MEM_FRAMES, "ui renderer storage");
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
    return true;
}

// An MSDF atlas image as RGBA texels, or NULL. The caller frees the image.
static io_image *ui_font_image_load(const char *path) {
    io_image *image = path && path[0] ? io_load_image(path) : NULL;
    if (image && (!image->data || image->channels != 4 || image->width <= 0 || image->height <= 0)) {
        free(image->data);
        free(image);
        image = NULL;
    }
    return image;
}

// The font pack holding the main/mono face and, once `zh` asks for it, the
// Chinese one: built on first use and again when the Chinese face first
// joins, so English text never uploads the large Chinese atlas. 0 when no
// face loads.
static u32 ui_font_pack(ui_renderer *r, ns_bool zh) {
    if (zh && (r->font_zh_texture_failed || !r->font_zh_image[0])) zh = false;
    if (r->font_pack && (r->font_pack_zh || !zh)) return r->font_pack;
    if (!r->font_pack && r->font_texture_failed && !zh) return 0;
    zh = zh || r->font_pack_zh;
    char main_path[UI_PATH_MAX];
    if (r->font_main_image[0]) snprintf(main_path, UI_PATH_MAX, "%s", r->font_main_image);
    else if (!ui_resolve_asset("latin_mono.png", main_path)) main_path[0] = '\0';
    io_image *main = r->font_texture_failed ? NULL : ui_font_image_load(main_path);
    if (!main && !r->font_texture_failed) {
        fprintf(stderr, "ui: cannot locate or load %s\n", main_path[0] ? main_path : "latin_mono.png");
        r->font_texture_failed = true;
    }
    io_image *chinese = zh ? ui_font_image_load(r->font_zh_image) : NULL;
    if (zh && !chinese) {
        fprintf(stderr, "ui: cannot load %s\n", r->font_zh_image);
        r->font_zh_texture_failed = true;
    }
    if (!main && !chinese) return r->font_pack;
    // The main face on top, the Chinese face under it after a gutter.
    const i32 main_w = main ? main->width : 0, main_h = main ? main->height : 0;
    const i32 zh_w = chinese ? chinese->width : 0, zh_h = chinese ? chinese->height : 0;
    const i32 zh_y = main && chinese ? main_h + UI_FONT_PACK_GUTTER : 0;
    const i32 width = main_w > zh_w ? main_w : zh_w;
    const i32 height = zh_y + (chinese ? zh_h : main_h);
    u8 *texels = (u8 *)calloc((size_t)width * (size_t)height, 4);
    u32 pack = 0;
    if (texels) {
        for (i32 y = 0; y < main_h; y++) {
            memcpy(texels + ((size_t)y * (size_t)width) * 4, main->data + (size_t)y * (size_t)main_w * 4, (size_t)main_w * 4);
        }
        for (i32 y = 0; y < zh_h; y++) {
            memcpy(texels + ((size_t)(zh_y + y) * (size_t)width) * 4, chinese->data + (size_t)y * (size_t)zh_w * 4, (size_t)zh_w * 4);
        }
        pack = ui_create_rgba_texture(texels, width, height, "ui msdf font pack");
        free(texels);
    }
    if (pack) {
        ui_font_pack_retire(r);
        r->font_pack = pack;
        r->font_pack_zh = chinese != NULL;
        memset(r->font_faces, 0, sizeof(r->font_faces));
        if (main) {
            r->font_faces[UI_FACE_MAIN][2] = (f32)main_w / (f32)width;
            r->font_faces[UI_FACE_MAIN][3] = (f32)main_h / (f32)height;
        }
        if (chinese) {
            r->font_faces[UI_FACE_ZH][1] = (f32)zh_y / (f32)height;
            r->font_faces[UI_FACE_ZH][2] = (f32)zh_w / (f32)width;
            r->font_faces[UI_FACE_ZH][3] = (f32)zh_h / (f32)height;
        }
    }
    if (main) { free(main->data); free(main); }
    if (chinese) { free(chinese->data); free(chinese); }
    return r->font_pack;
}

// A 1x1 texture for a slot no draw reads, made only when there is nothing
// else to bind there.
static u32 ui_placeholder_texture(ui_renderer *r) {
    if (!r->placeholder_texture) {
        u32 clear = 0;
        r->placeholder_texture = ui_create_rgba_texture(&clear, 1, 1, "ui unused slot");
    }
    return r->placeholder_texture;
}

// The RGBA texture a command samples: its atlas or image, or its bitmap face.
// 0 for a solid shape, an arc or MSDF text, which sample none of their own.
static u32 ui_command_rgba(const ui_renderer *r, const ui_command *cmd) {
    if (cmd->kind == UI_KIND_BITMAP) {
        return cmd->texture_id == UI_FONT_BITMAP_ZH_TEXTURE
                   ? r->font_bitmap_zh_texture
                   : r->font_bitmap_texture;
    }
    if (cmd->kind != UI_KIND_IMAGE || cmd->retained) return 0;
    if (cmd->texture_id == UI_PREVIEW_TEXTURE) return r->preview_texture;
    if (cmd->texture_id >= 3 && cmd->texture_id < UI_MAX_TEXTURES + 3) return r->textures[cmd->texture_id - 3];
    return 0;
}

// The kind and MSDF face a command's vertices carry in their meta. An image
// or bitmap glyph with no texture to sample is drawn solid.
static u32 ui_command_meta(const ui_command *cmd, u32 rgba) {
    if (cmd->kind == UI_KIND_MSDF) {
        const u32 face = cmd->texture_id == UI_FONT_ZH_TEXTURE ? UI_FACE_ZH : UI_FACE_MAIN;
        return (u32)UI_KIND_MSDF | (face << UI_META_FACE_SHIFT);
    }
    if (cmd->retained && cmd->kind == UI_KIND_IMAGE && cmd->texture_id == UI_WHITE_TEXTURE && !rgba) return UI_KIND_COVERAGE;
    if ((cmd->kind == UI_KIND_IMAGE || cmd->kind == UI_KIND_BITMAP) && !rgba) return UI_KIND_SOLID;
    return (u32)cmd->kind;
}

static f32 ui_dot3(const f32 a[3], const f32 b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Where the immersive overlay hangs this frame, and where the player is
// looking on it.
//
// visionOS never hands a fully immersive application a live eye vector, so the
// only continuous answer to "where is the player looking" is the head's own
// forward ray. That ray is also what an application draws its reticle on, which
// is why the panel below is not rigidly locked to the head: it keeps its own
// bearing and yields only once the ray leaves the middle of it, so the reticle
// sweeps the overlay and every control on it is reachable by turning the head.
// The panel's distance and extent still come from the compositor's own
// projection, so it covers the drawable exactly as before while it is centred.
typedef struct {
    f32 eye_right[3];
    f32 eye_up[3];
    f32 eye_forward[3];
    f32 eye_pos[3];
    f32 forward[3];
    f32 right[3];
    f32 up[3];
    f32 center[3];
    f32 half_width;
    f32 half_height;
    f32 proj_x, proj_y, proj_zx, proj_zy;
    // Compositor reverse-Z terms, in tracking metres.
    f32 depth_c, depth_q;
    // The gaze on the panel, in the same -1..1 the overlay's own NDC uses.
    f32 gaze_u, gaze_v;
} ui_hud_frame;

// The panel's bearing survives between frames and between the two eyes: both
// eyes must be handed the same panel or the overlay would not fuse. Following
// is a clamp rather than a step, so re-solving inside one frame - once for each
// eye's flush, again for each read of the gaze - leaves the panel exactly where
// the first solve of that frame put it.
static ns_bool ui_hud_anchored;
static f32 ui_hud_yaw;
static f32 ui_hud_pitch;

static f32 ui_wrap_angle(f32 angle) {
    while (angle > 3.14159265f) angle -= 6.28318531f;
    while (angle < -3.14159265f) angle += 6.28318531f;
    return angle;
}

static void ui_hud_follow(f32 head_yaw, f32 head_pitch, f32 yaw_limit, f32 pitch_limit) {
    if (!ui_hud_anchored) {
        ui_hud_yaw = head_yaw;
        ui_hud_pitch = head_pitch;
        ui_hud_anchored = true;
        return;
    }
    f32 yaw_delta = ui_wrap_angle(head_yaw - ui_hud_yaw);
    if (yaw_delta > yaw_limit) ui_hud_yaw = ui_wrap_angle(ui_hud_yaw + yaw_delta - yaw_limit);
    else if (yaw_delta < -yaw_limit) ui_hud_yaw = ui_wrap_angle(ui_hud_yaw + yaw_delta + yaw_limit);
    f32 pitch_delta = head_pitch - ui_hud_pitch;
    if (pitch_delta > pitch_limit) ui_hud_pitch += pitch_delta - pitch_limit;
    else if (pitch_delta < -pitch_limit) ui_hud_pitch += pitch_delta + pitch_limit;
    if (ui_hud_pitch > UI_HUD_PITCH_LIMIT) ui_hud_pitch = UI_HUD_PITCH_LIMIT;
    if (ui_hud_pitch < -UI_HUD_PITCH_LIMIT) ui_hud_pitch = -UI_HUD_PITCH_LIMIT;
}

static ns_bool ui_hud_solve(ui_hud_frame *hud) {
    if (!hud) return false;
    memset(hud, 0, sizeof(*hud));
    if (view_immersive_status() != 2 || view_immersive_eye() < 0) {
        // Nothing to keep a bearing for; the next space opens facing forward.
        ui_hud_anchored = false;
        return false;
    }
    hud->eye_right[0] = (f32)view_immersive_value(0);
    hud->eye_right[1] = (f32)view_immersive_value(1);
    hud->eye_right[2] = (f32)view_immersive_value(2);
    hud->eye_up[0] = (f32)view_immersive_value(4);
    hud->eye_up[1] = (f32)view_immersive_value(5);
    hud->eye_up[2] = (f32)view_immersive_value(6);
    hud->eye_forward[0] = (f32)-view_immersive_value(8);
    hud->eye_forward[1] = (f32)-view_immersive_value(9);
    hud->eye_forward[2] = (f32)-view_immersive_value(10);
    hud->eye_pos[0] = (f32)view_immersive_value(12);
    hud->eye_pos[1] = (f32)view_immersive_value(13);
    hud->eye_pos[2] = (f32)view_immersive_value(14);
    f32 head_forward[3] = { (f32)-view_immersive_value(40), (f32)-view_immersive_value(41), (f32)-view_immersive_value(42) };
    f32 head_pos[3] = { (f32)view_immersive_value(44), (f32)view_immersive_value(45), (f32)view_immersive_value(46) };
    hud->proj_x = (f32)view_immersive_value(16);
    hud->proj_y = (f32)view_immersive_value(21);
    hud->proj_zx = (f32)view_immersive_value(24);
    hud->proj_zy = (f32)view_immersive_value(25);
    hud->depth_c = (f32)view_immersive_value(26);
    hud->depth_q = (f32)view_immersive_value(30);
    if (fabsf(hud->proj_x) < 1e-5f || fabsf(hud->proj_y) < 1e-5f) return false;
    f32 head_length = ui_dot3(head_forward, head_forward);
    if (head_length < 1e-6f || ui_dot3(hud->eye_forward, hud->eye_forward) < 1e-6f) return false;
    head_length = sqrtf(head_length);
    head_forward[0] /= head_length;
    head_forward[1] /= head_length;
    head_forward[2] /= head_length;
    const f32 distance = UI_HUD_DISTANCE_METRES;
    hud->half_width = fabsf(distance / hud->proj_x);
    hud->half_height = fabsf(distance / hud->proj_y);
    if (hud->half_width < 1e-4f || hud->half_height < 1e-4f) return false;

    // Tracking space has Y up, so the panel is carried as a yaw and a pitch and
    // never rolls: a HUD that tilted with the head would be read at an angle.
    f32 head_yaw = atan2f(head_forward[0], -head_forward[2]);
    f32 head_pitch = asinf(head_forward[1] < -1.0f ? -1.0f : (head_forward[1] > 1.0f ? 1.0f : head_forward[1]));
    ui_hud_follow(head_yaw, head_pitch,
                  atanf(UI_HUD_GAZE_LIMIT * hud->half_width / distance),
                  atanf(UI_HUD_GAZE_LIMIT * hud->half_height / distance));
    f32 cos_yaw = cosf(ui_hud_yaw), sin_yaw = sinf(ui_hud_yaw);
    f32 cos_pitch = cosf(ui_hud_pitch), sin_pitch = sinf(ui_hud_pitch);
    hud->forward[0] = sin_yaw * cos_pitch;
    hud->forward[1] = sin_pitch;
    hud->forward[2] = -cos_yaw * cos_pitch;
    hud->right[0] = cos_yaw;
    hud->right[1] = 0.0f;
    hud->right[2] = sin_yaw;
    hud->up[0] = -sin_yaw * sin_pitch;
    hud->up[1] = cos_pitch;
    hud->up[2] = cos_yaw * sin_pitch;
    hud->center[0] = head_pos[0] + hud->forward[0] * distance;
    hud->center[1] = head_pos[1] + hud->forward[1] * distance;
    hud->center[2] = head_pos[2] + hud->forward[2] * distance;

    // The gaze ray leaves the head, not an eye, so both eyes agree on it.
    f32 facing = ui_dot3(head_forward, hud->forward);
    if (facing > 0.05f) {
        f32 reach = distance / facing;
        f32 offset[3] = {
            head_forward[0] * reach - hud->forward[0] * distance,
            head_forward[1] * reach - hud->forward[1] * distance,
            head_forward[2] * reach - hud->forward[2] * distance
        };
        hud->gaze_u = ui_dot3(offset, hud->right) / hud->half_width;
        hud->gaze_v = ui_dot3(offset, hud->up) / hud->half_height;
        if (hud->gaze_u < -1.0f) hud->gaze_u = -1.0f;
        if (hud->gaze_u > 1.0f) hud->gaze_u = 1.0f;
        if (hud->gaze_v < -1.0f) hud->gaze_v = -1.0f;
        if (hud->gaze_v > 1.0f) hud->gaze_v = 1.0f;
    }
    return true;
}

// The panel is solved once for a flush and copied into each draw command's
// root: it is the same panel for every triangle in the frame, and solving it
// per command would put trigonometry under a loop that runs thousands of times.
static void ui_fill_hud_root(ui_gpu_root *root, const ui_hud_frame *hud) {
    if (!root || !hud) return;
    f32 rel[3] = {
        hud->center[0] - hud->eye_pos[0],
        hud->center[1] - hud->eye_pos[1],
        hud->center[2] - hud->eye_pos[2]
    };
    root->hud_center_enable[0] = ui_dot3(rel, hud->eye_right);
    root->hud_center_enable[1] = ui_dot3(rel, hud->eye_up);
    root->hud_center_enable[2] = ui_dot3(rel, hud->eye_forward);
    root->hud_center_enable[3] = 1.0f;
    root->hud_right_hw[0] = ui_dot3(hud->right, hud->eye_right);
    root->hud_right_hw[1] = ui_dot3(hud->right, hud->eye_up);
    root->hud_right_hw[2] = ui_dot3(hud->right, hud->eye_forward);
    root->hud_right_hw[3] = hud->half_width;
    root->hud_up_hh[0] = ui_dot3(hud->up, hud->eye_right);
    root->hud_up_hh[1] = ui_dot3(hud->up, hud->eye_up);
    root->hud_up_hh[2] = ui_dot3(hud->up, hud->eye_forward);
    root->hud_up_hh[3] = hud->half_height;
    root->hud_proj[0] = hud->proj_x;
    root->hud_proj[1] = hud->proj_y;
    root->hud_proj[2] = hud->proj_zx;
    root->hud_proj[3] = hud->proj_zy;
    root->hud_depth[0] = hud->depth_c;
    root->hud_depth[1] = hud->depth_q;
}

// The gaze in the overlay's own drawable coordinates: the point an application
// draws its reticle at, and the point its pointer belongs at so a pinch lands
// on whatever the reticle is over. Both are false and the surface centre while
// no immersive HUD is up, so a caller may read them unconditionally.
ns_bool ui_hud_gaze_active(ui_renderer *r) {
    ui_hud_frame hud;
    return r != NULL && ui_hud_solve(&hud);
}

f64 ui_hud_gaze_x(ui_renderer *r) {
    if (!r) return 0.0;
    ui_hud_frame hud;
    if (!ui_hud_solve(&hud)) return r->rect.w * 0.5;
    return ((f64)hud.gaze_u + 1.0) * 0.5 * r->rect.w;
}

f64 ui_hud_gaze_y(ui_renderer *r) {
    if (!r) return 0.0;
    ui_hud_frame hud;
    if (!ui_hud_solve(&hud)) return r->rect.h * 0.5;
    return (1.0 - (f64)hud.gaze_v) * 0.5 * r->rect.h;
}

static f64 ui_dot3d(const f64 a[3], const f64 b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static ns_bool ui_unit3d(f64 v[3]) {
    f64 length = sqrt(ui_dot3d(v, v));
    if (length < 1e-9) return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// Lay the canvas out on a flat panel in 3D: its centre, its right and up
// directions and its half extents in world units, seen through view_proj (16
// floats, column-major, clip space with z in 0..1). The canvas fills the panel
// edge to edge, left to right along `right` and top to bottom against `up`.
void ui_set_panel(ui_renderer *r, f32 *view_proj,
                  f64 center_x, f64 center_y, f64 center_z,
                  f64 right_x, f64 right_y, f64 right_z,
                  f64 up_x, f64 up_y, f64 up_z,
                  f64 half_width, f64 half_height) {
    if (!r || !view_proj || half_width <= 0.0 || half_height <= 0.0) return;
    f64 right[3] = {right_x, right_y, right_z};
    f64 up[3] = {up_x, up_y, up_z};
    if (!ui_unit3d(right) || !ui_unit3d(up)) return;
    f64 normal[3] = {right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2], right[0] * up[1] - right[1] * up[0]};
    if (!ui_unit3d(normal)) return;
    // panel_clip = view_proj * (right * half_width, up * half_height, normal, centre).
    const f64 basis[4][4] = {
        {right[0] * half_width, right[1] * half_width, right[2] * half_width, 0.0},
        {up[0] * half_height, up[1] * half_height, up[2] * half_height, 0.0},
        {normal[0], normal[1], normal[2], 0.0},
        {center_x, center_y, center_z, 1.0},
    };
    for (i32 column = 0; column < 4; column++) {
        for (i32 row = 0; row < 4; row++) {
            f64 sum = 0.0;
            for (i32 k = 0; k < 4; k++) sum += (f64)view_proj[k * 4 + row] * basis[column][k];
            r->panel_clip[column * 4 + row] = (f32)sum;
        }
    }
    r->panel_center[0] = center_x;
    r->panel_center[1] = center_y;
    r->panel_center[2] = center_z;
    memcpy(r->panel_right, right, sizeof(right));
    memcpy(r->panel_up, up, sizeof(up));
    memcpy(r->panel_normal, normal, sizeof(normal));
    r->panel_half_w = half_width;
    r->panel_half_h = half_height;
    r->panel = true;
}

void ui_clear_panel(ui_renderer *r) {
    if (r) r->panel = false;
}

// Draws until ui_end_overlay stay flat on the screen, over everything the
// frame drew before, even while the renderer has a 3D panel. One renderer
// then holds both the panel and its screen overlay, and one flush draws both
// in the order they were recorded. ui_begin_frame ends an open overlay.
void ui_begin_overlay(ui_renderer *r) {
    if (r) r->overlay = true;
}

void ui_end_overlay(ui_renderer *r) {
    if (r) r->overlay = false;
}

// Tilt the canvas plane away from the viewer about the horizontal line
// through (center_x, center_y): a pinhole camera `focal` canvas units in front
// of the screen sees the plane turned by `tilt` radians, top edge receding.
// Everything drawn until ui_end_plane is laid out flat as usual and projected
// on the GPU; clips stay in plane space and the screen rectangle (x, y, w, h)
// bounds the result. A tilt of 0 is the ordinary flat canvas.
void ui_pop_clip(ui_renderer *r);

void ui_begin_plane(ui_renderer *r, f64 center_x, f64 center_y, f64 tilt, f64 focal,
                    f64 x, f64 y, f64 w, f64 h) {
    if (!r || r->plane_count >= UI_MAX_PLANES || focal <= 0.0 || r->rect.w <= 0.0 || r->rect.h <= 0.0) return;
    const f64 screen_w = r->rect.w;
    const f64 screen_h = r->rect.h;
    const f64 ncx = 2.0 * (center_x + r->safe_rect.x) / screen_w - 1.0;
    const f64 ncy = 1.0 - 2.0 * (center_y + r->safe_rect.y) / screen_h;
    // In NDC, w = depth / focal = 1 + k * (ndc_y - ncy); the plane keeps z = 0.
    const f64 k = 0.5 * screen_h * sin(tilt) / focal;
    const f64 c = cos(tilt);
    f32 *m = r->planes[r->plane_count];
    memset(m, 0, sizeof(f32) * 16);
    m[0] = 1.0f;
    m[4] = (f32)(ncx * k);
    m[5] = (f32)(ncy * k + c);
    m[7] = (f32)k;
    m[12] = (f32)(-ncx * k * ncy);
    m[13] = (f32)(ncy - ncy * ncy * k - c * ncy);
    m[15] = (f32)(1.0 - k * ncy);
    r->plane_scissors[r->plane_count] = (ui_clip){x + r->safe_rect.x, y + r->safe_rect.y, w, h};
    // On screen, 1 - 1/w of the plane is linear in y: tan(tilt) / focal per
    // ui unit from the pivot line. The composite pass derives each pixel's
    // defocus from it, so the scene needs no depth target.
    f32 *dof = r->plane_dof[r->plane_count];
    dof[0] = r->plane_focus[0];
    dof[1] = r->plane_focus[1];
    dof[2] = (f32)(center_y + r->safe_rect.y);
    dof[3] = (f32)fabs(tan(tilt) / focal);
    if (dof[3] <= 0.0f) dof[0] = 0.0f;
    r->plane_count++;
    r->current_plane = r->plane_count;
    // The plane reaches past the screen toward its horizon, so its clips
    // start unbounded rather than at the screen's edges.
    if (r->clip_count < UI_MAX_CLIPS) {
        r->clips[r->clip_count++] = (ui_clip){-1.0e7, -1.0e7, 2.0e7, 2.0e7};
    }
}

// Depth of field for the planes begun after this call in the frame, as a
// camera focused on the plane's pivot line would see it. Each pixel's defocus
// is |1 - 1/w| for its depth w on the plane: none within `band`, growing to a
// blur radius of `blur` ui units toward the horizon and the near edge. The
// plane draws into an offscreen scene that a screen-space pass gathers back
// through a lens disc turned and jittered per pixel by blue noise, so the
// blur carries the fine grain of a real lens rather than banding. A plane
// that is not tilted stays sharp and costs nothing. ui_begin_frame returns
// it to sharp.
void ui_set_plane_focus(ui_renderer *r, f64 blur, f64 band) {
    if (!r) return;
    r->plane_focus[0] = (f32)(blur > 0.0 ? blur : 0.0);
    r->plane_focus[1] = (f32)(band < 0.0 ? 0.0 : (band > 0.95 ? 0.95 : band));
}

void ui_end_plane(ui_renderer *r) {
    if (!r || r->current_plane == 0) return;
    r->current_plane = 0;
    ui_pop_clip(r);
}

// Follow one fingertip (touch 0..3) against the panel. Its world position
// lands on the canvas at (x, y); depth is its distance in front of the panel,
// toward the viewer. It hovers within reach in front of the panel, presses
// when it crosses the plane inside the canvas, stays down while it is behind
// the plane, and releases once it comes back out past a small margin or is
// lost. pressed and released hold for the one update that changed them.
ui_panel_touch *ui_panel_touch_update(ui_renderer *r, i32 touch, ns_bool tracked, f64 x, f64 y, f64 z) {
    static ui_panel_touch result;
    memset(&result, 0, sizeof(result));
    if (!r || !r->panel || touch < 0 || touch >= UI_PANEL_TOUCHES) return &result;
    ui_panel_touch_state *state = &r->touches[touch];
    f64 offset[3] = {x - r->panel_center[0], y - r->panel_center[1], z - r->panel_center[2]};
    f64 u = ui_dot3d(offset, r->panel_right) / r->panel_half_w;
    f64 v = ui_dot3d(offset, r->panel_up) / r->panel_half_h;
    f64 depth = ui_dot3d(offset, r->panel_normal);
    f64 extent = r->panel_half_w < r->panel_half_h ? r->panel_half_w : r->panel_half_h;
    result.x = (u + 1.0) * 0.5 * r->rect.w - r->safe_rect.x;
    result.y = (1.0 - v) * 0.5 * r->rect.h - r->safe_rect.y;
    result.depth = depth;
    result.inside = tracked && fabs(u) <= 1.0 && fabs(v) <= 1.0;
    ns_bool near = tracked && fabs(u) <= UI_PANEL_DRAG_SLACK && fabs(v) <= UI_PANEL_DRAG_SLACK;
    if (state->down) {
        if (!near || depth > extent * UI_PANEL_RELEASE_MARGIN) {
            state->down = false;
            result.released = true;
        }
    } else if (result.inside && state->tracked && state->depth > 0.0 && depth <= 0.0) {
        state->down = true;
        result.pressed = true;
    }
    result.down = state->down;
    result.hover = result.inside && !state->down && depth > 0.0 && depth <= extent * UI_PANEL_HOVER_RANGE;
    state->tracked = tracked;
    state->depth = depth;
    return &result;
}

// Where a command's vertices land: the panel unless the command is an
// overlay, else its tilted plane, else the flat screen. The immersive HUD
// places everything itself.
static u32 ui_command_projection(const ui_renderer *r, const ui_command *cmd, ns_bool panel, ns_bool hud) {
    if (hud) return 0;
    if (panel && !cmd->overlay) return UI_PROJECTION_PANEL;
    if (cmd->plane > 0 && cmd->plane <= r->plane_count) return (u32)cmd->plane;
    return 0;
}

// Group the frame's commands into draws and stamp each immediate vertex with
// its meta. Consecutive immediate commands share one draw however their kind,
// face, clip or projection differ; a draw ends only at a retained batch, a
// gap in the vertices, or a second RGBA texture.
static ns_bool ui_plan_draws(ui_renderer *r, ns_bool panel, ns_bool hud) {
    r->draw_count = 0;
    r->plan_msdf = false;
    r->plan_zh = false;
    if (r->command_count > r->draw_capacity) {
        ui_draw *draws = (ui_draw *)realloc(r->draws, (size_t)r->command_count * sizeof(ui_draw));
        if (!draws) return false;
        r->draws = draws;
        r->draw_capacity = r->command_count;
    }
    ui_draw *run = NULL;
    for (i32 i = 0; i < r->command_count; i++) {
        const ui_command *cmd = &r->commands[i];
        if (cmd->clip_w <= 0 || cmd->clip_h <= 0 || cmd->vertex_count <= 0) {
            run = NULL;
            continue;
        }
        const u32 projection = ui_command_projection(r, cmd, panel, hud);
        const u32 rgba = ui_command_rgba(r, cmd);
        const u32 meta = ui_command_meta(cmd, rgba) | (projection << UI_META_PROJECTION_SHIFT);
        const ns_bool msdf = cmd->kind == UI_KIND_MSDF;
        const ns_bool focus = projection > 0 && projection <= (u32)r->plane_count &&
                              r->plane_dof[projection - 1][0] > 0.0f;
        const u32 planes = focus ? 1u << (projection - 1) : 0u;
        if (msdf) {
            r->plan_msdf = true;
            if (cmd->texture_id == UI_FONT_ZH_TEXTURE) r->plan_zh = true;
        }
        if (cmd->retained) {
            r->draws[r->draw_count++] = (ui_draw){
                .command = i,
                .batch_meta = meta,
                .rgba = rgba,
                .msdf = msdf,
                .focus = focus,
                .planes = planes,
            };
            run = NULL;
            continue;
        }
        if (!run || run->vertex_offset + run->vertex_count != cmd->vertex_offset ||
            (rgba && run->rgba && rgba != run->rgba) || run->focus != focus) {
            run = &r->draws[r->draw_count++];
            *run = (ui_draw){.command = -1, .vertex_offset = cmd->vertex_offset, .focus = focus};
        }
        run->planes |= planes;
        if (rgba) run->rgba = rgba;
        if (msdf) run->msdf = true;
        run->vertex_count += cmd->vertex_count;
        ui_vertex *v = r->vertices + cmd->vertex_offset;
        for (i32 j = 0; j < cmd->vertex_count; j++) v[j].meta = meta;
    }
    return true;
}

// The planes and the panel, in the frame storage ui_upload_storage reserved
// for them. A plane's bounding rectangle is kept in framebuffer pixels, where
// the fragment stage tests it.
static void ui_upload_projections(ui_renderer *r, f64 sx, f64 sy, f32 *words) {
    memset(words, 0, sizeof(f32) * UI_PROJECTION_PANEL * UI_PROJECTION_WORDS);
    for (i32 i = 0; i < r->plane_count; i++) {
        f32 *out = words + i * UI_PROJECTION_WORDS;
        const ui_clip c = r->plane_scissors[i];
        memcpy(out, r->planes[i], 16 * sizeof(f32));
        out[16] = (f32)floor(c.x * sx);
        out[17] = (f32)floor(c.y * sy);
        out[18] = (f32)ceil((c.x + c.w) * sx);
        out[19] = (f32)ceil((c.y + c.h) * sy);
    }
    memcpy(words + (UI_PROJECTION_PANEL - 1) * UI_PROJECTION_WORDS, r->panel_clip, 16 * sizeof(f32));
    gpu_write(r->storage + r->projection_offset, words, sizeof(f32) * UI_PROJECTION_PANEL * UI_PROJECTION_WORDS);
}

// Depth of field composite. It reads the plane's offscreen scene, which holds
// premultiplied colour (straight-alpha draws over a cleared target), and
// gathers each pixel's lens disc from it: a golden-angle spiral of taps,
// turned and jittered per pixel by a tiling blue-noise mask, its tap count
// growing with the disc's area. A tap counts as far as its own blur reaches
// back to the pixel, so a sharp region does not bleed into its neighbours.
// The defocus is analytic: on a tilted plane it grows linearly with the
// distance from the pivot line.
static const char *ui_dof_shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct DofRoot { float4 rect; float4 focus; float4 size; };\n"
"struct DofOut { float4 pos [[position]]; };\n"
"vertex DofOut ui_dof_vs(uint vertex_id [[vertex_id]], constant DofRoot &ns_root [[buffer(0)]]) {\n"
"  float2 t = float2(vertex_id == 1u || vertex_id == 3u || vertex_id == 4u ? 1.0 : 0.0, vertex_id == 2u || vertex_id >= 4u ? 1.0 : 0.0);\n"
"  float2 p = mix(ns_root.rect.xy, ns_root.rect.zw, t);\n"
"  DofOut o; o.pos = float4(p.x / ns_root.size.x * 2.0 - 1.0, 1.0 - p.y / ns_root.size.y * 2.0, 0.0, 1.0); return o;\n"
"}\n"
"static inline float ui_dof_coc(float y, constant DofRoot &root) {\n"
"  float off = abs(y - root.focus.z) * root.focus.w;\n"
"  return root.focus.x * clamp((off - root.focus.y) / max(1.0 - root.focus.y, 1e-3), 0.0, 1.0);\n"
"}\n"
"fragment float4 ui_dof_fs(DofOut in [[stage_in]], constant DofRoot &ns_root [[buffer(0)]], texture2d<float> ns_texture_table_scene [[texture(3)]], texture2d<float> ns_texture_table_noise [[texture(4)]]) {\n"
"  uint2 texel = uint2(in.pos.xy);\n"
"  float4 sharp = ns_texture_table_scene.read(texel);\n"
"  float coc = ui_dof_coc(in.pos.y, ns_root);\n"
"  if (coc < 0.5) { return sharp; }\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_zero);\n"
"  float turn = ns_texture_table_noise.read(texel & 63u).r * 6.2831853;\n"
"  float jitter = ns_texture_table_noise.read((texel + uint2(32u, 21u)) & 63u).r;\n"
"  int taps = clamp(int(coc * coc * 0.25) + 8, 8, 48);\n"
"  float2 size = float2(ns_texture_table_scene.get_width(), ns_texture_table_scene.get_height());\n"
"  float4 sum = sharp; float weight = 1.0;\n"
"  for (int i = 0; i < taps; i++) {\n"
"    float radius = coc * sqrt((float(i) + jitter) / float(taps));\n"
"    float a = turn + float(i) * 2.39996323;\n"
"    float2 q = in.pos.xy + radius * float2(cos(a), sin(a));\n"
"    float w = clamp(ui_dof_coc(q.y, ns_root) - radius + 1.0, 0.0, 1.0);\n"
"    sum += ns_texture_table_scene.sample(samp, q / size, level(0.0)) * w; weight += w;\n"
"  }\n"
"  return sum / weight;\n"
"}\n";

static const char *ui_dof_shader_glsl_vs =
    "#version 450\n"
    "layout(set = 0, binding = 2, std140) uniform DofRoot { vec4 rect; vec4 focus; vec4 size; } ns_root;\n"
    "void main() {\n"
    "    uint v = uint(gl_VertexIndex);\n"
    "    vec2 t = vec2((v == 1u || v == 3u || v == 4u) ? 1.0 : 0.0, (v == 2u || v >= 4u) ? 1.0 : 0.0);\n"
    "    vec2 p = mix(ns_root.rect.xy, ns_root.rect.zw, t);\n"
    "    gl_Position = vec4(p.x / ns_root.size.x * 2.0 - 1.0, 1.0 - p.y / ns_root.size.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *ui_dof_shader_glsl_fs =
    "#version 450\n"
    "layout(set = 0, binding = 2, std140) uniform DofRoot { vec4 rect; vec4 focus; vec4 size; } ns_root;\n"
    "layout(set = 0, binding = 5) uniform sampler2D ns_texture_table[2];\n"
    "layout(location = 0) out vec4 ns_frag_color;\n"
    "float ui_dof_coc(float y) {\n"
    "    float off = abs(y - ns_root.focus.z) * ns_root.focus.w;\n"
    "    return ns_root.focus.x * clamp((off - ns_root.focus.y) / max(1.0 - ns_root.focus.y, 1e-3), 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    ivec2 texel = ivec2(gl_FragCoord.xy);\n"
    "    vec4 sharp = texelFetch(ns_texture_table[0], texel, 0);\n"
    "    float coc = ui_dof_coc(gl_FragCoord.y);\n"
    "    if (coc < 0.5) { ns_frag_color = sharp; return; }\n"
    "    float turn = texelFetch(ns_texture_table[1], texel & 63, 0).r * 6.2831853;\n"
    "    float jitter = texelFetch(ns_texture_table[1], (texel + ivec2(32, 21)) & 63, 0).r;\n"
    "    int taps = clamp(int(coc * coc * 0.25) + 8, 8, 48);\n"
    "    vec2 size = vec2(textureSize(ns_texture_table[0], 0));\n"
    "    vec4 sum = sharp;\n"
    "    float weight = 1.0;\n"
    "    for (int i = 0; i < taps; i++) {\n"
    "        float radius = coc * sqrt((float(i) + jitter) / float(taps));\n"
    "        float a = turn + float(i) * 2.39996323;\n"
    "        vec2 q = gl_FragCoord.xy + radius * vec2(cos(a), sin(a));\n"
    "        float w = clamp(ui_dof_coc(q.y) - radius + 1.0, 0.0, 1.0);\n"
    "        vec2 uv = q / size;\n"
    "        vec4 tap = (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) ? vec4(0.0) : textureLod(ns_texture_table[0], uv, 0.0);\n"
    "        sum += tap * w;\n"
    "        weight += w;\n"
    "    }\n"
    "    ns_frag_color = sum / weight;\n"
    "}\n";

typedef struct ui_dof_root {
    f32 rect[4];  // the plane's framebuffer rectangle (x0, y0, x1, y1)
    f32 focus[4]; // blur radius px, band, pivot y px, defocus per px
    f32 size[4];  // framebuffer width, height
} ui_dof_root;

#define UI_DOF_NOISE_SIZE 64
#define UI_DOF_NOISE_RADIUS 6

// Add (sign 1) or remove (-1) one point's Gaussian from the toroidal energy.
static void ui_noise_splat(f32 *energy, const f32 *kernel, i32 index, f32 sign) {
    const i32 n = UI_DOF_NOISE_SIZE;
    const i32 reach = UI_DOF_NOISE_RADIUS;
    const i32 side = 2 * reach + 1;
    const i32 x = index % n;
    const i32 y = index / n;
    for (i32 dy = -reach; dy <= reach; dy++) {
        const i32 row = ((y + dy + n) % n) * n;
        for (i32 dx = -reach; dx <= reach; dx++) {
            energy[row + (x + dx + n) % n] += sign * kernel[(dy + reach) * side + dx + reach];
        }
    }
}

// The set point with the most energy (the tightest cluster) or the empty
// pixel with the least (the largest void).
static i32 ui_noise_extreme(const f32 *energy, const u8 *bits, u8 set) {
    i32 best = -1;
    for (i32 i = 0; i < UI_DOF_NOISE_SIZE * UI_DOF_NOISE_SIZE; i++) {
        if (bits[i] != set) continue;
        if (best < 0 || (set ? energy[i] > energy[best] : energy[i] < energy[best])) best = i;
    }
    return best;
}

// A tiling blue-noise rank mask by void and cluster (Ulichney 1993): every
// value 0..255 appears equally often, and any threshold of it spreads its
// pixels evenly with no low-frequency clumps. Built once, deterministically.
static ns_bool ui_blue_noise(u8 *out) {
    const i32 count = UI_DOF_NOISE_SIZE * UI_DOF_NOISE_SIZE;
    const i32 side = 2 * UI_DOF_NOISE_RADIUS + 1;
    f32 kernel[(2 * UI_DOF_NOISE_RADIUS + 1) * (2 * UI_DOF_NOISE_RADIUS + 1)];
    for (i32 dy = 0; dy < side; dy++) {
        for (i32 dx = 0; dx < side; dx++) {
            const f32 x = (f32)(dx - UI_DOF_NOISE_RADIUS);
            const f32 y = (f32)(dy - UI_DOF_NOISE_RADIUS);
            kernel[dy * side + dx] = expf(-(x * x + y * y) / (2.0f * 1.5f * 1.5f));
        }
    }
    f32 *energy = (f32 *)calloc((size_t)count, sizeof(f32));
    f32 *seed_energy = (f32 *)malloc((size_t)count * sizeof(f32));
    u8 *bits = (u8 *)calloc((size_t)count, 1);
    u8 *seed_bits = (u8 *)malloc((size_t)count);
    i32 *rank = (i32 *)malloc((size_t)count * sizeof(i32));
    if (!energy || !seed_energy || !bits || !seed_bits || !rank) {
        free(energy); free(seed_energy); free(bits); free(seed_bits); free(rank);
        return false;
    }
    // A tenth of the pixels from a fixed generator, then the tightest
    // cluster moves into the largest void until it lands where it was.
    const i32 ones = count / 10;
    u32 state = 0x9e3779b9u;
    for (i32 placed = 0; placed < ones;) {
        state = state * 1664525u + 1013904223u;
        const i32 i = (i32)((state >> 8) % (u32)count);
        if (bits[i]) continue;
        bits[i] = 1;
        ui_noise_splat(energy, kernel, i, 1.0f);
        placed++;
    }
    for (i32 guard = 0; guard < count; guard++) {
        const i32 cluster = ui_noise_extreme(energy, bits, 1);
        bits[cluster] = 0;
        ui_noise_splat(energy, kernel, cluster, -1.0f);
        const i32 hole = ui_noise_extreme(energy, bits, 0);
        bits[hole] = 1;
        ui_noise_splat(energy, kernel, hole, 1.0f);
        if (hole == cluster) break;
    }
    memcpy(seed_bits, bits, (size_t)count);
    memcpy(seed_energy, energy, (size_t)count * sizeof(f32));
    // The seed points rank below it, the tightest cluster removed first
    // taking the highest rank; then every void filled in turn ranks above.
    for (i32 r = ones - 1; r >= 0; r--) {
        const i32 cluster = ui_noise_extreme(energy, bits, 1);
        bits[cluster] = 0;
        ui_noise_splat(energy, kernel, cluster, -1.0f);
        rank[cluster] = r;
    }
    memcpy(bits, seed_bits, (size_t)count);
    memcpy(energy, seed_energy, (size_t)count * sizeof(f32));
    for (i32 r = ones; r < count; r++) {
        const i32 hole = ui_noise_extreme(energy, bits, 0);
        bits[hole] = 1;
        ui_noise_splat(energy, kernel, hole, 1.0f);
        rank[hole] = r;
    }
    for (i32 i = 0; i < count; i++) out[i] = (u8)((rank[i] * 256) / count);
    free(energy); free(seed_energy); free(bits); free(seed_bits); free(rank);
    return true;
}

// The composite shader, the blue-noise mask and a scene the framebuffer's
// size, made the first time a plane wants depth of field. On failure the
// plane draws sharp on the screen as before.
static ns_bool ui_dof_ready(ui_renderer *r, i32 width, i32 height) {
    if (r->dof_failed || width <= 0 || height <= 0) return false;
    if (!r->dof_shader) {
        if (strcmp(gpu_shader_target(), "glsl") == 0) {
            r->dof_shader = gpu_shader_graphics_create(ui_dof_shader_glsl_vs, ui_dof_shader_glsl_fs, "ui_dof_vs", "ui_dof_fs");
        } else {
            r->dof_shader = gpu_shader_graphics_create(ui_dof_shader_src, ui_dof_shader_src, "ui_dof_vs", "ui_dof_fs");
        }
        r->dof_state = gpu_state_create(PRIMITIVE_TRIANGLES, CULL_NONE, FACE_WINDING_CCW,
                                        COMPARE_ALWAYS, false, GPU_BLEND_PREMULTIPLIED, COLOR_MASK_ALL);
        u8 noise[UI_DOF_NOISE_SIZE * UI_DOF_NOISE_SIZE];
        if (r->dof_shader && ui_blue_noise(noise)) {
            r->dof_noise = gpu_texture_create(UI_DOF_NOISE_SIZE, UI_DOF_NOISE_SIZE, 1, PIXELFORMAT_R8,
                                              TEXTURE_USAGE_READ, 1, TEXTURE_2D, "ui depth of field blue noise");
            if (r->dof_noise) gpu_texture_upload(r->dof_noise, 0, 0, noise, sizeof(noise));
        }
        if (!r->dof_shader || !r->dof_state || !r->dof_noise) {
            r->dof_failed = true;
            return false;
        }
    }
    if (r->dof_scene && r->dof_width == width && r->dof_height == height) return true;
    if (r->dof_scene) {
        if (r->dof_retired) gpu_texture_destroy(r->dof_retired);
        r->dof_retired = r->dof_scene;
        r->dof_retired_frames = 4;
    }
    r->dof_scene = gpu_texture_create(width, height, 1, PIXELFORMAT_RGBA8,
                                      TEXTURE_USAGE_READ | TEXTURE_USAGE_RENDER_TARGET, 1, TEXTURE_2D,
                                      "ui depth of field scene");
    r->dof_width = width;
    r->dof_height = height;
    if (!r->dof_scene) r->dof_failed = true;
    return r->dof_scene != 0;
}

// What every draw of one flush shares.
typedef struct ui_flush_frame {
    ns_bool hud;
    ui_hud_frame hud_frame;
    u32 clip_offset;
    u32 root_offset;
    u32 pack;
    i32 width;
    i32 height;
    f32 projection_words[UI_PROJECTION_PANEL * UI_PROJECTION_WORDS];
} ui_flush_frame;

// Encode draws [from, to) in the open pass. The shader, state and storage are
// bound again for each run: a composite between runs replaces them.
static void ui_encode_draws(ui_renderer *r, const ui_flush_frame *f, i32 from, i32 to) {
    const i32 framebuffer_width = f->width;
    const i32 framebuffer_height = f->height;
    gpu_set_viewport(0, 0, framebuffer_width, framebuffer_height);
    gpu_set_scissor(0, 0, framebuffer_width, framebuffer_height);
    gpu_set_state(f->hud ? r->render_state_hud : r->render_state);
    gpu_set_shader(r->shader);
    gpu_set_storage(r->storage);
    gpu_addr bound_storage = r->storage;
    for (i32 i = from; i < to; i++) {
        const ui_draw *d = &r->draws[i];
        ui_gpu_root root = {
            .screen_width = (f32)r->rect.w,
            .screen_height = (f32)r->rect.h,
            .clip_offset = f->clip_offset,
            .projection_offset = r->projection_offset,
            .vertex_scale = {1.0f, 1.0f, 0.0f, (f32)((f64)framebuffer_width / r->rect.w)},
        };
        memcpy(root.font_faces, r->font_faces, sizeof(root.font_faces));
        // Both slots are bound on every draw; one the draw does not read
        // holds a texture the draw does read where there is one.
        u32 textures[2];
        textures[UI_TEXTURE_MSDF] = d->msdf && f->pack ? f->pack : (d->rgba ? d->rgba : f->pack);
        if (!textures[UI_TEXTURE_MSDF]) textures[UI_TEXTURE_MSDF] = ui_placeholder_texture(r);
        textures[UI_TEXTURE_RGBA] = d->rgba ? d->rgba : textures[UI_TEXTURE_MSDF];
        i32 vertex_offset = d->vertex_offset;
        i32 vertex_count = d->vertex_count;
        gpu_addr storage = r->storage;
        if (d->command >= 0) {
            const ui_command *cmd = &r->commands[d->command];
            if (cmd->rect_batch_id > 0) {
                ui_rect_batch *batch = ui_rect_batch_get(r, cmd->rect_batch_id);
                if (!batch || !batch->buffer || batch->dirty) continue;
                storage = batch->buffer;
            }
            vertex_offset = cmd->vertex_offset;
            vertex_count = cmd->vertex_count;
            root.offset_x = (f32)cmd->offset_x;
            root.offset_y = (f32)cmd->offset_y;
            root.batch_meta = d->batch_meta;
            root.quad_origin = (u32)vertex_offset;
            root.vertex_scale[0] = cmd->scale_x > 0.0 ? (f32)cmd->scale_x : 1.0f;
            root.vertex_scale[1] = cmd->scale_y > 0.0 ? (f32)cmd->scale_y : 1.0f;
            root.vertex_scale[2] = 1.0f;
            root.batch_clip[0] = (f32)(cmd->batch_clip.x + r->safe_rect.x);
            root.batch_clip[1] = (f32)(cmd->batch_clip.y + r->safe_rect.y);
            root.batch_clip[2] = (f32)(cmd->batch_clip.x + r->safe_rect.x + cmd->batch_clip.w);
            root.batch_clip[3] = (f32)(cmd->batch_clip.y + r->safe_rect.y + cmd->batch_clip.h);
            const u32 projection = (d->batch_meta >> UI_META_PROJECTION_SHIFT) & 15u;
            if (projection > 0 && projection <= UI_PROJECTION_PANEL) {
                memcpy(root.projection, f->projection_words + (projection - 1) * UI_PROJECTION_WORDS, sizeof(root.projection));
            }
        }
        if (f->hud) ui_fill_hud_root(&root, &f->hud_frame);
        // All roots have reserved, aligned space in this frame's private
        // storage. Large command lists no longer consume the frame ring once
        // per draw, or retry its allocation thousands of times on failure.
        gpu_addr root_addr = r->storage + f->root_offset + (u64)(u32)i * GPU_V2_ALLOC_ALIGN;
        gpu_write(root_addr, &root, sizeof(root));
        gpu_set_root(root_addr);
        gpu_set_texture_table(textures, 2);
        if (storage != bound_storage) {
            gpu_set_storage(storage);
            bound_storage = storage;
        }
        gpu_draw_vertices(vertex_offset, vertex_count, 1);
    }
}

// Gather each plane the scene holds back onto the screen, within its rectangle.
static void ui_composite_focus(ui_renderer *r, const ui_flush_frame *f, u32 planes) {
    const f64 scale = (f64)f->height / r->rect.h;
    gpu_set_viewport(0, 0, f->width, f->height);
    gpu_set_scissor(0, 0, f->width, f->height);
    gpu_set_state(r->dof_state);
    gpu_set_shader(r->dof_shader);
    const u32 textures[2] = {r->dof_scene, r->dof_noise};
    gpu_set_texture_table(textures, 2);
    for (i32 i = 0; i < r->plane_count; i++) {
        if (!(planes & (1u << i))) continue;
        const f32 *dof = r->plane_dof[i];
        const f32 *box = f->projection_words + i * UI_PROJECTION_WORDS + 16;
        ui_dof_root root = {
            .rect = {box[0], box[1], box[2], box[3]},
            .focus = {(f32)(dof[0] * scale), dof[1], (f32)(dof[2] * scale), (f32)(dof[3] / scale)},
            .size = {(f32)f->width, (f32)f->height, 0.0f, 0.0f},
        };
        gpu_set_root_data(&root, sizeof(root));
        gpu_draw_vertices(0, 6, 1);
    }
}

void ui_flush(ui_renderer *r, ui_color_rgba *clear) {
    if (!r || !r->gpu_ready) return;
    // ui_color_rgba is passed by value at the ns surface and the native FFI
    // adapter retains the historical opaque pointer ABI. The old renderer did
    // not dereference it either; keep that ABI and use the established default.
    ns_unused(clear);
    static ui_flush_frame f;
    f.hud = ui_hud_solve(&f.hud_frame);
    // The immersive HUD places itself; a 3D panel otherwise.
    ns_bool panel = !f.hud && r->panel;
    ui_preview_sync(r);
    if (!ui_plan_draws(r, panel, f.hud)) return;
    if (!ui_upload_storage(r, &f.clip_offset, &f.root_offset)) return;
    const f64 fallback_scale = r->content_scale > 0.0 ? r->content_scale : 1.0;
    f.width = r->v && r->v->framebuffer_width > 0
                  ? r->v->framebuffer_width
                  : (i32)(r->rect.w * fallback_scale + 0.5);
    f.height = r->v && r->v->framebuffer_height > 0
                   ? r->v->framebuffer_height
                   : (i32)(r->rect.h * fallback_scale + 0.5);
    ui_upload_projections(r, (f64)f.width / r->rect.w, (f64)f.height / r->rect.h, f.projection_words);
    f.pack = r->plan_msdf ? ui_font_pack(r, r->plan_zh) : r->font_pack;
    // One pass, one pipeline: clips, planes and the panel are resolved per
    // fragment, so nothing changes between the draws but their roots. Only a
    // run of draws on a plane with depth of field leaves the screen: it draws
    // into the offscreen scene, and the screen pass that resumes after it
    // gathers the scene back through the lens before the next run.
    ns_bool screen = false;
    for (i32 i = 0; i < r->draw_count || !screen;) {
        i32 end = i;
        u32 planes = 0;
        ns_bool focus = i < r->draw_count && r->draws[i].focus;
        while (end < r->draw_count && r->draws[end].focus == focus) planes |= r->draws[end++].planes;
        if (focus && !ui_dof_ready(r, f.width, f.height)) focus = false;
        if (focus) {
            if (screen) gpu_pass_end();
            screen = false;
            gpu_pass_begin("ui focus plane", r->dof_scene, 0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0);
            ui_encode_draws(r, &f, i, end);
            gpu_pass_end();
        }
        if (!screen) {
            if (f.hud) gpu_screen_pass_begin("ui", 0.0, 0.0, 0.0, 1.0);
            else gpu_screen_pass_begin_depth("ui", 0.0, 0.0, 0.0, 1.0, 0.0);
            screen = true;
        }
        if (focus) ui_composite_focus(r, &f, planes);
        else ui_encode_draws(r, &f, i, end);
        i = end;
    }
    gpu_pass_end();
    // No commit: the pass joins the frame's one command buffer, which the
    // host submits and presents when the frame callback returns. Committing
    // here split every flush into its own buffer and advanced the frame ring
    // once per flush instead of once per frame.
    r->vertex_count = 0;
    r->command_count = 0;
    r->draw_count = 0;
    ui_age_retired_buffers(r);
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
// Struct results that ns reads through a `ref` come from a small per-thread
// ring rather than the allocator: nothing owns such a result, so allocating
// one per call leaked it, and per-frame callers leaked steadily. Each result
// stays valid for the next UI_RESULT_RING - 1 calls on the same thread.
#define UI_RESULT_RING 16
static ui_rect *ui_result_rect(void) {
    static _Thread_local ui_rect ring[UI_RESULT_RING];
    static _Thread_local u32 next;
    return &ring[next++ % UI_RESULT_RING];
}

ui_rect *ui_safe_rect(ui_renderer *r) {
    ui_rect *out = ui_result_rect();
    *out = r ? (ui_rect){0.0, 0.0, r->safe_rect.w, r->safe_rect.h}
             : (ui_rect){0.0, 0.0, 0.0, 0.0};
    return out;
}

ui_rect *ui_surface_rect(ui_renderer *r) {
    ui_rect *out = ui_result_rect();
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
    static _Thread_local ui_insets ring[UI_RESULT_RING];
    static _Thread_local u32 next;
    ui_insets *out = &ring[next++ % UI_RESULT_RING];
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
    ui_rect *rect = ui_result_rect();
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

f64 ui_pixel_scale(ui_renderer *r) {
    if (!r) return 1.0;
    if (r->v && r->v->framebuffer_width > 0 && r->rect.w > 0.0) return (f64)r->v->framebuffer_width / r->rect.w;
    return r->content_scale > 0.0 ? r->content_scale : 1.0;
}

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

// How far down, at font_px, a glyph that `font` draws in place of `primary`
// moves so its middle meets the primary face's: each face lays its glyphs
// out from its own line top, and Chinese would otherwise sit well above the
// Latin beside it. The bitmap faces share one pixel grid and stay put.
static f64 ui_fallback_shift(ui_renderer *r, ui_font *primary, ui_font *font, f64 font_px) {
    if (font == primary || ui_font_is_bitmap(r, font) || ui_font_is_bitmap(r, primary) ||
        primary->font_size <= 0.0 || font->font_size <= 0.0) return 0.0;
    return primary->middle * (font_px / primary->font_size) - font->middle * (font_px / font->font_size);
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

void ui_draw_text_scaled(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type, f64 scale_x, f64 scale_y) {
    if (!r || !text || font_px <= 0 || scale_x <= 0 || scale_y <= 0) return;
    ui_font *primary = ui_primary_font(r, font_type);
    f64 cx = x;
    f64 cy = y;
    const unsigned char *p = (const unsigned char*)text;
    while (*p) {
        i32 code = ui_utf8_next(&p);
        if (code == '\n') {
            cx = x;
            cy += primary->line_height * (font_px / primary->font_size) * scale_y;
            continue;
        }
        ui_glyph *g = NULL;
        ui_font *font = ui_font_for_code(r, font_type, code, &g);
        if (!g) {
            cx += ui_missing_glyph_advance(font_type, font_px) * scale_x;
            continue;
        }
        f64 scale = font_px / font->font_size;
        if (g->width > 0 && g->height > 0) {
            f64 x0 = cx + g->x_offset * scale * scale_x;
            f64 y0 = cy + (g->y_offset * scale + ui_fallback_shift(r, primary, font, font_px)) * scale_y;
            const ns_bool bitmap = ui_font_is_bitmap(r, font);
            if (bitmap && scale_x == 1.0 && scale_y == 1.0) {
                x0 = floor(x0);
                y0 = floor(y0);
            }
            f64 x1 = x0 + g->width * scale * scale_x;
            f64 y1 = y0 + g->height * scale * scale_y;
            r->current_texture_id = ui_font_texture_id(r, font);
            ui_push_quad_ex(r, x0, y0, x1, y1,
                            g->atlas_x / font->texture_width,
                            g->atlas_y / font->texture_height,
                            (g->atlas_x + g->width) / font->texture_width,
                            (g->atlas_y + g->height) / font->texture_height,
                            rgba, bitmap ? UI_KIND_BITMAP : UI_KIND_MSDF, 5.0, 0.0, 1.0);
        }
        cx += g->x_advance * scale * scale_x;
    }
}

void ui_draw_text(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type) {
    ui_draw_text_scaled(r, x, y, text, font_px, rgba, font_type, 1.0, 1.0);
}

// Lay a line of text into a retained batch, in the batch's local space. Glyphs
// keep their unsnapped positions so the batch scales cleanly at draw time.
// The first glyph fixes the batch's font texture; a glyph that falls back to
// another atlas (or a batch already holding rectangles) is skipped.
static void ui_rect_batch_text_range(ui_renderer *r, i32 batch_id, f64 x, f64 y, const char *text, i32 length, f64 font_px, u32 rgba, i32 font_type) {
    ui_rect_batch *batch = ui_rect_batch_get(r, batch_id);
    if (!batch || !text || font_px <= 0.0) return;
    if (batch->kind == UI_KIND_IMAGE) return;
    ui_font *primary = ui_primary_font(r, font_type);
    f64 cx = x;
    f64 cy = y;
    const unsigned char *p = (const unsigned char*)text;
    const unsigned char *end = p + length;
    while (p < end && *p) {
        // A byte range may end inside a UTF-8 sequence. Give the decoder the
        // same zero-terminated tail that a substring would have supplied.
        i32 code;
        if (end - p < 4) {
            unsigned char tail[5] = {0};
            size_t remaining = (size_t)(end - p);
            memcpy(tail, p, remaining);
            const unsigned char *cursor = tail;
            code = ui_utf8_next(&cursor);
            p += cursor - tail;
        } else {
            code = ui_utf8_next(&p);
        }
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
            const i32 kind = ui_font_is_bitmap(r, font) ? UI_KIND_BITMAP : UI_KIND_MSDF;
            const i32 texture_id = ui_font_texture_id(r, font);
            if (batch->kind < 0) {
                batch->kind = kind;
                batch->texture_id = texture_id;
            }
            if (batch->kind == kind && batch->texture_id == texture_id && ui_rect_batch_reserve(batch, 6)) {
                batch->dirty = true;
                f32 x0 = (f32)(cx + g->x_offset * scale);
                f32 y0 = (f32)(cy + g->y_offset * scale + ui_fallback_shift(r, primary, font, font_px));
                f32 x1 = x0 + (f32)(g->width * scale);
                f32 y1 = y0 + (f32)(g->height * scale);
                f32 u0 = (f32)(g->atlas_x / font->texture_width);
                f32 v0 = (f32)(g->atlas_y / font->texture_height);
                f32 u1 = (f32)((g->atlas_x + g->width) / font->texture_width);
                f32 v1 = (f32)((g->atlas_y + g->height) / font->texture_height);
                ui_vertex quad[6] = {
                    {.x = x0, .y = y0, .u = u0, .v = v0, .color = rgba, .range = 5.0f, .softness = 1.0f},
                    {.x = x1, .y = y0, .u = u1, .v = v0, .color = rgba, .range = 5.0f, .softness = 1.0f},
                    {.x = x1, .y = y1, .u = u1, .v = v1, .color = rgba, .range = 5.0f, .softness = 1.0f},
                    {.x = x0, .y = y0, .u = u0, .v = v0, .color = rgba, .range = 5.0f, .softness = 1.0f},
                    {.x = x1, .y = y1, .u = u1, .v = v1, .color = rgba, .range = 5.0f, .softness = 1.0f},
                    {.x = x0, .y = y1, .u = u0, .v = v1, .color = rgba, .range = 5.0f, .softness = 1.0f},
                };
                memcpy(batch->vertices + batch->vertex_count, quad, sizeof(quad));
                batch->vertex_count += 6;
            }
        }
        cx += g->x_advance * scale;
    }
}

void ui_rect_batch_add_text(ui_renderer *r, i32 batch_id, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type) {
    if (!text) return;
    ui_rect_batch_text_range(r, batch_id, x, y, text, (i32)strlen(text), font_px, rgba, font_type);
}

void ui_rect_batch_add_text_range(ui_renderer *r, i32 batch_id, f64 x, f64 y, const char *text, i32 offset, i32 length, f64 font_px, u32 rgba, i32 font_type) {
    if (!text || offset < 0 || length <= 0) return;
    size_t size = strlen(text);
    if ((size_t)offset > size) return;
    if ((size_t)length > size - (size_t)offset) length = (i32)(size - (size_t)offset);
    ui_rect_batch_text_range(r, batch_id, x, y, text + offset, length, font_px, rgba, font_type);
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
