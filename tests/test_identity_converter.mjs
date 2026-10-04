import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const directory = mkdtempSync(join(tmpdir(), 'chess-converter-test-'));
const tool = fileURLToPath(new URL('../tools/convert_server_identity.mjs', import.meta.url));
const invoke = (...args) => spawnSync(process.execPath, [tool, ...args], { encoding: 'utf8' });
try {
    const source = join(directory, 'source.key');
    const destination = join(directory, 'identity.txt');
    const raw = randomBytes(4032);
    writeFileSync(source, raw, { mode: 0o600 });
    const result = invoke(source, destination);
    assert.equal(result.status, 0);
    const text = readFileSync(destination, 'ascii');
    const lines = text.split('\n');
    assert.equal(lines[0], 'CHESS-ML-DSA-65-PRIVATE-KEY-V1');
    assert.match(lines[1], /^[A-Za-z0-9+/]{5376}$/);
    assert.equal(lines[2], '');
    // Boolean comparisons keep private fixture contents out of assertion output.
    assert.ok(Buffer.from(lines[1], 'base64').equals(raw));
    assert.ok(readFileSync(source).equals(raw));
    assert.ok(!`${result.stdout}${result.stderr}`.includes(lines[1]));
    assert.equal(invoke(source, destination).status, 1);
    assert.ok(readFileSync(destination, 'ascii') === text);
    assert.equal(invoke(source, source).status, 1);
    writeFileSync(join(directory, 'bad.key'), Buffer.alloc(3));
    assert.equal(invoke(join(directory, 'bad.key'), join(directory, 'bad.txt')).status, 1);
    assert.equal(invoke(join(directory, 'missing'), join(directory, 'other.txt')).status, 1);
    assert.equal(invoke().status, 1);
    raw.fill(0);
    console.log('PASS identity converter: exact round trip, unchanged source, no disclosure, no overwrite and error paths.');
} finally {
    rmSync(directory, { recursive: true, force: true }); // Own temporary fixtures only.
}
