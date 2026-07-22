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
fn main() i32 {
    if gpu_request_device(browser_view) {
        return answer() - ${answer}
    }
    return 1
}
fn frame(time_ms: f64, width: i32, height: i32) { }
`;
fs.writeFileSync(path.join(root, 'ns.mod'), 'schema = "ns.mod/v1"\nname = "wasm-e2e"\ntype = "app"\ntarget = "wasm"\nsource = "."\nentry = "main.ns"\n');
fs.writeFileSync(path.join(root, 'main.ns'), source());
fs.mkdirSync(path.join(root, 'assets'));
fs.writeFileSync(path.join(root, 'assets', 'fixture.txt'), 'asset payload');
fs.mkdirSync(path.join(root, 'bin'));
fs.writeFileSync(path.join(root, 'bin', 'keep.txt'), 'unrelated output');
fs.mkdirSync(path.join(root, 'bin', 'assets'));
fs.writeFileSync(path.join(root, 'bin', 'assets', 'stale.txt'), 'remove me');

const relocated = path.join(root, 'dist', 'custom.wasm');
const relocatedBuild = spawnSync(ns, ['build', root, '-o', relocated], { encoding: 'utf8' });
assert.strictEqual(relocatedBuild.status, 0, relocatedBuild.stdout + relocatedBuild.stderr);
for (const file of ['custom.wasm', 'ns-wasm.js', 'index.html', 'assets/fixture.txt']) {
  assert(fs.existsSync(path.join(root, 'dist', file)), `missing relocated bundle file ${file}`);
}
assert.match(fs.readFileSync(path.join(root, 'dist', 'index.html'), 'utf8'), /custom\.wasm/);

const shaderWasm = path.join(root, 'shader-metadata.wasm');
const shaderBuild = spawnSync(ns, ['--wasm', path.resolve('test/gpu_pipeline_test.ns'), '-o', shaderWasm], { encoding: 'utf8' });
assert.strictEqual(shaderBuild.status, 0, shaderBuild.stdout + shaderBuild.stderr);
const shaderModule = new WebAssembly.Module(fs.readFileSync(shaderWasm));
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
fs.writeFileSync(path.join(unsupportedRoot, 'main.ns'), 'use os\nfn main() {}\n');
const unsupportedBuild = spawnSync(ns, ['build', unsupportedRoot], { encoding: 'utf8' });
assert.notStrictEqual(unsupportedBuild.status, 0);
assert.match(unsupportedBuild.stdout + unsupportedBuild.stderr, /does not support module `os`/);
fs.writeFileSync(path.join(unsupportedRoot, 'main.ns'), 'use std\nfn main() { let fd = open("x", "r") }\n');
const unsupportedImport = spawnSync(ns, ['build', unsupportedRoot], { encoding: 'utf8' });
assert.notStrictEqual(unsupportedImport.status, 0);
assert.match(unsupportedImport.stdout + unsupportedImport.stderr, /does not support import `std\.open`/);
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
    const wasm = fs.readFileSync(wasmPath);
    assert(WebAssembly.validate(wasm));
    const instance = await WebAssembly.instantiate(wasm, {
      view: { view_create: () => 512 },
      gpu: { gpu_request_device: pointer => pointer === 512 ? 1 : 0 },
    });
    instance.instance.exports.__ns_init();
    assert.strictEqual(instance.instance.exports.main(), 0);
    for (const file of ['/', '/ns-wasm.js', '/wasm-e2e.wasm']) assert.strictEqual((await fetch(base + file)).status, 200);
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

    fs.writeFileSync(path.join(root, 'main.ns'), 'fn main() { let broken = }\n');
    await deadline(() => messages.includes('build-error'));
    assert.strictEqual(hash(wasmPath), goodHash);
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
