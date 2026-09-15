#include "auth/token.h"

#include "crypto/base64.h"
#include "crypto/constant_time.h"
#include "crypto/hmac.h"
#include "crypto/random.h"
#include "crypto/sha512.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace chess {
namespace auth {

using namespace chess::crypto;
using json = nlohmann::json;

namespace {

/// The signed input is `base64url(header) || "." || base64url(payload)`.
/// Header is fixed for HS384; canonicalising it out of a struct rather than
/// re-encoding from JSON every time keeps the byte string byte-stable across
/// nlohmann versions and platforms.
constexpr const char* HS384_HEADER_JSON = "{\"alg\":\"HS384\",\"typ\":\"JWT\"}";

std::string encode_json(const json& j) {
    // Compact serialisation, no spaces. Order matters for reproducibility, but
    // the header we sign uses the FIXED string above — the JWT spec only
    // requires the sender's byte string to be signed, so as long as we use one
    // canonical form and re-verify against the exact bytes received, we are
    // fine.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

Sha384::Digest sign_bytes(const SecureBuffer& key,
                          const std::string& header_b64,
                          const std::string& payload_b64) {
    HmacSha384 h(key.data(), key.size());
    h.update(reinterpret_cast<const uint8_t*>(header_b64.data()), header_b64.size());
    const uint8_t dot = '.';
    h.update(&dot, 1);
    h.update(reinterpret_cast<const uint8_t*>(payload_b64.data()), payload_b64.size());
    return h.finish();
}

} // namespace

// ── TokenSigner ──────────────────────────────────────────────────────────────

TokenSigner TokenSigner::from_key(SecureBuffer key) {
    if (key.size() != 32) return TokenSigner{};
    return TokenSigner(std::move(key));
}

TokenSigner TokenSigner::from_env(std::string& out_error) {
    out_error.clear();
    const char* value = std::getenv("JWT_SIGNING_KEY");
    if (value == nullptr || value[0] == '\0') {
        out_error = "JWT_SIGNING_KEY is not set";
        return TokenSigner{};
    }
    std::vector<uint8_t> raw;
    if (!decode_base64(value, raw, /*expected_len=*/32)) {
        out_error = "JWT_SIGNING_KEY is not exactly 32 bytes of base64";
        return TokenSigner{};
    }
    SecureBuffer k(raw.data(), raw.size());
    // Wipe the intermediate — a plain vector is not zeroed on destruction.
    OPENSSL_cleanse(raw.data(), raw.size());
    return TokenSigner(std::move(k));
}

TokenSigner TokenSigner::generate_random() {
    return TokenSigner(secure_random_buffer(32));
}

std::string TokenSigner::issue_access(const AccessClaims& c) const {
    if (!valid()) return std::string();

    json payload = {
        {"sub",      c.player_id},
        {"username", c.username},
        {"epoch",    c.token_epoch},
        {"iat",      c.issued_at},
        {"exp",      c.expires_at},
    };

    const std::string header_json = HS384_HEADER_JSON;
    const std::string payload_json = encode_json(payload);

    const std::string header_b64 = encode_base64url(
        reinterpret_cast<const uint8_t*>(header_json.data()), header_json.size());
    const std::string payload_b64 = encode_base64url(
        reinterpret_cast<const uint8_t*>(payload_json.data()), payload_json.size());

    const Sha384::Digest sig = sign_bytes(key_, header_b64, payload_b64);
    const std::string sig_b64 = encode_base64url(sig.data(), sig.size());

    return header_b64 + "." + payload_b64 + "." + sig_b64;
}

bool TokenSigner::verify_access(const std::string& jwt, int64_t now_unix,
                                AccessClaims& out) const {
    out = AccessClaims{};
    if (!valid()) return false;

    // Locate the two dots. A JWT has exactly two. Any other count is malformed
    // and must be rejected before base64url work happens.
    const size_t d1 = jwt.find('.');
    if (d1 == std::string::npos) return false;
    const size_t d2 = jwt.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    if (jwt.find('.', d2 + 1) != std::string::npos) return false;

    const std::string header_b64  = jwt.substr(0, d1);
    const std::string payload_b64 = jwt.substr(d1 + 1, d2 - d1 - 1);
    const std::string sig_b64     = jwt.substr(d2 + 1);
    if (header_b64.empty() || payload_b64.empty() || sig_b64.empty()) return false;

    // Header must be exactly what we produce. Reconstructing from parsed JSON
    // and comparing structurally would let algorithm-confusion attacks through
    // any JSON-canonicalisation subtlety we did not anticipate — better to
    // require byte equality against a fixed constant.
    std::vector<uint8_t> header_bytes;
    if (!decode_base64url(header_b64, header_bytes)) return false;
    const std::string header_json(reinterpret_cast<const char*>(header_bytes.data()),
                                  header_bytes.size());
    if (header_json != HS384_HEADER_JSON) return false;

    // Now the signature. Byte-order matters: verify BEFORE parsing the payload,
    // so a malformed payload from an attacker never triggers JSON code.
    std::vector<uint8_t> sig;
    if (!decode_base64url(sig_b64, sig, Sha384::DIGEST_SIZE)) return false;

    const Sha384::Digest expected = sign_bytes(key_, header_b64, payload_b64);
    if (!constant_time_equals(expected.data(), sig.data(), sig.size())) return false;

    // Payload only after the tag verified.
    std::vector<uint8_t> payload_bytes;
    if (!decode_base64url(payload_b64, payload_bytes)) return false;
    const std::string payload_json(reinterpret_cast<const char*>(payload_bytes.data()),
                                   payload_bytes.size());
    json payload = json::parse(payload_json, nullptr, /*allow_exceptions=*/false);
    if (payload.is_discarded() || !payload.is_object()) return false;

    // Strict on type of each claim. A JWT with `"exp":"soon"` should not silently
    // become a JWT with exp=0.
    auto get_i64 = [&](const char* key, int64_t& v) {
        auto it = payload.find(key);
        if (it == payload.end() || !it->is_number_integer()) return false;
        v = it->get<int64_t>();
        return true;
    };
    auto get_str = [&](const char* key, std::string& v) {
        auto it = payload.find(key);
        if (it == payload.end() || !it->is_string()) return false;
        v = it->get<std::string>();
        return true;
    };

    AccessClaims c;
    int64_t epoch64 = 0;
    if (!get_i64("sub",   c.player_id))  return false;
    if (!get_str("username", c.username)) return false;
    if (!get_i64("epoch", epoch64))      return false;
    if (!get_i64("iat",   c.issued_at))  return false;
    if (!get_i64("exp",   c.expires_at)) return false;
    if (epoch64 < 0 || epoch64 > INT32_MAX) return false;
    c.token_epoch = static_cast<int>(epoch64);

    // Time checks. `iat > now + 60` catches a token issued in the future
    // (either a wildly wrong clock or a caller injecting one); a small skew
    // window is normal, but a large one hides a bug.
    if (now_unix < c.issued_at - 60) return false;
    if (now_unix >= c.expires_at)    return false;

    out = c;
    return true;
}

// ── Refresh tokens ───────────────────────────────────────────────────────────

std::string mint_refresh_token() {
    uint8_t bytes[REFRESH_TOKEN_BYTES];
    secure_random_bytes(bytes, sizeof(bytes));
    std::string out = encode_base64url(bytes, sizeof(bytes));
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return out;
}

std::string refresh_token_hash(const std::string& token) {
    Sha384::Digest d = Sha384::hash(
        reinterpret_cast<const uint8_t*>(token.data()), token.size());
    return Sha384::to_hex(d);
}

std::string new_uuid_v4() {
    uint8_t b[16];
    secure_random_bytes(b, sizeof(b));
    // RFC 4122: set the version (4) and variant (10xx) bits.
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3],  b[4], b[5],  b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out, 36);
}

} // namespace auth
} // namespace chess
