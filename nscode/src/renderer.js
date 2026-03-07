// WebGPU-based text renderer for the NS code editor

import { tokenizeLine, TOKEN_COLOR } from './syntax.js';

const ATLAS_COLS  = 16;
const ATLAS_ROWS  = 8;
const GLYPH_START = 32; // ASCII space
const GLYPH_COUNT = ATLAS_COLS * ATLAS_ROWS; // 128 glyphs

const WGSL_SHADER = /* wgsl */`
struct Uniforms {
    viewport: vec2f,
    scroll:   vec2f,
};

@group(0) @binding(0) var<uniform> uni: Uniforms;
@group(0) @binding(1) var font_tex: texture_2d<f32>;
@group(0) @binding(2) var font_smp: sampler;

struct VIn {
    @location(0) pos:   vec2f,
    @location(1) uv:    vec2f,
    @location(2) color: vec4f,
};

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv:        vec2f,
    @location(1) color:     vec4f,
};

@vertex fn vs(v: VIn) -> VOut {
    var out: VOut;
    let p = v.pos - uni.scroll;
    let ndc = p / uni.viewport * 2.0 - vec2f(1.0);
    out.pos   = vec4f(ndc.x, -ndc.y, 0.0, 1.0);
    out.uv    = v.uv;
    out.color = v.color;
    return out;
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
    let a = textureSample(font_tex, font_smp, v.uv).r;
    return vec4f(v.color.rgb, v.color.a * a);
}

// Solid rectangle (cursor / selection) — no texture
struct RectVIn {
    @location(0) pos:   vec2f,
    @location(1) color: vec4f,
};

struct RectVOut {
    @builtin(position) pos: vec4f,
    @location(0) color:     vec4f,
};

@vertex fn rect_vs(v: RectVIn) -> RectVOut {
    var out: RectVOut;
    let p = v.pos - uni.scroll;
    let ndc = p / uni.viewport * 2.0 - vec2f(1.0);
    out.pos   = vec4f(ndc.x, -ndc.y, 0.0, 1.0);
    out.color = v.color;
    return out;
}

@fragment fn rect_fs(v: RectVOut) -> @location(0) vec4f {
    return v.color;
}
`;

export class Renderer {
    constructor(canvas) {
        this.canvas  = canvas;
        this.device  = null;
        this.context = null;

        // Font metrics (filled after atlas build)
        this.glyphW  = 0;
        this.glyphH  = 0;
        this.atlasW  = 0;
        this.atlasH  = 0;

        // GPU resources
        this.textPipeline = null;
        this.rectPipeline = null;
        this.uniformBuf   = null;
        this.bindGroup    = null;
        this.fontTex      = null;
        this.sampler      = null;

        // Layout config
        this.fontSize     = 15;
        this.lineNumWidth = 0; // px, set after font known
        this.paddingLeft  = 8;
        this.paddingTop   = 8;

        this.ready = false;
    }

    async init() {
        if (!navigator.gpu) throw new Error('WebGPU not supported');
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) throw new Error('No WebGPU adapter');
        this.device = await adapter.requestDevice();

        this.context = this.canvas.getContext('webgpu');
        const fmt = navigator.gpu.getPreferredCanvasFormat();
        this.context.configure({
            device: this.device,
            format: fmt,
            alphaMode: 'premultiplied',
        });

