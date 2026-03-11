// Immediate-mode UI system (ImGui-style).
//
// Each frame:
//   ui.beginFrame(mouseX, mouseY, mouseDown, vpW, vpH)
//   ... build UI with widget calls ...
//   ui.endFrame()  → returns DrawList
//
// Widgets return true when activated (button clicked, etc.)
//
// Color palette (all values 0..1):

export const C = {
    BG:         [0.118, 0.118, 0.141, 1.0],  // #1e1e24
    SURFACE:    [0.165, 0.165, 0.208, 1.0],  // #2a2a35
    SURFACE2:   [0.200, 0.200, 0.247, 1.0],  // slightly lighter
    BORDER:     [0.227, 0.227, 0.282, 1.0],  // #3a3a48
    TEXT:       [0.878, 0.878, 0.925, 1.0],  // #e0e0ec
    TEXT_DIM:   [0.533, 0.533, 0.533, 1.0],  // #888
    ACCENT:     [0.361, 0.561, 0.937, 1.0],  // #5c8fef
    ACCENT_DIM: [0.227, 0.416, 0.831, 1.0],  // #3a6ad4
    GREEN:      [0.298, 0.686, 0.447, 1.0],  // #4caf72
    RED:        [0.878, 0.361, 0.361, 1.0],  // #e05c5c
    YELLOW:     [0.831, 0.643, 0.298, 1.0],  // #d4a44c
    SEL:        [0.361, 0.561, 0.937, 0.25], // selection highlight
    LINE_NUM:   [0.400, 0.400, 0.450, 1.0],
    // syntax token colours
    SYN_KW:     [0.482, 0.671, 0.996, 1.0],  // blue  #7ab4ff
    SYN_TYPE:   [0.529, 0.882, 0.702, 1.0],  // teal  #87e1b3
    SYN_NUM:    [0.722, 0.886, 0.573, 1.0],  // lime  #b8e292
    SYN_STR:    [0.914, 0.737, 0.518, 1.0],  // orange #e9bc84
    SYN_CMT:    [0.420, 0.490, 0.420, 1.0],  // grey-green
    SYN_PUNCT:  [0.800, 0.800, 0.850, 1.0],
    SYN_OP:     [0.937, 0.600, 0.600, 1.0],  // pinkish
    SYN_ID:     [0.878, 0.878, 0.925, 1.0],  // same as TEXT
};

import { DrawList } from './draw.js';
import { tokenizeLine } from './syntax.js';

// Map token type → color tuple
const TOK_COLOR = {
    KEYWORD:     C.SYN_KW,
    TYPE:        C.SYN_TYPE,
    NUMBER:      C.SYN_NUM,
    STRING:      C.SYN_STR,
    COMMENT:     C.SYN_CMT,
    OPERATOR:    C.SYN_OP,
    PUNCTUATION: C.SYN_PUNCT,
    IDENTIFIER:  C.SYN_ID,
    WHITESPACE:  C.SYN_ID,
    UNKNOWN:     C.SYN_ID,
};

export class UI {
    constructor() {
        this._dl     = new DrawList();
        this._font   = null;
        this._vpW    = 0;
        this._vpH    = 0;

        // Mouse state
        this._mx     = 0;
        this._my     = 0;
        this._down   = false;
        this._justDown  = false;   // pressed this frame
        this._justUp    = false;   // released this frame

        // Scroll tracking for the output pane (managed externally via scrollRef)
        this._hotId  = '';
        this._activeId = '';
    }

    setFont(font) { this._font = font; }

    beginFrame(mx, my, down, justDown, justUp, vpW, vpH) {
        this._mx = mx; this._my = my;
        this._down = down; this._justDown = justDown; this._justUp = justUp;
        this._vpW = vpW; this._vpH = vpH;
        this._dl.clear();
        this._hotId = '';
    }

    endFrame() {
        this._dl.finalize();
        return this._dl;
    }

    get font() { return this._font; }
    get dl()   { return this._dl; }

    // ── Primitives ────────────────────────────────────────────────────────────
    drawRect(x, y, w, h, c) {
        this._dl.rect(x, y, w, h, c[0], c[1], c[2], c[3] ?? 1);
    }
    drawBorder(x, y, w, h, c, t = 1) {
        this._dl.rect(x,     y,     w, t, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x,     y+h-t, w, t, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x,     y,     t, h, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x+w-t, y,     t, h, c[0], c[1], c[2], c[3] ?? 1);
    }
    drawText(str, x, y, c) {
        this._dl.text(str, x, y, this._font, c[0], c[1], c[2], c[3] ?? 1);
    }
    drawTextClipped(str, x, y, maxW, c) {
        this._dl.textClipped(str, x, y, maxW, this._font, c[0], c[1], c[2], c[3] ?? 1);
    }

