#include "storage/database.h"

#include <cstdlib>
#include <cstring>

namespace chess {
namespace storage {

namespace {

std::string safe_error(PGconn* c) {
    // PQerrorMessage may include a trailing newline; trim it so log lines
    // stay one-per-message.
    const char* msg = c ? PQerrorMessage(c) : nullptr;
    if (msg == nullptr) return "unknown libpq error";
    std::string out(msg);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string sqlstate_of(PGresult* r) {
    const char* s = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
    return s ? std::string(s) : std::string();
}

} // namespace

Database::Database(Database&& other) noexcept {
    [[maybe_unused]] auto operation = other.acquire_operation();
    conn_ = std::move(other.conn_);
    last_sqlstate_ = std::move(other.last_sqlstate_);
}

Database& Database::operator=(Database&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock operations(operation_mutex_, other.operation_mutex_);
    conn_ = std::move(other.conn_);
    last_sqlstate_ = std::move(other.last_sqlstate_);
    return *this;
}

bool Database::connect_from_env(std::string& out_error) {
    return connect_from_env(out_error, 0);
}

bool Database::connect_from_env(std::string& out_error, int timeout_seconds) {
    const char* url = std::getenv("DATABASE_URL");
    if (url == nullptr || url[0] == '\0') {
        out_error = "DATABASE_URL is not set";
        return false;
    }
    if (timeout_seconds <= 0) return connect(url, out_error);
    [[maybe_unused]] auto operation = acquire_operation();
    const auto timeout = std::to_string(timeout_seconds);
    const char* keywords[] = {"dbname", "connect_timeout", nullptr};
    const char* values[] = {url, timeout.c_str(), nullptr};
    conn_.reset(PQconnectdbParams(keywords, values, 1));
    if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK) {
        out_error = safe_error(conn_.get());
        conn_.reset();
        return false;
    }
    return true;
}

bool Database::connect(const std::string& conninfo, std::string& out_error) {
    [[maybe_unused]] auto operation = acquire_operation();
    conn_.reset(PQconnectdb(conninfo.c_str()));
    if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK) {
        // Never include the conninfo in this message: it usually contains a
        // password, and error paths are one of the easiest ways for secrets to
        // reach a log.
        out_error = safe_error(conn_.get());
        conn_.reset();
        return false;
    }
    return true;
}

bool Database::connected() const {
    [[maybe_unused]] auto operation = acquire_operation();
    return conn_ != nullptr && PQstatus(conn_.get()) == CONNECTION_OK;
}

bool Database::reset_for_reuse() {
    [[maybe_unused]] auto operation = acquire_operation();
    if (!connected()) return false;
    const auto status = PQtransactionStatus(conn_.get());
    if (status == PQTRANS_IDLE) return true;
    if (status != PQTRANS_INTRANS && status != PQTRANS_INERROR) return false;
    PGresultPtr result(PQexec(conn_.get(), "ROLLBACK"));
    return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK &&
           PQtransactionStatus(conn_.get()) == PQTRANS_IDLE;
}

std::string Database::last_sqlstate() const {
    [[maybe_unused]] auto operation = acquire_operation();
    return last_sqlstate_;
}

QueryResult Database::exec(const std::string& sql,
                           const std::vector<Param>& params) {
    query_count_.fetch_add(1, std::memory_order_relaxed);
    [[maybe_unused]] auto operation = acquire_operation();
    QueryResult result;
    if (!connected()) {
        result.error = "not connected";
        return result;
    }

    // libpq wants two parallel arrays: char** for values (nullptr means NULL)
    // and int* for lengths (ignored in text mode). Building them once and
    // handing raw pointers is what makes text-mode binding cheap.
    const size_t n = params.size();
    std::vector<const char*> values(n, nullptr);
    for (size_t i = 0; i < n; ++i) {
        values[i] = params[i].is_null() ? nullptr : params[i].text_value().c_str();
    }

    PGresultPtr res(PQexecParams(conn_.get(), sql.c_str(),
                                 static_cast<int>(n),
                                 /*paramTypes=*/nullptr,
                                 values.data(),
                                 /*paramLengths=*/nullptr,
                                 /*paramFormats=*/nullptr,
                                 /*resultFormat=*/0));   // text mode

    if (!res) {
        result.error = safe_error(conn_.get());
        return result;
    }

    const ExecStatusType status = PQresultStatus(res.get());
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        result.sqlstate = sqlstate_of(res.get());
        result.error    = PQresultErrorMessage(res.get());
        // Same trim as connect(); a nullptr from PQresultErrorMessage becomes
        // an empty string, so no null check is needed.
        while (!result.error.empty() &&
               (result.error.back() == '\n' || result.error.back() == '\r')) {
            result.error.pop_back();
        }
        last_sqlstate_ = result.sqlstate;
        return result;
    }

