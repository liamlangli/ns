// WebGPU backend: two pipelines (rect + MSDF text), uploads draw list each frame.

const RECT_SHADER = /* wgsl */`
struct Uni { viewport: vec2f }
@group(0) @binding(0) var<uniform> uni: Uni;

struct VIn  { @location(0) pos: vec2f, @location(1) col: vec4f }
struct VOut { @builtin(position) pos: vec4f, @location(0) col: vec4f }

@vertex fn vs(v: VIn) -> VOut {
    let ndc = vec2f(
        v.pos.x / uni.viewport.x * 2.0 - 1.0,
        1.0 - v.pos.y / uni.viewport.y * 2.0
    );
    return VOut(vec4f(ndc, 0.0, 1.0), v.col);
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
    return v.col;
}
`;

const TEXT_SHADER = /* wgsl */`
struct Uni { viewport: vec2f }
@group(0) @binding(0) var<uniform> uni      : Uni;
@group(0) @binding(1) var          msdf_tex : texture_2d<f32>;
@group(0) @binding(2) var          msdf_smp : sampler;

struct VIn  { @location(0) pos: vec2f, @location(1) uv: vec2f, @location(2) col: vec4f }
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f }

@vertex fn vs(v: VIn) -> VOut {
    let ndc = vec2f(
        v.pos.x / uni.viewport.x * 2.0 - 1.0,
        1.0 - v.pos.y / uni.viewport.y * 2.0
    );
    return VOut(vec4f(ndc, 0.0, 1.0), v.uv, v.col);
}

// MSDF: take median of R,G,B channels, then anti-alias via screen-space derivatives.
fn median(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
    let s   = textureSample(msdf_tex, msdf_smp, v.uv);
    let sd  = median(s.r, s.g, s.b);
    // Use screen-space derivative for pixel-perfect AA at any scale
    let w   = fwidth(sd);
    let alpha = clamp((sd - 0.5) / max(w, 0.0001) + 0.5, 0.0, 1.0);
    return vec4f(v.col.rgb, v.col.a * alpha);
}
`;

export class GPU {
    constructor(canvas) {
        this.canvas = canvas;
        this.device = null;
        this.ctx    = null;
        this.fmt    = null;

        this._rectPipeline = null;
        this._textPipeline = null;
        this._uniformBuf   = null;
        this._uniformBG    = null;
        this._textBG       = null;

        this._vpW = 0;
        this._vpH = 0;
    }

