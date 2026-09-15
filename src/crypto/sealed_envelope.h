/**
 * sealed_envelope.h — one-shot hybrid public-key encryption for opaque bytes.
 *
 * WHAT THIS IS
 *
 * A payload-agnostic service. It seals bytes and opens bytes. It knows nothing
 * about passwords, chess, or message types — which is exactly what makes it
 * reusable for a future private-chat feature with no new cryptographic work
 * (decision O2).
 *
 * It is deliberately NOT a persistent secure channel. There are no sequence
 * numbers, no rekeying, no resumption. Those mechanisms exist to make a
 * long-lived channel cheap; a login is one request and one response.
 *
 * WHY IT EXISTS AT ALL, GIVEN TLS
 *
 * On Cloud Run, TLS terminates at the Google Front End. The chess_server
 * process never sees the handshake and receives already-decrypted bytes. So
 * "the connection is encrypted" is a statement about the network, not about
 * the path from the user's browser to this code. The envelope closes that
 * remaining gap for the single value worth protecting for a decade — the
 * user's password.
 *
 * THE CONSTRUCTION
 *
 *     ss_pq  = ML-KEM-768.Decapsulate(dk, kem_ct)          32 bytes
 *     ss_cl  = X25519(xsk, client_x25519_pk)               32 bytes
 *     master = ss_pq ‖ ss_cl                               64 bytes
 *     prk    = HKDF-Extract-SHA-384(salt = "", ikm = master)
 *     k_enc  = HKDF-Expand(prk, "CHESS-SEAL-1 enc", 32)
 *     k_mac  = HKDF-Expand(prk, "CHESS-SEAL-1 mac", 32)
 *     aad    = "CHESS-SEAL-1" ‖ key_id                     12 + 16 bytes
 *     ct     = AES-256-CTR(k_enc, iv, payload)
 *     tag    = HMAC-SHA-384(k_mac, len64(aad) ‖ aad ‖ iv ‖ ct)
 *
 * Hybrid because both halves must be broken to recover the payload: ML-KEM
 * covers a future quantum adversary, X25519 covers the possibility that a
 * classical weakness is found in a standard that is one year old rather than
 * fifteen. HKDF's extract step mixes the whole 64-byte input, so half a break
 * yields nothing. This mirrors TLS 1.3's X25519MLKEM768 group.
 *
 * WHAT IS AND IS NOT AUTHENTICATED
 *
 * Only `key_id` is passed as explicit AAD. That is not an oversight — every
 * other field is bound through key derivation and is therefore unforgeable in
 * a stronger sense than a MAC over it would give:
 *
 *   - kem_ct       : substituting it changes ss_pq, hence k_mac, hence the tag.
 *   - x25519_pk    : substituting it changes ss_cl, same consequence.
 *   - iv           : covered directly by the tag (see aead.cpp).
 *   - ct           : covered directly by the tag (Encrypt-then-MAC).
 *
 * This is where ML-KEM's implicit rejection (see kem.h) becomes load-bearing.
 * Decapsulating a tampered ciphertext SUCCEEDS and returns a different secret;
 * the KEM will never tell us anything was wrong. The AEAD tag is the only
 * detector, and it works precisely because a wrong secret gives a wrong MAC key.
 *
 * REPLAY
 *
 * Not defended here. It is the key store's job: one-time keys are consumed on
 * first lookup, so a captured envelope cannot be opened twice because the only
 * key that could ever open it no longer exists. See sealed_key_store.h.
 */

#pragma once

#include "crypto/kem.h"
#include "crypto/secure_buffer.h"
#include "crypto/signature.h"
#include "crypto/x25519.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {
namespace crypto {

/// Protocol version string. Changing the construction MUST change this, so an
/// old client and a new server fail to derive rather than half-agreeing.
constexpr const char* SEAL_CONTEXT = "CHESS-SEAL-1";

namespace seal {
constexpr size_t KEY_ID_SIZE = 16;
constexpr size_t IV_SIZE     = 16;   ///< AES-CTR initial counter block
constexpr size_t TAG_SIZE    = 48;   ///< HMAC-SHA-384
} // namespace seal

/**
 * The wire form of a sealed message, after base64 decoding.
 * Every field here is public; none of it is secret.
 */
struct SealedEnvelope {
    std::vector<uint8_t> key_id;       ///< 16 bytes, selects the one-time key
    std::vector<uint8_t> kem_ct;       ///< 1088 bytes
    std::vector<uint8_t> x25519_pk;    ///< 32 bytes, the client's ephemeral key
    std::vector<uint8_t> iv;           ///< 16 bytes
    std::vector<uint8_t> ct;           ///< variable
    std::vector<uint8_t> tag;          ///< 48 bytes

