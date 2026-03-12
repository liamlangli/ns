// Immediate-mode UI system (ImGui-style).
//
// Each frame:
//   ui.begin_frame(mouse_x, mouse_y, mouse_down, vp_w, vp_h)
//   ... build UI with widget calls ...
//   ui.end_frame()  → returns DrawList
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
import { tokenize_line } from './syntax.js';

// Map token type (numeric) → color tuple. Order matches TT enum: 0..9
const TOK_COLOR = [
    C.SYN_KW,    // 0 KEYWORD
    C.SYN_TYPE,  // 1 TYPE
    C.SYN_NUM,   // 2 NUMBER
    C.SYN_STR,   // 3 STRING
    C.SYN_CMT,   // 4 COMMENT
    C.SYN_OP,    // 5 OPERATOR
    C.SYN_ID,    // 6 IDENTIFIER
    C.SYN_PUNCT, // 7 PUNCTUATION
    C.SYN_ID,    // 8 WHITESPACE
    C.SYN_ID,    // 9 UNKNOWN
];

export class UI {
    constructor() {
        this._dl       = new DrawList();
        this._font     = null;
        this._vp_w     = 0;
        this._vp_h     = 0;

        // Mouse state
        this._mx       = 0;
        this._my       = 0;
        this._down     = false;
        this._just_down = false;   // pressed this frame
        this._just_up   = false;   // released this frame

        // Scroll tracking for the output pane (managed externally via scroll_ref)
        this._hot_id   = '';
        this._active_id = '';
    }

    set_font(font) { this._font = font; }

    begin_frame(mx, my, down, just_down, just_up, vp_w, vp_h) {
        this._mx = mx; this._my = my;
        this._down = down; this._just_down = just_down; this._just_up = just_up;
        this._vp_w = vp_w; this._vp_h = vp_h;
        this._dl.clear();
        this._hot_id = '';
    }

    end_frame() {
        this._dl.finalize();
        return this._dl;
    }

    /**
     * Start an overlay pass. Like begin_frame but does NOT clear the draw list,
     * so overlays are composited on top of what was already drawn.
     * Call this after the main begin_frame / widget pass, before drawing overlays.
     */
    begin_overlay(mx, my, down, just_down, just_up) {
        this._mx = mx; this._my = my;
        this._down = down; this._just_down = just_down; this._just_up = just_up;
        this._hot_id = '';
    }

    get font() { return this._font; }
    get dl()   { return this._dl; }

    // ── Primitives ────────────────────────────────────────────────────────────
    draw_rect(x, y, w, h, c) {
        this._dl.rect(x, y, w, h, c[0], c[1], c[2], c[3] ?? 1);
    }

    /**
     * Filled triangle. dir: 'right' (▶) or 'down' (▾).
     * cx/cy = centre, size = half-extent in pixels.
     */
    draw_triangle(cx, cy, size, dir, c) {
        const [r, g, b, a] = [c[0], c[1], c[2], c[3] ?? 1];
        const pts = dir === 'down'
            ? [ [cx - size, cy - size * 0.55],
                [cx + size, cy - size * 0.55],
                [cx,        cy + size * 0.55] ]
            : [ [cx - size * 0.55, cy - size],
                [cx + size * 0.55, cy        ],
                [cx - size * 0.55, cy + size ] ];
        this._dl.fill_convex_poly(pts, r, g, b, a);
    }
    draw_border(x, y, w, h, c, t = 1) {
        this._dl.rect(x,     y,     w, t, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x,     y+h-t, w, t, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x,     y,     t, h, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x+w-t, y,     t, h, c[0], c[1], c[2], c[3] ?? 1);
    }
    draw_text(str, x, y, c) {
        this._dl.text(str, x, y, this._font, c[0], c[1], c[2], c[3] ?? 1);
    }
    draw_text_clipped(str, x, y, max_w, c) {
        this._dl.text_clipped(str, x, y, max_w, this._font, c[0], c[1], c[2], c[3] ?? 1);
    }

