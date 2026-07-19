import { build } from 'esbuild';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const script_dir = path.dirname(fileURLToPath(import.meta.url));
const root_dir = path.resolve(script_dir, '..');

async function load(entry) {
    const result = await build({ entryPoints: [entry], bundle: true, write: false, platform: 'node', format: 'esm' });
    const source = result.outputFiles[0].text;
    return import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
}

function expect(ok, message) {
    if (!ok) throw new Error(message);
}

const interpreter = await load(path.join(root_dir, 'src', 'interpreter.ts'));
const output = [];
const errors = [];
const runtime = new interpreter.ns_interpreter({ print: value => output.push(value), error: value => errors.push(value) });
const source = `
enum os_platform: u8 {
    unknown = 0,
    macos,
    linux = macos + 4,
}
let platform = os_platform.macos
let underlying: u8 = platform
let restored = underlying as os_platform
let raw = restored | 2
let unnamed = 3 as os_platform
print(platform)
print('|')
print(underlying)
print('|')
print(restored)
print('|')
print(raw)
print('|')
print(unnamed)
`;
runtime.eval_program(interpreter.parse_to_ast(source), runtime.globals);
expect(errors.length === 0, `unexpected interpreter error: ${errors.join(', ')}`);
expect(output.join('') === 'os_platform.macos|1|os_platform.macos|3|os_platform(3)', `unexpected enum output: ${output.join('')}`);

let rejected = false;
try {
    runtime.eval_program(interpreter.parse_to_ast('enum bad: u8 { max = 255, overflow, }'), runtime.globals);
} catch (error) {
    rejected = String(error).includes('outside its underlying type range');
}
expect(rejected, 'enum auto-increment overflow was not rejected');

let bad_shift_rejected = false;
try {
    runtime.eval_program(interpreter.parse_to_ast('enum bad { value = 1 << -1, }'), runtime.globals);
} catch (error) {
    bad_shift_rejected = String(error).includes('invalid shift');
}
expect(bad_shift_rejected, 'enum invalid shift was not rejected');

const tokenizer_wasm = await WebAssembly.instantiate(await readFile(path.join(root_dir, 'public', 'ns_token.wasm')));
const tokenizer = tokenizer_wasm.instance.exports;
const probe = 'kernel enum enumerable';
const probe_bytes = new TextEncoder().encode(probe);
new Uint8Array(tokenizer.memory.buffer, tokenizer.get_src_buf(), probe_bytes.length).set(probe_bytes);
const token_count = tokenizer.tokenize(probe_bytes.length);
const token_data = new Int32Array(tokenizer.memory.buffer, tokenizer.get_tok_buf(), token_count * 3);
const wasm_tokens = [];
for (let i = 0; i < token_count; i++) {
    const type = token_data[i * 3];
    const offset = token_data[i * 3 + 1];
    const length = token_data[i * 3 + 2];
    const token_text = probe.slice(offset, offset + length);
    if (token_text.trim()) wasm_tokens.push([token_text, type]);
}
expect(JSON.stringify(wasm_tokens) === JSON.stringify([['kernel', 73], ['enum', 74], ['enumerable', 48]]),
    `unexpected WASM tokenizer mapping: ${JSON.stringify(wasm_tokens)}`);

const saved_warn = console.warn;
console.warn = () => {};
const syntax = await load(path.join(root_dir, 'src', 'syntax.ts'));
console.warn = saved_warn;
const highlighted = syntax.tokenize_line('enum os_platform: u8 { macos = 1 }');
const find = text => highlighted.find(token => token.text === text)?.type;
expect(find('enum') === syntax.TT.KEYWORD, 'enum keyword highlighting');
expect(find('os_platform') === syntax.TT.TYPE, 'enum type-name highlighting');
expect(find('macos') === syntax.TT.CONSTANT, 'enum member highlighting');
expect(syntax.tokenize_line('kernel fn cs_main() {}').find(token => token.text === 'kernel')?.type === syntax.TT.KEYWORD,
    'existing kernel token mapping remains a keyword');
console.log('web enum tests passed');