    const int rows = PQntuples(res.get());
    const int cols = PQnfields(res.get());
    result.rows.reserve(rows);
    for (int i = 0; i < rows; ++i) {
        Row row;
        row.values.resize(cols);
        row.nulls.resize(cols, false);
        for (int j = 0; j < cols; ++j) {
            if (PQgetisnull(res.get(), i, j)) {
                row.nulls[j] = true;
            } else {
                row.values[j] = PQgetvalue(res.get(), i, j);
            }
        }
        result.rows.push_back(std::move(row));
    }

    if (const char* affected = PQcmdTuples(res.get())) {
        if (*affected != '\0') result.rows_affected = std::strtoull(affected, nullptr, 10);
    }

    last_sqlstate_.clear();
    result.ok = true;
    return result;
}

bool Database::run_script(const std::string& sql, std::string& out_error) {
    [[maybe_unused]] auto operation = acquire_operation();
    if (!connected()) { out_error = "not connected"; return false; }

    // PQexec (no params) accepts multiple semicolon-separated statements, which
    // is what we want for schema files. This is the only entry point that
    // allows more than one statement per call; it takes NO parameters and is
    // used only for our own DDL.
    PGresultPtr res(PQexec(conn_.get(), sql.c_str()));
    if (!res) { out_error = safe_error(conn_.get()); return false; }

    const ExecStatusType status = PQresultStatus(res.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        out_error = PQresultErrorMessage(res.get());
        last_sqlstate_ = sqlstate_of(res.get());
        return false;
    }
    return true;
}

bool Database::apply_migration(const std::string& version,
                               const std::string& sql,
                               bool& out_applied,
                               std::string& out_error) {
    out_applied = false;
    out_error.clear();
    [[maybe_unused]] auto operation = acquire_operation();

    if (!connected()) {
        out_error = "not connected";
        return false;
    }
    if (version.empty()) {
        out_error = "migration version is empty";
        return false;
    }

    auto begin = exec("BEGIN");
    if (!begin.ok) {
        out_error = begin.error;
        return false;
    }

    // Keep cleanup local so every failure path leaves the connection outside
    // the aborted transaction. ROLLBACK itself is best-effort: the original
    // error is the useful diagnostic and must not be overwritten.
    const auto rollback = [this]() { (void)exec("ROLLBACK"); };

    // Serialise migration runners on this database. This avoids the classic
    // first-run CREATE race as well as the later check-then-apply race,
    // without holding a permanent session-level lock.
    auto locked = exec("SELECT pg_advisory_xact_lock(1128813617)");
    if (!locked.ok) {
        out_error = locked.error;
        rollback();
        return false;
    }

    auto table = exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        " version TEXT PRIMARY KEY,"
        " applied_at TIMESTAMPTZ NOT NULL DEFAULT now()"
        ")");
    if (!table.ok) {
        out_error = table.error;
        rollback();
        return false;
    }

    auto existing = exec(
        "SELECT 1 FROM schema_migrations WHERE version=$1",
        {Param::text(version)});
    if (!existing.ok) {
        out_error = existing.error;
        rollback();
        return false;
    }
    if (!existing.empty()) {
        auto commit = exec("COMMIT");
        if (!commit.ok) {
            out_error = commit.error;
            rollback();
            return false;
        }
        return true;
    }

    if (sql.empty()) {
        out_error = "migration SQL is empty";
        rollback();
        return false;
    }
    if (!run_script(sql, out_error)) {
        rollback();
        return false;
    }

    auto recorded = exec(
        "INSERT INTO schema_migrations(version) VALUES($1)",
        {Param::text(version)});
    if (!recorded.ok) {
        out_error = recorded.error;
        rollback();
        return false;
    }

    auto commit = exec("COMMIT");
    if (!commit.ok) {
        out_error = commit.error;
        rollback();
        return false;
    }

    out_applied = true;
    return true;
}

} // namespace storage
} // namespace chess
