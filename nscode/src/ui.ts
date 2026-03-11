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

import { DrawList, type Rgba, type FontAtlas } from './draw.ts';
import { tokenize_line } from './syntax.ts';
import type { TextBuffer } from './editor.ts';

export const C: Record<string, Rgba> = {
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

// Map token type → color tuple
const TOK_COLOR: Record<string, Rgba> = {
    KEYWORD:     C.SYN_KW!,
    TYPE:        C.SYN_TYPE!,
    NUMBER:      C.SYN_NUM!,
    STRING:      C.SYN_STR!,
    COMMENT:     C.SYN_CMT!,
    OPERATOR:    C.SYN_OP!,
    PUNCTUATION: C.SYN_PUNCT!,
    IDENTIFIER:  C.SYN_ID!,
    WHITESPACE:  C.SYN_ID!,
    UNKNOWN:     C.SYN_ID!,
};

export interface TreeItem  { label: string; value: string; }
export interface TreeGroup { label: string; open: boolean; items: TreeItem[]; }
export interface ClickResult { line: number; col: number; }

export interface DragRef {
    value:     number;
    start_x:   number;
    start_val: number;
}

export interface OutputLine {
    text: string;
    cls:  string;
}

export class UI {
    private _dl:        DrawList;
    private _font:      FontAtlas | null;
    private _vp_w:      number;
    private _vp_h:      number;

    // Mouse state
    private _mx:        number;
    private _my:        number;
    private _down:      boolean;
    private _just_down: boolean;   // pressed this frame
    private _just_up:   boolean;   // released this frame

    // Scroll tracking for the output pane (managed externally via scroll_ref)
    private _hot_id:    string;
    private _active_id: string;

    // Scrollbar drag internals
    private _scroll_drag_y: number;
    private _scroll_orig:   number;

    constructor() {
        this._dl        = new DrawList();
        this._font      = null;
        this._vp_w      = 0;
        this._vp_h      = 0;

        this._mx        = 0;
        this._my        = 0;
        this._down      = false;
        this._just_down = false;
        this._just_up   = false;

        this._hot_id    = '';
        this._active_id = '';

        this._scroll_drag_y = 0;
        this._scroll_orig   = 0;
    }

    set_font(font: FontAtlas): void { this._font = font; }

    begin_frame(mx: number, my: number, down: boolean, just_down: boolean, just_up: boolean, vp_w: number, vp_h: number): void {
        this._mx = mx; this._my = my;
        this._down = down; this._just_down = just_down; this._just_up = just_up;
        this._vp_w = vp_w; this._vp_h = vp_h;
        this._dl.clear();
        this._hot_id = '';
    }

    end_frame(): DrawList {
        this._dl.finalize();
        return this._dl;
    }

    get font(): FontAtlas | null { return this._font; }
    get dl(): DrawList   { return this._dl; }

    // ── Primitives ────────────────────────────────────────────────────────────
    draw_rect(x: number, y: number, w: number, h: number, c: Rgba): void {
        this._dl.rect(x, y, w, h, c[0], c[1], c[2], c[3] ?? 1);
    }

    /**
     * Filled triangle. dir: 'right' (▶) or 'down' (▾).
     * cx/cy = centre, size = half-extent in pixels.
     */
    draw_triangle(cx: number, cy: number, size: number, dir: string, c: Rgba): void {
        const [r, g, b, a] = [c[0], c[1], c[2], c[3] ?? 1];
        const pts: readonly [number, number][] = dir === 'down'
            ? [ [cx - size, cy - size * 0.55],
                [cx + size, cy - size * 0.55],
                [cx,        cy + size * 0.55] ]
            : [ [cx - size * 0.55, cy - size],
                [cx + size * 0.55, cy        ],
                [cx - size * 0.55, cy + size ] ];
        this._dl.fill_convex_poly(pts, r, g, b, a);
    }

    draw_border(x: number, y: number, w: number, h: number, c: Rgba, t = 1): void {
        this._dl.rect(x,     y,     w, t, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x,     y+h-t, w, t, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x,     y,     t, h, c[0], c[1], c[2], c[3] ?? 1);
        this._dl.rect(x+w-t, y,     t, h, c[0], c[1], c[2], c[3] ?? 1);
    }

    draw_text(str: string, x: number, y: number, c: Rgba): void {
        this._dl.text(str, x, y, this._font!, c[0], c[1], c[2], c[3] ?? 1);
    }

    draw_text_clipped(str: string, x: number, y: number, max_w: number, c: Rgba): void {
        this._dl.text_clipped(str, x, y, max_w, this._font!, c[0], c[1], c[2], c[3] ?? 1);
    }

    // ── Hit test ──────────────────────────────────────────────────────────────
    private _hit(x: number, y: number, w: number, h: number): boolean {
        return this._mx >= x && this._mx < x+w && this._my >= y && this._my < y+h;
    }

    // ── Widgets ───────────────────────────────────────────────────────────────

    /**
     * Filled panel background.
     */
    panel(x: number, y: number, w: number, h: number, c: Rgba = C.SURFACE!): void {
        this.draw_rect(x, y, w, h, c);
    }

    /**
     * Horizontal separator line.
     */
    separator(x: number, y: number, w: number, c: Rgba = C.BORDER!): void {
        this._dl.rect(x, y, w, 1, c[0], c[1], c[2], c[3] ?? 1);
    }

    /**
     * Label (non-interactive text).
     */
    label(text: string, x: number, y: number, c: Rgba = C.TEXT!): void {
        this.draw_text(text, x, y, c);
    }

    /**
     * Button. Returns true on click.
     * @param id  unique widget id
     */
    button(id: string, label: string, x: number, y: number, w: number, h: number, primary = false): boolean {
        const hot     = this._hit(x, y, w, h);
        const pressed = this._active_id === id;
        if (hot) this._hot_id = id;
        if (hot && this._just_down) this._active_id = id;

        let bg: Rgba, fg: Rgba;
        if (primary) {
            bg = pressed ? C.ACCENT_DIM! : (hot ? C.ACCENT_DIM! : C.ACCENT!);
            fg = C.TEXT!;
        } else {
            bg = pressed ? C.BORDER! : (hot ? C.SURFACE2! : C.SURFACE!);
            fg = hot ? C.ACCENT! : C.TEXT!;
        }

        this.draw_rect(x, y, w, h, bg);
        this.draw_border(x, y, w, h, C.BORDER!);

        // Center label
        const f    = this._font!;
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
     * @param id
     * @param x   current x of bar
     * @param y, h  extents
     * @returns  new x position (caller must clamp + store)
     */
    divider(id: string, x: number, y: number, h: number, drag_ref: DragRef): void {
        const hw  = 4;
        const hot = this._hit(x - hw, y, hw * 2, h);

        if (hot) this._hot_id = id;
        if (hot && this._just_down) {
            this._active_id      = id;
            drag_ref.start_x     = this._mx;
            drag_ref.start_val   = drag_ref.value;
        }
        if (this._active_id === id) {
            drag_ref.value = drag_ref.start_val + (drag_ref.start_x - this._mx);
            if (this._just_up) this._active_id = '';
        }

        const c = (this._active_id === id || hot) ? C.ACCENT! : C.BORDER!;
        this._dl.rect(x - 2, y, 4, h, c[0], c[1], c[2], 1);
    }

    /**
     * Dropdown select widget. Opens an overlay popup.
     * options: [{label, value}]
     * Returns the selected value, or null if unchanged.
     */
    select(id: string, options: TreeItem[], selected_label: string, x: number, y: number, w: number, h: number): string | null {
        const hot = this._hit(x, y, w, h);
        if (hot) this._hot_id = id;

        const active = this._active_id === id;
        const bg  = hot ? C.SURFACE2! : C.SURFACE!;
        this.draw_rect(x, y, w, h, bg);
        this.draw_border(x, y, w, h, C.BORDER!);
        const f   = this._font!;
        const ty  = y + (h - f.glyph_h) / 2;
        this.draw_text_clipped(selected_label, x + 6, ty, w - 12, C.TEXT_DIM!);
        // Chevron ▾
        this.draw_text('v', x + w - f.glyph_w - 4, ty, C.TEXT_DIM!);

        if (hot && this._just_down) {
            this._active_id = active ? '' : id;
        }

        let chosen: string | null = null;
        if (this._active_id === id) {
            // Popup
            const pop_h  = options.length * h;
            const pop_y  = y + h;
            this.draw_rect(x, pop_y, w, pop_h, C.SURFACE2!);
            this.draw_border(x, pop_y, w, pop_h, C.BORDER!);
            for (let i = 0; i < options.length; i++) {
                const iy   = pop_y + i * h;
                const i_hot = this._hit(x, iy, w, h);
                if (i_hot) this.draw_rect(x, iy, w, h, C.ACCENT_DIM!);
                this.draw_text_clipped(options[i]!.label, x + 6, iy + (h - f.glyph_h) / 2, w - 12, i_hot ? C.TEXT! : C.TEXT_DIM!);
                if (i_hot && this._just_down) {
                    chosen = options[i]!.value;
                    this._active_id = '';
                }
            }
        }
        return chosen;
    }

    /**
     * Scrollbar. Returns updated scroll_top if dragged.
     * @param scroll_top    current scroll offset in items
     * @param total_items
     * @param visible_items
     */
    v_scrollbar(id: string, x: number, y: number, w: number, h: number, scroll_top: number, total_items: number, visible_items: number): number {
        if (total_items <= visible_items) return scroll_top;
        this.draw_rect(x, y, w, h, C.SURFACE!);

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

        const thumb_c = (dragging || hot) ? C.ACCENT! : C.BORDER!;
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
    code_editor(buf: TextBuffer, x: number, y: number, w: number, h: number, cursor_visible: boolean, font?: FontAtlas): ClickResult | null {
        const f    = font ?? this._font!;
        const g_w  = f.glyph_w;
        const g_h  = f.glyph_h;

        const GUTTER_PAD  = 6;
        const line_count  = buf.line_count();
        const gutter_cols = String(line_count).length;
        const gutter_w    = gutter_cols * g_w + GUTTER_PAD * 2;

        // Background
        this.draw_rect(x, y, w, h, C.BG!);
        this.draw_rect(x, y, gutter_w, h, C.SURFACE!);

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
                    this._dl.rect(sx, y + vy * g_h, sw, g_h, C.SEL![0], C.SEL![1], C.SEL![2], C.SEL![3]);
                }
            }
        }

        // ── Text lines ────────────────────────────────────────────────────────
        for (let vi = 0; vi < vis_lines + 1; vi++) {
            const li = scroll_t + vi;
            if (li >= line_count) break;
            const ty2      = y + vi * g_h;
            const line_str = buf.line_at(li);

            // Line number
            const num_str = String(li + 1).padStart(gutter_cols, ' ');
            this._dl.text(num_str, x + GUTTER_PAD, ty2, f, C.LINE_NUM![0], C.LINE_NUM![1], C.LINE_NUM![2], 1);

            // Syntax-highlighted code
            let cx = code_x - scroll_l;
            const tokens = tokenize_line(line_str);
            for (const tok of tokens) {
                const c = TOK_COLOR[tok.type] ?? C.TEXT!;
                for (let ci = 0; ci < tok.text.length; ci++) {
                    const gcx = cx + ci * g_w;
                    if (gcx + g_w < code_x || gcx > code_x + code_w) continue; // horizontal clip
                    const gi = f.get_glyph(tok.text[ci]!);
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
                this._dl.rect(cx2, y + vy * g_h, 2, g_h, C.TEXT![0], C.TEXT![1], C.TEXT![2], 0.9);
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
        let click_result: ClickResult | null = null;
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
    output_panel(lines: OutputLine[], x: number, y: number, w: number, h: number, scroll_top: number): number {
        const f    = this._font!;
        const g_h  = f.glyph_h;
        const vis  = Math.floor(h / g_h);
        const total = lines.length;

        this.draw_rect(x, y, w, h, C.BG!);
        this._dl.scissor(x, y, w, h);

        const cls_color: Record<string, Rgba> = {
            print: C.TEXT!,
            error: C.RED!,
            info:  C.TEXT_DIM!,
            sep:   C.BORDER!,
            time:  C.YELLOW!,
        };

        const st = Math.floor(scroll_top);
        for (let vi = 0; vi < vis + 1; vi++) {
            const li = st + vi;
            if (li >= total) break;
            const c = cls_color[lines[li]!.cls] ?? C.TEXT!;
            const ty = y + vi * g_h;
            this._dl.text_clipped(lines[li]!.text, x + 8, ty, w - 16, f, c[0], c[1], c[2], 1);
        }

        this._dl.scissor(0, 0, this._vp_w, this._vp_h);

        const SBW = 8;
        return this.v_scrollbar('out-sb', x + w - SBW, y, SBW, h, st, total, vis);
    }

    /**
     * Run button with a drawn play-triangle icon (no unicode glyph needed).
     * Returns true on click.
     */
    run_button(id: string, x: number, y: number, w: number, h: number): boolean {
        const hot     = this._hit(x, y, w, h);
        const pressed = this._active_id === id;
        if (hot) this._hot_id = id;
        if (hot && this._just_down) this._active_id = id;

        const bg = pressed ? C.ACCENT_DIM! : (hot ? C.ACCENT_DIM! : C.ACCENT!);
        this.draw_rect(x, y, w, h, bg);
        this.draw_border(x, y, w, h, C.BORDER!);

        const f      = this._font!;
        const label  = 'Run';
        const icon_w = 9;
        const gap    = 5;
        const total  = icon_w + gap + label.length * f.glyph_w;
        const sx     = x + (w - total) / 2;
        const mid_y  = y + h / 2;

        this.draw_triangle(sx + icon_w * 0.5, mid_y, 4, 'right', C.TEXT!);
        this.draw_text(label, sx + icon_w + gap, y + (h - f.glyph_h) / 2, C.TEXT!);

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
    file_tree(groups: TreeGroup[], active_key: string, x: number, y: number, w: number, h: number): string | null {
        const f     = this._font!;
        const row_h = Math.round(f.glyph_h + 8);
        const PAD   = 8;
        const IND   = 14;   // item indent under group header

        this.draw_rect(x, y, w, h, C.SURFACE!);
        this._dl.scissor(x, y, w, h);

        let cy     = y + 4;
        let chosen: string | null = null;

        for (const group of groups) {
            if (cy + row_h > y + h) break;

            const hot = this._hit(x, cy, w, row_h);
            if (hot) this._hot_id = 'ftg_' + group.label;
            if (hot) this.draw_rect(x, cy, w, row_h, C.SURFACE2!);
            if (hot && this._just_down) group.open = !group.open;

            // Fold arrow
            this.draw_triangle(x + PAD + 4, cy + row_h / 2, 3.5,
                group.open ? 'down' : 'right', C.TEXT_DIM!);
            this.draw_text_clipped(group.label, x + PAD + 14, cy + (row_h - f.glyph_h) / 2,
                w - PAD - 14 - 4, C.TEXT!);
            cy += row_h;

            if (group.open) {
                for (const item of group.items) {
                    if (cy + row_h > y + h) break;
                    const active = item.value === active_key;
                    const i_hot  = this._hit(x, cy, w, row_h);
                    if (i_hot) this._hot_id = 'fti_' + item.value;

                    if (active) {
                        this.draw_rect(x, cy, w, row_h, C.SEL!);
                        // Accent left bar
                        this._dl.rect(x, cy, 2, row_h,
                            C.ACCENT![0], C.ACCENT![1], C.ACCENT![2], 1);
                    } else if (i_hot) {
                        this.draw_rect(x, cy, w, row_h, C.SURFACE2!);
                    }

                    this.draw_text_clipped(item.label,
                        x + PAD + IND, cy + (row_h - f.glyph_h) / 2,
                        w - PAD - IND - 4,
                        active ? C.ACCENT! : C.TEXT!);

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
    status_dot(x: number, y: number, r: number, cls: string): void {
        const c = cls === 'ok'  ? C.GREEN!
                : cls === 'err' ? C.RED!
                : cls === 'run' ? C.YELLOW!
                :                 C.TEXT_DIM!;
        this._dl.rect(x, y, r*2, r*2, c[0], c[1], c[2], 1);
    }
}
