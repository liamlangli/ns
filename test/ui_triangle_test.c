// Exercise the emitted geometry directly: a faded internal edge exposes the
// background even when neighbouring triangles use identical coordinates.
#include "../lib/src/ui.c"
#include <assert.h>

static void test_joined_triangles(f64 feather) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    ui_vertex vertices[64];
    r->rect = (ui_rect){0, 0, 100, 100};
    r->vertices = vertices;
    r->vertex_capacity = 64;
    const u32 translucent = 0x1affffffu;
    ui_fill_triangle(r, 50, 50, 80, 40, 80, 60, translucent, feather);
    ui_fill_triangle(r, 50, 50, 80, 60, 50, 80, translucent, feather);
    assert(r->vertex_count == 6);
    for (i32 i = 0; i < r->vertex_count; ++i) {
        assert(vertices[i].color == translucent);
    }
    // The shared edge has the same endpoints and alpha in both primitives.
    assert(vertices[0].x == vertices[3].x && vertices[0].y == vertices[3].y);
    assert(vertices[2].x == vertices[4].x && vertices[2].y == vertices[4].y);
    free(r);
}

static void test_standalone_feather(void) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    ui_vertex vertices[64];
    r->rect = (ui_rect){0, 0, 100, 100};
    r->vertices = vertices;
    r->vertex_capacity = 64;
    ui_fill_triangle(r, 10, 10, 90, 10, 50, 90, 0x80ffffffu, 1.5);
    assert(r->vertex_count == 21);
    assert(vertices[0].color == 0x80ffffffu);
    assert(vertices[3].color == 0x00ffffffu);
    assert(vertices[3].x == 10 && vertices[3].y == 10);
    free(r);
}

static void test_line_feather(f64 x0, f64 y0, f64 x1, f64 y1, f64 thickness, f64 feather) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    ui_vertex vertices[64];
    r->rect = (ui_rect){0, 0, 100, 100};
    r->vertices = vertices;
    r->vertex_capacity = 64;
    const u32 color = 0x4dffffffu;
    ui_stroke_line(r, x0, y0, x1, y1, thickness, color, feather);
    const f64 len = hypot(x1 - x0, y1 - y0);
    const f64 dx = (x1 - x0) / len, dy = (y1 - y0) / len;
    const f64 fade = fmin(fmax(feather, 0.0) * 0.5, thickness * 0.5);
    const f64 inner = thickness * 0.5 - fade;
    const f64 outer = thickness * 0.5 + fade;
    assert(r->vertex_count == (fade <= 0.0 ? 6 : (inner > 0.0 ? 18 : 12)));
    i32 transparent_count = 0;
    for (i32 i = 0; i < r->vertex_count; ++i) {
        const ui_vertex v = vertices[i];
        const f64 along = (v.x - x0) * dx + (v.y - y0) * dy;
        const f64 across = fabs((v.x - x0) * -dy + (v.y - y0) * dx);
        assert(fabs(along) < 0.00001 || fabs(along - len) < 0.00001);
        if ((v.color >> 24) == 0) {
            assert(fabs(across - outer) < 0.00001);
            assert(v.color == (color & 0x00ffffffu));
            transparent_count++;
        } else {
            assert(v.color == color);
            assert(fabs(across - inner) < 0.00001);
        }
    }
    assert(transparent_count == (fade <= 0.0 ? 0 : 6));
    free(r);
}

static void test_arc_coverage_mode(f64 feather) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    ui_vertex vertices[64];
    r->rect = (ui_rect){0, 0, 100, 100};
    r->vertices = vertices;
    r->vertex_capacity = 64;
    ui_fill_arc(r, 50, 50, 12, 2, 0, M_PI / 3, 0x4dffffffu, feather);
    assert(r->vertex_count == 6);
    assert(r->commands[0].kind == UI_KIND_ARC_SDF);
    for (i32 i = 0; i < 6; ++i) {
        assert(vertices[i].range == (feather > 0.0 ? 12 : -12));
        assert(vertices[i].color == 0x4dffffffu);
    }
    free(r);
}

