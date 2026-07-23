const STORAGE = Object.freeze({
  source: 'nscode.source.v2',
  example: 'nscode.example.v2',
  theme: 'nscode.theme.v2',
  sidebar: 'nscode.sidebar.v2',
  sidebar_width: 'nscode.sidebarWidth.v2',
  output_width: 'nscode.outputWidth.v2',
});

const EXAMPLES = Object.freeze([
  {
    id: 'hello',
    title: 'Language tour',
    detail: 'Values, functions, and interpolation',
    source: `// Nano Script runs in the browser through a Nano Script Wasm backend.
fn greet(name: str, score: i32) str {
    let level = "ready"
    if score >= 90 {
        level = "excellent"
    }
    return \`{name} is {level}\`
}

let message = greet("Ada", 96)
println(message)
`,
  },
  {
    id: 'loops',
    title: 'Control flow',
    detail: 'Ranges, branches, and state',
    source: `// Ranges are half-open: this visits 1, 2, 3, 4, and 5.
let total = 0
for value in 1 to 6 {
    total = total + value
}

if total == 15 {
    println("sum verified")
} else {
    println("unexpected result")
}
println(total)
`,
  },
  {
    id: 'recursion',
    title: 'Functions',
    detail: 'Typed parameters and recursion',
    source: `fn fibonacci(n: i32) i32 {
    if n < 2 {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

for n in 0 to 11 {
    let value = fibonacci(n)
    println(\`fib({n}) = {value}\`)
}
`,
  },
  {
    id: 'numbers',
    title: 'Numeric work',
    detail: 'Math and explicit calculations',
    source: `fn distance_squared(x: f64, y: f64) f64 {
    return x * x + y * y
}

let x = 3.0
let y = 4.0
let distance = sqrt(distance_squared(x, y))
println(\`distance = {distance}\`)
`,
  },
]);

const TOKEN_CLASSES = Object.freeze({
  1: 'tok-keyword',
  2: 'tok-type',
  3: 'tok-number',
  4: 'tok-string',
  5: 'tok-comment',
  6: 'tok-function',
  7: 'tok-constant',
});

const $ = selector => document.querySelector(selector);

function escape_html(value) {
  return value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
}

function byte_to_utf16_map(source) {
  const map = [];
  let byte_offset = 0;
  let utf16_offset = 0;
  for (const character of source) {
    const byte_length = new TextEncoder().encode(character).length;
    for (let index = 0; index < byte_length; index++) map[byte_offset + index] = utf16_offset;
    byte_offset += byte_length;
    utf16_offset += character.length;
  }
  map[byte_offset] = utf16_offset;
  return map;
}

function parse_spans(serialized) {
  if (!serialized) return [];
  return serialized.split(';').filter(Boolean).map(entry => {
    const [start, length, kind] = entry.split(':').map(Number);
    return { start, length, kind };
  });
}

function highlighted_html(source, serialized) {
  const byte_map = byte_to_utf16_map(source);
  const spans = parse_spans(serialized);
  let cursor = 0;
  let html = '';
  for (const span of spans) {
    const start = byte_map[span.start] ?? source.length;
    const end = byte_map[span.start + span.length] ?? source.length;
    if (start < cursor) continue;
    html += escape_html(source.slice(cursor, start));
    const class_name = TOKEN_CLASSES[span.kind] ?? '';
    html += `<span class="${class_name}">${escape_html(source.slice(start, end))}</span>`;
    cursor = end;
  }
  html += escape_html(source.slice(cursor));
  return `${html}\n`;
}

function create_splitter(element, on_move) {
  element.addEventListener('pointerdown', event => {
    element.setPointerCapture(event.pointerId);
    element.classList.add('dragging');
  });
  element.addEventListener('pointermove', event => {
    if (!element.hasPointerCapture(event.pointerId)) return;
    on_move(event);
  });
  const release = event => {
    if (element.hasPointerCapture(event.pointerId)) element.releasePointerCapture(event.pointerId);
    element.classList.remove('dragging');
  };
  element.addEventListener('pointerup', release);
  element.addEventListener('pointercancel', release);
}

