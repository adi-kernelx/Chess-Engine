/**
 * test_token.cpp — Phase 7.7.
 *
 * Three layers:
 *
 *   1. JWT itself. Round-trip, and every classical failure mode a JWT library
 *      has ever had a CVE for: alg=none, algorithm confusion, tampered
 *      payload, tampered signature, wrong key, missing segment, expired.
 *
 *   2. Refresh tokens. Fresh values are distinct and their hashes are stable;
 *      the token itself never appears in the stored hash.
 *
 *   3. The DB-backed session layer: rotation, family revocation on reuse,
 *      logout, logout_all + token_epoch, and the whole "authorize this
 *      access token" gate. Skipped cleanly if $DATABASE_URL is unset.
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth/session.h"
#include "auth/token.h"
#include "crypto/base64.h"
#include "storage/database.h"

using namespace chess::auth;
using namespace chess::storage;
using namespace chess::crypto;
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

static AccessClaims sample_claims(int64_t now) {
    AccessClaims c;
    c.player_id   = 42;
    c.username    = "Adi";
    c.token_epoch = 0;
    c.issued_at   = now;
    c.expires_at  = now + 900;
    return c;
}

static std::string forge(const std::string& header_json,
                         const std::string& payload_json,
                         const std::string& sig = "") {
    return encode_base64url(reinterpret_cast<const uint8_t*>(header_json.data()),
                            header_json.size()) + "." +
           encode_base64url(reinterpret_cast<const uint8_t*>(payload_json.data()),
                            payload_json.size()) + "." + sig;
}

// ============================================================
// JWT
// ============================================================

static void test_jwt() {
    std::cout << "\n--- JWT (HS384) ---" << std::endl;

    const TokenSigner signer = TokenSigner::generate_random();
    const int64_t now = 1'700'000'000;

    run_test("Round trip: issue then verify agrees on every claim", [&] {
        AccessClaims in = sample_claims(now);
        const std::string jwt = signer.issue_access(in);
        AccessClaims out;
        return !jwt.empty() &&
               signer.verify_access(jwt, now + 60, out) &&
               out.player_id   == in.player_id &&
               out.username    == in.username &&
               out.token_epoch == in.token_epoch &&
               out.issued_at   == in.issued_at &&
               out.expires_at  == in.expires_at;
    });

    run_test("JWT has exactly three base64url segments", [&] {
        const std::string jwt = signer.issue_access(sample_claims(now));
        return std::count(jwt.begin(), jwt.end(), '.') == 2;
    });

    run_test("Header is exactly {alg:HS384,typ:JWT} — byte-stable", [&] {
        const std::string jwt = signer.issue_access(sample_claims(now));
        const size_t d = jwt.find('.');
        std::vector<uint8_t> header;
        decode_base64url(jwt.substr(0, d), header);
        const std::string hj(header.begin(), header.end());
        return hj == "{\"alg\":\"HS384\",\"typ\":\"JWT\"}";
    });

    run_test("Expired token is rejected", [&] {
        AccessClaims in = sample_claims(now);
        in.expires_at = now + 10;
        const std::string jwt = signer.issue_access(in);
        AccessClaims out;
        return !signer.verify_access(jwt, now + 20, out);
    });

    run_test("Not-yet-issued token is rejected (large clock skew)", [&] {
        // iat 10 minutes in the future: should be refused, not accepted with
        // its own future expiry.
        AccessClaims in = sample_claims(now);
        in.issued_at = now + 600;
        in.expires_at = in.issued_at + 900;
        const std::string jwt = signer.issue_access(in);
        AccessClaims out;
        return !signer.verify_access(jwt, now, out);
    });

    run_test("Tampered payload is rejected", [&] {
        // Rewrite the payload to a higher player_id — a classic forge attempt.
        const std::string jwt = signer.issue_access(sample_claims(now));
        const size_t d1 = jwt.find('.');
        const size_t d2 = jwt.find('.', d1 + 1);
        json evil = {
            {"sub", 1}, {"username", "root"}, {"epoch", 0},
            {"iat", now}, {"exp", now + 900}};
        const std::string evil_b64 = encode_base64url(
            reinterpret_cast<const uint8_t*>(evil.dump().data()), evil.dump().size());
        const std::string forged = jwt.substr(0, d1 + 1) + evil_b64 + jwt.substr(d2);
        AccessClaims out;
        return !signer.verify_access(forged, now + 60, out);
    });

    run_test("Tampered signature is rejected", [&] {
        std::string jwt = signer.issue_access(sample_claims(now));
        jwt[jwt.size() - 1] = (jwt.back() == 'A') ? 'B' : 'A';
        AccessClaims out;
        return !signer.verify_access(jwt, now + 60, out);
    });

    run_test("alg:none is rejected (CVE-2015-9235 shape)", [&] {
        // The classical vulnerability. If accepted, an attacker can create any
        // claim set they like and sign it with nothing.
        const std::string evil = forge(
            "{\"alg\":\"none\",\"typ\":\"JWT\"}",
            R"({"sub":1,"username":"root","epoch":0,"iat":0,"exp":9999999999})",
            /*sig=*/"");
        AccessClaims out;
        return !signer.verify_access(evil, 1, out);
    });

    run_test("Algorithm confusion: HS256 header is rejected", [&] {
        // Even if the attacker had the key, we don't compute HS256 — the
        // verifier refuses anything but the fixed HS384 header.
        const std::string evil = forge("{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
                                       R"({"sub":42})", "AAAAAAAA");
        AccessClaims out;
        return !signer.verify_access(evil, 1, out);
    });

    run_test("Header with extra field is rejected (byte-exact compare)", [&] {
        // {"alg":"HS384","typ":"JWT","kid":"..."} would be legal per JWS but
        // our verifier deliberately requires the exact canonical string, so
        // no clever variation opens a new attack surface.
        const std::string evil = forge(
            "{\"alg\":\"HS384\",\"typ\":\"JWT\",\"kid\":\"1\"}",
            R"({"sub":42,"username":"a","epoch":0,"iat":0,"exp":9999999999})",
            "AAAAAAAAAAAA");
        AccessClaims out;
        return !signer.verify_access(evil, 1, out);
    });

    run_test("Wrong signing key rejects the token", [&] {
        const std::string jwt = signer.issue_access(sample_claims(now));
        AccessClaims out;
        return !TokenSigner::generate_random().verify_access(jwt, now + 60, out);
    });

    run_test("Missing segment is rejected", [&] {
        const std::string jwt = signer.issue_access(sample_claims(now));
        const size_t d = jwt.find('.');
        AccessClaims out;
        return !signer.verify_access(jwt.substr(0, d), now + 60, out) &&
               !signer.verify_access(jwt + ".extra", now + 60, out) &&
               !signer.verify_access("..", now + 60, out);
    });

    run_test("Non-integer exp is rejected", [&] {
        // {"exp":"soon"} must NOT silently become exp=0. get_i64 checks the
        // JSON type explicitly.
        std::string evil = forge(
            "{\"alg\":\"HS384\",\"typ\":\"JWT\"}",
            R"({"sub":1,"username":"a","epoch":0,"iat":0,"exp":"soon"})",
            "AAAAAAAAAAAA");
        AccessClaims out;
        return !signer.verify_access(evil, 1, out);
    });

    run_test("Empty JWT is rejected without crashing", [&] {
        AccessClaims out;
        return !signer.verify_access("", 1, out) &&
               !signer.verify_access("...", 1, out);
    });

    run_test("Invalid signer (no key) refuses to issue or verify", [] {
        TokenSigner none;
        AccessClaims out;
        return !none.valid() &&
               none.issue_access(AccessClaims{}).empty() &&
               !none.verify_access("a.b.c", 0, out);
    });

    run_test("Signing key is exactly 32 bytes; wrong sizes rejected", [] {
        SecureBuffer short_key(16);
        return !TokenSigner::from_key(std::move(short_key)).valid() &&
               TokenSigner::from_key(SecureBuffer(32)).valid();
    });
}

