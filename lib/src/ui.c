#include "gpu.h"
#include "ns_type.h"
#include "view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define UI_POPEN _popen
#define UI_PCLOSE _pclose
#else
#define UI_POPEN popen
#define UI_PCLOSE pclose
#endif

#define UI_VERTEX_STRIDE 36
#define UI_INITIAL_VERTEX_CAP 131072
#define UI_MAX_COMMANDS 4096
#define UI_MAX_CLIPS 32
#define UI_MAX_GPU_CLIPS 4096
#define UI_FONT_MAIN 0
#define UI_FONT_MONO 1
#define UI_WHITE_TEXTURE 1
#define UI_FONT_TEXTURE 2
#define UI_KIND_IMAGE 0
#define UI_KIND_MSDF 1

typedef struct io_image {
    i32 width;
    i32 height;
    i32 channels;
    u8 *data;
} io_image;

extern io_image *io_load_image(const char *path);

typedef struct ui_color_rgba {
    f64 r;
    f64 g;
    f64 b;
    f64 a;
} ui_color_rgba;

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
    i32 clip_x, clip_y, clip_w, clip_h;
} ui_command;

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
} ui_font;

typedef struct ui_renderer {
    void *handle;
    view *v;
    i32 width;
    i32 height;

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

    ui_font fonts[2];
    gpu_texture white_texture;
    gpu_texture font_texture;
    gpu_buffer screen_buffer;
    gpu_buffer clip_buffer;
    gpu_buffer vertex_buffer;
    gpu_shader shader_image;
    gpu_shader shader_msdf;
    gpu_pipeline pipeline_image;
    gpu_pipeline pipeline_msdf;
    gpu_binding binding_white_image;
    gpu_binding binding_font_msdf;
    gpu_mesh mesh;
    gpu_render_pass screen_pass;
    ns_bool gpu_ready;
} ui_renderer;

void ui_renderer_destroy(ui_renderer *r);

static const char *ui_shader_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; uchar4 col [[attribute(2)]]; float4 params [[attribute(3)]]; };\n"
"struct VOut { float4 pos [[position]]; float2 pixel; float2 uv; float4 col; float4 params; };\n"
"vertex VOut ui_vs(VIn in [[stage_in]], constant float2 &screen [[buffer(1)]]) {\n"
"  VOut o; float2 ndc = float2((in.pos.x / screen.x) * 2.0 - 1.0, 1.0 - (in.pos.y / screen.y) * 2.0);\n"
"  o.pos = float4(ndc, 0.0, 1.0); o.pixel = in.pos; o.uv = in.uv; o.col = float4(in.col) / 255.0; o.params = in.params; return o;\n"
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
"}\n";

