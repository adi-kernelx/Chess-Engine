/**
 * test_oauth.cpp — Phase 7.8: Supabase JWT verification + Google Sign-In.
 *
 * We cannot exercise a real Supabase project from a CI run, so the test mints
 * its own HS256 JWTs shaped exactly like Supabase's own — same header, same
 * claim set, signed with a known secret — and hands them to the verifier.
 * That is enough to prove every branch a hostile token can take: bad
 * signature, bad iss/aud, bad exp, bad provider, unverified email.
 *
 * The service-level tests (Google Sign-In, link, unlink) run against the same
 * ephemeral Postgres the other DB tests use, and check the account-linking
 * safety rule explicitly: an unknown Google identity whose email matches an
 * existing account must NOT auto-link — that is the classical account
 * takeover vector.
 */

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth/oauth_verify.h"
#include "auth/service.h"
#include "auth/token.h"
#include "crypto/base64.h"
#include "crypto/hmac.h"
#include "crypto/secure_buffer.h"
#include "crypto/sha256.h"
#include "storage/database.h"

using namespace chess::auth;
using namespace chess::crypto;
using namespace chess::storage;
using json = nlohmann::json;

// ============================================================
// Harness
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

static void run_test(const std::string& name, std::function<bool()> fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (fn()) { std::cout << "PASS" << std::endl; g_passed++; }
    else      { std::cout << "FAIL" << std::endl; g_failed++; }
}

// ============================================================
// A tiny HS256 JWT signer, for building Supabase-shaped tokens.
// This is TEST CODE ONLY — the production verifier never signs.
// ============================================================

