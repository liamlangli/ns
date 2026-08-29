import assert from 'node:assert/strict';
import fs from 'node:fs';

const source = fs.readFileSync(new URL('../lib/ns-wasm.js', import.meta.url), 'utf8');
const { NSBrowserRuntime } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);

globalThis.GPUBufferUsage = { COPY_SRC: 1, COPY_DST: 2, STORAGE: 4, VERTEX: 8, INDEX: 16, INDIRECT: 32, UNIFORM: 64 };
globalThis.GPUTextureUsage = { COPY_DST: 1, COPY_SRC: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 };

let configured = null;
let configureCount = 0;
const canvasEvents = new Map();
const windowEvents = new Map();
let capturedPointer = 0;
const canvas = {
  clientWidth: 320,
  clientHeight: 180,
  width: 0,
  height: 0,
  style: {},
  setAttribute() {},
  addEventListener(name, handler) { canvasEvents.set(name, handler); },
  setPointerCapture(pointer) { capturedPointer = pointer; },
  hasPointerCapture(pointer) { return capturedPointer === pointer; },
  releasePointerCapture(pointer) { if (capturedPointer === pointer) capturedPointer = 0; },
  getBoundingClientRect() { return { left: 0, top: 0 }; },
  focus() {},
  getContext(kind) {
    assert.equal(kind, 'webgpu');
    return { configure(value) { configured = value; configureCount += 1; }, getCurrentTexture() { return { createView() { return {}; } }; } };
  },
};

const writes = [];
let computeDispatch = null;
let computePassLabel = null;
const device = {
  lost: new Promise(() => {}),
  limits: { maxStorageBuffersPerShaderStage: 8, maxBindingsPerBindGroup: 1000 },
  queue: { writeBuffer(...args) { writes.push(args); }, submit() {} },
  createBuffer(desc) { return { desc, destroy() {} }; },
  createTexture(desc) { return { desc, createView() { return {}; }, destroy() {} }; },
  createSampler(desc) { return { desc }; },
  createShaderModule(desc) { return { desc }; },
  createRenderPipeline(desc) { return { desc }; },
  createComputePipeline(desc) { return { desc, getBindGroupLayout() { return {}; } }; },
  createCommandEncoder() { return {
    beginRenderPass() { return { end() {}, draw() {}, setPipeline() {}, setVertexBuffer() {}, setIndexBuffer() {} }; },
    beginComputePass(desc) { computePassLabel = desc?.label ?? null; return { end() {}, setPipeline() {}, setBindGroup() {}, dispatchWorkgroups(...args) { computeDispatch = args; } }; },
    finish() { return {}; },
  }; },
};
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {
  gpu: { async requestAdapter() { return { async requestDevice() { return device; } }; }, getPreferredCanvasFormat() { return 'bgra8unorm'; } },
} });
Object.defineProperty(globalThis, 'window', { configurable: true, value: {
  devicePixelRatio: 2,
  addEventListener(name, handler) { windowEvents.set(name, handler); },
} });
const safeAreaPadding = { paddingTop: '0px', paddingRight: '0px', paddingBottom: '0px', paddingLeft: '0px' };
Object.defineProperty(globalThis, 'document', { configurable: true, value: {
  title: 'Manifest project',
  body: { appendChild() {} },
  createElement() { return { style: { cssText: '' } }; },
} });
Object.defineProperty(globalThis, 'getComputedStyle', { configurable: true, value: () => safeAreaPadding });

const runtime = new NSBrowserRuntime(canvas);
let contextMenuPrevented = false;
canvasEvents.get('contextmenu')({ preventDefault() { contextMenuPrevented = true; } });
assert.equal(contextMenuPrevented, true);
runtime.memory = new WebAssembly.Memory({ initial: 1 });
let heap = 1024;
runtime.instance = { exports: { __ns_alloc(size) { const p = heap; heap += Number(size); return p; } } };
assert.equal(await runtime.initializeGPU(), true);
assert.deepEqual([canvas.width, canvas.height], [640, 360]);
assert.equal(configured.format, 'bgra8unorm');
assert.equal(configureCount, 1);
runtime.resizeCanvas();
assert.equal(configureCount, 1);
canvas.clientWidth = 321;
runtime.resizeCanvas();
assert.equal(configureCount, 2);
canvas.clientWidth = 320;
runtime.resizeCanvas();
assert.equal(configureCount, 3);