static f64 ui_clamp_f64(f64 v, f64 lo, f64 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
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

static f64 ui_detect_baseline(ui_font *font) {
    static const i32 refs[] = {72, 77, 78, 73, 76, 69, 88, 84};
    for (u32 i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        ui_glyph *g = ui_font_glyph(font, refs[i]);
        if (g && g->height > 0) return g->y_offset + g->height;
    }
    return round(font->font_size * 0.8);
}

static ns_bool ui_load_font_face(char *json, const char *face_name, i32 tex_w, i32 tex_h, ui_font *font) {
    char *face = ui_find_key(json, face_name);
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
    font->baseline = ui_detect_baseline(font);
    return true;
}

static ns_bool ui_load_fonts(ui_renderer *r) {
    size_t len = 0;
    char *json = ui_read_file("lib/assets/latin_mono.json", &len);
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
    ui_command *cmd = &r->commands[r->command_count - 1];
    if (r->command_count > 0 &&
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

    cmd = &r->commands[r->command_count++];
    *cmd = (ui_command){
        .vertex_offset = base,
        .vertex_count = count,
        .texture_id = r->current_texture_id,
        .kind = kind,
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

static void ui_create_gpu_resources(ui_renderer *r) {
    f32 screen[2] = {(f32)r->width, (f32)r->height};
    r->screen_buffer = gpu_create_buffer(&(gpu_buffer_desc){
        .size = sizeof(screen),
        .data = (ns_data){screen, sizeof(screen)},
        .type = BUFFER_UNIFORM,
        .usage = USAGE_DEFAULT,
    });
    r->clip_buffer = gpu_create_buffer(&(gpu_buffer_desc){
        .size = (int)sizeof(r->gpu_clips),
        .type = BUFFER_UNIFORM,
        .usage = USAGE_DEFAULT,
    });
    r->vertex_buffer = gpu_create_buffer(&(gpu_buffer_desc){
        .size = r->vertex_capacity * UI_VERTEX_STRIDE,
        .type = BUFFER_VERTEX,
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

    io_image *img = io_load_image("lib/assets/latin_mono.png");
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
    shader_desc.fragment.entry = ns_str_cstr("ui_fs_msdf");
    r->shader_msdf = gpu_create_shader(&shader_desc);

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
    pipe.depth.format = PIXELFORMAT_NONE;
    pipe.primitive_type = PRIMITIVE_TRIANGLES;
    pipe.index_type = INDEX_NONE;
    pipe.cull_mode = CULL_NONE;
    pipe.face_winding = FACE_WINDING_CCW;
    pipe.sample_count = 1;
    pipe.shader = r->shader_image;
    r->pipeline_image = gpu_create_pipeline(&pipe);
    pipe.shader = r->shader_msdf;
    r->pipeline_msdf = gpu_create_pipeline(&pipe);

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
    r->mesh = gpu_create_mesh(&(gpu_mesh_desc){
        .buffers = {r->vertex_buffer},
        .pipeline = r->pipeline_image,
    });
    r->screen_pass = (gpu_render_pass){.id = 0};
    r->gpu_ready = r->screen_buffer.id && r->clip_buffer.id && r->vertex_buffer.id && r->white_texture.id &&
                   r->font_texture.id && r->pipeline_image.id && r->pipeline_msdf.id &&
                   r->binding_white_image.id && r->binding_font_msdf.id && r->mesh.id;
}

ui_renderer *ui_renderer_create(view *v) {
    ui_renderer *r = (ui_renderer*)calloc(1, sizeof(ui_renderer));
    if (!r) return NULL;
    r->handle = r;
    r->v = v;
    r->width = v ? v->framebuffer_width : 1;
    r->height = v ? v->framebuffer_height : 1;
    if (r->width <= 0 && v) r->width = v->width;
    if (r->height <= 0 && v) r->height = v->height;
    if (r->width <= 0) r->width = 1;
    if (r->height <= 0) r->height = 1;
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

void ui_renderer_destroy(ui_renderer *r) {
    if (!r) return;
    for (i32 i = 0; i < 2; i++) free(r->fonts[i].glyphs);
    free(r->vertices);
    free(r);
}

void ui_resize(ui_renderer *r) {
    if (!r) return;
    if (r->v) {
        r->width = r->v->framebuffer_width > 0 ? r->v->framebuffer_width : r->v->width;
        r->height = r->v->framebuffer_height > 0 ? r->v->framebuffer_height : r->v->height;
    }
    f32 screen[2] = {(f32)r->width, (f32)r->height};
    if (r->screen_buffer.id) gpu_update_buffer(r->screen_buffer, (ns_data){screen, sizeof(screen)});
}

void ui_resize_to(ui_renderer *r, i32 width, i32 height) {
    if (!r) return;
    r->width = width > 0 ? width : 1;
    r->height = height > 0 ? height : 1;
    f32 screen[2] = {(f32)r->width, (f32)r->height};
    if (r->screen_buffer.id) gpu_update_buffer(r->screen_buffer, (ns_data){screen, sizeof(screen)});
}

void ui_request_render(ui_renderer *r, i32 frames) {
    ns_unused(r);
    ns_unused(frames);
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
    gpu_update_buffer(r->vertex_buffer, (ns_data){r->vertices, (size_t)r->vertex_count * UI_VERTEX_STRIDE});
    if (r->clip_buffer.id) {
        ui_gpu_clip empty_clip = {0};
        const void *clip_data = r->gpu_clip_count > 0 ? (const void*)r->gpu_clips : (const void*)&empty_clip;
        const size_t clip_len = r->gpu_clip_count > 0 ? (size_t)r->gpu_clip_count * sizeof(ui_gpu_clip) : sizeof(empty_clip);
        gpu_update_buffer(r->clip_buffer, (ns_data){(void*)clip_data, clip_len});
    }
    gpu_begin_render_pass(r->screen_pass);
    gpu_set_viewport(0, 0, r->width, r->height);
    ns_unused(clear);
    for (i32 i = 0; i < r->command_count; i++) {
        ui_command *cmd = &r->commands[i];
        if (cmd->clip_w <= 0 || cmd->clip_h <= 0) continue;
        gpu_set_scissor(cmd->clip_x, cmd->clip_y, cmd->clip_w, cmd->clip_h);
        if (cmd->kind == UI_KIND_MSDF) {
            gpu_set_pipeline(r->pipeline_msdf);
            gpu_set_binding(r->binding_font_msdf);
        } else {
            gpu_set_pipeline(r->pipeline_image);
            gpu_set_binding(r->binding_white_image);
        }
        gpu_set_mesh(r->mesh);
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

ns_bool ui_rect_clipped(ui_renderer *r, f64 x, f64 y, f64 w, f64 h) {
    if (!r) return true;
    ui_clip c = ui_current_clip(r);
    return x + w <= c.x || y + h <= c.y || x >= c.x + c.w || y >= c.y + c.h;
}

void ui_fill_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, u32 rgba, f64 feather) {
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

void ui_fill_round_rect(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 radius, u32 rgba, f64 feather) {
    ns_unused(feather);
    if (!r || w <= 0 || h <= 0) return;
    f64 pts[4 * 9 * 2];
    i32 n = 0;
    ui_round_rect_points(pts, &n, x, y, w, h, radius);
    if (n < 3) return;
    r->current_texture_id = UI_WHITE_TEXTURE;
    f64 u = 0, v = 0;
    f64 x0 = pts[0], y0 = pts[1];
    for (i32 i = 1; i < n - 1; i++) {
        ui_push_tri(r, x0, y0, pts[i * 2], pts[i * 2 + 1], pts[(i + 1) * 2], pts[(i + 1) * 2 + 1], u, v, rgba);
    }
}

void ui_fill_round_rect_per_corner(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, f64 rtl, f64 rtr, f64 rbl, f64 rbr, u32 rgba, f64 feather) {
    ns_unused(rtl);
    ns_unused(rtr);
    ns_unused(rbl);
    ui_fill_round_rect(r, x, y, w, h, rbr, rgba, feather);
}

void ui_draw_text(ui_renderer *r, f64 x, f64 y, const char *text, f64 font_px, u32 rgba, i32 font_type) {
    if (!r || !text || font_px <= 0) return;
    ui_font *primary = &r->fonts[(font_type == UI_FONT_MONO) ? UI_FONT_MONO : UI_FONT_MAIN];
    f64 cx = x;
    f64 cy = y;
    f64 baseline_y = cy + primary->baseline * (font_px / primary->font_size);
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            cy += primary->line_height * (font_px / primary->font_size);
            baseline_y = cy + primary->baseline * (font_px / primary->font_size);
            continue;
        }
        i32 code = *p;
        ui_glyph *g = ui_font_glyph(primary, code);
        if (!g) g = ui_font_glyph(primary, 32);
        if (!g) continue;
        f64 scale = font_px / primary->font_size;
        if (g->width > 0 && g->height > 0) {
            f64 x0 = cx + g->x_offset * scale;
            f64 y0 = baseline_y - (primary->baseline - g->y_offset) * scale;
            f64 x1 = x0 + g->width * scale;
            f64 y1 = y0 + g->height * scale;
            r->current_texture_id = UI_FONT_TEXTURE;
            ui_push_quad_ex(r, x0, y0, x1, y1,
                            g->atlas_x / primary->texture_width,
                            g->atlas_y / primary->texture_height,
                            (g->atlas_x + g->width) / primary->texture_width,
                            (g->atlas_y + g->height) / primary->texture_height,
                            rgba, UI_KIND_MSDF, 5.0, 0.0, 1.0);
        }
        cx += g->x_advance * scale;
    }
}

f64 ui_text_line_height(ui_renderer *r, f64 font_px, i32 font_type) {
    if (!r) return font_px;
    ui_font *font = &r->fonts[(font_type == UI_FONT_MONO) ? UI_FONT_MONO : UI_FONT_MAIN];
    return font->font_size > 0 ? font->line_height * (font_px / font->font_size) : font_px;
}

f64 ui_text_v_center_y(ui_renderer *r, f64 y, f64 h, f64 font_px, i32 font_type) {
    f64 line_h = ui_text_line_height(r, font_px, font_type);
    return y + (h - line_h) * 0.5 - font_px * 0.03;
}

f64 ui_text_width(ui_renderer *r, const char *text, f64 font_px, i32 font_type) {
    if (!r || !text) return 0;
    ui_font *font = &r->fonts[(font_type == UI_FONT_MONO) ? UI_FONT_MONO : UI_FONT_MAIN];
    f64 width = 0;
    for (const unsigned char *p = (const unsigned char*)text; *p && *p != '\n'; p++) {
        ui_glyph *g = ui_font_glyph(font, *p);
        if (!g) g = ui_font_glyph(font, 32);
        width += g ? g->x_advance * (font_px / font->font_size) : font_px * 0.55;
    }
    return width;
}

f64 ui_mono_char_width(ui_renderer *r, f64 font_px, i32 font_type) {
    ns_unused(font_type);
    return ui_text_width(r, "0", font_px, UI_FONT_MONO);
}

// ---- native agentic coding bridge ----------------------------------------
// Nano Script cannot yet assign function values into view.on_frame reliably, so
// the native demo installs a C frame callback and keeps the renderer alive for
// the app loop. This bridge draws the agentic coding shell used by
// nscode/native: chat history, agent timeline, and a Git-backed local diff.

typedef void(*ui_view_callback)(view*);

#define UI_AGENT_DIFF_CAP (256 * 1024)
#define UI_AGENT_DIFF_REFRESH_FRAMES 120

typedef struct ui_agent_chat {
    const char *title;
    const char *subtitle;
    const char *meta;
    const char *badge;
} ui_agent_chat;

typedef struct ui_agent_step {
    const char *kind;
    const char *title;
    const char *detail;
    ns_bool done;
} ui_agent_step;

typedef enum ui_agent_mode {
    UI_AGENT_MODE_CODE = 0,
    UI_AGENT_MODE_AGENTIC = 1,
} ui_agent_mode;

static ui_renderer *ui_agent_renderer;
static view *ui_agent_view;
static f64 ui_agent_diff_scroll_y;
static f64 ui_agent_dialog_scroll_y;
static f64 ui_code_scroll_y;
static i32 ui_agent_selected_chat = 0;
static i32 ui_agent_frame_count;
static ui_agent_mode ui_agent_current_mode = UI_AGENT_MODE_AGENTIC;
static char *ui_agent_diff_text;
static size_t ui_agent_diff_len;
static ns_bool ui_agent_diff_loaded;
static ns_bool ui_agent_diff_truncated;
static ns_bool ui_agent_diff_error;
static char ui_agent_diff_message[256];

static void ui_agent_terminate(view *v);

static const ui_agent_chat ui_agent_chats[] = {
    {"Agentic native shell", "Design and wire the NSCode layout", "now", "active"},
    {"Parser diagnostics", "Review AST checks for editor modules", "14m", "done"},
    {"GPU text pass", "Tighten clipping and glyph metrics", "1h", "notes"},
    {"Diff review", "Inspect local changes before commit", "2h", "git"},
};

static const ui_agent_step ui_agent_steps[] = {
    {"scan", "Read current native bridge", "Mapped the C callback path that owns runtime rendering.", true},
    {"plan", "Choose shell shape", "Three panes: conversations, agent dialog, and Git diff.", true},
    {"edit", "Patch renderer", "Add seeded agent activity and live diff loading.", true},
    {"check", "Verify native surface", "Build, AST checks, then visual review in the running app.", false},
};

static f64 ui_agent_scale(void) {
    if (ui_agent_view && ui_agent_view->display_ratio > 0.0) {
        return ui_agent_view->display_ratio;
    }
    return (ui_agent_view && ui_agent_view->ui_scale > 0.0) ? ui_agent_view->ui_scale : 1.0;
}

static f64 ui_agent_font_px(void) { return 14.0 * ui_agent_scale(); }
static f64 ui_agent_small_px(void) { return 12.0 * ui_agent_scale(); }
static f64 ui_agent_title_px(void) { return 18.0 * ui_agent_scale(); }

static u32 ui_agent_bg(void) { return ui_pack_color("#121417"); }
static u32 ui_agent_rail(void) { return ui_pack_color("#171b20"); }
static u32 ui_agent_surface(void) { return ui_pack_color("#1e242b"); }
static u32 ui_agent_surface_hi(void) { return ui_pack_color("#27313a"); }
static u32 ui_agent_line(void) { return ui_pack_color("#34404a"); }
static u32 ui_agent_text(void) { return ui_pack_color("#d7dde5"); }
static u32 ui_agent_muted(void) { return ui_pack_color("#83909e"); }
static u32 ui_agent_accent(void) { return ui_pack_color("#61d394"); }
static u32 ui_agent_blue(void) { return ui_pack_color("#7aa8ff"); }
static u32 ui_agent_yellow(void) { return ui_pack_color("#f2c86b"); }
static u32 ui_agent_red(void) { return ui_pack_color("#ff7b86"); }
static u32 ui_agent_green_bg(void) { return ui_pack_color("#173726"); }
static u32 ui_agent_red_bg(void) { return ui_pack_color("#3a2025"); }
static u32 ui_agent_hunk_bg(void) { return ui_pack_color("#23304a"); }
static u32 ui_agent_add(void) { return ui_pack_color("#8ee6a7"); }
static u32 ui_agent_del(void) { return ui_pack_color("#ff9aa5"); }
static u32 ui_agent_hunk(void) { return ui_pack_color("#a8c5ff"); }
static u32 ui_agent_titlebar(void) { return ui_pack_color("#101317"); }
static u32 ui_agent_close(void) { return ui_pack_color("#ff5f57"); }
static u32 ui_agent_min(void) { return ui_pack_color("#ffbd2e"); }
static u32 ui_agent_zoom(void) { return ui_pack_color("#28c840"); }
static u32 ui_code_editor_bg(void) { return ui_pack_color("#0f1117"); }
static u32 ui_code_editor_panel(void) { return ui_pack_color("#171a22"); }
static u32 ui_code_editor_panel_hi(void) { return ui_pack_color("#202633"); }
static u32 ui_code_editor_gutter(void) { return ui_pack_color("#222733"); }
static u32 ui_code_editor_keyword(void) { return ui_pack_color("#ffcf70"); }
static u32 ui_code_editor_string(void) { return ui_pack_color("#94e2a6"); }
static u32 ui_code_editor_comment(void) { return ui_pack_color("#758197"); }
static u32 ui_code_editor_number(void) { return ui_pack_color("#8fb8ff"); }
static u32 ui_code_editor_cursor(void) { return ui_pack_color("#f4d35e"); }

static ui_color_rgba ui_agent_clear(void) {
    return (ui_color_rgba){0.071, 0.078, 0.090, 1.0};
}

static void ui_agent_reset_input(view *v) {
    if (!v) return;
    v->mouse_pressed = false;
    v->mouse_released = false;
    v->mouse_right_pressed = false;
    v->mouse_right_released = false;
    v->mouse_middle_pressed = false;
    v->mouse_middle_released = false;
    v->scroll_x = 0.0;
    v->scroll_y = 0.0;
}

static ns_bool ui_agent_hit(view *v, f64 x, f64 y, f64 w, f64 h) {
    return v && v->mouse_x >= x && v->mouse_x < x + w && v->mouse_y >= y && v->mouse_y < y + h;
}

static void ui_agent_draw_label(ui_renderer *r, f64 x, f64 y, const char *text) {
    ui_draw_text(r, x, y, text, ui_agent_small_px(), ui_agent_muted(), UI_FONT_MONO);
}

static void ui_agent_draw_rule(ui_renderer *r, f64 x, f64 y, f64 w) {
    const f64 s = ui_agent_scale();
    ui_fill_rect(r, x, y, w, 1.0 * s, ui_agent_line(), 0.0);
}

static void ui_agent_draw_pill(ui_renderer *r, f64 x, f64 y, const char *text, u32 bg, u32 fg) {
    const f64 s = ui_agent_scale();
    const f64 px = ui_agent_small_px();
    const f64 tw = ui_text_width(r, text, px, UI_FONT_MONO);
    ui_fill_round_rect(r, x, y, tw + 16.0 * s, 22.0 * s, 6.0 * s, bg, 0.0);
    ui_draw_text(r, x + 8.0 * s, y + 6.0 * s, text, px, fg, UI_FONT_MONO);
}

static void ui_agent_draw_text_clipped(ui_renderer *r, f64 x, f64 y, f64 w, f64 h, const char *text, f64 font_px, u32 color) {
    if (!r || !text || w <= 0.0 || h <= 0.0) return;
    ui_push_clip(r, x, y, w, h);
    ui_draw_text(r, x, y, text, font_px, color, UI_FONT_MONO);
    ui_pop_clip(r);
}

static void ui_agent_draw_mode_switch(ui_renderer *r, view *v, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    const f64 pad = 3.0 * s;
    const f64 half_w = (w - pad * 2.0) * 0.5;
    const f64 code_x = x + pad;
    const f64 agent_x = x + pad + half_w;
    const ns_bool code_hot = ui_agent_hit(v, code_x, y + pad, half_w, h - pad * 2.0);
    const ns_bool agent_hot = ui_agent_hit(v, agent_x, y + pad, half_w, h - pad * 2.0);

    if (v && v->mouse_pressed) {
        if (code_hot) {
            ui_agent_current_mode = UI_AGENT_MODE_CODE;
        } else if (agent_hot) {
            ui_agent_current_mode = UI_AGENT_MODE_AGENTIC;
        }
    }

    ui_fill_round_rect(r, x, y, w, h, 8.0 * s, ui_agent_surface(), 0.0);
    ui_fill_rect(r, x + 8.0 * s, y, w - 16.0 * s, 1.0 * s, ui_agent_line(), 0.0);
    ui_fill_rect(r, x + 8.0 * s, y + h - 1.0 * s, w - 16.0 * s, 1.0 * s, ui_agent_line(), 0.0);
    ui_fill_rect(r, x, y + 8.0 * s, 1.0 * s, h - 16.0 * s, ui_agent_line(), 0.0);
    ui_fill_rect(r, x + w - 1.0 * s, y + 8.0 * s, 1.0 * s, h - 16.0 * s, ui_agent_line(), 0.0);

    const ns_bool code_active = ui_agent_current_mode == UI_AGENT_MODE_CODE;
    const f64 active_x = code_active ? code_x : agent_x;
    ui_fill_round_rect(r, active_x, y + pad, half_w, h - pad * 2.0, 6.0 * s, ui_agent_accent(), 0.0);

    const u32 code_color = code_active ? ui_agent_bg() : (code_hot ? ui_agent_text() : ui_agent_muted());
    const u32 agent_color = code_active ? (agent_hot ? ui_agent_text() : ui_agent_muted()) : ui_agent_bg();
    ui_agent_draw_text_clipped(r, code_x + 15.0 * s, y + 8.0 * s, half_w - 24.0 * s, 14.0 * s, "Code", ui_agent_small_px(), code_color);
    ui_agent_draw_text_clipped(r, agent_x + 10.0 * s, y + 8.0 * s, half_w - 18.0 * s, 14.0 * s, "Agent", ui_agent_small_px(), agent_color);
}

static void ui_agent_draw_title_bar(ui_renderer *r, view *v, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    ui_fill_rect(r, x, y, w, h, ui_agent_titlebar(), 0.0);
    ui_fill_rect(r, x, y + h - 1.0 * s, w, 1.0 * s, ui_agent_line(), 0.0);

    const f64 radius = 6.0 * s;
    const f64 cy = y + h * 0.5;
    const f64 close_x = x + 18.0 * s;
    const f64 min_x = x + 40.0 * s;
    const f64 zoom_x = x + 62.0 * s;
    const ns_bool close_hot = ui_agent_hit(v, close_x - radius, cy - radius, radius * 2.0, radius * 2.0);

    ui_fill_round_rect(r, close_x - radius, cy - radius, radius * 2.0, radius * 2.0, radius, ui_agent_close(), 0.0);
    ui_fill_round_rect(r, min_x - radius, cy - radius, radius * 2.0, radius * 2.0, radius, ui_agent_min(), 0.0);
    ui_fill_round_rect(r, zoom_x - radius, cy - radius, radius * 2.0, radius * 2.0, radius, ui_agent_zoom(), 0.0);
    if (close_hot) {
        ui_draw_text(r, close_x - 3.0 * s, cy - 5.5 * s, "x", 10.0 * s, ui_agent_bg(), UI_FONT_MONO);
    }

    const char *title = "NSCode Native";
    const f64 title_px = ui_agent_small_px();
    const f64 title_w = ui_text_width(r, title, title_px, UI_FONT_MONO);
    const f64 switch_w = 150.0 * s;
    const f64 switch_h = 28.0 * s;
    const f64 switch_x = x + fmax(92.0 * s, w - switch_w - 16.0 * s);
    const f64 title_clip_w = fmax(1.0, switch_x - x - 102.0 * s);
    ui_agent_draw_text_clipped(r, x + fmax(86.0 * s, fmin((w - title_w) * 0.5, switch_x - title_w - 18.0 * s)), y + 12.0 * s, title_clip_w, 16.0 * s, title, title_px, ui_agent_muted());
    ui_agent_draw_mode_switch(r, v, switch_x, y + 5.0 * s, switch_w, switch_h);

    if (v && v->mouse_pressed && close_hot) {
        ui_agent_terminate(v);
        exit(0);
    }
}

static void ui_agent_free_diff(void) {
    if (ui_agent_diff_text) {
        free(ui_agent_diff_text);
        ui_agent_diff_text = NULL;
    }
    ui_agent_diff_len = 0;
}

static i32 ui_agent_count_lines(const char *text) {
    if (!text || !text[0]) return 0;
    i32 lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') lines++;
    }
    return lines;
}

static void ui_agent_load_git_diff(void) {
    ui_agent_free_diff();
    ui_agent_diff_loaded = true;
    ui_agent_diff_truncated = false;
    ui_agent_diff_error = false;
    snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "No local changes");

    FILE *pipe = UI_POPEN("git diff --no-ext-diff -- 2>&1", "r");
    if (!pipe) {
        ui_agent_diff_error = true;
        snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "Unable to run git diff");
        return;
    }

    char *buf = (char*)malloc(UI_AGENT_DIFF_CAP + 1);
    if (!buf) {
        UI_PCLOSE(pipe);
        ui_agent_diff_error = true;
        snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "Unable to allocate diff buffer");
        return;
    }

    size_t used = 0;
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), pipe)) {
        const size_t n = strlen(chunk);
        if (used + n > UI_AGENT_DIFF_CAP) {
            const size_t remain = UI_AGENT_DIFF_CAP - used;
            if (remain > 0) {
                memcpy(buf + used, chunk, remain);
                used += remain;
            }
            ui_agent_diff_truncated = true;
            break;
        }
        memcpy(buf + used, chunk, n);
        used += n;
    }
    while (fgets(chunk, sizeof(chunk), pipe)) {
        ui_agent_diff_truncated = true;
    }

    const int status = UI_PCLOSE(pipe);
    buf[used] = '\0';
    ui_agent_diff_text = buf;
    ui_agent_diff_len = used;

    if (status != 0) {
        ui_agent_diff_error = true;
        if (used > 0) {
            snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "Git diff failed");
        } else {
            snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "Not a Git repository or Git is unavailable");
        }
    } else if (used == 0) {
        snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "No local changes");
    } else if (ui_agent_diff_truncated) {
        snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "Local diff truncated at 256 KB");
    } else {
        snprintf(ui_agent_diff_message, sizeof(ui_agent_diff_message), "Local Git diff");
    }
}

