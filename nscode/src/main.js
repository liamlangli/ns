import { TextBuffer } from './editor.js';
import { Renderer }   from './renderer.js';
import { NSInterpreter } from './interpreter.js';

// ── Examples ──────────────────────────────────────────────────────────────────

const EXAMPLES = {
  hello: `// Hello World in NanoScript
fn main() {
    println("Hello, World!")
    println("Welcome to NSCode!")
}

main()
`,

  fib: `// Fibonacci — recursive
fn fib(n: i32) i32 {
    if n == 0 {
        return 0
    } else if n == 1 {
        return 1
    } else {
        return fib(n - 1) + fib(n - 2)
    }
}

for i in 0 to 10 {
    println(fib(i))
}
`,

  factorial: `// Factorial — recursive
fn factorial(n: i32) i32 {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

for i in 0 to 13 {
    println(factorial(i))
}
`,

  loop: `// Loop and accumulate
fn sum(n: i32) i32 {
    let total: i32 = 0
    for i in 1 to n + 1 {
        total = total + i
    }
    return total
}

println(sum(10))
println(sum(100))
`,

  fizzbuzz: `// FizzBuzz 1..30
for i in 1 to 31 {
    if i % 15 == 0 {
        println("FizzBuzz")
    } else if i % 3 == 0 {
        println("Fizz")
    } else if i % 5 == 0 {
        println("Buzz")
    } else {
        println(i)
    }
}
`,

  primes: `// Sieve-like prime check
fn is_prime(n: i32) i32 {
    if n < 2 { return 0 }
    if n == 2 { return 1 }
    if n % 2 == 0 { return 0 }
    let i: i32 = 3
    for i in 3 to n {
        if i * i > n { return 1 }
        if n % i == 0 { return 0 }
    }
    return 1
}

let count: i32 = 0
for n in 2 to 50 {
    if is_prime(n) == 1 {
        println(n)
        count = count + 1
    }
}
println("Total primes < 50:")
println(count)
`,

  closure: `// Functions as values
fn make_adder(x: i32) {
    return fn(y: i32) {
        return x + y
    }
}

let add5 = make_adder(5)
let add10 = make_adder(10)

println(add5(3))
println(add10(3))
println(add5(add10(2)))
`,
};

const DEFAULT_CODE = EXAMPLES.fib;

// ── Output panel ──────────────────────────────────────────────────────────────

class OutputPanel {
    constructor(scrollEl, dotEl, statusEl) {
        this._el  = scrollEl;
        this._dot = dotEl;
        this._st  = statusEl;
    }

    _append(text, cls) {
        const p = document.createElement('p');
        p.className = `out-line ${cls}`;
        p.textContent = text;
        this._el.appendChild(p);
        this._el.scrollTop = this._el.scrollHeight;
    }

    print(text)  { this._append(String(text), 'print'); }
    error(text)  { this._append(String(text), 'error'); }
    info(text)   { this._append(String(text), 'info'); }
    sep()        { this._append('─'.repeat(40), 'sep'); }
    time(ms)     { this._append(`Finished in ${ms.toFixed(1)} ms`, 'time'); }

    clear() {
        this._el.innerHTML = '';
    }

    setStatus(state, text) {
        // state: 'idle' | 'run' | 'ok' | 'err'
        this._dot.className = '';
        if (state === 'run') this._dot.classList.add('run');
        else if (state === 'ok') this._dot.classList.add('ok');
        else if (state === 'err') this._dot.classList.add('err');
        this._st.textContent = text;
    }
}

// ── Divider drag ──────────────────────────────────────────────────────────────

