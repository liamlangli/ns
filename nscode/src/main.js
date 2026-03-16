// NSCode — GPU-rendered code playground (no HTML/CSS UI elements)
// All UI rendered via MSDF text + rect pipelines on a single WebGPU canvas.

import { text_buffer }           from './editor.js';
import { ns_interpreter, parse_to_ast } from './interpreter.js';
import { load_msdf_font }       from './font.js';
import { GPU }                  from './gpu.js';
import { gpu_parser }            from './gpu_parser.js';
import { compare_normalized_asts, normalize_cpu_ast_by_function } from './parser_validation.js';
import { UI, C }                from './ui.js';
import { fuzzy_filter, key_map, event_key_id } from './commands.js';
import { WebGPUModule }         from './webgpu_module.js';

// ── Layout constants ──────────────────────────────────────────────────────────
const DEBUG_GPU_PARSE_VALIDATION = globalThis.localStorage?.getItem('ns.debugGpuParseValidation') === '1';
const TOOLBAR_H        = 32;
const STATUS_H         = 20;
const TREE_HEADER_H    = 28;
const TREE_COLLAPSED_W = 24;
const DIVIDER_W        = 4;
const BTN_W            = 72;
const BTN_H            = 22;
const BTN_PAD          = 5;

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

    // ── WebGPU examples (JavaScript, not NanoScript) ────────────────────────
    webgpu_triangle: `// WebGPU Triangle
// Renders a rainbow triangle using WebGPU.
// Globals: device, canvas, context, format, print

const shader = device.createShaderModule({
    code: \`
        struct VSOut {
            @builtin(position) pos : vec4f,
            @location(0)       col : vec3f,
        }
        @vertex fn vs(@builtin(vertex_index) i: u32) -> VSOut {
            var p = array<vec2f, 3>(
                vec2f( 0.0,  0.6),
                vec2f(-0.6, -0.6),
                vec2f( 0.6, -0.6),
            );
            var c = array<vec3f, 3>(
                vec3f(1.0, 0.4, 0.4),
                vec3f(0.4, 1.0, 0.4),
                vec3f(0.4, 0.4, 1.0),
            );
            return VSOut(vec4f(p[i], 0.0, 1.0), c[i]);
        }
        @fragment fn fs(v: VSOut) -> @location(0) vec4f {
            return vec4f(v.col, 1.0);
        }
    \`,
});

const pipeline = device.createRenderPipeline({
    layout:    'auto',
    vertex:    { module: shader, entryPoint: 'vs' },
    fragment:  { module: shader, entryPoint: 'fs', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
});

const enc  = device.createCommandEncoder();
const view = context.getCurrentTexture().createView();
const pass = enc.beginRenderPass({
    colorAttachments: [{
        view,
        clearValue: { r: 0.118, g: 0.118, b: 0.141, a: 1 },
        loadOp:  'clear',
        storeOp: 'store',
    }],
});
pass.setPipeline(pipeline);
pass.draw(3);
pass.end();
device.queue.submit([enc.finish()]);
print('Triangle rendered!');
`,
};

// File-tree structure (mutable: group.open can toggle)
const FILE_TREE = [
    {
        label: 'Examples',
        open:  true,
        items: [
            { label: 'Hello World', value: 'hello'     },
            { label: 'Fibonacci',   value: 'fib'       },
            { label: 'Factorial',   value: 'factorial' },
            { label: 'Loop & Sum',  value: 'loop'      },
            { label: 'FizzBuzz',    value: 'fizzbuzz'  },
            { label: 'Primes',      value: 'primes'    },
            { label: 'Closures',    value: 'closure'   },
        ],
    },
    {
        label: 'WebGPU',
        open:  true,
        items: [
            { label: 'Triangle', value: 'webgpu_triangle' },
        ],
    },
];

// WebGPU examples execute as JavaScript, not NanoScript
const WEBGPU_EXAMPLES = new Set(['webgpu_triangle']);

// ── Default keybindings ───────────────────────────────────────────────────────
const DEFAULT_BINDINGS = [
    { id: 'run',          label: 'Run Code',         category: 'Code',    keys: ['ctrl+Enter'] },
    { id: 'clear',        label: 'Clear Output',     category: 'Code',    keys: ['ctrl+l'] },
    { id: 'palette',      label: 'Command Palette',  category: 'UI',      keys: ['ctrl+p'] },
    { id: 'find',         label: 'Find',             category: 'Search',  keys: ['ctrl+f'] },
    { id: 'find_replace', label: 'Find & Replace',   category: 'Search',  keys: ['ctrl+h'] },
    { id: 'goto_line',    label: 'Go to Line',       category: 'Navigate',keys: ['ctrl+g'] },
    { id: 'toggle_tree',  label: 'Toggle Sidebar',   category: 'UI',      keys: ['ctrl+b'] },
    { id: 'select_all',   label: 'Select All',       category: 'Edit',    keys: ['ctrl+a'] },
    { id: 'copy',         label: 'Copy',             category: 'Edit',    keys: ['ctrl+c'] },
    { id: 'paste',        label: 'Paste',            category: 'Edit',    keys: ['ctrl+v'] },
    { id: 'cut',          label: 'Cut',              category: 'Edit',    keys: ['ctrl+x'] },
    { id: 'comment',      label: 'Toggle Comment',   category: 'Edit',    keys: ['ctrl+/'] },
    { id: 'move_up',      label: 'Move Line Up',     category: 'Edit',    keys: ['alt+ArrowUp'] },
    { id: 'move_down',    label: 'Move Line Down',   category: 'Edit',    keys: ['alt+ArrowDown'] },
    { id: 'duplicate',    label: 'Duplicate Line',   category: 'Edit',    keys: ['alt+shift+ArrowDown'] },
    { id: 'keybindings',  label: 'Keyboard Shortcuts', category: 'UI',    keys: ['ctrl+k'] },
];

