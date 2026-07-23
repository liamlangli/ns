#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/ns" >&2
    exit 2
fi

node - "$1" <<'NODE'
const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, spawnSync } = require('child_process');
const net = require('net');

const ns = process.argv[2];
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ns-wasm-project-'));
const helperSource = `fn source_map_helper(value: i32) i32 {
    return value + 1
}
`;
const source = (answer = 84) => `use std
use view
use gpu
struct point { x: i32, y: i32 }
struct box { p: point, name: str, values: [i32] }
let global_counter = 40
let browser_view = view_create("Wasm test", 640, 360)
fn touch(q: point) i32 {
    q.x = 99
    return q.x
}
fn answer() i32 {
    let values = [1, 2, 3]
    values[1] = 5
    let scratch = [i32](4)
    scratch[3] = 1
    let text = "wasm"
    let p = point { x: values[1], y: 37 }
    p.x = p.x + scratch[3]
    let p2 = p
    p2.x = 100
    let touched = touch(p)
    let shared = [7, 8]
    let box1 = box { p: p, name: text, values: shared }
    let box2 = box1
    box2.p.x = 70
    box2.values[0] = 9
    global_counter = global_counter + 1
    return p.x + p.y + global_counter + values.len - 3 + text.len - 4 + text[0] - 119 +
        p.x - 6 + p2.x - 100 + touched - 99 + box1.p.x - 6 + box2.p.x - 70 +
        box1.name.len - 4 + box1.values[0] - 9
}
fn string_ops() i32 {
    let joined = "nano" + "script"
    if joined == "nanoscript" {
        if "alpha" < "beta" {
            if "same" <= "same" && "zeta" > "eta" && "zeta" >= "zeta" && "left" != "right" {
                return joined.len
            }
        }
    }
    return 0
}
fn no_result() { }
fn main() i32 {
    no_result()
    if gpu_request_device(browser_view) {
        return answer() - ${answer}
    }
    return 1
}
fn frame(time_ms: f64, width: i32, height: i32) { }
`;
const manifest = (name = 'wasm-e2e', icon = '', shell = '') => `schema = "ns.mod/v1"
name = "${name}"
type = "app"
target = "wasm"
source = "."
entry = "main.ns"
${icon ? `icon = "${icon}"\n` : ''}${shell ? `shell = "${shell}"\n` : ''}`;
fs.writeFileSync(path.join(root, 'ns.mod'), manifest());
fs.writeFileSync(path.join(root, 'main.ns'), source());
fs.writeFileSync(path.join(root, 'helper.ns'), helperSource);
fs.mkdirSync(path.join(root, 'assets'));
fs.writeFileSync(path.join(root, 'assets', 'fixture.txt'), 'asset payload');
fs.mkdirSync(path.join(root, 'bin'));
fs.writeFileSync(path.join(root, 'bin', 'keep.txt'), 'unrelated output');
fs.mkdirSync(path.join(root, 'bin', 'assets'));
fs.writeFileSync(path.join(root, 'bin', 'assets', 'stale.txt'), 'remove me');

const relocated = path.join(root, 'dist', 'custom.wasm');
const relocatedBuild = spawnSync(ns, ['build', root, '-o', relocated], { encoding: 'utf8' });
assert.strictEqual(relocatedBuild.status, 0, relocatedBuild.stdout + relocatedBuild.stderr);
for (const file of ['custom.wasm', 'custom.wasm.map', 'ns-wasm.js', 'ns.svg', 'index.html', 'assets/fixture.txt']) {
  assert(fs.existsSync(path.join(root, 'dist', file)), `missing relocated bundle file ${file}`);
}
const defaultHtml = fs.readFileSync(path.join(root, 'dist', 'index.html'), 'utf8');
assert.match(defaultHtml, /custom\.wasm/);
assert.match(defaultHtml, /<title>wasm-e2e<\/title>/);
assert.match(defaultHtml, /<link rel="icon" href="\.\/ns\.svg">/);
assert.match(defaultHtml, /canvas\{outline:none\}/);
assert.match(fs.readFileSync(path.join(root, 'dist', 'ns.svg'), 'utf8'), /^<svg/);

