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

// @liamlangli/ui imports its assets with Vite's `?url` query suffix
// (e.g. `import url from './ui.wgsl?url'`), expecting the import to resolve to
// the asset's URL string. esbuild doesn't recognize that convention: it leaves
// the suffix on the path and emits a literal `import '...?url'` into the
// bundle, which the browser then tries to load as an ES module — failing strict
// MIME-type checks (text/wgsl, image/png, image/webp).
//
// The `file` loader is what turns such an import into a URL string (it emits
// the asset and substitutes the import with the emitted path). The `copy`
// loader, by contrast, preserves the `import` statement, so it can't be used
// for these default imports. Route every `?url` import through the `file`
// loader here; the extension-keyed `copy` loaders below still serve the
// `new URL('...', import.meta.url)` assets the app loads directly.
const url_suffix_plugin = {
    name: 'url-asset',
    setup(build) {
        build.onResolve({ filter: /\?url$/ }, (args) => ({
            path:      path.resolve(args.resolveDir, args.path.replace(/\?url$/, '')),
            namespace: 'url-asset',
        }));
        build.onLoad({ filter: /.*/, namespace: 'url-asset' }, async (args) => ({
            contents: await readFile(args.path),
            loader:   'file',
        }));
    },
};

async function main() {
    // Clean output directory.
    await rm(out_dir, { recursive: true, force: true });
    await mkdir(out_dir, { recursive: true });

    // Bundle the app. esbuild does NOT rewrite `new URL('...', import.meta.url)`
    // asset references — it leaves the specifier literal — so runtime assets like
    // ns_token.wasm are supplied by the `cp public → dist/public` step below
    // rather than emitted by a loader. The literal path is corrected for the
    // bundle's location after the build (see the `../public/` rewrite).
    await build({
        entryPoints: [path.join(root_dir, 'src', 'main.ts')],
        outfile:     path.join(out_dir, 'main.js'),
        bundle:      true,
        format:      'esm',
        target:      'es2022',
        minify:      true,
        sourcemap:   true,
        legalComments: 'none',
        plugins: [url_suffix_plugin],
        loader: {
            '.wasm':  'copy',
            '.png':   'copy',
            '.webp':  'copy',
            '.ttf':   'copy',
            '.woff':  'copy',
            '.woff2': 'copy',
            '.wgsl':  'copy',
        },
        assetNames: '[name]',
        logLevel:   'info',
    });

    // The source resolves the WASM via `new URL('../public/ns_token.wasm',
    // import.meta.url)`, which is correct under Vite dev (src/main.ts sits a
    // level above public/). In the static build the bundle lives at the output
    // root with public/ as a *child* directory, so the `../` would overshoot to
    // the parent of the deploy dir (e.g. /nscode/main.js → /public/...). Rewrite
    // it to `./public/` so the fetch resolves to dist/public/ns_token.wasm.
    const main_path = path.join(out_dir, 'main.js');
    const main_js   = await readFile(main_path, 'utf8');
    await writeFile(main_path, main_js.replaceAll('../public/', './public/'));

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

    // Favicon lives in the repo-root sample directory.
    const favicon = path.resolve(root_dir, '..', 'sample', 'ns.png');
    if (existsSync(favicon)) {
        await cp(favicon, path.join(out_dir, 'ns.png'));
    }

    console.log(`NSCode built to ${out_dir}`);
}

main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
