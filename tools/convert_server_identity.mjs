// Re-encode an existing raw key for text-only upload. Base64 is NOT encryption.
import { readFileSync, writeFileSync } from 'node:fs';

if (process.argv.length !== 4 || process.argv[2] === process.argv[3]) {
  console.error('Usage: node tools/convert_server_identity.mjs <raw-key> <new-text-key>');
  process.exitCode = 1;
} else {
  let raw;
  let text;
  try {
    raw = readFileSync(process.argv[2]);
    if (raw.length !== 4032) throw new Error('Invalid raw length');
    text = Buffer.from(`CHESS-ML-DSA-65-PRIVATE-KEY-V1\n${raw.toString('base64')}\n`, 'ascii');
    writeFileSync(process.argv[3], text, { flag: 'wx', mode: 0o600 });
    console.log('Text-safe identity file created; private contents were not printed.');
  } catch (error) {
    console.error(error.code === 'EEXIST'
      ? 'Refusing to overwrite an existing file.'
      : 'Identity conversion failed; check source size, paths and permissions.');
    process.exitCode = 1;
  } finally {
    raw?.fill(0);
    text?.fill(0);
  }
}
