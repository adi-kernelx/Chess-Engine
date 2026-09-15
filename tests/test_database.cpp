/**
 * test_database.cpp — Phase 7.5 verification for the libpq wrapper.
 *
 * A test that only connected to Supabase would prove nothing on this laptop
 * and would refuse to run in CI without credentials, so this file talks to
 * whatever Postgres URL is in `$DATABASE_URL`. The regression driver
 * (regress.sh) spins up an ephemeral local cluster and points that variable
 * at it; the same test binary runs unchanged against Supabase later.
 *
 * If `$DATABASE_URL` is unset, the test prints why and exits 0. That is
 * deliberate: a "no database" failure would gate every other suite behind an
 * environment we do not need to build the C++ code.
 *
 * WHAT THE TESTS ARE ANCHORED TO
 *
 *  1. The schema loads clean. If it does not, nothing else means anything.
 *  2. Real CRUD round-trip through `players` — every column comes back.
 *  3. UNIQUE constraint violations arrive as SQLSTATE 23505, not a text-parse.
 *  4. `has_a_login_method` CHECK arrives as 23514, so a Google-only account
 *     that later has its google_sub cleared is refused at the database, not
 *     accidentally allowed by an application-layer forget.
 *  5. Case-insensitive uniqueness: "Adi" collides with "adi".
 *  6. SQL injection: a username of `'; DROP TABLE players;--` becomes a
 *     literal row, and `players` is still there afterwards. This is the
 *     single test that justifies the whole "prepared statements only" rule.
 *  7. NULL round-trips (google_sub for a password-only account).
 *  8. The FOREIGN KEY on sessions cascades on player delete.
 */

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "storage/database.h"

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

/// One Database per test — cheap enough and keeps failures localised.
static Database open_db() {
    Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "\nconnect failed: " << err << std::endl;
    }
    return db;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

// ============================================================
// Tests
// ============================================================

static const char* SCHEMA_ENV      = "SCHEMA_PATH";
static const char* DEFAULT_SCHEMA  = "src/storage/schema_phase7.sql";

