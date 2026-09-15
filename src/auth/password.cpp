#include "auth/password.h"

#include "crypto/random.h"
#include "crypto/secure_buffer.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace chess {
namespace auth {

using namespace chess::crypto;

namespace {

// ── Unpadded base64 (PHC dialect) ────────────────────────────────────────────
// RFC 9106 §3.2 and the PHC spec use the standard alphabet with padding
// stripped. We already have a strict padded codec in crypto/base64; this file
// wraps it because PHC and the wire protocol differ in one detail only.

const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int8_t B64_REV[256];

std::once_flag rev_once;
void init_rev() {
    for (int i = 0; i < 256; ++i) B64_REV[i] = -1;
    for (int i = 0; i < 64; ++i) B64_REV[static_cast<uint8_t>(B64[i])] = static_cast<int8_t>(i);
}

std::string b64_unpadded(const uint8_t* data, size_t len) {
    std::string out;
    if (len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i])   << 16) |
                           (static_cast<uint32_t>(data[i+1]) << 8)  |
                            static_cast<uint32_t>(data[i+2]);
        out.push_back(B64[(v >> 18) & 63]);
        out.push_back(B64[(v >> 12) & 63]);
        out.push_back(B64[(v >>  6) & 63]);
        out.push_back(B64[ v        & 63]);
    }
    if (i < len) {
        const bool two = (i + 1 < len);
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (two) v |= static_cast<uint32_t>(data[i+1]) << 8;
        out.push_back(B64[(v >> 18) & 63]);
        out.push_back(B64[(v >> 12) & 63]);
        if (two) out.push_back(B64[(v >> 6) & 63]);
    }
    return out;
}

bool b64_unpadded_decode(const std::string& s, std::vector<uint8_t>& out) {
    std::call_once(rev_once, init_rev);
    out.clear();
    if (s.empty()) return true;

    // Length mod 4 must be 0, 2 or 3; a padded remainder of 1 is impossible.
    const size_t r = s.size() % 4;
    if (r == 1) return false;

    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        const int8_t d = B64_REV[c];
        if (d < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero — a non-canonical trailing sextet would let
    // two different strings decode to the same hash.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) { out.clear(); return false; }
    return true;
}

// ── The Argon2id call ────────────────────────────────────────────────────────

struct Argon2Params {
    uint32_t m_kib   = argon2::MEMCOST_KIB;
    uint32_t t_iters = argon2::ITERATIONS;
    uint32_t p_lanes = argon2::PARALLELISM;
    uint32_t version = argon2::VERSION;
};

bool argon2id_derive(const std::string& password,
                     const uint8_t* salt, size_t salt_len,
                     const Argon2Params& p,
                     uint8_t* out, size_t out_len) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!kdf) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return false;

    // OpenSSL 3.5 accepts these parameter names for Argon2id — confirmed by
    // scratchpad/probe_argon.c. Field names other than these will silently
    // be ignored, so keep the exact spelling.
    uint32_t lanes  = p.p_lanes;
    uint32_t mem    = p.m_kib;
    uint32_t iters  = p.t_iters;
    // Version defaults to 0x13; passing it explicitly makes rollback obvious
    // if an older 0x10 hash is ever re-derived.
    uint32_t version = p.version;

    OSSL_PARAM params[] = {
        OSSL_PARAM_octet_string("pass", const_cast<char*>(password.data()), password.size()),
        OSSL_PARAM_octet_string("salt", const_cast<uint8_t*>(salt),         salt_len),
        OSSL_PARAM_uint("lanes",   &lanes),
        OSSL_PARAM_uint("memcost", &mem),
        OSSL_PARAM_uint("iter",    &iters),
        OSSL_PARAM_uint("version", &version),
        OSSL_PARAM_END,
    };
    const int r = EVP_KDF_derive(ctx, out, out_len, params);
    EVP_KDF_CTX_free(ctx);
    return r == 1;
}

// ── PHC string encoding and parsing ──────────────────────────────────────────

std::string encode_phc(const Argon2Params& p,
                       const uint8_t* salt, size_t salt_len,
                       const uint8_t* hash, size_t hash_len) {
    std::string out = "$argon2id$v=" + std::to_string(p.version) +
                      "$m=" + std::to_string(p.m_kib) +
                      ",t=" + std::to_string(p.t_iters) +
                      ",p=" + std::to_string(p.p_lanes) +
                      "$" + b64_unpadded(salt, salt_len) +
                      "$" + b64_unpadded(hash, hash_len);
    return out;
}

/**
 * Parse a PHC string into its parts. Structural checks are strict — this
 * function is called once at every login, so a lenient parser would be a
 * per-request oracle. Returns false with everything cleared on any deviation.
 */