const customShell = '<!doctype html><title>{{title}}</title><link rel="icon" href="{{favicon}}"><script type="module">globalThis.wasm = "{{wasm}}";</script>\n';
fs.writeFileSync(path.join(root, 'shell.html'), customShell);
fs.writeFileSync(path.join(root, 'ns.mod'), manifest('custom & shell', '', 'shell.html'));
const customShellOutput = path.join(root, 'shell-dist', 'browser.wasm');
const customShellBuild = spawnSync(ns, ['build', root, '-o', customShellOutput], { encoding: 'utf8' });
assert.strictEqual(customShellBuild.status, 0, customShellBuild.stdout + customShellBuild.stderr);
assert.strictEqual(fs.readFileSync(path.join(root, 'shell-dist', 'index.html'), 'utf8'),
  '<!doctype html><title>custom &amp; shell</title><link rel="icon" href="ns.svg"><script type="module">globalThis.wasm = "browser.wasm";</script>\n');

const projectIcon = '<svg xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="8"/></svg>\n';
fs.writeFileSync(path.join(root, 'project-icon.svg'), projectIcon);
fs.writeFileSync(path.join(root, 'ns.mod'), manifest('wasm-e2e & demo', 'project-icon.svg'));
const iconRelocated = path.join(root, 'icon-dist', 'custom.wasm');
const iconBuild = spawnSync(ns, ['build', root, '-o', iconRelocated], { encoding: 'utf8' });
assert.strictEqual(iconBuild.status, 0, iconBuild.stdout + iconBuild.stderr);
assert.strictEqual(fs.readFileSync(path.join(root, 'icon-dist', 'favicon.svg'), 'utf8'), projectIcon);
const iconHtml = fs.readFileSync(path.join(root, 'icon-dist', 'index.html'), 'utf8');
assert.match(iconHtml, /<title>wasm-e2e &amp; demo<\/title>/);
assert.match(iconHtml, /<link rel="icon" href="\.\/favicon\.svg">/);
fs.writeFileSync(path.join(root, 'ns.mod'), manifest());

const customSectionString = (wasm, sectionName) => {
  const sections = WebAssembly.Module.customSections(new WebAssembly.Module(wasm), sectionName);
  assert.strictEqual(sections.length, 1, `missing ${sectionName} custom section`);
  const data = new Uint8Array(sections[0]);
  let cursor = 0, length = 0, shift = 0, byte;
  do { byte = data[cursor++]; length |= (byte & 0x7f) << shift; shift += 7; } while (byte & 0x80);
  assert.strictEqual(cursor + length, data.length);
  return new TextDecoder().decode(data.subarray(cursor));
};
const decodeMappings = map => {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  let generated = 0, sourceIndex = 0, line = 0, column = 0;
  assert(!map.mappings.includes(';'), 'wasm mappings must stay on generated line one');
  return map.mappings.split(',').filter(Boolean).map(segment => {
    let cursor = 0;
    const vlq = () => {
      let value = 0, shift = 0, digit;
      do {
        digit = alphabet.indexOf(segment[cursor++]);
        assert(digit >= 0, `invalid source-map digit in ${segment}`);
        value |= (digit & 31) << shift;
        shift += 5;
      } while (digit & 32);
      return value & 1 ? -(value >> 1) : value >> 1;
    };
    generated += vlq(); sourceIndex += vlq(); line += vlq(); column += vlq();
    assert.strictEqual(cursor, segment.length);
    return { generated, sourceIndex, line, column };
  });
};

const relocatedWasm = fs.readFileSync(relocated);
assert.strictEqual(customSectionString(relocatedWasm, 'sourceMappingURL'), 'custom.wasm.map');
const relocatedMap = JSON.parse(fs.readFileSync(`${relocated}.map`, 'utf8'));
assert.strictEqual(relocatedMap.version, 3);
assert.strictEqual(relocatedMap.file, 'custom.wasm');
for (const file of ['helper.ns', 'main.ns']) assert(relocatedMap.sources.includes(file), `source map missing ${file}`);
assert.strictEqual(relocatedMap.sourcesContent[relocatedMap.sources.indexOf('helper.ns')], helperSource);
assert.strictEqual(relocatedMap.sourcesContent[relocatedMap.sources.indexOf('main.ns')], source());
const relocatedMappings = decodeMappings(relocatedMap);
assert(relocatedMappings.length > 0);
for (let i = 0; i < relocatedMappings.length; ++i) {
  const mapping = relocatedMappings[i];
  assert(mapping.generated < relocatedWasm.length);
  assert(mapping.sourceIndex >= 0 && mapping.sourceIndex < relocatedMap.sources.length);
  assert(mapping.line >= 0 && mapping.line < relocatedMap.sourcesContent[mapping.sourceIndex].split('\n').length);
  if (i > 0) assert(mapping.generated > relocatedMappings[i - 1].generated);
}

