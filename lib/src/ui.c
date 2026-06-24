#include "gpu.h"
#include "ns_type.h"
#include "view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_VERTEX_STRIDE 36
#define UI_INITIAL_VERTEX_CAP 131072
#define UI_MAX_COMMANDS 4096
#define UI_MAX_CLIPS 32
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
    f32 range, weight, softness, pad;
} ui_vertex;

typedef struct ui_clip {
    f64 x, y, w, h;
} ui_clip;

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
    i32 current_texture_id;

    ui_font fonts[2];
    gpu_texture white_texture;
    gpu_texture font_texture;
    gpu_buffer screen_buffer;
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
"struct VOut { float4 pos [[position]]; float2 uv; float4 col; float4 params; };\n"
"vertex VOut ui_vs(VIn in [[stage_in]], constant float2 &screen [[buffer(1)]]) {\n"
"  VOut o; float2 ndc = float2((in.pos.x / screen.x) * 2.0 - 1.0, 1.0 - (in.pos.y / screen.y) * 2.0);\n"
"  o.pos = float4(ndc, 0.0, 1.0); o.uv = in.uv; o.col = float4(in.col) / 255.0; o.params = in.params; return o;\n"
"}\n"
"static inline half ui_median3(half r, half g, half b) { return max(min(r, g), min(max(r, g), b)); }\n"
"fragment float4 ui_fs_image(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
"  constexpr sampler samp(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
"  return tex.sample(samp, in.uv) * in.col;\n"
"}\n"
"fragment float4 ui_fs_msdf(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
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

