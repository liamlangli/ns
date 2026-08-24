// End-to-end cover of the ui module on the wasm target: compile a project that
// drives the renderer, the widget layer, text metrics and the selectable-label
// helpers, then run the emitted module against lib/ns-wasm.js on a mocked
// canvas. Compiling proves the imports are accepted by the wasm validator;
// running proves the browser middleware implements them and reads every lib
// struct at the offsets the compiler emitted.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const ns = process.argv[2];
assert(ns, 'usage: node test/ns_wasm_ui_test.mjs /absolute/path/to/ns');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ns-wasm-ui-'));
fs.writeFileSync(path.join(root, 'ns.mod'), `schema = "ns.mod/v1"
name = "wasm-ui"
version = "0.1.0"
type = "app"
target = "wasm"
source = "."
entry = "main.ns"
`);
fs.writeFileSync(path.join(root, 'main.ns'), `use view
use ui

let v = view_create("Wasm ui", 480, 270)
let r = ui_renderer_create(v)
let w = ui_widgets_create(r)
let selection = ui_text_sel_create()
let slider_value: f64 = 0.25
let clicks = 0

fn canvas_width() i32 { return ui_canvas_width(r) }
fn surface_height() i32 { return ui_surface_height(r) }

fn safe_left() f64 {
    let insets = ui_safe_area(r)
    return insets.left
}

fn layout_centered_x() f64 {
    let rect = ui_layout(0.0, 0.0, 100.0, 40.0, 20.0, 10.0, UI_ALIGN_CENTER_HORIZONTAL)
    return rect.x
}

fn surface_rect_y() f64 {
    let rect = ui_surface_rect(r)
    return rect.y
}

fn clipped_after_push() bool {
    ui_push_clip(r, 0.0, 0.0, 40.0, 40.0)
    let outside = ui_rect_clipped(r, 100.0, 100.0, 10.0, 10.0)
    ui_pop_clip(r)
    return outside
}

fn measure_width() f64 {
    let size = ui_measure_text(r, "widget", 14.0, UI_FONT_MONO)
    return size.w
}

fn caret_at(x: f64) i32 {
    return ui_text_index_at_x(r, "widget", 14.0, UI_FONT_MONO, x)
}

fn packed() u32 {
    return ui_pack_color("#204080")
}

fn packed_floats() u32 {
    return ui_pack_rgba_floats(1.0, 0.5, 0.25, 1.0)
}

// Ten mono cells wrapped into a five-cell column: two lines of line box.
fn wrapped_height() f64 {
    return ui_draw_text_wrapped(r, 0.0, 0.0, 40.0, "abcdefghij", 12.0, 0xffffffff, UI_FONT_MONO)
}

fn line_height() f64 {
    return ui_text_line_height(r, 12.0, UI_FONT_MONO)
}

fn draw_frame() {
    ui_begin_frame(r)
    ui_fill_surface(r, ui_pack_color("#101418"))
    ui_fill_rect(r, 4.0, 5.0, 30.0, 20.0, ui_pack_color("#112233"), 0.0)
    ui_fill_round_rect_per_corner(r, 8.0, 8.0, 40.0, 20.0, 4.0, 4.0, 0.0, 0.0, 0xff334455, 0.0)
    ui_fill_triangle(r, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 0xff00ff00, 0.0)
    ui_fill_arc(r, 40.0, 40.0, 12.0, 3.0, 0.0, 1.5, 0xffff0000, 0.0)
    ui_stroke_circle(r, 60.0, 60.0, 8.0, 2.0, 0xff0000ff, 0.0)
    ui_draw_text(r, 8.0, 9.0, "native UI", 14.0, 0xffffffff, UI_FONT_MONO)
    let used = ui_draw_text_wrapped(r, 8.0, 40.0, 60.0, "wrap this label across lines", 12.0, 0xffffffff, UI_FONT_MAIN)
    ui_draw_text_vertical(r, 200.0, 20.0, "天行健", 16.0, 0xffffffff, UI_FONT_MAIN)
    ui_draw_text_arc(r, 120.0, 120.0, 30.0, 0.0, "arc", 12.0, 0xffffffff, UI_FONT_MAIN)
    ui_draw_text_sel(r, 8.0, 90.0, 18.0, "selectable", 12.0, 0xffffffff, UI_FONT_MAIN, selection, 0, 0xff3355ff)
    if used > 0.0 {
        ui_stroke_line(r, 0.0, used, 10.0, used, 1.0, 0xffffffff, 0.0)
    }
    let batch = ui_rect_batch_create(r)
    ui_rect_batch_begin(r, batch)
    ui_rect_batch_add(r, batch, 0.0, 0.0, 4.0, 4.0, 0xffffffff)
    ui_rect_batch_end(r, batch)
    ui_rect_batch_draw_at(r, batch, 12.0, 12.0)
    ui_rect_batch_destroy(r, batch)
    ui_flush(r, ui_clear_color())
}

fn ui_clear_color() ui_color_rgba {
    return ui_color_rgba { r: 0.1, g: 0.2, b: 0.3, a: 1.0 }
}

fn widget_frame() {
    ui_widgets_begin_view(w, ui_theme_empty(), v, false)
    if ui_button(w, "ok", 10.0, 10.0, 80.0, 24.0, "OK", false) {
        clicks = clicks + 1
    }
    slider_value = ui_slider(w, "amount", 10.0, 60.0, 100.0, 20.0, slider_value, 0.0, 1.0, true)
    let hit = ui_hit_region(w, 0.0, 0.0, 200.0, 200.0)
    if hit.hovered {
        clicks = clicks + 0
    }
    ui_widgets_end_frame(w)
}

fn widget_mouse_x() f64 { return ui_widgets_mouse_x(w) }
fn click_count() i32 { return clicks }
fn slider_now() f64 { return slider_value }

fn select_range(begin_x: f64, end_x: f64) i32 {
    ui_text_sel_interact(r, selection, 0, "selectable", 0.0, 0.0, 200.0, 20.0, 12.0, UI_FONT_MAIN, begin_x, 10.0, true, true, false)
    ui_text_sel_interact(r, selection, 0, "selectable", 0.0, 0.0, 200.0, 20.0, 12.0, UI_FONT_MAIN, end_x, 10.0, false, true, true)
    return ui_text_sel_hi(selection) - ui_text_sel_lo(selection)
}

fn selected_text() str {
    return ui_text_sel_slice("selectable", selection)
}

fn main() {
    view_run(v)
}
`);

