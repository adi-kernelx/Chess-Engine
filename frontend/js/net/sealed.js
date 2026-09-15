/**
 * sealed.js — the browser half of the sealed envelope (Phase 7.4).
 *
 * Mirrors src/crypto/sealed_envelope.cpp exactly:
 *
 *     master = ss_mlkem ‖ ss_x25519                        64 bytes
 *     prk    = HKDF-Extract-SHA-384(salt = "", ikm = master)
 *     k_enc  = HKDF-Expand(prk, "CHESS-SEAL-1 enc", 32)
 *     k_mac  = HKDF-Expand(prk, "CHESS-SEAL-1 mac", 32)
 *     aad    = "CHESS-SEAL-1" ‖ key_id
 *     ct     = AES-256-CTR(k_enc, iv, payload)
 *     tag    = HMAC-SHA-384(k_mac, be64(len(aad)) ‖ aad ‖ iv ‖ ct)
 *
 * Two implementations of one construction is the risk this file carries. A
 * single divergent byte — a different salt convention, a label with a stray
 * space, a 64-bit counter where the server uses 128 — fails silently and looks
 * exactly like a wrong password. tests/seal_vectors.mjs pins both sides to the
 * same fixed byte strings so that can never go unnoticed.
 *
 * WHAT IS HERE AND WHAT IS INJECTED
 *
 * Everything below runs on WebCrypto alone, which every target browser has
 * built in. ML-KEM-768 encapsulation and ML-DSA-65 verification are the two
 * operations WebCrypto does not provide; they arrive through the `pqc` object
 * passed to createSealer(), which is backed by the vendored, pinned
 * @noble/post-quantum build. Keeping that seam explicit means the whole
 * symmetric layer is testable in Node with no third-party code at all.
 */

const SEAL_CONTEXT = 'CHESS-SEAL-1';
const KEY_ID_SIZE = 16;
const IV_SIZE = 16;
const TAG_SIZE = 48;
const KEM_EK_SIZE = 1184;
const KEM_CT_SIZE = 1088;
const X25519_PK_SIZE = 32;
const SIG_SIZE = 3309;
const IDENTITY_PK_SIZE = 1952;

const subtle = globalThis.crypto.subtle;
const utf8 = new TextEncoder();

// ── base64 ────────────────────────────────────────────────────────────────
// Written out rather than using atob/btoa so that decoding is STRICT: every
// field on this wire has a known exact length, so a lenient decoder could only
// turn a detectable protocol error into an undetectable one. Matches
// src/crypto/base64.cpp, including the rejection of non-zero trailing bits.

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64_REV = (() => {
  const t = new Int8Array(256).fill(-1);
  for (let i = 0; i < 64; i++) t[B64.charCodeAt(i)] = i;
  return t;
})();

export function toBase64(bytes) {
  let out = '';
  let i = 0;
  for (; i + 2 < bytes.length; i += 3) {
    const v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64[(v >> 18) & 63] + B64[(v >> 12) & 63] + B64[(v >> 6) & 63] + B64[v & 63];
  }
  if (i < bytes.length) {
    const two = i + 1 < bytes.length;
    const v = (bytes[i] << 16) | (two ? bytes[i + 1] << 8 : 0);
    out += B64[(v >> 18) & 63] + B64[(v >> 12) & 63] + (two ? B64[(v >> 6) & 63] : '=') + '=';
  }
  return out;
}

export function fromBase64(text, expectedLen = 0) {
  if (typeof text !== 'string') return null;
  if (text.length === 0) return expectedLen === 0 ? new Uint8Array(0) : null;
  if (text.length % 4 !== 0) return null;

  let pad = 0;
  if (text[text.length - 1] === '=') pad = text[text.length - 2] === '=' ? 2 : 1;

  const body = text.length - pad;
  const out = new Uint8Array((text.length / 4) * 3 - pad);
  let acc = 0, bits = 0, n = 0;
  for (let i = 0; i < body; i++) {
    const d = B64_REV[text.charCodeAt(i) & 0xff];
    if (d < 0) return null;
    acc = (acc << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[n++] = (acc >> bits) & 0xff;
    }
  }
  // Leftover bits belong to a partial byte and must be zero, otherwise two
  // different strings would decode to the same bytes.
  if (bits > 0 && (acc & ((1 << bits) - 1)) !== 0) return null;
  if (expectedLen !== 0 && n !== expectedLen) return null;
  return out;
}