    // ── Hit test ──────────────────────────────────────────────────────────────
    _hit(x, y, w, h) {
        return this._mx >= x && this._mx < x+w && this._my >= y && this._my < y+h;
    }

    // ── Widgets ───────────────────────────────────────────────────────────────

    /**
     * Filled panel background.
     */
    panel(x, y, w, h, c = C.SURFACE) {
        this.drawRect(x, y, w, h, c);
    }

    /**
     * Horizontal separator line.
     */
    separator(x, y, w, c = C.BORDER) {
        this._dl.rect(x, y, w, 1, c[0], c[1], c[2], c[3] ?? 1);
    }

    /**
     * Label (non-interactive text).
     */
    label(text, x, y, c = C.TEXT) {
        this.drawText(text, x, y, c);
    }

    /**
     * Button. Returns true on click.
     * @param {string} id  unique widget id
     */
    button(id, label, x, y, w, h, primary = false) {
        const hot     = this._hit(x, y, w, h);
        const pressed = this._activeId === id;
        if (hot) this._hotId = id;
        if (hot && this._justDown) this._activeId = id;

        let bg, fg;
        if (primary) {
            bg = pressed ? C.ACCENT_DIM : (hot ? C.ACCENT_DIM : C.ACCENT);
            fg = C.TEXT;
        } else {
            bg = pressed ? C.BORDER : (hot ? C.SURFACE2 : C.SURFACE);
            fg = hot ? C.ACCENT : C.TEXT;
        }

        this.drawRect(x, y, w, h, bg);
        this.drawBorder(x, y, w, h, C.BORDER);

        // Center label
        const f    = this._font;
        const tw   = label.length * f.glyphW;
        const tx   = x + (w - tw) / 2;
        const ty   = y + (h - f.glyphH) / 2;
        this.drawText(label, tx, ty, fg);

        const clicked = hot && this._justUp && this._activeId === id;
        if (this._justUp && this._activeId === id) this._activeId = '';
        return clicked;
    }

    /**
     * Draggable vertical divider bar.
     * @param {string} id
     * @param {number} x   current x of bar
     * @param {number} y, h  extents
     * @returns {number}  new x position (caller must clamp + store)
     */
    divider(id, x, y, h, dragRef) {
        const hw  = 4;
        const hot = this._hit(x - hw, y, hw * 2, h);

        if (hot) this._hotId = id;
        if (hot && this._justDown) {
            this._activeId  = id;
            dragRef.startX  = this._mx;
            dragRef.startVal = dragRef.value;
        }
        if (this._activeId === id) {
            dragRef.value = dragRef.startVal + (dragRef.startX - this._mx);
            if (this._justUp) this._activeId = '';
        }

        const c = (this._activeId === id || hot) ? C.ACCENT : C.BORDER;
        this._dl.rect(x - 2, y, 4, h, c[0], c[1], c[2], 1);
    }

    /**
     * Dropdown select widget. Opens an overlay popup.
     * options: [{label, value}]
     * Returns the selected value, or null if unchanged.
     */
    select(id, options, selectedLabel, x, y, w, h) {
        const hot = this._hit(x, y, w, h);
        if (hot) this._hotId = id;

        const active = this._activeId === id;
        const bg  = hot ? C.SURFACE2 : C.SURFACE;
        this.drawRect(x, y, w, h, bg);
        this.drawBorder(x, y, w, h, C.BORDER);
        const f   = this._font;
        const ty  = y + (h - f.glyphH) / 2;
        this.drawTextClipped(selectedLabel, x + 6, ty, w - 12, C.TEXT_DIM);
        // Chevron ▾
        this.drawText('v', x + w - f.glyphW - 4, ty, C.TEXT_DIM);

        if (hot && this._justDown) {
            this._activeId = active ? '' : id;
        }

        let chosen = null;
        if (this._activeId === id) {
            // Popup
            const popH  = options.length * h;
            const popY  = y + h;
            this.drawRect(x, popY, w, popH, C.SURFACE2);
            this.drawBorder(x, popY, w, popH, C.BORDER);
            for (let i = 0; i < options.length; i++) {
                const iy  = popY + i * h;
                const iHot = this._hit(x, iy, w, h);
                if (iHot) this.drawRect(x, iy, w, h, C.ACCENT_DIM);
                this.drawTextClipped(options[i].label, x + 6, iy + (h - f.glyphH) / 2, w - 12, iHot ? C.TEXT : C.TEXT_DIM);
                if (iHot && this._justDown) {
                    chosen = options[i].value;
                    this._activeId = '';
                }
            }
        }
        return chosen;
    }