// A panel 0.4 x 0.6 units one unit in front of an identity camera: the
// canvas corners land on the panel corners, and a fingertip hovers, presses
// when it crosses the plane, holds through jitter and releases on the way out.
static void test_panel(void) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    r->rect = (ui_rect){0, 0, 400, 600};
    r->safe_rect = r->rect;
    f32 identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ui_set_panel(r, identity, 0, 0, -1, 2, 0, 0, 0, 3, 0, 0.2, 0.3);
    assert(r->panel);
    // Column 0 is right * half_width, column 3 the centre: (u, v) = (1, 1) is
    // the top-right corner at (0.2, 0.3, -1).
    assert(fabs(r->panel_clip[0] - 0.2f) < 1e-6 && fabs(r->panel_clip[5] - 0.3f) < 1e-6);
    assert(fabs(r->panel_clip[14] + 1.0f) < 1e-6 && r->panel_clip[15] == 1.0f);

    ui_panel_touch t = *ui_panel_touch_update(r, 0, true, 0.1, -0.15, -0.9);
    assert(fabs(t.x - 300.0) < 1e-9 && fabs(t.y - 450.0) < 1e-9);
    assert(t.inside && t.hover && !t.down && !t.pressed && fabs(t.depth - 0.1) < 1e-9);
    t = *ui_panel_touch_update(r, 0, true, 0.1, -0.15, -1.01);
    assert(t.pressed && t.down && !t.hover);
    // Back to just in front of the plane, inside the release margin: held.
    t = *ui_panel_touch_update(r, 0, true, 0.1, -0.15, -0.998);
    assert(t.down && !t.pressed && !t.released);
    t = *ui_panel_touch_update(r, 0, true, 0.1, -0.15, -0.98);
    assert(t.released && !t.down);
    // A press needs the fingertip seen in front first; a lost one releases.
    t = *ui_panel_touch_update(r, 1, true, 0.0, 0.0, -1.2);
    assert(!t.pressed && !t.down);
    ui_panel_touch_update(r, 1, true, 0.0, 0.0, -0.95);
    t = *ui_panel_touch_update(r, 1, true, 0.0, 0.0, -1.05);
    assert(t.pressed && t.down);
    t = *ui_panel_touch_update(r, 1, false, 0.0, 0.0, -1.05);
    assert(t.released && !t.down);
    // Outside the canvas nothing hovers or presses.
    ui_panel_touch_update(r, 2, true, 0.5, 0.0, -0.95);
    t = *ui_panel_touch_update(r, 2, true, 0.5, 0.0, -1.05);
    assert(!t.inside && !t.pressed);
    ui_clear_panel(r);
    assert(!r->panel);
    free(r);
}

static void test_batch_text_range(const char *source, i32 offset, i32 length, const char *expected) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    ui_glyph latin[] = {
        {.code = 32, .x_advance = 6},
        {.code = 65, .width = 5, .height = 8, .x_offset = 1, .x_advance = 6},
        {.code = 66, .width = 5, .height = 8, .x_advance = 6, .atlas_x = 8},
    };
    ui_glyph chinese[] = {{.code = 19968, .width = 10, .height = 10, .x_advance = 10}};
    r->fonts[UI_FONT_MONO] = (ui_font){.glyphs = latin, .glyph_count = 3,
        .texture_width = 128, .texture_height = 128, .font_size = 10, .line_height = 12};
    r->fonts[UI_FONT_ZH] = (ui_font){.glyphs = chinese, .glyph_count = 1,
        .texture_width = 128, .texture_height = 128, .font_size = 10, .line_height = 12};
    i32 a = ui_rect_batch_create(r), b = ui_rect_batch_create(r);
    ui_rect_batch_begin(r, a);
    ui_rect_batch_begin(r, b);
    ui_rect_batch_add_text(r, a, 3, 4, expected, 12, 0x12345678u, UI_FONT_MONO);
    ui_rect_batch_add_text_range(r, b, 3, 4, source, offset, length, 12, 0x12345678u, UI_FONT_MONO);
    ui_rect_batch *original = ui_rect_batch_get(r, a), *range = ui_rect_batch_get(r, b);
    assert(original->vertex_count == range->vertex_count);
    assert(original->kind == range->kind && original->texture_id == range->texture_id);
    if (original->vertex_count) assert(memcmp(original->vertices, range->vertices,
        (size_t)original->vertex_count * sizeof(ui_vertex)) == 0);
    ui_rect_batch_destroy(r, a);
    ui_rect_batch_destroy(r, b);
    free(r);
}

