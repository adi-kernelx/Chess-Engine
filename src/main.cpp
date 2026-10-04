/**
 * main.cpp — server entry point / composition root.
 *
 * Boot order (post LLD-5):
 *   1. Read $PORT from the environment (Cloud Run passes the port to bind on
 *      via this variable; local dev falls back to 9000).
 *   2. Build the network layer (thread pool → TcpServer → router).
 *   3. Build the game layer (RoomManager → Matchmaker → GameHandler).
 *   4. Optional auth stack: if $DATABASE_URL and $JWT_SIGNING_KEY are both
 *      present, wire Database → TokenSigner → SupabaseVerifier (Google, if
 *      configured) → SealedRegistry (if the ML-DSA identity key is on disk)
 *      → AuthHandler. Any missing piece degrades gracefully — password
 *      auth without Google, or capability-preview mode without a DB at all.
 *   5. Persistence ports: real PostgresGameStore + PostgresPlayerQueries
 *      when auth is enabled, Null adapters (LLD-3.3) otherwise. Services
 *      always see non-null references.
 *   6. Build the shared RequestPipeline (LLD-5) with the IdentityExtractor,
 *      a SealOpenFn wrapping SealedRegistry::inspect (or empty when seals
 *      are disabled), and the fd → Connection lookup. Both AuthHandler and
 *      GameHandler register their routes on it, then `install_on_router`
 *      binds every route on the router in one call.
 */

#include "application/auth/identity_extractor.h"
#include "application/ports/null_persistence.h"
#include "auth/auth_handler.h"
#include "auth/oauth_verify.h"
#include "auth/token.h"
#include "concurrent/thread_pool.h"
#include "core/logger.h"
#include "crypto/sealed_key_store.h"
#include "crypto/sealed_registry.h"
#include "crypto/signature.h"
#include "crypto/identity_file.h"
#include "game/game_handler.h"
#include "game/match_notify.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/tcp_server.h"
#include "protocol/request_pipeline.h"
#include "storage/database.h"
#include "storage/postgres_game_store.h"
#include "storage/postgres_player_queries.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

volatile std::sig_atomic_t shutdown_signal = 0;

void signal_handler(int signum) {
    // No allocation, logging, locks or socket cleanup in asynchronous context.
    shutdown_signal = signum;
}

namespace {

/**
 * Cloud Run passes the required bind port in $PORT. In local dev the variable
 * is unset and we use 9000, which the frontend defaults to as well. A malformed
 * $PORT is a startup-time error — silently falling back would let a broken
 * deployment silently expose the wrong port and be hard to diagnose.
 */
uint16_t resolve_port() {
    const char* env = std::getenv("PORT");
    if (!env || !*env) return 9000;
    char* end = nullptr;
    long v = std::strtol(env, &end, 10);
    if (end == env || *end != '\0' || v <= 0 || v > 65535) {
        chess::core::Logger::error("main", "startup",
            "PORT env var is not a valid TCP port; refusing to start");
        std::exit(2);
    }
    return static_cast<uint16_t>(v);
}

/**
 * If $SERVER_IDENTITY_KEY_PATH is set, read the ML-DSA-65 private key from
 * that file and return a live keypair. Returns an invalid MlDsa65KeyPair if
 * the env var is unset OR the file is unreadable — the caller treats that
 * as disabled only when no key was requested and sealing is not required.
 *
 * The key is generated once out-of-band via `tools/gen_server_identity`
 * (§7.3). On Cloud Run the recommended wiring is `--set-secrets
 * /secrets/server_identity.key=projects/…/secrets/server-identity/versions/latest`,
 * with $SERVER_IDENTITY_KEY_PATH pointing at /secrets/server_identity.key.
 */
chess::crypto::MlDsa65KeyPair try_load_identity(bool& out_loaded) {
    out_loaded = false;
    const char* path = std::getenv("SERVER_IDENTITY_KEY_PATH");
    if (!path || !*path) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        chess::core::Logger::warn("main", "startup",
            std::string("SERVER_IDENTITY_KEY_PATH set but unreadable: ") + path);
        return {};
    }
    in.close();
    // Accept original raw keys and the versioned text-safe deployment wrapper.
    auto kp = chess::crypto::load_identity_key_file(path);
    out_loaded = kp.valid();
    if (!out_loaded) {
        chess::core::Logger::warn("main", "startup",
            "SERVER_IDENTITY_KEY_PATH file is not a valid ML-DSA-65 key");
    }
    return kp;
}

} // namespace

