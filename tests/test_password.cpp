/**
 * test_password.cpp — Phase 7.6.
 *
 * Three separately meaningful layers:
 *
 *  1. Argon2id + PHC — round-trip, salting, constant-time verify, upgrade
 *     detection. This is pure crypto; nothing here needs a database.
 *
 *  2. Username validation — the whitelist that pushes XSS/JSON/log injection
 *     out of scope for every downstream consumer of a username.
 *
 *  3. The register+authenticate service against a real Postgres. This is
 *     where the wire-facing behaviour is anchored: username-taken, bad
 *     password, hash upgrade, and — most importantly — the timing property
 *     that a wrong-username lookup takes as long as a wrong-password one.
 *     Without that, an attacker with only wall-clock access can enumerate
 *     accounts.
 *
 * Like test_database, the DB-backed section is skipped cleanly when
 * $DATABASE_URL is unset.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "auth/password.h"
#include "auth/service.h"
#include "auth/username.h"
#include "storage/database.h"

using namespace chess::auth;
using namespace chess::storage;

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
// Username
// ============================================================

static void test_username() {
    std::cout << "\n--- Username validation ---" << std::endl;

    run_test("Accepts letters, digits, underscore, 3..20 chars", [] {
        const char* ok[] = {"abc", "Adi", "A_1", "user_123",
                            "aaaaaaaaaaaaaaaaaaaa" /*20*/, "aaa"};
        for (const char* u : ok) if (!valid_username(u)) return false;
        return true;
    });

    run_test("Rejects short, long, empty", [] {
        return !valid_username("") && !valid_username("ab") &&
               !valid_username(std::string(21, 'a'));
    });

    run_test("Rejects characters that would leak into HTML/JSON/paths/logs", [] {
        // Each of these would need its own escaping rule at some downstream
        // site; forbidding them here means no site needs the rule.
        const char* bad[] = {
            "adi<script",   // HTML
            "adi\"quote",   // JSON
            "adi/slash",    // URL path
            "adi.dot",      // path-ish
            "adi space",
            "adi\ttab",
            "adi\nnewline", // log injection
            "adiキ",         // Unicode / homograph
            "adi-dash",     // not on the whitelist
        };
        for (const char* u : bad) if (valid_username(u)) return false;
        return true;
    });

    run_test("to_lower_ascii matches Postgres lower() for allowed chars", [] {
        return to_lower_ascii("Adi_42") == "adi_42" &&
               to_lower_ascii("ADI")    == "adi";
    });
}

// ============================================================
// Password
// ============================================================