const stringPointer = runtime.writeString('hello wasm');
assert.equal(runtime.readString(stringPointer), 'hello wasm');
assert.equal(runtime.readString(runtime.std('substr', [stringPointer, 6, 4])), 'wasm');
const titlePointer = runtime.writeString('Canvas view');
const canvasView = runtime.invoke('view', 'view_create', [titlePointer, 960, 540]);
assert.equal(document.title, 'Manifest project');
assert.equal(runtime.view().getInt32(canvasView + 4, true), 320);
assert.equal(runtime.view().getInt32(canvasView + 12, true), 640);
assert.equal(runtime.view().getFloat64(canvasView + 88, true), 2);
assert.equal(runtime.gpu('gpu_request_device', [canvasView]), 1);
assert.equal(runtime.gpu('gpu_request_device', [canvasView + 4]), 0);
assert.equal(runtime.view().getUint32(canvasView + 144, true), 1);
canvasEvents.get('pointerdown')({ clientX: 10, clientY: 16, pointerType: 'mouse', pointerId: 7, button: 0 });
assert.equal(capturedPointer, 7);
assert.equal(runtime.view().getInt32(canvasView + 52, true), 1);
canvasEvents.get('pointermove')({ clientX: 12, clientY: 18, pointerType: 'mouse', pointerId: 1, timeStamp: 10 });
assert.equal(runtime.view().getFloat64(canvasView + 20, true), 12);
assert.equal(runtime.viewImport('view_input_count', [canvasView]), 3);
canvasEvents.get('pointerup')({ clientX: 12, clientY: 18, pointerType: 'mouse', pointerId: 7, button: 0 });
assert.equal(capturedPointer, 0);
assert.equal(runtime.view().getInt32(canvasView + 52, true), 0);
windowEvents.get('keydown')({ key: 'A' });
assert.equal(runtime.viewImport('view_is_key_pressed', [canvasView, 65]), 1);
assert.equal(runtime.viewImport('view_take_key_press', [canvasView, 65]), 0);
runtime.viewImport('view_input_reset', [canvasView]);
assert.equal(runtime.viewImport('view_input_count', [canvasView]), 0);
assert.equal(runtime.gpu('gpu_caps', []), 6);
assert.equal(runtime.gpu('gpu_storage_slot_count', []), 8);
assert.equal(runtime.gpu('gpu_malloc', [32n, 0, runtime.writeString('')]), 0n);
const buffer = runtime.gpu('gpu_malloc', [32n, 0, runtime.writeString('wasm test storage')]);
assert(buffer > 0n);
assert.equal(runtime.get(runtime.addressParts(buffer).id, 'buffer').desc.label, 'wasm test storage');
const texture = runtime.gpu('gpu_texture_new_2d', [8, 4, 23, 0]);
assert.equal(runtime.gpu('gpu_texture_valid', [texture]), 1);
assert.equal(runtime.gpu('gpu_texture_bytes', [texture]), 128n);
const allocation = runtime.gpu('gpu_memory_alloc', [64n, 0, runtime.writeString('wasm test allocation')]);
assert.equal(runtime.gpu('gpu_memory_valid', [allocation]), 1);
const allocationBase = runtime.view().getBigUint64(allocation, true);
assert.equal(runtime.get(runtime.addressParts(allocationBase).id, 'buffer').desc.label, 'wasm test allocation');
const allocationAddress = runtime.gpu('gpu_memory_at', [allocation, 16n]);
assert.equal(allocationAddress & 0xffffffffn, 16n);
const frameAddress = runtime.gpu('gpu_frame_alloc', [32n, 256]);
assert.equal(frameAddress & 255n, 0n);
assert.equal(runtime.get(runtime.addressParts(frameAddress).id, 'buffer').desc.label, 'ns frame ring 0');
runtime.gpu('gpu_commit', []);
const secondFrameAddress = runtime.gpu('gpu_frame_alloc', [32n, 256]);
assert.equal(runtime.get(runtime.addressParts(secondFrameAddress).id, 'buffer').desc.label, 'ns frame ring 1');
runtime.gpu('gpu_commit', []);
const thirdFrameAddress = runtime.gpu('gpu_frame_alloc', [32n, 256]);
assert.equal(runtime.get(runtime.addressParts(thirdFrameAddress).id, 'buffer').desc.label, 'ns frame ring 2');

