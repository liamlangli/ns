import { cp, mkdir, readdir, rm, stat, copyFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(scriptDir, '..');
const targetDir = path.resolve(process.argv[2] ?? process.env.NSCODE_DEPLOY_DIR ?? '/var/www/ns');

const copyEntries = ['index.html', 'src', 'public'];
const faviconSource = path.resolve(rootDir, '../nslang/ns.png');
const faviconTarget = path.join(targetDir, 'public', 'ns.png');

async function ensureCleanDir(dir) {
    await mkdir(dir, { recursive: true });
    const entries = await readdir(dir, { withFileTypes: true });

    await Promise.all(entries.map(async (entry) => {
        const entryPath = path.join(dir, entry.name);
        await rm(entryPath, { recursive: true, force: true });
    }));
}

async function copyEntry(name) {
    const from = path.join(rootDir, name);
    const to = path.join(targetDir, name);
    const info = await stat(from);

    if (info.isDirectory()) {
        await cp(from, to, { recursive: true });
        return;
    }

    await mkdir(path.dirname(to), { recursive: true });
    await cp(from, to);
}

async function main() {
    await ensureCleanDir(targetDir);
    await Promise.all(copyEntries.map(copyEntry));

    await mkdir(path.dirname(faviconTarget), { recursive: true });
    await copyFile(faviconSource, faviconTarget);

    console.log(`NSCode deployed to ${targetDir}`);
}

main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
