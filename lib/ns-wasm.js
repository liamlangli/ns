// Nano Script browser middleware. This file is copied beside project Wasm
// artifacts by `ns build` and intentionally has no package/runtime dependency.

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

function align(value, n) { return Math.ceil(value / n) * n; }

// wasm32 layout of lib/view.ns::view. Pointers, strings, `any`, and function
// values are four-byte handles; f64 fields retain their eight-byte payload.
const VIEW = Object.freeze({
  size: 132, title: 0, width: 4, height: 8, framebufferWidth: 12,
  framebufferHeight: 16, mouseX: 20, mouseY: 28, scrollX: 36, scrollY: 44,
  mouseDown: 52, mousePressed: 56, mouseReleased: 60,
  rightDown: 64, rightPressed: 68, rightReleased: 72,
  middleDown: 76, middlePressed: 80, middleReleased: 84,
  displayRatio: 88, uiScale: 96, nativeWindow: 104, gpuDevice: 108,
  captureRequired: 112, captureStarted: 116,
});

const VIEW_INPUT_EVENT_SIZE = 84;
const VIEW_GESTURE_SIZE = 48;

class NSBrowserRuntime {
  constructor(canvas) {
    this.canvas = canvas;
    this.instance = null;
    this.memory = null;
    this.adapter = null;
    this.device = null;
    this.context = null;
    this.format = "bgra8unorm";
    this.resources = new Map();
    this.nextHandle = 1;
    this.commandEncoder = null;
    this.pass = null;
    this.currentShader = 0;
    this.currentState = 0;
    this.frameBuffers = [];
    this.frameIndex = 0;
    this.shaders = new Map();
    this.currentMesh = 0;
    this.views = new Map();
    this.activeView = 0;
    this.viewEventsInstalled = false;
    this.keysDown = new Set();
    this.keyPresses = new Map();
    this.clipboard = "";
    this.closed = false;
    this.canvas?.addEventListener?.("contextmenu", event => event.preventDefault?.());
  }

  view() { return new DataView(this.memory.buffer); }

  readString(pointer) {
    if (!pointer || !this.memory) return "";
    const view = this.view();
    const bytes = view.getUint32(pointer, true);
    const length = view.getUint32(pointer + 4, true);
    return textDecoder.decode(new Uint8Array(this.memory.buffer, bytes, length));
  }

  writeString(value) {
    const bytes = textEncoder.encode(String(value));
    const descriptor = this.instance.exports.__ns_alloc(bytes.length + 8);
    const data = descriptor + 8;
    new Uint8Array(this.memory.buffer, data, bytes.length).set(bytes);
    const view = this.view();
    view.setUint32(descriptor, data, true);
    view.setUint32(descriptor + 4, bytes.length, true);
    return descriptor;
  }

  allocStruct(size) {
    const pointer = this.instance.exports.__ns_alloc(size);
    new Uint8Array(this.memory.buffer, pointer, size).fill(0);
    return pointer;
  }

  readBytes(descriptor, requested = 0) {
    if (!descriptor || !this.memory) return new Uint8Array();
    const view = this.view();
    const data = view.getUint32(descriptor, true);
    const length = requested || view.getUint32(descriptor + 4, true);
    return new Uint8Array(this.memory.buffer, data, length);
  }

  readI32Array(descriptor, count) {
    if (!descriptor || !this.memory || count <= 0) return [];
    const data = this.view().getUint32(Number(descriptor), true);
    return Array.from(new Int32Array(this.memory.buffer, data, Number(count)));
  }

  addressParts(address) {
    const value = BigInt(address);
    return { id: Number(value >> 32n), offset: Number(value & 0xffffffffn) };
  }

  put(kind, value) {
    const id = this.nextHandle++;
    this.resources.set(id, { kind, value });
    return id;
  }

  get(id, kind) {
    const resource = this.resources.get(Number(id));
    return resource && (!kind || resource.kind === kind) ? resource.value : null;
  }

  drop(id) { this.resources.delete(Number(id)); }

  async initializeGPU() {
    if (!navigator.gpu) return false;
    try {
      this.adapter = await navigator.gpu.requestAdapter();
      if (!this.adapter) return false;
      this.device = await this.adapter.requestDevice();
      this.device.lost.then(() => {
        this.adapter = this.device = null;
        this.resources.clear();
        for (const pointer of this.views.keys()) this.view().setUint32(pointer + VIEW.gpuDevice, 0, true);
        if (globalThis.location?.reload) globalThis.location.reload();
        else this.initializeGPU();
      });
      this.context = this.canvas.getContext("webgpu");
      this.format = navigator.gpu.getPreferredCanvasFormat();
      this.resizeCanvas();
      return true;
    } catch (error) {
      console.warn("Nano Script: WebGPU initialization failed", error);
      this.adapter = this.device = null;
      return false;
    }
  }

  resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.canvas.clientWidth * dpr));
    const height = Math.max(1, Math.round(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
    }
    if (this.device && this.context) {
      this.context.configure({ device: this.device, format: this.format, alphaMode: "premultiplied" });
    }
    for (const pointer of this.views.keys()) this.syncView(pointer);
    return [width, height];
  }

  syncView(pointer) {
    const record = this.views.get(Number(pointer));
    if (!record || !this.memory) return;
    const dpr = globalThis.window?.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.canvas.clientWidth || record.requestedWidth || this.canvas.width / dpr || 1));
    const height = Math.max(1, Math.round(this.canvas.clientHeight || record.requestedHeight || this.canvas.height / dpr || 1));
    const view = this.view(), p = record.pointer;
    view.setInt32(p + VIEW.width, width, true);
    view.setInt32(p + VIEW.height, height, true);
    view.setInt32(p + VIEW.framebufferWidth, this.canvas.width || Math.round(width * dpr), true);
    view.setInt32(p + VIEW.framebufferHeight, this.canvas.height || Math.round(height * dpr), true);
    view.setFloat64(p + VIEW.displayRatio, dpr, true);
    view.setFloat64(p + VIEW.uiScale, dpr, true);
    view.setUint32(p + VIEW.nativeWindow, 1, true); // browser canvas handle
    view.setUint32(p + VIEW.gpuDevice, this.device ? 1 : 0, true);
  }

  createView(title, width, height) {
    const pointer = this.allocStruct(VIEW.size);
    this.view().setUint32(pointer + VIEW.title, Number(title), true);
    this.views.set(pointer, {
      pointer, requestedWidth: Number(width), requestedHeight: Number(height),
      events: [], eventStorage: [], gesture: 0, closed: false,
    });
    this.activeView = pointer;
    this.closed = false;
    if (this.canvas) {
      this.canvas.tabIndex = 0;
      this.canvas.setAttribute?.("aria-label", this.readString(title) || "Nano Script canvas");
    }
    this.syncView(pointer);
    this.installViewEvents();
    return pointer;
  }

  eventModifiers(event) {
    return (event?.shiftKey ? 1 : 0) | (event?.ctrlKey ? 2 : 0) |
      (event?.altKey ? 4 : 0) | (event?.metaKey ? 8 : 0);
  }

  browserKey(event) {
    const key = event?.key || "";
    if (key.length === 1) {
      const code = key.toUpperCase().charCodeAt(0);
      if ((code >= 48 && code <= 57) || (code >= 65 && code <= 90)) return code;
    }
    return ({ Escape: 256, Enter: 257, Tab: 258, Backspace: 259, Insert: 260,
      Delete: 261, ArrowRight: 262, ArrowLeft: 263, ArrowDown: 264, ArrowUp: 265,
      PageUp: 266, PageDown: 267, Home: 268, End: 269, CapsLock: 280,
      ScrollLock: 281, NumLock: 282, PrintScreen: 283, Pause: 284,
      Shift: 340, Control: 341, Alt: 342, Meta: 343, " ": 32 })[key] ?? Number(event?.keyCode || 0);
  }

  pushViewEvent(pointer, values) {
    const record = this.views.get(Number(pointer));
    if (!record || record.events.length >= 512) return 0;
    const index = record.events.length;
    const eventPointer = record.eventStorage[index] || this.allocStruct(VIEW_INPUT_EVENT_SIZE);
    record.eventStorage[index] = eventPointer;
    new Uint8Array(this.memory.buffer, eventPointer, VIEW_INPUT_EVENT_SIZE).fill(0);
    const view = this.view();
    [values.device, values.phase, values.pointerId, values.modifiers].forEach((value, i) =>
      view.setInt32(eventPointer + i * 4, Number(value || 0), true));
    [values.x, values.y, values.dx, values.dy, values.pressure, values.altitude,
      values.azimuth, values.timestamp].forEach((value, i) =>
      view.setFloat64(eventPointer + 16 + i * 8, Number(value || 0), true));
    view.setInt32(eventPointer + 80, Number(values.toolAction || 0), true);
    record.events.push(eventPointer);
    return eventPointer;
  }

  pointerPosition(event) {
    const rect = this.canvas.getBoundingClientRect?.() || { left: 0, top: 0 };
    return [Number(event.clientX || 0) - rect.left, Number(event.clientY || 0) - rect.top];
  }

  installViewEvents() {
    if (this.viewEventsInstalled) return;
    this.viewEventsInstalled = true;
    const canvas = this.canvas;
    canvas.addEventListener?.("pointermove", event => {
      if (!this.activeView) return;
      const [x, y] = this.pointerPosition(event);
      const device = event.pointerType === "touch" ? 1 : event.pointerType === "pen" ? 2 : 0;
      this.viewImport("view_on_pointer_event", [this.activeView, device, 2, event.pointerId || 0,
        x, y, event.pressure || 0, event.altitudeAngle || 0, event.azimuthAngle || 0,
        event.timeStamp || performance.now(), this.eventModifiers(event)]);
    });
    const button = (event, action) => {
      if (!this.activeView) return;
      const [x, y] = this.pointerPosition(event);
      this.viewImport("view_on_mouse_move", [this.activeView, x, y]);
      this.viewImport("view_on_mouse_btn", [this.activeView, event.button === 2 ? 1 : event.button === 1 ? 2 : 0, action]);
      canvas.focus?.();
    };
    canvas.addEventListener?.("pointerdown", event => {
      canvas.setPointerCapture?.(event.pointerId);
      button(event, 0);
    });
    const release = event => {
      button(event, 1);
      if (canvas.hasPointerCapture?.(event.pointerId)) canvas.releasePointerCapture?.(event.pointerId);
    };
    canvas.addEventListener?.("pointerup", release);
    canvas.addEventListener?.("pointercancel", release);
    canvas.addEventListener?.("wheel", event => {
      if (this.activeView) this.viewImport("view_on_scroll", [this.activeView, event.deltaX || 0, event.deltaY || 0]);
      event.preventDefault?.();
    }, { passive: false });
    globalThis.window?.addEventListener?.("resize", () => this.resizeCanvas());
    globalThis.window?.addEventListener?.("keydown", event => {
      if (this.activeView) this.viewImport("view_on_key_action", [this.activeView, this.browserKey(event), 0, this.eventModifiers(event)]);
    });
    globalThis.window?.addEventListener?.("keyup", event => {
      if (this.activeView) this.viewImport("view_on_key_action", [this.activeView, this.browserKey(event), 1, this.eventModifiers(event)]);
    });
    globalThis.window?.addEventListener?.("paste", event => {
      this.clipboard = event.clipboardData?.getData("text/plain") || this.clipboard;
    });
  }

  viewImport(name, a) {
    const pointer = Number(a[0] || 0), record = this.views.get(pointer), view = () => this.view();
    if (name === "view_create" || name === "view_create_no_title") return this.createView(a[0], a[1], a[2]);
    if (name === "view_run") { if (record) { this.activeView = pointer; this.syncView(pointer); } return; }
    if (name === "view_close") { if (record) record.closed = true; if (pointer === this.activeView) this.closed = true; return; }
    if (name === "view_capture_require") { if (record) view().setInt32(pointer + VIEW.captureRequired, 1, true); return; }
    if (!record) {
      if (name === "view_get_clipboard") return this.writeString("");
      return name === "view_take_key_press" ? -1 : 0;
    }
    if (name === "view_on_resize") {
      const width = Math.max(1, Number(a[1])), height = Math.max(1, Number(a[2]));
      const dpr = globalThis.window?.devicePixelRatio || 1;
      record.requestedWidth = width; record.requestedHeight = height;
      view().setInt32(pointer + VIEW.width, width, true);
      view().setInt32(pointer + VIEW.height, height, true);
      view().setInt32(pointer + VIEW.framebufferWidth, Math.round(width * dpr), true);
      view().setInt32(pointer + VIEW.framebufferHeight, Math.round(height * dpr), true);
      return;
    }
    if (name === "view_on_scroll") {
      view().setFloat64(pointer + VIEW.scrollX, view().getFloat64(pointer + VIEW.scrollX, true) + Number(a[1]), true);
      view().setFloat64(pointer + VIEW.scrollY, view().getFloat64(pointer + VIEW.scrollY, true) + Number(a[2]), true);
      return;
    }
    if (name === "view_on_pointer_event") {
      const previousX = view().getFloat64(pointer + VIEW.mouseX, true), previousY = view().getFloat64(pointer + VIEW.mouseY, true);
      const x = Number(a[4]), y = Number(a[5]);
      view().setFloat64(pointer + VIEW.mouseX, x, true); view().setFloat64(pointer + VIEW.mouseY, y, true);
      this.pushViewEvent(pointer, { device: a[1], phase: a[2], pointerId: a[3], modifiers: a[10],
        x, y, dx: x - previousX, dy: y - previousY, pressure: a[6], altitude: a[7],
        azimuth: a[8], timestamp: a[9] });
      return;
    }
    if (name === "view_on_mouse_move") {
      return this.viewImport("view_on_pointer_event", [pointer, 0, 2, 0, a[1], a[2], 0, 0, 0,
        globalThis.performance?.now?.() || 0, 0]);
    }
    if (name === "view_on_mouse_btn") {
      const offsets = Number(a[1]) === 1
        ? [VIEW.rightDown, VIEW.rightPressed, VIEW.rightReleased]
        : Number(a[1]) === 2
          ? [VIEW.middleDown, VIEW.middlePressed, VIEW.middleReleased]
          : [VIEW.mouseDown, VIEW.mousePressed, VIEW.mouseReleased];
      const pressed = Number(a[2]) === 0;
      view().setInt32(pointer + offsets[0], pressed ? 1 : 0, true);
      view().setInt32(pointer + offsets[pressed ? 1 : 2], 1, true);
      this.pushViewEvent(pointer, { device: 0, phase: pressed ? 1 : 3, pointerId: 0,
        x: view().getFloat64(pointer + VIEW.mouseX, true), y: view().getFloat64(pointer + VIEW.mouseY, true),
        timestamp: globalThis.performance?.now?.() || 0 });
      return;
    }
    if (name === "view_on_key_action") {
      const key = Number(a[1]), pressed = Number(a[2]) === 0;
      if (pressed) { this.keysDown.add(key); this.keyPresses.set(key, Number(a[3] || 0)); }
      else this.keysDown.delete(key);
      return;
    }
    if (name === "view_is_key_pressed") return this.keysDown.has(Number(a[1])) ? 1 : 0;
    if (name === "view_take_key_press") {
      const key = Number(a[1]);
      if (!this.keyPresses.has(key)) return -1;
      const modifiers = this.keyPresses.get(key); this.keyPresses.delete(key); return modifiers;
    }
    if (name === "view_clear_key_presses") { this.keyPresses.clear(); return; }
    if (name === "view_on_tool_action") {
      this.pushViewEvent(pointer, { device: 4, phase: 5, timestamp: a[2], toolAction: a[1] }); return;
    }
    if (name === "view_on_gesture") {
      const gesture = this.viewImport("view_gesture", [pointer]);
      view().setFloat64(gesture, view().getFloat64(gesture, true) + Number(a[1]), true);
      view().setFloat64(gesture + 8, view().getFloat64(gesture + 8, true) + Number(a[2]), true);
      if (Number(a[3]) > 0) view().setFloat64(gesture + 16, view().getFloat64(gesture + 16, true) * Number(a[3]), true);
      view().setFloat64(gesture + 24, view().getFloat64(gesture + 24, true) + Number(a[4]), true);
      return;
    }
    if (name === "view_input_count") return record.events.length;
    if (name === "view_input_at") return record.events[Number(a[1])] || 0;
    if (name === "view_gesture") {
      if (!record.gesture) { record.gesture = this.allocStruct(VIEW_GESTURE_SIZE); view().setFloat64(record.gesture + 16, 1, true); }
      return record.gesture;
    }
    if (name === "view_input_pending") {
      return record.events.length || this.keyPresses.size || view().getFloat64(pointer + VIEW.scrollX, true) ||
        view().getFloat64(pointer + VIEW.scrollY, true) ? 1 : 0;
    }
    if (name === "view_input_reset") {
      record.events.length = 0;
      [VIEW.scrollX, VIEW.scrollY].forEach(offset => view().setFloat64(pointer + offset, 0, true));
      [VIEW.mousePressed, VIEW.mouseReleased, VIEW.rightPressed, VIEW.rightReleased,
        VIEW.middlePressed, VIEW.middleReleased].forEach(offset => view().setInt32(pointer + offset, 0, true));
      this.keyPresses.clear();
      if (record.gesture) { new Uint8Array(this.memory.buffer, record.gesture, VIEW_GESTURE_SIZE).fill(0); view().setFloat64(record.gesture + 16, 1, true); }
      return;
    }
    if (name === "view_get_clipboard") return this.writeString(this.clipboard);
    if (name === "view_set_clipboard") {
      this.clipboard = this.readString(a[1]);
      globalThis.navigator?.clipboard?.writeText?.(this.clipboard).catch?.(() => {});
      return;
    }
    throw new Error(`browser view backend does not implement ${name}`);
  }

  importsFor(module) {
    const imports = {};
    for (const item of WebAssembly.Module.imports(module)) {
      if (item.kind !== "function") continue;
      imports[item.module] ||= {};
      imports[item.module][item.name] = (...args) => this.invoke(item.module, item.name, args);
    }
    return imports;
  }

  loadShaders(module) {
    const sections = WebAssembly.Module.customSections(module, "ns.shaders");
    if (!sections.length) return;
    const bytes = new Uint8Array(sections[0]);
    let offset = 0;
    const leb = () => {
      let value = 0, shift = 0, byte;
      do { byte = bytes[offset++]; value |= (byte & 0x7f) << shift; shift += 7; } while (byte & 0x80);
      return value >>> 0;
    };
    const string = () => { const n = leb(); const value = textDecoder.decode(bytes.subarray(offset, offset + n)); offset += n; return value; };
    const version = leb();
    if (version !== 1) throw new Error(`unsupported ns.shaders version ${version}`);
    const count = leb();
    for (let i = 0; i < count; i++) {
      const id = leb(), stage = bytes[offset++], name = string(), wgsl = string();
      const stride = leb(), attributeCount = leb(), attributes = [];
      for (let a = 0; a < attributeCount; a++) attributes.push({ offset: leb(), size: leb() });
      this.shaders.set(id, { id, stage, name, wgsl, stride, attributes });
    }
  }

  invoke(namespace, name, args) {
    if (namespace === "std") return this.std(name, args);
    if (namespace === "view") return this.viewImport(name, args);
    if (namespace === "gpu") return this.gpu(name, args);
    if (namespace === "shader") return this.shader(name, args);
    throw new Error(`unsupported Nano Script Wasm import ${namespace}.${name}`);
  }

  std(name, a) {
    const unaryMath = { sin: Math.sin, cos: Math.cos, tan: Math.tan, asin: Math.asin,
      acos: Math.acos, atan: Math.atan, sqrt: Math.sqrt, floor: Math.floor,
      ceil: Math.ceil, round: Math.round, exp: Math.exp, log: Math.log };
    if (name === "print") { console.log(this.readString(a[0]).replace(/\n$/, "")); return; }
    if (unaryMath[name]) return unaryMath[name](a[0]);
    if (name === "pow") return Math.pow(a[0], a[1]);
    if (name === "atan2") return Math.atan2(a[0], a[1]);
    if (name === "abs") return Math.abs(a[0]);
    if (name === "min") return Math.min(a[0], a[1]);
    if (name === "max") return Math.max(a[0], a[1]);
    if (name === "ftos" || name === "itos") return this.writeString(a[0]);
    if (name === "stof" || name === "stoi") return Number(this.readString(a[0]));
    if (name === "substr") {
      const descriptor = Number(a[0]), view = this.view();
      const bytes = view.getUint32(descriptor, true), length = view.getUint32(descriptor + 4, true);
      const start = Math.max(0, Math.min(length, Number(a[1])));
      const count = Math.max(0, Math.min(length - start, Number(a[2])));
      return this.writeString(textDecoder.decode(new Uint8Array(this.memory.buffer, bytes + start, count)));
    }
    if (name === "utf8_len") return Array.from(this.readString(a[0])).length;
    if (name === "unescape") return this.writeString(this.readString(a[0]));
    throw new Error(`unsupported portable std import ${name}`);
  }

  shader(name, a) {
    const metadata = this.shaders.get(Number(a[0]));
    if (name === "shader_entry") return this.writeString(metadata?.name || "main");
    if (name === "shader_transpile" || name === "shader_transpile_stage") return this.writeString(metadata?.wgsl || "");
    if (name === "shader_vertex_stride") return metadata?.stride || 0;
    if (name === "shader_vertex_attr_count") return metadata?.attributes.length || 0;
    if (name === "shader_vertex_attr_offset") return metadata?.attributes[Number(a[1])]?.offset || 0;
    if (name === "shader_vertex_attr_size") return metadata?.attributes[Number(a[1])]?.size || 0;
    throw new Error(`shader intrinsic ${name} may only execute inside pre-transpiled WGSL`);
  }

  bufferUsage() {
    if (!globalThis.GPUBufferUsage) return 0;
    return GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE |
      GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.INDIRECT;
  }

  createBuffer(size, uniform = false) {
    if (!this.device) return 0;
    const usage = uniform
      ? GPUBufferUsage.COPY_DST | GPUBufferUsage.UNIFORM
      : this.bufferUsage();
    return this.put("buffer", this.device.createBuffer({ size: align(Math.max(4, Number(size)), 4), usage }));
  }

  ensureEncoder() {
    if (this.device && !this.commandEncoder) this.commandEncoder = this.device.createCommandEncoder();
    return this.commandEncoder;
  }

  formatFor(value, depth = false) {
    if (depth || value === 43) return "depth24plus";
    if (value === 23) return "rgba8unorm";
    return this.format;
  }

  texture(id) { return this.get(id, "texture"); }

  textureView(id) { return this.texture(id)?.texture.createView() || null; }

  graphicsPipeline(shaderHandle = this.currentShader, stateHandle = this.currentState) {
    const shader = this.get(shaderHandle, "shader");
    if (!shader?.vertex || !shader?.fragment || !this.device) return null;
    shader.pipelines ||= new Map();
    const state = this.get(stateHandle, "state") || {};
    const key = `${stateHandle}:${this.format}`;
    if (!shader.pipelines.has(key)) {
      const topology = state.primitive === 1 ? "line-list" : "triangle-list";
      shader.pipelines.set(key, this.device.createRenderPipeline({
        layout: "auto",
        vertex: { module: shader.vertex, entryPoint: shader.vs || "main" },
        fragment: { module: shader.fragment, entryPoint: shader.fs || "main", targets: [{ format: this.format }] },
        primitive: { topology, cullMode: state.cull === 2 ? "back" : "none" },
        ...(state.depthWrite ? { depthStencil: { format: "depth24plus", depthWriteEnabled: true, depthCompare: "less-equal" } } : {}),
      }));
    }
    return shader.pipelines.get(key);
  }

  gpu(name, a) {
    const view = () => this.view();
    if (name === "dispatch_gpu") {
      const shader = this.shaders.get(Number(a[0]));
      if (!shader) return 0;
      return this.gpu("gpu_dispatch_compute_source", [this.writeString(shader.wgsl), this.writeString(shader.name), a[1], a[2], a[3]]);
    }
    if (name === "gpu_texture_new" || name === "gpu_texture_new_2d" || name === "gpu_texture_none") {
      const values = name === "gpu_texture_none" ? [0, 0, 0, 0, 1, 0, 0, 0] :
        name === "gpu_texture_new_2d" ? [0, a[0], a[1], 1, a[2], a[3], 1, 0] : [0, ...a];
      if (name !== "gpu_texture_none") values[0] = this.gpu("gpu_texture_create", values.slice(1));
      const result = this.allocStruct(32);
      values.forEach((value, i) => view().setUint32(result + i * 4, Number(value), true));
      return result;
    }
    if (name === "gpu_texture_valid") return view().getUint32(Number(a[0]), true) !== 0 ? 1 : 0;
    if (name === "gpu_texture_bytes") {
      const p = Number(a[0]);
      return BigInt(this.gpu("gpu_pixel_format_surface_pitch", [view().getInt32(p + 16, true), view().getInt32(p + 4, true), view().getInt32(p + 8, true), 1]));
    }
    if (name === "gpu_texture_update" || name === "gpu_texture_update_all") {
      const p = Number(a[0]), size = name === "gpu_texture_update_all" ? this.gpu("gpu_texture_bytes", [p]) : a[4];
      this.gpu("gpu_texture_upload", [view().getUint32(p, true), name === "gpu_texture_update_all" ? 0 : a[1], name === "gpu_texture_update_all" ? 0 : a[2], name === "gpu_texture_update_all" ? a[1] : a[3], size]);
      return;
    }
    if (name === "gpu_texture_release") { this.gpu("gpu_texture_destroy", [view().getUint32(Number(a[0]), true)]); return; }
    if (name === "gpu_sampler_new") {
      const id = this.gpu("gpu_sampler_create", a), result = this.allocStruct(36);
      view().setUint32(result, Number(id), true);
      a.forEach((value, i) => view().setInt32(result + 4 + i * 4, Number(value), true));
      return result;
    }
    if (name === "gpu_sampler_valid") return view().getUint32(Number(a[0]), true) !== 0 ? 1 : 0;
    if (name === "gpu_sampler_release") { this.gpu("gpu_sampler_destroy", [view().getUint32(Number(a[0]), true)]); return; }
    if (name === "gpu_render_state_new") {
      const id = this.gpu("gpu_state_create", a), result = this.allocStruct(32);
      view().setUint32(result, Number(id), true);
      a.forEach((value, i) => view().setInt32(result + 4 + i * 4, Number(value), true));
      return result;
    }
    if (name === "gpu_render_state_bind") { this.gpu("gpu_set_state", [view().getUint32(Number(a[0]), true)]); return; }
    if (name === "gpu_memory_alloc") {
      const result = this.allocStruct(24), address = this.gpu("gpu_malloc", a);
      view().setBigUint64(result, BigInt(address), true); view().setBigUint64(result + 8, BigInt(a[0]), true); view().setUint32(result + 16, Number(a[1]), true);
      return result;
    }
    if (name === "gpu_memory_valid") return view().getBigUint64(Number(a[0]), true) !== 0n ? 1 : 0;
    if (name === "gpu_memory_at") {
      const p = Number(a[0]), offset = BigInt(a[1]), size = view().getBigUint64(p + 8, true);
      return offset < size ? view().getBigUint64(p, true) + offset : 0n;
    }
    if (name === "gpu_memory_write" || name === "gpu_memory_read") {
      const p = Number(a[0]), offset = BigInt(a[1]), size = BigInt(a[3]), extent = view().getBigUint64(p + 8, true);
      if (offset + size > extent) return name === "gpu_memory_read" ? 0 : undefined;
      return this.gpu(name === "gpu_memory_read" ? "gpu_read" : "gpu_write", [view().getBigUint64(p, true) + offset, a[2], a[3]]);
    }
    if (name === "gpu_memory_free") { this.gpu("gpu_free", [view().getBigUint64(Number(a[0]), true)]); return; }
    if (name === "gpu_shader_graphics" || name === "gpu_shader_compute") {
      const compute = name === "gpu_shader_compute", first = this.shaders.get(Number(a[0])), second = compute ? null : this.shaders.get(Number(a[1]));
      if (!first || (!compute && !second)) return 0;
      const id = compute ? this.gpu("gpu_shader_compute_create", [this.writeString(first.wgsl), this.writeString(first.name)]) :
        this.gpu("gpu_shader_graphics_create", [this.writeString(first.wgsl), this.writeString(second.wgsl), this.writeString(first.name), this.writeString(second.name)]);
      const result = this.allocStruct(64);
      view().setUint32(result, Number(id), true); view().setUint32(result + 4, compute ? 1 : 0, true);
      view().setUint32(result + 16, this.writeString("wgsl"), true);
      view().setUint32(result + 32, this.writeString(first.name), true);
      view().setUint32(result + 48, this.writeString(second?.name || ""), true);
      return result;
    }
    if (name === "gpu_shader_valid") return view().getUint32(Number(a[0]), true) !== 0 ? 1 : 0;
    if (name === "gpu_shader_bind") { this.gpu("gpu_set_shader", [view().getUint32(Number(a[0]), true)]); return; }
    if (name === "gpu_shader_release") { this.gpu("gpu_shader_destroy", [view().getUint32(Number(a[0]), true)]); return; }
    if (name === "gpu_pass_begin_target") {
      const color = Number(a[0]), depth = Number(a[1]);
      this.gpu("gpu_pass_begin", [view().getUint32(color, true), 0, 0, 0, view().getUint32(depth, true), a[2], ...a.slice(3)]);
      const width = view().getInt32(color + 4, true), height = view().getInt32(color + 8, true);
      if (width > 0 && height > 0) this.gpu("gpu_set_viewport", [0, 0, width, height]);
      return;
    }
    if (name === "gpu_request_device") {
      const owner = Number(a[0] || 0);
      if (owner && !this.views.has(owner)) return 0;
      if (owner) { this.activeView = owner; this.syncView(owner); }
      return this.device ? 1 : 0;
    }
    if (name === "gpu_destroy_device") { this.resources.clear(); return; }
    if (name === "gpu_shader_target") return this.writeString("wgsl");
    if (name === "gpu_caps") return this.device ? 2 | 4 : 0;
    if (name === "gpu_create_buffer" || name === "gpu_create_index_buffer") return this.createBuffer(a[0]);
    if (name === "gpu_create_uniform_buffer") return this.createBuffer(a[0], true);
    if (name === "gpu_update_buffer") {
      const buffer = this.get(a[0], "buffer");
      if (buffer && this.device) this.device.queue.writeBuffer(buffer, 0, this.readBytes(a[1], Number(a[2])));
      return;
    }
    if (name === "gpu_destroy_buffer_id") { this.get(a[0], "buffer")?.destroy(); this.drop(a[0]); return; }
    if (name === "gpu_create_texture_2d" || name === "gpu_texture_create") {
      if (!this.device) return 0;
      const depth = name === "gpu_texture_create" ? Math.max(1, Number(a[2])) : 1;
      const formatArg = name === "gpu_texture_create" ? a[3] : a[2];
      const width = Math.max(1, Number(a[0])), height = Math.max(1, Number(a[1]));
      const format = this.formatFor(formatArg);
      const texture = this.device.createTexture({
        size: [width, height, depth],
        format,
        usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING |
          GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT,
        mipLevelCount: name === "gpu_texture_create" ? Math.max(1, Number(a[5])) : 1,
      });
      return this.put("texture", { texture, width, height, depth, format });
    }
    if (name === "gpu_destroy_texture_id" || name === "gpu_texture_destroy") {
      this.texture(a[0])?.texture.destroy(); this.drop(a[0]); return;
    }
    if (name === "gpu_update_texture_id" || name === "gpu_texture_upload") {
      const record = this.texture(a[0]);
      if (!record || !this.device?.queue.writeTexture) return;
      const dataArg = name === "gpu_texture_upload" ? a[3] : a[1];
      const sizeArg = name === "gpu_texture_upload" ? a[4] : a[2];
      const bytes = this.readBytes(dataArg, Number(sizeArg));
      const bytesPerRow = Math.max(4, record.width * 4);
      this.device.queue.writeTexture(
        { texture: record.texture, mipLevel: name === "gpu_texture_upload" ? Number(a[1]) : 0, origin: [0, 0, name === "gpu_texture_upload" ? Number(a[2]) : 0] },
        bytes, { bytesPerRow, rowsPerImage: record.height }, [record.width, record.height, 1],
      );
      return;
    }
    if (name === "gpu_sampler_create") {
      if (!this.device) return 0;
      const filter = (v) => Number(v) ? "linear" : "nearest";
      return this.put("sampler", this.device.createSampler({ minFilter: filter(a[0]), magFilter: filter(a[1]), mipmapFilter: filter(a[2]) }));
    }
    if (name === "gpu_sampler_destroy") { this.drop(a[0]); return; }
    if (name === "gpu_create_shader_source" || name === "gpu_shader_graphics_create") {
      if (!this.device) return 0;
      const vertex = this.device.createShaderModule({ code: this.readString(a[0]) });
      const fragment = this.device.createShaderModule({ code: this.readString(a[1]) });
      return this.put("shader", { vertex, fragment, vs: this.readString(a[2]) || "main", fs: this.readString(a[3]) || "main" });
    }
    if (name === "gpu_dispatch_compute_source" || name === "gpu_dispatch_compute_texture_source") {
      if (!this.device) return 0;
      const texture = name === "gpu_dispatch_compute_texture_source" ? this.texture(a[2]) : null;
      if (name === "gpu_dispatch_compute_texture_source" && (!texture || !this.device.createBindGroup)) return 0;
      const pipeline = this.device.createComputePipeline({
        layout: "auto",
        compute: { module: this.device.createShaderModule({ code: this.readString(a[0]) }), entryPoint: this.readString(a[1]) || "main" },
      });
      const encoder = this.device.createCommandEncoder();
      const pass = encoder.beginComputePass();
      pass.setPipeline(pipeline);
      if (name === "gpu_dispatch_compute_texture_source") {
        pass.setBindGroup(0, this.device.createBindGroup({
          layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: texture.texture.createView() }],
        }));
        pass.dispatchWorkgroups(Number(a[3]), Number(a[4]), Number(a[5]));
      } else {
        pass.dispatchWorkgroups(Number(a[2]), Number(a[3]), Number(a[4]));
      }
      pass.end();
      this.device.queue.submit([encoder.finish()]);
      return 1;
    }
    if (name === "gpu_create_pipeline" || name === "gpu_create_pipeline_ex") {
      if (!this.device) return 0;
      const vs = this.shaders.get(Number(a[0])), fs = this.shaders.get(Number(a[1]));
      if (!vs || !fs) throw new Error("pipeline references shader metadata that is not in ns.shaders");
      const formatForSize = (size) => size === 4 ? "float32" : size === 8 ? "float32x2" : size === 12 ? "float32x3" : "float32x4";
      const buffers = vs.stride ? [{ arrayStride: vs.stride, attributes: vs.attributes.map((x, i) => ({ shaderLocation: i, offset: x.offset, format: formatForSize(x.size) })) }] : [];
      const pipeline = this.device.createRenderPipeline({
        layout: "auto",
        vertex: { module: this.device.createShaderModule({ code: vs.wgsl }), entryPoint: vs.name, buffers },
        fragment: { module: this.device.createShaderModule({ code: fs.wgsl }), entryPoint: fs.name, targets: [{ format: this.formatFor(a[2]) }] },
        primitive: { topology: Number(a[3]) === 1 ? "line-list" : "triangle-list" },
      });
      return this.put("pipeline", pipeline);
    }
    if (name === "gpu_create_pipeline_layout" || name === "gpu_create_pipeline_layout_ex" || name === "gpu_create_pipeline_layout_indexed_ex") {
      if (!this.device) return 0;
      const shader = this.get(a[0], "shader");
      if (!shader?.vertex || !shader?.fragment) return 0;
      const count = Number(a[5]);
      const offsets = this.readI32Array(a[2], count), sizes = this.readI32Array(a[3], count);
      const formatForSize = size => size === 4 ? "float32" : size === 8 ? "float32x2" : size === 12 ? "float32x3" : "float32x4";
      const buffers = count ? [{ arrayStride: Number(a[1]), attributes: offsets.map((offset, i) => ({ shaderLocation: i, offset, format: formatForSize(sizes[i]) })) }] : [];
      const extended = name !== "gpu_create_pipeline_layout";
      const depthOffset = name === "gpu_create_pipeline_layout_indexed_ex" ? 9 : 8;
      const depthFormat = extended ? Number(a[depthOffset]) : 0;
      const pipeline = this.device.createRenderPipeline({
        layout: "auto",
        vertex: { module: shader.vertex, entryPoint: shader.vs, buffers },
        fragment: { module: shader.fragment, entryPoint: shader.fs, targets: [{ format: this.formatFor(a[6]) }] },
        primitive: { topology: Number(a[7]) === 1 ? "line-list" : "triangle-list", cullMode: extended && Number(a[depthOffset + 3]) === 2 ? "back" : "none" },
        ...(depthFormat ? { depthStencil: { format: this.formatFor(depthFormat, true), depthWriteEnabled: !!a[depthOffset + 2], depthCompare: "less-equal" } } : {}),
      });
      return this.put("pipeline", pipeline);
    }
    if (name === "gpu_shader_compute_create") {
      if (!this.device) return 0;
      return this.put("shader", { compute: this.device.createShaderModule({ code: this.readString(a[0]) }), cs: this.readString(a[1]) || "main" });
    }
    if (name === "gpu_destroy_shader_id" || name === "gpu_shader_destroy") { this.drop(a[0]); return; }
    if (name === "gpu_state_create") return this.put("state", { primitive: Number(a[0]), cull: Number(a[1]), depthWrite: !!a[4], blend: Number(a[5]) });
    if (name === "gpu_create_texture_binding") return this.put("binding", { pipeline: Number(a[0]), textures: [Number(a[1])] });
    if (name === "gpu_create_buffer_texture_binding") return this.put("binding", { pipeline: Number(a[0]), buffer: Number(a[1]), textures: [Number(a[3]), Number(a[5])] });
    if (name === "gpu_create_depth_pass") return this.put("render-pass", { depth: Number(a[0]) });
    if (name === "gpu_set_shader") { this.currentShader = Number(a[0]); return; }
    if (name === "gpu_set_state") { this.currentState = Number(a[0]); return; }
    if (name === "gpu_screen_pass_begin" || name === "gpu_create_screen_pass") {
      if (name === "gpu_create_screen_pass") return this.put("screen-pass", [...a]);
      if (!this.device || !this.context) return;
      this.ensureEncoder();
      this.pass = this.commandEncoder.beginRenderPass({ colorAttachments: [{
        view: this.context.getCurrentTexture().createView(),
        clearValue: { r: a[0], g: a[1], b: a[2], a: a[3] }, loadOp: "clear", storeOp: "store",
      }] });
      return;
    }
    if (name === "gpu_pass_begin") {
      if (!this.device) return;
      this.ensureEncoder();
      const colorAttachments = [];
      for (let i = 0; i < 4; i++) {
        const view = this.textureView(a[i]);
        if (view) colorAttachments.push({ view, clearValue: { r: a[6], g: a[7], b: a[8], a: a[9] }, loadOp: ((Number(a[5]) >> (i * 2)) & 3) === 1 ? "load" : "clear", storeOp: "store" });
      }
      const depthView = this.textureView(a[4]);
      this.pass = this.commandEncoder.beginRenderPass({ colorAttachments, ...(depthView ? { depthStencilAttachment: { view: depthView, depthClearValue: a[10], depthLoadOp: ((Number(a[5]) >> 8) & 3) === 1 ? "load" : "clear", depthStoreOp: "store" } } : {}) });
      return;
    }
    if (name === "gpu_begin_render_pass_id") {
      const pass = this.resources.get(Number(a[0]));
      if (pass?.kind === "render-pass") return this.gpu("gpu_pass_begin", [0, 0, 0, 0, pass.value.depth, 0, 0, 0, 0, 1, 1]);
      const clear = pass?.kind === "screen-pass" ? pass.value : [0, 0, 0, 1];
      return this.gpu("gpu_screen_pass_begin", clear);
    }
    if (name === "gpu_set_pipeline_id") { const p = this.get(a[0], "pipeline"); if (p) this.pass?.setPipeline(p); return; }
    if (name === "gpu_set_binding_id") {
      const binding = this.get(a[0], "binding");
      const pipeline = binding && this.get(binding.pipeline, "pipeline");
      if (binding && pipeline && this.pass && this.device.createBindGroup) {
        const entries = [];
        if (binding.buffer) { const buffer = this.get(binding.buffer, "buffer"); if (buffer) entries.push({ binding: entries.length, resource: { buffer } }); }
        for (const texture of binding.textures) { const view = this.textureView(texture); if (view) entries.push({ binding: entries.length, resource: view }); }
        this.pass.setBindGroup(0, this.device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries }));
      }
      return;
    }
    if (name === "gpu_create_mesh_1") return this.put("mesh", { pipeline: Number(a[0]), vertex: Number(a[1]) });
    if (name === "gpu_create_mesh_indexed") return this.put("mesh", { pipeline: Number(a[0]), vertex: Number(a[1]), index: Number(a[2]), indexType: Number(a[3]) });
    if (name === "gpu_set_mesh_id") {
      const mesh = this.get(a[0], "mesh");
      if (mesh && this.pass) {
        const pipeline = this.get(mesh.pipeline, "pipeline"); if (pipeline) this.pass.setPipeline(pipeline);
        const vertex = this.get(mesh.vertex, "buffer"); if (vertex) this.pass.setVertexBuffer(0, vertex);
        const index = this.get(mesh.index, "buffer"); if (index) this.pass.setIndexBuffer(index, mesh.indexType === 2 ? "uint32" : "uint16");
      }
      return;
    }
    if (name === "gpu_begin_render_pass" || name === "gpu_set_pipeline" || name === "gpu_set_binding" || name === "gpu_set_mesh") {
      const id = a[0] ? this.view().getInt32(Number(a[0]), true) : 0;
      const target = name === "gpu_begin_render_pass" ? "gpu_begin_render_pass_id" :
        name === "gpu_set_pipeline" ? "gpu_set_pipeline_id" : name === "gpu_set_binding" ? "gpu_set_binding_id" : "gpu_set_mesh_id";
      return this.gpu(target, [id]);
    }
    if (name === "gpu_set_viewport") { this.pass?.setViewport(Number(a[0]), Number(a[1]), Number(a[2]), Number(a[3]), 0, 1); return; }
    if (name === "gpu_set_scissor") { this.pass?.setScissorRect(Number(a[0]), Number(a[1]), Number(a[2]), Number(a[3])); return; }
    if (name === "gpu_pass_end" || name === "gpu_end_pass") { this.pass?.end(); this.pass = null; return; }
    if (name === "gpu_commit") {
      if (this.pass) this.gpu("gpu_pass_end", []);
      if (this.commandEncoder && this.device) this.device.queue.submit([this.commandEncoder.finish()]);
      this.commandEncoder = null;
      this.frameIndex++;
      const nextFrame = this.frameBuffers[this.frameIndex % 3];
      if (nextFrame) nextFrame.offset = 0;
      return;
    }
    if (name === "gpu_draw" || name === "gpu_draw_vertices") {
      if (name === "gpu_draw_vertices") { const pipeline = this.graphicsPipeline(); if (pipeline) this.pass?.setPipeline(pipeline); }
      this.pass?.draw(Number(a[1]), Number(a[2]) || 1, Number(a[0]), 0); return;
    }
    if (name === "gpu_draw_indexed") {
      const { id, offset } = this.addressParts(a[0]);
      const buffer = this.get(id, "buffer");
      const pipeline = this.graphicsPipeline(); if (pipeline) this.pass?.setPipeline(pipeline);
      if (buffer && this.pass) { this.pass.setIndexBuffer(buffer, Number(a[1]) === 2 ? "uint32" : "uint16", offset); this.pass.drawIndexed(Number(a[2]), Number(a[3]) || 1, 0, Number(a[4]), 0); }
      return;
    }
    if (name === "gpu_draw_indirect") {
      const { id, offset } = this.addressParts(a[0]);
      const buffer = this.get(id, "buffer"), count = Number(a[1]), stride = Math.max(16, Number(a[2]));
      const pipeline = this.graphicsPipeline(); if (pipeline) this.pass?.setPipeline(pipeline);
      if (buffer && this.pass?.drawIndirect) for (let i = 0; i < count; i++) this.pass.drawIndirect(buffer, offset + i * stride);
      return;
    }
    if (name === "gpu_dispatch") {
      const shader = this.get(this.currentShader, "shader");
      if (!shader?.compute || !this.device) return;
      shader.pipeline ||= this.device.createComputePipeline({ layout: "auto", compute: { module: shader.compute, entryPoint: shader.cs } });
      const encoder = this.ensureEncoder(), pass = encoder.beginComputePass();
      pass.setPipeline(shader.pipeline); pass.dispatchWorkgroups(Number(a[0]), Number(a[1]), Number(a[2])); pass.end(); return;
    }
    if (name === "gpu_dispatch_indirect") {
      const shader = this.get(this.currentShader, "shader"), address = this.addressParts(a[0]), buffer = this.get(address.id, "buffer");
      if (!shader?.compute || !buffer || !this.device) return;
      shader.pipeline ||= this.device.createComputePipeline({ layout: "auto", compute: { module: shader.compute, entryPoint: shader.cs } });
      const encoder = this.ensureEncoder(), pass = encoder.beginComputePass();
      pass.setPipeline(shader.pipeline); pass.dispatchWorkgroupsIndirect(buffer, address.offset); pass.end(); return;
    }
    if (name === "gpu_malloc") {
      const id = this.createBuffer(Number(a[0]));
      return BigInt(id) << 32n;
    }
    if (name === "gpu_frame_alloc") {
      if (!this.device) return 0n;
      const ring = this.frameIndex % 3;
      if (!this.frameBuffers[ring]) {
        const usage = this.bufferUsage() | GPUBufferUsage.UNIFORM;
        this.frameBuffers[ring] = { id: this.put("buffer", this.device.createBuffer({ size: 1024 * 1024, usage })), offset: 0 };
      }
      const frame = this.frameBuffers[ring], alignment = Number(a[1]) || 256;
      frame.offset = align(frame.offset, alignment);
      const offset = frame.offset; frame.offset += Number(a[0]);
      if (frame.offset > 1024 * 1024) return 0n;
      return (BigInt(frame.id) << 32n) | BigInt(offset);
    }
    if (name === "gpu_free") { const { id } = this.addressParts(a[0]); this.get(id, "buffer")?.destroy(); this.drop(id); return; }
    if (name === "gpu_write") {
      const { id, offset } = this.addressParts(a[0]);
      const buffer = this.get(id, "buffer");
      if (buffer && this.device) this.device.queue.writeBuffer(buffer, offset, this.readBytes(a[1], Number(a[2]))); return;
    }
    if (name === "gpu_set_root") { this.currentRoot = BigInt(a[0]); return; }
    if (name === "gpu_set_root_data") {
      const address = this.gpu("gpu_frame_alloc", [a[1], 256]);
      if (address) { this.gpu("gpu_write", [address, a[0], a[1]]); this.currentRoot = address; }
      return;
    }
    if (name === "gpu_read") return 0;
    if (name === "gpu_draw_indirect" || name === "gpu_dispatch_indirect" ||
        name === "gpu_signal_after" || name === "gpu_wait_before" || name === "gpu_set_root" ||
        name === "gpu_set_root_data" || name === "gpu_set_viewport" || name === "gpu_set_scissor" ||
        name === "gpu_set_binding_id") return;
    if (name === "gpu_pixel_format_size") return (Number(a[0]) === 43 ? 4 : 4);
    if (name === "gpu_pixel_format_row_pitch") return align(Number(a[1]) * 4, Math.max(1, Number(a[2])));
    if (name === "gpu_pixel_format_surface_pitch") return align(Number(a[1]) * 4, Math.max(1, Number(a[3]))) * Number(a[2]);
    if (name.startsWith("gpu_destroy_")) { this.drop(a[0]); return; }
    // Capability-dependent and legacy resource combinations are safe no-ops
    // when there is no device. A device-backed implementation must explicitly
    // add them above so accidental rendering omissions remain visible.
    if (!this.device) return name.includes("create") ? 0 : undefined;
    throw new Error(`WebGPU middleware does not implement ${name}`);
  }
}

