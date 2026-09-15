/**
 * aes_ctr.h — AES-256 in Counter (CTR) mode, per NIST SP 800-38A §6.5.
 *
 * CTR turns a block cipher into a stream cipher. Instead of encrypting the
 * plaintext, it encrypts a COUNTER and XORs the result with the plaintext:
 *
 *     keystream_i = AES(key, counter_block + i)
 *     ciphertext  = plaintext XOR keystream
 *
 * Consequences worth understanding, because they drive several design choices
 * elsewhere in Phase 7:
 *
 *   - Encryption and decryption are the SAME operation. XOR is its own
 *     inverse, so one function serves both. There is no separate decrypt path
 *     to get wrong.
 *   - Only the FORWARD block cipher is ever needed. This is why aes.cpp
 *     implements no inverse S-box or inverse MixColumns at all.
 *   - No padding. The plaintext length is preserved exactly, so there is no
 *     padding oracle to attack — a whole vulnerability class that CBC has and
 *     CTR simply does not.
 *   - Random access: block i can be decrypted without touching blocks 0..i-1.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * THE ONE RULE: NEVER reuse a (key, counter_block) pair.
 *
 * Two messages encrypted with the same key and starting counter produce the
 * same keystream, so XORing the two ciphertexts yields the XOR of the two
 * plaintexts — the key drops out entirely. This is how Venona broke Soviet
 * one-time-pad reuse, and it is exactly as fatal here.
 *
 * In Phase 7 this is enforced structurally rather than by discipline: the
 * sealed envelope derives a fresh key from a fresh ML-KEM encapsulation for
 * every single login, so no key is ever used twice in the first place.
 * ─────────────────────────────────────────────────────────────────────────
 *
 * CTR mode provides CONFIDENTIALITY ONLY. It is trivially malleable — flipping
 * a ciphertext bit flips the same plaintext bit, undetected. Never use this
 * class directly for anything that crosses the network; use the Aead wrapper
 * in aead.h, which adds the authentication CTR lacks.
 */

#pragma once

#include "crypto/aes.h"
#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chess {
namespace crypto {

class AesCtr {
public:
    static constexpr size_t KEY_SIZE     = Aes256::KEY_SIZE;    // 32
    static constexpr size_t BLOCK_SIZE   = Aes256::BLOCK_SIZE;  // 16
    static constexpr size_t COUNTER_SIZE = Aes256::BLOCK_SIZE;  // 16

    /**
     * Encrypt (or equivalently decrypt) `len` bytes.
     *
     * @param key           32-byte AES-256 key
     * @param counter_block 16-byte initial counter block, incremented as a
     *                      128-bit big-endian integer once per block
     * @param in            input bytes
     * @param out           output buffer, `len` bytes; may alias `in`
     */
    static void process(const uint8_t key[KEY_SIZE],
                        const uint8_t counter_block[COUNTER_SIZE],
                        const uint8_t* in, uint8_t* out, size_t len);

    /// Convenience overload returning a fresh SecureBuffer.
    static SecureBuffer process(const SecureBuffer& key,
                                const uint8_t counter_block[COUNTER_SIZE],
                                const uint8_t* in, size_t len);

private:
    /// Increment a 16-byte big-endian counter in place, with carry.
    static void increment_counter(uint8_t counter[COUNTER_SIZE]);
};

} // namespace crypto
} // namespace chess