    /**
     * Scrollbar. Returns delta scrollTop if dragged.
     * @param {number} scrollTop    current scroll offset in items
     * @param {number} totalItems
     * @param {number} visibleItems
     */
    vScrollbar(id, x, y, w, h, scrollTop, totalItems, visibleItems) {
        if (totalItems <= visibleItems) return scrollTop;
        this.drawRect(x, y, w, h, C.SURFACE);

        const thumbRatio = Math.min(1, visibleItems / totalItems);
        const thumbH     = Math.max(20, h * thumbRatio);
        const trackH     = h - thumbH;
        const thumbY     = y + trackH * (scrollTop / (totalItems - visibleItems));

        const hot = this._hit(x, thumbY, w, thumbH);
        if (hot) this._hotId = id;

        const dragging = this._activeId === id;
        if (hot && this._justDown) {
            this._activeId     = id;
            this._scrollDragY  = this._my - thumbY;
            this._scrollOrig   = scrollTop;
        }

        let newScroll = scrollTop;
        if (dragging) {
            const newThumbY = this._my - this._scrollDragY;
            const t = Math.max(0, Math.min(1, (newThumbY - y) / trackH));
            newScroll = t * (totalItems - visibleItems);
            if (this._justUp) this._activeId = '';
        }

        const thumbC = (dragging || hot) ? C.ACCENT : C.BORDER;
        this.drawRect(x + 1, thumbY, w - 2, thumbH, thumbC);
        return newScroll;
    }

