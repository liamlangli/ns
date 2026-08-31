// Nano Script browser middleware. This file is copied beside project Wasm
// artifacts by `ns build` and intentionally has no package/runtime dependency.

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

function align(value, n) { return Math.ceil(value / n) * n; }

// Bindings 0...6 are reserved by the portable shader prelude for textures,
// samplers and root uniforms. Storage slots start after that range.
const STORAGE_BINDING_BASE = 7;

// The wasm32 struct layout a browser target compiles to: fields are laid out in
// declaration order, each aligned to its own size capped at four bytes. Strings,
// arrays and function values are four-byte handles, an `any` handle takes eight,
// a bool takes a four-byte slot, and f64 keeps its eight-byte payload at
// four-byte alignment. `ns build` gives lib structs this same layout rather than
// the C layout of the host that compiled the bundle, because nothing allocates
// them natively here (see ns_ssa_native_struct).

// lib/view.ns::view.
const VIEW = Object.freeze({
  size: 172, title: 0, width: 4, height: 8, framebufferWidth: 12,
  framebufferHeight: 16, mouseX: 20, mouseY: 28, scrollX: 36, scrollY: 44,
  mouseDown: 52, mousePressed: 56, mouseReleased: 60,
  rightDown: 64, rightPressed: 68, rightReleased: 72,
  middleDown: 76, middlePressed: 80, middleReleased: 84,
  displayRatio: 88, uiScale: 96,
  safeAreaTop: 104, safeAreaRight: 112, safeAreaBottom: 120, safeAreaLeft: 128,
  nativeWindow: 136, gpuDevice: 144,
  captureRequired: 152, captureStarted: 156,
});

// lib/ui.ns::ui_rect, and ui_color_rgba with the same four f64 fields.
const UI_RECT = Object.freeze({ x: 0, y: 8, w: 16, h: 24, size: 32 });
// lib/ui.ns::ui_insets, four logical-point margins.
const UI_INSETS = Object.freeze({ top: 0, right: 8, bottom: 16, left: 24, size: 32 });
// lib/ui.ns::ui_text_size and ui_hit.
const UI_TEXT_SIZE = Object.freeze({ w: 0, h: 8, size: 16 });
const UI_HIT = Object.freeze({ hovered: 0, pressed: 4, size: 8 });
// lib/ui.ns::ui_text_sel, the shared selection of read-only labels.
const UI_TEXT_SEL = Object.freeze({ active: 0, anchor: 4, head: 8, dragging: 12, size: 16 });
// An opaque handle struct: ui_renderer, ui_widgets, ui_theme.
const UI_HANDLE_SIZE = 8;
// lib/ui.ns::ui_input, the per-frame pointer and keyboard snapshot.
const UI_INPUT = Object.freeze({
  mouseX: 0, mouseY: 8, mouseDown: 16, mousePressed: 20, mouseReleased: 24,
  mouseMiddleDown: 28, mouseRightPressed: 32, mouseRightDown: 36,
  panDx: 40, panDy: 48, zoomFactor: 56, wheelY: 64,
  typedText: 72, imeComposition: 76,
  keyBackspace: 80, keyDelete: 84, keyEnter: 88, keyEscape: 92,
  keyLeft: 96, keyRight: 100, keyUp: 104, keyDown: 108,
  keyHome: 112, keyEnd: 116, keyPageUp: 120, keyPageDown: 124,
  keyA: 128, keyC: 132, shift: 136, ctrl: 140, meta: 144, alt: 148,
  gizmoManipulating: 152, size: 156,
});
// Texture ids 0..2 are the white and font textures a renderer reserves;
// application atlases start after them.
const UI_FIRST_ATLAS = 3;

