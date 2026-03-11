// Text buffer and editor state management

export class TextBuffer {
    constructor(initial_text = '') {
        this.lines = initial_text ? initial_text.split('\n') : [''];
        this.cursor = { line: 0, col: 0 };
        this.selection = null; // { anchor, focus } each {line, col}
        this.scroll_top = 0;   // in lines
        this.scroll_left = 0;  // in pixels
        this._on_change = null;
        this._version = 0;
    }

    get version() { return this._version; }

    on_change(fn) { this._on_change = fn; }

    _dirty() {
        this._version++;
        if (this._on_change) this._on_change();
    }

    get_text() { return this.lines.join('\n'); }

    set_text(text) {
        this.lines = text.split('\n');
        this.cursor = { line: 0, col: 0 };
        this.selection = null;
        this._dirty();
    }

    line_count() { return this.lines.length; }

    line_at(i) { return i >= 0 && i < this.lines.length ? this.lines[i] : ''; }

    // Clamp cursor to valid position
    clamp_cursor(c) {
        let line = Math.max(0, Math.min(c.line, this.lines.length - 1));
        let col  = Math.max(0, Math.min(c.col, this.lines[line].length));
        return { line, col };
    }

    move_cursor(line, col, select = false) {
        const new_cursor = this.clamp_cursor({ line, col });
        if (select) {
            if (!this.selection) {
                this.selection = { anchor: { ...this.cursor }, focus: new_cursor };
            } else {
                this.selection.focus = new_cursor;
            }
        } else {
            this.selection = null;
        }
        this.cursor = new_cursor;
        this._dirty();
    }

    // Returns selection in normalized (start <= end) form
    get_selection_range() {
        if (!this.selection) return null;
        const a = this.selection.anchor;
        const b = this.selection.focus;
        const a_less = a.line < b.line || (a.line === b.line && a.col <= b.col);
        return a_less ? { start: a, end: b } : { start: b, end: a };
    }

    delete_selection() {
        const sel = this.get_selection_range();
        if (!sel) return;
        const { start, end } = sel;
        const before = this.lines[start.line].slice(0, start.col);
        const after  = this.lines[end.line].slice(end.col);
        this.lines.splice(start.line, end.line - start.line + 1, before + after);
        this.cursor = { ...start };
        this.selection = null;
    }

    // Insert text at cursor (handles newlines)
    insert_text(text) {
        if (this.selection) this.delete_selection();
        const parts = text.split('\n');
        const { line, col } = this.cursor;
        const line_text = this.lines[line] ?? '';
        const before = line_text.slice(0, col);
        const after  = line_text.slice(col);

        if (parts.length === 1) {
            this.lines[line] = before + parts[0] + after;
            this.cursor = { line, col: col + parts[0].length };
        } else {
            const new_lines = [];
            new_lines.push(before + parts[0]);
            for (let i = 1; i < parts.length - 1; i++) new_lines.push(parts[i]);
            new_lines.push(parts[parts.length - 1] + after);
            this.lines.splice(line, 1, ...new_lines);
            this.cursor = {
                line: line + parts.length - 1,
                col: parts[parts.length - 1].length
            };
        }
        this._dirty();
    }

    backspace() {
        if (this.selection) { this.delete_selection(); this._dirty(); return; }
        const { line, col } = this.cursor;
        if (col > 0) {
            this.lines[line] = this.lines[line].slice(0, col - 1) + this.lines[line].slice(col);
            this.cursor.col--;
        } else if (line > 0) {
            const prev = this.lines[line - 1];
            this.lines.splice(line - 1, 2, prev + this.lines[line]);
            this.cursor = { line: line - 1, col: prev.length };
        }
        this._dirty();
    }

    delete_forward() {
        if (this.selection) { this.delete_selection(); this._dirty(); return; }
        const { line, col } = this.cursor;
        const line_text = this.lines[line];
        if (col < line_text.length) {
            this.lines[line] = line_text.slice(0, col) + line_text.slice(col + 1);
        } else if (line < this.lines.length - 1) {
            this.lines.splice(line, 2, line_text + this.lines[line + 1]);
        }
        this._dirty();
    }

    move_left(select = false) {
        let { line, col } = this.cursor;
        if (!select && this.selection) {
            const sel = this.get_selection_range();
            this.move_cursor(sel.start.line, sel.start.col, false);
            return;
        }
        if (col > 0) col--;
        else if (line > 0) { line--; col = this.lines[line].length; }
        this.move_cursor(line, col, select);
    }

    move_right(select = false) {
        let { line, col } = this.cursor;
        if (!select && this.selection) {
            const sel = this.get_selection_range();
            this.move_cursor(sel.end.line, sel.end.col, false);
            return;
        }
        const line_text = this.lines[line];
        if (col < line_text.length) col++;
        else if (line < this.lines.length - 1) { line++; col = 0; }
        this.move_cursor(line, col, select);
    }

    move_up(select = false) {
        const { line, col } = this.cursor;
        if (line > 0) this.move_cursor(line - 1, col, select);
        else this.move_cursor(0, 0, select);
    }

    move_down(select = false) {
        const { line, col } = this.cursor;
        if (line < this.lines.length - 1) this.move_cursor(line + 1, col, select);
        else this.move_cursor(line, this.lines[line].length, select);
    }

    move_line_start(select = false) {
        this.move_cursor(this.cursor.line, 0, select);
    }

    move_line_end(select = false) {
        const { line } = this.cursor;
        this.move_cursor(line, this.lines[line].length, select);
    }

    select_all() {
        const last_line = this.lines.length - 1;
        this.selection = {
            anchor: { line: 0, col: 0 },
            focus:  { line: last_line, col: this.lines[last_line].length }
        };
        this.cursor = this.selection.focus;
        this._dirty();
    }

    // Auto-indent: return indent of previous line
    auto_indent() {
        const { line } = this.cursor;
        if (line === 0) return '';
        const prev = this.lines[line - 1];
        const match = prev.match(/^(\s*)/);
        return match ? match[1] : '';
    }
}
