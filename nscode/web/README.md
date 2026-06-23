# NSCode — NanoScript Playground

A WebGPU-based code editor and interpreter for NanoScript, running entirely in the browser.

## Requirements

- A browser with WebGPU support (Chrome 113+, Edge 113+)
- Node.js 18+

## Local Development

WebGPU requires a [secure context](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts). `localhost` is treated as a secure context, so the Vite dev server can run over plain HTTP without a local certificate.

```sh
npm run dev
# → http://localhost:8443
```

Or specify a custom port:

```sh
npm run dev -- --port 9000
# → http://localhost:9000
```

Open `http://localhost:8443` in a browser with WebGPU support.

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
├── vite.config.ts      # Vite dev server config
├── src/
│   ├── main.ts         # App entry, event loop
│   ├── editor.ts       # Text editor buffer
│   ├── interpreter.ts  # NanoScript interpreter
│   ├── ui.ts           # UI adapter backed by @liamlangli/ui
│   ├── draw.ts         # Draw list types and helpers
│   ├── gpu.ts          # Legacy WebGPU backend
│   └── syntax.ts       # Syntax highlighting
└── public/             # Static assets
```