const shaderWasm = path.join(root, 'shader-metadata.wasm');
const shaderBuild = spawnSync(ns, ['--wasm', path.resolve('test/gpu_pipeline_test.ns'), '-o', shaderWasm], { encoding: 'utf8' });
assert.strictEqual(shaderBuild.status, 0, shaderBuild.stdout + shaderBuild.stderr);
const shaderModule = new WebAssembly.Module(fs.readFileSync(shaderWasm));
assert.strictEqual(WebAssembly.Module.customSections(shaderModule, 'sourceMappingURL').length, 0);
const shaderSections = WebAssembly.Module.customSections(shaderModule, 'ns.shaders');
assert.strictEqual(shaderSections.length, 1);
assert.match(new TextDecoder().decode(shaderSections[0]), /@vertex/);
{
  const data = new Uint8Array(shaderSections[0]);
  let cursor = 0;
  const leb = () => { let value = 0, shift = 0, byte; do { byte = data[cursor++]; value |= (byte & 0x7f) << shift; shift += 7; } while (byte & 0x80); return value >>> 0; };
  const string = () => { const length = leb(), value = new TextDecoder().decode(data.subarray(cursor, cursor + length)); cursor += length; return value; };
  assert.strictEqual(leb(), 1);
  assert(leb() >= 2);
  leb(); cursor++; string(); string();
  assert.strictEqual(leb(), 24);
  assert.strictEqual(leb(), 3);
  assert.deepStrictEqual([[leb(), leb()], [leb(), leb()], [leb(), leb()]], [[0, 12], [12, 8], [20, 4]]);
}

const unsupportedRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ns-wasm-unsupported-'));
fs.writeFileSync(path.join(unsupportedRoot, 'ns.mod'), 'schema = "ns.mod/v1"\nname = "unsupported"\ntype = "app"\ntarget = "wasm"\nsource = "."\nentry = "main.ns"\n');
fs.writeFileSync(path.join(unsupportedRoot, 'main.ns'), 'use http\nfn main() {}\n');
const unsupportedBuild = spawnSync(ns, ['build', unsupportedRoot], { encoding: 'utf8' });
assert.notStrictEqual(unsupportedBuild.status, 0);
assert.match(unsupportedBuild.stdout + unsupportedBuild.stderr, /does not support module `http`/);
fs.writeFileSync(path.join(unsupportedRoot, 'main.ns'), 'use os\nfn main() { let p = os_platform() }\n');
const unsupportedImport = spawnSync(ns, ['build', unsupportedRoot], { encoding: 'utf8' });
assert.notStrictEqual(unsupportedImport.status, 0);
assert.match(unsupportedImport.stdout + unsupportedImport.stderr, /does not support import `os\.os_platform`/);
fs.writeFileSync(path.join(unsupportedRoot, 'main.ns'), 'use gpu\nfn main() { gpu_request_device(1) }\n');
const untypedDeviceRequest = spawnSync(ns, ['build', unsupportedRoot], { encoding: 'utf8' });
assert.notStrictEqual(untypedDeviceRequest.status, 0);
assert.match(untypedDeviceRequest.stdout + untypedDeviceRequest.stderr, /type mismatch|expected ref view/);
fs.rmSync(unsupportedRoot, { recursive: true, force: true });