static void test_batch_append(void) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    i32 a = ui_rect_batch_create(r), b = ui_rect_batch_create(r);
    ui_rect_batch_begin(r, a);
    ui_rect_batch_begin(r, b);
    ui_rect_batch_add(r, a, 1, 2, 3, 4, 0xffffffffu);
    ui_rect_batch_add(r, b, 5, 6, 7, 8, 0x12345678u);
    ui_rect_batch *dst = ui_rect_batch_get(r, a), *src = ui_rect_batch_get(r, b);
    assert(ui_rect_batch_append(r, a, b));
    assert(dst->vertex_count == 12 && src->vertex_count == 6);
    assert(memcmp(dst->vertices + 6, src->vertices, 6 * sizeof(ui_vertex)) == 0);
    assert(!ui_rect_batch_append(r, a, a));
    src->texture_id++;
    assert(!ui_rect_batch_append(r, a, b) && dst->vertex_count == 12);
    src->texture_id--;
    src->kind = UI_KIND_MSDF;
    assert(!ui_rect_batch_append(r, a, b) && dst->vertex_count == 12);
    ui_rect_batch_begin(r, a);
    assert(ui_rect_batch_append(r, a, b));
    assert(dst->kind == UI_KIND_MSDF && dst->vertex_count == 6);
    ui_rect_batch_destroy(r, a);
    ui_rect_batch_destroy(r, b);
    free(r);
}

static u32 meta_of(ui_renderer *r, i32 command) {
    return r->vertices[r->commands[command].vertex_offset].meta;
}

static u32 meta(i32 kind, u32 face, u32 projection) {
    return (u32)kind | (face << UI_META_FACE_SHIFT) | (projection << UI_META_PROJECTION_SHIFT);
}

