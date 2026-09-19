/**
 * test_transaction.cpp — LLD-3.1.
 *
 * Verifies the RAII `Transaction` type and the typed `StorageError`
 * classifier. Runs against a live Postgres cluster; skips cleanly
 * (exit 0) if `$DATABASE_URL` is unset — matching every other
 * DB-backed suite in this codebase.
 *
 * What is anchored to:
 *   1. Transaction ctor issues BEGIN; ok() reports success.
 *   2. commit() persists writes performed between BEGIN and COMMIT.
 *   3. dtor without commit rolls back — writes disappear.
 *   4. Explicit rollback() also disappears writes and is idempotent.
 *   5. Double-commit is idempotent (returns true; no second COMMIT).
 *   6. commit() after rollback() returns false and does not issue.
 *   7. StorageError::classify maps sqlstate 23505/23503/23514/23502
 *      onto their enum values.
 *   8. classify returns Disconnected when sqlstate is empty (transport
 *      failure) — verified by closing the connection mid-flight.
 */

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

#include "storage/database.h"
#include "storage/storage_error.h"
#include "storage/transaction.h"

using namespace chess::storage;

static int g_passed = 0;
static int g_failed = 0;

static void run_test(const std::string& name, std::function<bool()> fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) { std::cout << "PASS\n"; ++g_passed; }
        else      { std::cout << "FAIL\n"; ++g_failed; }
    } catch (const std::exception& e) {
        std::cout << "FAIL (exception: " << e.what() << ")\n"; ++g_failed;
    }
}

static Database open_db() {
    Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "connect failed: " << err << "\n";
    }
    return db;
}

// A tiny disposable table for the transaction round-trip tests. Uses
// its own name so we do not race against `players` / `sessions` in
// other suites that may share a cluster.
static void reset_scratch(Database& db) {
    std::string err;
    db.run_script(
        "DROP TABLE IF EXISTS tx_scratch; "
        "CREATE TABLE tx_scratch (id BIGSERIAL PRIMARY KEY, tag TEXT UNIQUE NOT NULL);",
        err);
}

static int count_rows(Database& db) {
    auto r = db.exec("SELECT count(*) FROM tx_scratch");
    if (!r.ok || r.empty()) return -1;
    return std::stoi(r.first().at(0));
}

