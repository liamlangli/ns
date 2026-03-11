// NSCode — GPU-rendered code playground (no HTML/CSS UI elements)
// All UI rendered via MSDF text + rect pipelines on a single WebGPU canvas.

import { TextBuffer }    from './editor.js';
import { NSInterpreter } from './interpreter.js';
import { loadMsdfFont }  from './font.js';
import { GPU }           from './gpu.js';
import { UI, C }         from './ui.js';

// ── Layout constants ──────────────────────────────────────────────────────────
const TOOLBAR_H = 44;
const STATUS_H  = 26;
const DIVIDER_W = 4;
const BTN_W     = 80;
const BTN_H     = 30;
const BTN_PAD   = 7;   // top padding inside toolbar

// ── Example programs ──────────────────────────────────────────────────────────
const EXAMPLES = {
    hello: `// Hello World
fn main() {
    println("Hello, World!")
    println("Welcome to NSCode!")
}
main()
`,
    fib: `// Fibonacci
fn fib(n: i32) i32 {
    if n == 0 { return 0 }
    else if n == 1 { return 1 }
    else { return fib(n - 1) + fib(n - 2) }
}
for i in 0 to 10 {
    println(fib(i))
}
`,
    factorial: `// Factorial
fn factorial(n: i32) i32 {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
for i in 0 to 13 { println(factorial(i)) }
`,
    loop: `// Loop & Sum
fn sum(n: i32) i32 {
    let total: i32 = 0
    for i in 1 to n + 1 { total = total + i }
    return total
}
println(sum(10))
println(sum(100))
`,
    fizzbuzz: `// FizzBuzz
for i in 1 to 31 {
    if i % 15 == 0 { println("FizzBuzz") }
    else if i % 3 == 0 { println("Fizz") }
    else if i % 5 == 0 { println("Buzz") }
    else { println(i) }
}
`,
    primes: `// Primes < 50
fn is_prime(n: i32) i32 {
    if n < 2 { return 0 }
    if n == 2 { return 1 }
    if n % 2 == 0 { return 0 }
    for i in 3 to n {
        if i * i > n { return 1 }
        if n % i == 0 { return 0 }
    }
    return 1
}
for n in 2 to 50 {
    if is_prime(n) == 1 { println(n) }
}
`,
    closure: `// Closures
fn make_adder(x: i32) {
    return fn(y: i32) { return x + y }
}
let add5  = make_adder(5)
let add10 = make_adder(10)
println(add5(3))
println(add10(3))
println(add5(add10(2)))
`,
};

const EXAMPLE_LIST = [
    { label: '— Examples —',  value: '' },
    { label: 'Hello World',   value: 'hello' },
    { label: 'Fibonacci',     value: 'fib' },
    { label: 'Factorial',     value: 'factorial' },
    { label: 'Loop & Sum',    value: 'loop' },
    { label: 'FizzBuzz',      value: 'fizzbuzz' },
    { label: 'Primes',        value: 'primes' },
    { label: 'Closures',      value: 'closure' },
];