// Ratios of the 42 px design size in lib/assets/latin_mono.json, the atlas a
// native renderer loads at init: the line box, and the middle of the cap band
// (cap top .. baseline) that ui_text_v_center_y centers in a rect. The browser
// draws with its own font stack and measures with that same stack, so it only
// borrows these vertical metrics to keep text sitting where a native build puts
// it. FONT_ZH and FONT_BITMAP ship no atlas, and ui_primary_font falls back to
// FONT_MAIN for a face whose atlas is missing.
const UI_FONT_MAIN_METRICS = Object.freeze({
  line: 50 / 42, capCenter: 23 / 42,
  stack: 'Lato, -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif',
});
const UI_FONT_MONO_METRICS = Object.freeze({
  line: 55 / 42, capCenter: 31.5 / 42,
  stack: "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace",
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
    this.uiContext = null;
    // The canvas the ui module paints on. It is the application canvas itself
    // unless WebGPU already holds that one's context; see initializeCanvasUI.
    this.uiCanvas = null;
    // Whether the loaded module imports `gpu`, which decides what a device
    // request is asking about; see hasDevice.
    this.usesGPU = false;
    this.format = "rgba8unorm";
    this.configuredDevice = null;
    this.configuredFormat = "";
    this.resources = new Map();
    this.nextHandle = 1;
    this.commandEncoder = null;
    this.pass = null;
    this.currentShader = 0;
    this.currentState = 0;
    this.currentRoot = 0n;
    this.currentRootSize = 0;
    this.currentRootWords = new Float32Array();
    this.defaultSampler = null;
    this.rg11Storage = false;
    this.gpuErrorLogged = false;
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
    this.uiRenderers = new Map();
    this.uiWidgetLayers = new Map();
    this.uiStatics = new Map();
    this.uiAtlases = new Map();
    this.nextUIAtlas = UI_FIRST_ATLAS;
    this.uiBatches = new Map();
    this.nextUIBatch = 1;
    this.virtualFiles = new Map([
      ["nscode/native/main.ns", "// NSCode browser workspace\\n\\nfn main() {\\n    print(\"hello from Nano Script\")\\n}\\n"],
      ["nscode/native/editor.ns", "// Shared NSCode document model\\n"],
      ["nscode/native/render.ns", "// Shared NSCode UI renderer\\n"],
      ["nscode/native/workspace.ns", "// Shared NSCode workspace model\\n"],
    ]);
    this.fileDescriptors = new Map();
    this.nextFileDescriptor = 1;
    this.scanEntries = [];
    this.predecodedGzip = [];
    this.decodedImages = new Map();
    this.glbs = new Map();
    this.meshes = new Map();
    this.nextIOHandle = 1;
    this.storageApp = "ns";
    this.storageError = "";
    this.storageValues = new Map();
    this.storageCache = new Map();
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

  readF64Array(descriptor, count) {
    if (!descriptor || !this.memory || count <= 0) return [];
    const data = this.view().getUint32(Number(descriptor), true);
    return Array.from(new Float64Array(this.memory.buffer, data, Number(count)));
  }

  readI32Array(descriptor, count) {
    if (!descriptor || !this.memory || count <= 0) return [];
    const data = this.view().getUint32(Number(descriptor), true);
    return Array.from(new Int32Array(this.memory.buffer, data, Number(count)));
  }

  writeArray(descriptor, values, kind = "u8", capacity = values.length) {
    if (!descriptor || !this.memory) return 0;
    const pointer = this.view().getUint32(Number(descriptor), true);
    const count = Math.min(Number(capacity), values.length);
    const constructors = { u8: Uint8Array, i32: Int32Array, u32: Uint32Array, f32: Float32Array };
    const ArrayType = constructors[kind] || Uint8Array;
    new ArrayType(this.memory.buffer, pointer, count).set(values.subarray ? values.subarray(0, count) : values.slice(0, count));
    return count;
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
      this.rg11Storage = this.adapter.features?.has?.("texture-formats-tier1") || false;
      this.device = await this.adapter.requestDevice({
        requiredFeatures: this.rg11Storage ? ["texture-formats-tier1"] : [],
      });
      this.device.addEventListener?.("uncapturederror", event => {
        if (!this.gpuErrorLogged) {
          console.error("Nano Script WebGPU validation:", event.error?.message || event.error || event);
          this.gpuErrorLogged = true;
        }
      });
      this.device.lost.then(() => {
        this.adapter = this.device = null;
        this.configuredDevice = null;
        this.configuredFormat = "";
        this.resources.clear();
        for (const pointer of this.views.keys()) this.view().setUint32(pointer + VIEW.gpuDevice, 0, true);
        if (globalThis.location?.reload) globalThis.location.reload();
        else this.initializeGPU();
      });
      this.context = this.canvas.getContext("webgpu");
      // The swapchain must use the browser/device preference. Asking for an
      // RGBA canvas on the usual BGRA device inserts a copy on every frame and
      // some implementations fail to present it reliably. Explicit RGBA8
      // textures remain RGBA through formatFor(GPU_PIXELFORMAT_RGBA8).
      this.format = navigator.gpu.getPreferredCanvasFormat?.() || "rgba8unorm";
      this.resizeCanvas();
      return true;
    } catch (error) {
      console.warn("Nano Script: WebGPU initialization failed", error);
      this.adapter = this.device = null;
      return false;
    }
  }

  // A canvas has one context. An application that draws its scene with `gpu`
  // and its chrome with `ui` therefore cannot have both on the application
  // canvas, so the ui module gets a transparent canvas of its own laid over
  // that one. It carries no pointer events, so input still reaches the view
  // through the canvas underneath, and a ui-only application keeps drawing
  // straight onto the application canvas as before.
  createUIOverlay() {
    const document = globalThis.document;
    const overlay = document?.createElement?.("canvas");
    if (!overlay) return null;
    overlay.id = "ns-ui-overlay";
    if (overlay.style) {
      overlay.style.position = "fixed";
      overlay.style.margin = "0";
      overlay.style.padding = "0";
      overlay.style.border = "0";
      overlay.style.background = "transparent";
      overlay.style.pointerEvents = "none";
    }
    (this.canvas?.parentNode || document.body)?.appendChild?.(overlay);
    return overlay;
  }

  initializeCanvasUI() {
    try {
      // `this.context` is the WebGPU context of the application canvas, so its
      // presence is exactly the case that needs an overlay of its own.
      this.uiCanvas = this.context ? this.createUIOverlay() || this.canvas : this.canvas;
      this.uiContext = this.uiCanvas.getContext("2d");
      this.resizeCanvas();
      return !!this.uiContext;
    } catch (error) {
      console.warn("Nano Script: Canvas UI initialization failed", error);
      this.uiCanvas = null;
      this.uiContext = null;
      return false;
    }
  }

  // A ui-only application never holds a WebGPU device: the canvas it paints on
  // is what ui_renderer_create asks a view for, so that canvas answers as the
  // device. An application that imports `gpu` is asking about the real device
  // instead, and only the real device answers for it - it has a scene to draw
  // and needs to hear that it cannot.
  hasDevice() {
    return !!(this.device || (!this.usesGPU && this.uiContext));
  }

  // Keep the overlay on top of the application canvas: the same box on the
  // page, and the same backing-store size so one point is one point on both.
  syncUIOverlay(width, height) {
    const overlay = this.uiCanvas;
    if (!overlay || overlay === this.canvas) return;
    const rect = this.canvas.getBoundingClientRect?.();
    if (rect && overlay.style) {
      const box = `${rect.left}|${rect.top}|${rect.width}|${rect.height}`;
      if (overlay.nsOverlayBox !== box) {
        overlay.nsOverlayBox = box;
        overlay.style.left = `${rect.left}px`;
        overlay.style.top = `${rect.top}px`;
        overlay.style.width = `${rect.width}px`;
        overlay.style.height = `${rect.height}px`;
      }
    }
    // Assigning either dimension clears the canvas, so only a real change.
    if (overlay.width !== width) overlay.width = width;
    if (overlay.height !== height) overlay.height = height;
  }

  resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.canvas.clientWidth * dpr));
    const height = Math.max(1, Math.round(this.canvas.clientHeight * dpr));
    const resized = this.canvas.width !== width || this.canvas.height !== height;
    if (resized) {
      this.canvas.width = width;
      this.canvas.height = height;
    }
    this.syncUIOverlay(width, height);
    if (this.device && this.context &&
        (resized || this.configuredDevice !== this.device || this.configuredFormat !== this.format)) {
      this.context.configure({ device: this.device, format: this.format, alphaMode: "premultiplied" });
      this.configuredDevice = this.device;
      this.configuredFormat = this.format;
    }
    for (const pointer of this.views.keys()) this.syncView(pointer);
    return [width, height];
  }

  // CSS env(safe-area-inset-*) reports the display chrome of the whole viewport
  // (the iOS notch and home indicator, an Android cutout). Only the part of a
  // margin the canvas actually reaches into applies to this view.
  safeAreaInsets() {
    const styles = globalThis.getComputedStyle;
    if (!styles || !globalThis.document?.body) return { top: 0, right: 0, bottom: 0, left: 0 };
    if (!this.safeAreaProbe) {
      const probe = globalThis.document.createElement("div");
      probe.style.cssText = "position:fixed;top:0;left:0;width:0;height:0;visibility:hidden;pointer-events:none;" +
        "padding-top:env(safe-area-inset-top);padding-right:env(safe-area-inset-right);" +
        "padding-bottom:env(safe-area-inset-bottom);padding-left:env(safe-area-inset-left);";
      globalThis.document.body.appendChild(probe);
      this.safeAreaProbe = probe;
    }
    const style = styles(this.safeAreaProbe);
    const px = value => Math.max(0, parseFloat(value) || 0);
    const viewport = { top: px(style.paddingTop), right: px(style.paddingRight), bottom: px(style.paddingBottom), left: px(style.paddingLeft) };
    const rect = this.canvas?.getBoundingClientRect?.();
    if (!rect) return viewport;
    const clip = (inset, gap) => Math.max(0, inset - (Number.isFinite(gap) && gap > 0 ? gap : 0));
    return {
      top: clip(viewport.top, rect.top),
      right: clip(viewport.right, (globalThis.innerWidth || rect.right) - rect.right),
      bottom: clip(viewport.bottom, (globalThis.innerHeight || rect.bottom) - rect.bottom),
      left: clip(viewport.left, rect.left),
    };
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
    const insets = this.safeAreaInsets();
    view.setFloat64(p + VIEW.safeAreaTop, insets.top, true);
    view.setFloat64(p + VIEW.safeAreaRight, insets.right, true);
    view.setFloat64(p + VIEW.safeAreaBottom, insets.bottom, true);
    view.setFloat64(p + VIEW.safeAreaLeft, insets.left, true);
    view.setUint32(p + VIEW.nativeWindow, 1, true); // browser canvas handle
    view.setUint32(p + VIEW.gpuDevice, this.hasDevice() ? 1 : 0, true);
  }

  createView(title, width, height) {
    const pointer = this.allocStruct(VIEW.size);
    this.view().setUint32(pointer + VIEW.title, Number(title), true);
    this.views.set(pointer, {
      pointer, requestedWidth: Number(width), requestedHeight: Number(height),
      events: [], eventStorage: [], pointerPositions: new Map(), activePointers: new Set(),
      mousePointerDown: false, gamepads: [], gamepadPresses: new Set(),
      gesture: 0, closed: false,
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

  refreshGamepads(record) {
    const pads = globalThis.navigator?.getGamepads?.() || [];
    const next = [];
    for (let slot = 0; slot < 4; slot++) {
      const pad = pads[slot];
      if (!pad || (pad.mapping && pad.mapping !== "standard")) {
        for (let button = 0; button < 17; button++) record.gamepadPresses.delete(`${slot}:${button}`);
        next[slot] = null;
        continue;
      }
      const previous = record.gamepads[slot];
      const buttons = Array.from({ length: 17 }, (_, button) => {
        const source = pad.buttons?.[button];
        const value = Math.max(0, Math.min(1, Number(source?.value ?? (source?.pressed ? 1 : 0)) || 0));
        const pressed = Boolean(source?.pressed) || value > 0.5;
        if (pressed && !previous?.buttons?.[button]?.pressed) {
          record.gamepadPresses.add(`${slot}:${button}`);
        }
        return { value, pressed };
      });
      const axes = Array.from({ length: 4 }, (_, axis) =>
        Math.max(-1, Math.min(1, Number(pad.axes?.[axis]) || 0)));
      next[slot] = { axes, buttons };
    }
    record.gamepads = next;
    return next;
  }

  pointerPosition(event) {
    const rect = this.canvas.getBoundingClientRect?.() || { left: 0, top: 0 };
    return [Number(event.clientX || 0) - rect.left, Number(event.clientY || 0) - rect.top];
  }

  installViewEvents() {
    if (this.viewEventsInstalled) return;
    this.viewEventsInstalled = true;
    const canvas = this.canvas;
    // Keep two-finger controls in the application instead of handing their
    // gesture to browser scrolling or zooming, which sends pointercancel.
    if (canvas.style) canvas.style.touchAction = "none";
    const device = event => event.pointerType === "touch" ? 1 : event.pointerType === "pen" ? 2 : 0;
    const pointer = (event, phase) => {
      if (!this.activeView) return;
      const [x, y] = this.pointerPosition(event);
      this.viewImport("view_on_pointer_event", [this.activeView, device(event), phase,
        event.pointerType === "mouse" ? 0 : event.pointerId || 0,
        x, y, event.pressure || 0, event.altitudeAngle || 0, event.azimuthAngle || 0,
        event.timeStamp || performance.now(), this.eventModifiers(event)]);
    };
    const touchMouseCompatibility = (event, phase) => {
      if (!this.activeView) return;
      const record = this.views.get(Number(this.activeView));
      if (!record) return;
      const id = Number(event.pointerId || 0);
      const [x, y] = this.pointerPosition(event);
      const view = this.view(), p = record.pointer;
      view.setFloat64(p + VIEW.mouseX, x, true);
      view.setFloat64(p + VIEW.mouseY, y, true);
      if (phase === 1) {
        record.activePointers.add(id);
        view.setInt32(p + VIEW.mousePressed, 1, true);
      } else if (phase === 3 || phase === 4) {
        record.activePointers.delete(id);
        view.setInt32(p + VIEW.mouseReleased, 1, true);
      }
      view.setInt32(p + VIEW.mouseDown, record.activePointers.size || record.mousePointerDown ? 1 : 0, true);
    };
    canvas.addEventListener?.("pointermove", event => {
      if (event.pointerType === "mouse") {
        if (!this.activeView) return;
        const [x, y] = this.pointerPosition(event);
        this.viewImport("view_on_mouse_move", [this.activeView, x, y]);
        return;
      }
      pointer(event, 2);
      touchMouseCompatibility(event, 2);
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
      if (event.pointerType === "mouse") button(event, 0);
      else {
        pointer(event, 1);
        touchMouseCompatibility(event, 1);
        canvas.focus?.();
      }
    });
    const release = (event, phase) => {
      if (event.pointerType === "mouse") button(event, 1);
      else {
        pointer(event, phase);
        touchMouseCompatibility(event, phase);
      }
      if (canvas.hasPointerCapture?.(event.pointerId)) canvas.releasePointerCapture?.(event.pointerId);
    };
    canvas.addEventListener?.("pointerup", event => release(event, 3));
    canvas.addEventListener?.("pointercancel", event => release(event, 4));
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
    if (name === "view_set_safe_area") {
      if (!record) return;
      const inset = value => Math.max(0, Number(value) || 0);
      view().setFloat64(pointer + VIEW.safeAreaTop, inset(a[1]), true);
      view().setFloat64(pointer + VIEW.safeAreaRight, inset(a[2]), true);
      view().setFloat64(pointer + VIEW.safeAreaBottom, inset(a[3]), true);
      view().setFloat64(pointer + VIEW.safeAreaLeft, inset(a[4]), true);
      return;
    }
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
      if (!record) return;
      const pointerId = Number(a[3]);
      const previous = record.pointerPositions.get(pointerId);
      const x = Number(a[4]), y = Number(a[5]), phase = Number(a[2]);
      const dx = previous ? x - previous[0] : 0, dy = previous ? y - previous[1] : 0;
      if (phase === 3 || phase === 4) record.pointerPositions.delete(pointerId);
      else record.pointerPositions.set(pointerId, [x, y]);
      this.pushViewEvent(pointer, { device: a[1], phase: a[2], pointerId: a[3], modifiers: a[10],
        x, y, dx, dy, pressure: a[6], altitude: a[7],
        azimuth: a[8], timestamp: a[9] });
      return;
    }
    if (name === "view_on_mouse_move") {
      view().setFloat64(pointer + VIEW.mouseX, Number(a[1]), true);
      view().setFloat64(pointer + VIEW.mouseY, Number(a[2]), true);
      const dragging = view().getInt32(pointer + VIEW.mouseDown, true) ||
        view().getInt32(pointer + VIEW.rightDown, true) || view().getInt32(pointer + VIEW.middleDown, true);
      return this.viewImport("view_on_pointer_event", [pointer, 0, dragging ? 2 : 0, 0, a[1], a[2], 0, 0, 0,
        globalThis.performance?.now?.() || 0, 0]);
    }
    if (name === "view_on_mouse_btn") {
      const button = Number(a[1]);
      const offsets = button === 1
        ? [VIEW.rightDown, VIEW.rightPressed, VIEW.rightReleased]
        : button === 2
          ? [VIEW.middleDown, VIEW.middlePressed, VIEW.middleReleased]
          : [VIEW.mouseDown, VIEW.mousePressed, VIEW.mouseReleased];
      const pressed = Number(a[2]) === 0;
      if (record && button === 0) record.mousePointerDown = pressed;
      const down = button === 0 && record ? record.activePointers.size || record.mousePointerDown : pressed;
      view().setInt32(pointer + offsets[0], down ? 1 : 0, true);
      view().setInt32(pointer + offsets[pressed ? 1 : 2], 1, true);
      return this.viewImport("view_on_pointer_event", [pointer, 0, pressed ? 1 : 3, 0,
        view().getFloat64(pointer + VIEW.mouseX, true), view().getFloat64(pointer + VIEW.mouseY, true),
        pressed ? 1 : 0, 0, 0, globalThis.performance?.now?.() || 0, 0]);
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
    if (name.startsWith("view_gamepad_") || name === "view_take_gamepad_button_press") {
      const gamepads = this.refreshGamepads(record);
      const slot = Number(a[1]), item = gamepads[slot];
      if (name === "view_gamepad_count") return gamepads.filter(Boolean).length;
      if (name === "view_gamepad_connected") return item ? 1 : 0;
      if (!item) return 0;
      if (name === "view_gamepad_axis") return item.axes[Number(a[2])] || 0;
      const button = Number(a[2]), state = item.buttons[button];
      if (name === "view_gamepad_button") return state?.value || 0;
      if (name === "view_gamepad_button_pressed") return state?.pressed ? 1 : 0;
      const edge = `${slot}:${button}`;
      if (!record.gamepadPresses.has(edge)) return 0;
      record.gamepadPresses.delete(edge);
      return 1;
    }
    // The shell already drives a frame per requestAnimationFrame tick, so an
    // application asking for a redraw burst, a delayed redraw, or a frame-rate
    // cap is already served.
    if (name === "view_request_frame" || name === "view_request_frame_after" ||
        name === "view_set_frame_per_second") return;
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
      this.refreshGamepads(record);
      return record.events.length || this.keyPresses.size || view().getFloat64(pointer + VIEW.scrollX, true) ||
        view().getFloat64(pointer + VIEW.scrollY, true) || record.gamepadPresses.size ? 1 : 0;
    }
    if (name === "view_input_reset") {
      record.events.length = 0;
      [VIEW.scrollX, VIEW.scrollY].forEach(offset => view().setFloat64(pointer + offset, 0, true));
      [VIEW.mousePressed, VIEW.mouseReleased, VIEW.rightPressed, VIEW.rightReleased,
        VIEW.middlePressed, VIEW.middleReleased].forEach(offset => view().setInt32(pointer + offset, 0, true));
      this.keyPresses.clear();
      record.gamepadPresses.clear();
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
    if (namespace === "wasm") return this.wasm(name, args);
    if (namespace === "view") return this.viewImport(name, args);
    if (namespace === "gpu") return this.gpu(name, args);
    if (namespace === "os") return this.os(name, args);
    if (namespace === "net") return this.net(name, args);
    if (namespace === "storage") return this.storage(name, args);
    if (namespace === "compress") return this.compress(name, args);
    if (namespace === "io") return this.io(name, args);
    if (namespace === "shader") return this.shader(name, args);
    if (namespace === "ui") return this.ui(name, args);
    throw new Error(`unsupported Nano Script Wasm import ${namespace}.${name}`);
  }

  storedFile(path) {
    if (this.virtualFiles.has(path)) {
      const value = this.virtualFiles.get(path);
      return value instanceof Uint8Array ? textDecoder.decode(value) : value;
    }
    try {
      const value = globalThis.localStorage?.getItem?.(`ns:file:${path}`);
      return value === null || value === undefined ? null : value;
    } catch (_) {
      return null;
    }
  }

  storeFile(path, value) {
    const text = String(value);
    this.virtualFiles.set(path, text);
    try { globalThis.localStorage?.setItem?.(`ns:file:${path}`, text); } catch (_) { /* storage is optional */ }
  }

  storedBytes(path) {
    const value = this.virtualFiles.get(path);
    if (value instanceof Uint8Array) return value;
    if (typeof value === "string") return textEncoder.encode(value);
    try {
      const encoded = globalThis.localStorage?.getItem?.(`ns:bytes:${path}`);
      if (encoded) return Uint8Array.from(atob(encoded), ch => ch.charCodeAt(0));
    } catch (_) { /* storage is optional */ }
    const text = this.storedFile(path);
    return text === null ? null : textEncoder.encode(text);
  }

  storeBytes(path, value) {
    const bytes = Uint8Array.from(value);
    this.virtualFiles.set(path, bytes);
    try {
      let binary = "";
      for (let offset = 0; offset < bytes.length; offset += 0x8000) {
        binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
      }
      globalThis.localStorage?.setItem?.(`ns:bytes:${path}`, btoa(binary));
    } catch (_) { /* storage is optional */ }
  }

  async preloadFiles(paths = []) {
    for (const path of paths) {
      const response = await fetch(path, { cache: "no-store" });
      if (!response.ok) throw new Error(`failed to load asset ${path}: ${response.status}`);
      const bytes = new Uint8Array(await response.arrayBuffer());
      this.virtualFiles.set(path.replace(/^\.\//, ""), bytes);
      if (path.endsWith(".anim") && bytes.length > 32 && typeof DecompressionStream === "function") {
        const stream = new Blob([bytes.subarray(32)]).stream().pipeThrough(new DecompressionStream("gzip"));
        const decoded = new Uint8Array(await new Response(stream).arrayBuffer());
        this.predecodedGzip.push({ encoded: bytes.slice(32), decoded });
      }
      if (path.endsWith(".glb")) await this.predecodeGLBImage(path.replace(/^\.\//, ""), bytes);
    }
  }

  parseGLB(bytes) {
    if (bytes.length < 20 || new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(0, true) !== 0x46546c67) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let offset = 12, jsonBytes = null, data = new Uint8Array();
    while (offset + 8 <= bytes.length) {
      const length = view.getUint32(offset, true), type = view.getUint32(offset + 4, true);
      const chunk = bytes.slice(offset + 8, offset + 8 + length);
      if (type === 0x4e4f534a) jsonBytes = chunk;
      if (type === 0x004e4942) data = chunk;
      offset += 8 + length;
    }
    if (!jsonBytes) return null;
    const text = textDecoder.decode(jsonBytes).replace(/[\u0000\s]+$/, "");
    return { json: JSON.parse(text), jsonBytes, data, image: null };
  }

  glbAccessor(glb, index) {
    const accessor = glb.json.accessors?.[Number(index)];
    if (!accessor) return new Float32Array();
    const bufferView = glb.json.bufferViews?.[accessor.bufferView];
    if (!bufferView) return new Float32Array();
    const components = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 }[accessor.type] || 1;
    const info = {
      5120: [1, "getInt8", true], 5121: [1, "getUint8", true],
      5122: [2, "getInt16", true], 5123: [2, "getUint16", true],
      5125: [4, "getUint32", true], 5126: [4, "getFloat32", false],
    }[accessor.componentType];
    if (!info) return new Float32Array();
    const [size, getter, integer] = info;
    const stride = bufferView.byteStride || size * components;
    const base = (bufferView.byteOffset || 0) + (accessor.byteOffset || 0);
    const result = new Float64Array(accessor.count * components);
    const view = new DataView(glb.data.buffer, glb.data.byteOffset, glb.data.byteLength);
    for (let item = 0; item < accessor.count; item++) {
      for (let component = 0; component < components; component++) {
        let value = view[getter](base + item * stride + component * size, true);
        if (accessor.normalized && integer) {
          const signed = accessor.componentType === 5120 || accessor.componentType === 5122;
          const bits = size * 8;
          value = signed ? Math.max(-1, value / (2 ** (bits - 1) - 1)) : value / (2 ** bits - 1);
        }
        result[item * components + component] = value;
      }
    }
    return result;
  }

  async predecodeGLBImage(path, bytes) {
    const glb = this.parseGLB(bytes);
    const image = glb?.json.images?.[0];
    const bufferView = image && glb.json.bufferViews?.[image.bufferView];
    if (!bufferView || typeof createImageBitmap !== "function") return;
    const encoded = glb.data.slice(bufferView.byteOffset || 0, (bufferView.byteOffset || 0) + bufferView.byteLength);
    const bitmap = await createImageBitmap(new Blob([encoded], { type: image.mimeType || "image/png" }));
    const canvas = document.createElement("canvas");
    canvas.width = bitmap.width; canvas.height = bitmap.height;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    context.drawImage(bitmap, 0, 0);
    const rgba = new Uint8Array(context.getImageData(0, 0, bitmap.width, bitmap.height).data);
    this.decodedImages.set(path, { width: bitmap.width, height: bitmap.height, rgba });
    bitmap.close?.();
  }

  wasm(name, a) {
    if (name === "strcat") return this.writeString(this.readString(a[0]) + this.readString(a[1]));
    if (name === "ftos") return this.writeString(Number(a[0]));
    if (name === "itos" || name === "utos") return this.writeString(BigInt(a[0]).toString());
    if (name === "btos") return this.writeString(a[0] ? "true" : "false");
    throw new Error(`unsupported Nano Script Wasm helper ${name}`);
  }

  net(name) {
    if (name === "net_close" || name === "net_set_nonblocking" || name === "net_udp_set_broadcast") return 0;
    if (name === "net_recv_try") return 0;
    if (name === "net_buf_read") return 0;
    if (name === "net_udp_sender_address_byte" || name === "net_udp_sender_port") return 0;
    return -1;
  }

  storageKey(key) { return `ns:storage:${this.storageApp}:${key}`; }

  storageValue(key) {
    if (this.storageValues.has(key)) return this.storageValues.get(key);
    try {
      const raw = globalThis.localStorage?.getItem?.(this.storageKey(key));
      return raw === null || raw === undefined ? undefined : JSON.parse(raw);
    } catch (_) { return undefined; }
  }

  setStorageValue(key, value) {
    this.storageValues.set(key, value);
    try { globalThis.localStorage?.setItem?.(this.storageKey(key), JSON.stringify(value)); } catch (_) { /* optional */ }
    return 1;
  }

  cacheKey(name, hash) { return `${name}:${BigInt(hash).toString(16)}`; }

  storage(name, a) {
    if (name === "storage_init") { this.storageApp = this.readString(a[0]) || "ns"; return 1; }
    if (name === "storage_last_error") return this.writeString(this.storageError);
    if (name.startsWith("storage_kv_set_")) {
      const key = this.readString(a[0]);
      const value = name.endsWith("str") ? { type: "str", value: this.readString(a[1]) } :
        name.endsWith("i64") ? { type: "i64", value: BigInt(a[1]).toString() } :
        name.endsWith("bool") ? { type: "bool", value: !!a[1] } : { type: "f64", value: Number(a[1]) };
      return this.setStorageValue(key, value);
    }
    if (name.startsWith("storage_kv_get_")) {
      const value = this.storageValue(this.readString(a[0]));
      if (!value) return name.endsWith("str") ? a[1] : a[1];
      if (name.endsWith("str")) return this.writeString(String(value.value));
      if (name.endsWith("i64")) return BigInt(value.value);
      if (name.endsWith("bool")) return value.value ? 1 : 0;
      return Number(value.value);
    }
    if (name === "storage_kv_remove") {
      const key = this.readString(a[0]); this.storageValues.delete(key);
      try { globalThis.localStorage?.removeItem?.(this.storageKey(key)); } catch (_) { /* optional */ }
      return 1;
    }
    if (name === "storage_kv_sync") return 1;
    if (name === "storage_cache_hash_str") {
      let hash = 1469598103934665603n;
      for (const byte of textEncoder.encode(this.readString(a[0]))) hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * 1099511628211n);
      return hash;
    }
    if (name === "storage_cache_path") return this.writeString(`.ns-cache/${this.cacheKey(this.readString(a[0]), a[1])}`);
    if (name === "storage_cache_has") return this.storageCache.has(this.cacheKey(this.readString(a[0]), a[1])) ? 1 : 0;
    if (name === "storage_cache_size") return this.storageCache.get(this.cacheKey(this.readString(a[0]), a[1]))?.length ?? -1;
    if (name === "storage_cache_read") {
      const bytes = this.storageCache.get(this.cacheKey(this.readString(a[0]), a[1]));
      if (!bytes || bytes.length > Number(a[3])) return -1;
      return this.writeArray(a[2], bytes, "u8", Number(a[3]));
    }
    if (name === "storage_cache_write") {
      this.storageCache.set(this.cacheKey(this.readString(a[0]), a[1]), Uint8Array.from(this.readBytes(a[2], Number(a[3]))));
      return 1;
    }
    if (name === "storage_cache_adopt") {
      const bytes = this.storedBytes(this.readString(a[2]));
      if (!bytes) return 0;
      this.storageCache.set(this.cacheKey(this.readString(a[0]), a[1]), Uint8Array.from(bytes));
      return 1;
    }
    if (name === "storage_cache_remove") {
      const prefix = `${this.readString(a[0])}:`;
      for (const key of this.storageCache.keys()) if (key.startsWith(prefix)) this.storageCache.delete(key);
      return 1;
    }
    if (name === "storage_db_open" || name === "storage_db_prepare") return this.allocStruct(8);
    if (name === "storage_db_exec") { this.storageError = "structured storage is unavailable in the browser"; return 0; }
    if (name === "storage_stmt_step") return -1;
    if (name === "storage_stmt_column_i64") return 0n;
    if (name === "storage_stmt_column_blob_size" || name === "storage_stmt_column_blob") return 0;
    if (name.startsWith("storage_stmt_bind_")) return 0;
    if (name === "storage_stmt_reset" || name === "storage_stmt_clear_bindings") return 0;
    if (name === "storage_db_close" || name === "storage_stmt_finalize") return;
    throw new Error(`browser storage backend does not implement ${name}`);
  }

  compress(name, a) {
    if (name.endsWith("_bound")) return Number(a[0]) + 64;
    if (name === "compress_gzip_decoded_size") {
      const bytes = this.readBytes(a[0], Number(a[1]));
      if (bytes.length < 4) return -3;
      return new DataView(bytes.buffer, bytes.byteOffset + bytes.length - 4, 4).getUint32(0, true);
    }
    if (name === "compress_gzip_inflate") {
      const encoded = this.readBytes(a[0], Number(a[1]));
      const found = this.predecodedGzip.find(item => item.encoded.length === encoded.length &&
        item.encoded.every((byte, index) => byte === encoded[index]));
      if (!found || found.decoded.length > Number(a[3])) return -3;
      return this.writeArray(a[2], found.decoded, "u8", Number(a[3]));
    }
    if (name === "compress_zstd_decoded_size") return -5;
    if (name.endsWith("_decode") || name.endsWith("_encode") || name.endsWith("_deflate")) return -5;
    return -5;
  }

  io(name, a) {
    const handle = value => this.nextIOHandle++ && this.nextIOHandle - 1;
    if (name === "io_save_image") return 0;
    if (name === "io_glb_read") {
      const path = this.readString(a[0]), glb = this.parseGLB(this.storedBytes(path) || new Uint8Array());
      if (!glb) return 0n;
      glb.path = path; glb.image = this.decodedImages.get(path) || null;
      const id = handle(glb); this.glbs.set(id, glb); return BigInt(id);
    }
    if (name === "io_glb_valid") return this.glbs.has(Number(a[0])) ? 1 : 0;
    if (name === "io_glb_json_size") return this.glbs.get(Number(a[0]))?.jsonBytes.length || 0;
    if (name === "io_glb_data_size") return this.glbs.get(Number(a[0]))?.data.length || 0;
    if (name === "io_glb_copy_json" || name === "io_glb_copy_data") {
      const glb = this.glbs.get(Number(a[0]));
      const bytes = name.endsWith("json") ? glb?.jsonBytes : glb?.data;
      return bytes ? this.writeArray(a[1], bytes, "u8", Number(a[2])) : 0;
    }
    if (name === "io_glb_destroy") { this.glbs.delete(Number(a[0])); return; }
    if (name === "io_glb_mesh_read") {
      const glb = this.glbs.get(Number(a[0]));
      const primitive = glb?.json.meshes?.[Number(a[1])]?.primitives?.[Number(a[2])];
      if (!glb || !primitive) return 0n;
      const id = handle(primitive); this.meshes.set(id, { glb, primitive }); return BigInt(id);
    }
    const mesh = this.meshes.get(Number(a[0]));
    if (name === "io_glb_mesh_valid") return mesh ? 1 : 0;
    if (name === "io_glb_mesh_vertex_count") return mesh ? this.glbAccessor(mesh.glb, mesh.primitive.attributes.POSITION).length / 3 : 0;
    if (name === "io_glb_mesh_index_count") return mesh ? this.glbAccessor(mesh.glb, mesh.primitive.indices).length : 0;
    if (name === "io_glb_mesh_image_width") return mesh?.glb.image?.width || 0;
    if (name === "io_glb_mesh_image_height") return mesh?.glb.image?.height || 0;
    const copies = {
      io_glb_mesh_copy_positions: ["POSITION", "f32"], io_glb_mesh_copy_normals: ["NORMAL", "f32"],
      io_glb_mesh_copy_texcoords: ["TEXCOORD_0", "f32"], io_glb_mesh_copy_joints: ["JOINTS_0", "i32"],
      io_glb_mesh_copy_weights: ["WEIGHTS_0", "f32"],
    };
    if (copies[name]) {
      if (!mesh) return 0;
      const [attribute, kind] = copies[name];
      const values = this.glbAccessor(mesh.glb, mesh.primitive.attributes[attribute]);
      const typed = kind === "f32" ? Float32Array.from(values) : Int32Array.from(values);
      return this.writeArray(a[1], typed, kind, Number(a[2]));
    }
    if (name === "io_glb_mesh_copy_indices") {
      const values = mesh ? Uint32Array.from(this.glbAccessor(mesh.glb, mesh.primitive.indices)) : new Uint32Array();
      return this.writeArray(a[1], values, "u32", Number(a[2]));
    }
    if (name === "io_glb_mesh_copy_image") {
      const values = mesh?.glb.image?.rgba || new Uint8Array();
      return this.writeArray(a[1], values, "u8", Number(a[2]));
    }
    if (name === "io_glb_mesh_destroy") { this.meshes.delete(Number(a[0])); return; }
    throw new Error(`browser io backend does not implement ${name}`);
  }

  std(name, a) {
    const unaryMath = { sin: Math.sin, cos: Math.cos, tan: Math.tan, asin: Math.asin,
      acos: Math.acos, atan: Math.atan, sqrt: Math.sqrt, floor: Math.floor,
      ceil: Math.ceil, round: Math.round, exp: Math.exp, log: Math.log };
    if (name === "print") { console.log(this.readString(a[0]).replace(/\n$/, "")); return; }
    if (name === "open") {
      const path = this.readString(a[0]), mode = this.readString(a[1]);
      const current = this.storedFile(path);
      if (mode.includes("r") && current === null) return 0n;
      const descriptor = this.nextFileDescriptor++;
      this.fileDescriptors.set(descriptor, {
        path, mode, text: mode.includes("a") && current !== null ? current : current || "",
      });
      return BigInt(descriptor);
    }
    if (name === "read") {
      const file = this.fileDescriptors.get(Number(a[0]));
      return this.writeString(file?.text || "");
    }
    if (name === "write") {
      const file = this.fileDescriptors.get(Number(a[0]));
      if (!file) return 0n;
      const text = this.readString(a[1]);
      file.text = file.mode.includes("a") ? file.text + text : text;
      return BigInt(textEncoder.encode(text).length);
    }
    if (name === "close") {
      const descriptor = Number(a[0]), file = this.fileDescriptors.get(descriptor);
      if (file?.mode && !file.mode.includes("r")) this.storeFile(file.path, file.text);
      this.fileDescriptors.delete(descriptor);
      return;
    }
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

  os(name, a) {
    const string = value => this.writeString(value);
    if (name === "os_platform") return 0;
    if (name === "os_time") return (globalThis.performance?.now?.() || Date.now()) / 1000;
    if (name === "os_time_ms") return BigInt(Date.now());
    if (name === "os_date_now") {
      const date = new Date(), pointer = this.allocStruct(40), view = this.view();
      const values = [date.getFullYear(), date.getMonth() + 1, date.getDate(), date.getHours(), date.getMinutes(),
        date.getSeconds(), date.getMilliseconds(), 0, 0, -date.getTimezoneOffset()];
      values.forEach((value, index) => view.setInt32(pointer + index * 4, value, true));
      return pointer;
    }
    if (name === "os_cwd") return string("/");
    if (name === "os_env") {
      const key = this.readString(a[0]);
      return string(key === "HOME" || key === "USERPROFILE" ? "/home/web" : "");
    }
    if (name === "os_make_dirs") return 1;
    if (name === "os_file_size") {
      const value = this.storedBytes(this.readString(a[0]));
      return value === null ? -1n : BigInt(value.length);
    }
    if (name === "os_read_file") return string(this.storedFile(this.readString(a[0])) || "");
    if (name === "os_read_file_bytes") {
      const bytes = this.storedBytes(this.readString(a[0]));
      return bytes === null ? -1n : BigInt(this.writeArray(a[1], bytes, "u8", Number(a[2])));
    }
    if (name === "os_write_file_atomic") {
      this.storeFile(this.readString(a[0]), this.readString(a[1]));
      return 1;
    }
    if (name === "os_write_file_bytes_atomic") {
      this.storeBytes(this.readString(a[0]), this.readBytes(a[1], Number(a[2])));
      return 1;
    }
    if (name === "os_dir_scan") {
      const root = this.readString(a[0]).replace(/\/+$/, "");
      const prefix = `${root}/`;
      this.scanEntries = [...this.virtualFiles.keys()]
        .filter(path => path.startsWith(prefix) && !path.slice(prefix.length).includes("/"))
        .sort()
        .map(path => ({ name: path.slice(prefix.length), path, depth: 0, parent: -1, directory: false }));
      return this.scanEntries.length;
    }
    if (name === "os_entry_name") return string(this.scanEntries[Number(a[0])]?.name || "");
    if (name === "os_entry_path") return string(this.scanEntries[Number(a[0])]?.path || "");
    if (name === "os_entry_depth") return this.scanEntries[Number(a[0])]?.depth ?? 0;
    if (name === "os_entry_parent") return this.scanEntries[Number(a[0])]?.parent ?? -1;
    if (name === "os_entry_is_dir") return this.scanEntries[Number(a[0])]?.directory ? 1 : 0;
    if (name === "os_watch_start") return 1;
    if (name === "os_watch_poll") return 0;
    if (name === "os_watch_stop") return;
    if (name === "os_open_folder_dialog") return string("");
    if (name === "os_launch_ns_project") return 0;
    throw new Error(`browser OS backend does not implement ${name}`);
  }

  uiColor(value) {
    const color = Number(value) >>> 0;
    const r = color & 255, g = (color >>> 8) & 255, b = (color >>> 16) & 255;
    return `rgba(${r},${g},${b},${((color >>> 24) & 255) / 255})`;
  }

  uiRenderer(pointer) { return this.uiRenderers.get(Number(pointer)); }

  // Clip a command is recorded under, the way lib/src/ui.c stores one clip per
  // draw command rather than replaying a stack: the innermost pushed rectangle,
  // already intersected with everything below it.
  uiClip(renderer) { return renderer.clips[renderer.clips.length - 1]; }

  uiCommand(pointer, command) {
    const renderer = this.uiRenderer(pointer);
    if (!renderer) return;
    const clip = command.clip || this.uiClip(renderer);
    if (!clip || clip.w <= 0 || clip.h <= 0) return;
    command.clip = clip;
    renderer.commands.push(command);
  }

  uiFontMetrics(fontType) {
    return Number(fontType) === 1 ? UI_FONT_MONO_METRICS : UI_FONT_MAIN_METRICS;
  }

  uiFont(px, fontType = 1) {
    return `${Math.max(1, Number(px))}px ${this.uiFontMetrics(fontType).stack}`;
  }

  // ui_text_width measures one line: it stops at the first newline.
  uiTextWidth(text, px, fontType) {
    const line = String(text).split("\n")[0];
    if (!line) return 0;
    const context = this.uiContext;
    // ui_missing_glyph_advance, the width a native renderer uses for a glyph
    // with no atlas entry, is the honest fallback with no context to measure in.
    if (!context) return [...line].length * Math.max(0, Number(px)) * 0.55;
    context.font = this.uiFont(px, fontType);
    return context.measureText(line).width;
  }

  // Per-code-point advances of one line, with the UTF-8 byte offset each glyph
  // starts at. Caret offsets at the ns surface are byte offsets so that they
  // compose with substr.
  uiGlyphAdvances(text, px, fontType) {
    const line = String(text).split("\n")[0];
    const glyphs = [];
    let prefix = "", offset = 0, width = 0;
    for (const glyph of line) {
      const next = this.uiTextWidth(prefix + glyph, px, fontType);
      glyphs.push({ offset, advance: next - width });
      offset += textEncoder.encode(glyph).length;
      prefix += glyph;
      width = next;
    }
    return { glyphs, end: offset, width };
  }

  uiTextIndexAtX(text, px, fontType, x) {
    if (!text || px <= 0 || x <= 0) return 0;
    const { glyphs, end } = this.uiGlyphAdvances(text, px, fontType);
    let cursor = 0;
    for (const glyph of glyphs) {
      if (x < cursor + glyph.advance * 0.5) return glyph.offset;
      cursor += glyph.advance;
    }
    return end;
  }

  // Width of the first `end` UTF-8 bytes of a single line.
  uiPrefixWidth(text, end, px, fontType) {
    if (!text || end <= 0 || px <= 0) return 0;
    const line = String(text).split("\n")[0];
    const bytes = textEncoder.encode(line);
    if (end >= bytes.length) return this.uiTextWidth(line, px, fontType);
    return this.uiTextWidth(textDecoder.decode(bytes.subarray(0, end)), px, fontType);
  }

  uiLineHeight(px, fontType) { return Math.max(0, Number(px)) * this.uiFontMetrics(fontType).line; }

  uiPathRoundRect(context, x, y, w, h, radii) {
    const limit = Math.min(Math.abs(w), Math.abs(h)) / 2;
    const clamp = value => Math.max(0, Math.min(Number(value) || 0, limit));
    context.beginPath();
    if (context.roundRect) {
      context.roundRect(x, y, w, h, Array.isArray(radii)
        ? radii.map(clamp)
        : clamp(radii));
      return;
    }
    context.rect(x, y, w, h);
  }

  executeUICommand(context, command, baseClip) {
    const c = command, color = c.color === undefined ? null : this.uiColor(c.color);
    // A command clipped to the whole drawable needs no canvas clip of its own.
    const guarded = c.clip && c.clip !== baseClip;
    if (guarded) {
      context.save();
      context.beginPath();
      context.rect(c.clip.x, c.clip.y, c.clip.w, c.clip.h);
      context.clip();
    }
    this.paintUICommand(context, c, color);
    if (guarded) context.restore();
  }

  paintUICommand(context, c, color) {
    if (c.kind === "fillRect") {
      context.fillStyle = color; context.fillRect(c.x, c.y, c.w, c.h); return;
    }
    if (c.kind === "fillRoundRect") {
      context.fillStyle = color; this.uiPathRoundRect(context, c.x, c.y, c.w, c.h, c.radius); context.fill(); return;
    }
    if (c.kind === "strokeRect") {
      context.strokeStyle = color; context.lineWidth = c.thickness;
      context.strokeRect(c.x, c.y, c.w, c.h); return;
    }
    if (c.kind === "strokeRoundRect") {
      context.strokeStyle = color; context.lineWidth = c.thickness;
      this.uiPathRoundRect(context, c.x, c.y, c.w, c.h, c.radius); context.stroke(); return;
    }
    if (c.kind === "fillCircle") {
      context.fillStyle = color; context.beginPath();
      context.arc(c.cx, c.cy, Math.max(0, c.radius), 0, Math.PI * 2); context.fill(); return;
    }
    if (c.kind === "strokeCircle") {
      context.strokeStyle = color; context.lineWidth = c.thickness; context.beginPath();
      context.arc(c.cx, c.cy, Math.max(0, c.radius), 0, Math.PI * 2); context.stroke(); return;
    }
    if (c.kind === "arc") {
      context.strokeStyle = color; context.lineWidth = Math.max(0, c.thickness);
      context.lineCap = "round"; context.beginPath();
      context.arc(c.cx, c.cy, Math.max(0, c.radius), c.start, c.end); context.stroke(); return;
    }
    if (c.kind === "strokeLine") {
      context.strokeStyle = color; context.lineWidth = c.thickness; context.lineCap = "round";
      context.beginPath(); context.moveTo(c.x0, c.y0); context.lineTo(c.x1, c.y1); context.stroke(); return;
    }
    if (c.kind === "polyline") {
      if (c.points.length < 4) return;
      context.strokeStyle = color; context.lineWidth = c.thickness;
      context.lineCap = "round"; context.lineJoin = "round";
      context.beginPath();
      context.moveTo(c.points[0], c.points[1]);
      for (let i = 2; i + 1 < c.points.length; i += 2) context.lineTo(c.points[i], c.points[i + 1]);
      context.stroke(); return;
    }
    if (c.kind === "triangle") {
      context.beginPath();
      context.moveTo(c.x0, c.y0); context.lineTo(c.x1, c.y1); context.lineTo(c.x2, c.y2);
      context.closePath();
      if (c.colors) {
        // One flat fill per vertex colour is the closest a 2D context gets to
        // the GPU's interpolated triangle; the average keeps the primitive's
        // overall tone right.
        context.fillStyle = this.uiColor(this.uiAverageColor(c.colors));
      } else {
        context.fillStyle = color;
      }
      context.fill(); return;
    }
    if (c.kind === "text") {
      context.fillStyle = color;
      context.font = this.uiFont(c.px, c.fontType);
      context.textBaseline = "top";
      context.fillText(c.text, c.x, c.y); return;
    }
    if (c.kind === "textArc") {
      this.paintUITextArc(context, c, color); return;
    }
    if (c.kind === "image") {
      const atlas = this.uiAtlases.get(Number(c.atlas));
      const image = atlas?.image;
      if (!image?.complete || !image.naturalWidth) return;
      const sw = c.sw > 0 ? c.sw : image.naturalWidth, sh = c.sh > 0 ? c.sh : image.naturalHeight;
      context.globalAlpha = ((Number(c.color) >>> 24) & 255) / 255;
      context.drawImage(image, c.sx, c.sy, sw, sh, c.x, c.y, c.w, c.h);
      context.globalAlpha = 1;
    }
  }

  uiAverageColor(colors) {
    let r = 0, g = 0, b = 0, a = 0;
    for (const value of colors) {
      const color = Number(value) >>> 0;
      r += color & 255; g += (color >>> 8) & 255; b += (color >>> 16) & 255; a += (color >>> 24) & 255;
    }
    const mean = value => Math.round(value / colors.length) & 255;
    return (mean(r) | (mean(g) << 8) | (mean(b) << 16) | (mean(a) << 24)) >>> 0;
  }

  // ui_draw_text_arc centers the run on a circular baseline: a glyph sits at
  // its own angle around the circle and rotates to the local tangent, and the
  // cap band is what the baseline runs through.
  paintUITextArc(context, c, color) {
    if (c.radius <= 0 || c.px <= 0) return;
    const capCenter = this.uiFontMetrics(c.fontType).capCenter * c.px;
    const { glyphs, width } = this.uiGlyphAdvances(c.text, c.px, c.fontType);
    const characters = [...String(c.text).split("\n")[0]];
    context.fillStyle = color;
    context.font = this.uiFont(c.px, c.fontType);
    context.textBaseline = "top";
    let cursor = -width * 0.5;
    characters.forEach((glyph, index) => {
      const advance = glyphs[index]?.advance || 0;
      const angle = c.centerAngle + (cursor + advance * 0.5) / c.radius;
      context.save();
      context.translate(c.cx + Math.cos(angle) * c.radius, c.cy + Math.sin(angle) * c.radius);
      context.rotate(angle + Math.PI * 0.5);
      context.fillText(glyph, -advance * 0.5, -capCenter);
      context.restore();
      cursor += advance;
    });
  }

  // Effective insets of a renderer: the display safe area unless the
  // application overrode or disabled it. Mirrors ui_resolve_safe_area.
  uiInsets(pointer) {
    const renderer = this.uiRenderer(pointer);
    const zero = { top: 0, right: 0, bottom: 0, left: 0 };
    if (!renderer || !renderer.safeAreaEnabled) return zero;
    const source = renderer.insets || this.uiViewInsets(renderer);
    const inset = value => Math.max(0, Number(value) || 0);
    const [width, height] = this.uiSurfaceSize(pointer);
    const top = inset(source.top), right = inset(source.right);
    const bottom = inset(source.bottom), left = inset(source.left);
    return {
      top: top + bottom < height ? top : 0,
      bottom: top + bottom < height ? bottom : 0,
      left: left + right < width ? left : 0,
      right: left + right < width ? right : 0,
    };
  }

  // Insets the renderer's view reports, the way ui_sync_view_metrics reads them.
  uiViewInsets(renderer) {
    const pointer = Number(renderer?.view || 0);
    if (!pointer || !this.views.has(pointer) || !this.memory) return this.safeAreaInsets();
    const view = this.view();
    return {
      top: view.getFloat64(pointer + VIEW.safeAreaTop, true),
      right: view.getFloat64(pointer + VIEW.safeAreaRight, true),
      bottom: view.getFloat64(pointer + VIEW.safeAreaBottom, true),
      left: view.getFloat64(pointer + VIEW.safeAreaLeft, true),
    };
  }

  // The whole drawable in logical points. ui_resize_to overrides it the way a
  // native renderer detaches r->rect from the view metrics.
  uiSurfaceSize(pointer) {
    const renderer = pointer === undefined ? null : this.uiRenderer(pointer);
    if (renderer?.size) return renderer.size;
    const dpr = globalThis.window?.devicePixelRatio || 1;
    return [
      Math.max(1, Math.round(this.canvas.clientWidth || this.canvas.width / dpr || 1)),
      Math.max(1, Math.round(this.canvas.clientHeight || this.canvas.height / dpr || 1)),
    ];
  }

  // The drawable in drawing coordinates: the insets sit at negative
  // coordinates because the origin is the top-left of the safe rect.
  uiSurfaceClip(pointer) {
    const insets = this.uiInsets(pointer), [width, height] = this.uiSurfaceSize(pointer);
    // `0 - 0` is negative zero, which reaches ns code as a distinguishable value.
    return { x: insets.left ? -insets.left : 0, y: insets.top ? -insets.top : 0, w: width, h: height };
  }

  uiWidgets(pointer) { return this.uiWidgetLayers.get(Number(pointer)); }

  // lib/src/ui.c returns a pointer to a static for these results, so the value
  // is only good until the next call. Reuse one slot per entry point rather
  // than growing the bump heap by a struct on every widget of every frame.
  uiStatic(key, size) {
    let pointer = this.uiStatics.get(key);
    if (!pointer) {
      pointer = this.allocStruct(size);
      this.uiStatics.set(key, pointer);
    } else {
      new Uint8Array(this.memory.buffer, pointer, size).fill(0);
    }
    return pointer;
  }

  writeRect(x, y, w, h) {
    const pointer = this.allocStruct(UI_RECT.size), view = this.view();
    view.setFloat64(pointer + UI_RECT.x, x, true);
    view.setFloat64(pointer + UI_RECT.y, y, true);
    view.setFloat64(pointer + UI_RECT.w, w, true);
    view.setFloat64(pointer + UI_RECT.h, h, true);
    return pointer;
  }

  readRect(pointer) {
    if (!pointer || !this.memory) return { x: 0, y: 0, w: 0, h: 0 };
    const view = this.view();
    return {
      x: view.getFloat64(Number(pointer) + UI_RECT.x, true),
      y: view.getFloat64(Number(pointer) + UI_RECT.y, true),
      w: view.getFloat64(Number(pointer) + UI_RECT.w, true),
      h: view.getFloat64(Number(pointer) + UI_RECT.h, true),
    };
  }

  writeColor(color) {
    const pointer = this.uiStatic("color", UI_RECT.size), view = this.view();
    view.setFloat64(pointer, color.r, true);
    view.setFloat64(pointer + 8, color.g, true);
    view.setFloat64(pointer + 16, color.b, true);
    view.setFloat64(pointer + 24, color.a, true);
    return pointer;
  }

  readColor(pointer, fallback = { r: 1, g: 1, b: 1, a: 1 }) {
    if (!pointer || !this.memory) return fallback;
    const view = this.view(), base = Number(pointer);
    return {
      r: view.getFloat64(base, true), g: view.getFloat64(base + 8, true),
      b: view.getFloat64(base + 16, true), a: view.getFloat64(base + 24, true),
    };
  }

  // lib/ui.ns::ui_input, the host-supplied snapshot a widget frame runs on.
  readUIInput(pointer) {
    const view = this.view(), base = Number(pointer);
    const f64 = offset => view.getFloat64(base + offset, true);
    const flag = offset => view.getInt32(base + offset, true) !== 0;
    return {
      mouseX: f64(UI_INPUT.mouseX), mouseY: f64(UI_INPUT.mouseY),
      mouseDown: flag(UI_INPUT.mouseDown), mousePressed: flag(UI_INPUT.mousePressed),
      mouseReleased: flag(UI_INPUT.mouseReleased), mouseMiddleDown: flag(UI_INPUT.mouseMiddleDown),
      mouseRightPressed: flag(UI_INPUT.mouseRightPressed), mouseRightDown: flag(UI_INPUT.mouseRightDown),
      panDx: f64(UI_INPUT.panDx), panDy: f64(UI_INPUT.panDy),
      zoomFactor: f64(UI_INPUT.zoomFactor), wheelY: f64(UI_INPUT.wheelY),
      keyEnter: flag(UI_INPUT.keyEnter), keyEscape: flag(UI_INPUT.keyEscape),
      shift: flag(UI_INPUT.shift), ctrl: flag(UI_INPUT.ctrl),
      meta: flag(UI_INPUT.meta), alt: flag(UI_INPUT.alt),
      gizmoManipulating: flag(UI_INPUT.gizmoManipulating),
    };
  }

  emptyUIInput() {
    return {
      mouseX: 0, mouseY: 0, mouseDown: false, mousePressed: false, mouseReleased: false,
      mouseMiddleDown: false, mouseRightPressed: false, mouseRightDown: false,
      panDx: 0, panDy: 0, zoomFactor: 1, wheelY: 0, keyEnter: false, keyEscape: false,
      shift: false, ctrl: false, meta: false, alt: false, gizmoManipulating: false,
    };
  }

  // A widget id is a wasm string from the application, or a JS string the
  // integer-keyed entry points build for themselves. Neither reaches the bump
  // heap: the id only ever feeds the hash.
  uiWidgetId(value) { return typeof value === "string" ? value : this.readString(value); }

  // FNV-1a over the widget id, the hash ui.c keys its active widget on.
  uiWidgetHash(text) {
    let hash = 2166136261;
    for (const byte of textEncoder.encode(String(text))) {
      hash = Math.imul(hash ^ byte, 16777619) >>> 0;
    }
    return hash || 1;
  }

  uiWidgetHover(widgets, x, y, width, height) {
    const input = widgets?.input;
    return !!input && input.mouseX >= x && input.mouseY >= y &&
      input.mouseX < x + width && input.mouseY < y + height;
  }

  ui(name, a) {
    const n = value => Number(value);
    const clamp = (value, low, high) => Math.max(low, Math.min(high, value));

    // ── renderer lifecycle ────────────────────────────────────────────────
    if (name === "ui_renderer_create") {
      const pointer = this.allocStruct(UI_HANDLE_SIZE);
      this.uiRenderers.set(pointer, {
        view: n(a[0]), commands: [], clips: [], size: null,
        insets: null, safeAreaEnabled: true, baseClip: null,
      });
      // ui_renderer_create requests the device itself natively, so a view+ui
      // application never imports gpu; publish the canvas as that device.
      if (this.views.has(n(a[0]))) this.syncView(a[0]);
      this.ui("ui_begin_frame", [pointer]);
      return pointer;
    }
    if (name === "ui_renderer_destroy") { this.uiRenderers.delete(n(a[0])); return; }
    if (name === "ui_resize") { const r = this.uiRenderer(a[0]); if (r) r.size = null; this.resizeCanvas(); return; }
    if (name === "ui_resize_to") {
      const renderer = this.uiRenderer(a[0]);
      if (renderer) renderer.size = [Math.max(1, n(a[1])), Math.max(1, n(a[2]))];
      return;
    }
    // The browser shell drives every frame through requestAnimationFrame, so a
    // redraw burst is already scheduled; forward it for a view that throttles.
    if (name === "ui_request_render") {
      this.viewImport("view_request_frame", [this.uiRenderer(a[0])?.view || 0, a[1]]); return;
    }
    if (name === "ui_request_render_after") {
      this.viewImport("view_request_frame_after", [this.uiRenderer(a[0])?.view || 0, a[1]]); return;
    }
    if (name === "ui_begin_frame") {
      const renderer = this.uiRenderer(a[0]);
      if (!renderer) return;
      renderer.commands.length = 0;
      renderer.baseClip = this.uiSurfaceClip(a[0]);
      renderer.clips = [renderer.baseClip];
      return;
    }
    if (name === "ui_flush" || name === "ui_flush_overlay") {
      const renderer = this.uiRenderer(a[0]), context = this.uiContext;
      if (!renderer) return;
      if (!context) { renderer.commands.length = 0; return; }
      const [pixelWidth, pixelHeight] = this.resizeCanvas();
      const dpr = globalThis.window?.devicePixelRatio || 1;
      const width = pixelWidth / dpr, height = pixelHeight / dpr;
      context.save();
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      const clear = n(a[1]);
      const channel = offset => Math.round(clamp(this.view().getFloat64(clear + offset, true), 0, 1) * 255);
      const alpha = clear ? clamp(this.view().getFloat64(clear + 24, true), 0, 1) : 1;
      // The clear replaces the target rather than painting over it: a
      // transparent clear is what leaves a `gpu` scene visible under a ui
      // overlay, and painting `rgba(...,0)` over the last frame would leave
      // that frame exactly where it was.
      context.clearRect(0, 0, width, height);
      if (alpha > 0) {
        context.fillStyle = clear ? `rgba(${channel(0)},${channel(8)},${channel(16)},${alpha})` : "rgba(18,20,23,1)";
        context.fillRect(0, 0, width, height);
      }
      // Commands are recorded in content space, whose origin is the top-left of
      // the safe rect. Nothing is clipped to the safe area on its own: a
      // background laid out against the surface rect reaches under the chrome.
      const insets = this.uiInsets(a[0]);
      context.translate(insets.left, insets.top);
      for (const command of renderer.commands) this.executeUICommand(context, command, renderer.baseClip);
      context.restore();
      renderer.commands.length = 0;
      return;
    }

    // ── font faces ────────────────────────────────────────────────────────
    // The browser draws text with its own font stack and has no filesystem to
    // read an atlas from, so a face load reports the same failure a native
    // renderer reports for a missing atlas file, and text keeps the fallback
    // face ui_primary_font selects.
    if (name === "ui_load_font" || name === "ui_load_chinese_font" ||
        name === "ui_load_bitmap_font" || name === "ui_load_bitmap_chinese_font" ||
        name === "ui_load_builtin_bitmap_font") return 0;

    // ── canvas, safe area and layout ──────────────────────────────────────
    if (name === "ui_canvas_width") {
      const insets = this.uiInsets(a[0]);
      return Math.max(1, Math.round(this.uiSurfaceSize(a[0])[0] - insets.left - insets.right));
    }
    if (name === "ui_canvas_height") {
      const insets = this.uiInsets(a[0]);
      return Math.max(1, Math.round(this.uiSurfaceSize(a[0])[1] - insets.top - insets.bottom));
    }
    if (name === "ui_surface_width") return this.uiSurfaceSize(a[0])[0];
    if (name === "ui_surface_height") return this.uiSurfaceSize(a[0])[1];
    if (name === "ui_safe_rect") {
      return this.writeRect(0, 0, this.ui("ui_canvas_width", a), this.ui("ui_canvas_height", a));
    }
    if (name === "ui_surface_rect") {
      const clip = this.uiSurfaceClip(a[0]);
      return this.writeRect(clip.x, clip.y, clip.w, clip.h);
    }
    if (name === "ui_safe_area") {
      const insets = this.uiInsets(a[0]), pointer = this.allocStruct(UI_INSETS.size), view = this.view();
      view.setFloat64(pointer + UI_INSETS.top, insets.top, true);
      view.setFloat64(pointer + UI_INSETS.right, insets.right, true);
      view.setFloat64(pointer + UI_INSETS.bottom, insets.bottom, true);
      view.setFloat64(pointer + UI_INSETS.left, insets.left, true);
      return pointer;
    }
    if (name === "ui_safe_area_enabled") return this.uiRenderer(a[0])?.safeAreaEnabled ? 1 : 0;
    if (name === "ui_set_safe_area_enabled") {
      const renderer = this.uiRenderer(a[0]);
      if (renderer) renderer.safeAreaEnabled = !!n(a[1]);
      return;
    }
    if (name === "ui_set_safe_area_insets") {
      const renderer = this.uiRenderer(a[0]);
      if (renderer) renderer.insets = { top: n(a[1]), right: n(a[2]), bottom: n(a[3]), left: n(a[4]) };
      return;
    }
    if (name === "ui_reset_safe_area_insets") {
      const renderer = this.uiRenderer(a[0]);
      if (renderer) renderer.insets = null;
      return;
    }
    if (name === "ui_content_x") return n(a[1]) - this.uiInsets(a[0]).left;
    if (name === "ui_content_y") return n(a[1]) - this.uiInsets(a[0]).top;
    if (name === "ui_surface_x") return n(a[1]) + this.uiInsets(a[0]).left;
    if (name === "ui_surface_y") return n(a[1]) + this.uiInsets(a[0]).top;
    if (name === "ui_layout") {
      const x = n(a[0]), y = n(a[1]), w = n(a[2]), h = n(a[3]);
      const childW = n(a[4]), childH = n(a[5]), align = n(a[6]);
      const left = align & 16 ? x + (w - childW) * 0.5 : align & 2 ? x + w - childW : x;
      const top = align & 32 ? y + (h - childH) * 0.5 : align & 8 ? y + h - childH : y;
      return this.writeRect(left, top, childW, childH);
    }

    // ── image atlases ─────────────────────────────────────────────────────
    if (name === "ui_atlas_load") {
      if (typeof globalThis.Image !== "function") return 0;
      const path = this.readString(a[1]);
      if (!path) return 0;
      const image = new Image();
      image.src = path;
      // Texture ids 0..2 are the white and font textures a native renderer
      // reserves; application atlases start at 3.
      const id = this.nextUIAtlas++;
      this.uiAtlases.set(id, { image });
      return id;
    }
    if (name === "ui_atlas_destroy") { this.uiAtlases.delete(n(a[1])); return; }
    if (name === "ui_atlas_width") return this.uiAtlases.get(n(a[1]))?.image?.naturalWidth || 0;
    if (name === "ui_atlas_height") return this.uiAtlases.get(n(a[1]))?.image?.naturalHeight || 0;
    if (name === "ui_atlas_draw") {
      const atlas = n(a[1]);
      return this.ui("ui_atlas_draw_region", [a[0], atlas, a[2], a[3], a[4], a[5], 0, 0,
        this.ui("ui_atlas_width", [a[0], atlas]), this.ui("ui_atlas_height", [a[0], atlas]), 0xffffffff]);
    }
    if (name === "ui_atlas_draw_region") {
      if (n(a[4]) <= 0 || n(a[5]) <= 0) return;
      this.uiCommand(a[0], { kind: "image", atlas: n(a[1]), x: n(a[2]), y: n(a[3]), w: n(a[4]), h: n(a[5]),
        sx: n(a[6]), sy: n(a[7]), sw: n(a[8]), sh: n(a[9]), color: a[10] });
      return;
    }

    // ── shapes ────────────────────────────────────────────────────────────
    if (name === "ui_fill_rect") {
      if (n(a[3]) <= 0 || n(a[4]) <= 0) return;
      this.uiCommand(a[0], { kind: "fillRect", x: n(a[1]), y: n(a[2]), w: n(a[3]), h: n(a[4]), color: a[5] }); return;
    }
    if (name === "ui_fill_gradient_rect") {
      const x = n(a[1]), y = n(a[2]), w = n(a[3]), h = n(a[4]);
      if (w <= 0 || h <= 0) return;
      this.uiCommand(a[0], { kind: "triangle", x0: x, y0: y, x1: x + w, y1: y,
        x2: x + w, y2: y + h, colors: [a[5], a[6], a[7]] });
      this.uiCommand(a[0], { kind: "triangle", x0: x, y0: y, x1: x + w, y1: y + h,
        x2: x, y2: y + h, colors: [a[5], a[7], a[8]] });
      return;
    }
    if (name === "ui_fill_round_rect") {
      this.uiCommand(a[0], { kind: "fillRoundRect", x: n(a[1]), y: n(a[2]), w: n(a[3]), h: n(a[4]), radius: n(a[5]), color: a[6] }); return;
    }
    if (name === "ui_fill_round_rect_per_corner") {
      this.uiCommand(a[0], { kind: "fillRoundRect", x: n(a[1]), y: n(a[2]), w: n(a[3]), h: n(a[4]),
        radius: [n(a[5]), n(a[6]), n(a[8]), n(a[7])], color: a[9] }); return;
    }
    if (name === "ui_stroke_rect") {
      this.uiCommand(a[0], { kind: "strokeRect", x: n(a[1]), y: n(a[2]), w: n(a[3]), h: n(a[4]), thickness: n(a[5]), color: a[6] }); return;
    }
    if (name === "ui_stroke_round_rect") {
      this.uiCommand(a[0], { kind: "strokeRoundRect", x: n(a[1]), y: n(a[2]), w: n(a[3]), h: n(a[4]),
        radius: n(a[5]), thickness: n(a[6]), color: a[7] }); return;
    }
    if (name === "ui_stroke_round_rect_per_corner") {
      this.uiCommand(a[0], { kind: "strokeRoundRect", x: n(a[1]), y: n(a[2]), w: n(a[3]), h: n(a[4]),
        radius: [n(a[5]), n(a[6]), n(a[8]), n(a[7])], thickness: n(a[9]), color: a[10] }); return;
    }
    if (name === "ui_fill_surface") {
      // Ignores the pushed clip stack the way ui_fill_surface resets it.
      const clip = this.uiSurfaceClip(a[0]);
      this.uiCommand(a[0], { kind: "fillRect", clip, x: clip.x, y: clip.y, w: clip.w, h: clip.h, color: a[1] });
      return;
    }
    if (name === "ui_fill_triangle") {
      this.uiCommand(a[0], { kind: "triangle", x0: n(a[1]), y0: n(a[2]), x1: n(a[3]), y1: n(a[4]),
        x2: n(a[5]), y2: n(a[6]), color: a[7] }); return;
    }
    if (name === "ui_fill_triangle_colors") {
      this.uiCommand(a[0], { kind: "triangle", x0: n(a[1]), y0: n(a[2]), x1: n(a[4]), y1: n(a[5]),
        x2: n(a[7]), y2: n(a[8]), colors: [a[3], a[6], a[9]] }); return;
    }
    if (name === "ui_fill_arc") {
      this.uiCommand(a[0], { kind: "arc", cx: n(a[1]), cy: n(a[2]), radius: n(a[3]), thickness: n(a[4]),
        start: n(a[5]), end: n(a[6]), color: a[7] }); return;
    }
    if (name === "ui_fill_circle") {
      this.uiCommand(a[0], { kind: "fillCircle", cx: n(a[1]), cy: n(a[2]), radius: n(a[3]), color: a[4] }); return;
    }
    if (name === "ui_stroke_circle") {
      this.uiCommand(a[0], { kind: "strokeCircle", cx: n(a[1]), cy: n(a[2]), radius: n(a[3]),
        thickness: n(a[4]), color: a[5] }); return;
    }
    if (name === "ui_stroke_line") {
      this.uiCommand(a[0], { kind: "strokeLine", x0: n(a[1]), y0: n(a[2]), x1: n(a[3]), y1: n(a[4]),
        thickness: n(a[5]), color: a[6] }); return;
    }
    if (name === "ui_stroke_polyline") {
      const points = this.readF64Array(a[1], n(a[2]) * 2);
      this.uiCommand(a[0], { kind: "polyline", points, thickness: n(a[3]), color: a[4] }); return;
    }

    // ── clipping and retained rectangle batches ───────────────────────────
    if (name === "ui_push_clip" || name === "ui_push_clip_round") {
      const renderer = this.uiRenderer(a[0]);
      if (!renderer) return;
      const current = this.uiClip(renderer);
      const x0 = Math.max(current.x, n(a[1])), y0 = Math.max(current.y, n(a[2]));
      const x1 = Math.min(current.x + current.w, n(a[1]) + n(a[3]));
      const y1 = Math.min(current.y + current.h, n(a[2]) + n(a[4]));
      renderer.clips.push({ x: x0, y: y0, w: Math.max(0, x1 - x0), h: Math.max(0, y1 - y0) });
      return;
    }
    if (name === "ui_pop_clip") {
      const renderer = this.uiRenderer(a[0]);
      if (renderer && renderer.clips.length > 1) renderer.clips.pop();
      return;
    }
    if (name === "ui_rect_clipped") {
      const renderer = this.uiRenderer(a[0]);
      if (!renderer) return 1;
      const c = this.uiClip(renderer), x = n(a[1]), y = n(a[2]), w = n(a[3]), h = n(a[4]);
      return x + w <= c.x || y + h <= c.y || x >= c.x + c.w || y >= c.y + c.h ? 1 : 0;
    }
    if (name === "ui_rect_batch_create") {
      const id = this.nextUIBatch++;
      this.uiBatches.set(id, []);
      return id;
    }
    if (name === "ui_rect_batch_destroy") { this.uiBatches.delete(n(a[1])); return; }
    if (name === "ui_rect_batch_begin") {
      const batch = this.uiBatches.get(n(a[1]));
      if (batch) batch.length = 0;
      return;
    }
    if (name === "ui_rect_batch_add") {
      const batch = this.uiBatches.get(n(a[1]));
      if (!batch || n(a[4]) <= 0 || n(a[5]) <= 0) return;
      batch.push({ kind: "fillRect", x: n(a[2]), y: n(a[3]), w: n(a[4]), h: n(a[5]), color: a[6] });
      return;
    }
    if (name === "ui_rect_batch_end") return this.uiBatches.has(n(a[1])) ? 1 : 0;
    if (name === "ui_rect_batch_draw") return this.ui("ui_rect_batch_draw_at", [a[0], a[1], 0, 0]);
    if (name === "ui_rect_batch_draw_at") {
      const dx = n(a[2]), dy = n(a[3]);
      for (const rect of this.uiBatches.get(n(a[1])) || []) {
        this.uiCommand(a[0], { ...rect, clip: null, x: rect.x + dx, y: rect.y + dy });
      }
      return;
    }

    // ── text ──────────────────────────────────────────────────────────────
    if (name === "ui_draw_text") {
      const lineHeight = this.uiLineHeight(n(a[4]), a[6]);
      String(this.readString(a[3])).split("\n").forEach((line, index) => {
        if (line) this.uiCommand(a[0], { kind: "text", x: n(a[1]), y: n(a[2]) + index * lineHeight,
          text: line, px: n(a[4]), color: a[5], fontType: n(a[6]) });
      });
      return;
    }
    if (name === "ui_draw_text_arc") {
      this.uiCommand(a[0], { kind: "textArc", cx: n(a[1]), cy: n(a[2]), radius: n(a[3]),
        centerAngle: n(a[4]), text: this.readString(a[5]), px: n(a[6]), color: a[7], fontType: n(a[8]) });
      return;
    }
    if (name === "ui_draw_text_wrapped") {
      const width = n(a[3]), px = n(a[5]);
      if (width <= 0 || px <= 0) return 0;
      const lineHeight = this.uiLineHeight(px, a[7]);
      const lines = this.uiWrapText(this.readString(a[4]), px, width, a[7]);
      lines.forEach((line, index) => {
        if (line) this.uiCommand(a[0], { kind: "text", x: n(a[1]), y: n(a[2]) + index * lineHeight,
          text: line, px, color: a[6], fontType: n(a[7]) });
      });
      return lines.length * lineHeight;
    }
    if (name === "ui_draw_text_vertical") {
      const px = n(a[4]);
      if (px <= 0) return;
      const columnWidth = this.ui("ui_text_vertical_column_width", [a[0], a[4], a[6]]);
      const stepY = this.uiLineHeight(px, a[6]);
      let columnX = n(a[1]) - columnWidth, y = n(a[2]);
      for (const glyph of this.readString(a[3])) {
        if (glyph === "\n" || glyph === "\r") { columnX -= columnWidth; y = n(a[2]); continue; }
        if (glyph === " " || glyph === "\t") continue;
        const width = this.uiTextWidth(glyph, px, a[6]);
        this.uiCommand(a[0], { kind: "text", x: columnX + (columnWidth - width) * 0.5, y,
          text: glyph, px, color: a[5], fontType: n(a[6]) });
        y += stepY;
      }
      return;
    }
    if (name === "ui_text_line_height") return this.uiLineHeight(n(a[1]), a[2]);
    if (name === "ui_text_v_center_y") {
      // The top that centers the cap band in the rect, not the line box.
      return n(a[1]) + n(a[2]) * 0.5 - this.uiFontMetrics(a[4]).capCenter * n(a[3]);
    }
    if (name === "ui_text_width") return this.uiTextWidth(this.readString(a[1]), n(a[2]), a[3]);
    if (name === "ui_text_index_at_x") {
      return this.uiTextIndexAtX(this.readString(a[1]), n(a[2]), a[3], n(a[4]));
    }
    if (name === "ui_text_prefix_width") {
      return this.uiPrefixWidth(this.readString(a[1]), n(a[2]), n(a[3]), a[4]);
    }
    if (name === "ui_measure_text") {
      const pointer = this.allocStruct(UI_TEXT_SIZE.size), view = this.view();
      view.setFloat64(pointer + UI_TEXT_SIZE.w, this.uiTextWidth(this.readString(a[1]), n(a[2]), a[3]), true);
      view.setFloat64(pointer + UI_TEXT_SIZE.h, this.uiLineHeight(n(a[2]), a[3]), true);
      return pointer;
    }
    // ui_mono_char_width always measures the mono face, whatever face is asked for.
    if (name === "ui_mono_char_width") return this.uiTextWidth("0", n(a[1]), 1);
    if (name === "ui_text_vertical_column_count") {
      const text = this.readString(a[0]);
      if (!text) return 0;
      let columns = 1, sawGlyph = false;
      for (const glyph of text) {
        if (glyph === "\n" || glyph === "\r") { if (sawGlyph) columns += 1; sawGlyph = false; }
        else if (glyph !== " " && glyph !== "\t") sawGlyph = true;
      }
      return !sawGlyph && columns > 1 ? columns - 1 : columns;
    }
    if (name === "ui_text_vertical_max_run") {
      let longest = 0, run = 0;
      for (const glyph of this.readString(a[0])) {
        if (glyph === "\n" || glyph === "\r") { longest = Math.max(longest, run); run = 0; }
        else if (glyph !== " " && glyph !== "\t") run += 1;
      }
      return Math.max(longest, run);
    }
    if (name === "ui_text_vertical_column_width") {
      const px = n(a[1]);
      return this.uiTextWidth("国", px, a[2]) || this.uiTextWidth("M", px, a[2]) || px;
    }
    if (name === "ui_text_vertical_size") {
      const pointer = this.allocStruct(UI_TEXT_SIZE.size), view = this.view();
      const columns = this.ui("ui_text_vertical_column_count", [a[1]]);
      const rows = this.ui("ui_text_vertical_max_run", [a[1]]);
      view.setFloat64(pointer + UI_TEXT_SIZE.w, this.ui("ui_text_vertical_column_width", [a[0], a[2], a[3]]) * columns, true);
      view.setFloat64(pointer + UI_TEXT_SIZE.h, this.uiLineHeight(n(a[2]), a[3]) * rows, true);
      return pointer;
    }

    // ── colours ───────────────────────────────────────────────────────────
    if (name === "ui_pack_color") {
      // ui_pack_color takes `#rrggbb`, treats a missing or short value as
      // opaque black, and always packs full alpha.
      const hex = this.readString(a[0]);
      if (hex[0] !== "#") return 0xff000000;
      const digit = index => {
        const value = parseInt(hex[index] || "", 16);
        return Number.isNaN(value) ? 0 : value;
      };
      const byte = index => (digit(index) << 4) | digit(index + 1);
      return (byte(1) | (byte(3) << 8) | (byte(5) << 16) | (255 << 24)) >>> 0;
    }
    if (name === "ui_pack_rgba_floats") {
      // The native cast truncates a clamped channel rather than rounding it.
      const byte = value => Math.trunc(clamp(n(value) * 255, 0, 255));
      return (byte(a[0]) | (byte(a[1]) << 8) | (byte(a[2]) << 16) | (byte(a[3]) << 24)) >>> 0;
    }

    // ── immediate-mode widgets ────────────────────────────────────────────
    if (name === "ui_input_empty") {
      const pointer = this.uiStatic("input", UI_INPUT.size);
      this.view().setFloat64(pointer + UI_INPUT.zoomFactor, 1, true);
      return pointer;
    }
    if (name === "ui_theme_empty") return this.uiStatic("theme", UI_HANDLE_SIZE);
    if (name === "ui_widgets_create") {
      if (!this.uiRenderer(a[0])) return 0;
      const pointer = this.allocStruct(UI_HANDLE_SIZE);
      this.uiWidgetLayers.set(pointer, {
        renderer: n(a[0]), light: false, activeId: 0, input: this.emptyUIInput(),
      });
      return pointer;
    }
    if (name === "ui_widgets_destroy") { this.uiWidgetLayers.delete(n(a[0])); return; }
    if (name === "ui_widgets_set_light") {
      const widgets = this.uiWidgets(a[0]);
      if (widgets) widgets.light = !!n(a[1]);
      return;
    }
    if (name === "ui_widgets_begin_frame") {
      const widgets = this.uiWidgets(a[0]);
      if (!widgets || !n(a[2])) return;
      const input = this.readUIInput(a[2]);
      // Pointer positions arrive in drawable space; widgets lay out in content space.
      input.mouseX = this.ui("ui_content_x", [widgets.renderer, input.mouseX]);
      input.mouseY = this.ui("ui_content_y", [widgets.renderer, input.mouseY]);
      widgets.input = input;
      return;
    }
    if (name === "ui_widgets_begin_view") {
      const widgets = this.uiWidgets(a[0]), pointer = n(a[2]);
      if (!widgets || !pointer) return;
      const view = this.view(), input = this.emptyUIInput();
      input.mouseX = this.ui("ui_content_x", [widgets.renderer, view.getFloat64(pointer + VIEW.mouseX, true)]);
      input.mouseY = this.ui("ui_content_y", [widgets.renderer, view.getFloat64(pointer + VIEW.mouseY, true)]);
      input.mouseDown = view.getInt32(pointer + VIEW.mouseDown, true) !== 0;
      input.mousePressed = view.getInt32(pointer + VIEW.mousePressed, true) !== 0;
      input.mouseReleased = view.getInt32(pointer + VIEW.mouseReleased, true) !== 0;
      input.mouseMiddleDown = view.getInt32(pointer + VIEW.middleDown, true) !== 0;
      input.mouseRightPressed = view.getInt32(pointer + VIEW.rightPressed, true) !== 0;
      input.mouseRightDown = view.getInt32(pointer + VIEW.rightDown, true) !== 0;
      input.wheelY = view.getFloat64(pointer + VIEW.scrollY, true);
      input.gizmoManipulating = !!n(a[3]);
      widgets.input = input;
      return;
    }
    if (name === "ui_widgets_end_frame") return;
    if (name === "ui_widgets_mouse_x") return this.uiWidgets(a[0])?.input.mouseX || 0;
    if (name === "ui_widgets_mouse_y") return this.uiWidgets(a[0])?.input.mouseY || 0;
    if (name === "ui_is_mouse_down") return this.uiWidgets(a[0])?.input.mouseDown ? 1 : 0;
    if (name === "ui_is_mouse_pressed") return this.uiWidgets(a[0])?.input.mousePressed ? 1 : 0;
    if (name === "ui_is_escape_pressed") return this.uiWidgets(a[0])?.input.keyEscape ? 1 : 0;
    if (name === "ui_is_enter_pressed") return this.uiWidgets(a[0])?.input.keyEnter ? 1 : 0;
    if (name === "ui_has_keyboard_focus") return 0;
    if (name === "ui_button") {
      const widgets = this.uiWidgets(a[0]);
      if (!widgets) return 0;
      const renderer = widgets.renderer;
      const x = n(a[2]), y = n(a[3]), width = n(a[4]), height = n(a[5]), active = !!n(a[7]);
      const hover = this.uiWidgetHover(widgets, x, y, width, height);
      const background = widgets.light
        ? (active ? 0xffffe4db : hover ? 0xfffff5e7 : 0xfffaf9f8)
        : (active ? 0xff4b805f : hover ? 0xff343b45 : 0xff262c34);
      const border = widgets.light ? (active ? 0xfff56e4c : 0xffe6e2de) : (active ? 0xff61d394 : 0xff48515d);
      const label = widgets.light ? 0xff292521 : 0xffedf2f7;
      this.ui("ui_fill_round_rect", [renderer, x, y, width, height, 7, background, 0]);
      this.ui("ui_stroke_round_rect", [renderer, x, y, width, height, 7, 1, border, 0]);
      this.ui("ui_draw_text", [renderer, x + 9, y + (height - 13) * 0.5, a[6], 13, label, 0]);
      const hash = this.uiWidgetHash(this.uiWidgetId(a[1]));
      if (hover && widgets.input.mousePressed) widgets.activeId = hash;
      const clicked = hover && widgets.input.mouseReleased && widgets.activeId === hash;
      // Only the button the press belongs to lets go of it, as in lib/src/ui.c:
      // releasing it for whichever button was drawn first would take it away
      // from every later one in the same frame.
      if (widgets.input.mouseReleased && widgets.activeId === hash) widgets.activeId = 0;
      return clicked ? 1 : 0;
    }
    if (name === "ui_slider") {
      const widgets = this.uiWidgets(a[0]);
      const x = n(a[2]), y = n(a[3]), width = n(a[4]), height = n(a[5]);
      const min = n(a[7]), max = n(a[8]);
      let value = n(a[6]);
      if (!widgets || max <= min) return value;
      const hash = this.uiWidgetHash(this.uiWidgetId(a[1]));
      const hover = this.uiWidgetHover(widgets, x, y, width, height);
      if (hover && widgets.input.mousePressed) widgets.activeId = hash;
      if (widgets.activeId === hash && widgets.input.mouseDown) {
        value = min + clamp((widgets.input.mouseX - x) / width, 0, 1) * (max - min);
      }
      if (widgets.input.mouseReleased && widgets.activeId === hash) widgets.activeId = 0;
      const t = clamp((value - min) / (max - min), 0, 1);
      const renderer = widgets.renderer;
      this.ui("ui_fill_round_rect", [renderer, x, y + height * 0.4, width, height * 0.2, height * 0.1, 0xff414a55, 0]);
      this.ui("ui_fill_round_rect", [renderer, x, y + height * 0.4, width * t, height * 0.2, height * 0.1, 0xff61d394, 0]);
      this.ui("ui_fill_circle", [renderer, x + width * t, y + height * 0.5, height * 0.28, 0xffedf2f7, 0]);
      return value;
    }
    if (name === "ui_slider_rect" || name === "ui_slider_id") {
      const rect = this.readRect(a[2]);
      const id = name === "ui_slider_id" ? `slider-${n(a[1])}` : a[1];
      return this.ui("ui_slider", [a[0], id, rect.x, rect.y, rect.w, rect.h, a[3], a[4], a[5], 0]);
    }
    if (name === "ui_color_picker") {
      const widgets = this.uiWidgets(a[0]);
      const value = this.readColor(a[6]);
      if (!widgets) return this.writeColor(value);
      const x = n(a[2]), y = n(a[3]), width = n(a[4]), height = n(a[5]);
      const hash = this.uiWidgetHash(this.uiWidgetId(a[1]));
      const hover = this.uiWidgetHover(widgets, x, y, width, height);
      if (hover && widgets.input.mousePressed) widgets.activeId = hash;
      if (widgets.activeId === hash && widgets.input.mouseDown) {
        value.r = clamp((widgets.input.mouseX - x) / width, 0, 1);
        value.g = clamp(1 - (widgets.input.mouseY - y) / height, 0, 1);
        value.b = clamp(1 - Math.abs(value.r - value.g), 0, 1);
      }
      if (widgets.input.mouseReleased && widgets.activeId === hash) widgets.activeId = 0;
      const cells = 12, renderer = widgets.renderer;
      for (let iy = 0; iy < cells; iy++) for (let ix = 0; ix < cells; ix++) {
        const r = ix / (cells - 1), g = 1 - iy / (cells - 1);
        this.ui("ui_fill_rect", [renderer, x + width * ix / cells, y + height * iy / cells,
          width / cells + 1, height / cells + 1, this.ui("ui_pack_rgba_floats", [r, g, 1 - Math.abs(r - g), 1]), 0]);
      }
      this.ui("ui_stroke_circle", [renderer, x + value.r * width, y + (1 - value.g) * height, 5, 2, 0xffffffff, 0]);
      return this.writeColor(value);
    }
    if (name === "ui_color_picker_rect" || name === "ui_color_picker_id") {
      const rect = this.readRect(a[2]);
      const id = name === "ui_color_picker_id" ? `color-${n(a[1])}` : a[1];
      return this.ui("ui_color_picker", [a[0], id, rect.x, rect.y, rect.w, rect.h, a[3]]);
    }
    if (name === "ui_hit_region") {
      const widgets = this.uiWidgets(a[0]);
      const hovered = this.uiWidgetHover(widgets, n(a[1]), n(a[2]), n(a[3]), n(a[4]));
      const pointer = this.uiStatic("hit", UI_HIT.size), view = this.view();
      view.setInt32(pointer + UI_HIT.hovered, hovered ? 1 : 0, true);
      view.setInt32(pointer + UI_HIT.pressed, hovered && widgets?.input.mousePressed ? 1 : 0, true);
      return pointer;
    }

    // ── selectable read-only labels ───────────────────────────────────────
    if (name === "ui_text_sel_create") {
      const pointer = this.allocStruct(UI_TEXT_SEL.size);
      this.view().setInt32(pointer + UI_TEXT_SEL.active, -1, true);
      return pointer;
    }
    if (name === "ui_text_sel_clear") { this.writeTextSel(a[0], { active: -1, anchor: 0, head: 0, dragging: 0 }); return; }
    if (name === "ui_text_sel_has") {
      const selection = this.readTextSel(a[0]);
      return selection.active >= 0 && selection.anchor !== selection.head ? 1 : 0;
    }
    if (name === "ui_text_sel_lo") {
      const selection = this.readTextSel(a[0]);
      return Math.min(selection.anchor, selection.head);
    }
    if (name === "ui_text_sel_hi") {
      const selection = this.readTextSel(a[0]);
      return Math.max(selection.anchor, selection.head);
    }
    if (name === "ui_text_sel_slice") {
      if (!this.ui("ui_text_sel_has", [a[1]])) return this.writeString("");
      const bytes = textEncoder.encode(this.readString(a[0]));
      const lo = clamp(this.ui("ui_text_sel_lo", [a[1]]), 0, bytes.length);
      const hi = clamp(this.ui("ui_text_sel_hi", [a[1]]), 0, bytes.length);
      return this.writeString(hi > lo ? textDecoder.decode(bytes.subarray(lo, hi)) : "");
    }
    if (name === "ui_text_sel_interact") {
      const selection = this.readTextSel(a[1]), field = n(a[2]), text = this.readString(a[3]);
      const x = n(a[4]), y = n(a[5]), w = n(a[6]), h = n(a[7]), px = n(a[8]), fontType = a[9];
      const mx = n(a[10]), my = n(a[11]);
      const pressed = !!n(a[12]), down = !!n(a[13]), released = !!n(a[14]);
      const inside = mx >= x && mx < x + w && my >= y && my < y + h;
      if (pressed) {
        if (inside) {
          const index = this.uiTextIndexAtX(text, px, fontType, mx - x);
          selection.active = field; selection.anchor = index; selection.head = index; selection.dragging = 1;
        } else if (selection.active === field && !selection.dragging) {
          selection.active = -1; selection.anchor = 0; selection.head = 0; selection.dragging = 0;
        }
      }
      if (selection.dragging && selection.active === field) {
        if (down) selection.head = this.uiTextIndexAtX(text, px, fontType, clamp(mx - x, 0, w));
        if (released) selection.dragging = 0;
      }
      if (released && selection.active === field) selection.dragging = 0;
      this.writeTextSel(a[1], selection);
      return selection.active === field ? 1 : 0;
    }
    if (name === "ui_draw_text_sel") {
      const text = this.readString(a[4]), px = n(a[5]), fontType = a[7];
      const selection = this.readTextSel(a[8]), field = n(a[9]);
      const x = n(a[1]), y = n(a[2]), h = n(a[3]);
      if (selection.active === field && selection.anchor !== selection.head) {
        const lo = Math.min(selection.anchor, selection.head), hi = Math.max(selection.anchor, selection.head);
        const x0 = x + this.uiPrefixWidth(text, lo, px, fontType);
        const width = this.uiPrefixWidth(text, hi, px, fontType) - (x0 - x);
        if (width > 0) this.ui("ui_fill_rect", [a[0], x0, y, width, h, a[10], 0]);
      }
      this.ui("ui_draw_text", [a[0], x, this.ui("ui_text_v_center_y", [a[0], y, h, px, fontType]),
        a[4], px, a[6], fontType]);
      return;
    }
    if (name === "ui_text_sel_copy") {
      if (!this.ui("ui_text_sel_has", [a[1]])) return 0;
      // VIEW_KEY_C with control or super held, the way ui_text_sel_copy reads it.
      const modifiers = this.viewImport("view_take_key_press", [a[0], 67]);
      if (modifiers < 0 || (modifiers & 2) === 0 && (modifiers & 8) === 0) return 0;
      const slice = this.readString(this.ui("ui_text_sel_slice", [a[2], a[1]]));
      if (!slice) return 0;
      this.viewImport("view_set_clipboard", [a[0], this.writeString(slice)]);
      return 1;
    }
    throw new Error(`browser UI backend does not implement ${name}`);
  }

  readTextSel(pointer) {
    const view = this.view(), base = Number(pointer);
    return {
      active: view.getInt32(base + UI_TEXT_SEL.active, true),
      anchor: view.getInt32(base + UI_TEXT_SEL.anchor, true),
      head: view.getInt32(base + UI_TEXT_SEL.head, true),
      dragging: view.getInt32(base + UI_TEXT_SEL.dragging, true),
    };
  }

  writeTextSel(pointer, selection) {
    const view = this.view(), base = Number(pointer);
    view.setInt32(base + UI_TEXT_SEL.active, selection.active, true);
    view.setInt32(base + UI_TEXT_SEL.anchor, selection.anchor, true);
    view.setInt32(base + UI_TEXT_SEL.head, selection.head, true);
    view.setInt32(base + UI_TEXT_SEL.dragging, selection.dragging ? 1 : 0, true);
  }

  // The lines ui_draw_text_wrapped would draw: a newline always starts one,
  // even an empty one, an overlong line breaks at the last space that fit, and
  // a line with no space breaks before the glyph that overflowed. Text that
  // ends on a newline adds no trailing line, so the count is also the number of
  // line heights the call advances by.
  uiWrapText(text, px, maxWidth, fontType) {
    const lines = [];
    let line = "", width = 0, lastSpace = -1;
    for (const glyph of String(text)) {
      if (glyph === "\n") {
        lines.push(line);
        line = ""; width = 0; lastSpace = -1;
        continue;
      }
      const advance = this.uiTextWidth(line + glyph, px, fontType) - width;
      if (glyph === " " || glyph === "\t") lastSpace = line.length;
      if (width + advance > maxWidth && line.length > 0) {
        const cut = lastSpace >= 0 ? lastSpace : line.length;
        lines.push(line.slice(0, cut));
        line = line.slice(cut).replace(/^[ \t]+/, "") + glyph;
        width = this.uiTextWidth(line, px, fontType);
        lastSpace = -1;
        continue;
      }
      line += glyph;
      width += advance;
    }
    if (line.length > 0) lines.push(line);
    return lines;
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

  createBuffer(size, label, uniform = false) {
    if (!this.device) return 0;
    const usage = uniform
      ? GPUBufferUsage.COPY_DST | GPUBufferUsage.UNIFORM
      : this.bufferUsage();
    return this.put("buffer", this.device.createBuffer({ label, size: align(Math.max(4, Number(size)), 4), usage }));
  }

  ensureEncoder() {
    if (this.device && !this.commandEncoder) this.commandEncoder = this.device.createCommandEncoder();
    return this.commandEncoder;
  }

  formatFor(value, depth = false) {
    if (depth || value === 43) return "depth24plus";
    if (value === 23) return "rgba8unorm";
    if (value === 30) return this.rg11Storage ? "rg11b10ufloat" : "rgba16float";
    if (value === 39) return "rgba16float";
    return this.format;
  }

  texture(id) { return this.get(id, "texture"); }

  textureView(id) { return this.texture(id)?.texture.createView() || null; }

  graphicsPipeline(shaderHandle = this.currentShader, stateHandle = this.currentState) {
    const shader = this.get(shaderHandle, "shader");
    if (!shader?.vertex || !shader?.fragment || !this.device) return null;
    shader.pipelines ||= new Map();
    const state = this.get(stateHandle, "state") || {};
    const colorFormats = this.activeColorFormats?.length ? this.activeColorFormats : [this.format];
    const depthFormat = this.activeDepthFormat || null;
    const key = `${stateHandle}:${colorFormats.join(",")}:${depthFormat || ""}`;
    if (!shader.pipelines.has(key)) {
      const topology = state.primitive === 1 ? "line-list" : "triangle-list";
      shader.pipelines.set(key, this.device.createRenderPipeline({
        layout: "auto",
        vertex: { module: shader.vertex, entryPoint: shader.vs || "main" },
        fragment: { module: shader.fragment, entryPoint: shader.fs || "main", targets: colorFormats.map(format => ({ format })) },
        primitive: { topology, cullMode: state.cull === 2 ? "back" : "none" },
        ...(depthFormat ? { depthStencil: { format: depthFormat, depthWriteEnabled: !!state.depthWrite, depthCompare: "less-equal" } } : {}),
      }));
    }
    return shader.pipelines.get(key);
  }

  rootBinding() {
    if (!this.currentRoot) return null;
    const { id, offset } = this.addressParts(this.currentRoot);
    const buffer = this.get(id, "buffer");
    if (!buffer) return null;
    return { buffer, offset, size: Math.max(64, align(this.currentRootSize || 64, 16)) };
  }

  storageSlotCount() {
    if (!this.device) return 0;
    const perStage = Number(this.device.limits?.maxStorageBuffersPerShaderStage ?? 8);
    const bindings = Number(this.device.limits?.maxBindingsPerBindGroup ?? 1000) - STORAGE_BINDING_BASE;
    return Math.max(0, Math.min(perStage, bindings));
  }

  shaderStorageSlots(source) {
    const slots = new Set();
    for (const match of String(source).matchAll(/ns_storage_buffer_(\d+)/g)) slots.add(Number(match[1]));
    return [...slots].sort((a, b) => a - b);
  }

  shaderCalls(source, name) {
    const calls = source.match(new RegExp(`\\b${name}\\s*\\(`, "g"))?.length || 0;
    const definitions = source.match(new RegExp(`\\bfn\\s+${name}\\s*\\(`, "g"))?.length || 0;
    return calls > definitions;
  }

  validateShaderStorageSlots(source, operation) {
    const slots = this.shaderStorageSlots(source);
    const required = slots.length ? slots[slots.length - 1] + 1 : 0;
    const available = this.storageSlotCount();
    if (required <= available) return slots;
    console.error(`Nano Script ${operation}: shader requires ${required} storage slots, but this WebGPU device supports ${available}.`);
    return null;
  }

  bindShaderResources(pass, pipeline, shader) {
    if (!pass || !pipeline || !shader || !this.device?.createBindGroup) return;
    const entries = [];
    if (shader.usesReadTexture) {
      const texture = this.texture(Math.round(this.currentRootWords[0] || 0));
      if (!texture) return;
      entries.push({ binding: 0, resource: texture.texture.createView() });
    }
    if (shader.usesWriteTexture) {
      const texture = this.texture(Math.round(this.currentRootWords[1] || 0));
      if (!texture) return;
      entries.push({ binding: 1, resource: texture.texture.createView() });
    }
    if (shader.usesWriteTextureSecondary) {
      const texture = this.texture(Math.round(this.currentRootWords[2] || 0));
      if (!texture) return;
      entries.push({ binding: 15, resource: texture.texture.createView() });
    }
    if (shader.usesRoot) {
      const root = this.rootBinding();
      if (!root) return;
      entries.push({ binding: 2, resource: root });
    }
    for (const slot of shader.storageSlots || []) {
      if (!this.currentStorages?.[slot]) return;
      const { id, offset } = this.addressParts(this.currentStorages[slot]);
      const buffer = this.get(id, "buffer");
      if (!buffer) return;
      entries.push({ binding: STORAGE_BINDING_BASE + slot, resource: { buffer, offset } });
    }
    if (shader.usesTextureMap) {
      const texture = this.texture(Math.round(this.currentRootWords[0] || 0));
      if (!texture) return;
      entries.push({ binding: 3, resource: texture.texture.createView() });
    }
    if (shader.usesTextureSampler) {
      this.defaultSampler ||= this.device.createSampler({ minFilter: "linear", magFilter: "linear", addressModeU: "clamp-to-edge", addressModeV: "clamp-to-edge" });
      entries.push({ binding: 4, resource: this.defaultSampler });
    }
    if (shader.usesMaskMap) {
      const texture = this.texture(Math.round(this.currentRootWords[1] || 0));
      if (!texture) return;
      entries.push({ binding: 5, resource: texture.texture.createView() });
    }
    if (shader.usesMaskSampler) {
      this.nearestSampler ||= this.device.createSampler({ minFilter: "nearest", magFilter: "nearest", addressModeU: "clamp-to-edge", addressModeV: "clamp-to-edge" });
      entries.push({ binding: 6, resource: this.nearestSampler });
    }
    if (entries.length) {
      pass.setBindGroup(0, this.device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries }));
    }
  }

  // A pass label states what the pass does; WebGPU carries it into error
  // messages and capture tools, so keep a placeholder rather than drop it.
  passLabel(pointer, fallback) {
    return this.readString(Number(pointer || 0)) || fallback;
  }

  gpu(name, a) {
    const view = () => this.view();
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
      const color = Number(a[1]), depth = Number(a[2]);
      this.gpu("gpu_pass_begin", [a[0], view().getUint32(color, true), 0, 0, 0, view().getUint32(depth, true), a[3], ...a.slice(4)]);
      const width = view().getInt32(color + 4, true), height = view().getInt32(color + 8, true);
      if (width > 0 && height > 0) this.gpu("gpu_set_viewport", [0, 0, width, height]);
      return;
    }
    if (name === "gpu_request_device") {
      const owner = Number(a[0] || 0);
      if (owner && !this.views.has(owner)) return 0;
      if (owner) { this.activeView = owner; this.syncView(owner); }
      return this.hasDevice() ? 1 : 0;
    }
    if (name === "gpu_destroy_device") { this.resources.clear(); return; }
    if (name === "gpu_shader_target") return this.writeString("wgsl");
    if (name === "gpu_caps") return this.device ? 2 | 4 : 0;
    if (name === "gpu_storage_slot_count") return this.storageSlotCount();
    if (name === "gpu_texture_create") {
      if (!this.device) return 0;
      const depth = Math.max(1, Number(a[2]));
      const formatArg = a[3];
      const width = Math.max(1, Number(a[0])), height = Math.max(1, Number(a[1]));
      const format = this.formatFor(formatArg);
      const isDepth = format === "depth24plus";
      const texture = this.device.createTexture({
        size: [width, height, depth],
        format,
        usage: isDepth
          ? GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT
          : GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING |
            GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT,
        mipLevelCount: Math.max(1, Number(a[5])),
      });
      return this.put("texture", { texture, width, height, depth, format });
    }
    if (name === "gpu_texture_destroy") {
      this.texture(a[0])?.texture.destroy(); this.drop(a[0]); return;
    }
    if (name === "gpu_texture_upload") {
      const record = this.texture(a[0]);
      if (!record || !this.device?.queue.writeTexture) return;
      const dataArg = a[3];
      const sizeArg = a[4];
      const bytes = this.readBytes(dataArg, Number(sizeArg));
      const bytesPerRow = Math.max(4, record.width * 4);
      this.device.queue.writeTexture(
        { texture: record.texture, mipLevel: Number(a[1]), origin: [0, 0, Number(a[2])] },
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
    if (name === "gpu_shader_graphics_create") {
      if (!this.device) return 0;
      const vertexSource = this.readString(a[0]), fragmentSource = this.readString(a[1]);
      const vertexStorages = this.validateShaderStorageSlots(vertexSource, "gpu_shader_graphics_create");
      const fragmentStorages = this.validateShaderStorageSlots(fragmentSource, "gpu_shader_graphics_create");
      if (!vertexStorages || !fragmentStorages) return 0;
      const vertex = this.device.createShaderModule({ code: vertexSource });
      const fragment = this.device.createShaderModule({ code: fragmentSource });
      for (const [stage, module, stageSource] of [["vertex", vertex, vertexSource], ["fragment", fragment, fragmentSource]]) {
        module.getCompilationInfo?.().then(info => {
          for (const message of info.messages || []) {
            if (message.type === "error") {
              const context = stageSource.split("\n").slice(Math.max(0, message.lineNum - 2), message.lineNum + 1).join("\n");
              console.error(`Nano Script ${stage} WGSL ${message.lineNum}:${message.linePos}: ${message.message}\n${context}`);
            }
            else if (message.type === "warning") console.warn(`Nano Script ${stage} WGSL ${message.lineNum}:${message.linePos}: ${message.message}`);
          }
        });
      }
      const source = `${vertexSource}\n${fragmentSource}`;
      return this.put("shader", {
        vertex, fragment, source,
        vs: this.readString(a[2]) || "main", fs: this.readString(a[3]) || "main",
        usesRoot: source.includes("ns_root_block"),
        usesReadTexture: source.includes("ns_read_texture"),
        usesWriteTexture: source.includes("ns_write_texture"),
        usesWriteTextureSecondary: source.includes("ns_secondary_write_texture"),
        storageSlots: [...new Set([...vertexStorages, ...fragmentStorages])].sort((x, y) => x - y),
        usesTextureMap: source.includes("ns_texture_map"),
        usesTextureSampler: this.shaderCalls(source, "ns_texture_sample"),
        usesMaskMap: source.includes("ns_mask_map"),
        usesMaskSampler: this.shaderCalls(source, "ns_mask_sample"),
      });
    }
    if (name === "gpu_shader_compute_create") {
      if (!this.device) return 0;
      let source = this.readString(a[0]);
      if (!this.rg11Storage) {
        source = source.replace("requires texture_formats_tier1;", "").replaceAll("rg11b10ufloat", "rgba16float");
      }
      const storageSlots = this.validateShaderStorageSlots(source, "gpu_shader_compute_create");
      if (!storageSlots) return 0;
      const compute = this.device.createShaderModule({ code: source });
      compute.getCompilationInfo?.().then(info => {
        for (const message of info.messages || []) {
          if (message.type === "error") {
            const context = source.split("\n").slice(Math.max(0, message.lineNum - 2), message.lineNum + 1).join("\n");
            console.error(`Nano Script WGSL ${message.lineNum}:${message.linePos}: ${message.message}\n${context}`);
          }
          else if (message.type === "warning") console.warn(`Nano Script WGSL ${message.lineNum}:${message.linePos}: ${message.message}`);
        }
      });
      return this.put("shader", {
        compute, source,
        cs: this.readString(a[1]) || "main",
        usesRoot: source.includes("ns_root_block"),
        usesReadTexture: source.includes("ns_read_texture"),
        usesWriteTexture: source.includes("ns_write_texture"),
        usesWriteTextureSecondary: source.includes("ns_secondary_write_texture"),
        storageSlots,
        usesTextureMap: source.includes("ns_texture_map"),
        usesTextureSampler: this.shaderCalls(source, "ns_texture_sample"),
        usesMaskMap: source.includes("ns_mask_map"),
        usesMaskSampler: this.shaderCalls(source, "ns_mask_sample"),
      });
    }
    if (name === "gpu_shader_destroy") { this.drop(a[0]); return; }
    if (name === "gpu_state_create") return this.put("state", { primitive: Number(a[0]), cull: Number(a[1]), depthWrite: !!a[4], blend: Number(a[5]) });
    if (name === "gpu_set_shader") { this.currentShader = Number(a[0]); return; }
    if (name === "gpu_set_state") { this.currentState = Number(a[0]); return; }
    if (name === "gpu_screen_pass_begin") {
      if (!this.device || !this.context) return;
      this.ensureEncoder();
      this.pass = this.commandEncoder.beginRenderPass({ label: this.passLabel(a[0], "screen pass"), colorAttachments: [{
        view: this.context.getCurrentTexture().createView(),
        clearValue: { r: a[1], g: a[2], b: a[3], a: a[4] }, loadOp: "clear", storeOp: "store",
      }] });
      this.activeColorFormats = [this.format];
      this.activeDepthFormat = null;
      return;
    }
    if (name === "gpu_pass_begin") {
      if (!this.device) return;
      this.ensureEncoder();
      const colorAttachments = [];
      const colorFormats = [];
      for (let i = 0; i < 4; i++) {
        const texture = this.texture(a[1 + i]);
        const view = texture?.texture.createView();
        if (view) {
          colorAttachments.push({ view, clearValue: { r: a[7], g: a[8], b: a[9], a: a[10] }, loadOp: ((Number(a[6]) >> (i * 2)) & 3) === 1 ? "load" : "clear", storeOp: "store" });
          colorFormats.push(texture.format);
        }
      }
      const depthTexture = this.texture(a[5]);
      const depthView = depthTexture?.texture.createView();
      this.activeColorFormats = colorFormats;
      this.activeDepthFormat = depthTexture?.format || null;
      this.pass = this.commandEncoder.beginRenderPass({ label: this.passLabel(a[0], "render pass"), colorAttachments, ...(depthView ? { depthStencilAttachment: { view: depthView, depthClearValue: a[11], depthLoadOp: ((Number(a[6]) >> 8) & 3) === 1 ? "load" : "clear", depthStoreOp: "store" } } : {}) });
      return;
    }
    if (name === "gpu_set_viewport") { this.pass?.setViewport(Number(a[0]), Number(a[1]), Number(a[2]), Number(a[3]), 0, 1); return; }
    if (name === "gpu_set_scissor") { this.pass?.setScissorRect(Number(a[0]), Number(a[1]), Number(a[2]), Number(a[3])); return; }
    if (name === "gpu_pass_end") { this.pass?.end(); this.pass = null; return; }
    if (name === "gpu_commit") {
      if (this.pass) this.gpu("gpu_pass_end", []);
      if (this.commandEncoder && this.device) this.device.queue.submit([this.commandEncoder.finish()]);
      this.commandEncoder = null;
      this.frameIndex++;
      const nextFrame = this.frameBuffers[this.frameIndex % 3];
      if (nextFrame) nextFrame.offset = 0;
      return;
    }
    if (name === "gpu_draw_vertices") {
      const pipeline = this.graphicsPipeline(), shader = this.get(this.currentShader, "shader");
      if (pipeline && this.pass) {
        this.pass.setPipeline(pipeline);
        this.bindShaderResources(this.pass, pipeline, shader);
      }
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
      const encoder = this.ensureEncoder(), pass = encoder.beginComputePass({ label: this.passLabel(a[0], "compute pass") });
      pass.setPipeline(shader.pipeline);
      this.bindShaderResources(pass, shader.pipeline, shader);
      pass.dispatchWorkgroups(Number(a[1]), Number(a[2]), Number(a[3])); pass.end(); return;
    }
    if (name === "gpu_dispatch_indirect") {
      const shader = this.get(this.currentShader, "shader"), address = this.addressParts(a[1]), buffer = this.get(address.id, "buffer");
      if (!shader?.compute || !buffer || !this.device) return;
      shader.pipeline ||= this.device.createComputePipeline({ layout: "auto", compute: { module: shader.compute, entryPoint: shader.cs } });
      const encoder = this.ensureEncoder(), pass = encoder.beginComputePass({ label: this.passLabel(a[0], "compute pass") });
      pass.setPipeline(shader.pipeline); pass.dispatchWorkgroupsIndirect(buffer, address.offset); pass.end(); return;
    }
    if (name === "gpu_malloc") {
      const label = this.readString(a[2]);
      if (!label) return 0n;
      const id = this.createBuffer(Number(a[0]), label);
      return BigInt(id) << 32n;
    }
    if (name === "gpu_frame_alloc") {
      if (!this.device) return 0n;
      const ring = this.frameIndex % 3;
      if (!this.frameBuffers[ring]) {
        const usage = this.bufferUsage() | GPUBufferUsage.UNIFORM;
        const label = `ns frame ring ${ring}`;
        this.frameBuffers[ring] = { id: this.put("buffer", this.device.createBuffer({ label, size: 1024 * 1024, usage })), offset: 0 };
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
    if (name === "gpu_set_root") {
      this.currentRoot = BigInt(a[0]);
      this.currentRootSize = 0;
      this.currentRootWords = new Float32Array();
      return;
    }
    if (name === "gpu_set_storage") {
      this.currentStorages ||= [];
      this.currentStorages[0] = BigInt(a[0]);
      return;
    }
    if (name === "gpu_set_storage_at") {
      const index = Number(a[0]);
      if (index < 0 || index >= this.storageSlotCount()) return;
      this.currentStorages ||= [];
      this.currentStorages[index] = BigInt(a[1]);
      return;
    }
    if (name === "gpu_set_root_data") {
      const bytes = Uint8Array.from(this.readBytes(a[0], Number(a[1])));
      const address = this.gpu("gpu_frame_alloc", [a[1], 256]);
      if (address) {
        this.gpu("gpu_write", [address, a[0], a[1]]);
        this.currentRoot = address;
        this.currentRootSize = Number(a[1]);
        this.currentRootWords = new Float32Array(bytes.buffer, bytes.byteOffset, Math.floor(bytes.byteLength / 4));
      }
      return;
    }
    if (name === "gpu_read") return 0;
    if (name === "gpu_draw_indirect" || name === "gpu_dispatch_indirect" ||
        name === "gpu_signal_after" || name === "gpu_wait_before" || name === "gpu_set_root" ||
        name === "gpu_set_root_data" || name === "gpu_set_viewport" || name === "gpu_set_scissor") return;
    if (name === "gpu_pixel_format_size") return (Number(a[0]) === 43 ? 4 : 4);
    if (name === "gpu_pixel_format_row_pitch") return align(Number(a[1]) * 4, Math.max(1, Number(a[2])));
    if (name === "gpu_pixel_format_surface_pitch") return align(Number(a[1]) * 4, Math.max(1, Number(a[3]))) * Number(a[2]);
    if (name.startsWith("gpu_destroy_")) { this.drop(a[0]); return; }
    // Capability-dependent resource combinations are safe no-ops
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

export async function boot(wasmURL, options = {}) {
  const canvas = document.getElementById("ns-canvas") || document.querySelector("canvas");
  if (!canvas) throw new Error("Nano Script Wasm shell requires a canvas");
  // Connect before compiling so a page that loaded a temporarily invalid
  // artifact can still recover after the next successful rebuild.
  if (isLoopbackPage()) connectReloadSocket();
  const runtime = new NSBrowserRuntime(canvas);
  await runtime.preloadFiles(options.assets || []);
  const response = await fetch(wasmURL, { cache: "no-store" });
  if (!response.ok) throw new Error(`failed to fetch ${wasmURL}: ${response.status}`);
  const module = WebAssembly.compileStreaming
    ? await WebAssembly.compileStreaming(Promise.resolve(response))
    : await WebAssembly.compile(await response.arrayBuffer());
  runtime.loadShaders(module);
  const imports = WebAssembly.Module.imports(module);
  const usesCanvasUI = imports.some(item => item.module === "ui");
  const usesGPU = imports.some(item => item.module === "gpu");
  const instance = await WebAssembly.instantiate(module, runtime.importsFor(module));
  runtime.instance = instance;
  runtime.memory = instance.exports.memory;
  runtime.usesGPU = usesGPU;
  // A program that imports neither still gets the device, so a canvas is ready
  // for whatever it draws through view. The ui canvas comes second because an
  // application that asked for both needs the overlay, which is decided by
  // whether WebGPU took the application canvas's context.
  if (usesGPU || !usesCanvasUI) await runtime.initializeGPU();
  if (usesCanvasUI) runtime.initializeCanvasUI();
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