static void ui_agent_draw_chat_row(ui_renderer *r, f64 x, f64 y, f64 w, i32 idx) {
    const f64 s = ui_agent_scale();
    const ui_agent_chat *chat = &ui_agent_chats[idx];
    const ns_bool selected = idx == ui_agent_selected_chat;
    const u32 bg = selected ? ui_agent_surface_hi() : ui_agent_rail();
    if (selected) {
        ui_fill_round_rect(r, x + 10.0 * s, y, w - 20.0 * s, 70.0 * s, 7.0 * s, bg, 0.0);
        ui_fill_rect(r, x + 10.0 * s, y + 12.0 * s, 3.0 * s, 46.0 * s, ui_agent_accent(), 0.0);
    }
    ui_agent_draw_text_clipped(r, x + 24.0 * s, y + 12.0 * s, w - 84.0 * s, 20.0 * s, chat->title, ui_agent_font_px(), ui_agent_text());
    ui_agent_draw_text_clipped(r, x + 24.0 * s, y + 34.0 * s, w - 48.0 * s, 16.0 * s, chat->subtitle, ui_agent_small_px(), ui_agent_muted());
    ui_agent_draw_text_clipped(r, x + w - 44.0 * s, y + 12.0 * s, 34.0 * s, 16.0 * s, chat->meta, ui_agent_small_px(), ui_agent_muted());
    ui_agent_draw_pill(r, x + 24.0 * s, y + 48.0 * s, chat->badge, selected ? ui_agent_bg() : ui_agent_surface(), selected ? ui_agent_accent() : ui_agent_muted());
}