const key_map_instance = new key_map(DEFAULT_BINDINGS);

function extract_function_spans(source) {
    const spans = [];
    const fn_re = /\bfn\s+[A-Za-z_][A-Za-z0-9_]*\s*\([^)]*\)\s*(?:[A-Za-z0-9_<>\[\]]+\s*)?\{/g;
    for (const match of source.matchAll(fn_re)) {
        const start = match.index ?? 0;
        let brace_i = source.indexOf('{', start);
        if (brace_i < 0) continue;
        let depth = 0;
        let end = source.length;
        for (let i = brace_i; i < source.length; i++) {
            const ch = source[i];
            if (ch === '{') depth++;
            else if (ch === '}') {
                depth--;
                if (depth === 0) {
                    end = i + 1;
                    break;
                }
            }
        }
        spans.push({ start, end });
    }
    if (spans.length === 0) spans.push({ start: 0, end: source.length });
    return spans;
}

// ── Build command palette entries ─────────────────────────────────────────────
function build_commands() {
    const base = DEFAULT_BINDINGS.map(b => ({
        id:       b.id,
        label:    b.label,
        category: b.category,
        hint:     key_map_instance.display(b.id),
    }));
    const file_cmds = FILE_TREE.flatMap(group =>
        group.items.map(item => ({
            id:       'load:' + item.value,
            label:    'Load: ' + item.label,
            category: 'File',
            hint:     '',
        }))
    );
    return [...base, ...file_cmds];
}