// ── byte helpers ──────────────────────────────────────────────────────────

export function concatBytes(...parts) {
  let total = 0;
  for (const p of parts) total += p.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) { out.set(p, off); off += p.length; }
  return out;
}

function be64(n) {
  const out = new Uint8Array(8);
  // Lengths here are far below 2^32; the high word is written explicitly so the
  // encoding is unambiguously 64-bit and matches the server's append_be64.
  const lo = n >>> 0;
  const hi = Math.floor(n / 0x100000000) >>> 0;
  new DataView(out.buffer).setUint32(0, hi);
  new DataView(out.buffer).setUint32(4, lo);
  return out;
}

/**
 * Constant-time-ish byte comparison. JS cannot make real timing guarantees, but
 * everything compared here (a pin hash, a signature-verification result) is
 * public, and the pattern keeps the habit visible.
 */
export function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

// ── the symmetric layer ───────────────────────────────────────────────────

/** HKDF-SHA-384: extract with an empty salt, then expand under a label. */
export async function deriveSealKeys(master, context = SEAL_CONTEXT) {
  const ikm = await subtle.importKey('raw', master, 'HKDF', false, ['deriveBits']);
  const expand = async (label) =>
    new Uint8Array(
      await subtle.deriveBits(
        { name: 'HKDF', hash: 'SHA-384', salt: new Uint8Array(0), info: utf8.encode(label) },
        ikm,
        32 * 8,
      ),
    );
  // Two independent keys from one secret. Sharing a key between the cipher and
  // the MAC voids the Encrypt-then-MAC security proof.
  return { enc: await expand(`${context} enc`), mac: await expand(`${context} mac`) };
}

/** AES-256-CTR. Encryption and decryption are the same operation. */
export async function aesCtr(keyBytes, iv, data) {
  const key = await subtle.importKey('raw', keyBytes, 'AES-CTR', false, ['encrypt']);
  // length: 128 means the WHOLE block is the counter, matching the server's
  // 128-bit big-endian increment. A smaller value would wrap early and silently
  // reuse keystream.
  const buf = await subtle.encrypt({ name: 'AES-CTR', counter: iv, length: 128 }, key, data);
  return new Uint8Array(buf);
}

export async function hmacSha384(keyBytes, data) {
  const key = await subtle.importKey(
    'raw', keyBytes, { name: 'HMAC', hash: 'SHA-384' }, false, ['sign'],
  );
  return new Uint8Array(await subtle.sign('HMAC', key, data));
}

export async function sha384(data) {
  return new Uint8Array(await subtle.digest('SHA-384', data));
}

/** aad = "CHESS-SEAL-1" ‖ key_id */
export function buildAad(keyId, context = SEAL_CONTEXT) {
  return concatBytes(utf8.encode(context), keyId);
}

/**
 * Encrypt-then-MAC over an already-agreed master secret.
 * Exported on its own so tests/seal_vectors.mjs can pin exactly this much
 * against the C++ implementation without needing ML-KEM.
 */
export async function sealWithMaster(master, keyId, iv, payload, context = SEAL_CONTEXT) {
  const keys = await deriveSealKeys(master, context);
  const ct = await aesCtr(keys.enc, iv, payload);
  const aad = buildAad(keyId, context);
  // The 8-byte length prefix makes the MAC input unambiguous: without it an
  // attacker could shift bytes across the aad/ct boundary and keep the tag valid.
  const tag = await hmacSha384(keys.mac, concatBytes(be64(aad.length), aad, iv, ct));
  return { ct, tag, keys };
}

// ── the offer: pin check and signature ────────────────────────────────────

/**
 * The bytes the server's identity key signs over. Must match
 * SealedEnvelopeService::signing_input byte for byte.
 *
 *     "CHESS-SEAL-1" ‖ kem_ek ‖ x25519_pk ‖ key_id ‖ be32(expires_in)
 */
export function offerSigningInput(offer, context = SEAL_CONTEXT) {
  const exp = new Uint8Array(4);
  new DataView(exp.buffer).setUint32(0, offer.expiresIn);
  return concatBytes(utf8.encode(context), offer.kemEk, offer.x25519Pk, offer.keyId, exp);
}