int main() {
    using namespace chess;
    core::Logger::init(core::LogLevel::DEBUG);
    core::Logger::info("main", "startup", "=== Multiplayer Chess Platform ===");
    const char* seal_policy = std::getenv("AUTH_SEAL_REQUIRED");
    if (seal_policy && std::string(seal_policy) != "0" && std::string(seal_policy) != "1") {
        core::Logger::error("main", "startup", "AUTH_SEAL_REQUIRED must be 0 or 1");
        return 1;
    }
    const bool require_sealing = seal_policy && std::string(seal_policy) == "1";

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    const uint16_t port = resolve_port();
    core::Logger::info("main", "startup", "Binding to port " + std::to_string(port));

    // ── Network layer ──
    // Bound the process's worker count independently of host CPU reporting.
    // At least two workers allow I/O waits to overlap on a one-vCPU deployment.
    uint32_t worker_count = 4;
    if (const char* value = std::getenv("SERVER_WORKER_THREADS")) {
        if (value[0] < '1' || value[0] > '8' || value[1] != '\0') {
            core::Logger::error("main", "startup", "SERVER_WORKER_THREADS must be 1..8");
            return 1;
        }
        worker_count = static_cast<uint32_t>(value[0] - '0');
    }
    concurrent::ThreadPool pool(worker_count);

    net::TcpServer server(port, pool);

    // ── Game layer ──
    game::RoomManager  room_mgr;
    game::Matchmaker   matchmaker(room_mgr);
    game::GameHandler  game_handler(room_mgr, matchmaker);
    game_handler.set_background_executor([&pool](std::function<void()> task) {
        pool.submit(std::move(task));
    });
    game_handler.set_foreign_sender([&server](int fd, const std::string& frame) {
        server.send_text(fd, frame);
    });

    game_handler.set_connection_lookup([&server](int fd) -> net::Connection* {
        return server.get_connection(fd);
    });

    matchmaker.set_match_callback([&server](const game::MatchResult& match) {
        core::Logger::info("game", "Matchmaker",
            "Match found! " + match.white_name + " vs " + match.black_name +
            " → Game " + std::to_string(match.game_id));
        const std::string white_msg = game::build_match_found(game::MatchSide::White, match);
        const std::string black_msg = game::build_match_found(game::MatchSide::Black, match);

        auto* wc = server.get_connection(match.white_fd);
        auto* bc = server.get_connection(match.black_fd);
        if (wc) {
            net::WebSocket::write_frame(*wc, net::WsOpcode::TEXT, white_msg);
            while (wc->has_data_to_write()) { if (wc->write_to_socket() <= 0) break; }
        }
        if (bc) {
            net::WebSocket::write_frame(*bc, net::WsOpcode::TEXT, black_msg);
            while (bc->has_data_to_write()) { if (bc->write_to_socket() <= 0) break; }
        }
    });

    server.set_disconnect_callback([&game_handler](int fd) {
        game_handler.on_player_disconnect(fd);
    });
    server.set_maintenance_callback([&game_handler]() {
        game_handler.expire_disconnected_games();
    });

    // ── Auth layer (optional; graceful skip if not configured) ──
    //
    // These are heap-allocated because their lifetime must span the run() call
    // AND they must be destroyed BEFORE the router that references them. Using
    // unique_ptrs on the stack in main() satisfies both.
    std::unique_ptr<storage::Database>                    db;
    std::unique_ptr<storage::Database>                    tournament_db;
    std::unique_ptr<storage::Database>                    persistence_db;
    std::unique_ptr<storage::DatabasePool>                identity_read_pool;
    std::unique_ptr<auth::TokenSigner>                    signer;
    std::unique_ptr<auth::SupabaseVerifier>               google;
    std::unique_ptr<crypto::MlDsa65KeyPair>               identity;
    std::unique_ptr<crypto::SealedKeyStore>               seal_store;
    std::unique_ptr<crypto::SealedRegistry>               sealed_reg;
    std::unique_ptr<auth::AuthHandler>                    auth_handler;
    // LLD-3.3 — persistence ports; always non-null. Real adapter when
    // the DB is up, Null adapter (capability-disabled) otherwise.
    std::unique_ptr<application::ports::GameStore>        game_store;
    std::unique_ptr<application::ports::PlayerQueries>    player_queries;

    bool auth_enabled = false;
    {
        std::string err;
        auto tentative_db = std::make_unique<storage::Database>();
        if (!tentative_db->connect_from_env(err)) {
            core::Logger::warn("main", "startup",
                "Auth disabled: no DATABASE_URL, or connection failed (" + err + ")");
        } else {
            auto tentative_signer = std::make_unique<auth::TokenSigner>(
                auth::TokenSigner::from_env(err));
            if (!tentative_signer->valid()) {
                core::Logger::warn("main", "startup",
                    "Auth disabled: no JWT_SIGNING_KEY (" + err + ")");
            } else {
                db     = std::move(tentative_db);
                signer = std::move(tentative_signer);
                auth_enabled = true;
            }
        }
    }

    if (require_sealing && !auth_enabled) {
        core::Logger::error("main", "startup", "AUTH_SEAL_REQUIRED needs configured authentication");
        return 1;
    }
    if (auth_enabled) {
        // Supabase / Google is optional — even without it, password auth works.
        std::string err;
        auto tentative_google = std::make_unique<auth::SupabaseVerifier>(
            auth::SupabaseVerifier::from_env(err));
        if (tentative_google->valid()) {
            google = std::move(tentative_google);
            core::Logger::info("main", "startup", "Google Sign-In enabled");
        } else {
            core::Logger::info("main", "startup",
                "Google Sign-In disabled: " + err);
        }

        // Sealed envelopes are optional too — they need the on-disk identity
        // key. Local development may explicitly omit sealing; production sets
        // AUTH_SEAL_REQUIRED=1. A configured but invalid key never downgrades.
        bool id_ok = false;
        auto tentative_id = std::make_unique<crypto::MlDsa65KeyPair>(
            try_load_identity(id_ok));
        const char* identity_path = std::getenv("SERVER_IDENTITY_KEY_PATH");
        if (!id_ok && (require_sealing || (identity_path && *identity_path))) {
            core::Logger::error("main", "startup",
                "Sealed authentication needs a readable, valid SERVER_IDENTITY_KEY_PATH; refusing downgrade");
            return 1;
        }
        if (id_ok) {
            identity   = std::move(tentative_id);
            seal_store = std::make_unique<crypto::SealedKeyStore>();
            sealed_reg = std::make_unique<crypto::SealedRegistry>(*identity, *seal_store);
            sealed_reg->require_sealed("login");
            sealed_reg->require_sealed("register");
            sealed_reg->require_sealed("google_auth");
            // LLD-5.3: seal opening now lives inside the request
            // pipeline (see below). The old `set_pre_dispatch` hook
            // is gone.
            core::Logger::info("main", "startup", "Sealed envelopes enabled");
        } else {
            core::Logger::info("main", "startup",
                "Sealed envelopes disabled (no SERVER_IDENTITY_KEY_PATH)");
        }

        // Wire game persistence: Database* for the tournament family
        // (which still calls Database directly — its own port is a
        // future slice), plus the LLD-3.2/3.3 ports for the game
        // family. Both ports get their real Postgres adapters here.
        // Bulk tournament reads/maintenance and completion transactions must
        // not queue ahead of access-token validation or refresh rotation.
        // These are three bounded, independently serialized PG sessions, not
        // three threads concurrently using one libpq connection.
        tournament_db = std::make_unique<storage::Database>();
        persistence_db = std::make_unique<storage::Database>();
        if (!tournament_db->connect_from_env(err) || !persistence_db->connect_from_env(err)) {
            core::Logger::error("main", "startup", "Persistence connection initialization failed");
            return 1;
        }
        game_handler.set_database(tournament_db.get());
        game_store     = std::make_unique<storage::PostgresGameStore>(*persistence_db);
        player_queries = std::make_unique<storage::PostgresPlayerQueries>(*persistence_db);
        game_handler.set_game_store(game_store.get());
        game_handler.set_player_queries(player_queries.get());
        // Narrow, reversible optimization: only fresh identity reads borrow
        // from a pool. Auth writes/refresh and tournament transactions retain
        // their verified dedicated sessions. At most seven total PG sessions.
        size_t read_connections = 2;
        if (const char* value = std::getenv("AUTH_READ_POOL_SIZE")) {
            if (value[0] < '0' || value[0] > '4' || value[1] != '\0') {
                core::Logger::error("main", "startup", "AUTH_READ_POOL_SIZE must be 0..4");
                return 1;
            }
            read_connections = static_cast<size_t>(value[0] - '0');
        }
        if (read_connections) {
            identity_read_pool = std::make_unique<storage::DatabasePool>();
            if (!identity_read_pool->connect_from_env(read_connections, err)) {
                core::Logger::error("main", "startup", "Identity read pool initialization failed");
                return 1;
            }
        }
        core::Logger::info("main", "startup", "Game persistence enabled");
    }

    // LLD-3.3 capability-disabled composition: if auth was NOT enabled,
    // the DB is unavailable. Wire the Null adapters so services still
    // receive real (non-null) references and every DB-touching route
    // uniformly emits its pre-refactor "unavailable" wire error.
    if (!game_store) {
        game_store     = std::make_unique<application::ports::NullGameStore>();
        player_queries = std::make_unique<application::ports::NullPlayerQueries>();
        game_handler.set_game_store(game_store.get());
        game_handler.set_player_queries(player_queries.get());
        core::Logger::info("main", "startup",
            "Persistence disabled (Null adapters wired)");
    }

    // ── Request pipeline (LLD-5) ──
    //
    // One auditable path per request. Stages: SealOpen (via the
    // callback below) → ParseJson → Auth → Dispatch. AuthHandler
    // registers as raw routes (SealOpen only); every other family
    // uses the typed path.
    application::auth::IdentityExtractor identity_extractor(
        db.get(), signer.get(), nullptr, identity_read_pool.get());
    auto lookup = [&server](int fd) -> net::Connection* {
        return server.get_connection(fd);
    };
    protocol::SealOpenFn seal_open;
    if (sealed_reg) {
        seal_open = [reg = sealed_reg.get()](const std::string& type,
                                             const std::string& message,
                                             std::string&       out) {
            std::string opened;
            switch (reg->inspect(type, message, opened)) {
                case crypto::SealedRegistry::Outcome::NotSealed:
                    return protocol::SealOutcome::Continue;
                case crypto::SealedRegistry::Outcome::Opened:
                    out = std::move(opened);
                    return protocol::SealOutcome::Rewritten;
                case crypto::SealedRegistry::Outcome::Rejected:
                    return protocol::SealOutcome::Rejected;
            }
            return protocol::SealOutcome::Rejected;
        };
    }
    protocol::RequestPipeline pipeline(&identity_extractor,
                                       std::move(seal_open),
                                       lookup);

    if (auth_enabled) {
        auth_handler = std::make_unique<auth::AuthHandler>(
            *db, *signer, google.get(), sealed_reg.get());
        auth_handler->register_handlers(pipeline);
        core::Logger::info("main", "startup", "Auth handlers registered");
    }

    game_handler.register_handlers(pipeline);

    // Bind every pipeline-registered route on the router in one call.
    pipeline.install_on_router(server.get_router());

    server.get_router().set_default_handler(
        [](net::Connection& conn, const std::string&) {
            // Even unregistered routes can contain credentials (e.g. Google
            // auth when the DB is unavailable). Never log user-controlled
            // payloads or types through this fallback.
            core::Logger::warn("main", "unknown", "Unrecognized message (payload redacted)");
            net::WebSocket::write_frame(conn, net::WsOpcode::TEXT,
                "{\"type\":\"error\",\"message\":\"Unknown message type\"}");
        });

    if (server.start()) {
        core::Logger::info("main", "startup",
            "Server ready. Listening on ws://0.0.0.0:" + std::to_string(port));
        server.run([] { return shutdown_signal != 0; });
    } else {
        core::Logger::error("main", "startup", "Failed to start server");
        return 1;
    }

    core::Logger::info("main", "shutdown", "Draining workers after event loop/maintenance exit");
    pool.shutdown();
    server.stop();
    core::Logger::info("main", "shutdown", "Server exited gracefully");
    return 0;
}