function reloadOverlay() {
  let overlay = document.getElementById("__ns-build-error");
  if (!overlay) {
    overlay = document.createElement("div");
    overlay.id = "__ns-build-error";
    overlay.style.cssText = "position:fixed;inset:0 auto auto 0;z-index:2147483647;background:#7f1d1d;color:white;padding:12px 16px;font:14px ui-monospace,monospace;white-space:pre-wrap";
    document.body.append(overlay);
  }
  overlay.textContent = "Nano Script rebuild failed. See terminal diagnostics.";
}

function connectReloadSocket() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${location.host}/__ns/reload`);
  socket.onmessage = ({ data }) => {
    try {
      const message = JSON.parse(data);
      if (message.type === "reload") location.reload();
      if (message.type === "build-error") reloadOverlay();
    } catch (_) { /* Ignore application WebSocket traffic on this private route. */ }
  };
  socket.onclose = () => setTimeout(connectReloadSocket, 500);
}

function isLoopbackPage() {
  return location.hostname === "localhost" || location.hostname === "127.0.0.1" || location.hostname === "::1";
}

export async function boot(wasmURL) {
  const canvas = document.getElementById("ns-canvas") || document.querySelector("canvas");
  if (!canvas) throw new Error("Nano Script Wasm shell requires a canvas");
  // Connect before compiling so a page that loaded a temporarily invalid
  // artifact can still recover after the next successful rebuild.
  if (isLoopbackPage()) connectReloadSocket();
  const runtime = new NSBrowserRuntime(canvas);
  const response = await fetch(wasmURL, { cache: "no-store" });
  if (!response.ok) throw new Error(`failed to fetch ${wasmURL}: ${response.status}`);
  const module = WebAssembly.compileStreaming
    ? await WebAssembly.compileStreaming(Promise.resolve(response))
    : await WebAssembly.compile(await response.arrayBuffer());
  runtime.loadShaders(module);
  const instance = await WebAssembly.instantiate(module, runtime.importsFor(module));
  runtime.instance = instance;
  runtime.memory = instance.exports.memory;
  await runtime.initializeGPU();
  instance.exports.__ns_init?.();
  const status = instance.exports.main?.();
  if (typeof status === "number" && status !== 0) console.warn(`Nano Script main returned ${status}`);
  if (typeof instance.exports.frame === "function") {
    const frame = (time) => {
      if (runtime.closed) return;
      const [width, height] = runtime.resizeCanvas();
      instance.exports.frame(time, width, height);
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }
  return { module, instance, runtime };
}

export { NSBrowserRuntime };