export function start_app({ instance, runtime }) {
  const exports = instance.exports;
  const editor = $('#source-editor');
  const highlight = $('#highlight-code');
  const line_numbers = $('#line-numbers');
  const output = $('#program-output');
  const workspace = $('#workspace');
  const root = document.documentElement;
  let active_example = localStorage.getItem(STORAGE.example) || EXAMPLES[0].id;
  let saved_source = localStorage.getItem(STORAGE.source);
  let highlight_timer = 0;

  const wasm_string = (name, value) => {
    const argument = runtime.writeString(value);
    const result = exports[name](argument);
    return runtime.readString(result);
  };

  const backend_version = runtime.readString(exports.ns_backend_version());
  $('#backend-label').textContent = backend_version;
  $('#backend-chip').classList.add('ready');
  $('#run-button').disabled = false;

  const initial_theme = localStorage.getItem(STORAGE.theme) ||
    (matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
  root.dataset.theme = initial_theme;

  const sidebar_visible = localStorage.getItem(STORAGE.sidebar) !== 'hidden';
  workspace.classList.toggle('sidebar-hidden', !sidebar_visible);
  const sidebar_width = Number(localStorage.getItem(STORAGE.sidebar_width));
  const output_width = Number(localStorage.getItem(STORAGE.output_width));
  if (sidebar_width >= 170 && sidebar_width <= 380) root.style.setProperty('--sidebar-width', `${sidebar_width}px`);
  if (output_width >= 240) root.style.setProperty('--output-width', `${output_width}px`);

  function selected_example() {
    return EXAMPLES.find(example => example.id === active_example) || EXAMPLES[0];
  }

  function set_status(label, kind = 'ready') {
    $('#run-status').innerHTML = `<span class="status-dot ${kind}"></span>${escape_html(label)}`;
  }

  function update_cursor() {
    const before = editor.value.slice(0, editor.selectionStart);
    const lines = before.split('\n');
    $('#cursor-status').textContent = `Ln ${lines.length}, Col ${lines.at(-1).length + 1}`;
  }

  function sync_scroll() {
    $('#highlight-layer').scrollTop = editor.scrollTop;
    $('#highlight-layer').scrollLeft = editor.scrollLeft;
    line_numbers.scrollTop = editor.scrollTop;
  }

  function update_editor() {
    const source = editor.value;
    const lines = source.split('\n').length;
    line_numbers.textContent = Array.from({ length: lines }, (_, index) => index + 1).join('\n');
    $('#source-stats').textContent = `${lines} lines · ${new TextEncoder().encode(source).length} bytes`;
    $('#dirty-dot').classList.toggle('visible', source !== saved_source);
    localStorage.setItem(STORAGE.source, source);
    clearTimeout(highlight_timer);
    highlight_timer = setTimeout(() => {
      try {
        highlight.innerHTML = highlighted_html(source, wasm_string('ns_highlight', source));
        const diagnostic = wasm_string('ns_diagnostics', source);
        set_status(diagnostic || 'Ready', diagnostic ? 'error' : 'ready');
      } catch (error) {
        set_status('Backend error', 'error');
        console.error(error);
      }
    }, 35);
    update_cursor();
    sync_scroll();
  }

  function set_source(source, name = 'main.ns') {
    editor.value = source;
    saved_source = source;
    $('#file-name').textContent = name;
    editor.scrollTop = 0;
    editor.scrollLeft = 0;
    update_editor();
  }

  function render_examples() {
    const list = $('#example-list');
    list.replaceChildren(...EXAMPLES.map((example, index) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = `example-button${example.id === active_example ? ' active' : ''}`;
      button.innerHTML = `<span class="example-index">${String(index + 1).padStart(2, '0')}</span><span class="example-name">${escape_html(example.title)}</span><span class="example-detail">${escape_html(example.detail)}</span>`;
      button.addEventListener('click', () => {
        active_example = example.id;
        localStorage.setItem(STORAGE.example, active_example);
        localStorage.removeItem(STORAGE.source);
        set_source(example.source);
        render_examples();
        editor.focus();
        if (matchMedia('(max-width: 900px)').matches) toggle_sidebar(false);
      });
      return button;
    }));
  }

  function toggle_sidebar(force) {
    const hidden = typeof force === 'boolean' ? !force : !workspace.classList.contains('sidebar-hidden');
    workspace.classList.toggle('sidebar-hidden', hidden);
    localStorage.setItem(STORAGE.sidebar, hidden ? 'hidden' : 'visible');
  }

  function clear_output() {
    output.textContent = '';
    $('#empty-output').classList.remove('hidden');
    $('#output-state').textContent = 'IDLE';
    $('#output-state').className = 'output-state';
    $('#execution-time').textContent = '—';
  }

  function run_source() {
    const started = performance.now();
    set_status('Running', 'running');
    $('#run-button').disabled = true;
    try {
      const result = wasm_string('ns_run', editor.value);
      const ok = exports.ns_run_ok() === 1;
      output.textContent = result || '(program completed without output)\n';
      $('#empty-output').classList.add('hidden');
      $('#output-state').textContent = ok ? 'COMPLETE' : 'ERROR';
      $('#output-state').className = `output-state ${ok ? 'success' : 'error'}`;
      set_status(ok ? 'Run complete' : 'Run failed', ok ? 'ready' : 'error');
      $('#execution-time').textContent = `${(performance.now() - started).toFixed(1)} ms`;
      output.parentElement.scrollTop = 0;
    } catch (error) {
      output.textContent = `Wasm runtime error: ${error?.message ?? error}\n`;
      $('#empty-output').classList.add('hidden');
      $('#output-state').textContent = 'ERROR';
      $('#output-state').className = 'output-state error';
      set_status('Runtime error', 'error');
    } finally {
      $('#run-button').disabled = false;
    }
  }

  editor.addEventListener('input', update_editor);
  editor.addEventListener('scroll', sync_scroll);
  editor.addEventListener('click', update_cursor);
  editor.addEventListener('keyup', update_cursor);
  editor.addEventListener('keydown', event => {
    if (event.key === 'Tab') {
      event.preventDefault();
      const start = editor.selectionStart;
      const end = editor.selectionEnd;
      editor.setRangeText('    ', start, end, 'end');
      update_editor();
    }
  });

  $('#run-button').addEventListener('click', run_source);
  $('#clear-output').addEventListener('click', clear_output);
  $('#copy-output').addEventListener('click', async () => {
    await navigator.clipboard?.writeText(output.textContent || '');
  });
  $('#sidebar-toggle').addEventListener('click', () => toggle_sidebar());
  $('#theme-button').addEventListener('click', () => {
    const theme = root.dataset.theme === 'dark' ? 'light' : 'dark';
    root.dataset.theme = theme;
    localStorage.setItem(STORAGE.theme, theme);
  });
  $('#open-button').addEventListener('click', () => $('#file-input').click());
  $('#file-input').addEventListener('change', async event => {
    const file = event.target.files?.[0];
    if (!file) return;
    active_example = '';
    set_source(await file.text(), file.name);
    render_examples();
    event.target.value = '';
  });
  $('#save-button').addEventListener('click', () => {
    const blob = new Blob([editor.value], { type: 'text/plain;charset=utf-8' });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = $('#file-name').textContent || 'main.ns';
    link.click();
    URL.revokeObjectURL(link.href);
    saved_source = editor.value;
    update_editor();
  });

  window.addEventListener('keydown', event => {
    const command = event.metaKey || event.ctrlKey;
    if (command && event.key === 'Enter') {
      event.preventDefault();
      run_source();
    } else if (command && event.key.toLowerCase() === 'b') {
      event.preventDefault();
      toggle_sidebar();
    } else if (command && event.key.toLowerCase() === 'l') {
      event.preventDefault();
      clear_output();
    }
  });

  create_splitter($('#sidebar-splitter'), event => {
    const width = Math.max(170, Math.min(380, event.clientX));
    root.style.setProperty('--sidebar-width', `${width}px`);
    localStorage.setItem(STORAGE.sidebar_width, String(width));
  });
  create_splitter($('#output-splitter'), event => {
    const bounds = workspace.getBoundingClientRect();
    const width = Math.max(240, Math.min(bounds.width * .55, bounds.right - event.clientX));
    root.style.setProperty('--output-width', `${width}px`);
    localStorage.setItem(STORAGE.output_width, String(width));
  });

  render_examples();
  set_source(saved_source ?? selected_example().source);
  clear_output();
  editor.focus();
}