    /// Structural check only — sizes, not authenticity.
    bool well_formed() const;
};

/**
 * The server's half of a one-time key: the two private keys plus the id they
 * are filed under. Move-only, because it carries secrets.
 */
struct SealKeyMaterial {
    std::vector<uint8_t> key_id;
    MlKem768KeyPair      kem;
    X25519KeyPair        ecc;

    SealKeyMaterial() = default;
    SealKeyMaterial(const SealKeyMaterial&)            = delete;
    SealKeyMaterial& operator=(const SealKeyMaterial&) = delete;
    SealKeyMaterial(SealKeyMaterial&&)                 = default;
    SealKeyMaterial& operator=(SealKeyMaterial&&)      = default;

    bool valid() const { return kem.valid() && ecc.valid(); }
};

/**
 * The public advertisement of a one-time key, signed by the server's long-term
 * ML-DSA-65 identity. This is what `seal_key` carries on the wire.
 */
struct SealKeyOffer {
    std::vector<uint8_t> key_id;
    std::vector<uint8_t> kem_ek;       ///< 1184 bytes
    std::vector<uint8_t> x25519_pk;    ///< 32 bytes
    uint32_t             expires_in = 0;   ///< seconds
    std::vector<uint8_t> signature;    ///< 3309 bytes
};

class SealedEnvelopeService {
public:
    /**
     * Build the exact byte string that the identity key signs over:
     *
     *     "CHESS-SEAL-1" ‖ kem_ek ‖ x25519_pk ‖ key_id ‖ be32(expires_in)
     *
     * The context string is inside the signature so that an ML-DSA signature
     * produced for some other purpose by the same identity key can never be
     * replayed as a seal-key offer (domain separation).
     *
     * expires_in is signed as a RELATIVE duration rather than an absolute
     * timestamp on purpose: the browser has no agreed clock with the server, so
     * an absolute value would be unverifiable there and the signature over it
     * would be decoration.
     */
    static std::vector<uint8_t> signing_input(const std::vector<uint8_t>& key_id,
                                              const std::vector<uint8_t>& kem_ek,
                                              const std::vector<uint8_t>& x25519_pk,
                                              uint32_t expires_in);

    /// Generate a fresh one-time key pair set and the signed offer for it.
    static bool mint(const MlDsa65KeyPair& identity, uint32_t expires_in,
                     SealKeyMaterial& out_material, SealKeyOffer& out_offer);

    /**
     * Client-side check: does this offer really come from the pinned identity?
     * Mirrors what sealed.js does in the browser, and exists in C++ so the test
     * suite can prove a substituted key is rejected.
     */
    static bool verify_offer(const std::vector<uint8_t>& identity_pk,
                             const SealKeyOffer& offer);

    /**
     * Client side of the construction: encrypt `payload` to an offer.
     * Used by tests and by the cross-language vector generator; the real client
     * is frontend/js/net/sealed.js.
     */
    static bool seal(const SealKeyOffer& offer,
                     const uint8_t* payload, size_t payload_len,
                     SealedEnvelope& out);

    /**
     * Server side: recover the payload.
     *
     * Returns false for every failure — malformed sizes, a bad tag, a rejected
     * peer key — with no way for the caller to tell which. Differentiated
     * errors here would be an oracle.
     *
     * Does NOT consume the key or check expiry: that is the store's job, and
     * keeping it out of here is what keeps this class payload- and
     * policy-agnostic.
     */
    static bool open(const SealKeyMaterial& material, const SealedEnvelope& env,
                     SecureBuffer& out_payload);

private:
    /// master = ss_pq ‖ ss_cl, then HKDF into an (enc, mac) pair.
    static SecureBuffer build_master(const SecureBuffer& ss_pq,
                                     const SecureBuffer& ss_cl);
    /// aad = "CHESS-SEAL-1" ‖ key_id
    static std::vector<uint8_t> build_aad(const std::vector<uint8_t>& key_id);
};

} // namespace crypto
} // namespace chess