// Rectangles, arcs, an image under a clip, a tilted plane and a screen
// overlay recorded in one frame are one draw, and every vertex carries the
// kind, MSDF face and projection the one shader needs; solid shapes sample
// nothing. A panel puts everything but the overlay on it.
static void test_one_draw(void) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    r->rect = r->safe_rect = (ui_rect){0, 0, 200, 100};
    for (i32 i = 0; i < UI_MAX_TEXTURES; i++) r->textures[i] = 100 + (u32)i;
    ui_begin_frame(r);
    ui_fill_rect(r, 0, 0, 10, 10, 0xffffffffu, 0.0);
    ui_fill_arc(r, 50, 50, 12, 2, 0, M_PI / 3, 0xffffffffu, 0.0);
    ui_push_clip(r, 0, 0, 50, 50);
    r->current_texture_id = 3;
    ui_push_quad_ex(r, 0, 0, 10, 10, 0, 0, 1, 1, 0xffffffffu, UI_KIND_IMAGE, 0, 0, 0);
    ui_pop_clip(r);
    ui_begin_plane(r, 100, 50, 0.3, 400, 0, 0, 200, 100);
    ui_fill_rect(r, 20, 20, 10, 10, 0xffffffffu, 0.0);
    ui_end_plane(r);
    ui_begin_overlay(r);
    ui_fill_rect(r, 30, 30, 10, 10, 0xffffffffu, 0.0);
    ui_end_overlay(r);
    assert(r->command_count == 5);

    assert(ui_plan_draws(r, false, false));
    assert(r->draw_count == 1);
    assert(r->draws[0].command == -1 && r->draws[0].vertex_offset == 0);
    assert(r->draws[0].vertex_count == r->vertex_count);
    assert(r->draws[0].rgba == 100 && !r->draws[0].msdf && !r->plan_msdf);
    assert(meta_of(r, 0) == meta(UI_KIND_SOLID, 0, 0));
    assert(meta_of(r, 1) == meta(UI_KIND_ARC_SDF, 0, 0));
    assert(meta_of(r, 2) == meta(UI_KIND_IMAGE, 0, 0));
    assert(meta_of(r, 3) == meta(UI_KIND_SOLID, 0, 1));
    assert(meta_of(r, 4) == meta(UI_KIND_SOLID, 0, 0));
    // The clipped quad keeps its clip per vertex rather than as a scissor.
    assert(r->vertices[r->commands[2].vertex_offset].clip > 0.0f);

    assert(ui_plan_draws(r, true, false));
    assert(r->draw_count == 1);
    assert(meta_of(r, 0) == meta(UI_KIND_SOLID, 0, UI_PROJECTION_PANEL));
    assert(meta_of(r, 3) == meta(UI_KIND_SOLID, 0, UI_PROJECTION_PANEL));
    assert(meta_of(r, 4) == meta(UI_KIND_SOLID, 0, 0));
    // The immersive HUD projects everything itself.
    assert(ui_plan_draws(r, false, true));
    assert(meta_of(r, 3) == meta(UI_KIND_SOLID, 0, 0));

    // A second RGBA texture starts a second draw, as does a retained batch;
    // solid shapes and MSDF text join any draw.
    ui_begin_frame(r);
    r->current_texture_id = 3;
    ui_push_quad_ex(r, 0, 0, 10, 10, 0, 0, 1, 1, 0xffffffffu, UI_KIND_IMAGE, 0, 0, 0);
    ui_push_quad_ex(r, 10, 0, 20, 10, 0, 0, 1, 1, 0xffffffffu, UI_KIND_IMAGE, 0, 0, 0);
    ui_fill_rect(r, 0, 0, 10, 10, 0xffffffffu, 0.0);
    r->current_texture_id = 4;
    ui_push_quad_ex(r, 0, 0, 10, 10, 0, 0, 1, 1, 0xffffffffu, UI_KIND_IMAGE, 0, 0, 0);
    const i32 batch = ui_rect_batch_create(r);
    ui_rect_batch_begin(r, batch);
    ui_rect_batch_add(r, batch, 1, 2, 3, 4, 0xffffffffu);
    ui_rect_batch_draw_range(r, batch, 0, 6, 0, 0, 1, 1);
    ui_fill_rect(r, 0, 0, 10, 10, 0xffffffffu, 0.0);
    r->current_texture_id = UI_FONT_ZH_TEXTURE;
    ui_push_quad_ex(r, 0, 0, 10, 10, 0, 0, 1, 1, 0xffffffffu, UI_KIND_MSDF, 5, 0, 1);
    assert(r->command_count == 6);
    assert(ui_plan_draws(r, false, false));
    assert(r->draw_count == 4);
    assert(r->draws[0].rgba == 100);
    assert(r->draws[0].vertex_count == r->commands[0].vertex_count + r->commands[1].vertex_count);
    assert(r->draws[1].rgba == 101 && r->draws[1].vertex_offset == r->commands[2].vertex_offset);
    assert(r->draws[2].command == 3 && r->draws[2].rgba == 0);
    assert(r->draws[2].batch_meta == meta(UI_KIND_SOLID, 0, 0));
    assert(r->draws[3].command == -1 && r->draws[3].rgba == 0 && r->draws[3].msdf);
    assert(r->plan_msdf && r->plan_zh);
    assert(meta_of(r, 5) == meta(UI_KIND_MSDF, UI_FACE_ZH, 0));

    // Vertices no command can take are taken back, so a draw never covers them.
    const i32 vertices = r->vertex_count;
    r->command_count = UI_MAX_COMMANDS;
    r->current_texture_id = 4;
    ui_push_quad_ex(r, 0, 0, 10, 10, 0, 0, 1, 1, 0xffffffffu, UI_KIND_IMAGE, 0, 0, 0);
    assert(r->vertex_count == vertices);
    ui_renderer_destroy(r);
    gpu_commit();
}