static ns_bool ui_push_vertex(ui_renderer *r, f64 x, f64 y, f64 u, f64 v, u32 color, f64 range, f64 weight, f64 softness) {
    if (r->vertex_count >= r->vertex_capacity) return false;
    r->vertices[r->vertex_count++] = (ui_vertex){
        .x = (f32)x, .y = (f32)y, .u = (f32)u, .v = (f32)v, .color = color,
        .range = (f32)range, .weight = (f32)weight, .softness = (f32)softness, .pad = 0,
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
    i32 base = r->vertex_count;
    if (!ui_push_vertex(r, cx0, cy0, cu0, cv0, color, range, weight, softness) ||
        !ui_push_vertex(r, cx1, cy0, cu1, cv0, color, range, weight, softness) ||
        !ui_push_vertex(r, cx1, cy1, cu1, cv1, color, range, weight, softness) ||
        !ui_push_vertex(r, cx0, cy0, cu0, cv0, color, range, weight, softness) ||
        !ui_push_vertex(r, cx1, cy1, cu1, cv1, color, range, weight, softness) ||
        !ui_push_vertex(r, cx0, cy1, cu0, cv1, color, range, weight, softness)) {
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
    i32 base = r->vertex_count;
    if (!ui_push_vertex(r, x0, y0, u, v, color, 0, 0, 0) ||
        !ui_push_vertex(r, x1, y1, u, v, color, 0, 0, 0) ||
        !ui_push_vertex(r, x2, y2, u, v, color, 0, 0, 0)) {
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
        .buffers = {{.buffer = r->screen_buffer, .name = ns_str_cstr("screen")}},
        .textures = {{.texture = r->white_texture, .name = ns_str_cstr("tex")}},
    });
    r->binding_font_msdf = gpu_create_binding(&(gpu_binding_desc){
        .pipeline = r->pipeline_msdf,
        .buffers = {{.buffer = r->screen_buffer, .name = ns_str_cstr("screen")}},
        .textures = {{.texture = r->font_texture, .name = ns_str_cstr("tex")}},
    });
    r->mesh = gpu_create_mesh(&(gpu_mesh_desc){
        .buffers = {r->vertex_buffer},
        .pipeline = r->pipeline_image,
    });
    r->screen_pass = (gpu_render_pass){.id = 0};
    r->gpu_ready = r->screen_buffer.id && r->vertex_buffer.id && r->white_texture.id &&
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
    r->clips[0] = (ui_clip){0, 0, r->width, r->height};
    r->current_texture_id = UI_WHITE_TEXTURE;
}

void ui_flush(ui_renderer *r, ui_color_rgba *clear) {
    if (!r || !r->gpu_ready) return;
    gpu_update_buffer(r->vertex_buffer, (ns_data){r->vertices, (size_t)r->vertex_count * UI_VERTEX_STRIDE});
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

// ---- native code-editor bridge -------------------------------------------
// Nano Script cannot yet assign function values into view.on_frame reliably, so
// the native demo installs a small C frame callback and keeps the renderer alive
// for the lifetime of the app loop. The draw commands mirror nscode/native's
// script renderer and the ~/os/ui code_editor plugin layout: file tree, tabs,
// gutter, code surface, caret, scrollbar, and status bar.

typedef void(*ui_view_callback)(view*);

static ui_renderer *ui_code_editor_renderer;
static view *ui_code_editor_view;
static f64 ui_code_scroll_y;
static i32 ui_code_caret_line;
static i32 ui_code_hover_line = -1;

static f64 ui_code_scale(void) {
    return (ui_code_editor_view && ui_code_editor_view->ui_scale > 0.0) ? ui_code_editor_view->ui_scale : 1.0;
}

static f64 ui_native_font_px(void) { return 18.0 * ui_code_scale(); }
static f64 ui_native_line_pad(void) { return 5.0 * ui_code_scale(); }
static f64 ui_native_gutter_pad(void) { return 10.0 * ui_code_scale(); }

static u32 ui_col_bg(void) { return ui_pack_color("#1e1e2e"); }
static u32 ui_col_gutter(void) { return ui_pack_color("#181825"); }
static u32 ui_col_text(void) { return ui_pack_color("#cdd6f4"); }
static u32 ui_col_dim(void) { return ui_pack_color("#6c7086"); }
static u32 ui_col_sel(void) { return ui_pack_color("#414b6b"); }
static u32 ui_col_caret(void) { return ui_pack_color("#f5e0dc"); }
static u32 ui_col_track(void) { return ui_pack_color("#11111b"); }
static u32 ui_col_thumb(void) { return ui_pack_color("#45475a"); }
static u32 ui_col_panel(void) { return ui_pack_color("#303446"); }
static u32 ui_col_accent(void) { return ui_pack_color("#8bd5ca"); }
static u32 ui_col_sidebar(void) { return ui_pack_color("#181926"); }
static u32 ui_col_sidebar_alt(void) { return ui_pack_color("#1e2030"); }
static u32 ui_col_tab(void) { return ui_pack_color("#24273a"); }
static u32 ui_col_line_hot(void) { return ui_pack_color("#292c3c"); }
static u32 ui_col_border(void) { return ui_pack_color("#494d64"); }
static u32 ui_col_keyword(void) { return ui_pack_color("#8aadf4"); }
static u32 ui_col_string(void) { return ui_pack_color("#f0c6a6"); }
static u32 ui_col_comment(void) { return ui_pack_color("#6e738d"); }
static u32 ui_col_number(void) { return ui_pack_color("#a6da95"); }

static ui_color_rgba ui_native_clear(void) {
    return (ui_color_rgba){0.118, 0.118, 0.180, 1.0};
}

static void ui_code_file_row(ui_renderer *r, f64 x, f64 y, f64 w, const char *text, i32 depth, ns_bool selected, ns_bool folder) {
    const f64 s = ui_code_scale();
    const f64 row_h = 24.0 * s;
    if (selected) {
        ui_fill_round_rect(r, x + 8.0 * s, y + 2.0 * s, w - 16.0 * s, row_h - 4.0 * s, 5.0 * s, ui_col_sel(), 0.0);
    }

    const f64 tx = x + 14.0 * s + (f64)depth * 16.0 * s;
    if (folder) {
        ui_draw_text(r, tx, y + 6.0 * s, "v", 13.0 * s, ui_col_dim(), UI_FONT_MONO);
        ui_draw_text(r, tx + 17.0 * s, y + 6.0 * s, text, 13.0 * s, ui_col_text(), UI_FONT_MONO);
    } else {
        ui_draw_text(r, tx, y + 6.0 * s, "-", 13.0 * s, ui_col_accent(), UI_FONT_MONO);
        ui_draw_text(r, tx + 17.0 * s, y + 6.0 * s, text, 13.0 * s, ui_col_dim(), UI_FONT_MONO);
    }
}

static void ui_code_file_tree(ui_renderer *r, f64 x, f64 y, f64 w, f64 h) {
    const f64 s = ui_code_scale();
    ui_fill_rect(r, x, y, w, h, ui_col_sidebar(), 0.0);
    ui_fill_rect(r, x + w - 1.0 * s, y, 1.0 * s, h, ui_col_border(), 0.0);
    ui_draw_text(r, x + 14.0 * s, y + 16.0 * s, "EXPLORER", 12.0 * s, ui_col_dim(), UI_FONT_MONO);
    ui_fill_rect(r, x + 12.0 * s, y + 42.0 * s, w - 24.0 * s, 1.0 * s, ui_col_border(), 0.0);

    ui_code_file_row(r, x, y + 56.0 * s, w, "nscode", 0, false, true);
    ui_code_file_row(r, x, y + 84.0 * s, w, "native", 1, true, true);
    ui_code_file_row(r, x, y + 112.0 * s, w, "main.ns", 2, false, false);
    ui_code_file_row(r, x, y + 140.0 * s, w, "render.ns", 2, true, false);
    ui_code_file_row(r, x, y + 168.0 * s, w, "editor.ns", 2, false, false);
    ui_code_file_row(r, x, y + 196.0 * s, w, "README.md", 2, false, false);

    const f64 info_y = y + h - 92.0 * s;
    if (info_y > y + 230.0 * s) {
        ui_fill_round_rect(r, x + 12.0 * s, info_y, w - 24.0 * s, 66.0 * s, 8.0 * s, ui_col_sidebar_alt(), 0.0);
        ui_draw_text(r, x + 24.0 * s, info_y + 14.0 * s, "renderer", 12.0 * s, ui_col_dim(), UI_FONT_MONO);
        ui_draw_text(r, x + 24.0 * s, info_y + 36.0 * s, "Metal / MSDF", 13.0 * s, ui_col_accent(), UI_FONT_MONO);
    }
}

static void ui_code_tab_bar(ui_renderer *r, f64 x, f64 y, f64 w) {
    const f64 s = ui_code_scale();
    const f64 tab_h = 38.0 * s;
    ui_fill_rect(r, x, y, w, tab_h, ui_col_tab(), 0.0);
    ui_fill_rect(r, x, y + tab_h - 1.0 * s, w, 1.0 * s, ui_col_border(), 0.0);
    ui_fill_round_rect(r, x + 10.0 * s, y + 7.0 * s, 116.0 * s, 24.0 * s, 5.0 * s, ui_col_panel(), 0.0);
    ui_fill_rect(r, x + 10.0 * s, y + 30.0 * s, 116.0 * s, 2.0 * s, ui_col_accent(), 0.0);
    ui_draw_text(r, x + 24.0 * s, y + 13.0 * s, "render.ns", 13.0 * s, ui_col_text(), UI_FONT_MONO);
    ui_draw_text(r, x + 146.0 * s, y + 13.0 * s, "main.ns", 13.0 * s, ui_col_dim(), UI_FONT_MONO);
    if (w > 420.0 * s) {
        ui_draw_text(r, x + w - 186.0 * s, y + 13.0 * s, "Nano Script Native", 13.0 * s, ui_col_dim(), UI_FONT_MONO);
    }
}

static void ui_code_status_bar(ui_renderer *r, view *v, f64 x, f64 y, f64 w) {
    const f64 s = ui_code_scale();
    const char *mouse_label = (v && v->mouse_down) ? "Mouse down" : "Mouse ready";
    ui_fill_rect(r, x, y, w, 28.0 * s, ui_col_accent(), 0.0);
    ui_draw_text(r, x + 12.0 * s, y + 8.0 * s, "nscode/native", 12.0 * s, ui_col_sidebar(), UI_FONT_MONO);
    if (w > 640.0 * s) {
        char pos[64];
        snprintf(pos, sizeof(pos), "Ln %d   %s", ui_code_caret_line + 1, mouse_label);
        ui_draw_text(r, x + w - 400.0 * s, y + 8.0 * s, pos, 12.0 * s, ui_col_sidebar(), UI_FONT_MONO);
        ui_draw_text(r, x + w - 236.0 * s, y + 8.0 * s, "UTF-8   LF   Nano Script", 12.0 * s, ui_col_sidebar(), UI_FONT_MONO);
    }
}

static f64 ui_code_text_segment(ui_renderer *r, f64 x, f64 y, const char *text, u32 color) {
    ui_draw_text(r, x, y, text, ui_native_font_px(), color, UI_FONT_MONO);
    return x + ui_text_width(r, text, ui_native_font_px(), UI_FONT_MONO);
}

static void ui_code_line(ui_renderer *r, f64 x, f64 y, i32 line_no, const char *number, const char **parts, const u32 *colors, i32 part_count, f64 gutter_w, f64 char_w) {
    const f64 line_h = ui_text_line_height(r, ui_native_font_px(), UI_FONT_MONO) + ui_native_line_pad();
    const f64 text_y = ui_text_v_center_y(r, y, line_h, ui_native_font_px(), UI_FONT_MONO);
    const f64 num_x = x + gutter_w - ui_native_gutter_pad() - (f64)strlen(number) * char_w;
    ui_draw_text(r, num_x, text_y, number, ui_native_font_px(), ui_col_dim(), UI_FONT_MONO);

    f64 tx = x + gutter_w;
    for (i32 i = 0; i < part_count; i++) {
        tx = ui_code_text_segment(r, tx, text_y, parts[i], colors[i]);
    }
    ns_unused(line_no);
}

static void ui_code_editor_draw(ui_renderer *r, view *v) {
    ui_resize(r);
    const f64 s = ui_code_scale();
    const f64 w = (f64)ui_canvas_width(r);
    const f64 h = (f64)ui_canvas_height(r);
    const f64 tree_w = w < 760.0 * s ? 164.0 * s : 220.0 * s;
    const f64 top_h = 38.0 * s;
    const f64 status_h = 28.0 * s;
    const f64 editor_x = tree_w;
    const f64 editor_y = top_h;
    const f64 editor_w = fmax(1.0, w - tree_w);
    const f64 editor_h = fmax(1.0, h - top_h - status_h);

    ui_code_file_tree(r, 0.0, 0.0, tree_w, h - status_h);
    ui_code_tab_bar(r, editor_x, 0.0, editor_w);

    const f64 char_w = fmax(1.0, ui_mono_char_width(r, ui_native_font_px(), UI_FONT_MONO));
    const f64 line_h = ui_text_line_height(r, ui_native_font_px(), UI_FONT_MONO) + ui_native_line_pad();
    const f64 gutter_w = 2.0 * char_w + ui_native_gutter_pad() * 2.0;
    const f64 code_x = editor_x + gutter_w;
    const f64 code_w = fmax(1.0, editor_w - gutter_w);
    const i32 total_lines = 36;
    const f64 content_h = (f64)total_lines * line_h;
    const f64 max_scroll_y = fmax(0.0, content_h - editor_h);

    if (v) {
        ui_code_scroll_y = ui_clamp_f64(ui_code_scroll_y - v->scroll_y, 0.0, max_scroll_y);
        ui_code_hover_line = -1;
        if (v->mouse_x >= code_x && v->mouse_x < code_x + code_w && v->mouse_y >= editor_y && v->mouse_y < editor_y + editor_h) {
            ui_code_hover_line = (i32)floor((v->mouse_y - editor_y + ui_code_scroll_y) / line_h);
            if (ui_code_hover_line >= total_lines) ui_code_hover_line = -1;
            if (v->mouse_pressed && ui_code_hover_line >= 0) {
                ui_code_caret_line = ui_code_hover_line;
            }
        }
    }
    ui_code_caret_line = (i32)ui_clamp_f64((f64)ui_code_caret_line, 0.0, (f64)total_lines - 1.0);

    ui_fill_rect(r, editor_x, editor_y, editor_w, editor_h, ui_col_bg(), 0.0);
    ui_fill_rect(r, editor_x, editor_y, gutter_w, editor_h, ui_col_gutter(), 0.0);
    ui_fill_rect(r, editor_x, editor_y, editor_w, 1.0, ui_col_border(), 0.0);
    if (ui_code_hover_line >= 0) {
        const f64 hover_y = editor_y + (f64)ui_code_hover_line * line_h - ui_code_scroll_y;
        ui_fill_rect(r, editor_x, hover_y, editor_w, line_h, ui_col_panel(), 0.0);
    }
    const f64 caret_y = editor_y + (f64)ui_code_caret_line * line_h - ui_code_scroll_y;
    if (caret_y + line_h >= editor_y && caret_y <= editor_y + editor_h) {
        ui_fill_rect(r, editor_x, caret_y, editor_w, line_h, ui_col_line_hot(), 0.0);
    }

    ui_push_clip(r, code_x, editor_y, code_w, editor_h);

    const char *l1[] = {"// native code editor bridge"};
    const u32 c1[] = {ui_col_comment()};
    const char *l2[] = {"use ", "ui"};
    const u32 c2[] = {ui_col_keyword(), ui_col_text()};
    const char *l3[] = {"use ", "editor"};
    const u32 c3[] = {ui_col_keyword(), ui_col_text()};
    const char *l4[] = {""};
    const u32 c4[] = {ui_col_text()};
    const char *l5[] = {"fn ", "render_frame", "(r: ref ui_renderer) {"};
    const u32 c5[] = {ui_col_keyword(), ui_col_accent(), ui_col_text()};
    const char *l6[] = {"    ", "ui_fill_round_rect", "(r, x, y, w, h, ", "8.0", ", panel, ", "0.0", ")"};
    const u32 c6[] = {ui_col_text(), ui_col_accent(), ui_col_text(), ui_col_number(), ui_col_text(), ui_col_number(), ui_col_text()};
    const char *l7[] = {"    ", "ui_draw_text", "(r, code_x, text_y, line, ", "16.0", ", text)"};
    const u32 c7[] = {ui_col_text(), ui_col_accent(), ui_col_text(), ui_col_number(), ui_col_text()};
    const char *l8[] = {"    let title = ", "\"render.ns\""};
    const u32 c8[] = {ui_col_keyword(), ui_col_string()};
    const char *l9[] = {"}"};
    const u32 c9[] = {ui_col_text()};

    const char **lines[] = {l1, l2, l3, l4, l5, l6, l7, l8, l9};
    const u32 *cols[] = {c1, c2, c3, c4, c5, c6, c7, c8, c9};
    const i32 counts[] = {1, 2, 2, 1, 3, 7, 5, 2, 1};
    const i32 first_line = (i32)floor(ui_code_scroll_y / line_h);
    const i32 visible_lines = (i32)ceil(editor_h / line_h) + 2;
    for (i32 i = 0; i < visible_lines; i++) {
        const i32 line = first_line + i;
        if (line >= total_lines) break;
        const i32 sample = line % 9;
        char num[16];
        snprintf(num, sizeof(num), "%d", line + 1);
        ui_code_line(r, editor_x, editor_y + (f64)line * line_h - ui_code_scroll_y, line + 1, num, lines[sample], cols[sample], counts[sample], gutter_w, char_w);
    }

    ui_fill_rect(r, code_x, caret_y + 2.0 * s, 2.0 * s, line_h - 4.0 * s, ui_col_caret(), 0.0);
    ui_pop_clip(r);

    const f64 track_w = 8.0 * s;
    const f64 track_x = editor_x + editor_w - track_w;
    ui_fill_rect(r, track_x, editor_y, track_w, editor_h, ui_col_track(), 0.0);
    const f64 thumb_h = fmax(28.0 * s, editor_h * editor_h / content_h);
    const f64 thumb_y = editor_y + (max_scroll_y > 0.0 ? ui_code_scroll_y / max_scroll_y * (editor_h - thumb_h) : 0.0);
    ui_fill_round_rect(r, track_x + 1.0 * s, thumb_y, track_w - 2.0 * s, thumb_h, 3.0 * s, ui_col_thumb(), 0.0);
    ui_code_status_bar(r, v, 0.0, h - status_h, w);
}

static void ui_code_editor_frame(view *v) {
    ns_unused(v);
    if (!ui_code_editor_renderer) return;
    ui_begin_frame(ui_code_editor_renderer);
    ui_code_editor_draw(ui_code_editor_renderer, v ? v : ui_code_editor_view);
    ui_color_rgba clear = ui_native_clear();
    ui_flush(ui_code_editor_renderer, &clear);
    if (v) {
        v->mouse_pressed = false;
        v->mouse_released = false;
        v->mouse_right_pressed = false;
        v->mouse_right_released = false;
        v->mouse_middle_pressed = false;
        v->mouse_middle_released = false;
        v->scroll_x = 0.0;
        v->scroll_y = 0.0;
    }
}

static void ui_code_editor_terminate(view *v) {
    ns_unused(v);
    if (ui_code_editor_renderer) {
        ui_renderer_destroy(ui_code_editor_renderer);
        ui_code_editor_renderer = NULL;
    }
    ui_code_editor_view = NULL;
}

void ui_code_editor_attach(view *v) {
    if (!v) return;
    ui_code_editor_terminate(v);
    ui_code_editor_view = v;
    ui_code_editor_renderer = ui_renderer_create(v);
    if (!ui_code_editor_renderer) return;
    ui_resize(ui_code_editor_renderer);
    v->on_frame = (void*)(ui_view_callback)ui_code_editor_frame;
    v->on_terminate = (void*)(ui_view_callback)ui_code_editor_terminate;
}