const build = spawnSync(ns, ['build', root], { encoding: 'utf8' });
assert.strictEqual(build.status, 0, `ui wasm build failed:\n${build.stdout}${build.stderr}`);

const wasm = fs.readFileSync(path.join(root, 'bin', 'wasm-ui.wasm'));
assert(WebAssembly.validate(wasm));

// Every ui import the program uses must be a real browser entry point, not a
// name the emitter accepted and the middleware throws on.
const module = new WebAssembly.Module(wasm);
const uiImports = WebAssembly.Module.imports(module).filter(item => item.module === 'ui').map(item => item.name);
assert(uiImports.length > 30, `expected the ui surface to be imported, got ${uiImports.length}`);

const source = fs.readFileSync(new URL('../lib/ns-wasm.js', import.meta.url), 'utf8');
const { NSBrowserRuntime } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);

const painted = [];
const context = {
  save() {}, restore() {}, setTransform() {}, translate() {}, beginPath() {}, clip() {},
  rect() {}, roundRect() {}, arc() {}, moveTo() {}, lineTo() {}, closePath() {},
  fill() { painted.push('fill'); }, stroke() { painted.push('stroke'); },
  fillRect() { painted.push('fillRect'); }, strokeRect() { painted.push('strokeRect'); },
  drawImage() {}, rotate() {},
  fillText(text) { painted.push(`text:${text}`); },
  // A fixed 8 px cell keeps the expected metrics arithmetic exact.
  measureText(text) { return { width: [...text].length * 8 }; },
};
const canvas = {
  clientWidth: 480, clientHeight: 270, width: 0, height: 0, style: {},
  setAttribute() {}, addEventListener() {}, focus() {},
  getBoundingClientRect() { return { left: 0, top: 0 }; },
  getContext(kind) { assert.equal(kind, '2d'); return context; },
};
Object.defineProperty(globalThis, 'window', { configurable: true, value: {
  devicePixelRatio: 1, addEventListener() {},
} });
Object.defineProperty(globalThis, 'document', { configurable: true, value: {
  title: 'ui', body: { appendChild() {} }, createElement() { return { style: { cssText: '' } }; },
} });
const padding = { paddingTop: '0px', paddingRight: '0px', paddingBottom: '0px', paddingLeft: '0px' };
Object.defineProperty(globalThis, 'getComputedStyle', { configurable: true, value: () => padding });

