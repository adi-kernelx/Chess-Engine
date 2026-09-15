/**
 * hmac.h — HMAC per RFC 2104. Hand-written, generic over the hash function.
 *
 *   HMAC(K, m) = H( (K' ⊕ opad) ‖ H( (K' ⊕ ipad) ‖ m ) )
 *
 *   K'   = H(K)          if K is longer than the hash's block size
 *        = K ‖ zeros     otherwise (padded up to the block size)
 *   ipad = 0x36 repeated block-size times
 *   opad = 0x5c repeated block-size times
 *
 * Why the nested structure rather than the obvious H(K ‖ m)?
 *   Because SHA-256 and SHA-512 are Merkle-Damgård hashes, and those are
 *   vulnerable to LENGTH EXTENSION: given H(K ‖ m) and len(m), an attacker can
 *   compute H(K ‖ m ‖ padding ‖ m2) for a message m2 of their choosing WITHOUT
 *   knowing K. The naive construction is therefore forgeable. The outer hash in
 *   HMAC breaks that, because the attacker never sees the inner digest in a
 *   form they can continue from. This is not a historical footnote — it is the
 *   Flickr API signature bug, among others.
 *
 * Why ipad and opad differ:
 *   The two derived keys must be distinct, otherwise the inner and outer
 *   invocations would be related and the security proof collapses. 0x36 and
 *   0x5c differ in four bits, which is enough.
 *
 * Templated on the hash so HmacSha256 and HmacSha384 share one implementation.
 * Phase 7 uses SHA-384 for session tokens (Category-3 matching) and SHA-256
 * inside HKDF where the label space is small.
 */

#pragma once

#include <openssl/crypto.h>   // OPENSSL_cleanse

#include "crypto/constant_time.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace chess {
namespace crypto {

template <typename HashT>
class Hmac {
public:
    static constexpr size_t DIGEST_SIZE = HashT::DIGEST_SIZE;
    static constexpr size_t BLOCK_SIZE  = HashT::BLOCK_SIZE;
    using Digest = typename HashT::Digest;

    Hmac(const uint8_t* key, size_t key_len) { init(key, key_len); }

    void update(const uint8_t* data, size_t len) { inner_.update(data, len); }
    void update(const std::string& s) { inner_.update(s); }

    Digest finish() {
        const Digest inner_digest = inner_.finish();

        HashT outer;
        outer.update(o_key_pad_, BLOCK_SIZE);
        outer.update(inner_digest.data(), inner_digest.size());
        Digest out = outer.finish();

        // The padded key blocks are derived from the secret; do not leave them
        // sitting in memory once the MAC is produced.
        OPENSSL_cleanse(o_key_pad_, BLOCK_SIZE);
        return out;
    }

    /// One-shot.
    static Digest mac(const uint8_t* key, size_t key_len,
                      const uint8_t* data, size_t data_len) {
        Hmac h(key, key_len);
        h.update(data, data_len);
        return h.finish();
    }

    static Digest mac(const std::string& key, const std::string& data) {
        return mac(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                   reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    /**
     * Verify a MAC in constant time.
     *
     * Always use this rather than comparing digests with == or memcmp:
     * an early-exit comparison leaks how many leading bytes were correct,
     * which is enough to forge a MAC byte-by-byte over a network.
     */
    static bool verify(const uint8_t* key, size_t key_len,
                       const uint8_t* data, size_t data_len,
                       const uint8_t* expected_mac, size_t expected_len) {
        if (expected_len != DIGEST_SIZE) return false;
        const Digest actual = mac(key, key_len, data, data_len);
        return constant_time_equals(actual.data(), expected_mac, DIGEST_SIZE);
    }

private:
    void init(const uint8_t* key, size_t key_len) {
        uint8_t k_prime[BLOCK_SIZE];
        std::memset(k_prime, 0, BLOCK_SIZE);

        if (key_len > BLOCK_SIZE) {
            // Keys longer than a block are hashed down first. Note the
            // consequence: HMAC(K) and HMAC(H(K)) are the same MAC.
            const typename HashT::Digest kh = HashT::hash(key, key_len);
            std::memcpy(k_prime, kh.data(), kh.size());
        } else if (key_len > 0) {
            std::memcpy(k_prime, key, key_len);
        }

        uint8_t i_key_pad[BLOCK_SIZE];
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            i_key_pad[i]  = static_cast<uint8_t>(k_prime[i] ^ 0x36);
            o_key_pad_[i] = static_cast<uint8_t>(k_prime[i] ^ 0x5c);
        }

        inner_.update(i_key_pad, BLOCK_SIZE);

        OPENSSL_cleanse(k_prime, BLOCK_SIZE);
        OPENSSL_cleanse(i_key_pad, BLOCK_SIZE);
    }

    HashT   inner_;
    uint8_t o_key_pad_[BLOCK_SIZE];
};

using HmacSha256 = Hmac<Sha256>;
using HmacSha384 = Hmac<Sha384>;
using HmacSha512 = Hmac<Sha512>;

} // namespace crypto
} // namespace chess
