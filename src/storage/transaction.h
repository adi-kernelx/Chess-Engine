/**
 * storage/transaction.h — RAII BEGIN / COMMIT / ROLLBACK (LLD-3.1).
 *
 * WHY THIS EXISTS
 *
 * Every existing multi-statement write in this codebase implements the same
 * five-branch cleanup by hand:
 *
 *     auto begin = db.exec("BEGIN");                     // (A) fail
 *     auto step1 = db.exec(...);
 *     if (!step1.ok) { db.exec("ROLLBACK"); return ...; } // (B) fail
 *     auto step2 = db.exec(...);
 *     if (!step2.ok) { db.exec("ROLLBACK"); return ...; } // (C) fail
 *     ...
 *     auto commit = db.exec("COMMIT");
 *     if (!commit.ok) { db.exec("ROLLBACK"); return ...; } // (D) fail
 *
 * `game_repo::save_game` has SIX of those (B)/(C) branches, one per step; any
 * new step needs a matching ROLLBACK. Miss one and the connection is stranded
 * inside an aborted transaction, silently returning `current transaction is
 * aborted, commands ignored until end of transaction block` to every later
 * caller on the same connection.
 *
 * `Transaction` is stack-only RAII: constructor runs BEGIN, destructor runs
 * ROLLBACK if the caller did not call `commit()`. The five-branch pattern
 * collapses to:
 *
 *     Transaction tx(db);
 *     if (!tx.ok()) return ...;
 *     auto step1 = db.exec(...); if (!step1.ok) return ...;   // dtor rolls back
 *     auto step2 = db.exec(...); if (!step2.ok) return ...;
 *     ...
 *     if (!tx.commit()) return ...;                            // COMMIT explicit
 *
 * Not adopted by any call site in this slice — LLD-3.2 / 3.3 migrate them.
 *
 * NOT COVERED HERE
 *
 * - Nested transactions. Postgres does not have real nested transactions —
 *   what you get is SAVEPOINTs, which are their own thing. This class rejects
 *   a second BEGIN on the same `Database` while the first is still open. If
 *   nested savepoint support is ever needed, add it as a separate `Savepoint`
 *   class, don't overload this one.
 *
 * - Isolation level. Every transaction here uses whatever level the Postgres
 *   default happens to be (READ COMMITTED). Callers that need SERIALIZABLE
 *   should issue `SET TRANSACTION ISOLATION LEVEL SERIALIZABLE` themselves
 *   right after `Transaction` construction — or we add a ctor arg. Not needed
 *   for anything in the codebase today.
 *
 * - Auto-retry on `SerializationFailure`. `storage_error::is_retriable`
 *   identifies retriable failures; retry policy is a caller decision because
 *   idempotency depends on what the transaction contained.
 */

#pragma once

#include <string>

#include "storage/database.h"

namespace chess {
namespace storage {

class Transaction {
public:
    /// BEGIN on construction. Check `ok()` before using the transaction —
    /// a failed BEGIN leaves `ok() == false` and `error()` non-empty, and
    /// `commit()` / manual `rollback()` are no-ops afterwards. The
    /// destructor is safe to run on a failed transaction (nothing to
    /// undo).
    explicit Transaction(Database& db);

    /// ROLLBACK if `commit()` was not called and the transaction is
    /// still open. Best-effort: any error from the ROLLBACK exec is
    /// swallowed, because it is running from a destructor and the
    /// user-visible failure is whatever tripped the write path.
    ~Transaction();

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&)                 = delete;
    Transaction& operator=(Transaction&&)      = delete;

    /// True iff BEGIN succeeded and no explicit commit/rollback has run.
    bool ok() const { return state_ == State::Open; }

    /// Human-readable reason the transaction is not `ok()`. Set only
    /// when BEGIN itself failed. Empty otherwise (an open transaction
    /// has no failure to report; a committed / rolled-back one carries
    /// no residual error either).
    const std::string& error() const { return error_; }

    /// COMMIT the transaction. Returns true on success. On failure the
    /// transaction is marked done (a subsequent destructor does not
    /// re-issue ROLLBACK — libpq already dropped it). Idempotent: a
    /// second call after success returns true and does no work.
    /// Calling on a `!ok()` transaction returns false with no exec.
    bool commit();

    /// Explicit ROLLBACK. Same semantics as letting the destructor run,
    /// but reports the ROLLBACK's own failure to the caller. Idempotent
    /// after the first call. No-op on a `!ok()` transaction.
    bool rollback();

private:
    enum class State {
        Failed,     ///< BEGIN failed; nothing to commit / rollback.
        Open,       ///< BEGIN succeeded; commit / rollback still to run.
        Committed,  ///< COMMIT ran (success or failure — either way libpq closed it).
        RolledBack, ///< ROLLBACK ran (explicit or from the destructor).
    };

    Database&                db_;
    Database::OperationGuard operation_guard_;
    State                    state_ = State::Failed;
    std::string              error_;
};

} // namespace storage
} // namespace chess