const runtime = new NSBrowserRuntime(canvas);
const instance = new WebAssembly.Instance(module, runtime.importsFor(module));
runtime.instance = instance;
runtime.memory = instance.exports.memory;
assert.equal(runtime.initializeCanvasUI(), true);
instance.exports.__ns_init();
instance.exports.main();

// Renderer metrics, read back through the same struct layout the module emits.
assert.equal(instance.exports.canvas_width(), 480);
assert.equal(instance.exports.surface_height(), 270);
assert.equal(instance.exports.safe_left(), 0);
assert.equal(instance.exports.layout_centered_x(), 40);
assert.equal(instance.exports.surface_rect_y(), 0);
assert.equal(instance.exports.clipped_after_push(), 1);
assert.equal(instance.exports.measure_width(), 48);
// The caret snaps to the nearer glyph edge, in UTF-8 byte offsets.
assert.equal(instance.exports.caret_at(0), 0);
assert.equal(instance.exports.caret_at(19), 2);
assert.equal(instance.exports.caret_at(20), 3);
assert.equal(instance.exports.packed() >>> 0, 0xff804020);
assert.equal(instance.exports.packed_floats() >>> 0, 0xff3f7fff);
assert.equal(instance.exports.wrapped_height(), instance.exports.line_height() * 2);

// A whole frame of draw calls reaches the canvas.
instance.exports.draw_frame();
assert(painted.includes('fillRect'), 'rectangles are painted');
assert(painted.includes('text:native UI'), 'text is painted');
assert(painted.filter(entry => entry.startsWith('text:天') || entry.startsWith('text:行')).length >= 2,
  'vertical text paints one glyph per cell');
assert(painted.includes('stroke'), 'strokes are painted');

// A device safe area moves the content origin and shrinks the canvas.
padding.paddingTop = '47px';
padding.paddingBottom = '34px';
runtime.syncView(runtime.activeView);
assert.equal(instance.exports.canvas_width(), 480);
assert.equal(instance.exports.surface_height(), 270);
assert.equal(instance.exports.safe_left(), 0);
assert.equal(instance.exports.surface_rect_y(), -47);
padding.paddingTop = '0px';
padding.paddingBottom = '0px';
runtime.syncView(runtime.activeView);

// Widgets read the pointer state out of the view record the middleware keeps.
const view = runtime.activeView;
runtime.viewImport('view_on_mouse_move', [view, 40, 20]);
instance.exports.widget_frame();
assert.equal(instance.exports.widget_mouse_x(), 40);
assert.equal(instance.exports.click_count(), 0);
runtime.viewImport('view_on_mouse_btn', [view, 0, 0]);
instance.exports.widget_frame();
runtime.viewImport('view_input_reset', [view]);
runtime.viewImport('view_on_mouse_move', [view, 40, 20]);
runtime.viewImport('view_on_mouse_btn', [view, 0, 1]);
instance.exports.widget_frame();
assert.equal(instance.exports.click_count(), 1, 'press then release inside the button clicks it');
runtime.viewImport('view_input_reset', [view]);

// Dragging the slider writes its value back through the same widget state.
runtime.viewImport('view_on_mouse_move', [view, 10, 70]);
runtime.viewImport('view_on_mouse_btn', [view, 0, 0]);
instance.exports.widget_frame();
runtime.viewImport('view_on_mouse_move', [view, 60, 70]);
instance.exports.widget_frame();
assert.equal(Math.round(instance.exports.slider_now() * 100), 50, 'the slider tracks the drag');

// A drag across a read-only label selects the bytes under it.
assert.equal(instance.exports.select_range(0, 24), 3);
assert.equal(runtime.readString(instance.exports.selected_text()), 'sel');

fs.rmSync(root, { recursive: true, force: true });
console.log('PASS: the ui module compiles to wasm and runs on the browser middleware (renderer, widgets, text metrics, selection).');
