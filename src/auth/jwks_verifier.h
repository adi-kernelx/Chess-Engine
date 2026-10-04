#pragma once
#include <openssl/evp.h>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chess::auth {
// Project-pinned HTTPS fetch; no token-controlled URLs, redirects or stale-key
// fallback. Public keys only. An injectable fetch/steady clock keeps tests offline.
class Es256JwksVerifier {
public:
    using Fetch = std::function<bool(const std::string&, std::string&)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit Es256JwksVerifier(std::string issuer, Fetch fetch = {}, Clock clock = {});
    bool valid() const;
    bool verify(const std::string& header, const std::string& signing_input,
                const std::string& signature) const;
    bool refresh() const; // Refetch/replacement on success; cooldown protected.
    size_t key_count() const;
private:
    bool refresh_locked(std::chrono::steady_clock::time_point now) const;
    std::string url_;
    Fetch fetch_;
    Clock clock_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<EVP_PKEY>> keys_;
    mutable std::chrono::steady_clock::time_point expires_{};
    mutable std::chrono::steady_clock::time_point attempted_{};
    mutable bool attempted_once_ = false;
};
}
