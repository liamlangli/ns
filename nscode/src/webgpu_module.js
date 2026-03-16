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
     * Execute user-supplied WebGPU JavaScript.
     * The code runs with these injected globals:
     *   device   – GPUDevice (shared with the main renderer)
     *   canvas   – the preview HTMLCanvasElement
     *   context  – GPUCanvasContext for the preview canvas
     *   format   – preferred canvas texture format string
     *   print    – function(value) to emit a line in the Output panel
     *
     * Returns { ok: boolean, error?: string }
     */
    async run(code, print_fn) {
        if (!this._ready) return { ok: false, error: 'WebGPU module not ready' };
        this.clear();
        try {
            const fn = new AsyncFn(
                'device', 'canvas', 'context', 'format', 'print',
                code
            );
            await fn(
                this.device,
                this.canvas,
                this.context,
                this.format,
                print_fn
            );
            return { ok: true };
        } catch (e) {
            return { ok: false, error: e.message ?? String(e) };
        }
    }
}