    // ── Hit test ──────────────────────────────────────────────────────────────
    _hit(x, y, w, h) {
        return this._mx >= x && this._mx < x+w && this._my >= y && this._my < y+h;
    }

    // ── Widgets ───────────────────────────────────────────────────────────────

    /**
     * Draw a styled text field box (no keyboard logic — caller owns the string).
     * focused: true draws accent border + blinking cursor at end of value.
     */
    _draw_text_field(x, y, w, h, value, focused, placeholder = '') {
        const f = this._font, gw = f.glyph_w, gh = f.glyph_h;
        this.draw_rect(x, y, w, h, focused ? C.BG : C.SURFACE);
        this.draw_border(x, y, w, h, focused ? C.ACCENT : C.BORDER);
        const pad = 6, ty = y + (h - gh) / 2;
        if (value.length === 0 && placeholder) {
            this.draw_text_clipped(placeholder, x + pad, ty, w - pad * 2, C.TEXT_DIM);
        } else {
            const max_ch = Math.floor((w - pad * 2) / gw);
            const vis    = value.length > max_ch ? value.slice(-max_ch) : value;
            this.draw_text_clipped(vis, x + pad, ty, w - pad * 2, C.TEXT);
        }
        if (focused) {
            const max_ch = Math.floor((w - 12) / gw);
            const cx = x + 6 + Math.min(value.length, max_ch) * gw;
            this._dl.rect(cx, ty, 2, gh, C.ACCENT[0], C.ACCENT[1], C.ACCENT[2], 0.9);
        }
    }

    /**
     * Filled panel background.
     */
    panel(x, y, w, h, c = C.SURFACE) {
        this.draw_rect(x, y, w, h, c);
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
        this.draw_text(text, x, y, c);
    }

    /**
     * Button. Returns true on click.
     * @param {string} id  unique widget id
     */
    button(id, label, x, y, w, h, primary = false) {
        const hot     = this._hit(x, y, w, h);
        const pressed = this._active_id === id;
        if (hot) this._hot_id = id;
        if (hot && this._just_down) this._active_id = id;

        let bg, fg;
        if (primary) {
            bg = pressed ? C.ACCENT_DIM : (hot ? C.ACCENT_DIM : C.ACCENT);
            fg = C.TEXT;
        } else {
            bg = pressed ? C.BORDER : (hot ? C.SURFACE2 : C.SURFACE);
            fg = hot ? C.ACCENT : C.TEXT;
        }

        this.draw_rect(x, y, w, h, bg);
        this.draw_border(x, y, w, h, C.BORDER);

        // Center label
        const f    = this._font;
        const tw   = label.length * f.glyph_w;
        const tx   = x + (w - tw) / 2;
        const ty   = y + (h - f.glyph_h) / 2;
        this.draw_text(label, tx, ty, fg);

        const clicked = hot && this._just_up && this._active_id === id;
        if (this._just_up && this._active_id === id) this._active_id = '';
        return clicked;
    }

    /**
     * Draggable vertical divider bar.
     * @param {string} id
     * @param {number} x   current x of bar
     * @param {number} y, h  extents
     * @returns {number}  new x position (caller must clamp + store)
     */
    divider(id, x, y, h, drag_ref, reverse = false) {
        const hw  = 4;
        const hot = this._hit(x - hw, y, hw * 2, h);

        if (hot) this._hot_id = id;
        if (hot && this._just_down) {
            this._active_id      = id;
            drag_ref.start_x     = this._mx;
            drag_ref.start_val   = drag_ref.value;
        }
        if (this._active_id === id) {
            const delta = reverse
                ? (this._mx - drag_ref.start_x)
                : (drag_ref.start_x - this._mx);
            drag_ref.value = drag_ref.start_val + delta;
            if (this._just_up) this._active_id = '';
        }

        const c = (this._active_id === id || hot) ? C.ACCENT : C.BORDER;
        this._dl.rect(x - 2, y, 4, h, c[0], c[1], c[2], 1);
    }

