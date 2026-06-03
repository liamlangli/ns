// @ts-nocheck
// NSCode — GPU-rendered code playground (no HTML/CSS UI elements)
// All UI rendered via the @liamlangli/ui WebGPU renderer (text + rect pipelines)
// on a single WebGPU canvas, driven through the UI immediate-mode wrapper.

import { text_buffer }           from './editor.ts';
import { ns_interpreter, parse_to_ast } from './interpreter.ts';
import { gpu_parser }            from './gpu_parser.ts';
import { compare_normalized_asts, normalize_cpu_ast_by_function } from './parser_validation.ts';
import {
    UI, C, create_empty_ui_input,
    create_default_dock_layout, compute_dock_frame, restore_dock_layout,
    serialize_dock_layout, activate_dock_tab, set_dock_split_ratio,
    move_dock_tab, split_dock_tab, close_dock_tab, resolve_dock_drop,
    find_leaf_by_id, visit_dock_leaves,
    default_themes, lerp_theme, NS_DEFAULT_THEME, apply_theme,
} from './ui.ts';
import { fuzzy_filter, key_map, event_key_id } from './commands.ts';
import { WebGPUModule }         from './webgpu_module.ts';

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

    // ── WebGPU examples (NanoScript with gpu_* built-in functions) ──────────
    webgpu_triangle: `// WebGPU Triangle
// Built-in gpu_* functions connect NanoScript to the WebGPU preview canvas.
//   gpu_shader(wgsl)       — compile WGSL source, return shader handle
//   gpu_pipeline(shader)   — build a triangle-list pipeline (vs + fs entry pts)
//   gpu_render(pipeline, n) — clear, draw n vertices, submit

let wgsl = "
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
"

let shader   = gpu_shader(wgsl)
let pipeline = gpu_pipeline(shader)
gpu_render(pipeline, 3)
println("Triangle rendered!")
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
    hidden_input.focus();

    // The output console and parse-texture view are now rendered entirely
    // through the @liamlangli/ui WebGPU renderer (no DOM overlays). See the
    // output text_view + GPU parse texture below.

    // ── Renderer / device bootstrap ──────────────────────────────────────────
    // The UI owns the @liamlangli/ui WebGPU renderer; ui.init() creates the
    // GPUDevice that the parser/preview modules share.
    const ui = new UI(canvas);
    const device = await ui.init();
    const font = ui.font;

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

    const buf = new text_buffer(EXAMPLES.fib);
    let active_example = 'fib';

    // ── Output state ──────────────────────────────────────────────────────────
    // Scroll + selection state lives in `ui.out_state` (the text_view widget).
    let out_lines      = [];
    let run_status     = 'idle';
    let run_status_msg = 'Ready';

    // ── Parse texture state ───────────────────────────────────────────────────
    let parse_tex_fn_count = 0;   // 0 = no texture to show
    let parse_tex_id       = -1;  // GPU texture handle (@liamlangli/ui renderer)
    let parse_tex_w        = 0;
    let parse_tex_h        = 0;

    /**
     * Build a per-function parse texture as a GPU texture (no DOM canvas).
     * Each row = one function, each pixel column = one token.
     * Pixel color encodes token kind (RGBA u8). Uploaded via the renderer and
     * drawn each frame with nearest-neighbour sampling.
     */
    function build_parse_texture(gpu_result) {
        const fn_count = gpu_result.function_spans.length;
        if (fn_count === 0) { parse_tex_fn_count = 0; return; }

        // Group tokens by function and sort by source position
        const fn_tokens = Array.from({ length: fn_count }, () => []);
        for (const tok of gpu_result.tokens) {
            if (tok.functionId < fn_count) fn_tokens[tok.functionId].push(tok);
        }
        for (const arr of fn_tokens) arr.sort((a, b) => a.start - b.start);

        let max_tok = 0;
        for (const arr of fn_tokens) max_tok = Math.max(max_tok, arr.length);
        if (max_tok === 0) { parse_tex_fn_count = 0; return; }

        // Token kind → RGBA u8 color
        // kind 0/unknown: dark bg | kind 1: identifier (blue) | kind 2: literal (green) | kind 3: symbol (orange)
        const KIND_COLORS = [
            [30,  30,  40,  255],   // 0: empty / unknown
            [80,  160, 255, 255],   // 1: identifier  — blue
            [80,  255, 140, 255],   // 2: literal      — green
            [255, 160,  60, 255],   // 3: symbol       — orange
        ];
        const BG = KIND_COLORS[0];

        const pixels = new Uint8ClampedArray(max_tok * fn_count * 4);
        // Fill background
        for (let i = 0; i < pixels.length; i += 4) {
            pixels[i]   = BG[0]; pixels[i+1] = BG[1];
            pixels[i+2] = BG[2]; pixels[i+3] = BG[3];
        }
        // Fill token pixels
        for (let fi = 0; fi < fn_count; fi++) {
            const toks = fn_tokens[fi];
            for (let ti = 0; ti < toks.length && ti < max_tok; ti++) {
                const col = KIND_COLORS[toks[ti].kind] ?? KIND_COLORS[0];
                const base = (fi * max_tok + ti) * 4;
                pixels[base]   = col[0]; pixels[base+1] = col[1];
                pixels[base+2] = col[2]; pixels[base+3] = col[3];
            }
        }

        // Upload to a GPU texture (recreate only when dimensions change).
        if (parse_tex_id < 0 || parse_tex_w !== max_tok || parse_tex_h !== fn_count) {
            if (parse_tex_id >= 0) ui.destroy_texture(parse_tex_id);
            parse_tex_id = ui.create_texture(max_tok, fn_count, { filter: 'nearest' });
            parse_tex_w  = max_tok;
            parse_tex_h  = fn_count;
        }
        ui.update_texture(parse_tex_id, pixels, { width: max_tok, height: fn_count });
        parse_tex_fn_count = fn_count;
    }

    // ── Dock layout state ──────────────────────────────────────────────────────
    // Panels live in a @liamlangli/ui dock tree: Files | Editor | Output, with
    // contextual Preview / Parse tabs. Persisted to localStorage.
    const DOCK_LS_KEY = 'ns.dockLayout';

    function make_ns_layout() {
        const leaf = (id, tabs, active) => ({ kind: 'leaf', id, tabs, active_tab_id: active, ox: 0, oy: 0, ow: 1, oh: 1 });
        return {
            root: {
                kind: 'split', id: 'split-root', axis: 'horizontal', ratio: 0.18,
                left:  leaf('leaf-tree', [{ id: 'files', title: 'Files' }], 'files'),
                right: {
                    kind: 'split', id: 'split-main', axis: 'horizontal', ratio: 0.58,
                    left:  leaf('leaf-editor', [{ id: 'editor', title: 'Editor' }], 'editor'),
                    right: leaf('leaf-panel', [{ id: 'console', title: 'Output' }], 'console'),
                },
            },
            next_id: 20,
            last_active_leaf_id: 'leaf-editor',
        };
    }

    function layout_has_essentials(l) {
        if (!l || !l.root) return false;
        let editor = false, console_ = false;
        visit_dock_leaves(l.root, leaf => {
            for (const t of leaf.tabs) {
                if (t.id === 'editor')  editor  = true;
                if (t.id === 'console') console_ = true;
            }
        });
        return editor && console_;
    }

    let dock_layout = make_ns_layout();
    try {
        const raw = globalThis.localStorage?.getItem(DOCK_LS_KEY);
        if (raw) {
            const restored = restore_dock_layout(JSON.parse(raw));
            if (restored && layout_has_essentials(restored)) dock_layout = restored;
        }
    } catch { /* ignore malformed saved layout */ }

    function save_dock_layout() {
        try { globalThis.localStorage?.setItem(DOCK_LS_KEY, serialize_dock_layout(dock_layout)); }
        catch { /* storage unavailable */ }
    }

    const split_drag = { active: false, id: null };
    const tab_drag   = { active: false, armed: false, leaf: null, tab: null, sx: 0, sy: 0 };
    let   last_dock_frame = { leaves: [], splits: [] };  // for wheel routing
    let   prev_webgpu_mode = false;

    function find_leaf_with_tab(tid) {
        let r = null;
        visit_dock_leaves(dock_layout.root, leaf => { if (leaf.tabs.some(t => t.id === tid)) r = leaf; });
        return r;
    }
    function host_panel_leaf() {
        return find_leaf_with_tab('console')
            ?? find_leaf_by_id(dock_layout, 'leaf-panel')
            ?? (() => { let f = null; visit_dock_leaves(dock_layout.root, l => { if (!f) f = l; }); return f; })();
    }
    /** Toggle the Files sidebar: collapse to its sibling, or re-add it on the left. */
    function toggle_files_panel() {
        const leaf = find_leaf_with_tab('files');
        if (leaf) {
            close_dock_tab(dock_layout, leaf.id, 'files');
        } else {
            dock_layout = {
                root: {
                    kind: 'split', id: `split-${dock_layout.next_id++}`, axis: 'horizontal', ratio: 0.18,
                    left:  { kind: 'leaf', id: `leaf-${dock_layout.next_id++}`, tabs: [{ id: 'files', title: 'Files' }], active_tab_id: 'files', ox: 0, oy: 0, ow: 1, oh: 1 },
                    right: dock_layout.root,
                },
                next_id: dock_layout.next_id,
                last_active_leaf_id: dock_layout.last_active_leaf_id,
            };
        }
        save_dock_layout();
        dirty = true;
    }

    /** Add or remove a contextual tab (preview / parse) without pruning leaves. */
    function ensure_tab(tid, title, present) {
        const leaf = find_leaf_with_tab(tid);
        if (present && !leaf) {
            host_panel_leaf()?.tabs.push({ id: tid, title });
        } else if (!present && leaf) {
            const i = leaf.tabs.findIndex(t => t.id === tid);
            if (i >= 0) leaf.tabs.splice(i, 1);
            if (leaf.active_tab_id === tid) leaf.active_tab_id = leaf.tabs[0]?.id ?? '';
        }
    }

    // ── Cursor blink ──────────────────────────────────────────────────────────
    let cursor_visible = true;
    let blink_t = 0;

    // ── Dirty flag — render only when something actually changed ──────────────
    let dirty = true;   // true on first frame to draw initial UI

    // ── Mouse state ───────────────────────────────────────────────────────────
    let mx = 0, my = 0, mouse_down = false, just_down = false, just_up = false;
    let vp_w = 0, vp_h = 0;

    // ── @liamlangli/ui input accumulators (for the output text_view) ──────────
    // Edge-triggered keys + wheel delta, consumed once per frame then reset.
    let out_wheel_y = 0;
    let mod_shift = false, mod_ctrl = false, mod_meta = false;
    const out_keys = { up: 0, down: 0, pgup: 0, pgdn: 0, home: 0, end: 0, a: 0, c: 0 };

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

    // ── Theme picker (built-in presets + linear cross-fade) ────────────────────
    // NSCode's own look leads the list; the @liamlangli/ui built-ins follow.
    const theme_presets = [
        NS_DEFAULT_THEME,
        ...default_themes.filter(p => p.name !== NS_DEFAULT_THEME.name),
    ];
    const theme_ctrl = {
        open:        false,                 // dropdown visible?
        index:       0,                     // active preset
        from:        NS_DEFAULT_THEME.theme,
        to:          NS_DEFAULT_THEME.theme,
        start:       0,
        duration_ms: 360,
        current:     NS_DEFAULT_THEME.theme,
        applied:     null,
    };
    // Restore the saved theme (applied instantly, no cross-fade on load).
    {
        const saved = globalThis.localStorage?.getItem('ns.theme');
        const i = saved ? theme_presets.findIndex(p => p.name === saved) : 0;
        if (i > 0) {
            theme_ctrl.index = i;
            theme_ctrl.from = theme_ctrl.to = theme_ctrl.current = theme_presets[i].theme;
        }
        apply_theme(theme_ctrl.current);
        theme_ctrl.applied = theme_ctrl.current;
    }
    /** Kick off a cross-fade to preset `i` from whatever is on screen now. */
    function select_theme(i, now) {
        if (i < 0 || i >= theme_presets.length || i === theme_ctrl.index) return;
        theme_ctrl.from  = theme_ctrl.current;
        theme_ctrl.to    = theme_presets[i].theme;
        theme_ctrl.start = now;
        theme_ctrl.index = i;
        try { globalThis.localStorage?.setItem('ns.theme', theme_presets[i].name); } catch {}
    }
    /** Advance the cross-fade; keeps the frame dirty until it settles. */
    function tick_theme(now) {
        const t = theme_ctrl.duration_ms <= 0 ? 1
            : Math.min(1, (now - theme_ctrl.start) / theme_ctrl.duration_ms);
        const cur = lerp_theme(theme_ctrl.from, theme_ctrl.to, t);
        theme_ctrl.current = cur;
        if (cur !== theme_ctrl.applied) { apply_theme(cur); theme_ctrl.applied = cur; }
        // lerp_theme returns the `to` reference by identity once settled.
        if (cur !== theme_ctrl.to) dirty = true;
    }

    // ── Viewport sizing ───────────────────────────────────────────────────────
    function resize() {
        vp_w = window.innerWidth;
        vp_h = window.innerHeight;
        ui.resize(vp_w, vp_h);
        dirty = true;
    }
    resize();
    window.addEventListener('resize', resize);

    // Output console colours per line class (consumed by the text_view widget).
    const OUT_CLS_COLOR = {
        print: '#e0e0ec',
        error: '#e05c5c',
        info:  '#888888',
        sep:   '#3a3a48',
        time:  '#d4a44c',
    };

    /** Map `out_lines` ({text, cls}) → text_view lines ({text, color}). */
    function out_view_lines() {
        return out_lines.map(l => ({
            text:  String(l.text),
            color: OUT_CLS_COLOR[l.cls] ?? OUT_CLS_COLOR.print,
        }));
    }

    /** Request the output console to scroll to its last line on the next frame. */
    function scroll_output_to_bottom() {
        ui.out_state.scroll_to_line = Math.max(0, out_lines.length - 1);
        dirty = true;
    }

    // ── Run code ──────────────────────────────────────────────────────────────
    async function run_code() {
        out_lines = [];
        ui.out_state.scroll_top = 0;
        dirty = true;
        run_status = 'run';
        run_status_msg = 'Running…';
        dirty = true;
        await new Promise(r => setTimeout(r, 0));

        // ── WebGPU execution path (NanoScript + injected gpu_* built-ins) ───
        if (WEBGPU_EXAMPLES.has(active_example)) {
            wgpu_module.clear();
            const interp = new ns_interpreter({
                print: v => out_lines.push({ text: String(v), cls: 'print' }),
                error: v => out_lines.push({ text: String(v), cls: 'error' }),
            });
            // Inject gpu_* built-ins into the global scope
            const gpu_globals = wgpu_module.make_ns_globals();
            for (const [name, fn_def] of Object.entries(gpu_globals)) {
                interp.globals.def(name, fn_def);
            }
            const source = buf.get_text();
            let ok = true;
            const t0 = performance.now();
            try {
                const ast = parse_to_ast(source);
                interp.eval_program(ast, interp.globals);
            } catch (e) {
                ok = false;
                out_lines.push({ text: `Error: ${e.message ?? String(e)}`, cls: 'error' });
            }
            const elapsed = (performance.now() - t0).toFixed(2);
            out_lines.push({ text: '─'.repeat(36), cls: 'sep' });
            out_lines.push({ text: `Execute: ${elapsed} ms`, cls: 'time' });
            run_status = ok ? 'ok' : 'err';
            run_status_msg = ok ? 'Success' : 'Error';
            scroll_output_to_bottom();
            dirty = true;
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
                build_parse_texture(gpu_result);
            } catch (e) {
                gpu_parse_ms = performance.now() - gpu_parse_t0;
                out_lines.push({ text: `[GPU parser] parse failed: ${e.message ?? String(e)}`, cls: 'error' });
                parse_tex_fn_count = 0;
            }
        } else {
            out_lines.push({ text: '[GPU parser] unavailable; CPU parser in use.', cls: 'info' });
            parse_tex_fn_count = 0;
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

        out_lines.push({ text: '─'.repeat(36), cls: 'sep' });
        out_lines.push({ text: `CPU parse: ${cpu_parse_ms.toFixed(2)} ms`, cls: 'time' });
        out_lines.push({ text: `GPU parse: ${gpu_parse_ms === null ? 'N/A' : `${gpu_parse_ms.toFixed(2)} ms`}`, cls: 'time' });
        out_lines.push({ text: `Execute: ${execute_ms === null ? 'N/A' : `${execute_ms.toFixed(2)} ms`}`, cls: 'time' });
        out_lines.push({ text: `Total: ${(cpu_parse_ms + (execute_ms ?? 0)).toFixed(2)} ms`, cls: 'time' });
        run_status     = ok ? 'ok' : 'err';
        run_status_msg = ok ? 'Success' : 'Error';
        scroll_output_to_bottom();
        dirty = true;
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
        case 'clear':        out_lines = []; ui.out_state.scroll_top = 0; run_status = 'idle'; run_status_msg = 'Ready'; parse_tex_fn_count = 0; dirty = true; break;
        case 'palette':      palette.open = true; palette.query = ''; palette.sel = 0;
                             find_state.open = false; goto_state.open = false; kb_state.open = false; break;
        case 'find':         open_overlay('find'); break;
        case 'find_replace': open_overlay('replace'); break;
        case 'goto_line':    open_overlay('goto'); break;
        case 'keybindings':  open_overlay('keybindings'); break;
        case 'toggle_tree':  toggle_files_panel(); break;
        case 'select_all':   buf.select_all(); break;
        case 'copy':
            // Output-console copy is handled by the text_view itself (Ctrl/Cmd+C
            // while it holds focus); this path is the editor selection.
            copy_selection();
            break;
        case 'paste':        navigator.clipboard?.readText().then(t => { if (t) buf.insert_text(t); }); break;
        case 'cut':
            copy_selection(); buf.delete_selection(); buf.mark_dirty();
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
    const any_overlay = () => palette.open || find_state.open || goto_state.open || kb_state.open || theme_ctrl.open;

    window.addEventListener('keydown', e => {
        dirty = true;

        // ── Output console focus ───────────────────────────────────────────────
        // While the output text_view holds focus, it owns the keyboard for
        // scrolling, select-all and copy. Capture only the keys it consumes.
        if (ui.out_state.focused && !any_overlay()) {
            mod_shift = e.shiftKey; mod_ctrl = e.ctrlKey; mod_meta = e.metaKey;
            let consumed = true;
            switch (e.key) {
                case 'ArrowUp':   out_keys.up   = 1; break;
                case 'ArrowDown': out_keys.down = 1; break;
                case 'PageUp':    out_keys.pgup = 1; break;
                case 'PageDown':  out_keys.pgdn = 1; break;
                case 'Home':      out_keys.home = 1; break;
                case 'End':       out_keys.end  = 1; break;
                case 'a': case 'A': if (e.ctrlKey || e.metaKey) out_keys.a = 1; else consumed = false; break;
                case 'c': case 'C': if (e.ctrlKey || e.metaKey) out_keys.c = 1; else consumed = false; break;
                case 'Escape':    ui.out_state.focused = false; break;
                default:          consumed = false;
            }
            if (consumed) { e.preventDefault(); return; }
        }

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
        if (handled) { e.preventDefault(); blink_t = 0; cursor_visible = true; dirty = true; }
    });

    // ── Text input routing ─────────────────────────────────────────────────────
    hidden_input.addEventListener('input', e => {
        const t = e.data ?? '';
        hidden_input.value = '';
        if (!t) return;
        dirty = true;
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
        dirty = true;
        e.preventDefault();
    });
    window.addEventListener('mousemove', e => { mx = e.clientX; my = e.clientY; dirty = true; });
    window.addEventListener('mouseup',   e => {
        mx = e.clientX; my = e.clientY;
        mouse_down = false; just_up = true;
        dirty = true;
    });

    canvas.addEventListener('wheel', e => {
        e.preventDefault();
        // Route the wheel to the panel under the cursor, using the dock frame.
        const leaf = last_dock_frame.leaves.find(l =>
            e.clientX >= l.x && e.clientX < l.x + l.w &&
            e.clientY >= l.y && e.clientY < l.y + l.h);
        const tab = leaf?.active_tab_id;
        if (tab === 'editor') {
            buf.scroll_top  = Math.max(0, Math.min((buf.scroll_top ?? 0) + e.deltaY / font.glyph_h, buf.line_count() - 1));
            buf.scroll_left = Math.max(0, (buf.scroll_left ?? 0) + e.deltaX);
        } else if (tab === 'console') {
            // Output console — feed the wheel delta to the text_view widget,
            // which owns its own scroll state and clamps internally.
            out_wheel_y += -e.deltaY / 20;
        }
        dirty = true;
    }, { passive: false });

    // ── Frame loop ────────────────────────────────────────────────────────────
    let last = 0;
    function frame(ts) {
        requestAnimationFrame(frame);
        const dt = ts - last; last = ts;

        // Cursor blink — only marks dirty when visible state actually toggles
        blink_t += dt;
        if (blink_t > 530) { blink_t = 0; cursor_visible = !cursor_visible; dirty = true; }

        if (!dirty) { just_down = false; just_up = false; return; }
        dirty = false;

        // Advance any in-flight theme cross-fade before drawing this frame
        // (re-marks dirty while still animating).
        tick_theme(ts);

        // Layout
        const main_h  = vp_h - TOOLBAR_H - STATUS_H;

        // Begin frame — mouse clicks inside overlays are processed by the overlay widgets;
        // suppress canvas clicks from reaching editor when any overlay is open.
        const ov_open = any_overlay();
        ui.begin_frame(mx, my, mouse_down,
            just_down && !ov_open,
            just_up   && !ov_open,
            vp_w, vp_h);

        // @liamlangli/ui widget pass: build a physical-pixel input snapshot for
        // the output text_view (selection / scroll / copy). Suppressed while an
        // overlay is open so clicks don't fall through to the console.
        const s = ui.scale;
        const wi = create_empty_ui_input();
        wi.mouse_x        = mx * s;
        wi.mouse_y        = my * s;
        wi.mouse_down     = mouse_down && !ov_open;
        wi.mouse_pressed  = just_down && !ov_open;
        wi.mouse_released = just_up   && !ov_open;
        wi.wheel_y        = out_wheel_y;
        wi.shift          = mod_shift;
        wi.ctrl           = mod_ctrl;
        wi.meta           = mod_meta;
        wi.key_up         = !!out_keys.up;
        wi.key_down       = !!out_keys.down;
        wi.key_page_up    = !!out_keys.pgup;
        wi.key_page_down  = !!out_keys.pgdn;
        wi.key_home       = !!out_keys.home;
        wi.key_end        = !!out_keys.end;
        wi.key_a          = !!out_keys.a;
        wi.key_c          = !!out_keys.c;
        ui.widgets_begin(wi);

        // ── Background ────────────────────────────────────────────────────────
        ui.draw_rect(0, 0, vp_w, vp_h, C.BG);

        // ── Toolbar ───────────────────────────────────────────────────────────
        ui.panel(0, 0, vp_w, TOOLBAR_H, C.SURFACE);
        ui.separator(0, TOOLBAR_H - 1, vp_w, C.BORDER);

        let tbx = 12;
        ui.draw_text('NSCode', tbx, (TOOLBAR_H - font.glyph_h) / 2, C.ACCENT);
        tbx += 7 * font.glyph_w + 8;

        ui.dl.rect(tbx, BTN_PAD, 1, BTN_H, C.BORDER[0], C.BORDER[1], C.BORDER[2], 1);
        tbx += 9;

        if (ui.run_button('run', tbx, BTN_PAD, BTN_W, BTN_H)) run_code();
        tbx += BTN_W + 6;

        if (ui.button('clear', 'Clear', tbx, BTN_PAD, BTN_W, BTN_H)) execute_command('clear');
        tbx += BTN_W + 6;

        if (ui.button('palette-btn', 'Cmd+P', tbx, BTN_PAD, BTN_W, BTN_H)) execute_command('palette');
        tbx += BTN_W + 8;

        // Theme picker (right-aligned). Click toggles the preset dropdown,
        // which is drawn in the overlay pass and anchored to this button.
        const theme_label = 'Theme: ' + theme_presets[theme_ctrl.index].name;
        const theme_btn_w = theme_label.length * font.glyph_w + 20;
        const theme_btn_x = vp_w - theme_btn_w - 12;
        if (ui.button('theme-btn', theme_label, theme_btn_x, BTN_PAD, theme_btn_w, BTN_H)) {
            theme_ctrl.open = !theme_ctrl.open;
        }
        const theme_menu_x = theme_btn_x;
        const theme_menu_y = BTN_PAD + BTN_H + 4;

        // Hint (right-aligned, left of the theme button)
        const hint   = 'Ctrl+Enter to run';
        const hint_x = theme_btn_x - hint.length * font.glyph_w - 16;
        ui.draw_text(hint, hint_x, (TOOLBAR_H - font.glyph_h) / 2, C.TEXT_DIM);

        // ── Dock panels (Files | Editor | Output, + contextual Preview/Parse) ──
        const webgpu_mode = WEBGPU_EXAMPLES.has(active_example);

        // Contextual tabs follow the active example / parse data.
        ensure_tab('preview', 'Preview', webgpu_mode);
        ensure_tab('parse',   'Parse',   !webgpu_mode && parse_tex_fn_count > 0);
        if (webgpu_mode && !prev_webgpu_mode) {
            const l = find_leaf_with_tab('preview');
            if (l) activate_dock_tab(dock_layout, l.id, 'preview');
        }
        prev_webgpu_mode = webgpu_mode;

        const dframe = compute_dock_frame(dock_layout.root, 0, TOOLBAR_H, vp_w, main_h, 1);
        last_dock_frame = dframe;

        // Splitters — draw + drag to resize.
        if (!mouse_down) split_drag.active = false;
        for (const sp of dframe.splits) {
            const ratio = ui.dock_splitter(sp, split_drag);
            if (ratio != null) { set_dock_split_ratio(dock_layout, sp.split_id, ratio); dirty = true; }
        }
        if (just_up && split_drag.id) { save_dock_layout(); split_drag.id = null; }

        // Editor leaf rect feeds the find / goto overlays.
        let editor_x = 0, editor_y = TOOLBAR_H, editor_w = 0;
        let preview_visible = false;

        for (const leaf of dframe.leaves) {
            const bar = ui.dock_tabbar(leaf);
            if (bar.activate) {
                activate_dock_tab(dock_layout, leaf.leaf_id, bar.activate);
                save_dock_layout();
                dirty = true;
            }
            if (bar.drag_tab) {
                const t = leaf.tabs.find(tt => tt.id === bar.drag_tab);
                tab_drag.armed = true; tab_drag.active = false;
                tab_drag.leaf = leaf.leaf_id; tab_drag.tab = bar.drag_tab;
                tab_drag.title = t?.title ?? '';
                tab_drag.sx = mx; tab_drag.sy = my;
            }

            const bx = leaf.x, by = leaf.y + leaf.tab_bar_h;
            const bw = leaf.w, bh = leaf.h - leaf.tab_bar_h;
            const busy = tab_drag.active;  // suppress content interaction while dragging a tab

            switch (leaf.active_tab_id) {
                case 'files': {
                    ui.draw_round_rect(bx + 4, by, bw - 8, bh - 4, C.SURFACE, 6);
                    const chosen = ui.file_tree(FILE_TREE, active_example, bx, by, bw, bh);
                    if (chosen && !busy) {
                        active_example = chosen;
                        buf.set_text(EXAMPLES[chosen]);
                        blink_t = 0; cursor_visible = true;
                        dirty = true;
                    }
                    break;
                }
                case 'editor': {
                    editor_x = bx; editor_y = by; editor_w = bw;
                    const hit = ui.code_editor(buf, bx, by, bw, bh,
                        cursor_visible && !ov_open, ui.mono_font,
                        find_state.open ? find_state.matches : null);
                    if (hit && !busy) { buf.move_cursor(hit.line, hit.col, false); blink_t = 0; cursor_visible = true; }
                    break;
                }
                case 'console': {
                    // Run-status dot + message, right-aligned in the tab bar.
                    ui.status_dot(leaf.x + leaf.w - 86, leaf.y + (leaf.tab_bar_h - 8) / 2, 4, run_status);
                    ui.draw_text_clipped(run_status_msg,
                        leaf.x + leaf.w - 74, leaf.y + (leaf.tab_bar_h - font.glyph_h) / 2, 70, C.TEXT_DIM);
                    ui.output_text_view('output', out_view_lines(), bx, by, bw, Math.max(0, bh));
                    break;
                }
                case 'preview': {
                    preview_visible = true;
                    wgpu_module.resize(bx, by, bw, Math.max(0, bh));
                    wgpu_module.show();
                    break;
                }
                case 'parse': {
                    ui.draw_round_rect(bx + 4, by, bw - 8, bh - 4, C.BG, 6);
                    if (parse_tex_id >= 0 && parse_tex_fn_count > 0) {
                        const row_scale = Math.max(1, Math.ceil(64 / parse_tex_fn_count));
                        const tex_h = Math.min(Math.max(0, bh - 8), parse_tex_fn_count * row_scale);
                        ui.draw_texture(parse_tex_id, bx + 4, by + 4, bw - 8, tex_h, { filter: 'nearest' });
                        ui.draw_text(`${parse_tex_fn_count} fn`,
                            bx + bw - `${parse_tex_fn_count} fn`.length * font.glyph_w - 10,
                            by + bh - font.glyph_h - 6, C.TEXT_DIM);
                    }
                    break;
                }
            }
        }

        if (!preview_visible) wgpu_module.hide();

        // ── Tab drag: move / split across leaves ───────────────────────────────
        if (tab_drag.armed && mouse_down && !tab_drag.active &&
            Math.abs(mx - tab_drag.sx) + Math.abs(my - tab_drag.sy) > 6) {
            tab_drag.active = true;
        }
        if (tab_drag.active && mouse_down) {
            const drop = resolve_dock_drop(dframe, mx, my, 1, t => t.length * font.glyph_w);
            const tl = drop.target_leaf_id ? dframe.leaves.find(l => l.leaf_id === drop.target_leaf_id) : null;
            if (tl) {
                let hx = tl.x, hy = tl.y, hw = tl.w, hh = tl.h;
                const tby = tl.y + tl.tab_bar_h, tbh = tl.h - tl.tab_bar_h;
                if      (drop.drop_kind === 'split-left')   { hw = tl.w * 0.5; }
                else if (drop.drop_kind === 'split-right')  { hx = tl.x + tl.w * 0.5; hw = tl.w * 0.5; }
                else if (drop.drop_kind === 'split-top')    { hy = tby; hh = tbh * 0.5; }
                else if (drop.drop_kind === 'split-bottom') { hy = tby + tbh * 0.5; hh = tbh * 0.5; }
                else                                        { hh = tl.tab_bar_h; }
                ui.draw_round_rect(hx, hy, hw, hh, [C.ACCENT[0], C.ACCENT[1], C.ACCENT[2], 0.18], 6);
                ui.draw_round_border(hx, hy, hw, hh, C.ACCENT, 6, 2);
            }
            // Floating tab label following the cursor.
            const lw = ui.tab_width(tab_drag.title);
            ui.draw_round_rect(mx + 10, my + 8, lw, 22, C.SURFACE2, 5);
            ui.draw_text_clipped(tab_drag.title, mx + 18, my + 8 + (22 - font.glyph_h) / 2, lw - 14, C.TEXT);
            ui.request_cursor('grabbing');
            dirty = true;
        }
        if (tab_drag.active && just_up) {
            const drop = resolve_dock_drop(dframe, mx, my, 1, t => t.length * font.glyph_w);
            if (drop.target_leaf_id) {
                if (drop.drop_kind === 'tabbar') {
                    move_dock_tab(dock_layout, tab_drag.leaf, tab_drag.tab, drop.target_leaf_id, drop.target_tab_index);
                } else {
                    split_dock_tab(dock_layout, tab_drag.leaf, tab_drag.tab, drop.target_leaf_id, drop.drop_kind);
                }
                save_dock_layout();
            }
            tab_drag.active = false; tab_drag.armed = false;
            dirty = true;
        }
        if (!mouse_down) { tab_drag.armed = false; tab_drag.active = false; }

        // ── Status bar ────────────────────────────────────────────────────────
        const sb_y = vp_h - STATUS_H;
        ui.panel(0, sb_y, vp_w, STATUS_H, C.SURFACE);
        ui.separator(0, sb_y, vp_w, C.BORDER);
        let sbx = 12;
        ui.draw_text('NSCode', sbx, sb_y + (STATUS_H - font.glyph_h) / 2, C.ACCENT);
        sbx += 8 * font.glyph_w;
        const { line, col } = buf.cursor;
        let ed_status = `Ln ${line+1}, Col ${col+1}  •  ${buf.line_count()} lines`;
        if (find_state.open && find_state.query) {
            ed_status += `  •  ${find_state.matches.length} match${find_state.matches.length !== 1 ? 'es' : ''}`;
        }
        ui.draw_text(ed_status, sbx, sb_y + (STATUS_H - font.glyph_h) / 2, C.TEXT_DIM);
        const badge   = 'WebGPU ✓';
        const badge_x = vp_w - badge.length * font.glyph_w - 12;
        ui.draw_text(badge, badge_x, sb_y + (STATUS_H - font.glyph_h) / 2, C.GREEN);

        // ── Overlays (rendered on top of everything) ───────────────────────────
        // Switch to overlay pass: updates mouse state without clearing the draw list,
        // so overlays are drawn on top of the main frame content.
        if (ov_open) {
            ui.begin_overlay(mx, my, mouse_down, just_down, just_up);

            if (find_state.open) {
                const fa = ui.find_bar(find_state, editor_x, editor_y, editor_w,
                    find_state.matches.length);
                if (fa === 'close')       find_state.open = false;
                else if (fa === 'next')   find_next();
                else if (fa === 'prev')   find_prev();
                else if (fa === 'replace') do_replace();
                else if (fa === 'replace_all') do_replace_all();
            }

            if (goto_state.open) {
                ui.goto_overlay(goto_state.query, editor_x, editor_y, editor_w, buf.line_count());
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

            if (theme_ctrl.open) {
                const picked = ui.theme_menu(theme_presets, theme_ctrl.index, theme_menu_x, theme_menu_y);
                if (picked === '__close__') { theme_ctrl.open = false; }
                else if (picked != null) { select_theme(picked, ts); theme_ctrl.open = false; dirty = true; }
            }
        }

        // ── Render ────────────────────────────────────────────────────────────
        ui.widgets_end();
        ui.render();

        just_down = false;
        just_up   = false;

        // Reset per-frame input edges consumed by the widget pass.
        out_wheel_y = 0;
        out_keys.up = out_keys.down = out_keys.pgup = out_keys.pgdn =
            out_keys.home = out_keys.end = out_keys.a = out_keys.c = 0;
    }

    requestAnimationFrame(frame);
}

main().catch(err => {
    console.error(err);
    document.body.style.cssText = 'margin:0;background:#1e1e24;color:#e05c5c;display:flex;align-items:center;justify-content:center;height:100vh;font:16px monospace;text-align:center;padding:32px';
    document.body.innerHTML = `<div><h2 style="color:#f0a0a0">Error</h2><pre style="color:#ccc;text-align:left">${err.stack ?? err}</pre></div>`;
});
