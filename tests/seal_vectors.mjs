/**
 * seal_vectors.mjs — cross-language check for the sealed envelope (Phase 7.4).
 *
 * The C++ server and the JS client implement the same construction twice. This
 * script proves they agree, by pinning both to fixed byte strings rather than
 * to each other.
 *
 * It is the highest-value test in this sub-phase. A one-byte divergence in the
 * KDF is invisible: everything compiles, everything runs, and login just fails
 * with "invalid credentials" forever. Nobody suspects the key schedule.
 *
 * Usage:
 *   node tests/seal_vectors.mjs tests/seal_vectors.json
 *
 * The vector file is produced by tools/gen_seal_vectors.cpp and committed, so a
 * later change to either implementation shows up as a failure here.
 */

import { readFileSync } from 'node:fs';
import {
  deriveSealKeys, sealWithMaster, toBase64, fromBase64,
} from '../frontend/js/net/sealed.js';

const path = process.argv[2] ?? 'tests/seal_vectors.json';

let passed = 0;
let failed = 0;

function check(name, ok) {
  console.log(`  [TEST] ${name}... ${ok ? 'PASS' : 'FAIL'}`);
  ok ? passed++ : failed++;
}

const hex = (b) => Buffer.from(b).toString('hex');
const unhex = (s) => new Uint8Array(Buffer.from(s, 'hex'));

const data = JSON.parse(readFileSync(path, 'utf8'));

console.log('========================================');
console.log(' Cross-language seal vectors - Phase 7.4');
console.log('========================================');
console.log(`\n  source : ${path}`);
console.log(`  kdf    : ${data.kdf}`);
console.log(`  labels : "${data.enc_label}" / "${data.mac_label}"\n`);

for (const [i, v] of data.vectors.entries()) {
  const master = unhex(v.master);
  const keyId = unhex(v.key_id);
  const iv = unhex(v.iv);
  const payload = unhex(v.payload);
  const label = `vector ${i} (payload ${payload.length} B)`;

  // 1. The KDF on its own. If this diverges, nothing downstream can agree,
  //    so it is worth failing on separately rather than only via the tag.
  const keys = await deriveSealKeys(master, data.context);
  check(`${label}: k_enc matches C++`, hex(keys.enc) === v.k_enc);
  check(`${label}: k_mac matches C++`, hex(keys.mac) === v.k_mac);

  // 2. The full Encrypt-then-MAC output.
  const { ct, tag } = await sealWithMaster(master, keyId, iv, payload, data.context);
  check(`${label}: ciphertext matches C++`, hex(ct) === v.ct);
  check(`${label}: tag matches C++`, hex(tag) === v.tag);

  // 3. base64 must agree too — it is how every one of these fields travels.
  check(`${label}: base64 matches C++`, toBase64(ct) === v.ct_b64);

  // 4. And decode back, since the JS decoder is strict where atob is not.
  const back = fromBase64(v.ct_b64, ct.length);
  check(`${label}: base64 round-trips`, back !== null && hex(back) === v.ct);
}

// Strictness checks for the JS decoder, mirroring test_sealed_envelope.cpp.
check('base64 rejects whitespace', fromBase64('Zm9v YmFy') === null);
check('base64 rejects bad length', fromBase64('Zm9vYmF') === null);
check('base64 rejects non-canonical trailing bits', fromBase64('Zh==') === null);
check('base64 enforces expectedLen', fromBase64('Zm9v', 4) === null);
check('base64 accepts a canonical value', hex(fromBase64('Zg==')) === '66');

console.log('\n========================================');
console.log(` Results: ${passed} passed, ${failed} failed`);
console.log('========================================');

process.exit(failed > 0 ? 1 : 0);