    /**
     * Dropdown select widget. Opens an overlay popup.
     * options: [{label, value}]
     * Returns the selected value, or null if unchanged.
     */
    select(id, options, selected_label, x, y, w, h) {
        const hot = this._hit(x, y, w, h);
        if (hot) this._hot_id = id;

        const active = this._active_id === id;
        const bg  = hot ? C.SURFACE2 : C.SURFACE;
        this.draw_rect(x, y, w, h, bg);
        this.draw_border(x, y, w, h, C.BORDER);
        const f   = this._font;
        const ty  = y + (h - f.glyph_h) / 2;
        this.draw_text_clipped(selected_label, x + 6, ty, w - 12, C.TEXT_DIM);
        // Chevron ▾
        this.draw_text('v', x + w - f.glyph_w - 4, ty, C.TEXT_DIM);

        if (hot && this._just_down) {
            this._active_id = active ? '' : id;
        }

        let chosen = null;
        if (this._active_id === id) {
            // Popup
            const pop_h  = options.length * h;
            const pop_y  = y + h;
            this.draw_rect(x, pop_y, w, pop_h, C.SURFACE2);
            this.draw_border(x, pop_y, w, pop_h, C.BORDER);
            for (let i = 0; i < options.length; i++) {
                const iy   = pop_y + i * h;
                const i_hot = this._hit(x, iy, w, h);
                if (i_hot) this.draw_rect(x, iy, w, h, C.ACCENT_DIM);
                this.draw_text_clipped(options[i].label, x + 6, iy + (h - f.glyph_h) / 2, w - 12, i_hot ? C.TEXT : C.TEXT_DIM);
                if (i_hot && this._just_down) {
                    chosen = options[i].value;
                    this._active_id = '';
                }
            }
        }
        return chosen;
    }

    /**
     * Scrollbar. Returns updated scroll_top if dragged.
     * @param {number} scroll_top    current scroll offset in items
     * @param {number} total_items
     * @param {number} visible_items
     */
    v_scrollbar(id, x, y, w, h, scroll_top, total_items, visible_items) {
        if (total_items <= visible_items) return scroll_top;
        this.draw_rect(x, y, w, h, C.SURFACE);

        const thumb_ratio = Math.min(1, visible_items / total_items);
        const thumb_h     = Math.max(20, h * thumb_ratio);
        const track_h     = h - thumb_h;
        const thumb_y     = y + track_h * (scroll_top / (total_items - visible_items));

        const hot = this._hit(x, thumb_y, w, thumb_h);
        if (hot) this._hot_id = id;

        const dragging = this._active_id === id;
        if (hot && this._just_down) {
            this._active_id      = id;
            this._scroll_drag_y  = this._my - thumb_y;
            this._scroll_orig    = scroll_top;
        }

        let new_scroll = scroll_top;
        if (dragging) {
            const new_thumb_y = this._my - this._scroll_drag_y;
            const t = Math.max(0, Math.min(1, (new_thumb_y - y) / track_h));
            new_scroll = t * (total_items - visible_items);
            if (this._just_up) this._active_id = '';
        }

        const thumb_c = (dragging || hot) ? C.ACCENT : C.BORDER;
        this.draw_rect(x + 1, thumb_y, w - 2, thumb_h, thumb_c);
        return new_scroll;
    }