/// Reset to a known state before every test that touches data.
static void reset(Database& db) {
    std::string err;
    db.run_script("DROP TABLE IF EXISTS sessions; "
                  "DROP TABLE IF EXISTS players; "
                  "DROP FUNCTION IF EXISTS assert_username_ci_matches();", err);

    const char* path = std::getenv(SCHEMA_ENV);
    const std::string schema = read_file(path ? path : DEFAULT_SCHEMA);
    if (!db.run_script(schema, err)) {
        std::cerr << "\nschema load failed: " << err << std::endl;
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Database (libpq wrapper) - Phase 7.5" << std::endl;
    std::cout << "========================================" << std::endl;

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping.\n"
                  << "  Set it to a `postgres://…` URL to run this suite\n"
                  << "  (regress.sh spins up an ephemeral local cluster).\n";
        return 0;
    }

    Database db = open_db();
    if (!db.connected()) return 1;

    run_test("Schema loads cleanly", [&] {
        reset(db);
        auto r = db.exec("SELECT to_regclass('players'), to_regclass('sessions')");
        return r.ok && r.rows.size() == 1 &&
               r.first().at(0) == "players" && r.first().at(1) == "sessions";
    });

    run_test("INSERT round-trip: every column survives the wire", [&] {
        reset(db);
        auto ins = db.exec(
            "INSERT INTO players(username, username_ci, email, password_hash)"
            " VALUES($1,$2,$3,$4) RETURNING id, elo_rating, token_epoch",
            {Param::text("Adi"), Param::text("adi"),
             Param::text("adi@example.com"), Param::text("hash")});
        if (!ins.ok || ins.rows.size() != 1) return false;
        // The DEFAULTS defined in the schema must actually apply.
        if (ins.first().at(1) != "1200" || ins.first().at(2) != "0") return false;

        auto sel = db.exec("SELECT username, email, password_hash, google_sub"
                           "  FROM players WHERE username_ci=$1",
                           {Param::text("adi")});
        return sel.ok && sel.rows.size() == 1 &&
               sel.first().at(0) == "Adi" &&
               sel.first().at(1) == "adi@example.com" &&
               sel.first().at(2) == "hash" &&
               sel.first().is_null(3);           // google_sub round-trips as NULL
    });

    run_test("Duplicate username_ci fails with SQLSTATE 23505", [&] {
        reset(db);
        db.exec("INSERT INTO players(username, username_ci, password_hash)"
                " VALUES($1,$2,$3)",
                {Param::text("Adi"), Param::text("adi"), Param::text("h")});
        auto again = db.exec("INSERT INTO players(username, username_ci, password_hash)"
                             " VALUES($1,$2,$3)",
                             {Param::text("ADI"), Param::text("adi"), Param::text("h")});
        // The application distinguishes "already registered" from
        // "database down" by SQLSTATE, not by parsing the error message.
        return !again.ok && again.sqlstate == pg_errors::UNIQUE_VIOLATION;
    });

    run_test("has_a_login_method CHECK rejects an account with neither auth", [&] {
        reset(db);
        auto r = db.exec("INSERT INTO players(username, username_ci) VALUES($1,$2)",
                         {Param::text("Ghost"), Param::text("ghost")});
        return !r.ok && r.sqlstate == pg_errors::CHECK_VIOLATION;
    });

    run_test("Case-insensitive username: mismatched casing is rejected by the trigger", [&] {
        reset(db);
        auto r = db.exec("INSERT INTO players(username, username_ci, password_hash)"
                         " VALUES($1,$2,$3)",
                         {Param::text("Adi"), Param::text("Adi"), Param::text("h")});
        // The trigger keeps the two columns aligned even if the caller forgets
        // to lowercase; no client bug can produce a mixed-case username_ci.
        return !r.ok && !r.error.empty();
    });

    run_test("Injection payload is stored as literal text", [&] {
        reset(db);
        // The classic. Under any string-concatenation code path this would
        // close the pending string literal, run `DROP TABLE players`, and
        // comment out the rest — no rows, no players table left. Under libpq
        // parameter binding, both fields are stored as inert text and the
        // players table is still standing after.
        const std::string payload    = "'; DROP TABLE players;--";
        const std::string payload_ci = "'; drop table players;--";  // lower(payload)
        auto ins = db.exec("INSERT INTO players(username, username_ci, password_hash)"
                           " VALUES($1,$2,$3)",
                           {Param::text(payload), Param::text(payload_ci),
                            Param::text("h")});
        if (!ins.ok) return false;

        auto sel = db.exec("SELECT username FROM players WHERE username_ci=$1",
                           {Param::text(payload_ci)});
        auto still = db.exec("SELECT to_regclass('players')");
        return sel.ok && sel.rows.size() == 1 && sel.first().at(0) == payload &&
               still.ok && still.first().at(0) == "players";
    });

    run_test("google_sub UNIQUE lets multiple accounts have NULL", [&] {
        reset(db);
        // UNIQUE + NULL == "as many nulls as we like". Password-only accounts
        // rely on that; a stricter constraint would forbid them.
        db.exec("INSERT INTO players(username, username_ci, password_hash)"
                " VALUES($1,$2,$3)",
                {Param::text("a"), Param::text("a"), Param::text("h")});
        auto b = db.exec("INSERT INTO players(username, username_ci, password_hash)"
                         " VALUES($1,$2,$3)",
                         {Param::text("b"), Param::text("b"), Param::text("h")});
        return b.ok;
    });

    run_test("Duplicate google_sub does fail with 23505", [&] {
        reset(db);
        db.exec("INSERT INTO players(username, username_ci, google_sub) VALUES($1,$2,$3)",
                {Param::text("a"), Param::text("a"), Param::text("google:sub-1")});
        auto dup = db.exec(
            "INSERT INTO players(username, username_ci, google_sub) VALUES($1,$2,$3)",
            {Param::text("b"), Param::text("b"), Param::text("google:sub-1")});
        return !dup.ok && dup.sqlstate == pg_errors::UNIQUE_VIOLATION;
    });

    run_test("UPDATE reports rows_affected and returns the new value", [&] {
        reset(db);
        db.exec("INSERT INTO players(username, username_ci, password_hash) VALUES($1,$2,$3)",
                {Param::text("a"), Param::text("a"), Param::text("h")});
        auto up = db.exec("UPDATE players SET elo_rating=$1 WHERE username_ci=$2",
                          {Param::int64(1500), Param::text("a")});
        if (!up.ok || up.rows_affected != 1) return false;
        auto sel = db.exec("SELECT elo_rating FROM players WHERE username_ci=$1",
                           {Param::text("a")});
        return sel.ok && sel.first().at(0) == "1500";
    });

    run_test("Cascading delete: sessions vanish with their player", [&] {
        reset(db);
        auto p = db.exec("INSERT INTO players(username, username_ci, password_hash)"
                         " VALUES($1,$2,$3) RETURNING id",
                         {Param::text("a"), Param::text("a"), Param::text("h")});
        const std::string pid = p.first().at(0);
        db.exec("INSERT INTO sessions(token_hash, player_id, family_id, expires_at)"
                " VALUES($1,$2::bigint,$3::uuid,$4::timestamptz)",
                {Param::text("hash1"), Param::text(pid),
                 Param::text("11111111-1111-1111-1111-111111111111"),
                 Param::text("2099-01-01T00:00:00Z")});
        // ON DELETE CASCADE means logout_all-style user removal cannot leave
        // orphaned sessions that would still accept the old refresh token.
        db.exec("DELETE FROM players WHERE id=$1::bigint", {Param::text(pid)});
        auto left = db.exec("SELECT count(*) FROM sessions");
        return left.ok && left.first().at(0) == "0";
    });

    run_test("Parameter count mismatch fails, doesn't crash", [&] {
        reset(db);
        auto r = db.exec("SELECT $1::text, $2::text", {Param::text("only-one")});
        return !r.ok && !r.error.empty();
    });

    run_test("NULL parameter travels as SQL NULL, not as the string \"NULL\"", [&] {
        reset(db);
        db.exec("INSERT INTO players(username, username_ci, password_hash, email)"
                " VALUES($1,$2,$3,$4)",
                {Param::text("a"), Param::text("a"),
                 Param::text("h"), Param::null()});
        auto r = db.exec("SELECT email IS NULL FROM players WHERE username_ci=$1",
                         {Param::text("a")});
        return r.ok && r.first().at(0) == "t";
    });

    run_test("A bad query does not poison the connection", [&] {
        reset(db);
        auto bad = db.exec("SELECT * FROM no_such_table");
        if (bad.ok) return false;
        // libpq will refuse further queries until PQreset() if the connection
        // has entered an error state. The wrapper must leave it usable.
        auto good = db.exec("SELECT 1");
        return good.ok && good.first().at(0) == "1";
    });

    run_test("connect() with a bad URL fails cleanly", [] {
        Database d;
        std::string err;
        return !d.connect("postgres://nobody:nope@127.0.0.1:1/nope", err) &&
               !d.connected() && !err.empty();
    });

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;
    return (g_failed > 0) ? 1 : 0;
}
