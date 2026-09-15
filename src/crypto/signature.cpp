#include "crypto/signature.h"

namespace chess {
namespace crypto {

namespace {

constexpr const char* ALG = "ML-DSA-65";

} // namespace

MlDsa65KeyPair MlDsa65KeyPair::generate() {
    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, ALG, nullptr));
    if (!ctx) return MlDsa65KeyPair{};
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) return MlDsa65KeyPair{};

    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_generate(ctx.get(), &raw) <= 0) return MlDsa65KeyPair{};

    return MlDsa65KeyPair(EvpPkeyPtr(raw));
}

MlDsa65KeyPair MlDsa65KeyPair::from_private_key(const uint8_t* key, size_t len) {
    if (key == nullptr || len != mldsa65::PRIVATE_KEY_SIZE) return MlDsa65KeyPair{};

    EvpPkeyPtr pkey(EVP_PKEY_new_raw_private_key_ex(nullptr, ALG, nullptr, key, len));
    if (!pkey) return MlDsa65KeyPair{};
    return MlDsa65KeyPair(std::move(pkey));
}

std::vector<uint8_t> MlDsa65KeyPair::public_key() const {
    std::vector<uint8_t> out;
    if (!pkey_) return out;

    out.resize(mldsa65::PUBLIC_KEY_SIZE);
    size_t len = out.size();
    if (EVP_PKEY_get_raw_public_key(pkey_.get(), out.data(), &len) <= 0 ||
        len != mldsa65::PUBLIC_KEY_SIZE) {
        out.clear();
    }
    return out;
}

SecureBuffer MlDsa65KeyPair::private_key() const {
    SecureBuffer out;
    if (!pkey_) return out;

    out.resize(mldsa65::PRIVATE_KEY_SIZE);
    size_t len = out.size();
    if (EVP_PKEY_get_raw_private_key(pkey_.get(), out.data(), &len) <= 0 ||
        len != mldsa65::PRIVATE_KEY_SIZE) {
        out.wipe();
    }
    return out;
}

std::vector<uint8_t> MlDsa65KeyPair::sign(const uint8_t* message, size_t len) const {
    std::vector<uint8_t> out;
    if (!pkey_ || (message == nullptr && len > 0)) return out;

    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey_.get(), nullptr));
    if (!ctx) return out;

    // sign_message_init, NOT sign_init: this selects pure ML-DSA, where the
    // provider hashes the message itself. Passing a digest here would be the
    // silent interoperability bug described in signature.h.
    if (EVP_PKEY_sign_message_init(ctx.get(), nullptr, nullptr) <= 0) return out;

    out.resize(mldsa65::SIGNATURE_SIZE);
    size_t sig_len = out.size();
    if (EVP_PKEY_sign(ctx.get(), out.data(), &sig_len, message, len) <= 0 ||
        sig_len != mldsa65::SIGNATURE_SIZE) {
        out.clear();
    }
    return out;
}

bool MlDsa65::verify(const uint8_t* public_key, size_t pk_len,
                     const uint8_t* message, size_t msg_len,
                     const uint8_t* signature, size_t sig_len) {
    if (public_key == nullptr || pk_len != mldsa65::PUBLIC_KEY_SIZE) return false;
    if (signature == nullptr || sig_len != mldsa65::SIGNATURE_SIZE) return false;
    if (message == nullptr && msg_len > 0) return false;

    EvpPkeyPtr pkey(EVP_PKEY_new_raw_public_key_ex(nullptr, ALG, nullptr,
                                                   public_key, pk_len));
    if (!pkey) return false;

    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!ctx) return false;
    if (EVP_PKEY_verify_message_init(ctx.get(), nullptr, nullptr) <= 0) return false;

    // EVP_PKEY_verify returns 1 only for a valid signature; 0 means invalid and
    // negative means an operational error. Both non-1 cases are "reject".
    return EVP_PKEY_verify(ctx.get(), signature, sig_len, message, msg_len) == 1;
}

} // namespace crypto
} // namespace chess