    /**
     * Code editor widget.
     * Handles:
     *  - background, line number gutter
     *  - selection highlight
     *  - syntax-highlighted text lines
     *  - cursor (blink controlled by caller via cursor_visible)
     *  - mouse click → cursor position
     *  - does NOT handle keyboard; caller does that via TextBuffer directly
     *
     * Returns { line, col } if user clicked (or null).
     */
    code_editor(buf, x, y, w, h, cursor_visible, font, find_matches = null) {
        const f    = font ?? this._font;
        const g_w  = f.glyph_w;
        const g_h  = f.glyph_h;

        const GUTTER_PAD  = 6;
        const line_count  = buf.line_count();
        const gutter_cols = String(line_count).length;
        const gutter_w    = gutter_cols * g_w + GUTTER_PAD * 2;

        // Background
        this.draw_rect(x, y, w, h, C.BG);
        this.draw_rect(x, y, gutter_w, h, C.SURFACE);

        const vis_lines = Math.floor(h / g_h);
        const scroll_t  = Math.floor(buf.scroll_top ?? 0);
        const scroll_l  = buf.scroll_left ?? 0;
        const code_x    = x + gutter_w;
        const code_w    = w - gutter_w;

        // ── Scissor the code area ─────────────────────────────────────────────
        this._dl.scissor(x, y, w, h);

        // ── Selection ─────────────────────────────────────────────────────────
        const sel = buf.get_selection_range?.();
        if (sel) {
            for (let l = sel.start.line; l <= sel.end.line; l++) {
                const vy = l - scroll_t;
                if (vy < 0 || vy >= vis_lines) continue;
                const line_len = buf.line_at(l).length;
                const sc = l === sel.start.line ? sel.start.col : 0;
                const ec = l === sel.end.line   ? sel.end.col   : line_len;
                const sx = code_x + sc * g_w - scroll_l;
                const sw = (ec - sc) * g_w;
                if (sw > 0) {
                    this._dl.rect(sx, y + vy * g_h, sw, g_h, C.SEL[0], C.SEL[1], C.SEL[2], C.SEL[3]);
                }
            }
        }

        // ── Find match highlights ─────────────────────────────────────────────
        if (find_matches && find_matches.length) {
            for (const m of find_matches) {
                const vy = m.line - scroll_t;
                if (vy < 0 || vy >= vis_lines) continue;
                const mx2 = code_x + m.col * g_w - scroll_l;
                const mw  = m.len * g_w;
                this._dl.rect(mx2, y + vy * g_h, mw, g_h,
                    C.YELLOW[0], C.YELLOW[1], C.YELLOW[2], 0.28);
            }
        }

        // ── Text lines ────────────────────────────────────────────────────────
        for (let vi = 0; vi < vis_lines + 1; vi++) {
            const li = scroll_t + vi;
            if (li >= line_count) break;
            const ty2     = y + vi * g_h;
            const line_str = buf.line_at(li);

            // Line number
            const num_str = String(li + 1).padStart(gutter_cols, ' ');
            this._dl.text(num_str, x + GUTTER_PAD, ty2, f, C.LINE_NUM[0], C.LINE_NUM[1], C.LINE_NUM[2], 1);

            // Syntax-highlighted code
            let cx = code_x - scroll_l;
            const tokens = tokenize_line(line_str);
            for (const tok of tokens) {
                const c = TOK_COLOR[tok.type] ?? C.TEXT;
                for (let ci = 0; ci < tok.text.length; ci++) {
                    const gcx = cx + ci * g_w;
                    if (gcx + g_w < code_x || gcx > code_x + code_w) continue; // horizontal clip
                    const gi = f.get_glyph(tok.text[ci]);
                    if (gi && gi.w > 0 && gi.h > 0) {
                        this._dl.glyph(gcx + gi.xoff, ty2 + gi.yoff, gi.w, gi.h,
                            [gi.u0, gi.v0, gi.u1, gi.v1], c[0], c[1], c[2], 1);
                    }
                }
                cx += tok.text.length * g_w;
            }
        }

        // ── Cursor ────────────────────────────────────────────────────────────
        if (cursor_visible) {
            const { line, col } = buf.cursor;
            const vy = line - scroll_t;
            if (vy >= 0 && vy < vis_lines) {
                const cx2 = code_x + col * g_w - scroll_l;
                this._dl.rect(cx2, y + vy * g_h, 2, g_h, C.TEXT[0], C.TEXT[1], C.TEXT[2], 0.9);
            }
        }

        // ── Reset scissor ────────────────────────────────────────────────────
        this._dl.scissor(0, 0, this._vp_w, this._vp_h);

        // ── Scrollbar ────────────────────────────────────────────────────────
        const SBW = 8;
        const new_scroll = this.v_scrollbar(
            'editor-sb', x + w - SBW, y, SBW, h,
            scroll_t, line_count, vis_lines
        );
        buf.scroll_top = new_scroll;

        // ── Hit test → cursor placement ───────────────────────────────────────
        let click_result = null;
        if (this._just_down && this._hit(code_x, y, code_w, h)) {
            const clicked_line = Math.min(line_count - 1,
                Math.max(0, Math.floor((this._my - y) / g_h) + scroll_t));
            const clicked_col  = Math.min(buf.line_at(clicked_line).length,
                Math.max(0, Math.round((this._mx - code_x + scroll_l) / g_w)));
            click_result = { line: clicked_line, col: clicked_col };
        }
        // Also handle click-drag selection (update focus on mouse move while down)
        if (this._down && !this._just_down && this._hit(code_x, y, code_w, h)) {
            const drag_line = Math.min(line_count - 1,
                Math.max(0, Math.floor((this._my - y) / g_h) + scroll_t));
            const drag_col  = Math.min(buf.line_at(drag_line).length,
                Math.max(0, Math.round((this._mx - code_x + scroll_l) / g_w)));
            buf.move_cursor(drag_line, drag_col, true);
        }

        return click_result;
    }

