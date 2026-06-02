// Static build for GitHub Pages (and any static host).
//
// Bundles the TypeScript entry (src/main.ts) into a single ESM module
// (dist/main.js) via esbuild, emits a production index.html that loads the
// compiled bundle, and copies the runtime assets (public/, favicon) alongside.
//
// Usage:
//   node scripts/build.mjs            # → ./dist
//   node scripts/build.mjs ./out      # → ./out

import { build } from 'esbuild';
import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const script_dir = path.dirname(fileURLToPath(import.meta.url));
const root_dir   = path.resolve(script_dir, '..');
const out_dir    = path.resolve(process.argv[2] ?? path.join(root_dir, 'dist'));

async function main() {
    // Clean output directory.
    await rm(out_dir, { recursive: true, force: true });
    await mkdir(out_dir, { recursive: true });

    // Bundle the app. `new URL('../public/*.wasm', import.meta.url)` references
    // in the source are emitted next to the bundle via the copy loader so the
    // runtime fetch resolves relative to main.js.
    await build({
        entryPoints: [path.join(root_dir, 'src', 'main.ts')],
        outfile:     path.join(out_dir, 'main.js'),
        bundle:      true,
        format:      'esm',
        target:      'es2022',
        minify:      true,
        sourcemap:   true,
        legalComments: 'none',
        loader: {
            '.wasm':  'copy',
            '.png':   'copy',
            '.ttf':   'copy',
            '.woff':  'copy',
            '.woff2': 'copy',
        },
        assetNames: '[name]',
        logLevel:   'info',
    });

    // Emit a production index.html that loads the compiled bundle and resolves
    // the favicon relative to the page (project-page friendly, no leading /).
    const src_html = await readFile(path.join(root_dir, 'index.html'), 'utf8');
    const out_html = src_html
        .replace('src="src/main.ts"', 'src="./main.js"')
        .replace('href="/ns.png"', 'href="./ns.png"');
    await writeFile(path.join(out_dir, 'index.html'), out_html);

    // Runtime assets.
    if (existsSync(path.join(root_dir, 'public'))) {
        await cp(path.join(root_dir, 'public'), path.join(out_dir, 'public'), { recursive: true });
    }

    // Favicon lives in the sibling nslang package.
    const favicon = path.resolve(root_dir, '..', 'nslang', 'ns.png');
    if (existsSync(favicon)) {
        await cp(favicon, path.join(out_dir, 'ns.png'));
    }

    console.log(`NSCode built to ${out_dir}`);
}

main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
