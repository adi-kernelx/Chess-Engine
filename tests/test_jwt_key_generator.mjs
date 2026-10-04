import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const directory = mkdtempSync(join(tmpdir(), 'chess-key-test-'));
const generator = new URL('../tools/gen_jwt_signing_key.mjs', import.meta.url);
// URL paths can contain spaces; fileURLToPath handles Windows and POSIX correctly.
const invoke = (...args) => spawnSync(process.execPath, [fileURLToPath(generator), ...args], { encoding: 'utf8' });
try {
  const first = join(directory, 'first.key');
  const second = join(directory, 'second.key');
  const created = invoke(first);
  assert.equal(created.status, 0);
  const secret = readFileSync(first, 'utf8');
  assert.match(secret, /^[A-Za-z0-9+/]{43}=$/);
  assert.equal(Buffer.from(secret, 'base64').length, 32);
  assert.ok(!`${created.stdout}${created.stderr}`.includes(secret));
  const duplicate = invoke(first);
  assert.equal(duplicate.status, 1);
  assert.match(duplicate.stderr, /Refusing to overwrite/);
  assert.equal(readFileSync(first, 'utf8'), secret);
  assert.equal(invoke(second).status, 0);
  assert.notEqual(readFileSync(second, 'utf8'), secret);
  assert.equal(invoke().status, 1);
  assert.equal(invoke(join(directory, 'missing', 'key')).status, 1);
  console.log('PASS JWT key generation: format, randomness, non-disclosure, exclusive creation and errors.');
} finally {
  // Only this test's freshly created temporary directory is removed.
  rmSync(directory, { recursive: true, force: true });
}
