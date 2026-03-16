// webgpu_module.js
// Manages a secondary WebGPU canvas overlay for user-authored WebGPU programs.
//
// Usage:
//   const mod = new WebGPUModule();
//   await mod.init(device);           // call once after main device is ready
//   mod.resize(x, y, w, h);          // called each frame to position the overlay
//   mod.show() / mod.hide();          // show/hide the overlay canvas
//   const { ok, error } = await mod.run(code, print_fn);  // execute user code

const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;

export class WebGPUModule {
    constructor() {
        this.canvas  = null;
        this.device  = null;
        this.context = null;
        this.format  = null;
        this._ready  = false;
    }

    /** Call once after the shared GPUDevice is created. */
    async init(device) {
        this.device = device;

        const c = document.createElement('canvas');
        c.style.cssText = [
            'position:fixed',
            'display:none',
            'z-index:3',
            'background:#1e1e24',
            'pointer-events:none',
        ].join(';');
        document.body.appendChild(c);
        this.canvas = c;

        this.format  = navigator.gpu.getPreferredCanvasFormat();
        this.context = c.getContext('webgpu');
        this._configure();
        this._ready = true;
    }

    _configure() {
        this.context.configure({
            device:    this.device,
            format:    this.format,
            alphaMode: 'premultiplied',
        });
    }

    /** Reposition and resize the overlay canvas each frame. */
    resize(x, y, w, h) {
        if (!this.canvas) return;
        const s = this.canvas.style;
        s.left   = `${x}px`;
        s.top    = `${y}px`;
        s.width  = `${w}px`;
        s.height = `${h}px`;

        const dpr = window.devicePixelRatio || 1;
        const pw  = Math.max(1, Math.round(w * dpr));
        const ph  = Math.max(1, Math.round(h * dpr));
        if (this.canvas.width !== pw || this.canvas.height !== ph) {
            this.canvas.width  = pw;
            this.canvas.height = ph;
            if (this._ready) this._configure();
        }
    }

    show() { if (this.canvas) this.canvas.style.display = 'block'; }
    hide() { if (this.canvas) this.canvas.style.display = 'none';  }

    /** Fill the preview canvas with the background colour. */
    clear() {
        if (!this._ready) return;
        const enc  = this.device.createCommandEncoder();
        const view = this.context.getCurrentTexture().createView();
        const pass = enc.beginRenderPass({
            colorAttachments: [{
                view,
                clearValue: { r: 0.118, g: 0.118, b: 0.141, a: 1 },
                loadOp:  'clear',
                storeOp: 'store',
            }],
        });
        pass.end();
        this.device.queue.submit([enc.finish()]);
    }

    /**
     * Returns an object of NS built-in function definitions to be injected
     * into the NS interpreter's global scope for WebGPU examples.
     *
     * Available in NanoScript as:
     *   gpu_shader(wgsl)          – compile a WGSL shader, return opaque handle
     *   gpu_pipeline(shader)      – build a triangle-list render pipeline (vs/fs)
     *   gpu_render(pipeline, n)   – clear, draw n vertices, submit
     */
    make_ns_globals() {
        if (!this._ready) return {};
        const device  = this.device;
        const context = this.context;
        const format  = this.format;

        return {
            gpu_shader: {
                __fn: true,
                call: ([wgsl]) => device.createShaderModule({ code: wgsl }),
            },
            gpu_pipeline: {
                __fn: true,
                call: ([shader]) => device.createRenderPipeline({
                    layout:    'auto',
                    vertex:    { module: shader, entryPoint: 'vs' },
                    fragment:  { module: shader, entryPoint: 'fs', targets: [{ format }] },
                    primitive: { topology: 'triangle-list' },
                }),
            },
            gpu_render: {
                __fn: true,
                call: ([pipeline, count]) => {
                    const n   = (typeof count === 'number' && count > 0) ? count : 3;
                    const enc = device.createCommandEncoder();
                    const view = context.getCurrentTexture().createView();
                    const pass = enc.beginRenderPass({
                        colorAttachments: [{
                            view,
                            clearValue: { r: 0.118, g: 0.118, b: 0.141, a: 1 },
                            loadOp:  'clear',
                            storeOp: 'store',
                        }],
                    });
                    pass.setPipeline(pipeline);
                    pass.draw(n);
                    pass.end();
                    device.queue.submit([enc.finish()]);
                    return null;
                },
            },
        };
    }
}
