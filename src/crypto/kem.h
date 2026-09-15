/**
 * kem.h — ML-KEM-768 (FIPS 203, formerly Kyber-768) via OpenSSL 3.5.
 *
 * WHY THIS ONE IS A LIBRARY CALL
 *
 * Every symmetric primitive in Phase 7 is hand-written, because the project
 * rule is to build what has been formally studied. Module-LWE *has* been
 * studied — that is not the reason this file wraps OpenSSL.
 *
 * The reason is that a lattice KEM's security depends on properties the maths
 * does not describe: every operation must run in time and with a memory-access
 * pattern independent of the secret. That is a separate specialisation from
 * understanding the scheme, and its failure mode is silent — a broken
 * implementation still round-trips, still matches the KAT vectors, and still
 * passes every test you would think to write, while leaking the private key
 * over a timing channel. KyberSlash (2024) was exactly this: the *reference*
 * implementation divided by a constant in a way that compiled to a
 * variable-time instruction on some targets, and secret-dependent timing fell
 * out of code that was otherwise correct.
 *
 * So the wall here is deliberate: production uses OpenSSL, and the from-scratch
 * study implementation lives in research/kyber_reference/ where it can never be
 * linked into chess_server.
 *
 * WHAT A KEM IS, AND WHY IT IS NOT DIFFIE-HELLMAN
 *
 * DH is symmetric: both sides contribute a public value and both compute the
 * same secret. A KEM is one-directional. The holder of a public key can be
 * *sent* a secret without ever replying:
 *
 *     server: (ek, dk) = KeyGen()          ek published, dk kept
 *     client: (ct, ss) = Encaps(ek)        ss is fresh 32 random-looking bytes
 *     server:  ss      = Decaps(dk, ct)    same ss, no round trip
 *
 * That one-shot shape is precisely what the sealed envelope (§7.4) needs: the
 * browser encrypts a password to the server's published key inside a single
 * message, with no handshake and no added latency.
 *
 * IMPLICIT REJECTION — the surprise in FIPS 203 §7.3
 *
 * Decapsulating a tampered ciphertext does NOT return an error. It returns a
 * different, deterministic 32-byte secret derived from a per-key rejection
 * seed. This is intentional: an explicit failure would give an attacker a
 * decryption oracle (the Fujisaki-Okamoto transform's chosen-ciphertext
 * defence rests on the two cases being indistinguishable).
 *
 * The consequence for calling code is important: decapsulate() succeeding
 * proves NOTHING about the ciphertext's authenticity. Detecting tampering is
 * the job of the AEAD tag computed over a key derived from ss — a wrong ss
 * yields a wrong MAC key and the tag check fails. Never treat a successful
 * decapsulation as authentication.
 */

#pragma once

#include "crypto/evp_raii.h"
#include "crypto/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chess {
namespace crypto {

/// Sizes fixed by FIPS 203 for parameter set ML-KEM-768 (NIST Category 3).
namespace mlkem768 {
constexpr size_t PUBLIC_KEY_SIZE    = 1184;  ///< encapsulation key (ek)
constexpr size_t PRIVATE_KEY_SIZE   = 2400;  ///< decapsulation key (dk)
constexpr size_t CIPHERTEXT_SIZE    = 1088;
constexpr size_t SHARED_SECRET_SIZE = 32;
} // namespace mlkem768

/// Result of Encaps(): a ciphertext to transmit and the secret it carries.
struct KemEncapsulation {
    std::vector<uint8_t> ciphertext;     ///< 1088 bytes, public
    SecureBuffer         shared_secret;  ///< 32 bytes, secret
    bool                 ok = false;
};

/**
 * An ML-KEM-768 key pair. Owns the private key; move-only, like every other
 * secret-bearing type in this project.
 */
class MlKem768KeyPair {
public:
    MlKem768KeyPair() = default;

    MlKem768KeyPair(const MlKem768KeyPair&)            = delete;
    MlKem768KeyPair& operator=(const MlKem768KeyPair&) = delete;
    MlKem768KeyPair(MlKem768KeyPair&&)                 = default;
    MlKem768KeyPair& operator=(MlKem768KeyPair&&)      = default;

    /// Generate a fresh pair. Returns an invalid object on failure.
    static MlKem768KeyPair generate();

    bool valid() const { return pkey_ != nullptr; }

    /// The encapsulation key, safe to publish. Empty if !valid().
    std::vector<uint8_t> public_key() const;

    /**
     * Recover the shared secret from a ciphertext.
     *
     * Returns an empty buffer only on a structural failure (wrong ciphertext
     * length, no key). A tampered-but-well-formed ciphertext SUCCEEDS and
     * returns a different secret — see "implicit rejection" above.
     */
    SecureBuffer decapsulate(const uint8_t* ciphertext, size_t len) const;

private:
    explicit MlKem768KeyPair(EvpPkeyPtr pkey) : pkey_(std::move(pkey)) {}
    EvpPkeyPtr pkey_;
};

class MlKem768 {
public:
    /// Encapsulate to a raw 1184-byte encapsulation key.
    static KemEncapsulation encapsulate(const uint8_t* public_key, size_t len);
};

} // namespace crypto
} // namespace chess