const leb = value => {
  const result = [];
  do { let byte = value & 0x7f; value >>>= 7; if (value) byte |= 0x80; result.push(byte); } while (value);
  return result;
};
const bytes = value => [...leb(value.length), ...Buffer.from(value)];
const shaderPayload = [1, 1, ...leb(7), 1, ...bytes('vs'), ...bytes('@vertex fn vs() {}'), ...leb(16), 1, ...leb(0), ...leb(12)];
const sectionName = bytes('ns.shaders');
const customContent = [...sectionName, ...shaderPayload];
const shaderModule = new WebAssembly.Module(Uint8Array.from([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00, ...leb(customContent.length), ...customContent,
]));
runtime.loadShaders(shaderModule);
assert.deepEqual(runtime.shaders.get(7), {
  id: 7, stage: 1, name: 'vs', wgsl: '@vertex fn vs() {}', stride: 16,
  attributes: [{ offset: 0, size: 12 }],
});
assert.equal(runtime.readString(runtime.shader('shader_transpile_stage', [7, 0, 0])), '@vertex fn vs() {}');
assert.equal(runtime.readString(runtime.shader('shader_entry', [7, 0])), 'vs');
assert.equal(runtime.shader('shader_vertex_stride', [7]), 16);
assert.equal(runtime.shader('shader_vertex_attr_count', [7]), 1);
assert.equal(runtime.shader('shader_vertex_attr_size', [7, 0]), 12);
const vertexSource = runtime.writeString('@vertex fn vs() -> @builtin(position) vec4<f32> { return vec4<f32>(0.0); }');
const fragmentSource = runtime.writeString('@fragment fn fs() -> @location(0) vec4<f32> { return vec4<f32>(1.0); }');
assert(runtime.gpu('gpu_shader_graphics_create', [vertexSource, fragmentSource, runtime.writeString('vs'), runtime.writeString('fs')]) > 0);
const computeSource = runtime.writeString('@compute @workgroup_size(1) fn cs() {}');
const computeEntry = runtime.writeString('cs');
const computeShader = runtime.gpu('gpu_shader_compute_create', [computeSource, computeEntry]);
runtime.gpu('gpu_set_shader', [computeShader]);
runtime.gpu('gpu_dispatch', [runtime.writeString('unit test dispatch'), 2, 3, 4]);
assert.deepEqual(computeDispatch, [2, 3, 4]);
// The pass label names the compute pass for frame capture tools.
assert.equal(computePassLabel, 'unit test dispatch');
const oversizedStorageSource = runtime.writeString('@group(0) @binding(15) var<storage, read_write> ns_storage_buffer_8: array<i32>;');
const oldConsoleError = console.error;
let storageLimitError = '';
console.error = message => { storageLimitError = String(message); };
assert.equal(runtime.gpu('gpu_shader_compute_create', [oversizedStorageSource, computeEntry]), 0);
console.error = oldConsoleError;
assert.match(storageLimitError, /requires 9 storage slots.*supports 8/);

const uiCalls = [];
const uiContext = {
  save() { uiCalls.push('save'); },
  restore() { uiCalls.push('restore'); },
  setTransform() {},
  translate(x, y) { uiCalls.push(`translate:${x},${y}`); },
  fillRect() { uiCalls.push('fillRect'); },
  clearRect() { uiCalls.push('clearRect'); },
  beginPath() {},
  rect() {},
  roundRect() {},
  clip() {},
  fill() { uiCalls.push('fill'); },
  stroke() {},
  strokeRect() {},
  arc() {},
  moveTo() {},
  lineTo() {},
  closePath() {},
  fillText(text) { uiCalls.push(`text:${text}`); },
  drawImage() {},
  measureText(text) { return { width: text.length * 8 }; },
};
const uiCanvas = {
  clientWidth: 480,
  clientHeight: 270,
  width: 0,
  height: 0,
  style: {},
  setAttribute() {},
  addEventListener() {},
  focus() {},
  getBoundingClientRect() { return { left: 0, top: 0 }; },
  getContext(kind) { assert.equal(kind, '2d'); return uiContext; },
};
const uiRuntime = new NSBrowserRuntime(uiCanvas);
uiRuntime.memory = new WebAssembly.Memory({ initial: 1 });
let uiHeap = 2048;
uiRuntime.instance = { exports: { __ns_alloc(size) { const p = uiHeap; uiHeap += Number(size); return p; } } };
assert.equal(uiRuntime.initializeCanvasUI(), true);
const uiTitle = uiRuntime.writeString('NSCode');
const uiView = uiRuntime.viewImport('view_create', [uiTitle, 480, 270]);
assert.equal(uiRuntime.gpu('gpu_request_device', [uiView]), 1);
const renderer = uiRuntime.ui('ui_renderer_create', [uiView]);
uiRuntime.ui('ui_begin_frame', [renderer]);
uiRuntime.ui('ui_fill_rect', [renderer, 4, 5, 30, 20, uiRuntime.ui('ui_pack_color', [uiRuntime.writeString('#112233')]), 0]);
uiRuntime.ui('ui_fill_gradient_rect', [renderer, 40, 5, 30, 20, 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff]);
assert.deepEqual(uiRuntime.uiRenderer(renderer).commands.slice(-2), [
  { kind: 'triangle', clip: { x: 0, y: 0, w: 480, h: 270 }, x0: 40, y0: 5, x1: 70, y1: 5,
    x2: 70, y2: 25, colors: [0xff0000ff, 0xff00ff00, 0xffff0000] },
  { kind: 'triangle', clip: { x: 0, y: 0, w: 480, h: 270 }, x0: 40, y0: 5, x1: 70, y1: 25,
    x2: 40, y2: 25, colors: [0xff0000ff, 0xffff0000, 0xffffffff] },
]);
uiRuntime.ui('ui_draw_text', [renderer, 8, 9, uiRuntime.writeString('native UI'), 14, 0xffffffff, 1]);
const clear = uiRuntime.allocStruct(32);
for (const [offset, value] of [[0, 0.1], [8, 0.2], [16, 0.3], [24, 1]]) uiRuntime.view().setFloat64(clear + offset, value, true);
uiRuntime.ui('ui_flush', [renderer, clear]);
assert(uiCalls.includes('fillRect'));
assert(uiCalls.includes('text:native UI'));