// ============================================================
// Refresh tokens
// ============================================================

static void test_refresh_bytes() {
    std::cout << "\n--- Refresh tokens ---" << std::endl;

    run_test("Fresh refresh tokens are distinct", [] {
        return mint_refresh_token() != mint_refresh_token();
    });

    run_test("SHA-384 hash is stable and hex-encoded", [] {
        const std::string t = mint_refresh_token();
        const std::string h1 = refresh_token_hash(t);
        const std::string h2 = refresh_token_hash(t);
        return h1 == h2 && h1.size() == 96 &&              // 48 bytes hex
               h1.find_first_not_of("0123456789abcdef") == std::string::npos;
    });

    run_test("The hash does NOT contain the token", [] {
        const std::string t = mint_refresh_token();
        return refresh_token_hash(t).find(t) == std::string::npos;
    });

    run_test("UUID v4 form is well-shaped", [] {
        const std::string u = new_uuid_v4();
        return u.size() == 36 &&
               u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-' &&
               u[14] == '4' &&                             // version nibble
               (u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
    });
}

// ============================================================
// Session — DB layer
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

/// Insert a player and return their id.
static int64_t make_player(Database& db, const std::string& username = "adi") {
    auto r = db.exec(
        "INSERT INTO players(username, username_ci, password_hash)"
        " VALUES($1,$2,$3) RETURNING id",
        {Param::text(username), Param::text(username), Param::text("phc")});
    return std::strtoll(r.first().at(0).c_str(), nullptr, 10);
}

static size_t session_count(Database& db) {
    auto r = db.exec("SELECT count(*) FROM sessions");
    return static_cast<size_t>(std::strtoull(r.first().at(0).c_str(), nullptr, 10));
}

static void test_sessions(Database& db) {
    std::cout << "\n--- Sessions (rotation, revocation, logout_all) ---" << std::endl;

    const TokenSigner signer = TokenSigner::generate_random();
    const int64_t now = 1'700'000'000;

    run_test("issue_session inserts one row and returns a valid pair", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto t = issue_session(db, signer, pid, "adi", 0, now);
        AccessClaims out;
        return t.ok && !t.access_token.empty() && !t.refresh_token.empty() &&
               t.access_expires_in == ACCESS_TOKEN_TTL_SECONDS &&
               signer.verify_access(t.access_token, now + 10, out) &&
               out.player_id == pid && out.token_epoch == 0 &&
               session_count(db) == 1;
    });

    run_test("Refresh rotates and the successor can rotate again", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto initial = issue_session(db, signer, pid, "adi", 0, now);

        // The rotation chain works as long as each token is used exactly once.
        // Replaying `initial` here would (correctly) burn the whole family —
        // that path is proved in the next test.
        RefreshOutcome o1;
        auto second = refresh_session(db, signer, initial.refresh_token, now + 30, o1);
        if (o1 != RefreshOutcome::Ok || !second.ok) return false;
        if (second.refresh_token == initial.refresh_token) return false;

        RefreshOutcome o2;
        auto third = refresh_session(db, signer, second.refresh_token, now + 60, o2);
        if (o2 != RefreshOutcome::Ok || !third.ok) return false;
        if (third.refresh_token == second.refresh_token) return false;

        // Three rows exist: two rotated tripwires plus the live successor.
        // The rotated rows are deliberately KEPT — that is how reuse detection
        // works. They are removed when the family is destroyed or the whole
        // player logs out.
        return session_count(db) == 3;
    });

    run_test("Reuse of a rotated token kills the WHOLE family", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto a = issue_session(db, signer, pid, "adi", 0, now);
        RefreshOutcome o;
        auto b = refresh_session(db, signer, a.refresh_token, now + 30, o);
        auto c = refresh_session(db, signer, b.refresh_token, now + 60, o);

        // At this point there are two live tokens: `c` (freshest) and,
        // conceptually, whichever ones we chose. Now REPLAY the old rotated
        // token `a`. That must destroy the whole family, including `c`.
        RefreshOutcome replay_out;
        (void)refresh_session(db, signer, a.refresh_token, now + 100, replay_out);
        if (replay_out != RefreshOutcome::InvalidOrRevoked) return false;

        // And `c` must no longer work either — the family is gone.
        RefreshOutcome after;
        (void)refresh_session(db, signer, c.refresh_token, now + 110, after);
        return after == RefreshOutcome::InvalidOrRevoked &&
               session_count(db) == 0;
    });

    run_test("Unknown refresh token = InvalidOrRevoked, no crash", [&] {
        reset(db);
        make_player(db);
        RefreshOutcome o;
        (void)refresh_session(db, signer, "not-a-real-token", now, o);
        return o == RefreshOutcome::InvalidOrRevoked;
    });

    run_test("Expired refresh token is rejected and swept", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto t = issue_session(db, signer, pid, "adi", 0, now);
        RefreshOutcome o;
        (void)refresh_session(db, signer, t.refresh_token,
                              now + REFRESH_TOKEN_TTL_SECONDS + 1, o);
        // The row for this token must be gone — an expired but stored token
        // still counts against per-user limits later.
        auto n = db.exec("SELECT count(*) FROM sessions WHERE player_id=$1",
                         {Param::int64(pid)});
        return o == RefreshOutcome::InvalidOrRevoked && n.first().at(0) == "0";
    });

    run_test("logout deletes the family for this token only", [&] {
        reset(db);
        const int64_t pid  = make_player(db, "adi");
        const int64_t pid2 = make_player(db, "bob");
        auto adi_a = issue_session(db, signer, pid,  "adi", 0, now);
        auto adi_b = issue_session(db, signer, pid,  "adi", 0, now);   // 2nd device
        auto bob   = issue_session(db, signer, pid2, "bob", 0, now);

        logout(db, adi_a.refresh_token);

        // adi's second device still works; adi's first device is gone;
        // bob is untouched.
        RefreshOutcome oa, ob, oc;
        (void)refresh_session(db, signer, adi_a.refresh_token, now + 10, oa);
        (void)refresh_session(db, signer, adi_b.refresh_token, now + 10, ob);
        (void)refresh_session(db, signer, bob.refresh_token,   now + 10, oc);
        return oa == RefreshOutcome::InvalidOrRevoked &&
               ob == RefreshOutcome::Ok &&
               oc == RefreshOutcome::Ok;
    });

    run_test("logout with unknown token is a no-op", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        issue_session(db, signer, pid, "adi", 0, now);
        logout(db, "junk");
        return session_count(db) == 1;
    });

    run_test("logout_all: all sessions deleted, token_epoch bumped", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        issue_session(db, signer, pid, "adi", 0, now);
        issue_session(db, signer, pid, "adi", 0, now);   // second device
        if (session_count(db) != 2) return false;

        if (!logout_all(db, pid)) return false;
        return session_count(db) == 0 &&
               current_token_epoch(db, pid) == 1;
    });

    run_test("Access token from before logout_all is rejected by the gate", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto t = issue_session(db, signer, pid, "adi", 0, now);
        AccessClaims c1;
        if (authorize_access_token(db, signer, t.access_token, now + 10, c1)
            != GateOutcome::Ok) return false;

        // Sign out everywhere. The same JWT is still in its exp window and
        // still verifies structurally — the epoch check is what invalidates it.
        logout_all(db, pid);

        AccessClaims c2;
        auto verdict = authorize_access_token(db, signer, t.access_token,
                                              now + 20, c2);
        return verdict == GateOutcome::RejectedRevoked;
    });

    run_test("Gate rejects a token whose player was deleted", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto t = issue_session(db, signer, pid, "adi", 0, now);
        db.exec("DELETE FROM players WHERE id=$1", {Param::int64(pid)});
        AccessClaims c;
        return authorize_access_token(db, signer, t.access_token, now + 10, c)
               == GateOutcome::RejectedUnknownUser;
    });

    run_test("Refresh after logout_all fails cleanly (family already gone)", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto t = issue_session(db, signer, pid, "adi", 0, now);
        logout_all(db, pid);
        RefreshOutcome o;
        (void)refresh_session(db, signer, t.refresh_token, now + 5, o);
        return o == RefreshOutcome::InvalidOrRevoked;
    });

    run_test("Rotated access token embeds the CURRENT token_epoch", [&] {
        reset(db);
        const int64_t pid = make_player(db);
        auto t = issue_session(db, signer, pid, "adi", 0, now);

        // Bump the epoch out-of-band — the DB is the truth.
        db.exec("UPDATE players SET token_epoch=7 WHERE id=$1", {Param::int64(pid)});

        RefreshOutcome o;
        auto refreshed = refresh_session(db, signer, t.refresh_token, now + 10, o);
        if (o != RefreshOutcome::Ok) return false;

        AccessClaims c;
        return signer.verify_access(refreshed.access_token, now + 20, c) &&
               c.token_epoch == 7;
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Token + session - Phase 7.7" << std::endl;
    std::cout << "========================================" << std::endl;

    test_jwt();
    test_refresh_bytes();

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping session tests.\n";
    } else {
        Database db;
        std::string err;
        if (!db.connect_from_env(err)) {
            std::cerr << "\nconnect failed: " << err << std::endl;
            return 1;
        }
        test_sessions(db);
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;
    return (g_failed > 0) ? 1 : 0;
}
