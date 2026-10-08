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

int main(void) {
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