function initDivider(dividerEl, outputPane) {
    let dragging = false;
    let startX = 0, startW = 0;

    dividerEl.addEventListener('mousedown', (e) => {
        dragging = true;
        startX = e.clientX;
        startW = outputPane.getBoundingClientRect().width;
        dividerEl.classList.add('dragging');
        document.body.style.cursor = 'col-resize';
        document.body.style.userSelect = 'none';
        e.preventDefault();
    });

    window.addEventListener('mousemove', (e) => {
        if (!dragging) return;
        const totalW = document.getElementById('main').getBoundingClientRect().width;
        const delta = startX - e.clientX;
        const newW = Math.max(180, Math.min(totalW * 0.65, startW + delta));
        outputPane.style.width = `${newW}px`;
    });

    window.addEventListener('mouseup', () => {
        if (!dragging) return;
        dragging = false;
        dividerEl.classList.remove('dragging');
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
    });
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function main() {
    const canvas      = document.getElementById('editor-canvas');
    const statusEl    = document.getElementById('editor-status');
    const wgpuBadge   = document.getElementById('wgpu-badge');
    const runBtn      = document.getElementById('run-btn');
    const clearBtn    = document.getElementById('clear-btn');
    const exampleSel  = document.getElementById('example-select');
    const outputPanel = new OutputPanel(
        document.getElementById('output-scroll'),
        document.getElementById('status-dot'),
        document.getElementById('run-status'),
    );

    initDivider(
        document.getElementById('divider'),
        document.getElementById('output-pane'),
    );

    const buf      = new TextBuffer(DEFAULT_CODE);
    const renderer = new Renderer(canvas);

    // ── Canvas sizing ──
    const editorPane = document.getElementById('editor-pane');
    function resizeCanvas() {
        const r = editorPane.getBoundingClientRect();
        renderer.resize(r.width, r.height);
        scheduleRender();
    }
    resizeCanvas();

    const ro = new ResizeObserver(() => resizeCanvas());
    ro.observe(editorPane);

    // ── WebGPU init ──
    let webgpuOk = false;
    try {
        await renderer.init();
        webgpuOk = true;
        wgpuBadge.textContent = 'WebGPU ✓';
        wgpuBadge.className = 'ok';
        statusEl.textContent = `Ln 1, Col 1  •  ${buf.lineCount()} lines`;
    } catch (e) {
        wgpuBadge.textContent = 'WebGPU ✗';
        wgpuBadge.className = 'err';
        statusEl.textContent = `WebGPU error: ${e.message}`;
        document.getElementById('no-webgpu').style.display = 'flex';
        console.error('WebGPU init failed:', e);
    }

    // ── Cursor blink ──
    let cursorVisible = true;
    let blinkTimer = setInterval(() => { cursorVisible = !cursorVisible; scheduleRender(); }, 530);

    function resetBlink() {
        cursorVisible = true;
        clearInterval(blinkTimer);
        blinkTimer = setInterval(() => { cursorVisible = !cursorVisible; scheduleRender(); }, 530);
    }

    // ── Render loop ──
    let rafPending = false;
    function scheduleRender() {
        if (!rafPending && webgpuOk) {
            rafPending = true;
            requestAnimationFrame(() => {
                rafPending = false;
                renderer.render(buf, cursorVisible);
                const { line, col } = buf.cursor;
                statusEl.textContent = `Ln ${line + 1}, Col ${col + 1}  •  ${buf.lineCount()} lines`;
            });
        }
    }
    buf.onChange(() => scheduleRender());

    // ── Run NS code ──
    async function runCode() {
        const src = buf.getText();
        outputPanel.clear();
        outputPanel.setStatus('run', 'Running…');
        runBtn.disabled = true;

        await new Promise(r => setTimeout(r, 0)); // let UI update

        const lines = [];
        const interp = new NSInterpreter({
            print: (v) => {
                const s = String(v);
                lines.push(s);
                outputPanel.print(s);
            },
            error: (v) => {
                outputPanel.error(String(v));
            },
        });

        const t0 = performance.now();
        let ok = true;
        try {
            interp.run(src);
        } catch (e) {
            ok = false;
            outputPanel.error(e.message ?? String(e));
        }
        const elapsed = performance.now() - t0;

        outputPanel.sep();
        outputPanel.time(elapsed);
        outputPanel.setStatus(ok ? 'ok' : 'err', ok ? 'Success' : 'Error');
        runBtn.disabled = false;
    }

    runBtn.addEventListener('click', runCode);
    clearBtn.addEventListener('click', () => {
        outputPanel.clear();
        outputPanel.setStatus('idle', 'Ready');
    });

    exampleSel.addEventListener('change', () => {
        const key = exampleSel.value;
        if (key && EXAMPLES[key]) {
            buf.setText(EXAMPLES[key]);
            scheduleRender();
        }
        exampleSel.value = '';
    });

    // ── Keyboard ──
    const hiddenInput = document.getElementById('hidden-input');
    hiddenInput.focus();

    window.addEventListener('keydown', (e) => {
        const ctrl  = e.ctrlKey || e.metaKey;
        const shift = e.shiftKey;

        // Ctrl+Enter → run
        if (ctrl && e.key === 'Enter') {
            e.preventDefault();
            runCode();
            return;
        }

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
            const curLine = buf.lineAt(buf.cursor.line);
            const before  = curLine.slice(0, buf.cursor.col).trimEnd();
            const extra   = before.endsWith('{') ? '    ' : '';
            buf.insertText('\n' + indent + extra);
            break;
        }
        case 'a': if (ctrl) { buf.selectAll(); break; } handled = false; break;
        case 'c':
            if (ctrl) {
                const sel = buf.getSelectionRange();
                if (sel) {
                    const parts = [];
                    for (let l = sel.start.line; l <= sel.end.line; l++) {
                        const ln = buf.lineAt(l);
                        const s  = l === sel.start.line ? sel.start.col : 0;
                        const en = l === sel.end.line   ? sel.end.col   : ln.length;
                        parts.push(ln.slice(s, en));
                    }
                    navigator.clipboard?.writeText(parts.join('\n'));
                }
                break;
            }
            handled = false; break;
        case 'v':
            if (ctrl) {
                navigator.clipboard?.readText().then(text => {
                    if (text) { buf.insertText(text); ensureCursorVisible(); scheduleRender(); }
                });
                break;
            }
            handled = false; break;
        case 'x':
            if (ctrl) {
                const sel2 = buf.getSelectionRange();
                if (sel2) {
                    const parts2 = [];
                    for (let l = sel2.start.line; l <= sel2.end.line; l++) {
                        const ln = buf.lineAt(l);
                        const s  = l === sel2.start.line ? sel2.start.col : 0;
                        const en = l === sel2.end.line   ? sel2.end.col   : ln.length;
                        parts2.push(ln.slice(s, en));
                    }
                    navigator.clipboard?.writeText(parts2.join('\n'));
                    buf.deleteSelection();
                }
                break;
            }
            handled = false; break;
        default: handled = false;
        }

        if (handled) {
            e.preventDefault();
            resetBlink();
            ensureCursorVisible();
        }
    });

    hiddenInput.addEventListener('input', (e) => {
        const text = e.data ?? '';
        if (text) {
            buf.insertText(text);
            resetBlink();
            ensureCursorVisible();
        }
        hiddenInput.value = '';
    });

    // ── Mouse ──
    canvas.addEventListener('mousedown', (e) => {
        hiddenInput.focus();
        const { line, col } = renderer.hitTest(buf, e.clientX, e.clientY);
        buf.moveCursor(line, col, e.shiftKey);
        resetBlink();

        const onMove = (me) => {
            const pos = renderer.hitTest(buf, me.clientX, me.clientY);
            buf.moveCursor(pos.line, pos.col, true);
            scheduleRender();
        };
        const onUp = () => {
            window.removeEventListener('mousemove', onMove);
            window.removeEventListener('mouseup', onUp);
        };
        window.addEventListener('mousemove', onMove);
        window.addEventListener('mouseup', onUp);
        e.preventDefault();
    });

    canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        buf.scrollTop  = Math.max(0, Math.min(buf.scrollTop  + e.deltaY / renderer.glyphH, buf.lineCount() - 1));
        buf.scrollLeft = Math.max(0, buf.scrollLeft + e.deltaX);
        scheduleRender();
    }, { passive: false });

    // ── Helpers ──
    function ensureCursorVisible() {
        const { line, col } = buf.cursor;
        const paneH    = editorPane.getBoundingClientRect().height;
        const visLines = Math.floor(paneH / renderer.glyphH) - 2;
        if (line < buf.scrollTop) buf.scrollTop = line;
        if (line >= buf.scrollTop + visLines) buf.scrollTop = line - visLines + 1;

        const codeX     = col * renderer.glyphW;
        const codeAreaW = editorPane.getBoundingClientRect().width - renderer.lineNumWidth - renderer.paddingLeft * 2;
        if (codeX < buf.scrollLeft) buf.scrollLeft = Math.max(0, codeX - 40);
        if (codeX + renderer.glyphW > buf.scrollLeft + codeAreaW)
            buf.scrollLeft = codeX + renderer.glyphW - codeAreaW + 40;
    }

    scheduleRender();
}

main().catch(console.error);