    /**
     * Output text panel. Renders an array of {text, cls} lines.
     * Returns updated scroll_top.
     */
    output_panel(lines, x, y, w, h, scroll_top) {
        void lines;
        void scroll_top;

        this.draw_rect(x, y, w, h, C.BG);
        return 0;
    }

    /**
     * Run button with a drawn play-triangle icon (no unicode glyph needed).
     * Returns true on click.
     */
    run_button(id, x, y, w, h) {
        const hot     = this._hit(x, y, w, h);
        const pressed = this._active_id === id;
        if (hot) this._hot_id = id;
        if (hot && this._just_down) this._active_id = id;

        const bg = pressed ? C.ACCENT_DIM : (hot ? C.ACCENT_DIM : C.ACCENT);
        this.draw_rect(x, y, w, h, bg);
        this.draw_border(x, y, w, h, C.BORDER);

        const f      = this._font;
        const label  = 'Run';
        const icon_w = 9;
        const gap    = 5;
        const total  = icon_w + gap + label.length * f.glyph_w;
        const sx     = x + (w - total) / 2;
        const mid_y  = y + h / 2;

        this.draw_triangle(sx + icon_w * 0.5, mid_y, 4, 'right', C.TEXT);
        this.draw_text(label, sx + icon_w + gap, y + (h - f.glyph_h) / 2, C.TEXT);

        const clicked = hot && this._just_up && this._active_id === id;
        if (this._just_up && this._active_id === id) this._active_id = '';
        return clicked;
    }

    /**
     * Foldable file-tree panel.
     * groups: Array<{ label: string, open: bool, items: [{label, value}] }>
     * active_key: currently loaded item value.
     * Returns clicked item value, or null.
     */
    file_tree(groups, active_key, x, y, w, h) {
        const f     = this._font;
        const row_h = Math.round(f.glyph_h + 8);
        const PAD   = 8;
        const IND   = 14;   // item indent under group header

        this.draw_rect(x, y, w, h, C.SURFACE);
        this._dl.scissor(x, y, w, h);

        let cy     = y + 4;
        let chosen = null;

        for (const group of groups) {
            if (cy + row_h > y + h) break;

            const hot = this._hit(x, cy, w, row_h);
            if (hot) this._hot_id = 'ftg_' + group.label;
            if (hot) this.draw_rect(x, cy, w, row_h, C.SURFACE2);
            if (hot && this._just_down) group.open = !group.open;

            // Fold arrow
            this.draw_triangle(x + PAD + 4, cy + row_h / 2, 3.5,
                group.open ? 'down' : 'right', C.TEXT_DIM);
            this.draw_text_clipped(group.label, x + PAD + 14, cy + (row_h - f.glyph_h) / 2,
                w - PAD - 14 - 4, C.TEXT);
            cy += row_h;

            if (group.open) {
                for (const item of group.items) {
                    if (cy + row_h > y + h) break;
                    const active = item.value === active_key;
                    const i_hot  = this._hit(x, cy, w, row_h);
                    if (i_hot) this._hot_id = 'fti_' + item.value;

                    if (active) {
                        this.draw_rect(x, cy, w, row_h, C.SEL);
                        // Accent left bar
                        this._dl.rect(x, cy, 2, row_h,
                            C.ACCENT[0], C.ACCENT[1], C.ACCENT[2], 1);
                    } else if (i_hot) {
                        this.draw_rect(x, cy, w, row_h, C.SURFACE2);
                    }

                    this.draw_text_clipped(item.label,
                        x + PAD + IND, cy + (row_h - f.glyph_h) / 2,
                        w - PAD - IND - 4,
                        active ? C.ACCENT : C.TEXT);

                    if (i_hot && this._just_up) chosen = item.value;
                    cy += row_h;
                }
            }
        }

        this._dl.scissor(0, 0, this._vp_w, this._vp_h);
        return chosen;
    }

