#include "crypto/sealed_envelope.h"

#include "crypto/aead.h"
#include "crypto/random.h"

#include <cstring>

namespace chess {
namespace crypto {

namespace {

void append(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
    v.insert(v.end(), b.begin(), b.end());
}

void append(std::vector<uint8_t>& v, const char* s) {
    v.insert(v.end(), s, s + std::strlen(s));
}

void append_be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

} // namespace

bool SealedEnvelope::well_formed() const {
    // Sizes are public, so rejecting on them leaks nothing and saves the
    // expensive path from ever running on obvious junk.
    return key_id.size()    == seal::KEY_ID_SIZE &&
           kem_ct.size()    == mlkem768::CIPHERTEXT_SIZE &&
           x25519_pk.size() == x25519::PUBLIC_KEY_SIZE &&
           iv.size()        == seal::IV_SIZE &&
           tag.size()       == seal::TAG_SIZE;
    // ct may legitimately be empty; its length is not fixed.
}

std::vector<uint8_t> SealedEnvelopeService::signing_input(
        const std::vector<uint8_t>& key_id,
        const std::vector<uint8_t>& kem_ek,
        const std::vector<uint8_t>& x25519_pk,
        uint32_t expires_in) {
    std::vector<uint8_t> m;
    m.reserve(std::strlen(SEAL_CONTEXT) + kem_ek.size() + x25519_pk.size() +
              key_id.size() + 4);
    append(m, SEAL_CONTEXT);
    append(m, kem_ek);
    append(m, x25519_pk);
    append(m, key_id);
    append_be32(m, expires_in);
    // Every component has a fixed length here, so concatenation is already
    // unambiguous and no length prefixes are needed. If a variable-length
    // field is ever added, it MUST be length-prefixed — see the AAD note in
    // aead.h for what goes wrong otherwise.
    return m;
}

std::vector<uint8_t> SealedEnvelopeService::build_aad(
        const std::vector<uint8_t>& key_id) {
    std::vector<uint8_t> aad;
    append(aad, SEAL_CONTEXT);
    append(aad, key_id);
    return aad;
}

SecureBuffer SealedEnvelopeService::build_master(const SecureBuffer& ss_pq,
                                                 const SecureBuffer& ss_cl) {
    SecureBuffer master(ss_pq.size() + ss_cl.size());
    std::memcpy(master.data(), ss_pq.data(), ss_pq.size());
    std::memcpy(master.data() + ss_pq.size(), ss_cl.data(), ss_cl.size());
    return master;
}

bool SealedEnvelopeService::mint(const MlDsa65KeyPair& identity,
                                 uint32_t expires_in,
                                 SealKeyMaterial& out_material,
                                 SealKeyOffer& out_offer) {
    if (!identity.valid()) return false;

    SealKeyMaterial material;
    material.key_id.resize(seal::KEY_ID_SIZE);
    secure_random_bytes(material.key_id.data(), material.key_id.size());

    material.kem = MlKem768KeyPair::generate();
    material.ecc = X25519KeyPair::generate();
    if (!material.valid()) return false;

    SealKeyOffer offer;
    offer.key_id     = material.key_id;
    offer.kem_ek     = material.kem.public_key();
    offer.x25519_pk  = material.ecc.public_key();
    offer.expires_in = expires_in;
    if (offer.kem_ek.empty() || offer.x25519_pk.empty()) return false;

    // Signing the one-time key is the whole point: without it, anyone between
    // the browser and this process — including whoever terminates TLS — could
    // substitute their own encapsulation key and the browser would faithfully
    // encrypt the password to them.
    const std::vector<uint8_t> msg =
        signing_input(offer.key_id, offer.kem_ek, offer.x25519_pk, expires_in);
    offer.signature = identity.sign(msg.data(), msg.size());
    if (offer.signature.size() != mldsa65::SIGNATURE_SIZE) return false;

    out_material = std::move(material);
    out_offer    = std::move(offer);
    return true;
}

bool SealedEnvelopeService::verify_offer(const std::vector<uint8_t>& identity_pk,
                                         const SealKeyOffer& offer) {
    if (offer.kem_ek.size()    != mlkem768::PUBLIC_KEY_SIZE)  return false;
    if (offer.x25519_pk.size() != x25519::PUBLIC_KEY_SIZE)    return false;
    if (offer.key_id.size()    != seal::KEY_ID_SIZE)          return false;
    if (offer.signature.size() != mldsa65::SIGNATURE_SIZE)    return false;

    const std::vector<uint8_t> msg =
        signing_input(offer.key_id, offer.kem_ek, offer.x25519_pk, offer.expires_in);
    return MlDsa65::verify(identity_pk.data(), identity_pk.size(),
                           msg.data(), msg.size(),
                           offer.signature.data(), offer.signature.size());
}

bool SealedEnvelopeService::seal(const SealKeyOffer& offer,
                                 const uint8_t* payload, size_t payload_len,
                                 SealedEnvelope& out) {
    out = SealedEnvelope{};
    if (offer.key_id.size() != seal::KEY_ID_SIZE) return false;
    if (payload == nullptr && payload_len > 0) return false;

    KemEncapsulation kem = MlKem768::encapsulate(offer.kem_ek.data(), offer.kem_ek.size());
    if (!kem.ok) return false;

    X25519KeyPair ephemeral = X25519KeyPair::generate();
    SecureBuffer ss_cl = ephemeral.derive_shared(offer.x25519_pk.data(),
                                                 offer.x25519_pk.size());
    if (ss_cl.empty()) return false;

    SecureBuffer master = build_master(kem.shared_secret, ss_cl);
    AeadKeys keys = Aead::derive_keys(master.data(), master.size(), SEAL_CONTEXT);

    out.key_id    = offer.key_id;
    out.kem_ct    = std::move(kem.ciphertext);
    out.x25519_pk = ephemeral.public_key();
    out.iv.resize(seal::IV_SIZE);
    secure_random_bytes(out.iv.data(), out.iv.size());

    const std::vector<uint8_t> aad = build_aad(out.key_id);
    std::vector<uint8_t> sealed_bytes =
        Aead::seal(keys, out.iv.data(), aad.data(), aad.size(), payload, payload_len);
    if (sealed_bytes.size() != payload_len + seal::TAG_SIZE) return false;

    // Aead::seal returns ciphertext ‖ tag; the wire format carries them apart.
    out.ct.assign(sealed_bytes.begin(), sealed_bytes.begin() + payload_len);
    out.tag.assign(sealed_bytes.begin() + payload_len, sealed_bytes.end());
    return true;
}

bool SealedEnvelopeService::open(const SealKeyMaterial& material,
                                 const SealedEnvelope& env,
                                 SecureBuffer& out_payload) {
    out_payload.wipe();

    if (!material.valid() || !env.well_formed()) return false;
    // The envelope must name the key we are about to open it with. The store
    // guarantees this, but checking here keeps the class correct standalone.
    if (env.key_id != material.key_id) return false;

    // Implicit rejection (kem.h): this SUCCEEDS even for a tampered kem_ct and
    // hands back a different secret. Nothing is authenticated yet.
    SecureBuffer ss_pq = material.kem.decapsulate(env.kem_ct.data(), env.kem_ct.size());
    if (ss_pq.empty()) return false;

    SecureBuffer ss_cl = material.ecc.derive_shared(env.x25519_pk.data(),
                                                    env.x25519_pk.size());
    if (ss_cl.empty()) return false;   // small-order client key, RFC 7748 §6.1

    SecureBuffer master = build_master(ss_pq, ss_cl);
    AeadKeys keys = Aead::derive_keys(master.data(), master.size(), SEAL_CONTEXT);

    // Aead::open expects ciphertext ‖ tag contiguously, and verifies the tag
    // BEFORE decrypting anything.
    std::vector<uint8_t> joined;
    joined.reserve(env.ct.size() + env.tag.size());
    joined.insert(joined.end(), env.ct.begin(), env.ct.end());
    joined.insert(joined.end(), env.tag.begin(), env.tag.end());

    const std::vector<uint8_t> aad = build_aad(env.key_id);
    return Aead::open(keys, env.iv.data(), aad.data(), aad.size(),
                      joined.data(), joined.size(), out_payload);
}

} // namespace crypto
} // namespace chess
