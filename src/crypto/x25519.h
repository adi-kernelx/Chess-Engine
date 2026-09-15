/**
 * x25519.h — X25519 ECDH (RFC 7748) via OpenSSL.
 *
 * WHY A CLASSICAL PRIMITIVE IN A POST-QUANTUM DESIGN
 *
 * ML-KEM is young. It was standardised in August 2024, and lattice
 * cryptanalysis is an active field — SIKE, a NIST alternate candidate, was
 * broken in 2022 by a classical attack that ran in about an hour on a laptop,
 * after it had survived years of public analysis. Betting the entire
 * confidentiality of the system on a single new assumption is the mistake that
 * story teaches.
 *
 * So the sealed envelope is hybrid. The AEAD key is derived from BOTH secrets
 * concatenated:
 *
 *     master = ss_mlkem ‖ ss_x25519
 *     k_enc, k_mac = HKDF(master, "CHESS-SEAL-1 ...")
 *
 * Because HKDF's extract step mixes the whole input, an attacker must break
 * BOTH to recover the key:
 *   - break only X25519 (quantum computer, Shor) → ML-KEM still protects it;
 *   - break only ML-KEM (classical cryptanalysis) → X25519 still protects it.
 *
 * This is the same reasoning behind TLS's X25519MLKEM768 hybrid group, and it
 * costs 32 extra bytes and roughly 60 microseconds. Cheap insurance.
 *
 * CONTRIBUTORY BEHAVIOUR
 *
 * A peer can send a small-order public key that forces the shared secret to
 * all zeroes regardless of our private key. RFC 7748 §6.1 requires rejecting
 * that; OpenSSL's derive fails on an all-zero output, and derive_shared()
 * propagates the failure as an empty buffer. Callers must check emptiness —
 * silently proceeding with a zero secret would let anyone fix half the hybrid
 * master to a known value.
 */

#pragma once

#include "crypto/evp_raii.h"
#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chess {
namespace crypto {

namespace x25519 {
constexpr size_t PUBLIC_KEY_SIZE    = 32;
constexpr size_t PRIVATE_KEY_SIZE   = 32;
constexpr size_t SHARED_SECRET_SIZE = 32;
} // namespace x25519

class X25519KeyPair {
public:
    X25519KeyPair() = default;

    X25519KeyPair(const X25519KeyPair&)            = delete;
    X25519KeyPair& operator=(const X25519KeyPair&) = delete;
    X25519KeyPair(X25519KeyPair&&)                 = default;
    X25519KeyPair& operator=(X25519KeyPair&&)      = default;

    static X25519KeyPair generate();

    /// Reload a 32-byte private scalar (clamping is handled by OpenSSL).
    static X25519KeyPair from_private_key(const uint8_t* key, size_t len);

    bool valid() const { return pkey_ != nullptr; }

    std::vector<uint8_t> public_key() const;
    SecureBuffer         private_key() const;

    /// ECDH against a peer's 32-byte public key. EMPTY means reject, not zero.
    SecureBuffer derive_shared(const uint8_t* peer_public, size_t len) const;

private:
    explicit X25519KeyPair(EvpPkeyPtr pkey) : pkey_(std::move(pkey)) {}
    EvpPkeyPtr pkey_;
};

} // namespace crypto
} // namespace chess
