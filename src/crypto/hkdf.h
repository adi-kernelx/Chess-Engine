/**
 * hkdf.h — HKDF per RFC 5869. Hand-written on top of the hand-written HMAC.
 *
 * The problem HKDF solves:
 *   A key-agreement step (ML-KEM decapsulation, X25519) produces a shared
 *   secret that is uniformly *unguessable* but not uniformly *distributed* —
 *   it may have structure, and it is a fixed length. You cannot simply slice it
 *   into an encryption key and a MAC key: the pieces might be related, and you
 *   might need more bytes than you have.
 *
 * HKDF is the standard two-step answer:
 *
 *   EXTRACT:  PRK = HMAC(salt, IKM)
 *             Concentrates whatever entropy the input has into one
 *             pseudorandom key of exactly hash-length bytes. The salt is not
 *             secret; it separates uses of the same IKM.
 *
 *   EXPAND:   OKM = T(1) ‖ T(2) ‖ ... truncated to L bytes, where
 *             T(0) = ""
 *             T(i) = HMAC(PRK, T(i-1) ‖ info ‖ byte(i))
 *             Stretches the PRK into as much key material as needed. `info`
 *             binds the output to a purpose, so the encryption key and the MAC
 *             key derived from the same PRK are independent.
 *
 * The `info` label is the part that matters operationally. Deriving two keys
 * with the same label yields the SAME key — a catastrophic mistake in an
 * encrypt-then-MAC scheme, where reusing one key for both roles breaks the
 * security proof. expand_label() exists to make labels explicit at every call
 * site rather than a raw byte string someone might copy-paste.
 *
 * Sealed-envelope labels used in Phase 7 (see implementation_phase_7.md §4):
 *   "CHESS-SEAL-1 enc"  -> AES-256-CTR key
 *   "CHESS-SEAL-1 mac"  -> HMAC-SHA-384 key
 */

#pragma once

#include "crypto/hmac.h"
#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace chess {
namespace crypto {

template <typename HashT>
class Hkdf {
public:
    static constexpr size_t HASH_LEN = HashT::DIGEST_SIZE;

    /// RFC 5869 §2.2. An empty salt is legal and means "hash-length of zeros".
    static SecureBuffer extract(const uint8_t* salt, size_t salt_len,
                                const uint8_t* ikm, size_t ikm_len) {
        static const uint8_t zero_salt[HASH_LEN] = {0};
        const uint8_t* s   = (salt_len == 0 || salt == nullptr) ? zero_salt : salt;
        const size_t   slen = (salt_len == 0 || salt == nullptr) ? HASH_LEN : salt_len;

        const typename HashT::Digest prk = Hmac<HashT>::mac(s, slen, ikm, ikm_len);
        return SecureBuffer(prk.data(), prk.size());
    }

    /// RFC 5869 §2.3. `length` must not exceed 255 * HASH_LEN.
    static SecureBuffer expand(const uint8_t* prk, size_t prk_len,
                               const uint8_t* info, size_t info_len,
                               size_t length) {
        SecureBuffer okm(length);
        if (length == 0) return okm;

        const size_t n = (length + HASH_LEN - 1) / HASH_LEN;
        if (n > 255) {
            // RFC 5869 caps the counter at one byte. Returning an empty buffer
            // makes the failure obvious at the call site rather than silently
            // producing short key material.
            okm.wipe();
            return okm;
        }

        typename HashT::Digest t{};
        size_t produced = 0;

        for (size_t i = 1; i <= n; ++i) {
            Hmac<HashT> h(prk, prk_len);
            if (i > 1) {
                h.update(t.data(), t.size());   // T(i-1), empty on the first round
            }
            if (info != nullptr && info_len > 0) {
                h.update(info, info_len);
            }
            const uint8_t counter = static_cast<uint8_t>(i);
            h.update(&counter, 1);
            t = h.finish();

            const size_t take = (length - produced < HASH_LEN) ? (length - produced) : HASH_LEN;
            std::memcpy(okm.data() + produced, t.data(), take);
            produced += take;
        }

        OPENSSL_cleanse(t.data(), t.size());
        return okm;
    }

    /// extract() followed by expand() — the usual whole-of-HKDF entry point.
    static SecureBuffer derive(const uint8_t* salt, size_t salt_len,
                               const uint8_t* ikm, size_t ikm_len,
                               const uint8_t* info, size_t info_len,
                               size_t length) {
        SecureBuffer prk = extract(salt, salt_len, ikm, ikm_len);
        return expand(prk.data(), prk.size(), info, info_len, length);
    }

    /**
     * Labelled expansion. Prefer this over raw expand() so every derived key
     * carries a readable, greppable purpose string and two keys can never
     * accidentally share a label.
     */
    static SecureBuffer expand_label(const SecureBuffer& prk,
                                     const std::string& label,
                                     size_t length) {
        return expand(prk.data(), prk.size(),
                      reinterpret_cast<const uint8_t*>(label.data()), label.size(),
                      length);
    }
};

using HkdfSha256 = Hkdf<Sha256>;
using HkdfSha384 = Hkdf<Sha384>;

} // namespace crypto
} // namespace chess
