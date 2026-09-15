/**
 * database.h — the libpq wrapper the rest of the server talks to.
 *
 * WHAT THIS CLASS IS FOR
 *
 * Everything above it — the auth handlers, sessions, ratings — should be able
 * to work with the database without touching a single libpq type. Callers pass
 * a query string and a vector of parameters, they get rows back or an error.
 * That is the whole interface.
 *
 * THE ONE PROPERTY EVERYTHING RESTS ON: NO STRING CONCATENATION.
 *
 * `exec()` is the ONLY way to run a query. It takes parameters through libpq's
 * `PQexecParams` binding, never as concatenated SQL. There is no `raw_query()`
 * escape hatch and there is no format-string overload, on purpose: those
 * routes are how SQL injection gets into a codebase, one "just this once" at a
 * time. `test_database` proves it with the classic `'; DROP TABLE players;--`
 * as a *username*, and the row round-trips containing exactly that literal.
 *
 * RAII AROUND libpq
 *
 * `PGconn*` and `PGresult*` are heap allocations freed by bespoke functions.
 * The project rule is zero raw new/delete; `unique_ptr` with a stateless
 * deleter is the natural expression, and it means the many early returns
 * scattered through error paths cannot leak. A leaked `PGresult` holding the
 * output of "SELECT password_hash FROM …" is not a memory bug, it is a secret
 * on the heap with no destructor left to overwrite it.
 *
 * CONNECTION POOLING — deliberately absent.
 *
 * Supabase's session-mode pooler already gives us one; adding a client-side
 * pool on top would race against it and complicate exactly the shutdown
 * behaviour we want to stay boring. The single-instance Cloud Run policy
 * (`--max-instances=1`) means one server process holds one connection, and
 * that's fine — Phase 7's traffic is dominated by Argon2id CPU, not by
 * database round trips.
 *
 * ENVIRONMENT NOT ARGUMENTS
 *
 * A URL like `postgres://user:pw@host/db` is a secret with a real password in
 * it. Passing it on the command line puts it in process listings and shell
 * history. It comes from `$DATABASE_URL` and nowhere else; `.env`, `secrets/`,
 * `*.key` and `*.pem` are already in `.gitignore` from Phase 7.3.
 */

#pragma once

#include <libpq-fe.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chess {
namespace storage {

/// One returned row — column values as text, plus per-column null flags.
struct Row {
    std::vector<std::string> values;
    std::vector<bool>        nulls;

    bool        is_null(size_t col) const { return col < nulls.size() && nulls[col]; }
    const std::string& at(size_t col) const { return values.at(col); }
};

/// The outcome of an exec() call. `ok == false` populates `error`.
struct QueryResult {
    bool                 ok = false;
    std::string          error;

    /// Text-mode rows. libpq's binary mode buys speed we do not need at this
    /// traffic level, and buys a family of endianness-and-type bugs we do not
    /// want to write tests for.
    std::vector<Row>     rows;

    /// libpq's SQLSTATE — five characters like "23505" (unique_violation),
    /// "23514" (check_violation), "23503" (foreign_key_violation). Callers
    /// distinguish "already registered" from "database gone" by this code,
    /// not by parsing the human-readable error message.
    std::string          sqlstate;

    /// For INSERT/UPDATE/DELETE.
    size_t               rows_affected = 0;

    bool empty() const { return rows.empty(); }
    const Row& first() const { return rows.at(0); }
};

/**
 * A parameter to bind. Text-mode: everything is a string except NULL, which is
 * a real null pointer at the libpq layer.
 *
 * There is no int/bool/timestamp overload set on purpose. Postgres accepts
 * "true"/"false", "42", or ISO-8601 timestamp literals as text, and the
 * server-side type check catches a mismatch. Having callers commit to a
 * canonical text form keeps the wrapper small and pushes coercion decisions
 * one layer up, where the surrounding context is available.
 */
class Param {
public:
    static Param text(std::string v) { return Param(std::move(v), false); }
    static Param null()               { return Param(std::string(), true); }

    static Param int64(int64_t v)     { return text(std::to_string(v)); }
    static Param boolean(bool v)      { return text(v ? "true" : "false"); }

    bool               is_null() const { return null_; }
    const std::string& text_value() const { return value_; }

private:
    Param(std::string v, bool n) : value_(std::move(v)), null_(n) {}
    std::string value_;
    bool        null_;
};

/// Deleter for PGresult, kept out of the class body so the pointer alias is short.
struct PGresultDeleter {
    void operator()(PGresult* r) const noexcept { PQclear(r); }
};
using PGresultPtr = std::unique_ptr<PGresult, PGresultDeleter>;

struct PGconnDeleter {
    void operator()(PGconn* c) const noexcept { PQfinish(c); }
};
using PGconnPtr = std::unique_ptr<PGconn, PGconnDeleter>;

class Database {
public:
    Database() = default;

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept        = default;
    Database& operator=(Database&&) noexcept = default;

    ~Database() = default;

    /**
     * Connect using the URL in `$DATABASE_URL` (a `postgres://user:pw@host/db`
     * or `postgresql://…` string).
     *
     * Returns false and writes to `out_error` on failure. Never logs the URL —
     * it usually contains the password.
     */
    bool connect_from_env(std::string& out_error);

    /// Explicit form for tests. Same rules: no logging of the string.
    bool connect(const std::string& conninfo, std::string& out_error);

    bool connected() const { return conn_ != nullptr && PQstatus(conn_.get()) == CONNECTION_OK; }

    /**
     * The ONLY entry point. `sql` uses `$1, $2, …` placeholders; parameters go
     * through libpq's binding path, never through the SQL string.
     */
    QueryResult exec(const std::string& sql,
                     const std::vector<Param>& params = {});

    /// Run a script (schema DDL). No parameters. Fails on the first error.
    bool run_script(const std::string& sql, std::string& out_error);

    /**
     * Apply one named schema migration exactly once.
     *
     * The migration body and its `schema_migrations` record share one
     * transaction: either both become visible or neither does. `version` is
     * always bound through exec(), never concatenated into SQL. `sql` is
     * trusted repository-owned DDL and follows the same rule as run_script().
     *
     * On success, `out_applied` is true when this call applied the migration
     * and false when the version was already present.
     */
    bool apply_migration(const std::string& version,
                         const std::string& sql,
                         bool& out_applied,
                         std::string& out_error);

    /// Escape-hatch-free view of libpq: the SQLSTATE of the last failure.
    const std::string& last_sqlstate() const { return last_sqlstate_; }

private:
    PGconnPtr    conn_;
    std::string  last_sqlstate_;
};

/**
 * Error codes worth distinguishing in the auth path. Complete list:
 * https://www.postgresql.org/docs/current/errcodes-appendix.html
 */
namespace pg_errors {
constexpr const char* UNIQUE_VIOLATION      = "23505";
constexpr const char* CHECK_VIOLATION       = "23514";
constexpr const char* FOREIGN_KEY_VIOLATION = "23503";
constexpr const char* NOT_NULL_VIOLATION    = "23502";
} // namespace pg_errors

} // namespace storage
} // namespace chess
