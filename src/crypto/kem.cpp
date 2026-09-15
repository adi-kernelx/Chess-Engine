#include "crypto/kem.h"

namespace chess {
namespace crypto {

namespace {

constexpr const char* ALG = "ML-KEM-768";

} // namespace

MlKem768KeyPair MlKem768KeyPair::generate() {
    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, ALG, nullptr));
    if (!ctx) return MlKem768KeyPair{};

    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) return MlKem768KeyPair{};

    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_generate(ctx.get(), &raw) <= 0) return MlKem768KeyPair{};

    return MlKem768KeyPair(EvpPkeyPtr(raw));
}

std::vector<uint8_t> MlKem768KeyPair::public_key() const {
    std::vector<uint8_t> out;
    if (!pkey_) return out;

    out.resize(mlkem768::PUBLIC_KEY_SIZE);
    size_t len = out.size();
    if (EVP_PKEY_get_raw_public_key(pkey_.get(), out.data(), &len) <= 0 ||
        len != mlkem768::PUBLIC_KEY_SIZE) {
        out.clear();
    }
    return out;
}

SecureBuffer MlKem768KeyPair::decapsulate(const uint8_t* ciphertext, size_t len) const {
    SecureBuffer out;
    if (!pkey_ || ciphertext == nullptr || len != mlkem768::CIPHERTEXT_SIZE) {
        // Length is public, so rejecting on it leaks nothing. Everything else
        // must go through decapsulation so that valid and invalid ciphertexts
        // are indistinguishable to the caller.
        return out;
    }

    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey_.get(), nullptr));
    if (!ctx) return out;
    if (EVP_PKEY_decapsulate_init(ctx.get(), nullptr) <= 0) return out;

    out.resize(mlkem768::SHARED_SECRET_SIZE);
    size_t ss_len = out.size();
    if (EVP_PKEY_decapsulate(ctx.get(), out.data(), &ss_len,
                             ciphertext, len) <= 0 ||
        ss_len != mlkem768::SHARED_SECRET_SIZE) {
        out.wipe();
    }
    return out;
}

KemEncapsulation MlKem768::encapsulate(const uint8_t* public_key, size_t len) {
    KemEncapsulation result;
    if (public_key == nullptr || len != mlkem768::PUBLIC_KEY_SIZE) return result;

    // A raw import validates the encoding; a corrupt or truncated key fails
    // here rather than producing a ciphertext nobody can decapsulate.
    EvpPkeyPtr pkey(EVP_PKEY_new_raw_public_key_ex(nullptr, ALG, nullptr,
                                                   public_key, len));
    if (!pkey) return result;

    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!ctx) return result;
    if (EVP_PKEY_encapsulate_init(ctx.get(), nullptr) <= 0) return result;

    result.ciphertext.resize(mlkem768::CIPHERTEXT_SIZE);
    result.shared_secret.resize(mlkem768::SHARED_SECRET_SIZE);

    size_t ct_len = result.ciphertext.size();
    size_t ss_len = result.shared_secret.size();
    if (EVP_PKEY_encapsulate(ctx.get(),
                             result.ciphertext.data(), &ct_len,
                             result.shared_secret.data(), &ss_len) <= 0 ||
        ct_len != mlkem768::CIPHERTEXT_SIZE ||
        ss_len != mlkem768::SHARED_SECRET_SIZE) {
        result.ciphertext.clear();
        result.shared_secret.wipe();
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace crypto
} // namespace chess
