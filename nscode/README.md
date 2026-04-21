# NSCode — NanoScript Playground

A WebGPU-based code editor and interpreter for NanoScript, running entirely in the browser.

## Requirements

- A browser with WebGPU support (Chrome 113+, Edge 113+)
- Node.js 18+

## Local Development (HTTPS)

WebGPU requires a [secure context](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts) (`https://` or `localhost`). The included dev server handles this with a self-signed certificate.

### 1. Generate a local certificate (one-time)

```sh
openssl req -x509 -newkey rsa:2048 \
  -keyout dev-key.pem -out dev-cert.pem \
  -days 3650 -nodes \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

The cert files are gitignored and only needed locally.

### 2. Start the dev server

```sh
npm run dev
# → HTTPS dev server → https://localhost:8443
```

Or specify a custom port:

```sh
node serve-https.js 9000
# → https://localhost:9000
```

### 3. Open in browser

Navigate to `https://localhost:8443`. On first visit, accept the self-signed certificate warning:

- **Chrome/Edge**: click _Advanced_ → _Proceed to localhost_
- **Firefox**: click _Advanced_ → _Accept the Risk and Continue_

After accepting once, the warning won't appear again for this cert.

## Deploy

Deploy the static app to `/var/www/ns`:

```sh
npm run ci
```

Deploy to a different directory:

```sh
npm run ci -- /tmp/ns-deploy
```

## Project Structure

```
nscode/
├── index.html          # Entry point
├── serve-https.js      # HTTPS dev server
├── src/
│   ├── main.js         # App entry, event loop
│   ├── editor.js       # Text editor buffer
│   ├── interpreter.js  # NanoScript interpreter
│   ├── ui.js           # UI layout and widgets
│   ├── draw.js         # Draw list (rects + text)
│   ├── gpu.js          # WebGPU rendering backend
│   ├── font.js         # MSDF font atlas
│   ├── syntax.js       # Syntax highlighting
│   └── fonts/          # Font assets
└── public/             # Static assets
```
