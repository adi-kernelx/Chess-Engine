/**
 * main.cpp — server entry point.
 *
 * Boot order (§7.10):
 *   1. Read $PORT from the environment (Cloud Run passes the port to bind on
 *      via this variable; local dev falls back to 9000).
 *   2. Build the network layer (thread pool → TcpServer → router).
 *   3. Build the game layer (RoomManager → Matchmaker → GameHandler).
 *   4. If $DATABASE_URL and $JWT_SIGNING_KEY are both present, build the auth
 *      layer (Database → TokenSigner → SupabaseVerifier if configured →
 *      SealedRegistry if the ML-DSA identity key is on disk → AuthHandler).
 *      If either core auth env is missing, the auth surface is simply not
 *      registered and the frontend falls back to capability-preview mode.
 *   5. Wire the sealed-envelope pre-dispatch hook so seal-required messages
 *      are opened (or refused if the type is registered but arrived
 *      unsealed) BEFORE the handler is reached.
 */

#include "auth/auth_handler.h"
#include "auth/oauth_verify.h"
#include "auth/token.h"
#include "concurrent/thread_pool.h"
#include "core/logger.h"
#include "crypto/sealed_key_store.h"
#include "crypto/sealed_registry.h"
#include "crypto/signature.h"
#include "game/game_handler.h"
#include "game/match_notify.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/tcp_server.h"
#include "storage/database.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

chess::net::TcpServer*          g_server = nullptr;
chess::concurrent::ThreadPool*  g_pool   = nullptr;

void signal_handler(int signum) {
    chess::core::Logger::info("main", "signal",
        "Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_server) g_server->stop();
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
 * as "sealed envelopes are disabled for this run".
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
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    auto kp = chess::crypto::MlDsa65KeyPair::from_private_key(buf.data(), buf.size());
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

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    const uint16_t port = resolve_port();
    core::Logger::info("main", "startup", "Binding to port " + std::to_string(port));

    // ── Network layer ──
    concurrent::ThreadPool pool;
    g_pool = &pool;

    net::TcpServer server(port, pool);
    g_server = &server;

    // ── Game layer ──
    game::RoomManager  room_mgr;
    game::Matchmaker   matchmaker(room_mgr);
    game::GameHandler  game_handler(room_mgr, matchmaker);

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

    game_handler.register_handlers(server.get_router());

    server.set_disconnect_callback([&game_handler](int fd) {
        game_handler.on_player_disconnect(fd);
    });

    // ── Auth layer (optional; graceful skip if not configured) ──
    //
    // These are heap-allocated because their lifetime must span the run() call
    // AND they must be destroyed BEFORE the router that references them. Using
    // unique_ptrs on the stack in main() satisfies both.
    std::unique_ptr<storage::Database>       db;
    std::unique_ptr<auth::TokenSigner>       signer;
    std::unique_ptr<auth::SupabaseVerifier>  google;
    std::unique_ptr<crypto::MlDsa65KeyPair>  identity;
    std::unique_ptr<crypto::SealedKeyStore>  seal_store;
    std::unique_ptr<crypto::SealedRegistry>  sealed_reg;
    std::unique_ptr<auth::AuthHandler>       auth_handler;

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
        // key. Password auth remains available without them (a downgrade the
        // operator explicitly opts into by not deploying an identity key).
        bool id_ok = false;
        auto tentative_id = std::make_unique<crypto::MlDsa65KeyPair>(
            try_load_identity(id_ok));
        if (id_ok) {
            identity   = std::move(tentative_id);
            seal_store = std::make_unique<crypto::SealedKeyStore>();
            sealed_reg = std::make_unique<crypto::SealedRegistry>(*identity, *seal_store);
            sealed_reg->require_sealed("login");
            sealed_reg->require_sealed("register");
            sealed_reg->require_sealed("google_auth");

            // Pre-dispatch hook: opens or rejects seal-required messages
            // before any handler is reached. See sealed_registry.h.
            server.get_router().set_pre_dispatch(
                [reg = sealed_reg.get()](net::Connection&, const std::string& type,
                                         const std::string& message,
                                         std::string& rewritten) {
                    std::string opened;
                    auto out = reg->inspect(type, message, opened);
                    if (out == crypto::SealedRegistry::Outcome::NotSealed) {
                        return net::MessageRouter::PreDispatch::Continue;
                    }
                    if (out == crypto::SealedRegistry::Outcome::Opened) {
                        rewritten = std::move(opened);
                        return net::MessageRouter::PreDispatch::Replace;
                    }
                    return net::MessageRouter::PreDispatch::Reject;
                });
            core::Logger::info("main", "startup", "Sealed envelopes enabled");
        } else {
            core::Logger::info("main", "startup",
                "Sealed envelopes disabled (no SERVER_IDENTITY_KEY_PATH)");
        }

        auth_handler = std::make_unique<auth::AuthHandler>(
            *db, *signer, google.get(), sealed_reg.get());
        auth_handler->register_handlers(server.get_router());
        core::Logger::info("main", "startup", "Auth handlers registered");

        // Wire game persistence: GameHandler needs the DB for save_completed_game,
        // get_profile, get_leaderboard, and the signer for auth extraction.
        game_handler.set_database(db.get());
        game_handler.set_signer(signer.get());
        core::Logger::info("main", "startup", "Game persistence enabled");
    }

    server.get_router().set_default_handler(
        [](net::Connection& conn, const std::string& message) {
            core::Logger::warn("main", "unknown", "Unrecognized message: " + message);
            net::WebSocket::write_frame(conn, net::WsOpcode::TEXT,
                "{\"type\":\"error\",\"message\":\"Unknown message type\"}");
        });

    if (server.start()) {
        core::Logger::info("main", "startup",
            "Server ready. Listening on ws://0.0.0.0:" + std::to_string(port));
        server.run();
    } else {
        core::Logger::error("main", "startup", "Failed to start server");
        return 1;
    }

    pool.shutdown();
    core::Logger::info("main", "shutdown", "Server exited gracefully");
    return 0;
}