// ── Main ──────────────────────────────────────────────────────────────────────
async function main() {
    if (!navigator.gpu) {
        document.body.style.cssText = 'margin:0;background:#1e1e24;color:#e05c5c;display:flex;align-items:center;justify-content:center;height:100vh;font:16px monospace;text-align:center';
        document.body.innerHTML = '<div><h2 style="color:#f0a0a0">WebGPU not available</h2><p>Requires Chrome 113+ or Edge 113+</p></div>';
        return;
    }

    const canvas       = document.getElementById('c');
    const hidden_input = document.getElementById('hi');
    const output_view  = document.createElement('div');
    hidden_input.focus();

    output_view.id = 'output-view';
    output_view.tabIndex = 0;
    output_view.style.position = 'fixed';
    output_view.style.margin = '0';
    output_view.style.padding = '8px';
    output_view.style.boxSizing = 'border-box';
    output_view.style.overflow = 'auto';
    output_view.style.whiteSpace = 'pre-wrap';
    output_view.style.wordBreak = 'break-word';
    output_view.style.userSelect = 'text';
    output_view.style.webkitUserSelect = 'text';
    output_view.style.outline = 'none';
    output_view.style.border = 'none';
    output_view.style.background = '#1e1e24';
    output_view.style.color = '#e0e0ec';
    output_view.style.zIndex = '2';
    document.body.appendChild(output_view);

    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('No WebGPU adapter');
    const device  = await adapter.requestDevice();
    const atlas   = await load_msdf_font(device, {
        json_url:     './public/fonts/jetbrains-mono-latin-400-normal.json',
        png_url:      './public/fonts/jetbrains-mono.png',
        display_size: 14,
    });

    const gpu = new GPU(canvas);
    await gpu.init(device, atlas);

    const wgpu_module = new WebGPUModule();
    await wgpu_module.init(device);

    // Compute-only parser pipeline is initialized separately from rendering.
    let compile_gpu = null;
    let gpu_parser_ready = false;
    try {
        compile_gpu = new gpu_parser(device);
        const boot_source = EXAMPLES.fib;
        const boot_spans = extract_function_spans(boot_source);
        compile_gpu.run(boot_source, boot_spans)
            .then((result) => {
                gpu_parser_ready = true;
                console.debug('[GPU parser] startup parse', {
                    functions: result.function_spans.length,
                    tokens: result.counters.token_count,
                    ast: result.counters.ast_count,
                    overflows: [result.counters.tokenOverflow, result.counters.astOverflow],
                });
            })
            .catch((err) => {
                gpu_parser_ready = false;
                compile_gpu = null;
                console.warn('[GPU parser] startup parse failed', err);
            });
    } catch (err) {
        compile_gpu = null;
        gpu_parser_ready = false;
        console.warn('[GPU parser] init failed, CPU parser fallback remains active', err);
    }

    const ui = new UI();
    ui.set_font(atlas);

    const buf = new text_buffer(EXAMPLES.fib);
    let active_example = 'fib';

    // ── Output state ──────────────────────────────────────────────────────────
    let out_lines      = [];
    let out_scroll     = 0;
    let run_status     = 'idle';
    let run_status_msg = 'Ready';
    let output_html    = '';

    // ── Layout state ──────────────────────────────────────────────────────────
    const tree_ref = { value: 160, start_x: 0, start_val: 0 };
    let   tree_collapsed = false;
    const div_ref  = { value: 0,   start_x: 0, start_val: 0 };
    let   computed_tree_w = 160, computed_editor_w = 0;

    // ── Cursor blink ──────────────────────────────────────────────────────────
    let cursor_visible = true;
    let blink_t = 0;

    // ── Mouse state ───────────────────────────────────────────────────────────
    let mx = 0, my = 0, mouse_down = false, just_down = false, just_up = false;
    let vp_w = 0, vp_h = 0;

    // ── Overlay state ─────────────────────────────────────────────────────────
    // Command palette
    const palette = { open: false, query: '', sel: 0 };

    // Find / Replace
    const find_state = {
        open:         false,
        replace_mode: false,
        query:        '',
        replace:      '',
        focus:        'q',   // 'q' | 'r'
        matches:      [],    // [{line, col, len}]
        cur_match:    -1,
    };

    // Go-to-line
    const goto_state = { open: false, query: '' };

    // Keybindings editor
    const kb_state = { open: false, edit_id: null };

    // ── Viewport sizing ───────────────────────────────────────────────────────
    function resize() {
        vp_w = window.innerWidth;
        vp_h = window.innerHeight;
        gpu.resize(vp_w, vp_h);
        if (div_ref.value === 0) div_ref.value = Math.round((vp_w - tree_ref.value) * 0.4);
    }
    resize();
    window.addEventListener('resize', resize);

    function escape_html(text) {
        return text
            .replaceAll('&', '&amp;')
            .replaceAll('<', '&lt;')
            .replaceAll('>', '&gt;');
    }

    function render_output_view() {
        const cls_style = {
            print: 'color:#e0e0ec;',
            error: 'color:#e05c5c;',
            info: 'color:#888;',
            sep: 'color:#3a3a48;',
            time: 'color:#d4a44c;',
        };
        output_html = out_lines.map(line => {
            const cls = line.cls ? ` out-${line.cls}` : '';
            const style = cls_style[line.cls] ?? 'color:#e0e0ec;';
            return `<div class="out-line${cls}" style="${style}">${escape_html(String(line.text)) || '&nbsp;'}</div>`;
        }).join('');
        output_view.innerHTML = output_html;
        output_view.scrollTop = out_scroll * atlas.glyph_h;
    }

    function has_output_selection() {
        const sel = window.getSelection();
        if (!sel || sel.isCollapsed) return false;
        return output_view.contains(sel.anchorNode) || output_view.contains(sel.focusNode);
    }

    output_view.addEventListener('scroll', () => {
        out_scroll = output_view.scrollTop / atlas.glyph_h;
    });
    render_output_view();

    // ── Run code ──────────────────────────────────────────────────────────────
    async function run_code() {
        out_lines = [];
        out_scroll = 0;
        render_output_view();
        run_status = 'run';
        run_status_msg = 'Running\u2026';
        await new Promise(r => setTimeout(r, 0));

        // ── WebGPU execution path ──────────────────────────────────────────
        if (WEBGPU_EXAMPLES.has(active_example)) {
            const print_fn = v => out_lines.push({ text: String(v), cls: 'print' });
            const t0 = performance.now();
            const result = await wgpu_module.run(buf.get_text(), print_fn);
            const elapsed = (performance.now() - t0).toFixed(2);
            if (result.ok) {
                out_lines.push({ text: '\u2500'.repeat(36), cls: 'sep' });
                out_lines.push({ text: `Done in ${elapsed} ms`, cls: 'time' });
                run_status = 'ok'; run_status_msg = 'Success';
            } else {
                out_lines.push({ text: `Error: ${result.error}`, cls: 'error' });
                run_status = 'err'; run_status_msg = 'Error';
            }
            out_scroll = Math.max(0, out_lines.length - 10);
            render_output_view();
            return;
        }

        // ── NanoScript execution path ──────────────────────────────────────
        const interp = new ns_interpreter({
            print: v => out_lines.push({ text: String(v), cls: 'print' }),
            error: v => out_lines.push({ text: String(v), cls: 'error' }),
        });
        const source = buf.get_text();
        const spans = extract_function_spans(source);

        let cpu_parse_ms = 0;
        let gpu_parse_ms = null;
        let execute_ms   = null;
        let cpu_ast      = null;
        let parse_ok     = true;
        let ok = true;

        const cpu_parse_t0 = performance.now();
        try {
            cpu_ast = parse_to_ast(source);
        } catch (e) {
            parse_ok = false;
            ok = false;
            out_lines.push({ text: `Parse error: ${e.message ?? String(e)}`, cls: 'error' });
        }
        cpu_parse_ms = performance.now() - cpu_parse_t0;

        if (compile_gpu && gpu_parser_ready) {
            const gpu_parse_t0 = performance.now();
            try {
                const gpu_result = await compile_gpu.run(source, spans);
                gpu_parse_ms = performance.now() - gpu_parse_t0;
                if (gpu_result.counters.tokenOverflow || gpu_result.counters.astOverflow) {
                    out_lines.push({ text: '[GPU parser] overflow flag raised (results clamped).', cls: 'error' });
                }
            } catch (e) {
                gpu_parse_ms = performance.now() - gpu_parse_t0;
                out_lines.push({ text: `[GPU parser] parse failed: ${e.message ?? String(e)}`, cls: 'error' });
            }
        } else {
            out_lines.push({ text: '[GPU parser] unavailable; CPU parser in use.', cls: 'info' });
        }

        if (DEBUG_GPU_PARSE_VALIDATION) {
            try {
                const cpu_normalized = normalize_cpu_ast_by_function(source, spans);
                if (!compile_gpu || !gpu_parser_ready) {
                    out_lines.push({ text: '[parser Validation] GPU unavailable; using CPU parser fallback.', cls: 'info' });
                } else {
                    const gpu_normalized_result = await compile_gpu.parse_normalized_ast(source, spans);
                    if (gpu_normalized_result.run.counters.tokenOverflow || gpu_normalized_result.run.counters.astOverflow) {
                        out_lines.push({ text: '[parser Validation] GPU overflow flag raised; using CPU parser fallback.', cls: 'error' });
                    } else {
                        const cmp = compare_normalized_asts(cpu_normalized, gpu_normalized_result.ast_by_function.map((nodes, functionIndex) => ({ functionIndex, nodes })));
                        if (cmp.ok) {
                            out_lines.push({ text: `[parser Validation] PASS (${spans.length} functions)`, cls: 'info' });
                        } else if (cmp.mismatch) {
                            const m = cmp.mismatch;
                            out_lines.push({ text: `[parser Validation] FAIL at function=${m.functionIndex} path=${m.path}: ${m.reason}`, cls: 'error' });
                            out_lines.push({ text: `[parser Validation] expected=${JSON.stringify(m.expected)} actual=${JSON.stringify(m.actual)}`, cls: 'error' });
                        }
                    }
                }
            } catch (e) {
                out_lines.push({ text: `[parser Validation] ERROR: ${e.message ?? String(e)} (CPU fallback in effect)`, cls: 'error' });
            }
        }

        if (parse_ok && cpu_ast) {
            const exec_t0 = performance.now();
            try {
                interp.eval_program(cpu_ast, interp.globals);
            } catch (e) {
                ok = false;
                out_lines.push({ text: `Runtime error: ${e.message ?? String(e)}`, cls: 'error' });
            }
            execute_ms = performance.now() - exec_t0;
        }

        out_lines.push({ text: '\u2500'.repeat(36), cls: 'sep' });
        out_lines.push({ text: `CPU parse: ${cpu_parse_ms.toFixed(2)} ms`, cls: 'time' });
        out_lines.push({ text: `GPU parse: ${gpu_parse_ms === null ? 'N/A' : `${gpu_parse_ms.toFixed(2)} ms`}`, cls: 'time' });
        out_lines.push({ text: `Execute: ${execute_ms === null ? 'N/A' : `${execute_ms.toFixed(2)} ms`}`, cls: 'time' });
        out_lines.push({ text: `Total: ${(cpu_parse_ms + (execute_ms ?? 0)).toFixed(2)} ms`, cls: 'time' });
        run_status     = ok ? 'ok' : 'err';
        run_status_msg = ok ? 'Success' : 'Error';
        out_scroll     = Math.max(0, out_lines.length - 10);
        render_output_view();
    }

    // ── Editor operations ─────────────────────────────────────────────────────
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

    function toggle_comment() {
        const sel     = buf.get_selection_range();
        const start_l = sel?.start.line ?? buf.cursor.line;
        const end_l   = sel?.end.line   ?? buf.cursor.line;
        const all_commented = Array.from(
            { length: end_l - start_l + 1 },
            (_, i) => buf.line_at(start_l + i)
        ).every(ln => /^\s*\/\//.test(ln));
        for (let l = start_l; l <= end_l; l++) {
            buf.lines[l] = all_commented
                ? buf.lines[l].replace(/^(\s*)\/\/\s?/, '$1')
                : '//' + buf.lines[l];
        }
        buf.mark_dirty();
    }

    function move_line(dir) {
        const l    = buf.cursor.line;
        const swap = l + dir;
        if (swap < 0 || swap >= buf.line_count()) return;
        [buf.lines[l], buf.lines[swap]] = [buf.lines[swap], buf.lines[l]];
        buf.cursor.line = swap;
        buf.mark_dirty();
    }

    function duplicate_line() {
        const l = buf.cursor.line;
        buf.lines.splice(l + 1, 0, buf.lines[l]);
        buf.cursor.line = l + 1;
        buf.mark_dirty();
    }

    function outdent_selection() {
        const sel     = buf.get_selection_range();
        const start_l = sel?.start.line ?? buf.cursor.line;
        const end_l   = sel?.end.line   ?? buf.cursor.line;
        for (let l = start_l; l <= end_l; l++) {
            if (buf.lines[l].startsWith('    '))      buf.lines[l] = buf.lines[l].slice(4);
            else if (buf.lines[l].startsWith('\t'))   buf.lines[l] = buf.lines[l].slice(1);
        }
        buf.mark_dirty();
    }

    // ── Find ──────────────────────────────────────────────────────────────────
    function update_find_matches() {
        find_state.matches   = [];
        find_state.cur_match = -1;
        if (!find_state.query) return;
        const q = find_state.query.toLowerCase();
        for (let l = 0; l < buf.line_count(); l++) {
            const ln = buf.line_at(l).toLowerCase();
            let idx = 0;
            while ((idx = ln.indexOf(q, idx)) !== -1) {
                find_state.matches.push({ line: l, col: idx, len: q.length });
                idx++;
            }
        }
    }

    function scroll_to_match(m) {
        if (!m) return;
        buf.scroll_top = Math.max(0, m.line - 3);
        buf.move_cursor(m.line, m.col, false);
        buf.move_cursor(m.line, m.col + m.len, true);
    }

    function find_next() {
        if (!find_state.matches.length) return;
        find_state.cur_match = (find_state.cur_match + 1) % find_state.matches.length;
        scroll_to_match(find_state.matches[find_state.cur_match]);
    }

    function find_prev() {
        if (!find_state.matches.length) return;
        find_state.cur_match = (find_state.cur_match - 1 + find_state.matches.length) % find_state.matches.length;
        scroll_to_match(find_state.matches[find_state.cur_match]);
    }

    function do_replace() {
        update_find_matches();
        if (!find_state.matches.length) return;
        if (find_state.cur_match < 0) { find_next(); return; }
        const m = find_state.matches[find_state.cur_match];
        buf.move_cursor(m.line, m.col, false);
        buf.move_cursor(m.line, m.col + m.len, true);
        buf.insert_text(find_state.replace);
        update_find_matches();
        find_next();
    }

    function do_replace_all() {
        update_find_matches();
        if (!find_state.matches.length) return;
        // Reverse order to preserve offsets
        for (const m of [...find_state.matches].reverse()) {
            buf.move_cursor(m.line, m.col, false);
            buf.move_cursor(m.line, m.col + m.len, true);
            buf.insert_text(find_state.replace);
        }
        update_find_matches();
    }

    // ── Command execution ─────────────────────────────────────────────────────
    function open_overlay(type) {
        palette.open      = false;
        find_state.open   = type === 'find' || type === 'replace';
        find_state.replace_mode = type === 'replace';
        goto_state.open   = type === 'goto';
        kb_state.open     = type === 'keybindings';
        if (find_state.open) { find_state.focus = 'q'; update_find_matches(); }
        if (goto_state.open)  goto_state.query = '';
    }

    function execute_command(id) {
        blink_t = 0; cursor_visible = true;
        switch (id) {
        case 'run':          run_code(); break;
        case 'clear':        out_lines = []; out_scroll = 0; run_status = 'idle'; run_status_msg = 'Ready'; render_output_view(); break;
        case 'palette':      palette.open = true; palette.query = ''; palette.sel = 0;
                             find_state.open = false; goto_state.open = false; kb_state.open = false; break;
        case 'find':         open_overlay('find'); break;
        case 'find_replace': open_overlay('replace'); break;
        case 'goto_line':    open_overlay('goto'); break;
        case 'keybindings':  open_overlay('keybindings'); break;
        case 'toggle_tree':  tree_collapsed = !tree_collapsed; break;
        case 'select_all':   buf.select_all(); break;
        case 'copy':
            if (has_output_selection()) navigator.clipboard?.writeText(window.getSelection()?.toString() ?? '');
            else copy_selection();
            break;
        case 'paste':        navigator.clipboard?.readText().then(t => { if (t) buf.insert_text(t); }); break;
        case 'cut':
            if (has_output_selection()) navigator.clipboard?.writeText(window.getSelection()?.toString() ?? '');
            else { copy_selection(); buf.delete_selection(); buf.mark_dirty(); }
            break;
        case 'comment':      toggle_comment(); break;
        case 'move_up':      move_line(-1); break;
        case 'move_down':    move_line(1); break;
        case 'duplicate':    duplicate_line(); break;
        default:
            if (id.startsWith('load:')) {
                const key = id.slice(5);
                if (EXAMPLES[key]) {
                    active_example = key;
                    buf.set_text(EXAMPLES[key]);
                    find_state.open = false; palette.open = false;
                }
            }
        }
    }

    // ── Keyboard handler ──────────────────────────────────────────────────────
    const any_overlay = () => palette.open || find_state.open || goto_state.open || kb_state.open;

    window.addEventListener('keydown', e => {
        const shift = e.shiftKey;

        // ── Palette overlay ────────────────────────────────────────────────────
        if (palette.open) {
            e.preventDefault();
            if (e.key === 'Escape') { palette.open = false; return; }
            if (e.key === 'ArrowUp') {
                palette.sel = Math.max(0, palette.sel - 1); return;
            }
            if (e.key === 'ArrowDown') {
                const filtered = fuzzy_filter(palette.query, build_commands(), c => c.label);
                palette.sel = Math.min(filtered.length - 1, palette.sel + 1); return;
            }
            if (e.key === 'Enter') {
                const filtered = fuzzy_filter(palette.query, build_commands(), c => c.label);
                const cmd = filtered[palette.sel];
                if (cmd) execute_command(cmd.id);
                palette.open = false; return;
            }
            if (e.key === 'Backspace') { palette.query = palette.query.slice(0, -1); palette.sel = 0; }
            return; // other chars handled by hidden_input input event
        }

        // ── Find / Replace overlay ─────────────────────────────────────────────
        if (find_state.open) {
            if (e.key === 'Escape') { find_state.open = false; e.preventDefault(); return; }
            if (e.key === 'Enter' && !shift) { find_next(); e.preventDefault(); return; }
            if (e.key === 'Enter' &&  shift) { find_prev(); e.preventDefault(); return; }
            if (e.key === 'Tab')  {
                find_state.focus = find_state.focus === 'q' ? 'r' : 'q';
                e.preventDefault(); return;
            }
            if (e.key === 'Backspace') {
                if (find_state.focus === 'q') {
                    find_state.query = find_state.query.slice(0, -1);
                    update_find_matches();
                } else {
                    find_state.replace = find_state.replace.slice(0, -1);
                }
                e.preventDefault(); return;
            }
            // Printable chars go through hidden_input input event — don't preventDefault
            return;
        }

        // ── Go-to-line overlay ─────────────────────────────────────────────────
        if (goto_state.open) {
            e.preventDefault();
            if (e.key === 'Escape') { goto_state.open = false; return; }
            if (e.key === 'Enter') {
                const n = parseInt(goto_state.query, 10);
                if (!isNaN(n)) {
                    const l = Math.max(0, Math.min(n - 1, buf.line_count() - 1));
                    buf.move_cursor(l, 0, false);
                    buf.scroll_top = Math.max(0, l - 5);
                }
                goto_state.open = false; return;
            }
            if (e.key === 'Backspace') { goto_state.query = goto_state.query.slice(0, -1); return; }
            if (/^\d$/.test(e.key))    { goto_state.query += e.key; return; }
            return;
        }

        // ── Keybindings overlay ────────────────────────────────────────────────
        if (kb_state.open) {
            if (e.key === 'Escape') { kb_state.open = false; kb_state.edit_id = null; e.preventDefault(); return; }
            // If editing a binding, capture the key combo
            if (kb_state.edit_id) {
                e.preventDefault();
                if (e.key === 'Backspace') { key_map_instance.reset(kb_state.edit_id); kb_state.edit_id = null; return; }
                if (['Shift','Control','Alt','Meta'].includes(e.key)) return; // modifier-only, ignore
                key_map_instance.override(kb_state.edit_id, [event_key_id(e)]);
                kb_state.edit_id = null; return;
            }
            return;
        }

        if (document.activeElement === output_view && !e.metaKey && !e.ctrlKey && !e.altKey) {
            return;
        }

        // ── Command dispatch via keymap ────────────────────────────────────────
        const cmd_id = key_map_instance.match(e);
        if (cmd_id) {
            e.preventDefault();
            execute_command(cmd_id);
            return;
        }

        // ── Editor keys ────────────────────────────────────────────────────────
        let handled = true;
        switch (e.key) {
        case 'ArrowLeft':  buf.move_left(shift);       break;
        case 'ArrowRight': buf.move_right(shift);      break;
        case 'ArrowUp':    buf.move_up(shift);         break;
        case 'ArrowDown':  buf.move_down(shift);       break;
        case 'Home':       buf.move_line_start(shift); break;
        case 'End':        buf.move_line_end(shift);   break;
        case 'Backspace':  buf.backspace();             break;
        case 'Delete':     buf.delete_forward();        break;
        case 'Tab':
            if (shift) outdent_selection();
            else buf.insert_text('    ');
            break;
        case 'Enter': {
            const indent = buf.auto_indent();
            const before = buf.line_at(buf.cursor.line).slice(0, buf.cursor.col).trimEnd();
            buf.insert_text('\n' + indent + (before.endsWith('{') ? '    ' : ''));
            break;
        }
        default: handled = false;
        }
        if (handled) { e.preventDefault(); blink_t = 0; cursor_visible = true; }
    });

    // ── Text input routing ─────────────────────────────────────────────────────
    hidden_input.addEventListener('input', e => {
        const t = e.data ?? '';
        hidden_input.value = '';
        if (!t) return;
        if (palette.open) {
            palette.query += t; palette.sel = 0; return;
        }
        if (find_state.open) {
            if (find_state.focus === 'q') {
                find_state.query += t; update_find_matches();
            } else {
                find_state.replace += t;
            }
            return;
        }
        if (goto_state.open || kb_state.open) return;
        buf.insert_text(t); blink_t = 0; cursor_visible = true;
    });

    // ── Mouse ─────────────────────────────────────────────────────────────────
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
        const tw = computed_tree_w;
        if (e.clientX < tw) {
            // tree — no scroll
        } else if (e.clientX < tw + DIVIDER_W + computed_editor_w) {
            buf.scroll_top  = Math.max(0, Math.min((buf.scroll_top ?? 0) + e.deltaY / atlas.glyph_h, buf.line_count() - 1));
            buf.scroll_left = Math.max(0, (buf.scroll_left ?? 0) + e.deltaX);
        } else {
            out_scroll = Math.max(0, Math.min(out_scroll + e.deltaY / atlas.glyph_h, Math.max(0, out_lines.length - 1)));
        }
    }, { passive: false });

    // ── Frame loop ────────────────────────────────────────────────────────────
    let last = 0;
    function frame(ts) {
        requestAnimationFrame(frame);
        const dt = ts - last; last = ts;

        // Cursor blink (suppress blinking when overlay is open for cleanliness)
        blink_t += dt;
        if (blink_t > 530) { blink_t = 0; cursor_visible = !cursor_visible; }

        // Layout
        const main_h  = vp_h - TOOLBAR_H - STATUS_H;
        const tree_w  = tree_collapsed
            ? TREE_COLLAPSED_W
            : Math.max(100, Math.min(400, tree_ref.value));
        const avail_w  = vp_w - tree_w - DIVIDER_W;
        const out_w    = Math.max(200, Math.min(Math.round(avail_w * 0.65), div_ref.value));
        const editor_w = Math.max(100, avail_w - out_w - DIVIDER_W);
        computed_tree_w = tree_w; computed_editor_w = editor_w;

        // Begin frame — mouse clicks inside overlays are processed by the overlay widgets;
        // suppress canvas clicks from reaching editor when any overlay is open.
        const ov_open = any_overlay();
        ui.begin_frame(mx, my, mouse_down,
            just_down && !ov_open,
            just_up   && !ov_open,
            vp_w, vp_h);

        // ── Background ────────────────────────────────────────────────────────
        ui.draw_rect(0, 0, vp_w, vp_h, C.BG);

        // ── Toolbar ───────────────────────────────────────────────────────────
        ui.panel(0, 0, vp_w, TOOLBAR_H, C.SURFACE);
        ui.separator(0, TOOLBAR_H - 1, vp_w, C.BORDER);

        let tbx = 12;
        ui.draw_text('NSCode', tbx, (TOOLBAR_H - atlas.glyph_h) / 2, C.ACCENT);
        tbx += 7 * atlas.glyph_w + 8;

        ui._dl.rect(tbx, BTN_PAD, 1, BTN_H, C.BORDER[0], C.BORDER[1], C.BORDER[2], 1);
        tbx += 9;

        if (ui.run_button('run', tbx, BTN_PAD, BTN_W, BTN_H)) run_code();
        tbx += BTN_W + 6;

        if (ui.button('clear', 'Clear', tbx, BTN_PAD, BTN_W, BTN_H)) execute_command('clear');
        tbx += BTN_W + 6;

        if (ui.button('palette-btn', 'Cmd+P', tbx, BTN_PAD, BTN_W, BTN_H)) execute_command('palette');
        tbx += BTN_W + 8;

        // Hint (right-aligned)
        const hint   = 'Ctrl+Enter to run';
        const hint_x = vp_w - hint.length * atlas.glyph_w - 12;
        ui.draw_text(hint, hint_x, (TOOLBAR_H - atlas.glyph_h) / 2, C.TEXT_DIM);

        // ── File tree panel ───────────────────────────────────────────────────
        ui.panel(0, TOOLBAR_H, tree_w, main_h, C.SURFACE);
        ui.panel(0, TOOLBAR_H, tree_w, TREE_HEADER_H, C.SURFACE);
        ui.separator(0, TOOLBAR_H + TREE_HEADER_H - 1, tree_w, C.BORDER);

        const tog_x = 4, tog_y = TOOLBAR_H + (TREE_HEADER_H - 18) / 2;
        if (ui.button('tree-toggle', tree_collapsed ? '>' : '<', tog_x, tog_y, 18, 18)) {
            tree_collapsed = !tree_collapsed;
            if (!tree_collapsed && tree_ref.value < 100) tree_ref.value = 160;
        }

        if (!tree_collapsed) {
            ui.draw_text('FILES', tog_x + 22, TOOLBAR_H + (TREE_HEADER_H - atlas.glyph_h) / 2, C.TEXT_DIM);
            const tree_chosen = ui.file_tree(FILE_TREE, active_example,
                0, TOOLBAR_H + TREE_HEADER_H, tree_w, main_h - TREE_HEADER_H);
            if (tree_chosen) {
                active_example = tree_chosen;
                buf.set_text(EXAMPLES[tree_chosen]);
                blink_t = 0; cursor_visible = true;
            }
            ui.divider('tree-div', tree_w, TOOLBAR_H, main_h, tree_ref, true);
        }

        // ── Editor pane ───────────────────────────────────────────────────────
        const editor_x = tree_w + DIVIDER_W;
        const hit = ui.code_editor(buf, editor_x, TOOLBAR_H, editor_w, main_h,
            cursor_visible && !ov_open, atlas,
            find_state.open ? find_state.matches : null);
        if (hit) {
            buf.move_cursor(hit.line, hit.col, false);
            blink_t = 0; cursor_visible = true;
        }

        // ── Output / editor divider ───────────────────────────────────────────
        const div_x = editor_x + editor_w;
        ui.divider('div', div_x, TOOLBAR_H, main_h, div_ref);

        const out_x      = div_x + DIVIDER_W;
        const HDR_H      = 34;
        const webgpu_mode = WEBGPU_EXAMPLES.has(active_example);

        if (webgpu_mode) {
            // ── WebGPU mode: preview top-right, output bottom-right ───────────
            const prev_h         = Math.floor(main_h * 0.6);  // preview panel (incl. header)
            const prev_content_h = prev_h - HDR_H;
            const sep_y          = TOOLBAR_H + prev_h;
            const out_panel_y    = sep_y + DIVIDER_W;
            const out_content_h  = main_h - prev_h - DIVIDER_W - HDR_H;

            // Preview header
            ui.panel(out_x, TOOLBAR_H, out_w, HDR_H, C.SURFACE);
            ui.separator(out_x, TOOLBAR_H + HDR_H - 1, out_w, C.BORDER);
            ui.draw_text('WebGPU Preview',
                out_x + 10, TOOLBAR_H + (HDR_H - atlas.glyph_h) / 2, C.ACCENT);

            // Thin separator strip between preview and output
            ui._dl.rect(out_x, sep_y, out_w, DIVIDER_W,
                C.SURFACE[0], C.SURFACE[1], C.SURFACE[2], 1);
            ui.separator(out_x, sep_y, out_w, C.BORDER);
            ui.separator(out_x, out_panel_y, out_w, C.BORDER);

            // Output header
            ui.panel(out_x, out_panel_y, out_w, HDR_H, C.SURFACE);
            ui.separator(out_x, out_panel_y + HDR_H - 1, out_w, C.BORDER);
            ui.status_dot(out_x + 10, out_panel_y + (HDR_H - 8) / 2, 4, run_status);
            ui.draw_text('Output', out_x + 26, out_panel_y + (HDR_H - atlas.glyph_h) / 2, C.TEXT);
            ui.draw_text(run_status_msg,
                out_x + 26 + 8 * atlas.glyph_w,
                out_panel_y + (HDR_H - atlas.glyph_h) / 2, C.TEXT_DIM);

            // Position WebGPU canvas overlay
            wgpu_module.resize(out_x, TOOLBAR_H + HDR_H, out_w, prev_content_h);
            wgpu_module.show();

            // Position output HTML overlay
            out_scroll = ui.output_panel(out_lines, out_x,
                out_panel_y + HDR_H, out_w, out_content_h, out_scroll);
            output_view.style.left       = `${out_x}px`;
            output_view.style.top        = `${out_panel_y + HDR_H}px`;
            output_view.style.width      = `${out_w}px`;
            output_view.style.height     = `${out_content_h}px`;
            output_view.style.font       = `${atlas.glyph_h - 4}px monospace`;
            output_view.style.lineHeight = `${atlas.glyph_h}px`;

        } else {
            // ── Normal mode: full right panel = output ────────────────────────
            wgpu_module.hide();

            // Output header
            ui.panel(out_x, TOOLBAR_H, out_w, HDR_H, C.SURFACE);
            ui.separator(out_x, TOOLBAR_H + HDR_H - 1, out_w, C.BORDER);
            ui.status_dot(out_x + 10, TOOLBAR_H + (HDR_H - 8) / 2, 4, run_status);
            ui.draw_text('Output', out_x + 26, TOOLBAR_H + (HDR_H - atlas.glyph_h) / 2, C.TEXT);
            ui.draw_text(run_status_msg,
                out_x + 26 + 8 * atlas.glyph_w,
                TOOLBAR_H + (HDR_H - atlas.glyph_h) / 2, C.TEXT_DIM);

            // Output body
            out_scroll = ui.output_panel(out_lines, out_x,
                TOOLBAR_H + HDR_H, out_w, main_h - HDR_H, out_scroll);
            output_view.style.left       = `${out_x}px`;
            output_view.style.top        = `${TOOLBAR_H + HDR_H}px`;
            output_view.style.width      = `${out_w}px`;
            output_view.style.height     = `${main_h - HDR_H}px`;
            output_view.style.font       = `${atlas.glyph_h - 4}px monospace`;
            output_view.style.lineHeight = `${atlas.glyph_h}px`;
        }

        // ── Status bar ────────────────────────────────────────────────────────
        const sb_y = vp_h - STATUS_H;
        ui.panel(0, sb_y, vp_w, STATUS_H, C.SURFACE);
        ui.separator(0, sb_y, vp_w, C.BORDER);
        let sbx = 12;
        ui.draw_text('NSCode', sbx, sb_y + (STATUS_H - atlas.glyph_h) / 2, C.ACCENT);
        sbx += 8 * atlas.glyph_w;
        const { line, col } = buf.cursor;
        let ed_status = `Ln ${line+1}, Col ${col+1}  \u2022  ${buf.line_count()} lines`;
        if (find_state.open && find_state.query) {
            ed_status += `  \u2022  ${find_state.matches.length} match${find_state.matches.length !== 1 ? 'es' : ''}`;
        }
        ui.draw_text(ed_status, sbx, sb_y + (STATUS_H - atlas.glyph_h) / 2, C.TEXT_DIM);
        const badge   = 'WebGPU \u2713';
        const badge_x = vp_w - badge.length * atlas.glyph_w - 12;
        ui.draw_text(badge, badge_x, sb_y + (STATUS_H - atlas.glyph_h) / 2, C.GREEN);

        // ── Overlays (rendered on top of everything) ───────────────────────────
        // Switch to overlay pass: updates mouse state without clearing the draw list,
        // so overlays are drawn on top of the main frame content.
        if (ov_open) {
            ui.begin_overlay(mx, my, mouse_down, just_down, just_up);

            if (find_state.open) {
                const fa = ui.find_bar(find_state, editor_x, TOOLBAR_H, editor_w,
                    find_state.matches.length);
                if (fa === 'close')       find_state.open = false;
                else if (fa === 'next')   find_next();
                else if (fa === 'prev')   find_prev();
                else if (fa === 'replace') do_replace();
                else if (fa === 'replace_all') do_replace_all();
            }

            if (goto_state.open) {
                ui.goto_overlay(goto_state.query, editor_x, TOOLBAR_H, editor_w, buf.line_count());
            }

            if (kb_state.open) {
                const bindings      = key_map_instance.get_all();
                const edit_state_ref = { id: kb_state.edit_id };
                const closed = ui.keybindings_overlay(bindings, edit_state_ref);
                kb_state.edit_id = edit_state_ref.id;
                if (closed) { kb_state.open = false; kb_state.edit_id = null; }
            }

            if (palette.open) {
                const cmds     = build_commands();
                const filtered = fuzzy_filter(palette.query, cmds, c => c.label);
                palette.sel    = Math.min(palette.sel, Math.max(0, filtered.length - 1));
                const picked   = ui.command_palette(palette.query, filtered, palette.sel);
                if (picked === '__close__') { palette.open = false; }
                else if (picked) { execute_command(picked); palette.open = false; }
            }
        }

        // ── Render ────────────────────────────────────────────────────────────
        gpu.render(ui.end_frame());

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