static void test_batch_gpu_pressure(void) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    r->rect = r->safe_rect = (ui_rect){0, 0, 640, 480};
    ui_begin_frame(r);
    enum { batches = 300 };
    for (i32 i = 0; i < batches; i++) {
        i32 id = ui_rect_batch_create(r);
        assert(id > 0);
        ui_rect_batch_begin(r, id);
        ui_rect_batch_add(r, id, 1, 2, 3, 4, 0xffffffffu);
        ui_rect_batch_draw_range(r, id, 0, 6, 10, 20, 2, 3);
    }
    u32 clips, roots;
    assert(ui_upload_storage(r, &clips, &roots));
    i32 cached = 0, streamed = 0;
    for (i32 i = 0; i < batches; i++) {
        ui_rect_batch *batch = &r->rect_batches[i];
        ui_command *cmd = &r->commands[i];
        if (batch->buffer) cached++;
        if (!cmd->rect_batch_id) {
            streamed++;
            assert(cmd->retained);
            assert(cmd->offset_x == 10 && cmd->offset_y == 20);
            assert(cmd->scale_x == 2 && cmd->scale_y == 3);
            assert(cmd->batch_clip.w == 640 && cmd->batch_clip.h == 480);
            assert(memcmp(r->vertices + cmd->vertex_offset, batch->vertices,
                          6 * sizeof(ui_vertex)) == 0);
        }
    }
    assert(cached == UI_BATCH_GPU_CACHE && streamed == batches - cached);
    assert(r->vertex_count == streamed * 6);

    // A full command list reserves all root parameters in framed storage.
    // Uploading them must not allocate another byte of the transient ring.
    ui_begin_frame(r);
    for (i32 i = 0; i < UI_MAX_COMMANDS; i++) {
        ui_rect_batch_draw_range(r, 1, 0, 6, 0, 0, 1, 1);
    }
    assert(ui_upload_storage(r, &clips, &roots));
    assert(roots % GPU_V2_ALLOC_ALIGN == 0);
    assert((u64)roots + UI_MAX_COMMANDS * GPU_V2_ALLOC_ALIGN <= r->storage_capacity);
    gpu_addr before = gpu_frame_alloc(1, GPU_V2_ALLOC_ALIGN);
    ui_gpu_root root = {.screen_width = 640, .screen_height = 480};
    for (i32 i = 0; i < UI_MAX_COMMANDS; i++) {
        gpu_addr addr = r->storage + roots + (u64)i * GPU_V2_ALLOC_ALIGN;
        gpu_write(addr, &root, sizeof(root));
    }
    gpu_addr after = gpu_frame_alloc(1, GPU_V2_ALLOC_ALIGN);
    assert(after - before == GPU_V2_ALLOC_ALIGN);
    ui_gpu_root read = {0};
    assert(gpu_read(r->storage + roots + (UI_MAX_COMMANDS - 1) * GPU_V2_ALLOC_ALIGN,
                    &read, sizeof(read)));
    assert(read.screen_width == 640 && read.screen_height == 480);

    // Reclaim GPU copies after their in-flight reads finish, keeping CPU data.
    for (i32 i = 0; i <= GPU_SWAP_BUFFER_COUNT; i++) {
        gpu_commit();
        ui_begin_frame(r);
    }
    ui_rect_batch_draw_range(r, 1, 0, 6, 0, 0, 1, 1);
    assert(ui_upload_storage(r, &clips, &roots));
    assert(r->rect_batches[0].buffer);
    assert(!r->rect_batches[1].buffer && r->rect_batches[1].dirty);
    assert(r->rect_batches[1].vertex_count == 6);
    ui_renderer_destroy(r);
    gpu_commit();
}