static std::string b64url_of(const std::string& s) {
    return encode_base64url(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static std::string sign_hs256(const SecureBuffer& key, const json& payload,
                              const std::string& header = R"({"alg":"HS256","typ":"JWT"})") {
    const std::string h_b64 = b64url_of(header);
    const std::string p_b64 = b64url_of(payload.dump());
    HmacSha256 h(key.data(), key.size());
    h.update(reinterpret_cast<const uint8_t*>(h_b64.data()), h_b64.size());
    const uint8_t dot = '.';
    h.update(&dot, 1);
    h.update(reinterpret_cast<const uint8_t*>(p_b64.data()), p_b64.size());
    const auto sig = h.finish();
    const std::string s_b64 = encode_base64url(sig.data(), sig.size());
    return h_b64 + "." + p_b64 + "." + s_b64;
}

// A canned "well-formed Supabase JWT" — payload constructor for reuse. Callers
// can mutate the returned object before signing, which is how each test
// carves out just the one field it wants to break.
static json fresh_supabase_payload(int64_t now) {
    return {
        {"iss", "https://xyz.supabase.co/auth/v1"},
        {"aud", "authenticated"},
        {"sub", "sup-user-1234"},         // Supabase's UUID — the stable id
        {"email", "adi@example.com"},
        {"email_verified", true},         // top-level (newer Supabase)
        {"role", "authenticated"},
        {"iat", now},
        {"exp", now + 3600},
        {"app_metadata", {{"provider", "google"}, {"providers", {"google"}}}},
        // user_metadata carries provider-native fields; we do NOT read `sub`
        // from here — the top-level Supabase `sub` is our key.
        {"user_metadata", {{"email_verified", true}, {"name", "Adi"},
                           {"provider_id", "google-oauth-sub-99"}}},
    };
}

// Shared verifier for the whole suite — same secret used to sign.
static SecureBuffer make_secret() {
    // 32 bytes of a known value keeps failures reproducible.
    const std::string s = "abcdefghijklmnopqrstuvwxyzABCDEF";
    return SecureBuffer(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static SupabaseVerifier make_verifier() {
    return SupabaseVerifier::make(
        make_secret(),
        "https://xyz.supabase.co/auth/v1",
        "authenticated",
        "google");
}

// ============================================================
// Verifier
// ============================================================

static void test_verifier() {
    std::cout << "\n--- SupabaseVerifier (HS256) ---" << std::endl;

    const int64_t now = 1'700'000'000;
    const SupabaseVerifier verifier = make_verifier();

    run_test("Well-formed Supabase JWT verifies and extracts identity", [&] {
        const std::string jwt = sign_hs256(make_secret(), fresh_supabase_payload(now));
        SupabaseIdentity id;
        return verifier.verify(jwt, now + 10, id) &&
               id.sub == "sup-user-1234" &&
               id.email == "adi@example.com" &&
               id.email_verified &&
               id.provider == "google";
    });

    run_test("Tampered payload is rejected", [&] {
        // Sign a valid token, then swap the payload bytes for a different one.
        // The signature stays valid against the ORIGINAL — a forged verifier
        // that ignored the tag would return the swapped identity.
        const std::string good = sign_hs256(make_secret(), fresh_supabase_payload(now));
        const size_t d1 = good.find('.');
        const size_t d2 = good.find('.', d1 + 1);
        json evil = fresh_supabase_payload(now);
        evil["sub"] = "attacker";
        const std::string forged = good.substr(0, d1 + 1) + b64url_of(evil.dump()) +
                                   good.substr(d2);
        SupabaseIdentity id;
        return !verifier.verify(forged, now + 10, id);
    });

    run_test("Wrong signing key is rejected", [&] {
        SecureBuffer other = SecureBuffer::from_string(
            "different_secret_of_at_least_32_bytes____");
        const std::string jwt = sign_hs256(other, fresh_supabase_payload(now));
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("Expired token is rejected", [&] {
        json p = fresh_supabase_payload(now);
        p["exp"] = now - 1;
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return !verifier.verify(jwt, now, id);
    });

    run_test("Wrong iss is rejected (token from a different Supabase project)", [&] {
        json p = fresh_supabase_payload(now);
        p["iss"] = "https://evil.supabase.co/auth/v1";
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("Wrong aud is rejected", [&] {
        json p = fresh_supabase_payload(now);
        p["aud"] = "service_role";
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("email_verified=false is rejected", [&] {
        json p = fresh_supabase_payload(now);
        p["email_verified"] = false;
        p["user_metadata"]["email_verified"] = false;
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("Missing email_verified is rejected (treat as unverified)", [&] {
        json p = fresh_supabase_payload(now);
        p.erase("email_verified");
        p["user_metadata"].erase("email_verified");
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("email_verified in user_metadata alone still passes", [&] {
        // Older Supabase versions surface the flag only under user_metadata.
        json p = fresh_supabase_payload(now);
        p.erase("email_verified");
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return verifier.verify(jwt, now + 10, id) && id.email_verified;
    });

    run_test("Wrong provider is rejected (Supabase email/password user)", [&] {
        // A Supabase account created with email/password has provider="email".
        // Accepting that would let anyone with a Supabase account impersonate
        // a Google identity.
        json p = fresh_supabase_payload(now);
        p["app_metadata"]["provider"] = "email";
        const std::string jwt = sign_hs256(make_secret(), p);
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("alg:none is rejected", [&] {
        // Classical JWT vulnerability. If accepted, an attacker forges any
        // claim set. The verifier requires the byte-exact HS256 header.
        json p = fresh_supabase_payload(now);
        std::string forged =
            b64url_of(R"({"alg":"none","typ":"JWT"})") + "." +
            b64url_of(p.dump()) + ".";
        SupabaseIdentity id;
        return !verifier.verify(forged, now + 10, id);
    });

    run_test("Algorithm confusion (HS384 header) is rejected", [&] {
        // Even though our project uses HS384 elsewhere, this verifier is
        // HS256-only. A mixed header must not slip through.
        const std::string jwt = sign_hs256(
            make_secret(), fresh_supabase_payload(now),
            R"({"alg":"HS384","typ":"JWT"})");
        SupabaseIdentity id;
        return !verifier.verify(jwt, now + 10, id);
    });

    run_test("Malformed shape is rejected without crashing", [&] {
        SupabaseIdentity id;
        return !verifier.verify("", 0, id) &&
               !verifier.verify("only.one", 0, id) &&
               !verifier.verify("a.b.c.d", 0, id);
    });

    run_test("Invalid verifier (no secret) refuses everything", [] {
        SupabaseVerifier v;
        SupabaseIdentity id;
        return !v.valid() && !v.verify("a.b.c", 0, id);
    });
}

// ============================================================
// Service — needs the DB
// ============================================================

static const char* SCHEMA_ENV     = "SCHEMA_PATH";
static const char* DEFAULT_SCHEMA = "src/storage/schema_phase7.sql";

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

static void reset(Database& db) {
    std::string err;
    db.run_script("DROP TABLE IF EXISTS sessions; "
                  "DROP TABLE IF EXISTS players; "
                  "DROP FUNCTION IF EXISTS assert_username_ci_matches();", err);
    const char* path = std::getenv(SCHEMA_ENV);
    db.run_script(read_file(path ? path : DEFAULT_SCHEMA), err);
}

static std::string signed_supabase_jwt(int64_t now,
                                       const std::string& sub,
                                       const std::string& email,
                                       bool email_verified = true,
                                       const std::string& provider = "google") {
    json p = fresh_supabase_payload(now);
    p["sub"]   = sub;
    p["email"] = email;
    p["email_verified"] = email_verified;
    p["user_metadata"]["email_verified"] = email_verified;
    p["app_metadata"]["provider"] = provider;
    return sign_hs256(make_secret(), p);
}

static void test_service(Database& db) {
    std::cout << "\n--- Google Sign-In service ---" << std::endl;

    const int64_t now = 1'700'000'000;
    const SupabaseVerifier verifier = make_verifier();

    run_test("Unknown sub creates a new account exactly once", [&] {
        reset(db);
        const std::string jwt = signed_supabase_jwt(now, "sup-1", "adi@example.com");

        auto r1 = google_sign_in(db, verifier, jwt, now + 5);
        if (r1.status != GoogleSignInStatus::Ok || !r1.created_new_account) return false;
        if (r1.username.empty() || r1.elo_rating != 1200) return false;

        // A second call must return THE SAME row, not create a duplicate.
        auto r2 = google_sign_in(db, verifier, jwt, now + 10);
        if (r2.status != GoogleSignInStatus::Ok || r2.created_new_account) return false;
        if (r2.player_id != r1.player_id) return false;

        auto n = db.exec("SELECT count(*) FROM players");
        return n.ok && n.first().at(0) == "1";
    });

    run_test("Derived username is on the §7.6 whitelist", [&] {
        reset(db);
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-1", "Adi_42@example.com"), now + 5);
        return r.status == GoogleSignInStatus::Ok && r.username == "Adi_42";
    });

    run_test("Email with characters outside the whitelist still yields a valid name", [&] {
        reset(db);
        // Dots and dashes are stripped; the result must still be 3..20 of
        // [A-Za-z0-9_]. If nothing survives, a synthetic name is generated.
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-2", "a.b-c@example.com"), now + 5);
        // "abc" survives the filter — check it validates.
        return r.status == GoogleSignInStatus::Ok && r.username == "abc";
    });

    run_test("Email collision does NOT auto-link (account-takeover defence)", [&] {
        reset(db);
        // Pre-existing password account with a specific email.
        register_password_user(db, "Adi", "hunter2!");
        db.exec("UPDATE players SET email=$1 WHERE username_ci='adi'",
                {Param::text("adi@example.com")});

        // A Google identity arrives claiming the same email. Auto-linking on
        // email is the classical takeover: an attacker who owns a Google
        // account with the target's email would inherit the account.
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "attacker-sup", "adi@example.com"), now + 5);
        return r.status == GoogleSignInStatus::EmailCollision &&
               // And nothing was written under the attacker's sub.
               db.exec("SELECT count(*) FROM players WHERE google_sub=$1",
                       {Param::text("attacker-sup")}).first().at(0) == "0";
    });

    run_test("Rejected JWT: expired", [&] {
        reset(db);
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-3", "x@example.com"),
            now + 4000);   // token exp is now + 3600
        return r.status == GoogleSignInStatus::InvalidToken;
    });

    run_test("Rejected JWT: unverified email", [&] {
        reset(db);
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-4", "x@example.com", /*verified=*/false),
            now + 5);
        return r.status == GoogleSignInStatus::InvalidToken;
    });

    run_test("Rejected JWT: wrong provider (email/password Supabase user)", [&] {
        reset(db);
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-5", "x@example.com", true, "email"),
            now + 5);
        return r.status == GoogleSignInStatus::InvalidToken;
    });

    run_test("Repeat sign-in updates last_login", [&] {
        reset(db);
        google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-6", "l@example.com"), now + 5);
        auto before = db.exec(
            "SELECT last_login FROM players WHERE google_sub=$1",
            {Param::text("sup-6")});
        if (!before.first().is_null(0)) return false;   // fresh row: NULL

        google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-6", "l@example.com"), now + 50);
        auto after = db.exec(
            "SELECT last_login FROM players WHERE google_sub=$1",
            {Param::text("sup-6")});
        return !after.first().is_null(0);
    });

    run_test("link_google attaches to an existing account", [&] {
        reset(db);
        auto reg = register_password_user(db, "Adi", "hunter2!");
        if (reg.status != RegisterResult::Status::Ok) return false;

        auto s = link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-link-1", "adi@example.com"), now + 5);
        if (s != LinkStatus::Ok) return false;

        auto r = db.exec("SELECT google_sub FROM players WHERE id=$1",
                         {Param::int64(reg.player_id)});
        return !r.first().is_null(0) && r.first().at(0) == "sup-link-1";
    });

    run_test("link_google is refused when the sub already belongs to another account", [&] {
        reset(db);
        // First, another user takes that Google identity.
        google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-owner", "other@example.com"), now + 5);
        auto reg = register_password_user(db, "Adi", "hunter2!");

        auto s = link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-owner", "other@example.com"), now + 10);
        // Silently overwriting would let a compromised or hijacked Google
        // account be swapped in on someone else's chess account.
        return s == LinkStatus::AlreadyLinkedElsewhere;
    });

    run_test("link_google is refused when THIS account already has a different link", [&] {
        reset(db);
        auto reg = register_password_user(db, "Adi", "hunter2!");
        link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-first", "a@example.com"), now + 5);
        auto s = link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-second", "b@example.com"), now + 10);
        return s == LinkStatus::AlreadyHasGoogle;
    });

    run_test("Re-linking the same sub is idempotent (Ok)", [&] {
        reset(db);
        auto reg = register_password_user(db, "Adi", "hunter2!");
        link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-same", "a@example.com"), now + 5);
        return link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-same", "a@example.com"), now + 10)
            == LinkStatus::Ok;
    });

    run_test("unlink_google works when the account has a password fallback", [&] {
        reset(db);
        auto reg = register_password_user(db, "Adi", "hunter2!");
        link_google(db, verifier, reg.player_id,
            signed_supabase_jwt(now, "sup-lnk", "a@example.com"), now + 5);
        auto s = unlink_google(db, reg.player_id);
        auto r = db.exec("SELECT google_sub FROM players WHERE id=$1",
                         {Param::int64(reg.player_id)});
        return s == UnlinkStatus::Ok && r.first().is_null(0);
    });

    run_test("unlink_google is refused when it would orphan the account", [&] {
        reset(db);
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-only", "sole@example.com"), now + 5);
        if (r.status != GoogleSignInStatus::Ok) return false;
        // No password on this account, so unlinking would leave nothing.
        // The app-layer check catches this before the DB CHECK fires.
        return unlink_google(db, r.player_id) == UnlinkStatus::LastLoginMethod;
    });

    run_test("unlink_google returns NotLinked when nothing is linked", [&] {
        reset(db);
        auto reg = register_password_user(db, "Adi", "hunter2!");
        return unlink_google(db, reg.player_id) == UnlinkStatus::NotLinked;
    });

    run_test("Database CHECK still catches a hand-crafted UPDATE that orphans", [&] {
        reset(db);
        auto r = google_sign_in(db, verifier,
            signed_supabase_jwt(now, "sup-x", "x@example.com"), now + 5);
        auto direct = db.exec(
            "UPDATE players SET google_sub=NULL WHERE id=$1",
            {Param::int64(r.player_id)});
        // If someone bypasses unlink_google and hits the DB directly, the
        // has_a_login_method CHECK is the last line of defence.
        return !direct.ok && direct.sqlstate == pg_errors::CHECK_VIOLATION;
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Google Sign-In via Supabase - Phase 7.8" << std::endl;
    std::cout << "========================================" << std::endl;

    test_verifier();

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping service tests.\n";
    } else {
        Database db;
        std::string err;
        if (!db.connect_from_env(err)) {
            std::cerr << "\nconnect failed: " << err << std::endl;
            return 1;
        }
        test_service(db);
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;
    return (g_failed > 0) ? 1 : 0;
}