    /** @param {GPUDevice} device  @param {object} font  atlas from buildFontAtlas */
    async init(device, font) {
        this.device = device;
        const dev   = this.device;

        this.ctx = this.canvas.getContext('webgpu');
        this.fmt = navigator.gpu.getPreferredCanvasFormat();
        this.ctx.configure({ device: dev, format: this.fmt, alphaMode: 'opaque' });

        // ── Uniform buffer (viewport vec2f, padded to 16 bytes) ─────────────
        this._uniformBuf = dev.createBuffer({
            label: 'uniform',
            size:  16,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });

        // ── Rect pipeline ────────────────────────────────────────────────────
        const rectMod = dev.createShaderModule({ label: 'rect', code: RECT_SHADER });
        const rectBGL = dev.createBindGroupLayout({
            label:   'rect-bgl',
            entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: {} }],
        });
        this._rectPipeline = dev.createRenderPipeline({
            label:  'rect',
            layout: dev.createPipelineLayout({ bindGroupLayouts: [rectBGL] }),
            vertex: {
                module:     rectMod,
                entryPoint: 'vs',
                buffers: [{
                    arrayStride: 24,   // 6 floats × 4 bytes
                    attributes: [
                        { shaderLocation: 0, offset:  0, format: 'float32x2' }, // pos
                        { shaderLocation: 1, offset:  8, format: 'float32x4' }, // col
                    ],
                }],
            },
            fragment: {
                module:     rectMod,
                entryPoint: 'fs',
                targets: [{
                    format: this.fmt,
                    blend: {
                        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
                        alpha: { srcFactor: 'one',       dstFactor: 'one-minus-src-alpha' },
                    },
                }],
            },
            primitive: { topology: 'triangle-list' },
        });
        this._rectBG = dev.createBindGroup({
            layout:  rectBGL,
            entries: [{ binding: 0, resource: { buffer: this._uniformBuf } }],
        });

        // ── Text pipeline ────────────────────────────────────────────────────
        const textMod = dev.createShaderModule({ label: 'text', code: TEXT_SHADER });
        const textBGL = dev.createBindGroupLayout({
            label:   'text-bgl',
            entries: [
                { binding: 0, visibility: GPUShaderStage.VERTEX,   buffer: {} },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
            ],
        });
        this._textPipeline = dev.createRenderPipeline({
            label:  'text',
            layout: dev.createPipelineLayout({ bindGroupLayouts: [textBGL] }),
            vertex: {
                module:     textMod,
                entryPoint: 'vs',
                buffers: [{
                    arrayStride: 32,   // 8 floats × 4 bytes
                    attributes: [
                        { shaderLocation: 0, offset:  0, format: 'float32x2' }, // pos
                        { shaderLocation: 1, offset:  8, format: 'float32x2' }, // uv
                        { shaderLocation: 2, offset: 16, format: 'float32x4' }, // col
                    ],
                }],
            },
            fragment: {
                module:     textMod,
                entryPoint: 'fs',
                targets: [{
                    format: this.fmt,
                    blend: {
                        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
                        alpha: { srcFactor: 'one',       dstFactor: 'one-minus-src-alpha' },
                    },
                }],
            },
            primitive: { topology: 'triangle-list' },
        });
        this._textBG = dev.createBindGroup({
            layout:  textBGL,
            entries: [
                { binding: 0, resource: { buffer: this._uniformBuf } },
                { binding: 1, resource: font.texture.createView() },
                { binding: 2, resource: font.sampler },
            ],
        });
    }

    resize(w, h) {
        this._vpW = w; this._vpH = h;
        this.canvas.width  = Math.max(1, Math.round(w * (window.devicePixelRatio ?? 1)));
        this.canvas.height = Math.max(1, Math.round(h * (window.devicePixelRatio ?? 1)));
        this.canvas.style.width  = w + 'px';
        this.canvas.style.height = h + 'px';
    }

    render(dl) {
        const dev = this.device;
        dl.finalize();

        // Update uniform (canvas size in CSS pixels so UI coords match)
        dev.queue.writeBuffer(this._uniformBuf, 0,
            new Float32Array([this._vpW, this._vpH, 0, 0]));

        const rData = dl.rectData;
        const tData = dl.textData;

        const rBuf = rData.length > 0 ? _upload(dev, rData) : null;
        const tBuf = tData.length > 0 ? _upload(dev, tData) : null;

        const enc  = dev.createCommandEncoder();
        const pass = enc.beginRenderPass({
            colorAttachments: [{
                view:       this.ctx.getCurrentTexture().createView(),
                loadOp:     'clear',
                clearValue: { r: 0.118, g: 0.118, b: 0.141, a: 1 }, // #1e1e24
                storeOp:    'store',
            }],
        });

        // Emit commands
        let curScissor = null;
        const dpr = window.devicePixelRatio ?? 1;
        const phW = Math.round(this._vpW * dpr);
        const phH = Math.round(this._vpH * dpr);
        pass.setScissorRect(0, 0, phW, phH);

        for (const cmd of dl.commands) {
            if (cmd.type === 'scissor') {
                // Convert CSS-pixel scissor to physical pixels
                const sx = Math.max(0, Math.round(cmd.x * dpr));
                const sy = Math.max(0, Math.round(cmd.y * dpr));
                const sw = Math.max(1, Math.min(Math.round(cmd.w * dpr), phW - sx));
                const sh = Math.max(1, Math.min(Math.round(cmd.h * dpr), phH - sy));
                pass.setScissorRect(sx, sy, sw, sh);
                curScissor = { sx, sy, sw, sh };
                continue;
            }
            // cmd.type === 'draw'
            if (cmd.rcnt > 0 && rBuf) {
                pass.setPipeline(this._rectPipeline);
                pass.setBindGroup(0, this._rectBG);
                pass.setVertexBuffer(0, rBuf, cmd.rbase * 24, cmd.rcnt * 24);
                pass.draw(cmd.rcnt);
            }
            if (cmd.tcnt > 0 && tBuf) {
                pass.setPipeline(this._textPipeline);
                pass.setBindGroup(0, this._textBG);
                pass.setVertexBuffer(0, tBuf, cmd.tbase * 32, cmd.tcnt * 32);
                pass.draw(cmd.tcnt);
            }
        }

        pass.end();
        dev.queue.submit([enc.finish()]);

        if (rBuf) rBuf.destroy();
        if (tBuf) tBuf.destroy();
    }
}

function _upload(device, data) {
    const buf = device.createBuffer({
        size:  Math.ceil(data.byteLength / 4) * 4,
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
        mappedAtCreation: true,
    });
    new Float32Array(buf.getMappedRange()).set(data);
    buf.unmap();
    return buf;
}