// ── App state ─────────────────────────────────────────────────────────────────
async function main() {
    if (!navigator.gpu) {
        document.body.style.cssText = 'margin:0;background:#1e1e24;color:#e05c5c;display:flex;align-items:center;justify-content:center;height:100vh;font:16px monospace;text-align:center';
        document.body.innerHTML = '<div><h2 style="color:#f0a0a0">WebGPU not available</h2><p>Requires Chrome 113+ or Edge 113+</p></div>';
        return;
    }

    const canvas  = document.getElementById('c');
    const hiddenInput = document.getElementById('hi');
    hiddenInput.focus();

    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('No WebGPU adapter');
    const device  = await adapter.requestDevice();
    const atlas   = await loadMsdfFont(device, {
        jsonUrl:     './public/fonts/jetbrains-mono-latin-400-normal.json',
        pngUrl:      './public/fonts/jetbrains-mono.png',
        displaySize: 20,   // scale the 48px atlas to ~20px cap-height
    });

    const gpu = new GPU(canvas);
    await gpu.init(device, atlas);

    const ui  = new UI();
    ui.setFont(atlas);

    const buf = new TextBuffer(EXAMPLES.fib);

    // Output state
    let outLines     = [];  // [{text, cls}]
    let outScroll    = 0;
    let runStatus    = 'idle';  // 'idle'|'run'|'ok'|'err'
    let runStatusMsg = 'Ready';

    // Divider (outputPane width = divRef.value, starts at 40% of window)
    const divRef = { value: 0, startX: 0, startVal: 0 };

    // Cursor blink
    let cursorVisible = true;
    let blinkT = 0;

    // Mouse state
    let mx = 0, my = 0, mouseDown = false, justDown = false, justUp = false;
    let vpW = 0, vpH = 0;

    // Viewport sizing
    function resize() {
        vpW = window.innerWidth;
        vpH = window.innerHeight;
        gpu.resize(vpW, vpH);
        if (divRef.value === 0) divRef.value = Math.round(vpW * 0.4);
    }
    resize();
    window.addEventListener('resize', resize);

    // ── Run NS code ──────────────────────────────────────────────────────────
    async function runCode() {
        outLines = [];
        outScroll = 0;
        runStatus = 'run';
        runStatusMsg = 'Running…';

        await new Promise(r => setTimeout(r, 0));
        const interp = new NSInterpreter({
            print: v => outLines.push({ text: String(v), cls: 'print' }),
            error: v => outLines.push({ text: String(v), cls: 'error' }),
        });
        const t0 = performance.now();
        let ok = true;
        try { interp.run(buf.getText()); }
        catch (e) { ok = false; outLines.push({ text: e.message ?? String(e), cls: 'error' }); }
        const ms = performance.now() - t0;
        outLines.push({ text: '─'.repeat(36), cls: 'sep' });
        outLines.push({ text: `Finished in ${ms.toFixed(1)} ms`, cls: 'time' });
        runStatus    = ok ? 'ok' : 'err';
        runStatusMsg = ok ? 'Success' : 'Error';
        outScroll    = Math.max(0, outLines.length - 10);
    }

    // ── Keyboard ─────────────────────────────────────────────────────────────
    window.addEventListener('keydown', e => {
        const ctrl = e.ctrlKey || e.metaKey;
        const shift = e.shiftKey;
        if (ctrl && e.key === 'Enter') { e.preventDefault(); runCode(); return; }
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
        case 'Tab':        buf.insertText('    ');   break;
        case 'Enter': {
            const indent = buf.autoIndent();
            const before = buf.lineAt(buf.cursor.line).slice(0, buf.cursor.col).trimEnd();
            buf.insertText('\n' + indent + (before.endsWith('{') ? '    ' : ''));
            break;
        }
        case 'a': if (ctrl) buf.selectAll(); else handled = false; break;
        case 'c': if (ctrl) { copySelection(); } else handled = false; break;
        case 'v': if (ctrl) { navigator.clipboard?.readText().then(t => { if (t) buf.insertText(t); }); } else handled = false; break;
        case 'x': if (ctrl) { copySelection(); buf.deleteSelection(); } else handled = false; break;
        default:  handled = false;
        }
        if (handled) { e.preventDefault(); blinkT = 0; cursorVisible = true; }
    });

    function copySelection() {
        const sel = buf.getSelectionRange();
        if (!sel) return;
        const parts = [];
        for (let l = sel.start.line; l <= sel.end.line; l++) {
            const ln = buf.lineAt(l);
            const s  = l === sel.start.line ? sel.start.col : 0;
            const en = l === sel.end.line   ? sel.end.col   : ln.length;
            parts.push(ln.slice(s, en));
        }
        navigator.clipboard?.writeText(parts.join('\n'));
    }

    hiddenInput.addEventListener('input', e => {
        const t = e.data ?? '';
        if (t) { buf.insertText(t); blinkT = 0; cursorVisible = true; }
        hiddenInput.value = '';
    });

    // ── Mouse ────────────────────────────────────────────────────────────────
    canvas.addEventListener('mousedown', e => {
        hiddenInput.focus();
        mx = e.clientX; my = e.clientY;
        mouseDown = true; justDown = true;
        e.preventDefault();
    });
    window.addEventListener('mousemove', e => { mx = e.clientX; my = e.clientY; });
    window.addEventListener('mouseup',   e => {
        mx = e.clientX; my = e.clientY;
        mouseDown = false; justUp = true;
    });

    canvas.addEventListener('wheel', e => {
        e.preventDefault();
        const mainH    = vpH - TOOLBAR_H - STATUS_H;
        const outW     = Math.max(200, Math.min(vpW * 0.65, divRef.value));
        const editorW  = vpW - outW - DIVIDER_W;

        // Determine which pane the wheel is over
        if (e.clientX < editorW) {
            buf.scrollTop = Math.max(0, Math.min((buf.scrollTop ?? 0) + e.deltaY / atlas.glyphH, buf.lineCount() - 1));
            buf.scrollLeft = Math.max(0, (buf.scrollLeft ?? 0) + e.deltaX);
        } else {
            outScroll = Math.max(0, Math.min(outScroll + e.deltaY / atlas.glyphH, Math.max(0, outLines.length - 1)));
        }
    }, { passive: false });

    // ── Frame loop ───────────────────────────────────────────────────────────
    let last = 0;
    function frame(ts) {
        requestAnimationFrame(frame);

        const dt = ts - last; last = ts;

        // Cursor blink
        blinkT += dt;
        if (blinkT > 530) { blinkT = 0; cursorVisible = !cursorVisible; }

        // Layout
        const mainH   = vpH - TOOLBAR_H - STATUS_H;
        const outW    = Math.max(200, Math.min(Math.round(vpW * 0.65), divRef.value));
        const editorW = vpW - outW - DIVIDER_W;

        // Begin ImGui frame
        ui.beginFrame(mx, my, mouseDown, justDown, justUp, vpW, vpH);

        // ── Toolbar ───────────────────────────────────────────────────────────
        ui.panel(0, 0, vpW, TOOLBAR_H, C.SURFACE);
        ui.separator(0, TOOLBAR_H - 1, vpW, C.BORDER);

        let tbx = 12;
        // Brand
        ui.drawText('NSCode', tbx, (TOOLBAR_H - atlas.glyphH) / 2, C.ACCENT);
        tbx += 7 * atlas.glyphW + 8;

        // Separator
        ui._dl.rect(tbx, BTN_PAD, 1, BTN_H, C.BORDER[0], C.BORDER[1], C.BORDER[2], 1);
        tbx += 9;

        // Run button
        if (ui.button('run', '\u25b6 Run', tbx, BTN_PAD, BTN_W, BTN_H, true)) {
            runCode();
        }
        tbx += BTN_W + 6;

        // Clear button
        if (ui.button('clear', 'Clear', tbx, BTN_PAD, BTN_W, BTN_H)) {
            outLines = []; outScroll = 0; runStatus = 'idle'; runStatusMsg = 'Ready';
        }
        tbx += BTN_W + 8;

        // Separator
        ui._dl.rect(tbx, BTN_PAD, 1, BTN_H, C.BORDER[0], C.BORDER[1], C.BORDER[2], 1);
        tbx += 9;

        // Example dropdown
        const exampleW = 16 * atlas.glyphW;
        const chosen = ui.select('examples', EXAMPLE_LIST, '— Examples —', tbx, BTN_PAD, exampleW, BTN_H);
        if (chosen) { buf.setText(EXAMPLES[chosen]); blinkT = 0; cursorVisible = true; }
        tbx += exampleW + 16;

        // Hint (right-aligned)
        const hint = 'Ctrl+Enter to run';
        const hintX = vpW - hint.length * atlas.glyphW - 12;
        ui.drawText(hint, hintX, (TOOLBAR_H - atlas.glyphH) / 2, C.TEXT_DIM);

        // ── Editor pane ───────────────────────────────────────────────────────
        const hit = ui.codeEditor(buf, 0, TOOLBAR_H, editorW, mainH, cursorVisible, atlas);
        if (hit) {
            buf.moveCursor(hit.line, hit.col, false);
            blinkT = 0; cursorVisible = true;
        }

        // ── Divider ───────────────────────────────────────────────────────────
        const divX = editorW;
        ui.divider('div', divX, TOOLBAR_H, mainH, divRef);

        // ── Output header ─────────────────────────────────────────────────────
        const outX    = divX + DIVIDER_W;
        const HDR_H   = 34;
        ui.panel(outX, TOOLBAR_H, outW, HDR_H, C.SURFACE);
        ui.separator(outX, TOOLBAR_H + HDR_H - 1, outW, C.BORDER);
        const dotY = TOOLBAR_H + (HDR_H - 8) / 2;
        ui.statusDot(outX + 10, dotY, 4, runStatus);
        ui.drawText('Output', outX + 26, TOOLBAR_H + (HDR_H - atlas.glyphH) / 2, C.TEXT);
        const msgX = outX + 26 + 8 * atlas.glyphW;
        ui.drawText(runStatusMsg, msgX, TOOLBAR_H + (HDR_H - atlas.glyphH) / 2, C.TEXT_DIM);

        // ── Output body ───────────────────────────────────────────────────────
        const outBodyY = TOOLBAR_H + HDR_H;
        const outBodyH = mainH - HDR_H;
        outScroll = ui.outputPanel(outLines, outX, outBodyY, outW, outBodyH, outScroll);

        // ── Status bar ────────────────────────────────────────────────────────
        const sbY = vpH - STATUS_H;
        ui.panel(0, sbY, vpW, STATUS_H, C.SURFACE);
        ui.separator(0, sbY, vpW, C.BORDER);

        let sbx = 12;
        ui.drawText('NSCode', sbx, sbY + (STATUS_H - atlas.glyphH) / 2, C.ACCENT);
        sbx += 8 * atlas.glyphW;

        const { line, col } = buf.cursor;
        const edStatus = `Ln ${line+1}, Col ${col+1}  \u2022  ${buf.lineCount()} lines`;
        ui.drawText(edStatus, sbx, sbY + (STATUS_H - atlas.glyphH) / 2, C.TEXT_DIM);

        // WebGPU badge (right side)
        const badge = 'WebGPU \u2713';
        const badgeX = vpW - badge.length * atlas.glyphW - 12;
        ui.drawText(badge, badgeX, sbY + (STATUS_H - atlas.glyphH) / 2, C.GREEN);

        // ── Render ────────────────────────────────────────────────────────────
        gpu.render(ui.endFrame());

        // Reset per-frame input flags
        justDown = false;
        justUp   = false;
    }

    requestAnimationFrame(frame);
}

main().catch(err => {
    console.error(err);
    document.body.style.cssText = 'margin:0;background:#1e1e24;color:#e05c5c;display:flex;align-items:center;justify-content:center;height:100vh;font:16px monospace;text-align:center;padding:32px';
    document.body.innerHTML = `<div><h2 style="color:#f0a0a0">Error</h2><pre style="color:#ccc;text-align:left">${err.stack ?? err}</pre></div>`;
});
