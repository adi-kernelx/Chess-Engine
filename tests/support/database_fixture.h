/**
 * database_fixture.h — safe destructive test-DB setup.
 *
 * Every DB-backed test in this repo starts by DROPping the tables it uses.
 * That is fine against the local `chess_bench` cluster; it is catastrophic
 * against the production Supabase pooler URL that lives in `.env`. This
 * header is a single-place gate that every destructive test path must
 * pass through, so that a wrong environment variable can never wipe a
 * real database.
 *
 * The rule the gate enforces:
 *   - $DATABASE_URL must resolve to 127.0.0.1 / localhost.
 *   - The database name must be one of the known test names below.
 *   - The URL must NOT contain the substring `supabase` or `pooler`.
 * If any check fails, `require_test_database()` aborts the test with a
 * clear diagnostic instead of running its DROPs. There is deliberately
 * no override flag — the fix is to set the right URL, not to bypass the
 * guard.
 *
 * Usage in a test's main():
 *
 *     if (std::getenv("DATABASE_URL") == nullptr) return 0;   // skip
 *     std::string why;
 *     if (!chess::tests::require_test_database(why)) {
 *         std::cerr << "  REFUSING to run destructive setup: " << why << '\n';
 *         return 2;
 *     }
 *     // ... proceed with DROP TABLE … ; CREATE TABLE … ; etc.
 */

#pragma once

#include <cstdlib>
#include <string>

namespace chess {
namespace tests {

// Databases a destructive test is allowed to touch. Extend this list only
// after confirming the target really is disposable.
inline bool is_allowed_test_dbname(const std::string& dbname) {
    return dbname == "chess_bench"
        || dbname == "chess_test"
        || dbname == "postgres_test";
}

// Parse `postgresql://user:pass@host:port/dbname?…` well enough to answer
// two questions: (1) is the host local, and (2) what is the dbname. We do
// not need a full URL parser — the two fields have unambiguous positions.
inline void parse_pg_url(const std::string& url,
                         std::string& out_host,
                         std::string& out_dbname) {
    out_host.clear();
    out_dbname.clear();
    // Find "://" — everything before it is the scheme, everything after
    // is "[user[:pass]@]host[:port]/dbname[?query]".
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return;
    const std::string rest = url.substr(scheme_end + 3);

    // Split off "user:pass@" if present.
    const auto at = rest.find('@');
    const std::string host_and_more = (at == std::string::npos)
        ? rest : rest.substr(at + 1);

    // Split "host[:port]/dbname[?query]" on the first '/'.
    const auto slash = host_and_more.find('/');
    if (slash == std::string::npos) return;                         // no dbname
    const std::string host_port = host_and_more.substr(0, slash);
    std::string dbname          = host_and_more.substr(slash + 1);

    // Strip "?query" off the dbname.
    const auto q = dbname.find('?');
    if (q != std::string::npos) dbname.resize(q);

    // Strip ":port" off host_port.
    const auto colon = host_port.find(':');
    out_host   = (colon == std::string::npos)
        ? host_port : host_port.substr(0, colon);
    out_dbname = dbname;
}

// Returns true iff DATABASE_URL is a local, disposable test cluster. On
// false, `out_reason` explains what looked wrong. Never logs the URL
// itself — a URL with credentials must not surface in test output.
inline bool require_test_database(std::string& out_reason) {
    const char* env = std::getenv("DATABASE_URL");
    if (env == nullptr || *env == '\0') {
        out_reason = "DATABASE_URL not set"; return false;
    }
    const std::string url = env;

    // Fast substring rejects — these can appear anywhere in the URL and
    // if they do, the URL is not a local test cluster.
    if (url.find("supabase") != std::string::npos) {
        out_reason = "DATABASE_URL contains 'supabase' — refusing destructive setup";
        return false;
    }
    if (url.find("pooler") != std::string::npos) {
        out_reason = "DATABASE_URL contains 'pooler' — refusing destructive setup";
        return false;
    }

    std::string host, dbname;
    parse_pg_url(url, host, dbname);
    if (host != "127.0.0.1" && host != "localhost" && host != "::1") {
        out_reason = "DATABASE_URL host is not local (found host='" + host + "')";
        return false;
    }
    if (!is_allowed_test_dbname(dbname)) {
        out_reason = "DATABASE_URL dbname is not a known test database "
                     "(found dbname='" + dbname + "'); "
                     "expected one of: chess_bench, chess_test, postgres_test";
        return false;
    }
    return true;
}

} // namespace tests
} // namespace chess