        await this._buildFontAtlas();
        this._buildPipelines(fmt);
        this.ready = true;
    }

    // Build a glyph atlas using Canvas 2D
    async _buildFontAtlas() {
        const font = `${this.fontSize}px 'Courier New', monospace`;

        // Measure glyph size
        const measure = document.createElement('canvas');
        const mctx = measure.getContext('2d');
        mctx.font = font;
        const met = mctx.measureText('W');
        this.glyphW = Math.ceil(met.width) + 2;
        this.glyphH = Math.ceil(this.fontSize * 1.5) + 2;

        this.atlasW = this.glyphW * ATLAS_COLS;
        this.atlasH = this.glyphH * ATLAS_ROWS;

        const ac = document.createElement('canvas');
        ac.width  = this.atlasW;
        ac.height = this.atlasH;
        const actx = ac.getContext('2d');
        actx.fillStyle = '#000';
        actx.fillRect(0, 0, this.atlasW, this.atlasH);
        actx.font = font;
        actx.fillStyle = '#fff';
        actx.textBaseline = 'top';

        for (let i = 0; i < GLYPH_COUNT; i++) {
            const ch = String.fromCharCode(GLYPH_START + i);
            const gx = (i % ATLAS_COLS) * this.glyphW + 1;
            const gy = Math.floor(i / ATLAS_COLS) * this.glyphH + 1;
            actx.fillText(ch, gx, gy);
        }

        // Upload to WebGPU texture
        const imageData = actx.getImageData(0, 0, this.atlasW, this.atlasH);
        // Extract red channel as atlas
        const pixels = new Uint8Array(this.atlasW * this.atlasH);
        for (let i = 0; i < pixels.length; i++) pixels[i] = imageData.data[i * 4];

        this.fontTex = this.device.createTexture({
            size: [this.atlasW, this.atlasH],
            format: 'r8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        });
        this.device.queue.writeTexture(
            { texture: this.fontTex },
            pixels,
            { bytesPerRow: this.atlasW },
            [this.atlasW, this.atlasH],
        );

        this.sampler = this.device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear',
        });

        this.lineNumWidth = this.glyphW * 4 + this.paddingLeft * 2;
    }

    _buildPipelines(format) {
        const module = this.device.createShaderModule({ code: WGSL_SHADER });
        const d = this.device;

        // Uniform buffer: viewport(vec2f) + scroll(vec2f) = 16 bytes
        this.uniformBuf = d.createBuffer({
            size: 16,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });

        const bgl = d.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.VERTEX,   buffer: {} },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT,  texture: { sampleType: 'float' } },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT,  sampler: {} },
            ],
        });

        this.bindGroup = d.createBindGroup({
            layout: bgl,
            entries: [
                { binding: 0, resource: { buffer: this.uniformBuf } },
                { binding: 1, resource: this.fontTex.createView() },
                { binding: 2, resource: this.sampler },
            ],
        });

        const blendAlpha = {
            color: {
                srcFactor: 'src-alpha',
                dstFactor: 'one-minus-src-alpha',
                operation: 'add',
            },
            alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
        };

        // Text pipeline (pos:2f, uv:2f, color:4f = 32 bytes/vertex)
        const textVBL = [{
            arrayStride: 32,
            attributes: [
                { shaderLocation: 0, offset:  0, format: 'float32x2' }, // pos
                { shaderLocation: 1, offset:  8, format: 'float32x2' }, // uv
                { shaderLocation: 2, offset: 16, format: 'float32x4' }, // color
            ],
        }];
        this.textPipeline = d.createRenderPipeline({
            layout: d.createPipelineLayout({ bindGroupLayouts: [bgl] }),
            vertex:   { module, entryPoint: 'vs',      buffers: textVBL },
            fragment: { module, entryPoint: 'fs',      targets: [{ format, blend: blendAlpha }] },
            primitive: { topology: 'triangle-list' },
        });

        // Rect pipeline (pos:2f, color:4f = 24 bytes/vertex)
        const rectVBL = [{
            arrayStride: 24,
            attributes: [
                { shaderLocation: 0, offset:  0, format: 'float32x2' }, // pos
                { shaderLocation: 1, offset:  8, format: 'float32x4' }, // color
            ],
        }];
        this.rectPipeline = d.createRenderPipeline({
            layout: d.createPipelineLayout({ bindGroupLayouts: [bgl] }),
            vertex:   { module, entryPoint: 'rect_vs', buffers: rectVBL },
            fragment: { module, entryPoint: 'rect_fs', targets: [{ format, blend: blendAlpha }] },
            primitive: { topology: 'triangle-list' },
        });
    }

    // Map ASCII code point to UV rect in atlas
    _glyphUV(codePoint) {
        const idx = codePoint - GLYPH_START;
        if (idx < 0 || idx >= GLYPH_COUNT) return this._glyphUV(63); // '?'
        const col = idx % ATLAS_COLS;
        const row = Math.floor(idx / ATLAS_COLS);
        const u0 = col * this.glyphW / this.atlasW;
        const v0 = row * this.glyphH / this.atlasH;
        const u1 = u0 + (this.glyphW - 1) / this.atlasW;
        const v1 = v0 + (this.glyphH - 1) / this.atlasH;
        return { u0, v0, u1, v1 };
    }

    // Push a quad (6 floats per vertex * 6 vertices = 36 for text)
    _pushTextQuad(verts, x, y, w, h, uv, color) {
        const { u0, v0, u1, v1 } = uv;
        const [r, g, b, a] = color;
        const push = (px, py, pu, pv) => {
            verts.push(px, py, pu, pv, r, g, b, a);
        };
        push(x,   y,   u0, v0);
        push(x+w, y,   u1, v0);
        push(x,   y+h, u0, v1);
        push(x+w, y,   u1, v0);
        push(x+w, y+h, u1, v1);
        push(x,   y+h, u0, v1);
    }

    _pushRectQuad(verts, x, y, w, h, color) {
        const [r, g, b, a] = color;
        const push = (px, py) => verts.push(px, py, r, g, b, a);
        push(x,   y  ); push(x+w, y  ); push(x,   y+h);
        push(x+w, y  ); push(x+w, y+h); push(x,   y+h);
    }

    render(buf, cursorBlink) {
        if (!this.ready) return;

        const d = this.device;
        const W = this.canvas.width;
        const H = this.canvas.height;

        // Update uniforms
        const scrollX = buf.scrollLeft;
        const scrollY = buf.scrollTop * this.glyphH;
        d.queue.writeBuffer(this.uniformBuf, 0,
            new Float32Array([W, H, scrollX, scrollY]));

        const lineH  = this.glyphH;
        const glyphW = this.glyphW;
        const lnW    = this.lineNumWidth;
        const pLeft  = this.paddingLeft;
        const pTop   = this.paddingTop;

        // Visible line range
        const firstLine = Math.floor(buf.scrollTop);
        const lastLine  = Math.min(
            buf.lineCount() - 1,
            firstLine + Math.ceil(H / lineH) + 1
        );

        const textVerts = [];
        const rectVerts = [];

        const GRAY_DIM   = [0.25, 0.25, 0.25, 1.0];
        const SEL_COLOR  = [0.27, 0.41, 0.62, 0.6];
        const CURSOR_COL = [0.90, 0.90, 0.90, 0.9];

        const sel = buf.getSelectionRange();

        for (let li = firstLine; li <= lastLine; li++) {
            const lineText = buf.lineAt(li);
            const baseY = pTop + li * lineH;

            // Selection background
            if (sel) {
                let selStart = -1, selEnd = -1;
                if (li > sel.start.line && li < sel.end.line) {
                    selStart = 0; selEnd = lineText.length;
                } else if (li === sel.start.line && li === sel.end.line) {
                    selStart = sel.start.col; selEnd = sel.end.col;
                } else if (li === sel.start.line) {
                    selStart = sel.start.col; selEnd = lineText.length;
                } else if (li === sel.end.line) {
                    selStart = 0; selEnd = sel.end.col;
                }
                if (selStart >= 0 && selEnd > selStart) {
                    const sx = pLeft + lnW + selStart * glyphW;
                    const sw = (selEnd - selStart) * glyphW;
                    this._pushRectQuad(rectVerts, sx, baseY, sw, lineH, SEL_COLOR);
                }
            }

            // Line number
            const lineNumStr = String(li + 1).padStart(3, ' ');
            for (let ci = 0; ci < lineNumStr.length; ci++) {
                const cp = lineNumStr.charCodeAt(ci);
                if (cp < 32 || cp >= 32 + GLYPH_COUNT) continue;
                const x = pLeft + ci * glyphW;
                this._pushTextQuad(textVerts, x, baseY, glyphW, lineH,
                    this._glyphUV(cp), GRAY_DIM);
            }

            // Code text with syntax highlighting
            const tokens = tokenizeLine(lineText);
            let colOffset = 0;
            for (const tok of tokens) {
                const color = TOKEN_COLOR[tok.type];
                for (let ci = 0; ci < tok.text.length; ci++) {
                    const cp = tok.text.charCodeAt(ci);
                    if (cp >= 32 && cp < 32 + GLYPH_COUNT) {
                        const x = pLeft + lnW + (colOffset + ci) * glyphW;
                        this._pushTextQuad(textVerts, x, baseY, glyphW, lineH,
                            this._glyphUV(cp), color);
                    }
                }
                colOffset += tok.text.length;
            }
        }

        // Cursor
        if (cursorBlink) {
            const { line, col } = buf.cursor;
            if (line >= firstLine && line <= lastLine) {
                const cx = pLeft + lnW + col * glyphW;
                const cy = pTop  + line * lineH;
                this._pushRectQuad(rectVerts, cx, cy, 2, lineH, CURSOR_COL);
            }
        }

        // Upload and draw
        const enc = d.createCommandEncoder();
        const view = this.context.getCurrentTexture().createView();
        const pass = enc.beginRenderPass({
            colorAttachments: [{
                view,
                clearValue: { r: 0.12, g: 0.12, b: 0.14, a: 1.0 },
                loadOp: 'clear',
                storeOp: 'store',
            }],
        });

        // Draw selection / cursor rects
        if (rectVerts.length > 0) {
            const rb = d.createBuffer({
                size: rectVerts.length * 4,
                usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
                mappedAtCreation: true,
            });
            new Float32Array(rb.getMappedRange()).set(rectVerts);
            rb.unmap();
            pass.setPipeline(this.rectPipeline);
            pass.setBindGroup(0, this.bindGroup);
            pass.setVertexBuffer(0, rb);
            pass.draw(rectVerts.length / 6); // 6 floats per vertex
        }

        // Draw text
        if (textVerts.length > 0) {
            const tb = d.createBuffer({
                size: textVerts.length * 4,
                usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
                mappedAtCreation: true,
            });
            new Float32Array(tb.getMappedRange()).set(textVerts);
            tb.unmap();
            pass.setPipeline(this.textPipeline);
            pass.setBindGroup(0, this.bindGroup);
            pass.setVertexBuffer(0, tb);
            pass.draw(textVerts.length / 8); // 8 floats per vertex
        }

        pass.end();
        d.queue.submit([enc.finish()]);
    }

    resize(w, h) {
        this.canvas.width  = w;
        this.canvas.height = h;
    }

    // Convert pixel coords to (line, col)
    hitTest(buf, px, py) {
        const lnW = this.lineNumWidth;
        const pLeft = this.paddingLeft;
        const pTop  = this.paddingTop;
        const scrollX = buf.scrollLeft;
        const scrollY = buf.scrollTop * this.glyphH;
        const line = Math.floor((py + scrollY - pTop) / this.glyphH);
        const col  = Math.floor((px + scrollX - pLeft - lnW) / this.glyphW);
        return {
            line: Math.max(0, Math.min(line, buf.lineCount() - 1)),
            col:  Math.max(0, col),
        };
    }
}
