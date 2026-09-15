#include "crypto/aead.h"

#include "crypto/constant_time.h"

#include <cstring>

namespace chess {
namespace crypto {

namespace {

/// Big-endian 64-bit length prefix, used to make the MAC input unambiguous.
void append_be64(std::vector<uint8_t>& v, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        v.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

} // namespace

AeadKeys Aead::derive_keys(const uint8_t* master, size_t master_len,
                           const std::string& context) {
    // Extract concentrates the master secret; expand splits it into two keys
    // that are computationally independent because their labels differ.
    SecureBuffer prk = HkdfSha384::extract(nullptr, 0, master, master_len);

    AeadKeys keys;
    keys.enc = HkdfSha384::expand_label(prk, context + " enc", ENC_KEY_SIZE);
    keys.mac = HkdfSha384::expand_label(prk, context + " mac", MAC_KEY_SIZE);
    return keys;
}

Sha384::Digest Aead::compute_tag(const SecureBuffer& mac_key,
                                 const uint8_t nonce[NONCE_SIZE],
                                 const uint8_t* aad, size_t aad_len,
                                 const uint8_t* ciphertext, size_t ct_len) {
    // MAC input: len64(aad) ‖ aad ‖ nonce ‖ ciphertext
    //
    // The length prefix removes any ambiguity about where the AAD ends. Without
    // it, ("AB", "CD") and ("A", "BCD") would hash identically and an attacker
    // could shift the boundary while keeping the tag valid.
    HmacSha384 h(mac_key.data(), mac_key.size());

    std::vector<uint8_t> prefix;
    prefix.reserve(8);
    append_be64(prefix, static_cast<uint64_t>(aad_len));
    h.update(prefix.data(), prefix.size());

    if (aad != nullptr && aad_len > 0) {
        h.update(aad, aad_len);
    }
    h.update(nonce, NONCE_SIZE);
    if (ciphertext != nullptr && ct_len > 0) {
        h.update(ciphertext, ct_len);
    }
    return h.finish();
}

std::vector<uint8_t> Aead::seal(const AeadKeys& keys,
                                const uint8_t nonce[NONCE_SIZE],
                                const uint8_t* aad, size_t aad_len,
                                const uint8_t* plaintext, size_t pt_len) {
    std::vector<uint8_t> out;

    if (keys.enc.size() != ENC_KEY_SIZE || keys.mac.size() != MAC_KEY_SIZE) {
        return out;   // programming error: wrong key sizes
    }

    out.resize(pt_len + TAG_SIZE);

    // 1. Encrypt.
    if (pt_len > 0) {
        AesCtr::process(keys.enc.data(), nonce, plaintext, out.data(), pt_len);
    }

    // 2. MAC the CIPHERTEXT (encrypt-then-MAC, not the other way round).
    const Sha384::Digest tag =
        compute_tag(keys.mac, nonce, aad, aad_len, out.data(), pt_len);
    std::memcpy(out.data() + pt_len, tag.data(), TAG_SIZE);

    return out;
}

bool Aead::open(const AeadKeys& keys,
                const uint8_t nonce[NONCE_SIZE],
                const uint8_t* aad, size_t aad_len,
                const uint8_t* sealed, size_t sealed_len,
                SecureBuffer& out_plaintext) {
    if (keys.enc.size() != ENC_KEY_SIZE || keys.mac.size() != MAC_KEY_SIZE) {
        return false;
    }
    // Anything shorter than a bare tag cannot be genuine.
    if (sealed == nullptr || sealed_len < TAG_SIZE) {
        return false;
    }

    const size_t ct_len = sealed_len - TAG_SIZE;
    const uint8_t* ciphertext    = sealed;
    const uint8_t* provided_tag  = sealed + ct_len;

    // 1. Recompute and compare the tag BEFORE touching the cipher.
    //    A forged or corrupted message is rejected here, so AES never runs on
    //    attacker-chosen input and there is no decryption oracle to probe.
    const Sha384::Digest expected =
        compute_tag(keys.mac, nonce, aad, aad_len, ciphertext, ct_len);

    if (!constant_time_equals(expected.data(), provided_tag, TAG_SIZE)) {
        // Single, undifferentiated failure. Callers must not report *why*.
        return false;
    }

    // 2. Only now decrypt.
    out_plaintext.resize(ct_len);
    if (ct_len > 0) {
        AesCtr::process(keys.enc.data(), nonce, ciphertext, out_plaintext.data(), ct_len);
    }
    return true;
}

} // namespace crypto
} // namespace chess
