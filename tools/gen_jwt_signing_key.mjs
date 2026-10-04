// Generate a deployment secret without printing it or overwriting existing keys.
import { randomBytes } from 'node:crypto';
import { writeFileSync } from 'node:fs';

const destination = process.argv[2];
if (!destination || process.argv.length !== 3) {
  console.error('Usage: node tools/gen_jwt_signing_key.mjs <new-key-file>');
  process.exitCode = 1;
} else {
  const key = randomBytes(32);
  try {
    // TokenSigner requires strict base64: no trailing newline.
    writeFileSync(destination, key.toString('base64'), { flag: 'wx', mode: 0o600 });
    console.log('JWT signing key created. Secret contents were not printed.');
  } catch (error) {
    console.error(error.code === 'EEXIST'
      ? 'Refusing to overwrite an existing key.'
      : 'Unable to create key file; check the destination and permissions.');
    process.exitCode = 1;
  } finally {
    key.fill(0);
  }
}
