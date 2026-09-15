/**
 * signature.h — ML-DSA-65 (FIPS 204, formerly Dilithium3) via OpenSSL 3.5.
 *
 * The same reasoning as kem.h applies to why this is a library call rather
 * than a hand-written implementation: the lattice mathematics is studied, but
 * constant-time implementation is a distinct specialisation with a silent
 * failure mode. See the header comment in kem.h.
 *
 * WHAT IT IS FOR HERE
 *
 * The KEM answers "how do I send the server a secret". It does not answer
 * "am I talking to the *right* server". Anyone can generate an ML-KEM key
 * pair and publish it; a network attacker who substitutes their own
 * encapsulation key sits in the middle of the exchange and reads everything.
 *
 * The server therefore holds a long-lived ML-DSA-65 identity key and signs the
 * ephemeral material it publishes. The browser has SHA-384(identity public key)
 * pinned in config.js, so a substituted key is detected before any secret is
 * encapsulated to it. That pin is what stops the substitution — the signature
 * only chains trust from the pin to the ephemeral key.
 *
 * PURE ML-DSA, NOT PRE-HASHED
 *
 * FIPS 204 defines two variants. Pure ML-DSA hashes the message internally as
 * part of the signing procedure; HashML-DSA takes a digest plus an explicit
 * OID identifying the hash. They are different algorithms and produce
 * incompatible signatures.
 *
 * The trap: if you SHA-384 the message yourself and pass the 48-byte digest to
 * pure ML-DSA, everything works. Signing succeeds, verification succeeds, and
 * every round-trip test in your own codebase passes — because you made the same
 * mistake on both sides. It only breaks when a browser using a correct library
 * tries to verify, or when a reviewer checks the signature against the spec.
 *
 * So: sign() takes the MESSAGE. Never hash before calling it.
 */

#pragma once

#include "crypto/evp_raii.h"
#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chess {
namespace crypto {

/// Sizes fixed by FIPS 204 for parameter set ML-DSA-65 (NIST Category 3).
namespace mldsa65 {
constexpr size_t PUBLIC_KEY_SIZE  = 1952;
constexpr size_t PRIVATE_KEY_SIZE = 4032;
constexpr size_t SIGNATURE_SIZE   = 3309;
} // namespace mldsa65

/**
 * An ML-DSA-65 signing key. Owns private material, so move-only.
 *
 * A single instance is safe to sign with from multiple threads: each call
 * builds its own EVP_PKEY_CTX and the underlying EVP_PKEY is only read.
 */
class MlDsa65KeyPair {
public:
    MlDsa65KeyPair() = default;

    MlDsa65KeyPair(const MlDsa65KeyPair&)            = delete;
    MlDsa65KeyPair& operator=(const MlDsa65KeyPair&) = delete;
    MlDsa65KeyPair(MlDsa65KeyPair&&)                 = default;
    MlDsa65KeyPair& operator=(MlDsa65KeyPair&&)      = default;

    /// Generate a fresh identity key.
    static MlDsa65KeyPair generate();

    /// Reload a key persisted by private_key() — used at server start-up.
    static MlDsa65KeyPair from_private_key(const uint8_t* key, size_t len);

    bool valid() const { return pkey_ != nullptr; }

    /// 1952-byte verification key, safe to publish.
    std::vector<uint8_t> public_key() const;

    /// 4032-byte signing key. Secret: written to disk with 0600 and nothing else.
    SecureBuffer private_key() const;

    /// Sign a MESSAGE (not a digest — see the header comment). Empty on failure.
    std::vector<uint8_t> sign(const uint8_t* message, size_t len) const;

private:
    explicit MlDsa65KeyPair(EvpPkeyPtr pkey) : pkey_(std::move(pkey)) {}
    EvpPkeyPtr pkey_;
};

class MlDsa65 {
public:
    /**
     * Verify a signature against a raw 1952-byte public key.
     * Returns false for any failure — bad key, wrong size, bad signature —
     * with no distinction between them.
     */
    static bool verify(const uint8_t* public_key, size_t pk_len,
                       const uint8_t* message, size_t msg_len,
                       const uint8_t* signature, size_t sig_len);
};

} // namespace crypto
} // namespace chess
