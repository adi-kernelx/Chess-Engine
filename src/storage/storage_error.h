/**
 * storage/storage_error.h — typed classification of Postgres failures (LLD-3.1).
 *
 * WHY THIS EXISTS
 *
 * The `QueryResult` struct returned by `Database::exec` carries a `bool ok` +
 * `std::string error` + `std::string sqlstate`. Every caller that wants to
 * distinguish "unique violation" from "connection lost" from "no rows" has
 * been either:
 *
 *   a) grepping `sqlstate == "23505"` inline (auth/service.cpp does this),
 *   b) parsing the human-readable `error` string, or
 *   c) not distinguishing at all — surfacing every failure as a generic
 *      "internal error" and forcing callers to look at the log.
 *
 * All three are fragile. Option (a) leaks the pg wire code into every layer;
 * (b) breaks when libpq updates its message text; (c) hides real distinctions
 * behind a single opaque error.
 *
 * `StorageError` gives callers a small enum they can `switch()` on, computed
 * once from the `QueryResult` via `classify()`. The mapping table lives here
 * and is the single source of truth. Adding a new class of failure means
 * adding one enum value and one row to the map.
 *
 * WHY NOT AN EXCEPTION HIERARCHY
 *
 * Every DB call in this codebase already returns a `QueryResult` (or a
 * repo-level struct that carries `ok + error`). Throwing would force every
 * caller to add a try/catch, which is a bigger churn than the value proposition
 * of typed errors. Returning `StorageError` beside the existing `ok` bool keeps
 * the caller shape the same.
 *
 * NOT COVERED HERE
 *
 * This enum only classifies failures the ADAPTER can observe from libpq. It is
 * NOT the application-layer `Result<T>::code` (Unauthorized / NotYourTurn /
 * IllegalMove — those live in `application/result.h`). Ports over this layer
 * (LLD-3.2 GameStore, LLD-3.3 PlayerQueries) translate `StorageError` into
 * their own domain-shaped failures at their boundary; nothing above `storage/`
 * should ever see a Postgres SQLSTATE.
 */

#pragma once

#include <string>

namespace chess {
namespace storage {

struct QueryResult;  // forward — full definition in database.h

/// Coarse classification of a failed `QueryResult`. The `Ok` value is
/// present for the natural `classify(r) == StorageError::Ok` shape;
/// callers still consult `r.ok` for the boolean, and use this enum only
/// when they need to branch on the failure class.
enum class StorageError {
    Ok,                    ///< r.ok == true
    Disconnected,          ///< underlying PGconn is not CONNECTION_OK
    UniqueViolation,       ///< sqlstate 23505 — duplicate key
    ForeignKeyViolation,   ///< sqlstate 23503 — parent row missing
    CheckViolation,        ///< sqlstate 23514 — CHECK constraint failed
    NotNullViolation,      ///< sqlstate 23502 — NULL in NOT NULL column
    SerializationFailure,  ///< sqlstate 40001 — retry candidate
    DeadlockDetected,      ///< sqlstate 40P01 — retry candidate
    QueryCanceled,         ///< sqlstate 57014 — statement_timeout, admin cancel
    InvalidInput,          ///< sqlstate class 22 — data exception (bad text-mode cast, etc.)
    Unknown,               ///< anything not mapped above
};

/// Map a `QueryResult` into the enum. `r.ok == true` → `Ok`; otherwise the
/// return value is derived from `r.sqlstate` (with a small `Disconnected`
/// fallback when `sqlstate` is empty, which is what libpq sets when the
/// failure was at the transport rather than the SQL level).
StorageError classify(const QueryResult& r);

/// Stable short name for logs. Not localised, not user-facing.
const char* to_string(StorageError e);

/// True for failures a caller may retry blindly (deadlock / serialization).
/// Everything else, including `Disconnected`, requires application-level
/// judgement — retrying a broken connection without a reconnect step is
/// just a busy loop.
bool is_retriable(StorageError e);

} // namespace storage
} // namespace chess
