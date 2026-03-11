#!/usr/bin/env node
// Simple HTTPS dev server for local debugging
// Usage: node serve-https.js [port]
import https from 'https';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.argv[2] ?? '8443');

const MIME = {
  '.html': 'text/html',
  '.js':   'application/javascript',
  '.css':  'text/css',
  '.json': 'application/json',
  '.png':  'image/png',
  '.wasm': 'application/wasm',
  '.ttf':  'font/ttf',
  '.woff': 'font/woff',
  '.woff2':'font/woff2',
};

const server = https.createServer({
  key:  fs.readFileSync(path.join(__dirname, 'dev-key.pem')),
  cert: fs.readFileSync(path.join(__dirname, 'dev-cert.pem')),
}, (req, res) => {
  let urlPath = req.url.split('?')[0];
  if (urlPath === '/') urlPath = '/index.html';

  const file = path.join(__dirname, urlPath);

  // Prevent path traversal
  if (!file.startsWith(__dirname)) {
    res.writeHead(403); res.end(); return;
  }

  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end('Not found'); return; }
    const ext = path.extname(file);
    res.writeHead(200, {
      'Content-Type': MIME[ext] ?? 'application/octet-stream',
      'Cache-Control': 'no-store',
    });
    res.end(data);
  });
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`HTTPS dev server → https://localhost:${PORT}`);
  console.log('(accept the self-signed cert warning in your browser)');
});