bool parse_phc(const std::string& phc, Argon2Params& p,
               std::vector<uint8_t>& salt, std::vector<uint8_t>& hash) {
    p    = Argon2Params{};
    salt.clear();
    hash.clear();

    if (phc.rfind("$argon2id$", 0) != 0) return false;

    // Split on '$'. Fields: "", "argon2id", "v=19", "m=…,t=…,p=…", "salt", "hash".
    std::vector<std::string> parts;
    parts.reserve(6);
    size_t start = 0;
    for (size_t i = 0; i <= phc.size(); ++i) {
        if (i == phc.size() || phc[i] == '$') {
            parts.emplace_back(phc.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 6) return false;
    if (parts[1] != "argon2id") return false;

    if (parts[2].rfind("v=", 0) != 0) return false;
    p.version = static_cast<uint32_t>(std::strtoul(parts[2].c_str() + 2, nullptr, 10));
    if (p.version != argon2::VERSION) return false;

    // The parameter list is comma-separated and MUST be ordered m,t,p per PHC.
    // A permissive parser here is a canonicalisation attack vector.
    unsigned long m = 0, t = 0, l = 0;
    if (std::sscanf(parts[3].c_str(), "m=%lu,t=%lu,p=%lu", &m, &t, &l) != 3) return false;
    // Sanity bounds — reject anything wildly outside plausible operational
    // range so a corrupted row cannot make us spend a minute per login.
    if (m == 0 || m > (1u << 20))  return false;   // ≤ 1 GiB
    if (t == 0 || t > 64)          return false;
    if (l == 0 || l > 16)          return false;
    p.m_kib   = static_cast<uint32_t>(m);
    p.t_iters = static_cast<uint32_t>(t);
    p.p_lanes = static_cast<uint32_t>(l);

    if (!b64_unpadded_decode(parts[4], salt) || salt.empty())  return false;
    if (!b64_unpadded_decode(parts[5], hash) || hash.empty())  return false;

    return true;
}

// ── The dummy hash, computed once and cached ─────────────────────────────────

std::once_flag dummy_once;
std::string    dummy_cache;

void init_dummy() {
    // A random password we do not know. This just needs to *exist* and hash
    // under the current parameters so verification takes the current cost.
    SecureBuffer pw = secure_random_buffer(32);
    dummy_cache = hash_password(std::string(reinterpret_cast<const char*>(pw.data()), pw.size()));
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

std::string hash_password(const std::string& password) {
    return hash_password_with_params(password,
                                     argon2::MEMCOST_KIB,
                                     argon2::ITERATIONS,
                                     argon2::PARALLELISM);
}

std::string hash_password_with_params(const std::string& password,
                                      uint32_t memcost_kib,
                                      uint32_t iterations,
                                      uint32_t parallelism) {
    // Same guardrails parse_phc() enforces. A caller that hands us m=0 or a
    // memcost the size of a small planet should fail here rather than at load.
    if (memcost_kib == 0 || memcost_kib > (1u << 20)) return std::string();
    if (iterations == 0 || iterations > 64)          return std::string();
    if (parallelism == 0 || parallelism > 16)        return std::string();

    Argon2Params p;
    p.m_kib   = memcost_kib;
    p.t_iters = iterations;
    p.p_lanes = parallelism;

    uint8_t salt[argon2::SALT_SIZE];
    secure_random_bytes(salt, sizeof(salt));

    uint8_t hash[argon2::HASH_SIZE];
    if (!argon2id_derive(password, salt, sizeof(salt), p, hash, sizeof(hash))) {
        return std::string();
    }
    return encode_phc(p, salt, sizeof(salt), hash, sizeof(hash));
}

bool verify_password(const std::string& password, const std::string& phc) {
    Argon2Params p;
    std::vector<uint8_t> salt, expected;
    if (!parse_phc(phc, p, salt, expected)) return false;

    // Derive with the SAME output length as the stored hash. A different length
    // parameter would trivially produce a mismatch, but derive() would also
    // waste that additional work — parse guarantees the two agree.
    std::vector<uint8_t> got(expected.size());
    if (!argon2id_derive(password, salt.data(), salt.size(), p,
                         got.data(), got.size())) {
        return false;
    }

    // Constant-time. CRYPTO_memcmp returns 0 on equal, non-zero otherwise —
    // note the inverted convention compared to std::memcmp's ordering.
    const bool ok = CRYPTO_memcmp(got.data(), expected.data(), expected.size()) == 0;
    OPENSSL_cleanse(got.data(), got.size());
    return ok;
}

bool needs_upgrade(const std::string& phc) {
    Argon2Params p;
    std::vector<uint8_t> salt, hash;
    if (!parse_phc(phc, p, salt, hash)) return true;   // unparseable == replace
    return p.m_kib   < argon2::MEMCOST_KIB ||
           p.t_iters < argon2::ITERATIONS  ||
           p.p_lanes < argon2::PARALLELISM ||
           hash.size() != argon2::HASH_SIZE;
}

const std::string& dummy_phc() {
    std::call_once(dummy_once, init_dummy);
    return dummy_cache;
}

} // namespace auth
} // namespace chess
