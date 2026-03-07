// Text buffer and editor state management

export class TextBuffer {
    constructor(initialText = '') {
        this.lines = initialText ? initialText.split('\n') : [''];
        this.cursor = { line: 0, col: 0 };
        this.selection = null; // { anchor, focus } each {line, col}
        this.scrollTop = 0;   // in lines
        this.scrollLeft = 0;  // in pixels
        this._onChange = null;
        this._version = 0;
    }

    get version() { return this._version; }

    onChange(fn) { this._onChange = fn; }

    _dirty() {
        this._version++;
        if (this._onChange) this._onChange();
    }

    getText() { return this.lines.join('\n'); }

    setText(text) {
        this.lines = text.split('\n');
        this.cursor = { line: 0, col: 0 };
        this.selection = null;
        this._dirty();
    }

    lineCount() { return this.lines.length; }

    lineAt(i) { return i >= 0 && i < this.lines.length ? this.lines[i] : ''; }

    // Clamp cursor to valid position
    clampCursor(c) {
        let line = Math.max(0, Math.min(c.line, this.lines.length - 1));
        let col  = Math.max(0, Math.min(c.col, this.lines[line].length));
        return { line, col };
    }

    moveCursor(line, col, select = false) {
        const newCursor = this.clampCursor({ line, col });
        if (select) {
            if (!this.selection) {
                this.selection = { anchor: { ...this.cursor }, focus: newCursor };
            } else {
                this.selection.focus = newCursor;
            }
        } else {
            this.selection = null;
        }
        this.cursor = newCursor;
        this._dirty();
    }

    // Returns selection in normalized (start <= end) form
    getSelectionRange() {
        if (!this.selection) return null;
        const a = this.selection.anchor;
        const b = this.selection.focus;
        const aLess = a.line < b.line || (a.line === b.line && a.col <= b.col);
        return aLess ? { start: a, end: b } : { start: b, end: a };
    }

    deleteSelection() {
        const sel = this.getSelectionRange();
        if (!sel) return;
        const { start, end } = sel;
        const before = this.lines[start.line].slice(0, start.col);
        const after  = this.lines[end.line].slice(end.col);
        this.lines.splice(start.line, end.line - start.line + 1, before + after);
        this.cursor = { ...start };
        this.selection = null;
    }

    // Insert text at cursor (handles newlines)
    insertText(text) {
        if (this.selection) this.deleteSelection();
        const parts = text.split('\n');
        const { line, col } = this.cursor;
        const lineText = this.lines[line] ?? '';
        const before = lineText.slice(0, col);
        const after  = lineText.slice(col);

        if (parts.length === 1) {
            this.lines[line] = before + parts[0] + after;
            this.cursor = { line, col: col + parts[0].length };
        } else {
            const newLines = [];
            newLines.push(before + parts[0]);
            for (let i = 1; i < parts.length - 1; i++) newLines.push(parts[i]);
            newLines.push(parts[parts.length - 1] + after);
            this.lines.splice(line, 1, ...newLines);
            this.cursor = {
                line: line + parts.length - 1,
                col: parts[parts.length - 1].length
            };
        }
        this._dirty();
    }

    backspace() {
        if (this.selection) { this.deleteSelection(); this._dirty(); return; }
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

    deleteForward() {
        if (this.selection) { this.deleteSelection(); this._dirty(); return; }
        const { line, col } = this.cursor;
        const lineText = this.lines[line];
        if (col < lineText.length) {
            this.lines[line] = lineText.slice(0, col) + lineText.slice(col + 1);
        } else if (line < this.lines.length - 1) {
            this.lines.splice(line, 2, lineText + this.lines[line + 1]);
        }
        this._dirty();
    }

    moveLeft(select = false) {
        let { line, col } = this.cursor;
        if (!select && this.selection) {
            const sel = this.getSelectionRange();
            this.moveCursor(sel.start.line, sel.start.col, false);
            return;
        }
        if (col > 0) col--;
        else if (line > 0) { line--; col = this.lines[line].length; }
        this.moveCursor(line, col, select);
    }

    moveRight(select = false) {
        let { line, col } = this.cursor;
        if (!select && this.selection) {
            const sel = this.getSelectionRange();
            this.moveCursor(sel.end.line, sel.end.col, false);
            return;
        }
        const lineText = this.lines[line];
        if (col < lineText.length) col++;
        else if (line < this.lines.length - 1) { line++; col = 0; }
        this.moveCursor(line, col, select);
    }

    moveUp(select = false) {
        const { line, col } = this.cursor;
        if (line > 0) this.moveCursor(line - 1, col, select);
        else this.moveCursor(0, 0, select);
    }

    moveDown(select = false) {
        const { line, col } = this.cursor;
        if (line < this.lines.length - 1) this.moveCursor(line + 1, col, select);
        else this.moveCursor(line, this.lines[line].length, select);
    }

    moveLineStart(select = false) {
        this.moveCursor(this.cursor.line, 0, select);
    }

    moveLineEnd(select = false) {
        const { line } = this.cursor;
        this.moveCursor(line, this.lines[line].length, select);
    }

    selectAll() {
        const lastLine = this.lines.length - 1;
        this.selection = {
            anchor: { line: 0, col: 0 },
            focus:  { line: lastLine, col: this.lines[lastLine].length }
        };
        this.cursor = this.selection.focus;
        this._dirty();
    }

    // Auto-indent: return indent of previous line
    autoIndent() {
        const { line } = this.cursor;
        if (line === 0) return '';
        const prev = this.lines[line - 1];
        const match = prev.match(/^(\s*)/);
        return match ? match[1] : '';
    }
}
