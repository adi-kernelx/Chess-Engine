// tools/bench_provision.cpp — pre-provision load-test accounts + JWTs.
//
// The load-test client (Python) cannot easily replay the sealed-envelope
// registration path: doing so means reimplementing ML-KEM-768 + X25519 +
// HKDF-SHA-384 + AES-CTR + HMAC-SHA-384 in Python for a throwaway tool.
// Sealed-envelope confidentiality only protects the password on the wire;
// nothing on the make_move hot path involves it, so bypassing it does not
// distort what the load test measures.
//
// This tool:
//   1. Reads DATABASE_URL and JWT_SIGNING_KEY from the environment — the
//      SAME env vars the server reads, so tokens minted here verify there.
//   2. Inserts N fresh test accounts named `loadtest_NNNNN` (idempotent:
//      an ON CONFLICT clause reuses an existing row's player_id).
//   3. Mints an HS384 access token for each.
//   4. Emits a JSON array on stdout: [{player_id, username, access_token}, …]
//
// Usage:  bench_provision <count>   (env: DATABASE_URL, JWT_SIGNING_KEY)

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "auth/token.h"
#include "storage/database.h"

using json = nlohmann::json;
using namespace chess;
using namespace chess::storage;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bench_provision <count>\n";
        return 2;
    }
    int count = std::atoi(argv[1]);
    if (count <= 0 || count > 10000) {
        std::cerr << "count must be in 1..10000\n";
        return 2;
    }

    // Same env vars the server reads.
    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cerr << "DATABASE_URL not set\n"; return 1;
    }
    if (std::getenv("JWT_SIGNING_KEY") == nullptr) {
        std::cerr << "JWT_SIGNING_KEY not set\n"; return 1;
    }

    Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "db connect failed: " << err << '\n'; return 1;
    }

    auto signer = auth::TokenSigner::from_env(err);
    if (!signer.valid()) {
        std::cerr << "signer init failed: " << err << '\n'; return 1;
    }

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    json out = json::array();

    for (int i = 0; i < count; ++i) {
        char name_buf[32];
        std::snprintf(name_buf, sizeof(name_buf), "loadtest_%05d", i);
        const std::string username = name_buf;
        std::string username_ci = username;  // already lowercase

        // Idempotent: if the row already exists (from a previous run),
        // reuse its id. password_hash is a fixed placeholder — nothing
        // in the load-test path ever authenticates against it.
        auto r = db.exec(
            "INSERT INTO players(username, username_ci, password_hash, elo_rating) "
            "VALUES($1, $2, $3, 1200) "
            "ON CONFLICT (username) DO UPDATE SET elo_rating = 1200 "
            "RETURNING id",
            {Param::text(username), Param::text(username_ci),
             Param::text("loadtest-placeholder")});
        if (!r.ok || r.empty()) {
            std::cerr << "insert " << username << " failed: " << r.error << '\n';
            return 1;
        }
        const int64_t pid = std::stoll(r.first().at(0));

        auth::AccessClaims c;
        c.player_id   = pid;
        c.username    = username;
        c.token_epoch = 0;
        c.issued_at   = now;
        // Generous TTL — the whole load test finishes well inside 15 min,
        // but pinning to 60 min removes clock-skew flakes if the WSL clock
        // drifts against the DB.
        c.expires_at  = now + 60 * 60;
        std::string tok = signer.issue_access(c);
        if (tok.empty()) {
            std::cerr << "issue_access failed for " << username << '\n';
            return 1;
        }

        out.push_back({
            {"player_id",    pid},
            {"username",     username},
            {"access_token", tok},
        });
    }

    std::cout << out.dump() << '\n';
    return 0;
}
