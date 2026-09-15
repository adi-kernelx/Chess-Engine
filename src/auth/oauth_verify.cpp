#include "auth/oauth_verify.h"

#include "auth/username.h"
#include "crypto/base64.h"
#include "crypto/constant_time.h"
#include "crypto/hmac.h"
#include "crypto/random.h"
#include "crypto/sha256.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>

namespace chess {
namespace auth {

using namespace chess::crypto;
using namespace chess::storage;
using json = nlohmann::json;

namespace {

constexpr const char* HS256_HEADER_JSON = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

Sha256::Digest sign_hs256(const SecureBuffer& key,
                          const std::string& header_b64,
                          const std::string& payload_b64) {
    HmacSha256 h(key.data(), key.size());
    h.update(reinterpret_cast<const uint8_t*>(header_b64.data()), header_b64.size());
    const uint8_t dot = '.';
    h.update(&dot, 1);
    h.update(reinterpret_cast<const uint8_t*>(payload_b64.data()), payload_b64.size());
    return h.finish();
}

/// Split "a.b.c" into its three pieces. Returns false for any other shape.
bool split_jwt(const std::string& jwt,
               std::string& header, std::string& payload, std::string& sig) {
    const size_t d1 = jwt.find('.');
    if (d1 == std::string::npos) return false;
    const size_t d2 = jwt.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    if (jwt.find('.', d2 + 1) != std::string::npos) return false;
    header  = jwt.substr(0, d1);
    payload = jwt.substr(d1 + 1, d2 - d1 - 1);
    sig     = jwt.substr(d2 + 1);
    return !header.empty() && !payload.empty() && !sig.empty();
}

/// Fetch a string claim (or a bool for the email_verified overload) safely.
bool get_str(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

bool get_i64(const json& j, const char* key, int64_t& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<int64_t>();
    return true;
}

/// Sanitise an email local-part to the §7.6 username whitelist, with a 3..20
/// length. Empty return means the input has no usable characters and the
/// caller must generate a synthetic name.
std::string derive_username_from_email(const std::string& email) {
    const size_t at = email.find('@');
    const std::string local = (at == std::string::npos) ? email : email.substr(0, at);
    std::string out;
    out.reserve(local.size());
    for (unsigned char c : local) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_';
        if (ok) out.push_back(static_cast<char>(c));
    }
    if (out.size() > USERNAME_MAX_LEN) out.resize(USERNAME_MAX_LEN);
    if (out.size() < USERNAME_MIN_LEN) out.clear();
    return out;
}

/// Ensure a stem is not already taken (case-insensitive). Appends `_N` with a
/// random N of up to 4 hex digits, up to 5 tries. Returns empty on give-up.
std::string find_free_username(Database& db, const std::string& stem) {
    if (stem.empty()) return "";
    if (!valid_username(stem)) return "";

    auto taken = [&](const std::string& u) {
        auto r = db.exec("SELECT 1 FROM players WHERE username_ci=$1 LIMIT 1",
                         {Param::text(to_lower_ascii(u))});
        return r.ok && !r.rows.empty();
    };

    if (!taken(stem)) return stem;

    for (int attempt = 0; attempt < 5; ++attempt) {
        uint8_t rb[2];
        secure_random_bytes(rb, sizeof(rb));
        char suffix[6];
        std::snprintf(suffix, sizeof(suffix), "_%03X",
                      static_cast<unsigned>((rb[0] << 4) | (rb[1] & 0x0F)) & 0xFFF);
        // Fit inside the 20-char limit: trim the stem, keep the suffix.
        size_t stem_room = USERNAME_MAX_LEN - std::strlen(suffix);
        std::string candidate = stem.substr(0, stem_room) + suffix;
        if (valid_username(candidate) && !taken(candidate)) return candidate;
    }
    return "";   // pathological — give up so the handler can surface the failure
}

/// Build a synthetic username from an opaque id when the email is unusable.
std::string synthetic_username_from_sub(Database& db, const std::string& sub) {
    // First 6 hex chars of SHA-256(sub) — deterministic per-user, short.
    const auto d = Sha256::hash(reinterpret_cast<const uint8_t*>(sub.data()),
                                sub.size());
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x", d[0], d[1], d[2]);
    return find_free_username(db, std::string("user_") + buf);
}

} // namespace

// ── SupabaseVerifier ─────────────────────────────────────────────────────────

SupabaseVerifier SupabaseVerifier::make(SecureBuffer hs256_secret,
                                        std::string issuer,
                                        std::string audience,
                                        std::string provider) {
    if (hs256_secret.empty() || issuer.empty() || audience.empty() || provider.empty()) {
        return SupabaseVerifier{};
    }
    return SupabaseVerifier(std::move(hs256_secret), std::move(issuer),
                            std::move(audience), std::move(provider));
}

SupabaseVerifier SupabaseVerifier::from_env(std::string& out_error) {
    out_error.clear();
    auto get = [](const char* name) -> const char* {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : nullptr;
    };
    const char* secret_str = get("SUPABASE_JWT_SECRET");
    const char* iss        = get("SUPABASE_ISSUER");
    const char* aud        = get("SUPABASE_AUDIENCE");
    const char* provider   = std::getenv("SUPABASE_PROVIDER");
    if (provider == nullptr || provider[0] == '\0') provider = "google";

    if (!secret_str) { out_error = "SUPABASE_JWT_SECRET is not set"; return {}; }
    if (!iss)        { out_error = "SUPABASE_ISSUER is not set";     return {}; }
    if (!aud)        { out_error = "SUPABASE_AUDIENCE is not set";   return {}; }

    // Supabase's dashboard prints the secret as raw text, not base64. But an
    // operator running us in Docker will often base64-wrap secrets to survive
    // shell escaping — so try that first, fall back to raw bytes.
    std::vector<uint8_t> raw;
    SecureBuffer key;
    if (decode_base64(secret_str, raw) && raw.size() >= 32) {
        key = SecureBuffer(raw.data(), raw.size());
        OPENSSL_cleanse(raw.data(), raw.size());
    } else {
        const size_t n = std::strlen(secret_str);
        if (n < 32) { out_error = "SUPABASE_JWT_SECRET is too short (< 32 bytes)"; return {}; }
        key = SecureBuffer(reinterpret_cast<const uint8_t*>(secret_str), n);
    }
    return make(std::move(key), iss, aud, provider);
}

bool SupabaseVerifier::verify(const std::string& jwt, int64_t now_unix,
                              SupabaseIdentity& out) const {
    out = SupabaseIdentity{};
    if (!valid()) return false;

    std::string header_b64, payload_b64, sig_b64;
    if (!split_jwt(jwt, header_b64, payload_b64, sig_b64)) return false;

    // Header: byte-exact HS256 form. Everything else is algorithm confusion.
    std::vector<uint8_t> header_bytes;
    if (!decode_base64url(header_b64, header_bytes)) return false;
    const std::string header_json(reinterpret_cast<const char*>(header_bytes.data()),
                                  header_bytes.size());
    if (header_json != HS256_HEADER_JSON) return false;

    // Signature next, before any JSON parsing of the payload. Attacker input
    // must not reach the parser unauthenticated — same discipline as token.cpp.
    std::vector<uint8_t> sig;
    if (!decode_base64url(sig_b64, sig, Sha256::DIGEST_SIZE)) return false;
    const auto expected = sign_hs256(secret_, header_b64, payload_b64);
    if (!constant_time_equals(expected.data(), sig.data(), sig.size())) return false;

    // Now the payload.
    std::vector<uint8_t> payload_bytes;
    if (!decode_base64url(payload_b64, payload_bytes)) return false;
    const std::string payload_json(reinterpret_cast<const char*>(payload_bytes.data()),
                                   payload_bytes.size());
    json payload = json::parse(payload_json, nullptr, /*allow_exceptions=*/false);
    if (payload.is_discarded() || !payload.is_object()) return false;

    // The project-invariant claims. iss/aud change if you migrate to a
    // different Supabase project or role, and both are explicit here so a
    // token from a different project fails loudly at the boundary.
    std::string iss, aud, sub, email;
    int64_t exp = 0, iat = 0;
    if (!get_str(payload, "iss", iss) || iss != issuer_) return false;
    if (!get_str(payload, "aud", aud) || aud != audience_) return false;
    if (!get_str(payload, "sub", sub) || sub.empty())      return false;
    if (!get_i64(payload, "exp", exp) || now_unix >= exp)  return false;
    // iat is only checked for being "not wildly in the future"; a small skew
    // matches token.h's own tolerance.
    if (get_i64(payload, "iat", iat) && now_unix < iat - 60) return false;

    if (!get_str(payload, "email", email)) return false;   // required for OAuth

    // email_verified may sit at the top level (newer Supabase versions) or
    // under user_metadata (older ones). Accept true from either; a missing or
    // false value must fail — Google's own claim about the email is our only
    // ground truth here.
    auto is_true = [](const json& j) {
        return j.is_boolean() && j.get<bool>();
    };
    bool verified = false;
    {
        auto top = payload.find("email_verified");
        if (top != payload.end() && is_true(*top)) verified = true;

        auto um = payload.find("user_metadata");
        if (!verified && um != payload.end() && um->is_object()) {
            auto ev = um->find("email_verified");
            if (ev != um->end() && is_true(*ev)) verified = true;
        }
    }
    if (!verified) return false;

    // Provider gate. app_metadata is admin-controlled (users cannot modify
    // it), which is what makes this claim trustworthy.
    std::string provider;
    auto app = payload.find("app_metadata");
    if (app != payload.end() && app->is_object()) {
        auto pr = app->find("provider");
        if (pr != app->end() && pr->is_string()) provider = pr->get<std::string>();
    }
    if (provider != provider_) return false;

    out.sub            = std::move(sub);
    out.email          = std::move(email);
    out.email_verified = true;
    out.provider       = std::move(provider);
    return true;
}

// ── Service layer ────────────────────────────────────────────────────────────

GoogleSignInResult google_sign_in(Database& db, const SupabaseVerifier& verifier,
                                  const std::string& supabase_jwt,
                                  int64_t now_unix) {
    GoogleSignInResult r;

    SupabaseIdentity id;
    if (!verifier.verify(supabase_jwt, now_unix, id)) {
        r.status = GoogleSignInStatus::InvalidToken;
        return r;
    }

    // Existing account by google_sub? That is the happy path — sign in.
    auto sel = db.exec(
        "SELECT id, username, elo_rating, token_epoch"
        "  FROM players WHERE google_sub=$1",
        {Param::text(id.sub)});
    if (!sel.ok) { r.status = GoogleSignInStatus::DatabaseError; return r; }

    if (!sel.rows.empty()) {
        r.status      = GoogleSignInStatus::Ok;
        r.player_id   = std::strtoll(sel.first().at(0).c_str(), nullptr, 10);
        r.username    = sel.first().at(1);
        r.elo_rating  = std::atoi(sel.first().at(2).c_str());
        r.token_epoch = std::atoi(sel.first().at(3).c_str());
        db.exec("UPDATE players SET last_login=now() WHERE id=$1",
                {Param::int64(r.player_id)});
        return r;
    }

    // First-time sign-in. Refuse if the email is already taken — auto-linking
    // on email is the classical takeover vector.
    auto by_email = db.exec("SELECT 1 FROM players WHERE email=$1 LIMIT 1",
                            {Param::text(id.email)});
    if (!by_email.ok) { r.status = GoogleSignInStatus::DatabaseError; return r; }
    if (!by_email.rows.empty()) {
        r.status = GoogleSignInStatus::EmailCollision;
        return r;
    }

    // Derive a username. The whitelist means most emails yield something
    // usable; anything wildly off falls through to a synthetic name.
    std::string username = find_free_username(db, derive_username_from_email(id.email));
    if (username.empty()) {
        username = synthetic_username_from_sub(db, id.sub);
    }
    if (username.empty()) { r.status = GoogleSignInStatus::InternalError; return r; }

    auto ins = db.exec(
        "INSERT INTO players(username, username_ci, email, google_sub)"
        " VALUES($1,$2,$3,$4) RETURNING id, elo_rating, token_epoch",
        {Param::text(username), Param::text(to_lower_ascii(username)),
         Param::text(id.email), Param::text(id.sub)});
    if (!ins.ok) {
        // A race with a concurrent sign-in for the same sub/email would land
        // here as 23505. Convert unique_violation to EmailCollision so the
        // client message stays consistent.
        if (ins.sqlstate == storage::pg_errors::UNIQUE_VIOLATION) {
            r.status = GoogleSignInStatus::EmailCollision;
        } else {
            r.status = GoogleSignInStatus::DatabaseError;
        }
        return r;
    }

    r.status              = GoogleSignInStatus::Ok;
    r.player_id           = std::strtoll(ins.first().at(0).c_str(), nullptr, 10);
    r.username            = username;
    r.elo_rating          = std::atoi(ins.first().at(1).c_str());
    r.token_epoch         = std::atoi(ins.first().at(2).c_str());
    r.created_new_account = true;
    return r;
}

LinkStatus link_google(Database& db, const SupabaseVerifier& verifier,
                       int64_t player_id, const std::string& supabase_jwt,
                       int64_t now_unix) {
    SupabaseIdentity id;
    if (!verifier.verify(supabase_jwt, now_unix, id)) return LinkStatus::InvalidToken;

    // Is this account already linked to something? If so, refuse — the user
    // must unlink first. Silently overwriting would let a compromised Google
    // account be swapped out for a different one with no audit trail.
    auto self = db.exec("SELECT google_sub FROM players WHERE id=$1",
                        {Param::int64(player_id)});
    if (!self.ok || self.rows.empty()) return LinkStatus::DatabaseError;
    if (!self.first().is_null(0) && self.first().at(0) != id.sub) {
        return LinkStatus::AlreadyHasGoogle;
    }

    auto other = db.exec("SELECT id FROM players WHERE google_sub=$1 AND id<>$2",
                         {Param::text(id.sub), Param::int64(player_id)});
    if (!other.ok) return LinkStatus::DatabaseError;
    if (!other.rows.empty()) return LinkStatus::AlreadyLinkedElsewhere;

    auto up = db.exec("UPDATE players SET google_sub=$1 WHERE id=$2",
                      {Param::text(id.sub), Param::int64(player_id)});
    if (!up.ok) {
        if (up.sqlstate == storage::pg_errors::UNIQUE_VIOLATION) {
            return LinkStatus::AlreadyLinkedElsewhere;
        }
        return LinkStatus::DatabaseError;
    }
    return LinkStatus::Ok;
}

UnlinkStatus unlink_google(Database& db, int64_t player_id) {
    auto sel = db.exec(
        "SELECT google_sub, password_hash FROM players WHERE id=$1",
        {Param::int64(player_id)});
    if (!sel.ok || sel.rows.empty()) return UnlinkStatus::DatabaseError;

    if (sel.first().is_null(0)) return UnlinkStatus::NotLinked;
    // Human-friendly check before the DB's CHECK fires. Both must catch this
    // — the app-layer refusal gives the user a useful message, the CHECK is
    // the backstop that survives a code bug or a hand-crafted UPDATE.
    if (sel.first().is_null(1)) return UnlinkStatus::LastLoginMethod;

    auto up = db.exec("UPDATE players SET google_sub=NULL WHERE id=$1",
                      {Param::int64(player_id)});
    if (!up.ok) {
        if (up.sqlstate == storage::pg_errors::CHECK_VIOLATION) {
            return UnlinkStatus::LastLoginMethod;
        }
        return UnlinkStatus::DatabaseError;
    }
    return UnlinkStatus::Ok;
}

} // namespace auth
} // namespace chess