    /**
     * Code editor widget.
     * Handles:
     *  - background, line number gutter
     *  - selection highlight
     *  - syntax-highlighted text lines
     *  - cursor (blink controlled by caller via cursorVisible)
     *  - mouse click → cursor position
     *  - does NOT handle keyboard; caller does that via TextBuffer directly
     *
     * Returns { clickedLine, clickedCol } if user clicked (or null).
     */
    codeEditor(buf, x, y, w, h, cursorVisible, font) {
        const f    = font ?? this._font;
        const gW   = f.glyphW;
        const gH   = f.glyphH;

        const GUTTER_PAD  = 6;
        const lineCount   = buf.lineCount();
        const gutterCols  = String(lineCount).length;
        const gutterW     = gutterCols * gW + GUTTER_PAD * 2;

        // Background
        this.drawRect(x, y, w, h, C.BG);
        this.drawRect(x, y, gutterW, h, C.SURFACE);

        const visLines = Math.floor(h / gH);
        const scrollT  = Math.floor(buf.scrollTop ?? 0);
        const scrollL  = buf.scrollLeft ?? 0;
        const codeX    = x + gutterW;
        const codeW    = w - gutterW;

        // ── Scissor the code area ─────────────────────────────────────────────
        this._dl.scissor(x, y, w, h);

        // ── Selection ─────────────────────────────────────────────────────────
        const sel = buf.getSelectionRange?.();
        if (sel) {
            for (let l = sel.start.line; l <= sel.end.line; l++) {
                const vy = l - scrollT;
                if (vy < 0 || vy >= visLines) continue;
                const lineLen = buf.lineAt(l).length;
                const sc = l === sel.start.line ? sel.start.col : 0;
                const ec = l === sel.end.line   ? sel.end.col   : lineLen;
                const sx = codeX + sc * gW - scrollL;
                const sw = (ec - sc) * gW;
                if (sw > 0) {
                    this._dl.rect(sx, y + vy * gH, sw, gH, C.SEL[0], C.SEL[1], C.SEL[2], C.SEL[3]);
                }
            }
        }

        // ── Text lines ────────────────────────────────────────────────────────
        for (let vi = 0; vi < visLines + 1; vi++) {
            const li = scrollT + vi;
            if (li >= lineCount) break;
            const ty2 = y + vi * gH;
            const lineStr = buf.lineAt(li);

            // Line number
            const numStr = String(li + 1).padStart(gutterCols, ' ');
            this._dl.text(numStr, x + GUTTER_PAD, ty2, f, C.LINE_NUM[0], C.LINE_NUM[1], C.LINE_NUM[2], 1);

            // Syntax-highlighted code
            let cx = codeX - scrollL;
            const tokens = tokenizeLine(lineStr);
            for (const tok of tokens) {
                const c = TOK_COLOR[tok.type] ?? C.TEXT;
                for (let ci = 0; ci < tok.text.length; ci++) {
                    const gcx = cx + ci * gW;
                    if (gcx + gW < codeX || gcx > codeX + codeW) continue; // horizontal clip
                    const gi = f.getGlyph(tok.text[ci]);
                    if (gi && gi.w > 0 && gi.h > 0) {
                        this._dl.glyph(gcx + gi.xoff, ty2 + gi.yoff, gi.w, gi.h,
                            [gi.u0, gi.v0, gi.u1, gi.v1], c[0], c[1], c[2], 1);
                    }
                }
                cx += tok.text.length * gW;
            }
        }

        // ── Cursor ────────────────────────────────────────────────────────────
        if (cursorVisible) {
            const { line, col } = buf.cursor;
            const vy = line - scrollT;
            if (vy >= 0 && vy < visLines) {
                const cx2 = codeX + col * gW - scrollL;
                this._dl.rect(cx2, y + vy * gH, 2, gH, C.TEXT[0], C.TEXT[1], C.TEXT[2], 0.9);
            }
        }

        // ── Reset scissor ────────────────────────────────────────────────────
        this._dl.scissor(0, 0, this._vpW, this._vpH);

        // ── Scrollbar ────────────────────────────────────────────────────────
        const SBW = 8;
        const newScroll = this.vScrollbar(
            'editor-sb', x + w - SBW, y, SBW, h,
            scrollT, lineCount, visLines
        );
        buf.scrollTop = newScroll;

        // ── Hit test → cursor placement ───────────────────────────────────────
        let clickResult = null;
        if (this._justDown && this._hit(codeX, y, codeW, h)) {
            const clickedLine = Math.min(lineCount - 1,
                Math.max(0, Math.floor((this._my - y) / gH) + scrollT));
            const clickedCol  = Math.min(buf.lineAt(clickedLine).length,
                Math.max(0, Math.round((this._mx - codeX + scrollL) / gW)));
            clickResult = { line: clickedLine, col: clickedCol };
        }
        // Also handle click-drag selection (update focus on mouse move while down)
        if (this._down && !this._justDown && this._hit(codeX, y, codeW, h)) {
            const dragLine = Math.min(lineCount - 1,
                Math.max(0, Math.floor((this._my - y) / gH) + scrollT));
            const dragCol  = Math.min(buf.lineAt(dragLine).length,
                Math.max(0, Math.round((this._mx - codeX + scrollL) / gW)));
            buf.moveCursor(dragLine, dragCol, true);
        }

        return clickResult;
    }

    /**
     * Output text panel. Renders an array of {text, cls} lines.
     * Returns updated scrollTop.
     */
    outputPanel(lines, x, y, w, h, scrollTop) {
        const f    = this._font;
        const gH   = f.glyphH;
        const vis  = Math.floor(h / gH);
        const total = lines.length;

        this.drawRect(x, y, w, h, C.BG);
        this._dl.scissor(x, y, w, h);

        const clsColor = {
            print: C.TEXT,
            error: C.RED,
            info:  C.TEXT_DIM,
            sep:   C.BORDER,
            time:  C.YELLOW,
        };

        const st = Math.floor(scrollTop);
        for (let vi = 0; vi < vis + 1; vi++) {
            const li = st + vi;
            if (li >= total) break;
            const c = clsColor[lines[li].cls] ?? C.TEXT;
            const ty = y + vi * gH;
            this._dl.textClipped(lines[li].text, x + 8, ty, w - 16, f, c[0], c[1], c[2], 1);
        }

        this._dl.scissor(0, 0, this._vpW, this._vpH);

        const SBW = 8;
        return this.vScrollbar('out-sb', x + w - SBW, y, SBW, h, st, total, vis);
    }

    /**
     * Status dot indicator. cls: 'idle'|'run'|'ok'|'err'
     */
    statusDot(x, y, r, cls) {
        const c = cls === 'ok'  ? C.GREEN
                : cls === 'err' ? C.RED
                : cls === 'run' ? C.YELLOW
                :                 C.TEXT_DIM;
        this._dl.rect(x, y, r*2, r*2, c[0], c[1], c[2], 1);
    }
}