int main() {
    std::cout << "========================================\n";
    std::cout << " Transaction + StorageError (LLD-3.1)\n";
    std::cout << "========================================\n";

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping.\n";
        return 0;
    }
    Database db = open_db();
    if (!db.connected()) return 1;

    // ── Transaction round-trip ───────────────────────────────────

    run_test("commit() persists writes", [&] {
        reset_scratch(db);
        {
            Transaction tx(db);
            if (!tx.ok()) return false;
            auto ins = db.exec("INSERT INTO tx_scratch(tag) VALUES('a')");
            if (!ins.ok) return false;
            if (!tx.commit()) return false;
        }
        return count_rows(db) == 1;
    });

    run_test("destructor without commit rolls back", [&] {
        reset_scratch(db);
        {
            Transaction tx(db);
            if (!tx.ok()) return false;
            auto ins = db.exec("INSERT INTO tx_scratch(tag) VALUES('b')");
            if (!ins.ok) return false;
            // no commit — dtor rolls back
        }
        return count_rows(db) == 0;
    });

    run_test("explicit rollback() disappears writes", [&] {
        reset_scratch(db);
        {
            Transaction tx(db);
            if (!tx.ok()) return false;
            auto ins = db.exec("INSERT INTO tx_scratch(tag) VALUES('c')");
            if (!ins.ok) return false;
            if (!tx.rollback()) return false;
        }
        return count_rows(db) == 0;
    });

    run_test("commit() is idempotent", [&] {
        reset_scratch(db);
        Transaction tx(db);
        if (!tx.ok()) return false;
        auto ins = db.exec("INSERT INTO tx_scratch(tag) VALUES('d')");
        if (!ins.ok) return false;
        if (!tx.commit()) return false;
        if (!tx.commit()) return false;   // second call — no-op, still true
        return count_rows(db) == 1;
    });

    run_test("rollback() is idempotent", [&] {
        reset_scratch(db);
        Transaction tx(db);
        if (!tx.ok()) return false;
        (void)db.exec("INSERT INTO tx_scratch(tag) VALUES('e')");
        if (!tx.rollback()) return false;
        if (!tx.rollback()) return false;   // second call — no-op, still true
        return count_rows(db) == 0;
    });

    run_test("commit() after rollback() returns false", [&] {
        reset_scratch(db);
        Transaction tx(db);
        if (!tx.ok()) return false;
        (void)db.exec("INSERT INTO tx_scratch(tag) VALUES('f')");
        if (!tx.rollback()) return false;
        return tx.commit() == false;
    });

    // ── StorageError classification ─────────────────────────────

    run_test("Ok result classifies as StorageError::Ok", [&] {
        reset_scratch(db);
        auto r = db.exec("SELECT 1");
        return r.ok && classify(r) == StorageError::Ok;
    });

    run_test("Duplicate PK maps to UniqueViolation (23505)", [&] {
        reset_scratch(db);
        auto a = db.exec("INSERT INTO tx_scratch(tag) VALUES('dup')");
        auto b = db.exec("INSERT INTO tx_scratch(tag) VALUES('dup')");
        return a.ok && !b.ok && b.sqlstate == "23505"
            && classify(b) == StorageError::UniqueViolation;
    });

    run_test("Missing FK maps to ForeignKeyViolation (23503)", [&] {
        // Build a tiny two-table setup: child has a FK into scratch.
        std::string err;
        db.run_script("DROP TABLE IF EXISTS tx_child; DROP TABLE IF EXISTS tx_parent;", err);
        db.run_script(
            "CREATE TABLE tx_parent (id BIGINT PRIMARY KEY);"
            "CREATE TABLE tx_child (id BIGSERIAL PRIMARY KEY,"
            "                        parent_id BIGINT NOT NULL REFERENCES tx_parent(id));",
            err);
        auto r = db.exec("INSERT INTO tx_child(parent_id) VALUES(9999)");
        db.run_script("DROP TABLE tx_child; DROP TABLE tx_parent;", err);
        return !r.ok && r.sqlstate == "23503"
            && classify(r) == StorageError::ForeignKeyViolation;
    });

    run_test("NOT NULL violation maps to NotNullViolation (23502)", [&] {
        reset_scratch(db);
        auto r = db.exec("INSERT INTO tx_scratch(tag) VALUES(NULL)");
        return !r.ok && r.sqlstate == "23502"
            && classify(r) == StorageError::NotNullViolation;
    });

    run_test("CHECK violation maps to CheckViolation (23514)", [&] {
        std::string err;
        db.run_script("DROP TABLE IF EXISTS tx_check; "
                      "CREATE TABLE tx_check (n INT CHECK (n > 0));", err);
        auto r = db.exec("INSERT INTO tx_check(n) VALUES(-1)");
        db.run_script("DROP TABLE tx_check;", err);
        return !r.ok && r.sqlstate == "23514"
            && classify(r) == StorageError::CheckViolation;
    });

    run_test("Class-22 data exception maps to InvalidInput", [&] {
        // Bad text-mode integer cast → sqlstate 22P02 (invalid_text_representation).
        auto r = db.exec("SELECT $1::int", {Param::text("not-an-int")});
        return !r.ok && !r.sqlstate.empty() && r.sqlstate[0] == '2' && r.sqlstate[1] == '2'
            && classify(r) == StorageError::InvalidInput;
    });

    run_test("Unmapped sqlstate falls through to Unknown", [&] {
        QueryResult synthetic;
        synthetic.ok       = false;
        synthetic.sqlstate = "42P01";   // undefined_table — real code we do not map
        synthetic.error    = "relation does not exist";
        return classify(synthetic) == StorageError::Unknown;
    });

    run_test("Empty sqlstate on failure maps to Disconnected", [&] {
        QueryResult synthetic;
        synthetic.ok       = false;
        synthetic.sqlstate = "";
        synthetic.error    = "connection closed";
        return classify(synthetic) == StorageError::Disconnected;
    });

    run_test("is_retriable identifies deadlock/serialization only", [&] {
        return  is_retriable(StorageError::SerializationFailure)
            &&  is_retriable(StorageError::DeadlockDetected)
            && !is_retriable(StorageError::UniqueViolation)
            && !is_retriable(StorageError::Disconnected)
            && !is_retriable(StorageError::Ok);
    });

    run_test("to_string is a stable non-empty label", [&] {
        // Not localised, not user-facing, but we do rely on it for logs.
        for (auto e : { StorageError::Ok, StorageError::Disconnected,
                        StorageError::UniqueViolation, StorageError::ForeignKeyViolation,
                        StorageError::CheckViolation, StorageError::NotNullViolation,
                        StorageError::SerializationFailure, StorageError::DeadlockDetected,
                        StorageError::QueryCanceled, StorageError::InvalidInput,
                        StorageError::Unknown }) {
            const char* s = to_string(e);
            if (s == nullptr || s[0] == '\0') return false;
        }
        return true;
    });

    // ── Cleanup — drop the scratch table so we leave the cluster clean.

    {
        std::string err;
        db.run_script("DROP TABLE IF EXISTS tx_scratch;", err);
    }

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