static void ui_agent_draw_history(ui_renderer *r, view *v, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    ui_fill_rect(r, x, y, w, h, ui_agent_rail(), 0.0);
    ui_fill_rect(r, x + w - 1.0 * s, y, 1.0 * s, h, ui_agent_line(), 0.0);
    ui_push_clip(r, x, y, w, h);
    ui_agent_draw_text_clipped(r, x + 18.0 * s, y + 18.0 * s, w - 36.0 * s, 24.0 * s, "NSCode Agent", ui_agent_title_px(), ui_agent_text());
    ui_agent_draw_label(r, x + 18.0 * s, y + 48.0 * s, "CHAT HISTORY");
    ui_agent_draw_rule(r, x + 18.0 * s, y + 72.0 * s, w - 36.0 * s);

    const i32 chat_count = (i32)(sizeof(ui_agent_chats) / sizeof(ui_agent_chats[0]));
    for (i32 i = 0; i < chat_count; i++) {
        const f64 row_y = y + 90.0 * s + (f64)i * 82.0 * s;
        if (v && v->mouse_pressed && ui_agent_hit(v, x, row_y, w, 74.0 * s)) {
            ui_agent_selected_chat = i;
        }
        ui_agent_draw_chat_row(r, x, row_y, w, i);
    }

    const f64 foot_y = y + h - 86.0 * s;
    if (foot_y > y + 440.0 * s) {
        ui_fill_round_rect(r, x + 14.0 * s, foot_y, w - 28.0 * s, 62.0 * s, 7.0 * s, ui_agent_surface(), 0.0);
        ui_agent_draw_label(r, x + 28.0 * s, foot_y + 13.0 * s, "WORKSPACE");
        ui_agent_draw_text_clipped(r, x + 28.0 * s, foot_y + 34.0 * s, w - 56.0 * s, 16.0 * s, "/Users/demi/os/ns", ui_agent_small_px(), ui_agent_accent());
    }
    ui_pop_clip(r);
}

