#include "crypto/x25519.h"

namespace chess {
namespace crypto {

namespace {

constexpr const char* ALG = "X25519";

} // namespace

X25519KeyPair X25519KeyPair::generate() {
    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, ALG, nullptr));
    if (!ctx) return X25519KeyPair{};
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) return X25519KeyPair{};

    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_generate(ctx.get(), &raw) <= 0) return X25519KeyPair{};

    return X25519KeyPair(EvpPkeyPtr(raw));
}

X25519KeyPair X25519KeyPair::from_private_key(const uint8_t* key, size_t len) {
    if (key == nullptr || len != x25519::PRIVATE_KEY_SIZE) return X25519KeyPair{};

    EvpPkeyPtr pkey(EVP_PKEY_new_raw_private_key_ex(nullptr, ALG, nullptr, key, len));
    if (!pkey) return X25519KeyPair{};
    return X25519KeyPair(std::move(pkey));
}

std::vector<uint8_t> X25519KeyPair::public_key() const {
    std::vector<uint8_t> out;
    if (!pkey_) return out;

    out.resize(x25519::PUBLIC_KEY_SIZE);
    size_t len = out.size();
    if (EVP_PKEY_get_raw_public_key(pkey_.get(), out.data(), &len) <= 0 ||
        len != x25519::PUBLIC_KEY_SIZE) {
        out.clear();
    }
    return out;
}

SecureBuffer X25519KeyPair::private_key() const {
    SecureBuffer out;
    if (!pkey_) return out;

    out.resize(x25519::PRIVATE_KEY_SIZE);
    size_t len = out.size();
    if (EVP_PKEY_get_raw_private_key(pkey_.get(), out.data(), &len) <= 0 ||
        len != x25519::PRIVATE_KEY_SIZE) {
        out.wipe();
    }
    return out;
}

SecureBuffer X25519KeyPair::derive_shared(const uint8_t* peer_public, size_t len) const {
    SecureBuffer out;
    if (!pkey_ || peer_public == nullptr || len != x25519::PUBLIC_KEY_SIZE) return out;

    EvpPkeyPtr peer(EVP_PKEY_new_raw_public_key_ex(nullptr, ALG, nullptr,
                                                   peer_public, len));
    if (!peer) return out;

    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey_.get(), nullptr));
    if (!ctx) return out;
    if (EVP_PKEY_derive_init(ctx.get()) <= 0) return out;
    if (EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) <= 0) return out;

    out.resize(x25519::SHARED_SECRET_SIZE);
    size_t ss_len = out.size();
    // Fails on an all-zero result, i.e. a small-order peer key (RFC 7748 §6.1).
    if (EVP_PKEY_derive(ctx.get(), out.data(), &ss_len) <= 0 ||
        ss_len != x25519::SHARED_SECRET_SIZE) {
        out.wipe();
    }
    return out;
}

} // namespace crypto
} // namespace chess