    /**
     * Status dot indicator. cls: 'idle'|'run'|'ok'|'err'
     */
    status_dot(x, y, r, cls) {
        const c = cls === 'ok'  ? C.GREEN
                : cls === 'err' ? C.RED
                : cls === 'run' ? C.YELLOW
                :                 C.TEXT_DIM;
        this._dl.rect(x, y, r*2, r*2, c[0], c[1], c[2], 1);
    }

    /**
     * Command palette overlay (rendered last, covers everything with dimmed backdrop).
     * cmds:  [{id, label, category, hint}] — already fuzzy-filtered and sorted.
     * query: current search string.
     * sel:   highlighted row index.
     * Returns the id of the clicked/activated command, or null.
     */
    command_palette(query, cmds, sel) {
        const f   = this._font, gw = f.glyph_w, gh = f.glyph_h;
        const PAL_W     = Math.min(560, this._vp_w - 80);
        const px        = Math.round((this._vp_w - PAL_W) / 2);
        const py        = 68;
        const ROW_H     = gh + 14;
        const INPUT_H   = 40;
        const MAX_VIS   = Math.min(cmds.length, 9);
        const PAL_H     = INPUT_H + MAX_VIS * ROW_H + 8;

        // Dimmed backdrop — click outside palette to close
        this._dl.rect(0, 0, this._vp_w, this._vp_h, 0, 0, 0, 0.5);
        if (this._just_down && !this._hit(px, py, PAL_W, PAL_H)) return '__close__';
        // Drop shadow
        this._dl.rect(px + 4, py + 4, PAL_W, PAL_H, 0, 0, 0, 0.4);
        // Panel
        this.draw_rect(px, py, PAL_W, PAL_H, C.SURFACE2);
        this.draw_border(px, py, PAL_W, PAL_H, C.ACCENT);

        // Input row
        const IPAD = 8;
        this._draw_text_field(px + IPAD, py + IPAD, PAL_W - IPAD * 2, INPUT_H - IPAD * 2,
            query, true, 'Type a command or example…');

        this.separator(px, py + INPUT_H - 1, PAL_W, C.BORDER);

        let chosen = null;
        for (let i = 0; i < MAX_VIS; i++) {
            const cmd = cmds[i];
            const ry  = py + INPUT_H + i * ROW_H;
            const hot = this._hit(px, ry, PAL_W, ROW_H);
            const active = i === sel;

            if (active) {
                this.draw_rect(px, ry, PAL_W, ROW_H, [C.ACCENT[0], C.ACCENT[1], C.ACCENT[2], 0.15]);
                this._dl.rect(px, ry, 2, ROW_H, C.ACCENT[0], C.ACCENT[1], C.ACCENT[2], 1);
            } else if (hot) {
                this.draw_rect(px, ry, PAL_W, ROW_H, C.SURFACE);
            }

            // Category chip
            const cat = cmd.category ? '[' + cmd.category + '] ' : '';
            this.draw_text(cat, px + 14, ry + (ROW_H - gh) / 2, C.TEXT_DIM);
            // Label
            this.draw_text_clipped(cmd.label,
                px + 14 + cat.length * gw, ry + (ROW_H - gh) / 2,
                PAL_W - 100 - cat.length * gw,
                active ? C.ACCENT : C.TEXT);
            // Key hint
            if (cmd.hint) {
                const hw = cmd.hint.length * gw;
                this.draw_text(cmd.hint, px + PAL_W - hw - 14, ry + (ROW_H - gh) / 2, C.TEXT_DIM);
            }

            if (hot && this._just_up) chosen = cmd.id;
        }
        return chosen;
    }

