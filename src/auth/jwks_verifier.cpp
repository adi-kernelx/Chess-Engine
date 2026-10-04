#include "auth/jwks_verifier.h"
#include "crypto/base64.h"
#include <curl/curl.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/params.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <vector>

namespace chess::auth {
namespace {
using json = nlohmann::json;
constexpr size_t MAX_BODY = 65536;
size_t receive(char* data, size_t size, size_t count, void* target) {
    auto& body = *static_cast<std::string*>(target);
    if (size && count > MAX_BODY / size) return 0;
    const size_t length = size * count;
    if (body.size() + length > MAX_BODY) return 0;
    try { body.append(data, length); }
    catch (...) { return 0; } // Never unwind through libcurl's C callback.
    return length;
}
bool https_get(const std::string& url, std::string& body) {
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!initialized) return false;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return false;
    body.clear();
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    if (curl_easy_perform(curl.get()) != CURLE_OK) return false;
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return status == 200 && !body.empty();
}
std::shared_ptr<EVP_PKEY> public_key(const json& key) {
    if (key.value("kty", "") != "EC" || key.value("crv", "") != "P-256" ||
        (key.contains("alg") && key.value("alg", "") != "ES256") ||
        (key.contains("use") && key.value("use", "") != "sig") || key.contains("d")) return {};
    if (key.contains("key_ops") && key["key_ops"] != json::array({"verify"})) return {};
    std::vector<uint8_t> x, y;
    if (!chess::crypto::decode_base64url(key.value("x", ""), x, 32) ||
        !chess::crypto::decode_base64url(key.value("y", ""), y, 32)) return {};
    std::vector<uint8_t> point{4};
    point.insert(point.end(), x.begin(), x.end()); point.insert(point.end(), y.begin(), y.end());
    char group[] = "prime256v1";
    OSSL_PARAM params[] = { OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()),
        OSSL_PARAM_construct_end() };
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* raw = nullptr;
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 ||
        EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params) <= 0) return {};
    std::shared_ptr<EVP_PKEY> result(raw, EVP_PKEY_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> check(
        EVP_PKEY_CTX_new_from_pkey(nullptr, raw, nullptr), EVP_PKEY_CTX_free);
    if (!check || EVP_PKEY_public_check(check.get()) != 1) return {};
    return result;
}
}
Es256JwksVerifier::Es256JwksVerifier(std::string issuer, Fetch fetch, Clock clock)
    : fetch_(fetch ? std::move(fetch) : https_get),
      clock_(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); }) {
    // Only this project's configured Supabase origin can supply trusted keys.
    if (std::regex_match(issuer, std::regex("https://[a-z0-9]{20}\\.supabase\\.co/auth/v1"))) {
        url_ = issuer + "/.well-known/jwks.json";
    }
}
bool Es256JwksVerifier::valid() const { return !url_.empty(); }
bool Es256JwksVerifier::refresh_locked(std::chrono::steady_clock::time_point now) const {
    if (!valid() || (attempted_once_ && now - attempted_ < std::chrono::seconds(30))) return false;
    attempted_once_ = true; attempted_ = now;
    try {
        std::string body;
        if (!fetch_(url_, body) || body.size() > MAX_BODY) return false;
        const auto document = json::parse(body, nullptr, false);
        if (!document.is_object() || !document.contains("keys") ||
            !document["keys"].is_array() || document["keys"].size() > 32) return false;
        std::unordered_map<std::string, std::shared_ptr<EVP_PKEY>> replacement;
        for (const auto& key : document["keys"]) {
            if (!key.is_object()) return false;
            const auto kid = key.value("kid", "");
            if (kid.empty() || kid.size() > 128) return false;
            auto imported = public_key(key);
            if (!imported || !replacement.emplace(kid, std::move(imported)).second) return false;
        }
        keys_ = std::move(replacement);
        expires_ = clock_() + std::chrono::minutes(5);
        return true;
    } catch (...) { return false; }
}
bool Es256JwksVerifier::refresh() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return refresh_locked(clock_());
}
size_t Es256JwksVerifier::key_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keys_.size();
}
bool Es256JwksVerifier::verify(const std::string& header, const std::string& input,
                              const std::string& signature) const {
    try {
        const auto h = json::parse(header, nullptr, false);
        if (!h.is_object() || h.value("alg", "") != "ES256" ||
            (h.contains("typ") && h.value("typ", "") != "JWT") ||
            h.contains("jku") || h.contains("jwk") || h.contains("x5u") ||
            h.contains("crit") || h.contains("b64")) return false;
        const auto kid = h.value("kid", "");
        if (kid.empty() || kid.size() > 128) return false;
        std::vector<uint8_t> raw_signature;
        if (!chess::crypto::decode_base64url(signature, raw_signature, 64)) return false;
        std::shared_ptr<EVP_PKEY> key;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = clock_();
            if (now >= expires_) {
                if (!refresh_locked(now)) return false; // No expired-key fallback.
            } else if (!keys_.count(kid)) {
                refresh_locked(now); // One bounded refresh per cooldown, not per attacker kid.
            }
            const auto found = keys_.find(kid);
            if (found == keys_.end()) return false;
            key = found->second;
        }
        std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(ECDSA_SIG_new(), ECDSA_SIG_free);
        BIGNUM* r = BN_bin2bn(raw_signature.data(), 32, nullptr);
        BIGNUM* s = BN_bin2bn(raw_signature.data()+32, 32, nullptr);
        if (!sig || !r || !s || ECDSA_SIG_set0(sig.get(), r, s) != 1) {
            BN_free(r); BN_free(s); return false;
        }
        const int length = i2d_ECDSA_SIG(sig.get(), nullptr);
        if (length <= 0) return false;
        std::vector<uint8_t> der(static_cast<size_t>(length));
        auto* cursor = der.data();
        if (i2d_ECDSA_SIG(sig.get(), &cursor) != length) return false;
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        return ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
            EVP_DigestVerify(ctx.get(), der.data(), der.size(),
                reinterpret_cast<const uint8_t*>(input.data()), input.size()) == 1;
    } catch (...) { return false; }
}
}