// Device safe areas: CSS env() insets shrink the canvas the application draws
// into, and the view publishes them for code that reads the metrics directly.
safeAreaPadding.paddingTop = '47px';
safeAreaPadding.paddingBottom = '34px';
uiRuntime.syncView(uiView);
assert.equal(uiRuntime.view().getFloat64(uiView + 104, true), 47);
assert.equal(uiRuntime.view().getFloat64(uiView + 120, true), 34);
assert.equal(uiRuntime.ui('ui_canvas_width', [renderer]), 480);
assert.equal(uiRuntime.ui('ui_canvas_height', [renderer]), 270 - 47 - 34);
assert.equal(uiRuntime.ui('ui_surface_height', [renderer]), 270);
assert.equal(uiRuntime.ui('ui_content_y', [renderer, 50]), 3);
assert.equal(uiRuntime.ui('ui_surface_y', [renderer, 0]), 47);
const insets = uiRuntime.ui('ui_safe_area', [renderer]);
assert.deepEqual([0, 8, 16, 24].map(offset => uiRuntime.view().getFloat64(insets + offset, true)), [47, 0, 34, 0]);
// An application override replaces the device values; opting out restores the
// full drawable.
uiRuntime.ui('ui_set_safe_area_insets', [renderer, 10, 0, 0, 20]);
assert.equal(uiRuntime.ui('ui_canvas_width', [renderer]), 460);
assert.equal(uiRuntime.ui('ui_canvas_height', [renderer]), 260);
uiRuntime.ui('ui_reset_safe_area_insets', [renderer]);
assert.equal(uiRuntime.ui('ui_canvas_height', [renderer]), 189);
assert.equal(uiRuntime.ui('ui_safe_area_enabled', [renderer]), 1);
uiRuntime.ui('ui_set_safe_area_enabled', [renderer, 0]);
assert.equal(uiRuntime.ui('ui_safe_area_enabled', [renderer]), 0);
assert.equal(uiRuntime.ui('ui_canvas_height', [renderer]), 270);
assert.equal(uiRuntime.ui('ui_content_y', [renderer, 50]), 50);
uiRuntime.ui('ui_set_safe_area_enabled', [renderer, 1]);
safeAreaPadding.paddingTop = '0px';
safeAreaPadding.paddingBottom = '0px';
const filePath = uiRuntime.writeString('/home/web/settings.db');
const writeMode = uiRuntime.writeString('wb');
const file = uiRuntime.std('open', [filePath, writeMode]);
uiRuntime.std('write', [file, uiRuntime.writeString('saved')]);
uiRuntime.std('close', [file]);
const readFile = uiRuntime.std('open', [filePath, uiRuntime.writeString('rb')]);
assert.equal(uiRuntime.readString(uiRuntime.std('read', [readFile])), 'saved');
uiRuntime.std('close', [readFile]);
assert.equal(uiRuntime.os('os_dir_scan', [uiRuntime.writeString('nscode/native')]), 4);

Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { gpu: { async requestAdapter() { return null; } } } });
const fallback = new NSBrowserRuntime(canvas);
assert.equal(await fallback.initializeGPU(), false);
assert.equal(fallback.gpu('gpu_request_device', [0]), 0);

console.log('PASS: mocked browser middleware covers view/WebGPU, Canvas UI, browser files, shader metadata, and adapter fallback.');
