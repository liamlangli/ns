// WebGPU backend: two pipelines (rect + MSDF text), uploads draw list each frame.

import type { DrawList, FontAtlas } from './draw.ts';

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

fn median(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
    let s     = textureSample(msdf_tex, msdf_smp, v.uv);
    let sd    = median(s.r, s.g, s.b);
    let w     = fwidth(sd);
    let alpha = clamp((sd - 0.5) / max(w, 0.0001) + 0.5, 0.0, 1.0);
    return vec4f(v.col.rgb, v.col.a * alpha);
}
`;

function _upload(device: GPUDevice, data: Float32Array): GPUBuffer {
    const buf = device.createBuffer({
        size:             Math.ceil(data.byteLength / 4) * 4,
        usage:            GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
        mappedAtCreation: true,
    });
    new Float32Array(buf.getMappedRange()).set(data);
    buf.unmap();
    return buf;
}

export class GPU {
    private canvas: HTMLCanvasElement;
    device: GPUDevice | null = null;
    private ctx!: GPUCanvasContext;
    private fmt!: GPUTextureFormat;

    private _rect_pipeline!: GPURenderPipeline;
    private _text_pipeline!: GPURenderPipeline;
    private _uniform_buf!:   GPUBuffer;
    private _rect_bg!:       GPUBindGroup;
    private _text_bg!:       GPUBindGroup;

    private _vp_w = 0;
    private _vp_h = 0;

    constructor(canvas: HTMLCanvasElement) {
        this.canvas = canvas;
    }

    async init(device: GPUDevice, font: FontAtlas): Promise<void> {
        this.device = device;
        const dev   = device;

        this.ctx = this.canvas.getContext('webgpu')!;
        this.fmt = navigator.gpu.getPreferredCanvasFormat();
        this.ctx.configure({ device: dev, format: this.fmt, alphaMode: 'opaque' });

        this._uniform_buf = dev.createBuffer({
            label: 'uniform', size: 16,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });

        // ── Rect pipeline ────────────────────────────────────────────────────
        const rect_mod = dev.createShaderModule({ label: 'rect', code: RECT_SHADER });
        const rect_bgl = dev.createBindGroupLayout({
            label:   'rect-bgl',
            entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: {} }],
        });
        this._rect_pipeline = dev.createRenderPipeline({
            label:  'rect',
            layout: dev.createPipelineLayout({ bindGroupLayouts: [rect_bgl] }),
            vertex: {
                module: rect_mod, entryPoint: 'vs',
                buffers: [{
                    arrayStride: 24,
                    attributes: [
                        { shaderLocation: 0, offset:  0, format: 'float32x2' },
                        { shaderLocation: 1, offset:  8, format: 'float32x4' },
                    ],
                }],
            },
            fragment: {
                module: rect_mod, entryPoint: 'fs',
                targets: [{
                    format: this.fmt,
                    blend: {
                        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
                        alpha: { srcFactor: 'one',       dstFactor: 'one-minus-src-alpha', operation: 'add' },
                    },
                }],
            },
            primitive: { topology: 'triangle-list' },
        });
        this._rect_bg = dev.createBindGroup({
            layout:  rect_bgl,
            entries: [{ binding: 0, resource: { buffer: this._uniform_buf } }],
        });

        // ── Text pipeline ────────────────────────────────────────────────────
        const text_mod = dev.createShaderModule({ label: 'text', code: TEXT_SHADER });
        const text_bgl = dev.createBindGroupLayout({
            label:   'text-bgl',
            entries: [
                { binding: 0, visibility: GPUShaderStage.VERTEX,   buffer: {} },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
            ],
        });
        this._text_pipeline = dev.createRenderPipeline({
            label:  'text',
            layout: dev.createPipelineLayout({ bindGroupLayouts: [text_bgl] }),
            vertex: {
                module: text_mod, entryPoint: 'vs',
                buffers: [{
                    arrayStride: 32,
                    attributes: [
                        { shaderLocation: 0, offset:  0, format: 'float32x2' },
                        { shaderLocation: 1, offset:  8, format: 'float32x2' },
                        { shaderLocation: 2, offset: 16, format: 'float32x4' },
                    ],
                }],
            },
            fragment: {
                module: text_mod, entryPoint: 'fs',
                targets: [{
                    format: this.fmt,
                    blend: {
                        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
                        alpha: { srcFactor: 'one',       dstFactor: 'one-minus-src-alpha', operation: 'add' },
                    },
                }],
            },
            primitive: { topology: 'triangle-list' },
        });
        this._text_bg = dev.createBindGroup({
            layout:  text_bgl,
            entries: [
                { binding: 0, resource: { buffer: this._uniform_buf } },
                { binding: 1, resource: font.texture.createView() },
                { binding: 2, resource: font.sampler },
            ],
        });
    }

    resize(w: number, h: number): void {
        this._vp_w = w; this._vp_h = h;
        const dpr = window.devicePixelRatio ?? 1;
        this.canvas.width        = Math.max(1, Math.round(w * dpr));
        this.canvas.height       = Math.max(1, Math.round(h * dpr));
        this.canvas.style.width  = `${w}px`;
        this.canvas.style.height = `${h}px`;
    }

    render(dl: DrawList): void {
        const dev = this.device!;
        dl.finalize();

        dev.queue.writeBuffer(this._uniform_buf, 0,
            new Float32Array([this._vp_w, this._vp_h, 0, 0]));

        const r_data = dl.rect_data;
        const t_data = dl.text_data;
        const r_buf  = r_data.length > 0 ? _upload(dev, r_data) : null;
        const t_buf  = t_data.length > 0 ? _upload(dev, t_data) : null;

        const enc  = dev.createCommandEncoder();
        const pass = enc.beginRenderPass({
            colorAttachments: [{
                view:       this.ctx.getCurrentTexture().createView(),
                loadOp:     'clear',
                clearValue: { r: 0.118, g: 0.118, b: 0.141, a: 1 },
                storeOp:    'store',
            }],
        });

        const dpr  = window.devicePixelRatio ?? 1;
        const ph_w = Math.round(this._vp_w * dpr);
        const ph_h = Math.round(this._vp_h * dpr);
        pass.setScissorRect(0, 0, ph_w, ph_h);

        for (const cmd of dl.commands) {
            if (cmd.type === 'scissor') {
                const sx = Math.max(0, Math.round(cmd.x * dpr));
                const sy = Math.max(0, Math.round(cmd.y * dpr));
                const sw = Math.max(1, Math.min(Math.round(cmd.w * dpr), ph_w - sx));
                const sh = Math.max(1, Math.min(Math.round(cmd.h * dpr), ph_h - sy));
                pass.setScissorRect(sx, sy, sw, sh);
                continue;
            }
            if (cmd.rcnt > 0 && r_buf) {
                pass.setPipeline(this._rect_pipeline);
                pass.setBindGroup(0, this._rect_bg);
                pass.setVertexBuffer(0, r_buf, cmd.rbase * 24, cmd.rcnt * 24);
                pass.draw(cmd.rcnt);
            }
            if (cmd.tcnt > 0 && t_buf) {
                pass.setPipeline(this._text_pipeline);
                pass.setBindGroup(0, this._text_bg);
                pass.setVertexBuffer(0, t_buf, cmd.tbase * 32, cmd.tcnt * 32);
                pass.draw(cmd.tcnt);
            }
        }

        pass.end();
        dev.queue.submit([enc.finish()]);
        r_buf?.destroy();
        t_buf?.destroy();
    }
}