static void ui_agent_draw_step(ui_renderer *r, f64 x, f64 y, f64 w, const ui_agent_step *step) {
    const f64 s = ui_agent_scale();
    const u32 dot = step->done ? ui_agent_accent() : ui_agent_yellow();
    ui_fill_round_rect(r, x + 7.0 * s, y + 12.0 * s, 10.0 * s, 10.0 * s, 5.0 * s, dot, 0.0);
    ui_agent_draw_text_clipped(r, x + 32.0 * s, y + 5.0 * s, 52.0 * s, 16.0 * s, step->kind, ui_agent_small_px(), ui_agent_blue());
    ui_agent_draw_text_clipped(r, x + 92.0 * s, y + 5.0 * s, w - 100.0 * s, 20.0 * s, step->title, ui_agent_font_px(), ui_agent_text());
    ui_agent_draw_text_clipped(r, x + 32.0 * s, y + 29.0 * s, w - 40.0 * s, 16.0 * s, step->detail, ui_agent_small_px(), ui_agent_muted());
    ui_fill_rect(r, x + 12.0 * s, y + 28.0 * s, 1.0 * s, 33.0 * s, ui_agent_line(), 0.0);
    ns_unused(w);
}

static void ui_agent_draw_dialog(ui_renderer *r, view *v, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    const f64 header_h = 58.0 * s;
    ui_fill_rect(r, x, y, w, h, ui_agent_bg(), 0.0);
    ui_fill_rect(r, x, y, w, header_h, ui_agent_bg(), 0.0);
    ui_agent_draw_text_clipped(r, x + 24.0 * s, y + 17.0 * s, w - 190.0 * s, 24.0 * s, "Agent Dialog", ui_agent_title_px(), ui_agent_text());
    ui_agent_draw_pill(r, x + w - 152.0 * s, y + 17.0 * s, "planning", ui_agent_surface(), ui_agent_accent());
    ui_agent_draw_rule(r, x, y + header_h, w);

    const f64 content_y = y + header_h;
    const f64 content_h = h - header_h;
    const f64 line_h = 54.0 * s;
    const f64 content_total = 500.0 * s;
    const f64 max_scroll = fmax(0.0, content_total - content_h);
    if (v && ui_agent_hit(v, x, content_y, w, content_h)) {
        ui_agent_dialog_scroll_y = ui_clamp_f64(ui_agent_dialog_scroll_y - v->scroll_y, 0.0, max_scroll);
    }

    ui_push_clip(r, x, content_y, w, content_h);
    const f64 yy = content_y + 22.0 * s - ui_agent_dialog_scroll_y;
    ui_fill_round_rect(r, x + 24.0 * s, yy, w - 48.0 * s, 92.0 * s, 8.0 * s, ui_agent_surface(), 0.0);
    ui_agent_draw_label(r, x + 42.0 * s, yy + 16.0 * s, "USER");
    ui_agent_draw_text_clipped(r, x + 42.0 * s, yy + 40.0 * s, w - 84.0 * s, 20.0 * s, "Make nscode/native feel like an agentic coding app.", ui_agent_font_px(), ui_agent_text());
    ui_agent_draw_text_clipped(r, x + 42.0 * s, yy + 64.0 * s, w - 84.0 * s, 20.0 * s, "Add chat history, agent timeline, and local Git diff review.", ui_agent_font_px(), ui_agent_text());

    const f64 agent_y = yy + 124.0 * s;
    ui_fill_round_rect(r, x + 24.0 * s, agent_y, w - 48.0 * s, 312.0 * s, 8.0 * s, ui_agent_surface(), 0.0);
    ui_agent_draw_label(r, x + 42.0 * s, agent_y + 16.0 * s, "AGENT");
    ui_agent_draw_text_clipped(r, x + 42.0 * s, agent_y + 40.0 * s, w - 84.0 * s, 20.0 * s, "Working plan", ui_agent_font_px(), ui_agent_text());
    ui_agent_draw_text_clipped(r, x + 42.0 * s, agent_y + 64.0 * s, w - 84.0 * s, 16.0 * s, "Runtime bridge owns the app shell until NS callbacks can drive frames.", ui_agent_small_px(), ui_agent_muted());

    const i32 step_count = (i32)(sizeof(ui_agent_steps) / sizeof(ui_agent_steps[0]));
    for (i32 i = 0; i < step_count; i++) {
        ui_agent_draw_step(r, x + 42.0 * s, agent_y + 98.0 * s + (f64)i * line_h, w - 84.0 * s, &ui_agent_steps[i]);
    }

    ui_fill_round_rect(r, x + 42.0 * s, agent_y + 264.0 * s, w - 84.0 * s, 32.0 * s, 6.0 * s, ui_agent_bg(), 0.0);
    ui_agent_draw_text_clipped(r, x + 56.0 * s, agent_y + 273.0 * s, w - 112.0 * s, 16.0 * s, "Next: run build checks and ask for visual review.", ui_agent_small_px(), ui_agent_yellow());
    ui_pop_clip(r);
}

