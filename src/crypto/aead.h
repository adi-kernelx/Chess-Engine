/**
 * aead.h — Authenticated encryption: AES-256-CTR + HMAC-SHA-384, composed as
 * Encrypt-then-MAC. Hand-written on top of the Phase 7.1 and 7.2 primitives.
 *
 * WHY THIS COMPOSITION RATHER THAN AES-GCM
 *
 * GCM is the obvious modern choice and it was deliberately rejected here:
 *
 *   1. GCM authenticates with GHASH, which multiplies in GF(2^128) using a
 *      bit-ordering convention of its own (the "reflected" representation).
 *      It is a documented source of implementations that pass some vectors
 *      and fail others.
 *   2. GCM's nonce-reuse failure is catastrophic beyond the usual: repeating a
 *      nonce once leaks the authentication subkey H, after which an attacker
 *      forges arbitrary messages forever. CTR+HMAC under nonce reuse leaks
 *      plaintext relationships — bad, but recoverable and not key-compromising.
 *   3. CTR and HMAC are both squarely inside the coursework (modes of
 *      operation; hash functions). GHASH is not.
 *
 * Encrypt-then-MAC is the provably correct ordering (Bellare-Namprempre 2000).
 * The alternatives are broken in practice:
 *   - MAC-then-Encrypt gave us Lucky 13 and the TLS CBC padding oracles.
 *   - Encrypt-and-MAC can leak plaintext through the MAC itself.
 * Encrypt-then-MAC lets the receiver reject forgeries WITHOUT decrypting, so
 * malformed input never reaches the cipher at all.
 *
 * WHAT THE TAG COVERS
 *
 *     tag = HMAC-SHA-384(k_mac, len64(aad) ‖ aad ‖ nonce ‖ ciphertext)
 *
 * The 8-byte big-endian length prefix on the AAD is not decoration. Without
 * it, an attacker can move bytes across the aad/ciphertext boundary and
 * produce a different (aad, ciphertext) pair with an identical MAC input — a
 * canonicalisation attack. Length-prefixing makes the encoding unambiguous.
 * The nonce is inside the MAC so it cannot be swapped either.
 *
 * TWO INDEPENDENT KEYS
 *
 * k_enc and k_mac must be different keys. Using one key for both roles voids
 * the security proof. derive_keys() produces both from a single master secret
 * via HKDF with distinct labels, which is how the sealed envelope (§7.4) will
 * turn one ML-KEM shared secret into a usable key pair.
 */

#pragma once

#include "crypto/aes_ctr.h"
#include "crypto/hkdf.h"
#include "crypto/hmac.h"
#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {
namespace crypto {

/// The two independent keys an AEAD operation needs.
struct AeadKeys {
    SecureBuffer enc;   ///< 32 bytes, AES-256
    SecureBuffer mac;   ///< 32 bytes, HMAC-SHA-384
};

class Aead {
public:
    static constexpr size_t ENC_KEY_SIZE = 32;                      // AES-256
    static constexpr size_t MAC_KEY_SIZE = 32;
    static constexpr size_t NONCE_SIZE   = AesCtr::COUNTER_SIZE;    // 16
    static constexpr size_t TAG_SIZE     = Sha384::DIGEST_SIZE;     // 48

    /**
     * Derive an independent (enc, mac) key pair from one master secret.
     *
     * `context` separates uses — e.g. "CHESS-SEAL-1". Two calls with the same
     * master and different contexts yield unrelated key pairs.
     */
    static AeadKeys derive_keys(const uint8_t* master, size_t master_len,
                                const std::string& context);

    /**
     * Encrypt and authenticate.
     * @return ciphertext ‖ tag  (plaintext length + TAG_SIZE bytes)
     */
    static std::vector<uint8_t> seal(const AeadKeys& keys,
                                     const uint8_t nonce[NONCE_SIZE],
                                     const uint8_t* aad, size_t aad_len,
                                     const uint8_t* plaintext, size_t pt_len);

    /**
     * Verify then decrypt.
     *
     * The tag is checked in constant time BEFORE any decryption happens; on
     * failure nothing is written to `out` and the function returns false.
     * Callers must treat false as "discard everything" — never as "retry" and
     * never with a distinct error message, since distinguishable failures are
     * what padding oracles feed on.
     *
     * @param sealed  ciphertext ‖ tag, as produced by seal()
     */
    static bool open(const AeadKeys& keys,
                     const uint8_t nonce[NONCE_SIZE],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* sealed, size_t sealed_len,
                     SecureBuffer& out_plaintext);

private:
    /// Build the exact byte string the tag is computed over.
    static Sha384::Digest compute_tag(const SecureBuffer& mac_key,
                                      const uint8_t nonce[NONCE_SIZE],
                                      const uint8_t* aad, size_t aad_len,
                                      const uint8_t* ciphertext, size_t ct_len);
};

} // namespace crypto
} // namespace chess
