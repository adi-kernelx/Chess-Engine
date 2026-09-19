#include "storage/transaction.h"

#include "storage/database.h"

namespace chess {
namespace storage {

Transaction::Transaction(Database& db) : db_(db) {
    auto r = db_.exec("BEGIN");
    if (r.ok) {
        state_ = State::Open;
    } else {
        state_ = State::Failed;
        error_ = r.error;
    }
}

Transaction::~Transaction() {
    if (state_ == State::Open) {
        // Best-effort ROLLBACK from a destructor. The original write
        // failure is what the caller cares about; a failure to roll
        // back is a connection-level problem the next exec will surface
        // on its own.
        (void)db_.exec("ROLLBACK");
    }
}

bool Transaction::commit() {
    switch (state_) {
        case State::Failed:     return false;
        case State::Committed:  return true;   // idempotent
        case State::RolledBack: return false;
        case State::Open:       break;
    }

    auto r = db_.exec("COMMIT");
    // Whether COMMIT succeeded or failed, libpq closed the transaction.
    // Mark it Committed so the destructor does not re-issue ROLLBACK
    // against a connection that no longer has one to roll back.
    state_ = State::Committed;
    return r.ok;
}

bool Transaction::rollback() {
    switch (state_) {
        case State::Failed:     return false;
        case State::Committed:  return false;
        case State::RolledBack: return true;   // idempotent
        case State::Open:       break;
    }

    auto r = db_.exec("ROLLBACK");
    state_ = State::RolledBack;
    return r.ok;
}

} // namespace storage
} // namespace chess