// Exercise the documented project-local form: `cd <project> && ns run`.
// Runtime module paths must remain anchored to the ns executable, not cwd.
const child = spawn(ns, ['run', '--port', '0'], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
let output = '';
child.stdout.on('data', d => { output += d; process.stdout.write(d); });
child.stderr.on('data', d => { output += d; process.stderr.write(d); });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const deadline = async (predicate, ms = 10000) => {
  const end = Date.now() + ms;
  while (Date.now() < end) { const value = predicate(); if (value) return value; await sleep(25); }
  throw new Error(`timeout; output:\n${output}`);
};
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const rawRequest = (port, request) => new Promise((resolve, reject) => {
  const socket = net.createConnection({ host: 'localhost', port }, () => socket.write(request));
  let response = '';
  socket.on('data', chunk => {
    response += chunk;
    if (response.includes('\r\n\r\n')) { socket.destroy(); resolve(response); }
  });
  socket.on('error', reject);
});

(async () => {
  try {
    const match = await deadline(() => output.match(/ns wasm server: http:\/\/localhost:(\d+)\//));
    const port = Number(match[1]), base = `http://localhost:${port}`;
    const wasmPath = path.join(root, 'bin', 'wasm-e2e.wasm');
    const mapPath = `${wasmPath}.map`;
    const wasm = fs.readFileSync(wasmPath);
    assert(WebAssembly.validate(wasm));
    const instance = await WebAssembly.instantiate(wasm, {
      view: { view_create: () => 512 },
      gpu: { gpu_request_device: pointer => pointer === 512 ? 1 : 0 },
    });
    instance.instance.exports.__ns_init();
    assert.strictEqual(instance.instance.exports.main(), 0);
    assert.strictEqual(instance.instance.exports.string_ops(), 10);
    for (const file of ['/', '/ns-wasm.js', '/ns.svg', '/wasm-e2e.wasm', '/wasm-e2e.wasm.map']) assert.strictEqual((await fetch(base + file)).status, 200);
    assert.strictEqual((await fetch(base + '/ns.svg')).headers.get('content-type'), 'image/svg+xml');
    const servedMap = await fetch(base + '/wasm-e2e.wasm.map');
    assert.match(servedMap.headers.get('content-type'), /^application\/json/);
    assert.strictEqual((await servedMap.json()).file, 'wasm-e2e.wasm');
    assert.strictEqual(fs.readFileSync(path.join(root, 'bin', 'keep.txt'), 'utf8'), 'unrelated output');
    assert.strictEqual(fs.existsSync(path.join(root, 'bin', 'assets', 'stale.txt')), false);
    assert.strictEqual(await (await fetch(base + '/assets/fixture.txt')).text(), 'asset payload');
    const head = await fetch(base + '/wasm-e2e.wasm', { method: 'HEAD' });
    assert.strictEqual(head.headers.get('content-type'), 'application/wasm');
    assert.strictEqual(await head.text(), '');
    const traversal = await rawRequest(port, 'GET /../main.ns HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n');
    assert.match(traversal, /^HTTP\/1\.1 403 /);
    const handshake = await rawRequest(port,
      'GET /__ns/reload HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n');
    assert.match(handshake, /Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK\+xOo=\r\n/i);

    const socket = new WebSocket(`ws://localhost:${port}/__ns/reload`);
    const socket2 = new WebSocket(`ws://localhost:${port}/__ns/reload`);
    await Promise.all([socket, socket2].map(client => new Promise((resolve, reject) => {
      client.onopen = resolve; client.onerror = reject;
    })));
    const messages = [];
    const messages2 = [];
    socket.onmessage = event => messages.push(JSON.parse(event.data).type);
    socket2.onmessage = event => messages2.push(JSON.parse(event.data).type);

    fs.writeFileSync(path.join(root, 'main.ns'), source() + '// valid edit\n');
    await deadline(() => messages.includes('reload') && messages2.includes('reload'));
    messages.length = 0;
    messages2.length = 0;
    const goodHash = hash(wasmPath);
    const goodMapHash = hash(mapPath);

    fs.writeFileSync(path.join(root, 'main.ns'), 'fn main() { let broken = }\n');
    await deadline(() => messages.includes('build-error'));
    assert.strictEqual(hash(wasmPath), goodHash);
    assert.strictEqual(hash(mapPath), goodMapHash);
    messages.length = 0;

    fs.writeFileSync(path.join(root, 'main.ns'), source());
    await deadline(() => messages.includes('reload'));
    socket.close();
    socket2.close();
    console.log('PASS: relocated/browser bundles, shader metadata, assets, loopback HTTP, RFC WebSocket handshake, multi-client reload, last-good preservation, and recovery.');
  } finally {
    child.kill('SIGTERM');
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); child.kill('SIGTERM'); process.exitCode = 1; });
NODE
