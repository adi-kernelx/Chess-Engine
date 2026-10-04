// Reproducible, integrity-checked vendoring. No npm lifecycle scripts or CDN.
import { mkdir, writeFile, readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
const root = resolve(import.meta.dirname, '..');
const work = resolve(root, 'build_release/pqc_vendor');
await mkdir(work, { recursive: true });
const manifestPath = resolve(root, 'frontend/js/vendor/pqc-manifest.json');
let locked;
try { locked = JSON.parse(await readFile(manifestPath, 'utf8')); }
catch (error) { if (error.code !== 'ENOENT') throw error; }
const packages = {
  '@noble/post-quantum': '0.7.1', '@noble/hashes': '2.4.0',
  '@noble/curves': '2.4.0', '@noble/ciphers': '2.4.0',
  [process.platform === 'win32' ? '@esbuild/win32-x64' : '@esbuild/linux-x64']: '0.25.12',
};
const manifest = [];
for (const [name, version] of Object.entries(packages)) {
  const response = await fetch(`https://registry.npmjs.org/${name}/${version}`);
  if (!response.ok) throw new Error(`Registry metadata failed: ${name}`);
  const metadata = await response.json();
  const lock = locked?.packages.find(item => item.name === name);
  if (lock && (lock.version !== version || lock.integrity !== metadata.dist.integrity)) {
    throw new Error(`Pinned dependency changed: ${name}`);
  }
  if (locked && name.startsWith('@noble/') && !lock) throw new Error(`Unpinned dependency: ${name}`);
  const archive = await fetch(metadata.dist.tarball);
  if (!archive.ok) throw new Error(`Archive fetch failed: ${name}`);
  const bytes = Buffer.from(await archive.arrayBuffer());
  const integrity = `sha512-${createHash('sha512').update(bytes).digest('base64')}`;
  if (integrity !== metadata.dist.integrity) throw new Error(`Integrity failure: ${name}`);
  manifest.push({ name, version, integrity });
  const target = resolve(work, 'node_modules', name);
  await mkdir(target, { recursive: true });
  const tarball = resolve(work, name.replaceAll('/', '-').replace('@', '') + '.tgz');
  await writeFile(tarball, bytes);
  execFileSync('tar', ['-xzf', tarball, '--strip-components=1', '-C', target]);
}
const entry = resolve(work, 'entry.mjs');
await writeFile(entry, `export { ml_kem768 } from '@noble/post-quantum/ml-kem.js';\nexport { ml_dsa65 } from '@noble/post-quantum/ml-dsa.js';\n`);
const executable = resolve(work, 'node_modules', Object.keys(packages).at(-1),
  process.platform === 'win32' ? 'esbuild.exe' : 'bin/esbuild');
const output = resolve(root, 'frontend/js/vendor/pqc.min.js');
const generated = resolve(work, 'pqc.min.js');
await mkdir(resolve(root, 'frontend/js/vendor'), { recursive: true });
execFileSync(executable, [entry, '--bundle', '--format=esm', '--platform=browser',
  '--target=es2022', '--minify', '--legal-comments=inline', `--outfile=${generated}`]);
const bundle = await readFile(generated);
const digest = createHash('sha256').update(bundle).digest('hex');
if (locked && digest !== locked.bundleSha256) throw new Error('Vendored bundle differs from its pinned digest');
await writeFile(output, bundle);
await writeFile(manifestPath,
  JSON.stringify({ packages: manifest, bundleSha256: digest }, null, 2) + '\n');
for (const name of Object.keys(packages).filter(name => name.startsWith('@noble/'))) {
  await writeFile(resolve(root, 'frontend/js/vendor', name.split('/')[1] + '-LICENSE.txt'),
    await readFile(resolve(work, 'node_modules', name, 'LICENSE')));
}
console.log(`Vendored pinned PQC bundle SHA-256: ${digest}`);
