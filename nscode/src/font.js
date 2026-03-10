// SDF Font Atlas
// Renders ASCII glyphs via OffscreenCanvas, computes signed distance field
// using the Felzenszwalb separable 1-D EDT, uploads as r8unorm GPU texture.

const FIRST = 32, LAST = 126;
const COUNT = LAST - FIRST + 1;          // 95 printable ASCII chars
const COLS  = 16;
const ROWS  = Math.ceil(COUNT / COLS);   // 6

/**
 * @param {GPUDevice} device
 * @param {object}    opts
 * @returns {{ texture, sampler, glyphW, glyphH, atlasW, atlasH, getUV(ch) }}
 */
export async function buildFontAtlas(device, {
    fontFamily = '"Courier New", Courier, monospace',
    renderSize = 28,       // px, rasterisation size
    sdfSpread  = 8,        // SDF spread in pixels (determines max blur)
} = {}) {

    // ── 1. Measure monospace advance ─────────────────────────────────────────
    const probe = new OffscreenCanvas(1, 1);
    const pc    = probe.getContext('2d');
    pc.font = `${renderSize}px ${fontFamily}`;
    const advance = pc.measureText('M').width;  // monospace: all chars same
    const lineH   = Math.ceil(renderSize * 1.5);

    // cell dims (padded for SDF bleed)
    const pad   = sdfSpread + 2;
    const cellW = Math.ceil(advance) + pad * 2;
    const cellH = lineH + pad * 2;
    const atlasW = COLS * cellW;
    const atlasH = ROWS * cellH;

    // ── 2. Rasterise glyphs ──────────────────────────────────────────────────
    const canvas = new OffscreenCanvas(atlasW, atlasH);
    const ctx    = canvas.getContext('2d', { willReadFrequently: true });
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, atlasW, atlasH);
    ctx.fillStyle = '#fff';
    ctx.font = `${renderSize}px ${fontFamily}`;
    ctx.textBaseline = 'alphabetic';

    const ascent = Math.ceil(renderSize * 0.8);
    const uvs    = new Float32Array(COUNT * 4); // u0,v0,u1,v1 per glyph

    for (let i = 0; i < COUNT; i++) {
        const ch  = String.fromCharCode(FIRST + i);
        const col = i % COLS;
        const row = Math.floor(i / COLS);
        const gx  = col * cellW + pad;
        const gy  = row * cellH + pad + ascent;
        ctx.fillText(ch, gx, gy);

        const u0 = (col * cellW) / atlasW;
        const v0 = (row * cellH) / atlasH;
        const u1 = ((col + 1) * cellW) / atlasW;
        const v1 = ((row + 1) * cellH) / atlasH;
        uvs[i * 4]     = u0;
        uvs[i * 4 + 1] = v0;
        uvs[i * 4 + 2] = u1;
        uvs[i * 4 + 3] = v1;
    }

    // ── 3. Compute SDF ───────────────────────────────────────────────────────
    const imgData = ctx.getImageData(0, 0, atlasW, atlasH);
    const raw     = imgData.data;

    const inside  = new Float32Array(atlasW * atlasH);
    const outside = new Float32Array(atlasW * atlasH);

    for (let i = 0; i < atlasW * atlasH; i++) {
        const on    = raw[i * 4] > 64;   // red channel threshold
        inside[i]   = on ? 0.0 : 1e10;
        outside[i]  = on ? 1e10 : 0.0;
    }

    edt2d(inside,  atlasW, atlasH);
    edt2d(outside, atlasW, atlasH);

    const sdfBytes = new Uint8Array(atlasW * atlasH);
    for (let i = 0; i < atlasW * atlasH; i++) {
        const on   = raw[i * 4] > 64;
        const dist = on
            ? +Math.sqrt(outside[i])    // inside: positive dist to edge
            : -Math.sqrt(inside[i]);    // outside: negative dist to edge
        const v = 0.5 + dist * 0.5 / sdfSpread;
        sdfBytes[i] = Math.round(Math.max(0, Math.min(1, v)) * 255);
    }

    // ── 4. Upload to GPU ─────────────────────────────────────────────────────
    const texture = device.createTexture({
        label: 'sdf-atlas',
        size:  [atlasW, atlasH],
        format: 'r8unorm',
        usage:  GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    device.queue.writeTexture(
        { texture },
        sdfBytes,
        { bytesPerRow: atlasW },
        [atlasW, atlasH],
    );

    const sampler = device.createSampler({
        label:     'sdf-sampler',
        magFilter: 'linear',
        minFilter: 'linear',
    });

    return {
        texture, sampler,
        glyphW: cellW, glyphH: cellH,
        atlasW, atlasH,
        renderSize, advance,
        /** @param {string} ch  @returns {Float32Array(4)} [u0,v0,u1,v1] */
        getUV(ch) {
            const cp = ch.codePointAt(0) ?? 63;
            const i  = (cp >= FIRST && cp <= LAST) ? cp - FIRST : 63 - FIRST; // '?' fallback
            return uvs.subarray(i * 4, i * 4 + 4);
        },
    };
}

// ── Felzenszwalb separable EDT ────────────────────────────────────────────────
// Computes in-place squared Euclidean distance transform.
// 0-valued cells are "seeds"; others get their sq-dist to nearest seed.

function edt2d(grid, w, h) {
    const tmp = new Float32Array(Math.max(w, h));
    const v   = new Int32Array(Math.max(w, h));
    const z   = new Float32Array(Math.max(w, h) + 1);

    for (let y = 0; y < h; y++) {
        const off = y * w;
        for (let x = 0; x < w; x++) tmp[x] = grid[off + x];
        edt1d(tmp, w, v, z);
        for (let x = 0; x < w; x++) grid[off + x] = tmp[x];
    }
    for (let x = 0; x < w; x++) {
        const col = new Float32Array(h);
        for (let y = 0; y < h; y++) col[y] = grid[y * w + x];
        edt1d(col, h, v, z);
        for (let y = 0; y < h; y++) grid[y * w + x] = col[y];
    }
}

function edt1d(f, n, v, z) {
    const INF = 1e10;
    let k = 0;
    v[0] = 0; z[0] = -INF; z[1] = INF;

    for (let q = 1; q < n; q++) {
        if (f[q] >= INF) continue;
        let s = 0;
        do {
            const r = v[k];
            s = ((f[q] + q * q) - (f[r] + r * r)) / (2 * q - 2 * r);
            if (s > z[k]) break;
            k--;
        } while (k >= 0);
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INF;
    }

    k = 0;
    const d = new Float32Array(n);
    for (let q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        const r    = v[k];
        const diff = q - r;
        d[q] = f[r] >= INF ? INF : diff * diff + f[r];
    }
    for (let q = 0; q < n; q++) f[q] = d[q];
}