static void test_password_primitive() {
    std::cout << "\n--- Argon2id + PHC ---" << std::endl;

    run_test("hash_password produces a valid PHC string", [] {
        const std::string phc = hash_password("hunter2!");
        return phc.rfind("$argon2id$v=19$m=19456,t=2,p=1$", 0) == 0 &&
               std::count(phc.begin(), phc.end(), '$') == 5;
    });

    run_test("Round trip: verify accepts the right password", [] {
        const std::string phc = hash_password("correct horse battery staple");
        return verify_password("correct horse battery staple", phc);
    });

    run_test("Wrong password is rejected", [] {
        const std::string phc = hash_password("real");
        return !verify_password("REAL", phc) && !verify_password("real ", phc) &&
               !verify_password("", phc);
    });

    run_test("Salting: two hashes of the same password differ", [] {
        // If salting is silently broken, two identical passwords produce
        // identical hashes and a leaked column becomes a rainbow-table oracle.
        const std::string a = hash_password("same-password");
        const std::string b = hash_password("same-password");
        return !a.empty() && a != b &&
               verify_password("same-password", a) &&
               verify_password("same-password", b);
    });

    run_test("PHC does not contain the plaintext password", [] {
        const std::string phc = hash_password("secret-marker-42");
        return phc.find("secret-marker-42") == std::string::npos;
    });

    run_test("Verify tolerates arbitrary bytes in the password", [] {
        // Assembled byte-by-byte: a literal "\xff" adjacent to "data" is
        // parsed as one long hex escape and warns as out-of-range.
        std::string weird;
        weird.append(1, char(0x00));
        weird += "binary";
        weird.append(1, char(0xFF));
        weird += "data";
        const std::string phc = hash_password(weird);
        return verify_password(weird, phc) && !verify_password("binary", phc);
    });

    run_test("Corrupted PHC is rejected without a crash", [] {
        const std::string phc = hash_password("p");
        const char* bad[] = {
            "",
            "$argon2i$v=19$m=19456,t=2,p=1$AAAA$BBBB",       // wrong variant
            "$argon2id$v=19$m=19456,t=2$AAAA$BBBB",          // missing p
            "$argon2id$v=19$m=19456,t=2,p=1$$BBBB",          // empty salt
            "$argon2id$v=19$m=19456,t=2,p=1$AAAA$",          // empty hash
            "$argon2id$v=19$m=99999999999,t=2,p=1$AA$BB",    // absurd m
            "$argon2id$v=18$m=19456,t=2,p=1$AA$BB",          // wrong version
            "not a phc string at all",
        };
        for (const char* p : bad) if (verify_password("p", p)) return false;
        return !verify_password("p", "");
    });

    run_test("needs_upgrade catches weaker parameters", [] {
        // Craft a PHC-shaped string with a weaker m and one with a stronger m
        // than today's default, then confirm the direction of the decision.
        auto craft = [](uint32_t m) {
            return "$argon2id$v=19$m=" + std::to_string(m) +
                   ",t=2,p=1$AAAAAAAAAAAAAAAAAAAAAA$BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
        };
        // A fresh hash uses today's params, so it must NOT need upgrade.
        const std::string fresh = hash_password("p");
        return !needs_upgrade(fresh) &&
               needs_upgrade(craft(argon2::MEMCOST_KIB / 2)) &&
               // A hash-shape string with parameters unchanged but a shorter
               // digest (24 base64 chars ≈ 18 bytes < 32) also needs upgrade.
               needs_upgrade(craft(argon2::MEMCOST_KIB).substr(0, 45) + "A$AAAA");
    });

    run_test("dummy_phc is a valid PHC and does NOT verify a random password", [] {
        const std::string& d = dummy_phc();
        return d.rfind("$argon2id$", 0) == 0 &&
               !verify_password("hunter2", d) &&
               !verify_password("", d);
    });
}

// ============================================================
// Service (needs the DB)
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

