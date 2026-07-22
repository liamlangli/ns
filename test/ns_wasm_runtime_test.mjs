import assert from 'node:assert/strict';
import fs from 'node:fs';

const source = fs.readFileSync(new URL('../lib/ns-wasm.js', import.meta.url), 'utf8');
const { NSBrowserRuntime } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);

globalThis.GPUBufferUsage = { COPY_SRC: 1, COPY_DST: 2, STORAGE: 4, VERTEX: 8, INDEX: 16, INDIRECT: 32, UNIFORM: 64 };
globalThis.GPUTextureUsage = { COPY_DST: 1, COPY_SRC: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 };

let configured = null;
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
    return { configure(value) { configured = value; }, getCurrentTexture() { return { createView() { return {}; } }; } };
  },
};

const writes = [];
let computeDispatch = null;
const device = {
  lost: new Promise(() => {}),
  queue: { writeBuffer(...args) { writes.push(args); }, submit() {} },
  createBuffer(desc) { return { desc, destroy() {} }; },
  createTexture(desc) { return { desc, createView() { return {}; }, destroy() {} }; },
  createSampler(desc) { return { desc }; },
  createShaderModule(desc) { return { desc }; },
  createRenderPipeline(desc) { return { desc }; },
  createComputePipeline(desc) { return { desc, getBindGroupLayout() { return {}; } }; },
  createCommandEncoder() { return {
    beginRenderPass() { return { end() {}, draw() {}, setPipeline() {}, setVertexBuffer() {}, setIndexBuffer() {} }; },
    beginComputePass() { return { end() {}, setPipeline() {}, setBindGroup() {}, dispatchWorkgroups(...args) { computeDispatch = args; } }; },
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
Object.defineProperty(globalThis, 'document', { configurable: true, value: { title: 'Manifest project' } });

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
assert.equal(runtime.view().getUint32(canvasView + 108, true), 1);
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
const buffer = runtime.gpu('gpu_create_buffer', [32, 0]);
assert(buffer > 0);
const texture = runtime.gpu('gpu_texture_new_2d', [8, 4, 23, 0]);
assert.equal(runtime.gpu('gpu_texture_valid', [texture]), 1);
assert.equal(runtime.gpu('gpu_texture_bytes', [texture]), 128n);
const allocation = runtime.gpu('gpu_memory_alloc', [64n, 0]);
assert.equal(runtime.gpu('gpu_memory_valid', [allocation]), 1);
const allocationAddress = runtime.gpu('gpu_memory_at', [allocation, 16n]);
assert.equal(allocationAddress & 0xffffffffn, 16n);
const frameAddress = runtime.gpu('gpu_frame_alloc', [32n, 256]);
assert.equal(frameAddress & 255n, 0n);

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
assert(runtime.gpu('gpu_create_pipeline', [7, 7, 28, 4]) > 0);
const computeSource = runtime.writeString('@compute @workgroup_size(1) fn cs() {}');
const computeEntry = runtime.writeString('cs');
assert.equal(runtime.gpu('gpu_dispatch_compute_source', [computeSource, computeEntry, 2, 3, 4]), 1);
assert.deepEqual(computeDispatch, [2, 3, 4]);

Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { gpu: { async requestAdapter() { return null; } } } });
const fallback = new NSBrowserRuntime(canvas);
assert.equal(await fallback.initializeGPU(), false);
assert.equal(fallback.gpu('gpu_request_device', [0]), 0);

console.log('PASS: mocked canvas view/WebGPU middleware initializes, tracks input and metrics, loads shader metadata, dispatches imports, and falls back without an adapter.');