/** Decode a `seal_key` frame into an offer, or null if any field is malformed. */
export function parseOffer(msg) {
  const identityPk = fromBase64(msg.identity_pk, IDENTITY_PK_SIZE);
  const keyId = fromBase64(msg.key_id, KEY_ID_SIZE);
  const kemEk = fromBase64(msg.kem_ek, KEM_EK_SIZE);
  const x25519Pk = fromBase64(msg.x25519_pk, X25519_PK_SIZE);
  const signature = fromBase64(msg.signature, SIG_SIZE);
  const expiresIn = msg.expires_in;
  if (!identityPk || !keyId || !kemEk || !x25519Pk || !signature) return null;
  if (!Number.isInteger(expiresIn) || expiresIn <= 0) return null;
  return { identityPk, keyId, kemEk, x25519Pk, signature, expiresIn };
}

/**
 * Is this offer really from a server we pinned?
 *
 * This is the check that makes the whole scheme mean something. TLS terminates
 * at Google's front end, so without it anyone between the browser and the
 * process — including the party that terminates TLS — could substitute their
 * own ML-KEM key and the browser would faithfully encrypt the password to them.
 *
 * @param pinnedKeys  ARRAY of base64 SHA-384 hashes, never a single value:
 *                    with one hash, rotating the server key instantly breaks
 *                    every deployed frontend. With a list, rotation is a
 *                    staged rollout.
 */
export async function verifyOffer(offer, pinnedKeys, pqc) {
  const digest = await sha384(offer.identityPk);
  const pin = toBase64(digest);
  if (!Array.isArray(pinnedKeys) || !pinnedKeys.includes(pin)) return false;
  return pqc.mlDsaVerify(offer.identityPk, offerSigningInput(offer), offer.signature);
}

// ── the full client flow ──────────────────────────────────────────────────

/**
 * @param pqc  { mlKemEncapsulate(ek) -> {ct, sharedSecret},
 *               x25519Generate() -> {publicKey, privateKey},
 *               x25519Derive(privateKey, peerPublic) -> Uint8Array,
 *               mlDsaVerify(pk, msg, sig) -> boolean }
 *             Backed by the vendored @noble/post-quantum build.
 */
export function createSealer(pqc, pinnedKeys) {
  return {
    /**
     * Seal a JSON-serialisable payload against a `seal_key` frame.
     *
     * Throws on a failed pin or signature check and NEVER falls back to sending
     * the payload unsealed. A fallback would mean an attacker who can make the
     * check fail can also choose to receive the password in the clear, which is
     * strictly worse than not having the check.
     */
    async seal(sealKeyMsg, payloadObject) {
      const offer = parseOffer(sealKeyMsg);
      if (!offer) throw new Error('SEAL_MALFORMED_OFFER');
      if (!(await verifyOffer(offer, pinnedKeys, pqc))) throw new Error('SEAL_UNTRUSTED_KEY');

      const kem = await pqc.mlKemEncapsulate(offer.kemEk);
      if (!kem || kem.ct.length !== KEM_CT_SIZE) throw new Error('SEAL_KEM_FAILED');

      const eph = await pqc.x25519Generate();
      const ssClassical = await pqc.x25519Derive(eph.privateKey, offer.x25519Pk);

      // Hybrid: both halves must be broken to recover the payload. HKDF's
      // extract step mixes all 64 bytes, so half a break yields nothing.
      const master = concatBytes(kem.sharedSecret, ssClassical);

      const iv = globalThis.crypto.getRandomValues(new Uint8Array(IV_SIZE));
      const payload = utf8.encode(JSON.stringify(payloadObject));
      const { ct, tag } = await sealWithMaster(master, offer.keyId, iv, payload);

      master.fill(0);
      ssClassical.fill(0);
      kem.sharedSecret.fill(0);

      return {
        key_id: toBase64(offer.keyId),
        kem_ct: toBase64(kem.ct),
        x25519_pk: toBase64(eph.publicKey),
        iv: toBase64(iv),
        ct: toBase64(ct),
        tag: toBase64(tag),
      };
    },
  };
}

export const constants = {
  SEAL_CONTEXT, KEY_ID_SIZE, IV_SIZE, TAG_SIZE,
  KEM_EK_SIZE, KEM_CT_SIZE, X25519_PK_SIZE, SIG_SIZE, IDENTITY_PK_SIZE,
};