/// Median wall time of `n` calls to `f`, in milliseconds. Median rather than
/// mean because a single scheduler blip skews an average; the median absorbs
/// it. Warm-up call excluded.
template <typename F>
static double median_ms(F&& f, int n) {
    f();   // warm-up (Argon2 fetches its provider once)
    std::vector<double> samples;
    samples.reserve(n);
    for (int i = 0; i < n; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

static void test_service(Database& db) {
    std::cout << "\n--- register / authenticate service ---" << std::endl;

    run_test("Register succeeds; login round-trips the same account", [&] {
        reset(db);
        auto r = register_password_user(db, "Adi", "hunter2!");
        if (r.status != RegisterResult::Status::Ok) return false;
        if (r.username != "Adi" || r.elo_rating != 1200) return false;

        auto l = authenticate_password(db, "adi", "hunter2!");
        return l.status == LoginResult::Status::Ok &&
               l.player_id == r.player_id &&
               l.username == "Adi" &&        // original case is what we return
               l.elo_rating == 1200 &&
               !l.hash_needs_upgrade;        // fresh hash, nothing to upgrade
    });

    run_test("Password is never stored as plaintext", [&] {
        reset(db);
        register_password_user(db, "leaky", "SECRET-MARKER-42");
        auto sel = db.exec("SELECT password_hash FROM players WHERE username_ci='leaky'");
        return sel.ok && sel.rows.size() == 1 &&
               sel.first().at(0).find("SECRET-MARKER-42") == std::string::npos;
    });

    run_test("Duplicate username (any case) is rejected as UsernameTaken", [&] {
        reset(db);
        register_password_user(db, "Adi", "hunter2!");
        auto r = register_password_user(db, "ADI", "different!");
        return r.status == RegisterResult::Status::UsernameTaken;
    });

    run_test("Invalid username never reaches the DB", [&] {
        reset(db);
        auto r = register_password_user(db, "bad name", "hunter2!");
        return r.status == RegisterResult::Status::InvalidUsername &&
               db.exec("SELECT count(*) FROM players").first().at(0) == "0";
    });

    run_test("Weak password is refused synchronously", [&] {
        reset(db);
        auto r = register_password_user(db, "Adi", "short");
        return r.status == RegisterResult::Status::WeakPassword;
    });

    run_test("Wrong password: InvalidCredentials, no upgrade fired", [&] {
        reset(db);
        register_password_user(db, "Adi", "hunter2!");
        auto l = authenticate_password(db, "Adi", "wrong");
        return l.status == LoginResult::Status::InvalidCredentials &&
               l.player_id == 0 &&
               !l.hash_needs_upgrade;
    });

    run_test("Unknown username: InvalidCredentials", [&] {
        reset(db);
        auto l = authenticate_password(db, "ghost", "anything!");
        return l.status == LoginResult::Status::InvalidCredentials;
    });

    run_test("A Google-only account cannot be password-logged in", [&] {
        reset(db);
        // Manually insert a Google-only row (no password_hash). This is what
        // §7.8 will produce; §7.6 must not let a password path bypass it.
        auto ins = db.exec(
            "INSERT INTO players(username, username_ci, google_sub)"
            " VALUES('goog','goog','google:12345')");
        if (!ins.ok) return false;
        auto l = authenticate_password(db, "goog", "anything!");
        return l.status == LoginResult::Status::InvalidCredentials;
    });

    run_test("Login updates last_login", [&] {
        reset(db);
        register_password_user(db, "Adi", "hunter2!");
        auto before = db.exec("SELECT last_login FROM players WHERE username_ci='adi'");
        // last_login is NULL immediately after registration.
        if (!before.first().is_null(0)) return false;
        authenticate_password(db, "adi", "hunter2!");
        auto after = db.exec("SELECT last_login FROM players WHERE username_ci='adi'");
        return !after.first().is_null(0);
    });

    run_test("Old hash is transparently upgraded on next login", [&] {
        reset(db);
        // Simulate a row from an older deployment: register, then overwrite
        // the row with a *genuinely* weaker Argon2id hash of the same
        // password. String-replacing the params in place would leave the
        // digest unmatched, so verification would fail — the mistake worth
        // avoiding in a test that is meant to prove upgrade WORKS.
        auto reg = register_password_user(db, "Adi", "hunter2!");
        if (reg.status != RegisterResult::Status::Ok) return false;

        const std::string weak_phc =
            hash_password_with_params("hunter2!", 8192, 1, 1);
        if (weak_phc.empty()) return false;
        db.exec("UPDATE players SET password_hash=$1 WHERE id=$2",
                {Param::text(weak_phc), Param::int64(reg.player_id)});

        auto l = authenticate_password(db, "adi", "hunter2!");
        if (l.status != LoginResult::Status::Ok || !l.hash_needs_upgrade) return false;

        // And the row must now hold a stronger PHC — the login upgraded it.
        auto sel = db.exec("SELECT password_hash FROM players WHERE id=$1",
                           {Param::int64(reg.player_id)});
        return sel.first().at(0).find("m=19456,t=2,p=1") != std::string::npos;
    });

    run_test("Timing: unknown-user and wrong-password branches match within tolerance", [&] {
        reset(db);
        register_password_user(db, "Adi", "hunter2!");

        // The two branches must both run Argon2id exactly once. If the
        // wrong-username branch short-circuited, its median time would be
        // sub-millisecond while the wrong-password branch takes ~30 ms — a
        // ratio a network attacker sees very clearly.
        const double wrong_pw = median_ms([&]{
            authenticate_password(db, "adi", "wrong");
        }, 5);
        const double no_user  = median_ms([&]{
            authenticate_password(db, "ghost", "wrong");
        }, 5);
        const double ratio = (wrong_pw > no_user) ? wrong_pw / no_user
                                                  : no_user / wrong_pw;
        // Tolerance is loose because CI is noisy; the point is to catch a
        // missing dummy hash, which produces a ratio of 100+, not fine
        // measurement of a well-tuned defence.
        std::cout << "\n     wrong_pw=" << wrong_pw << "ms  no_user=" << no_user
                  << "ms  ratio=" << ratio << std::flush;
        return ratio < 3.0;
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Password + username + auth - Phase 7.6" << std::endl;
    std::cout << "========================================" << std::endl;

    test_username();
    test_password_primitive();

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
