import { TextBuffer } from './editor.js';
import { Renderer }   from './renderer.js';

const SAMPLE_CODE = `// NanoScript fibonacci example
use std

fn fib(n: i32) i32 {
    if n == 0 {
        return 0
    } else if n == 1 {
        return 1
    } else {
        return fib(n - 1) + fib(n - 2)
    }
}

fn sum(n: i32) i32 {
    let total: i32 = 0
    for i in 0 to n {
        total = total + i
    }
    return total
}

type Point = { x: f64, y: f64 }

fn distance(a: Point, b: Point) f64 {
    let dx = a.x - b.x
    let dy = a.y - b.y
    return dx * dx + dy * dy
}
`;

async function main() {
    const canvas = document.getElementById('editor-canvas');
    const statusEl = document.getElementById('status');

    const buf = new TextBuffer(SAMPLE_CODE);
    const renderer = new Renderer(canvas);

    // Resize canvas to window
    function resizeCanvas() {
        renderer.resize(window.innerWidth, window.innerHeight);
    }
    resizeCanvas();
    window.addEventListener('resize', () => { resizeCanvas(); scheduleRender(); });

    // Try to init WebGPU
    try {
        await renderer.init();
        statusEl.textContent = `WebGPU ready  •  ${renderer.glyphW}×${renderer.glyphH}px glyphs`;
    } catch (e) {
        statusEl.textContent = `WebGPU error: ${e.message}`;
        console.error(e);
        return;
    }

    // Cursor blink
    let cursorVisible = true;
    let blinkTimer = setInterval(() => {
        cursorVisible = !cursorVisible;
        scheduleRender();
    }, 530);

    function resetBlink() {
        cursorVisible = true;
        clearInterval(blinkTimer);
        blinkTimer = setInterval(() => {
            cursorVisible = !cursorVisible;
            scheduleRender();
        }, 530);
    }

    // Render loop
    let rafPending = false;
    function scheduleRender() {
        if (!rafPending) {
            rafPending = true;
            requestAnimationFrame(() => {
                rafPending = false;
                renderer.render(buf, cursorVisible);
                updateStatus();
            });
        }
    }

    buf.onChange(() => scheduleRender());

    // Status bar
    function updateStatus() {
        const { line, col } = buf.cursor;
        statusEl.textContent =
            `Ln ${line + 1}, Col ${col + 1}  •  ${buf.lineCount()} lines  •  WebGPU`;
    }

    // Scroll to keep cursor visible
    function ensureCursorVisible() {
        const { line, col } = buf.cursor;
        const visLines = Math.floor(window.innerHeight / renderer.glyphH) - 2;

        // Vertical
        if (line < buf.scrollTop) buf.scrollTop = line;
        if (line >= buf.scrollTop + visLines) buf.scrollTop = line - visLines + 1;

        // Horizontal (very simple)
        const codeX = col * renderer.glyphW;
        const codeAreaW = window.innerWidth - renderer.lineNumWidth - renderer.paddingLeft * 2;
        if (codeX < buf.scrollLeft) buf.scrollLeft = Math.max(0, codeX - 40);
        if (codeX + renderer.glyphW > buf.scrollLeft + codeAreaW)
            buf.scrollLeft = codeX + renderer.glyphW - codeAreaW + 40;
    }

    // Keyboard input
    window.addEventListener('keydown', (e) => {
        const ctrl  = e.ctrlKey || e.metaKey;
        const shift = e.shiftKey;
        let handled = true;

        switch (e.key) {
        case 'ArrowLeft':  buf.moveLeft(shift);      break;
        case 'ArrowRight': buf.moveRight(shift);     break;
        case 'ArrowUp':    buf.moveUp(shift);        break;
        case 'ArrowDown':  buf.moveDown(shift);      break;
        case 'Home':       buf.moveLineStart(shift); break;
        case 'End':        buf.moveLineEnd(shift);   break;
        case 'Backspace':  buf.backspace();          break;
        case 'Delete':     buf.deleteForward();      break;
        case 'Tab':
            buf.insertText('    ');
            break;
        case 'Enter': {
            const indent = buf.autoIndent();
            // Add extra indent after '{'
            const curLine = buf.lineAt(buf.cursor.line);
            const before  = curLine.slice(0, buf.cursor.col).trimEnd();
            const extra   = before.endsWith('{') ? '    ' : '';
            buf.insertText('\n' + indent + extra);
            break;
        }
        case 'a':
            if (ctrl) { buf.selectAll(); break; }
            handled = false; break;
        case 'c':
            if (ctrl) {
                const sel = buf.getSelectionRange();
                if (sel) {
                    const lines = [];
                    for (let l = sel.start.line; l <= sel.end.line; l++) {
                        const line = buf.lineAt(l);
                        const s = l === sel.start.line ? sel.start.col : 0;
                        const e2 = l === sel.end.line   ? sel.end.col   : line.length;
                        lines.push(line.slice(s, e2));
                    }
                    navigator.clipboard?.writeText(lines.join('\n'));
                }
                break;
            }
            handled = false; break;
        case 'v':
            if (ctrl) {
                navigator.clipboard?.readText().then(text => {
                    if (text) { buf.insertText(text); ensureCursorVisible(); }
                });
                break;
            }
            handled = false; break;
        case 'x':
            if (ctrl) {
                const sel2 = buf.getSelectionRange();
                if (sel2) {
                    const lines2 = [];
                    for (let l = sel2.start.line; l <= sel2.end.line; l++) {
                        const line = buf.lineAt(l);
                        const s = l === sel2.start.line ? sel2.start.col : 0;
                        const e2 = l === sel2.end.line   ? sel2.end.col   : line.length;
                        lines2.push(line.slice(s, e2));
                    }
                    navigator.clipboard?.writeText(lines2.join('\n'));
                    buf.deleteSelection();
                    buf._dirty();
                }
                break;
            }
            handled = false; break;
        case 'z':
            if (ctrl) break; // TODO: undo
            handled = false; break;
        default:
            handled = false;
        }

        if (handled) {
            e.preventDefault();
            resetBlink();
            ensureCursorVisible();
        }
    });

    // Printable character input via 'keypress' / 'input' on hidden textarea
    const hiddenInput = document.getElementById('hidden-input');
    hiddenInput.focus();

    hiddenInput.addEventListener('input', (e) => {
        const text = e.data ?? '';
        if (text) {
            buf.insertText(text);
            resetBlink();
            ensureCursorVisible();
        }
        hiddenInput.value = '';
    });

    // Keep hidden input focused when canvas is clicked
    canvas.addEventListener('mousedown', (e) => {
        hiddenInput.focus();
        const { line, col } = renderer.hitTest(buf, e.clientX, e.clientY);
        buf.moveCursor(line, col, e.shiftKey);
        resetBlink();

        // Drag selection
        const onMove = (me) => {
            const pos = renderer.hitTest(buf, me.clientX, me.clientY);
            buf.moveCursor(pos.line, pos.col, true);
        };
        const onUp = () => {
            window.removeEventListener('mousemove', onMove);
            window.removeEventListener('mouseup', onUp);
        };
        window.addEventListener('mousemove', onMove);
        window.addEventListener('mouseup', onUp);
        e.preventDefault();
    });

    // Scroll with mouse wheel
    canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        const linesDelta = e.deltaY / renderer.glyphH;
        buf.scrollTop = Math.max(0,
            Math.min(buf.scrollTop + linesDelta, buf.lineCount() - 1));
        buf.scrollLeft = Math.max(0, buf.scrollLeft + e.deltaX);
        scheduleRender();
    }, { passive: false });

    // Initial render
    scheduleRender();
}

main().catch(console.error);