// A Chinese glyph drawn in Latin text from the Chinese face is moved so its
// middle meets the middle of the Latin capitals beside it, whatever line
// metrics each face brings. The numbers are baloo2's H and gensen's 中.
static void test_mixed_faces_share_a_middle(void) {
    ui_renderer *r = calloc(1, sizeof(*r));
    assert(r);
    ui_vertex vertices[64];
    r->rect = r->safe_rect = (ui_rect){0, 0, 200, 100};
    r->vertices = vertices;
    r->vertex_capacity = 64;
    ui_glyph latin[] = {{.code = 'H', .width = 38, .height = 44, .y_offset = 32, .x_advance = 43}};
    ui_glyph chinese[] = {{.code = 0x4E2D, .width = 38, .height = 43, .y_offset = 4, .x_advance = 42}};
    r->fonts[UI_FONT_MAIN] = (ui_font){.glyphs = latin, .glyph_count = 1, .texture_width = 512, .texture_height = 416, .font_size = 64};
    r->fonts[UI_FONT_ZH] = (ui_font){.glyphs = chinese, .glyph_count = 1, .texture_width = 2541, .texture_height = 2620, .font_size = 42};
    ui_detect_cap_metrics(&r->fonts[UI_FONT_MAIN]);
    ui_detect_cap_metrics(&r->fonts[UI_FONT_ZH]);
    assert(r->fonts[UI_FONT_MAIN].middle == 54.0 && r->fonts[UI_FONT_ZH].middle == 25.5);
    ui_draw_text(r, 0, 10, "H\xe4\xb8\xad", 20, 0xffffffffu, UI_FONT_MAIN);
    assert(r->vertex_count == 12);
    // Each quad's top and bottom: its first vertex and its third.
    const f64 latin_middle = (vertices[0].y + vertices[2].y) * 0.5;
    const f64 chinese_middle = (vertices[6].y + vertices[8].y) * 0.5;
    assert(fabs(latin_middle - (10 + 54.0 * 20 / 64)) < 1e-4);
    assert(fabs(chinese_middle - latin_middle) < 1e-4);
    free(r);
}

int main(void) {
    test_mixed_faces_share_a_middle();
    test_batch_gpu_pressure();
    test_one_draw();
    test_batch_text_range("@@AB\nA!!", 2, 4, "AB\nA");
    test_batch_text_range("@@A一B!!", 2, 5, "A一B");
    test_batch_text_range("@@一A!!", 2, 4, "一A");
    test_batch_text_range("@@A\xe4\xb8!!", 2, 3, "A\xe4\xb8");
    test_batch_text_range("@@A", 2, 99, "A");
    test_batch_text_range("@@A", 99, 3, "");
    test_batch_append();
    test_panel();
    test_joined_triangles(0.0);
    test_joined_triangles(-1.0);
    test_standalone_feather();
    test_line_feather(10, 20, 90, 20, 2, 1.5);
    test_line_feather(20, 90, 20, 10, 2, 1.5);
    test_line_feather(10, 10, 70, 90, 2, 1.5);
    test_line_feather(10, 20, 90, 20, 2, 0);
    test_line_feather(10, 10, 70, 90, 2, 0);
    test_line_feather(20, 90, 20, 10, 2, -1);
    test_line_feather(10, 20, 90, 20, 1, 3);
    test_arc_coverage_mode(0);
    test_arc_coverage_mode(-1);
    test_arc_coverage_mode(1.5);
    puts("ui triangle, line and arc coverage tests passed");
    return 0;
}