static void ui_agent_copy_line(char *dst, size_t dst_cap, const char *start, size_t len) {
    if (dst_cap == 0) return;
    if (len >= dst_cap) len = dst_cap - 1;
    memcpy(dst, start, len);
    dst[len] = '\0';
}

static u32 ui_agent_diff_line_color(const char *line) {
    if (!line || !line[0]) return ui_agent_muted();
    if (strncmp(line, "diff --git", 10) == 0 || strncmp(line, "index ", 6) == 0) return ui_agent_blue();
    if (strncmp(line, "@@", 2) == 0) return ui_agent_hunk();
    if (line[0] == '+' && strncmp(line, "+++", 3) != 0) return ui_agent_add();
    if (line[0] == '-' && strncmp(line, "---", 3) != 0) return ui_agent_del();
    if (strncmp(line, "+++", 3) == 0 || strncmp(line, "---", 3) == 0) return ui_agent_yellow();
    return ui_agent_text();
}

static u32 ui_agent_diff_line_bg(const char *line) {
    if (!line || !line[0]) return 0;
    if (strncmp(line, "@@", 2) == 0) return ui_agent_hunk_bg();
    if (line[0] == '+' && strncmp(line, "+++", 3) != 0) return ui_agent_green_bg();
    if (line[0] == '-' && strncmp(line, "---", 3) != 0) return ui_agent_red_bg();
    return 0;
}

static void ui_agent_draw_diff(ui_renderer *r, view *v, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    const f64 header_h = 58.0 * s;
    const f64 line_h = 22.0 * s;
    ui_fill_rect(r, x, y, w, h, ui_agent_surface(), 0.0);
    ui_fill_rect(r, x, y, 1.0 * s, h, ui_agent_line(), 0.0);
    ui_agent_draw_text_clipped(r, x + 18.0 * s, y + 17.0 * s, w - 178.0 * s, 24.0 * s, "Local Diff", ui_agent_title_px(), ui_agent_text());
    ui_agent_draw_pill(r, x + w - 148.0 * s, y + 17.0 * s, ui_agent_diff_error ? "error" : "git diff", ui_agent_bg(), ui_agent_diff_error ? ui_agent_red() : ui_agent_accent());
    ui_agent_draw_rule(r, x, y + header_h, w);

    if (!ui_agent_diff_loaded) {
        ui_agent_load_git_diff();
    }

    const i32 line_count = ui_agent_count_lines(ui_agent_diff_text);
    const f64 body_y = y + header_h;
    const f64 body_h = h - header_h;
    const f64 content_h = fmax(body_h, (f64)(line_count + 2) * line_h);
    const f64 max_scroll = fmax(0.0, content_h - body_h);
    if (v && ui_agent_hit(v, x, body_y, w, body_h)) {
        ui_agent_diff_scroll_y = ui_clamp_f64(ui_agent_diff_scroll_y - v->scroll_y, 0.0, max_scroll);
    }

    ui_push_clip(r, x, body_y, w, body_h);
    if (ui_agent_diff_error || ui_agent_diff_len == 0) {
        const u32 tone = ui_agent_diff_error ? ui_agent_red() : ui_agent_muted();
        ui_fill_round_rect(r, x + 18.0 * s, body_y + 24.0 * s, w - 36.0 * s, 84.0 * s, 8.0 * s, ui_agent_bg(), 0.0);
        ui_agent_draw_text_clipped(r, x + 34.0 * s, body_y + 46.0 * s, w - 68.0 * s, 20.0 * s, ui_agent_diff_message, ui_agent_font_px(), tone);
        if (ui_agent_diff_error && ui_agent_diff_text && ui_agent_diff_text[0]) {
            char msg[220];
            const char *end = strchr(ui_agent_diff_text, '\n');
            const size_t len = end ? (size_t)(end - ui_agent_diff_text) : strlen(ui_agent_diff_text);
            ui_agent_copy_line(msg, sizeof(msg), ui_agent_diff_text, len);
            ui_agent_draw_text_clipped(r, x + 34.0 * s, body_y + 72.0 * s, w - 68.0 * s, 16.0 * s, msg, ui_agent_small_px(), ui_agent_muted());
        } else {
            ui_agent_draw_text_clipped(r, x + 34.0 * s, body_y + 72.0 * s, w - 68.0 * s, 16.0 * s, "Modify a tracked file to populate this review pane.", ui_agent_small_px(), ui_agent_muted());
        }
        ui_pop_clip(r);
        return;
    }

    if (ui_agent_diff_truncated) {
        ui_agent_draw_text_clipped(r, x + 18.0 * s, body_y + 10.0 * s - ui_agent_diff_scroll_y, w - 36.0 * s, 16.0 * s, ui_agent_diff_message, ui_agent_small_px(), ui_agent_yellow());
    }

    const char *p = ui_agent_diff_text;
    i32 line_no = 0;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        const f64 row_y = body_y + 18.0 * s + (f64)line_no * line_h - ui_agent_diff_scroll_y;
        if (row_y + line_h >= body_y && row_y <= body_y + body_h) {
            char line[420];
            ui_agent_copy_line(line, sizeof(line), p, len);
            const u32 bg = ui_agent_diff_line_bg(line);
            if (bg != 0) {
                ui_fill_rect(r, x, row_y, w, line_h, bg, 0.0);
            }
            char num[16];
            snprintf(num, sizeof(num), "%d", line_no + 1);
            ui_agent_draw_text_clipped(r, x + 12.0 * s, row_y + 5.0 * s, 38.0 * s, 16.0 * s, num, ui_agent_small_px(), ui_agent_muted());
            ui_agent_draw_text_clipped(r, x + 58.0 * s, row_y + 5.0 * s, w - 68.0 * s, 16.0 * s, line, ui_agent_small_px(), ui_agent_diff_line_color(line));
        }
        line_no++;
        if (!end) break;
        p = end + 1;
    }
    ui_pop_clip(r);

    if (content_h > body_h) {
        const f64 track_w = 8.0 * s;
        const f64 thumb_h = fmax(28.0 * s, body_h * body_h / content_h);
        const f64 thumb_y = body_y + (max_scroll > 0.0 ? ui_agent_diff_scroll_y / max_scroll * (body_h - thumb_h) : 0.0);
        ui_fill_rect(r, x + w - track_w, body_y, track_w, body_h, ui_agent_bg(), 0.0);
        ui_fill_round_rect(r, x + w - track_w + 1.0 * s, thumb_y, track_w - 2.0 * s, thumb_h, 3.0 * s, ui_agent_line(), 0.0);
    }
}

