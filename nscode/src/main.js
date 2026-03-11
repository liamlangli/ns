// NSCode — GPU-rendered code playground (no HTML/CSS UI elements)
// All UI rendered via MSDF text + rect pipelines on a single WebGPU canvas.

import { TextBuffer }    from './editor.js';
import { NSInterpreter } from './interpreter.js';
import { load_msdf_font } from './font.js';
import { GPU }           from './gpu.js';
import { UI, C }         from './ui.js';

// ── Layout constants ──────────────────────────────────────────────────────────
const TOOLBAR_H = 32;
const STATUS_H  = 20;
const DIVIDER_W = 4;
const BTN_W     = 72;
const BTN_H     = 22;
const BTN_PAD   = 5;   // top padding inside toolbar

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

    const canvas       = document.getElementById('c');
    const hidden_input = document.getElementById('hi');
    hidden_input.focus();

    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('No WebGPU adapter');
    const device  = await adapter.requestDevice();
    const atlas   = await load_msdf_font(device, {
        json_url:     './public/fonts/jetbrains-mono-latin-400-normal.json',
        png_url:      './public/fonts/jetbrains-mono.png',
        display_size: 14,   // scale the 48px atlas to ~14px cap-height
    });

    const gpu = new GPU(canvas);
    await gpu.init(device, atlas);

    const ui  = new UI();
    ui.set_font(atlas);

    const buf = new TextBuffer(EXAMPLES.fib);

    // Output state
    let out_lines      = [];  // [{text, cls}]
    let out_scroll     = 0;
    let run_status     = 'idle';  // 'idle'|'run'|'ok'|'err'
    let run_status_msg = 'Ready';

    // Divider (output pane width = div_ref.value, starts at 40% of window)
    const div_ref = { value: 0, start_x: 0, start_val: 0 };

    // Cursor blink
    let cursor_visible = true;
    let blink_t = 0;

    // Mouse state
    let mx = 0, my = 0, mouse_down = false, just_down = false, just_up = false;
    let vp_w = 0, vp_h = 0;

    // Viewport sizing
    function resize() {
        vp_w = window.innerWidth;
        vp_h = window.innerHeight;
        gpu.resize(vp_w, vp_h);
        if (div_ref.value === 0) div_ref.value = Math.round(vp_w * 0.4);
    }
    resize();
    window.addEventListener('resize', resize);

    // ── Run NS code ──────────────────────────────────────────────────────────
    async function run_code() {
        out_lines = [];
        out_scroll = 0;
        run_status = 'run';
        run_status_msg = 'Running…';

        await new Promise(r => setTimeout(r, 0));
        const interp = new NSInterpreter({
            print: v => out_lines.push({ text: String(v), cls: 'print' }),
            error: v => out_lines.push({ text: String(v), cls: 'error' }),
        });
        const t0 = performance.now();
        let ok = true;
        try { interp.run(buf.get_text()); }
        catch (e) { ok = false; out_lines.push({ text: e.message ?? String(e), cls: 'error' }); }
        const ms = performance.now() - t0;
        out_lines.push({ text: '─'.repeat(36), cls: 'sep' });
        out_lines.push({ text: `Finished in ${ms.toFixed(1)} ms`, cls: 'time' });
        run_status     = ok ? 'ok' : 'err';
        run_status_msg = ok ? 'Success' : 'Error';
        out_scroll     = Math.max(0, out_lines.length - 10);
    }

    // ── Keyboard ─────────────────────────────────────────────────────────────
    window.addEventListener('keydown', e => {
        const ctrl  = e.ctrlKey || e.metaKey;
        const shift = e.shiftKey;
        if (ctrl && e.key === 'Enter') { e.preventDefault(); run_code(); return; }
        let handled = true;
        switch (e.key) {
        case 'ArrowLeft':  buf.move_left(shift);       break;
        case 'ArrowRight': buf.move_right(shift);      break;
        case 'ArrowUp':    buf.move_up(shift);         break;
        case 'ArrowDown':  buf.move_down(shift);       break;
        case 'Home':       buf.move_line_start(shift); break;
        case 'End':        buf.move_line_end(shift);   break;
        case 'Backspace':  buf.backspace();             break;
        case 'Delete':     buf.delete_forward();       break;
        case 'Tab':        buf.insert_text('    ');    break;
        case 'Enter': {
            const indent = buf.auto_indent();
            const before = buf.line_at(buf.cursor.line).slice(0, buf.cursor.col).trimEnd();
            buf.insert_text('\n' + indent + (before.endsWith('{') ? '    ' : ''));
            break;
        }
        case 'a': if (ctrl) buf.select_all(); else handled = false; break;
        case 'c': if (ctrl) { copy_selection(); } else handled = false; break;
        case 'v': if (ctrl) { navigator.clipboard?.readText().then(t => { if (t) buf.insert_text(t); }); } else handled = false; break;
        case 'x': if (ctrl) { copy_selection(); buf.delete_selection(); } else handled = false; break;
        default:  handled = false;
        }
        if (handled) { e.preventDefault(); blink_t = 0; cursor_visible = true; }
    });

    function copy_selection() {
        const sel = buf.get_selection_range();
        if (!sel) return;
        const parts = [];
        for (let l = sel.start.line; l <= sel.end.line; l++) {
            const ln = buf.line_at(l);
            const s  = l === sel.start.line ? sel.start.col : 0;
            const en = l === sel.end.line   ? sel.end.col   : ln.length;
            parts.push(ln.slice(s, en));
        }
        navigator.clipboard?.writeText(parts.join('\n'));
    }

    hidden_input.addEventListener('input', e => {
        const t = e.data ?? '';
        if (t) { buf.insert_text(t); blink_t = 0; cursor_visible = true; }
        hidden_input.value = '';
    });

    // ── Mouse ────────────────────────────────────────────────────────────────
    canvas.addEventListener('mousedown', e => {
        hidden_input.focus();
        mx = e.clientX; my = e.clientY;
        mouse_down = true; just_down = true;
        e.preventDefault();
    });
    window.addEventListener('mousemove', e => { mx = e.clientX; my = e.clientY; });
    window.addEventListener('mouseup',   e => {
        mx = e.clientX; my = e.clientY;
        mouse_down = false; just_up = true;
    });

    canvas.addEventListener('wheel', e => {
        e.preventDefault();
        const main_h   = vp_h - TOOLBAR_H - STATUS_H;
        const out_w    = Math.max(200, Math.min(vp_w * 0.65, div_ref.value));
        const editor_w = vp_w - out_w - DIVIDER_W;

        // Determine which pane the wheel is over
        if (e.clientX < editor_w) {
            buf.scroll_top  = Math.max(0, Math.min((buf.scroll_top ?? 0) + e.deltaY / atlas.glyph_h, buf.line_count() - 1));
            buf.scroll_left = Math.max(0, (buf.scroll_left ?? 0) + e.deltaX);
        } else {
            out_scroll = Math.max(0, Math.min(out_scroll + e.deltaY / atlas.glyph_h, Math.max(0, out_lines.length - 1)));
        }
    }, { passive: false });

    // ── Frame loop ───────────────────────────────────────────────────────────
    let last = 0;
    function frame(ts) {
        requestAnimationFrame(frame);

        const dt = ts - last; last = ts;

        // Cursor blink
        blink_t += dt;
        if (blink_t > 530) { blink_t = 0; cursor_visible = !cursor_visible; }

        // Layout
        const main_h   = vp_h - TOOLBAR_H - STATUS_H;
        const out_w    = Math.max(200, Math.min(Math.round(vp_w * 0.65), div_ref.value));
        const editor_w = vp_w - out_w - DIVIDER_W;

        // Begin ImGui frame
        ui.begin_frame(mx, my, mouse_down, just_down, just_up, vp_w, vp_h);

        // ── Toolbar ───────────────────────────────────────────────────────────
        ui.panel(0, 0, vp_w, TOOLBAR_H, C.SURFACE);
        ui.separator(0, TOOLBAR_H - 1, vp_w, C.BORDER);

        let tbx = 12;
        // Brand
        ui.draw_text('NSCode', tbx, (TOOLBAR_H - atlas.glyph_h) / 2, C.ACCENT);
        tbx += 7 * atlas.glyph_w + 8;

        // Separator
        ui._dl.rect(tbx, BTN_PAD, 1, BTN_H, C.BORDER[0], C.BORDER[1], C.BORDER[2], 1);
        tbx += 9;

        // Run button
        if (ui.button('run', '\u25b6 Run', tbx, BTN_PAD, BTN_W, BTN_H, true)) {
            run_code();
        }
        tbx += BTN_W + 6;

        // Clear button
        if (ui.button('clear', 'Clear', tbx, BTN_PAD, BTN_W, BTN_H)) {
            out_lines = []; out_scroll = 0; run_status = 'idle'; run_status_msg = 'Ready';
        }
        tbx += BTN_W + 8;

        // Separator
        ui._dl.rect(tbx, BTN_PAD, 1, BTN_H, C.BORDER[0], C.BORDER[1], C.BORDER[2], 1);
        tbx += 9;

        // Example dropdown
        const example_w = 16 * atlas.glyph_w;
        const chosen = ui.select('examples', EXAMPLE_LIST, '— Examples —', tbx, BTN_PAD, example_w, BTN_H);
        if (chosen) { buf.set_text(EXAMPLES[chosen]); blink_t = 0; cursor_visible = true; }
        tbx += example_w + 16;

        // Hint (right-aligned)
        const hint  = 'Ctrl+Enter to run';
        const hint_x = vp_w - hint.length * atlas.glyph_w - 12;
        ui.draw_text(hint, hint_x, (TOOLBAR_H - atlas.glyph_h) / 2, C.TEXT_DIM);

        // ── Editor pane ───────────────────────────────────────────────────────
        const hit = ui.code_editor(buf, 0, TOOLBAR_H, editor_w, main_h, cursor_visible, atlas);
        if (hit) {
            buf.move_cursor(hit.line, hit.col, false);
            blink_t = 0; cursor_visible = true;
        }

        // ── Divider ───────────────────────────────────────────────────────────
        const div_x = editor_w;
        ui.divider('div', div_x, TOOLBAR_H, main_h, div_ref);

        // ── Output header ─────────────────────────────────────────────────────
        const out_x    = div_x + DIVIDER_W;
        const HDR_H    = 34;
        ui.panel(out_x, TOOLBAR_H, out_w, HDR_H, C.SURFACE);
        ui.separator(out_x, TOOLBAR_H + HDR_H - 1, out_w, C.BORDER);
        const dot_y = TOOLBAR_H + (HDR_H - 8) / 2;
        ui.status_dot(out_x + 10, dot_y, 4, run_status);
        ui.draw_text('Output', out_x + 26, TOOLBAR_H + (HDR_H - atlas.glyph_h) / 2, C.TEXT);
        const msg_x = out_x + 26 + 8 * atlas.glyph_w;
        ui.draw_text(run_status_msg, msg_x, TOOLBAR_H + (HDR_H - atlas.glyph_h) / 2, C.TEXT_DIM);

        // ── Output body ───────────────────────────────────────────────────────
        const out_body_y = TOOLBAR_H + HDR_H;
        const out_body_h = main_h - HDR_H;
        out_scroll = ui.output_panel(out_lines, out_x, out_body_y, out_w, out_body_h, out_scroll);

        // ── Status bar ────────────────────────────────────────────────────────
        const sb_y = vp_h - STATUS_H;
        ui.panel(0, sb_y, vp_w, STATUS_H, C.SURFACE);
        ui.separator(0, sb_y, vp_w, C.BORDER);

        let sbx = 12;
        ui.draw_text('NSCode', sbx, sb_y + (STATUS_H - atlas.glyph_h) / 2, C.ACCENT);
        sbx += 8 * atlas.glyph_w;

        const { line, col } = buf.cursor;
        const ed_status = `Ln ${line+1}, Col ${col+1}  \u2022  ${buf.line_count()} lines`;
        ui.draw_text(ed_status, sbx, sb_y + (STATUS_H - atlas.glyph_h) / 2, C.TEXT_DIM);

        // WebGPU badge (right side)
        const badge   = 'WebGPU \u2713';
        const badge_x = vp_w - badge.length * atlas.glyph_w - 12;
        ui.draw_text(badge, badge_x, sb_y + (STATUS_H - atlas.glyph_h) / 2, C.GREEN);

        // ── Render ────────────────────────────────────────────────────────────
        gpu.render(ui.end_frame());

        // Reset per-frame input flags
        just_down = false;
        just_up   = false;
    }

    requestAnimationFrame(frame);
}

main().catch(err => {
    console.error(err);
    document.body.style.cssText = 'margin:0;background:#1e1e24;color:#e05c5c;display:flex;align-items:center;justify-content:center;height:100vh;font:16px monospace;text-align:center;padding:32px';
    document.body.innerHTML = `<div><h2 style="color:#f0a0a0">Error</h2><pre style="color:#ccc;text-align:left">${err.stack ?? err}</pre></div>`;
});
