// Draw list: accumulates colored rects and SDF-text quads each frame.
// The GPU backend consumes this to build vertex buffers and render.

const RECT_FLOATS = 6;   // x,y, r,g,b,a  per vertex × 6 verts/rect
const TEXT_FLOATS = 8;   // x,y, u,v, r,g,b,a  per vertex × 6 verts/glyph

export class DrawList {
    constructor() {
        this._rects = new Float32Array(1024 * RECT_FLOATS * 6);
        this._texts = new Float32Array(1024 * TEXT_FLOATS * 6);
        this._rcnt  = 0;  // rect vertex count
        this._tcnt  = 0;  // text vertex count
        this._cmds  = []; // [{type:'scissor',x,y,w,h}|{type:'flush',rcnt,tcnt}]
        this._rbase = 0;
        this._tbase = 0;
    }

    clear() {
        this._rcnt = 0; this._tcnt = 0;
        this._cmds.length = 0;
        this._rbase = 0; this._tbase = 0;
    }

    // Set GPU scissor rect (pixels, top-left origin).
    // Call before drawing clipped content; pass null to reset.
    scissor(x, y, w, h) {
        this._flush_cmd();
        this._cmds.push({ type: 'scissor', x: x|0, y: y|0, w: Math.max(1, w|0), h: Math.max(1, h|0) });
        this._rbase = this._rcnt;
        this._tbase = this._tcnt;
    }

    reset_scissor(vp_w, vp_h) {
        this.scissor(0, 0, vp_w, vp_h);
    }

    // Filled axis-aligned rect. Color components 0..1.
    rect(x, y, w, h, r, g, b, a = 1) {
        this._grow_rects(6);
        const x1 = x + w, y1 = y + h;
        const base = this._rcnt * RECT_FLOATS;
        const d = this._rects;
        // 2 triangles, CCW
        d[base +  0]=x;  d[base +  1]=y;  d[base +  2]=r; d[base +  3]=g; d[base +  4]=b; d[base +  5]=a;
        d[base +  6]=x1; d[base +  7]=y;  d[base +  8]=r; d[base +  9]=g; d[base + 10]=b; d[base + 11]=a;
        d[base + 12]=x;  d[base + 13]=y1; d[base + 14]=r; d[base + 15]=g; d[base + 16]=b; d[base + 17]=a;
        d[base + 18]=x1; d[base + 19]=y;  d[base + 20]=r; d[base + 21]=g; d[base + 22]=b; d[base + 23]=a;
        d[base + 24]=x1; d[base + 25]=y1; d[base + 26]=r; d[base + 27]=g; d[base + 28]=b; d[base + 29]=a;
        d[base + 30]=x;  d[base + 31]=y1; d[base + 32]=r; d[base + 33]=g; d[base + 34]=b; d[base + 35]=a;
        this._rcnt += 6;
    }

    // Draw a single glyph quad. uv = [u0,v0,u1,v1]
    glyph(x, y, w, h, uv, r, g, b, a = 1) {
        this._grow_texts(6);
        const x1 = x + w, y1 = y + h;
        const u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
        const base = this._tcnt * TEXT_FLOATS;
        const d = this._texts;
        d[base +  0]=x;  d[base +  1]=y;  d[base +  2]=u0; d[base +  3]=v0; d[base +  4]=r; d[base +  5]=g; d[base +  6]=b; d[base +  7]=a;
        d[base +  8]=x1; d[base +  9]=y;  d[base + 10]=u1; d[base + 11]=v0; d[base + 12]=r; d[base + 13]=g; d[base + 14]=b; d[base + 15]=a;
        d[base + 16]=x;  d[base + 17]=y1; d[base + 18]=u0; d[base + 19]=v1; d[base + 20]=r; d[base + 21]=g; d[base + 22]=b; d[base + 23]=a;
        d[base + 24]=x1; d[base + 25]=y;  d[base + 26]=u1; d[base + 27]=v0; d[base + 28]=r; d[base + 29]=g; d[base + 30]=b; d[base + 31]=a;
        d[base + 32]=x1; d[base + 33]=y1; d[base + 34]=u1; d[base + 35]=v1; d[base + 36]=r; d[base + 37]=g; d[base + 38]=b; d[base + 39]=a;
        d[base + 40]=x;  d[base + 41]=y1; d[base + 42]=u0; d[base + 43]=v1; d[base + 44]=r; d[base + 45]=g; d[base + 46]=b; d[base + 47]=a;
        this._tcnt += 6;
    }

    // Draw a string at pixel (x, y) — top-left of glyph cell.
    // Font must expose get_glyph(ch) → {u0,v0,u1,v1,xoff,yoff,w,h} and glyph_w.
    // Returns x position after the last glyph.
    text(str, x, y, font, r, g, b, a = 1) {
        let cx = x;
        for (let i = 0; i < str.length; i++) {
            const gi = font.get_glyph(str[i]);
            if (gi && gi.w > 0 && gi.h > 0) {
                this.glyph(cx + gi.xoff, y + gi.yoff, gi.w, gi.h,
                    [gi.u0, gi.v0, gi.u1, gi.v1], r, g, b, a);
            }
            cx += font.glyph_w;
        }
        return cx;
    }

    // Draw string clipped to max_w pixels (measured in glyph_w cells). Returns end x.
    text_clipped(str, x, y, max_w, font, r, g, b, a = 1) {
        let cx = x;
        const xmax = x + max_w;
        for (let i = 0; i < str.length; i++) {
            if (cx + font.glyph_w > xmax) break;
            const gi = font.get_glyph(str[i]);
            if (gi && gi.w > 0 && gi.h > 0) {
                this.glyph(cx + gi.xoff, y + gi.yoff, gi.w, gi.h,
                    [gi.u0, gi.v0, gi.u1, gi.v1], r, g, b, a);
            }
            cx += font.glyph_w;
        }
        return cx;
    }

    // ── Internal ──────────────────────────────────────────────────────────────
    _flush_cmd() {
        const r_delta = this._rcnt - this._rbase;
        const t_delta = this._tcnt - this._tbase;
        if (r_delta > 0 || t_delta > 0) {
            this._cmds.push({ type: 'draw', rbase: this._rbase, rcnt: r_delta, tbase: this._tbase, tcnt: t_delta });
        }
    }

    finalize() {
        this._flush_cmd();
    }

    _grow_rects(need) {
        while ((this._rcnt + need) * RECT_FLOATS > this._rects.length) {
            const n = new Float32Array(this._rects.length * 2);
            n.set(this._rects);
            this._rects = n;
        }
    }

    _grow_texts(need) {
        while ((this._tcnt + need) * TEXT_FLOATS > this._texts.length) {
            const n = new Float32Array(this._texts.length * 2);
            n.set(this._texts);
            this._texts = n;
        }
    }

    get rect_data() { return this._rects.subarray(0, this._rcnt * RECT_FLOATS); }
    get text_data() { return this._texts.subarray(0, this._tcnt * TEXT_FLOATS); }
    get commands() { return this._cmds; }
}