static void ui_agent_status_bar(ui_renderer *r, view *v, f64 x, f64 y, f64 w) {
    const f64 s = ui_agent_scale();
    const char *mouse = (v && v->mouse_down) ? "mouse down" : "ready";
    ui_fill_rect(r, x, y, w, 28.0 * s, ui_agent_accent(), 0.0);
    ui_agent_draw_text_clipped(r, x + 14.0 * s, y + 8.0 * s, 220.0 * s, 16.0 * s, "nscode/native agent", ui_agent_small_px(), ui_agent_bg());
    if (w > 760.0 * s) {
        ui_agent_draw_text_clipped(r, x + w - 350.0 * s, y + 8.0 * s, 80.0 * s, 16.0 * s, mouse, ui_agent_small_px(), ui_agent_bg());
        ui_agent_draw_text_clipped(r, x + w - 250.0 * s, y + 8.0 * s, 236.0 * s, 16.0 * s, ui_agent_diff_message, ui_agent_small_px(), ui_agent_bg());
    }
}

static void ui_code_draw_token(ui_renderer *r, f64 *x, f64 y, const char *text, u32 color) {
    const f64 px = ui_agent_font_px();
    ui_draw_text(r, *x, y, text, px, color, UI_FONT_MONO);
    *x += ui_text_width(r, text, px, UI_FONT_MONO);
}

static void ui_code_draw_line(ui_renderer *r, f64 x, f64 y, i32 line_no) {
    const u32 text = ui_agent_text();
    const u32 muted = ui_agent_muted();
    const u32 kw = ui_code_editor_keyword();
    const u32 str = ui_code_editor_string();
    const u32 num = ui_code_editor_number();
    const u32 comment = ui_code_editor_comment();
    char ln[8];
    snprintf(ln, sizeof(ln), "%2d", line_no);
    ui_agent_draw_text_clipped(r, x, y, 34.0 * ui_agent_scale(), 18.0 * ui_agent_scale(), ln, ui_agent_small_px(), muted);

    f64 cx = x + 52.0 * ui_agent_scale();
    switch (line_no) {
        case 1:
            ui_code_draw_token(r, &cx, y, "// ns native editor", comment);
            break;
        case 2:
            ui_code_draw_token(r, &cx, y, "// classic texture editor mode", comment);
            break;
        case 3:
            break;
        case 4:
            ui_code_draw_token(r, &cx, y, "fn", kw);
            ui_code_draw_token(r, &cx, y, " main", text);
            ui_code_draw_token(r, &cx, y, "() ", muted);
            ui_code_draw_token(r, &cx, y, "{", text);
            break;
        case 5:
            ui_code_draw_token(r, &cx, y, "    let", kw);
            ui_code_draw_token(r, &cx, y, " answer ", text);
            ui_code_draw_token(r, &cx, y, "= ", muted);
            ui_code_draw_token(r, &cx, y, "42", num);
            break;
        case 6:
            ui_code_draw_token(r, &cx, y, "    print", text);
            ui_code_draw_token(r, &cx, y, "(", muted);
            ui_code_draw_token(r, &cx, y, "\"ready\"", str);
            ui_code_draw_token(r, &cx, y, ")", muted);
            break;
        case 7:
            ui_code_draw_token(r, &cx, y, "}", text);
            break;
        default:
            ui_code_draw_token(r, &cx, y, "", text);
            break;
    }
}

static void ui_code_draw_texture_panel(ui_renderer *r, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    const u32 tex_colors[] = {
        0xff70cfffu, 0xff94e2a6u, 0xffffcf70u, 0xffff7b86u,
        0xff8fb8ffu, 0xff27313au, 0xffd7dde5u, 0xff61d394u,
    };
    ui_fill_rect(r, x, y, w, h, ui_code_editor_panel(), 0.0);
    ui_fill_rect(r, x, y, 1.0 * s, h, ui_agent_line(), 0.0);
    ui_agent_draw_text_clipped(r, x + 18.0 * s, y + 18.0 * s, w - 36.0 * s, 20.0 * s, "Parse Texture", ui_agent_title_px(), ui_agent_text());
    ui_agent_draw_text_clipped(r, x + 18.0 * s, y + 48.0 * s, w - 36.0 * s, 16.0 * s, "tokens x functions", ui_agent_small_px(), ui_agent_muted());
    ui_agent_draw_rule(r, x, y + 76.0 * s, w);

    const f64 grid_x = x + 18.0 * s;
    const f64 grid_y = y + 102.0 * s;
    const f64 cell = fmax(4.0 * s, fmin(12.0 * s, (w - 36.0 * s) / 18.0));
    const i32 cols = (i32)fmax(1.0, floor((w - 36.0 * s) / cell));
    const i32 rows = (i32)fmax(1.0, floor((h - 178.0 * s) / cell));
    ui_push_clip(r, grid_x, grid_y, w - 36.0 * s, h - 150.0 * s);
    for (i32 row = 0; row < rows; row++) {
        for (i32 col = 0; col < cols; col++) {
            const i32 idx = (row * 5 + col * 3 + (col / 4)) & 7;
            const f64 alpha_skip = ((row + col) % 11 == 0) ? 0.38 : 1.0;
            u32 c = tex_colors[idx];
            if (alpha_skip < 1.0) {
                c = ui_code_editor_panel_hi();
            }
            ui_fill_rect(r, grid_x + (f64)col * cell, grid_y + (f64)row * cell, cell - 1.0 * s, cell - 1.0 * s, c, 0.0);
        }
    }
    ui_pop_clip(r);

    const f64 legend_y = y + h - 46.0 * s;
    ui_agent_draw_pill(r, x + 18.0 * s, legend_y, "classic", ui_agent_bg(), ui_agent_accent());
    ui_agent_draw_text_clipped(r, x + 106.0 * s, legend_y + 5.0 * s, w - 124.0 * s, 16.0 * s, "texture-backed code view", ui_agent_small_px(), ui_agent_muted());
}