    /**
     * Find / Replace bar floating at top-right of the editor pane.
     * state: { query, replace, replace_mode, focus:'q'|'r' }
     * match_count: number of current matches (shown as badge).
     * Returns: 'next'|'prev'|'replace'|'replace_all'|'close' or null.
     */
    find_bar(state, ex, ey, ew, match_count) {
        const f = this._font, gw = f.glyph_w, gh = f.glyph_h;
        const BTN   = 24;
        const GAP   = 4;
        const ROWS  = state.replace_mode ? 2 : 1;
        const FH    = 24;   // field height
        const W     = Math.min(380, ew - 16);
        const H     = 10 + ROWS * (FH + GAP) - GAP + 10;
        const x     = ex + ew - W - 6;
        const y     = ey + 6;

        // Shadow + panel
        this._dl.rect(x + 3, y + 3, W, H, 0, 0, 0, 0.35);
        this.draw_rect(x, y, W, H, C.SURFACE2);
        this.draw_border(x, y, W, H, C.BORDER);

        let action = null;
        const FIELD_W = W - BTN * 2 - GAP * 3 - 16;

        // ── Find row ────────────────────────────────────────────────────────
        const fy = y + 8;
        const focused_q = state.focus === 'q';
        this._draw_text_field(x + 8, fy, FIELD_W, FH, state.query, focused_q, 'Find…');
        if (this._just_down && this._hit(x + 8, fy, FIELD_W, FH)) state.focus = 'q';

        // Match badge inside field
        const badge   = match_count > 0 ? String(match_count) : (state.query ? '✕' : '');
        const badge_c = match_count > 0 ? C.GREEN : C.TEXT_DIM;
        if (badge) this.draw_text(badge, x + 8 + FIELD_W - badge.length * gw - 4, fy + (FH - gh) / 2, badge_c);

        // ↑ ↓ buttons
        const b1x = x + 8 + FIELD_W + GAP;
        if (this.button('fb-prev', '\u2191', b1x,           fy, BTN, FH)) action = 'prev';
        if (this.button('fb-next', '\u2193', b1x + BTN + GAP, fy, BTN, FH)) action = 'next';

        // × close
        if (this.button('fb-cls', '\u00d7', x + W - 18, y + 2, 16, 16)) action = 'close';

        // ── Replace row ─────────────────────────────────────────────────────
        if (state.replace_mode) {
            const ry = fy + FH + GAP;
            const focused_r = state.focus === 'r';
            this._draw_text_field(x + 8, ry, FIELD_W, FH, state.replace, focused_r, 'Replace…');
            if (this._just_down && this._hit(x + 8, ry, FIELD_W, FH)) state.focus = 'r';
            if (this.button('fb-rep',  '1\u00d7', b1x,           ry, BTN, FH)) action = 'replace';
            if (this.button('fb-repa', 'All',     b1x + BTN + GAP, ry, BTN, FH)) action = 'replace_all';
        }

        return action;
    }