static void ui_code_draw_editor(ui_renderer *r, view *v, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_agent_scale();
    const f64 tab_h = 42.0 * s;
    const f64 line_h = 24.0 * s;
    ui_fill_rect(r, x, y, w, h, ui_code_editor_bg(), 0.0);
    ui_fill_rect(r, x, y, w, tab_h, ui_code_editor_panel(), 0.0);
    ui_fill_rect(r, x, y + tab_h - 1.0 * s, w, 1.0 * s, ui_agent_line(), 0.0);
    ui_fill_round_rect(r, x + 14.0 * s, y + 9.0 * s, 156.0 * s, 32.0 * s, 6.0 * s, ui_code_editor_panel_hi(), 0.0);
    ui_agent_draw_text_clipped(r, x + 30.0 * s, y + 18.0 * s, 130.0 * s, 16.0 * s, "main.ns", ui_agent_small_px(), ui_agent_text());

    const f64 body_y = y + tab_h;
    const f64 body_h = h - tab_h;
    const f64 gutter_w = 72.0 * s;
    const f64 max_scroll = fmax(0.0, 14.0 * line_h - body_h);
    if (v && ui_agent_hit(v, x, body_y, w, body_h)) {
        ui_code_scroll_y = ui_clamp_f64(ui_code_scroll_y - v->scroll_y, 0.0, max_scroll);
    }

    ui_fill_rect(r, x, body_y, gutter_w, body_h, ui_code_editor_gutter(), 0.0);
    ui_fill_rect(r, x + gutter_w - 1.0 * s, body_y, 1.0 * s, body_h, ui_agent_line(), 0.0);
    ui_push_clip(r, x, body_y, w, body_h);
    for (i32 i = 1; i <= 12; i++) {
        const f64 ly = body_y + 22.0 * s + (f64)(i - 1) * line_h - ui_code_scroll_y;
        if (ly + line_h >= body_y && ly <= body_y + body_h) {
            if (i == 5) {
                ui_fill_rect(r, x + gutter_w, ly - 4.0 * s, w - gutter_w, line_h, ui_code_editor_panel(), 0.0);
            }
            ui_code_draw_line(r, x + 20.0 * s, ly, i);
        }
    }
    const f64 cursor_x = x + gutter_w + 136.0 * s;
    const f64 cursor_y = body_y + 22.0 * s + 4.0 * line_h - ui_code_scroll_y;
    ui_fill_rect(r, cursor_x, cursor_y - 2.0 * s, 2.0 * s, 20.0 * s, ui_code_editor_cursor(), 0.0);
    ui_pop_clip(r);
}

static void ui_code_status_bar(ui_renderer *r, f64 x, f64 y, f64 w) {
    const f64 s = ui_agent_scale();
    ui_fill_rect(r, x, y, w, 28.0 * s, ui_code_editor_panel_hi(), 0.0);
    ui_agent_draw_text_clipped(r, x + 14.0 * s, y + 8.0 * s, 240.0 * s, 16.0 * s, "nscode/native code", ui_agent_small_px(), ui_agent_text());
    if (w > 680.0 * s) {
        ui_agent_draw_text_clipped(r, x + w - 296.0 * s, y + 8.0 * s, 282.0 * s, 16.0 * s, "Ln 5, Col 18  |  Nano Script", ui_agent_small_px(), ui_agent_muted());
    }
}

static void ui_code_draw(ui_renderer *r, view *v) {
    ui_resize(r);
    const f64 s = ui_agent_scale();
    const f64 w = (f64)ui_canvas_width(r);
    const f64 h = (f64)ui_canvas_height(r);
    const f64 title_h = 38.0 * s;
    const f64 status_h = 28.0 * s;
    const f64 app_y = title_h;
    const f64 app_h = fmax(1.0, h - title_h - status_h);
    f64 texture_w = 288.0 * s;
    if (w < 820.0 * s) texture_w = 220.0 * s;
    if (w < 620.0 * s) texture_w = 0.0;
    const f64 editor_w = fmax(1.0, w - texture_w);

    ui_fill_rect(r, 0.0, 0.0, w, h, ui_code_editor_bg(), 0.0);
    ui_agent_draw_title_bar(r, v, 0.0, 0.0, w, title_h);
    ui_code_draw_editor(r, v, 0.0, app_y, editor_w, app_h);
    if (texture_w > 0.0) {
        ui_code_draw_texture_panel(r, editor_w, app_y, texture_w, app_h);
    }
    ui_code_status_bar(r, 0.0, h - status_h, w);
}

static void ui_agent_draw(ui_renderer *r, view *v) {
    if (ui_agent_current_mode == UI_AGENT_MODE_CODE) {
        ui_code_draw(r, v);
        return;
    }

    ui_resize(r);
    const f64 s = ui_agent_scale();
    const f64 w = (f64)ui_canvas_width(r);
    const f64 h = (f64)ui_canvas_height(r);
    const f64 title_h = 38.0 * s;
    const f64 status_h = 28.0 * s;
    const f64 app_y = title_h;
    const f64 app_h = fmax(1.0, h - title_h - status_h);

    f64 history_w = 276.0 * s;
    f64 diff_w = 430.0 * s;
    if (w < 980.0 * s) {
        history_w = 214.0 * s;
        diff_w = 330.0 * s;
    }
    if (w < 740.0 * s) {
        history_w = 180.0 * s;
        diff_w = 0.0;
    }
    const f64 dialog_x = history_w;
    const f64 dialog_w = fmax(1.0, w - history_w - diff_w);
    const f64 diff_x = history_w + dialog_w;

    if ((ui_agent_frame_count % UI_AGENT_DIFF_REFRESH_FRAMES) == 0) {
        ui_agent_load_git_diff();
    }
    ui_agent_frame_count++;

    ui_fill_rect(r, 0.0, 0.0, w, h, ui_agent_bg(), 0.0);
    ui_agent_draw_title_bar(r, v, 0.0, 0.0, w, title_h);
    ui_agent_draw_history(r, v, 0.0, app_y, history_w, app_h);
    ui_agent_draw_dialog(r, v, dialog_x, app_y, dialog_w, app_h);
    if (diff_w > 0.0) {
        ui_agent_draw_diff(r, v, diff_x, app_y, diff_w, app_h);
    }
    ui_agent_status_bar(r, v, 0.0, h - status_h, w);
}

static void ui_agent_frame(view *v) {
    if (!ui_agent_renderer) return;
    ui_begin_frame(ui_agent_renderer);
    ui_agent_draw(ui_agent_renderer, v ? v : ui_agent_view);
    ui_color_rgba clear = ui_agent_clear();
    ui_flush(ui_agent_renderer, &clear);
    ui_agent_reset_input(v);
}

static void ui_agent_terminate(view *v) {
    ns_unused(v);
    if (ui_agent_renderer) {
        ui_renderer_destroy(ui_agent_renderer);
        ui_agent_renderer = NULL;
    }
    ui_agent_view = NULL;
    ui_agent_free_diff();
    ui_agent_diff_loaded = false;
}

static void ui_agent_attach_mode(view *v, ui_agent_mode mode) {
    if (!v) return;
    ui_agent_terminate(v);
    ui_agent_view = v;
    ui_agent_current_mode = mode;
    ui_agent_renderer = ui_renderer_create(v);
    if (!ui_agent_renderer) return;
    ui_agent_diff_scroll_y = 0.0;
    ui_agent_dialog_scroll_y = 0.0;
    ui_code_scroll_y = 0.0;
    ui_agent_selected_chat = 0;
    ui_agent_frame_count = 0;
    ui_agent_load_git_diff();
    ui_resize(ui_agent_renderer);
    v->on_frame = (void*)(ui_view_callback)ui_agent_frame;
    v->on_terminate = (void*)(ui_view_callback)ui_agent_terminate;
}

void ui_agentic_coding_attach(view *v) {
    ui_agent_attach_mode(v, UI_AGENT_MODE_AGENTIC);
}

void ui_code_editor_attach(view *v) {
    ui_agent_attach_mode(v, UI_AGENT_MODE_CODE);
}