    /**
     * Minimal go-to-line overlay centred at the top of the editor pane.
     * query: digit string. line_count: total lines (shown as hint).
     * Caller handles Enter (navigate) and Escape (close).
     */
    goto_overlay(query, ex, ey, ew, line_count) {
        const f  = this._font, gw = f.glyph_w, gh = f.glyph_h;
        const W  = 240, H = 40;
        const x  = ex + Math.round((ew - W) / 2);
        const y  = ey + 6;
        this._dl.rect(x + 3, y + 3, W, H, 0, 0, 0, 0.35);
        this.draw_rect(x, y, W, H, C.SURFACE2);
        this.draw_border(x, y, W, H, C.BORDER);
        const label = 'Go to line:';
        const lw = label.length * gw;
        this.draw_text(label, x + 8, y + (H - gh) / 2, C.TEXT_DIM);
        const fw = W - lw - 24;
        this._draw_text_field(x + 8 + lw + 4, y + 8, fw, H - 16, query, true, '1');
        const hint = '/ ' + line_count;
        this.draw_text(hint, x + 8 + lw + 4 + (query.length + 1) * gw + 4,
            y + (H - gh) / 2, C.TEXT_DIM);
    }

    /**
     * Keybinding settings overlay. Shows all commands with their effective bindings.
     * bindings: [{ id, label, category, keys: string[] }]
     * edit_state: { id: string|null }  — which command is being re-bound.
     * Returns: { close: bool }
     */
    keybindings_overlay(bindings, edit_state) {
        const f = this._font, gw = f.glyph_w, gh = f.glyph_h;
        const W = Math.min(520, this._vp_w - 60);
        const H = Math.min(this._vp_h - 80, bindings.length * (gh + 10) + 60);
        const x = Math.round((this._vp_w - W) / 2);
        const y = Math.round((this._vp_h - H) / 2);
        const ROW_H = gh + 10;

        this._dl.rect(0, 0, this._vp_w, this._vp_h, 0, 0, 0, 0.5);
        if (this._just_down && !this._hit(x, y, W, H)) return true; // close
        this._dl.rect(x + 4, y + 4, W, H, 0, 0, 0, 0.4);
        this.draw_rect(x, y, W, H, C.SURFACE2);
        this.draw_border(x, y, W, H, C.BORDER);

        // Header
        const HDR = 36;
        this.draw_rect(x, y, W, HDR, C.SURFACE);
        this.draw_text('Keyboard Shortcuts', x + 12, y + (HDR - gh) / 2, C.TEXT);
        let closed = false;
        if (this.button('kb-close', '\u00d7', x + W - 28, y + 6, 22, 22)) closed = true;
        this.separator(x, y + HDR - 1, W, C.BORDER);

        this._dl.scissor(x, y + HDR, W, H - HDR);
        const vis_rows = Math.floor((H - HDR) / ROW_H);
        const max_show = Math.min(bindings.length, vis_rows);
        for (let i = 0; i < max_show; i++) {
            const b  = bindings[i];
            const ry = y + HDR + i * ROW_H;
            if (i % 2 === 0) this.draw_rect(x, ry, W, ROW_H, C.SURFACE);
            // Category
            this.draw_text('[' + b.category + ']', x + 10, ry + (ROW_H - gh) / 2, C.TEXT_DIM);
            // Label
            this.draw_text_clipped(b.label, x + 90, ry + (ROW_H - gh) / 2, W - 200, C.TEXT);
            // Key
            const key_str = b.keys.length ? b.keys[0] : '—';
            const editing  = edit_state.id === b.id;
            const kw = key_str.length * gw + 12;
            this.draw_rect(x + W - kw - 8, ry + 2, kw, ROW_H - 4,
                editing ? C.ACCENT_DIM : C.SURFACE);
            this.draw_text(key_str, x + W - kw - 2, ry + (ROW_H - gh) / 2,
                editing ? C.TEXT : C.TEXT_DIM);
            if (this._just_down && this._hit(x + W - kw - 8, ry + 2, kw, ROW_H - 4)) {
                edit_state.id = editing ? null : b.id;
            }
        }
        this._dl.scissor(0, 0, this._vp_w, this._vp_h);

        return closed;
    }
}
