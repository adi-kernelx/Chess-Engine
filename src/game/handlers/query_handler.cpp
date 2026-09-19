/**
 * game/handlers/query_handler.cpp — see header for role.
 *
 * Every handler follows the same three-line pattern established by
 * GameplayHandler:
 *
 *     parse JSON  → auth?  → build ctx + sink  → delegate to service
 *
 * The service does the business work and every send. This file has no
 * knowledge of RoomManager or Database — it only glues the transport
 * surface to the application boundary.
 *
 * WIRE PARITY
 *   Error phrasings and JSON shapes are preserved bit-for-bit from the
 *   pre-refactor `GameHandler`. Only `spectate` is auth-required in
 *   this family; the other six routes are public reads.
 */

#include "game/handlers/query_handler.h"

#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

#include "core/logger.h"

using nlohmann::json;

namespace chess::game::handlers {

QueryHandler::QueryHandler(chess::application::GameQueryService&              service,
                           const chess::application::auth::IdentityExtractor* identity,
                           chess::net::ConnectionLookup                       lookup)
    : service_(service), identity_(identity), lookup_(std::move(lookup)) {}

void QueryHandler::register_handlers(chess::net::MessageRouter& router) {
    router.register_handler("get_profile",
        [this](chess::net::Connection& c, const std::string& m) { handle_get_profile(c, m); });
    router.register_handler("get_leaderboard",
        [this](chess::net::Connection& c, const std::string& m) { handle_get_leaderboard(c, m); });
    router.register_handler("get_history",
        [this](chess::net::Connection& c, const std::string& m) { handle_get_history(c, m); });
    router.register_handler("get_game",
        [this](chess::net::Connection& c, const std::string& m) { handle_get_game(c, m); });
    router.register_handler("list_live_games",
        [this](chess::net::Connection& c, const std::string& m) { handle_list_live_games(c, m); });
    router.register_handler("spectate",
        [this](chess::net::Connection& c, const std::string& m) { handle_spectate(c, m); });
    router.register_handler("stop_spectating",
        [this](chess::net::Connection& c, const std::string& m) { handle_stop_spectating(c, m); });
}

// ── Adapter helpers ─────────────────────────────────────────────────

chess::application::RequestContext
QueryHandler::make_ctx(chess::net::Connection& conn) const {
    chess::application::RequestContext ctx;
    ctx.caller           = conn.handle();
    ctx.received_at_unix = static_cast<int64_t>(std::time(nullptr));
    return ctx;
}

std::unique_ptr<chess::net::SocketMessageSink>
QueryHandler::make_caller_sink(chess::net::Connection& conn) const {
    // Direct-ctor sink — see socket_message_sink.h and
    // gameplay_handler.cpp for the contract.
    return std::make_unique<chess::net::SocketMessageSink>(conn);
}

void QueryHandler::send_error(chess::net::Connection& conn,
                              const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

void QueryHandler::send_auth_error(chess::net::Connection& conn,
                                   const std::string& message) {
    json err;
    err["type"]    = "error";
    err["code"]    = chess::application::auth::kAuthRequiredCode;
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

// ── get_profile ─────────────────────────────────────────────────────

void QueryHandler::handle_get_profile(chess::net::Connection& conn,
                                      const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    std::string username = msg.value("username", std::string{});
    int offset           = msg.value("offset", 0);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.get_profile(ctx, username, offset, *sink);
}

// ── get_leaderboard ─────────────────────────────────────────────────

void QueryHandler::handle_get_leaderboard(chess::net::Connection& conn,
                                          const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    int limit  = msg.value("limit", 50);
    int offset = msg.value("offset", 0);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.get_leaderboard(ctx, limit, offset, *sink);
}

// ── get_history ─────────────────────────────────────────────────────

void QueryHandler::handle_get_history(chess::net::Connection& conn,
                                      const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    std::string username = msg.value("username", std::string{});
    int limit            = msg.value("limit", 20);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.get_history(ctx, username, limit, *sink);
}

// ── get_game ────────────────────────────────────────────────────────

void QueryHandler::handle_get_game(chess::net::Connection& conn,
                                   const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.get_game(ctx, game_id, *sink);
}

// ── list_live_games ─────────────────────────────────────────────────

void QueryHandler::handle_list_live_games(chess::net::Connection& conn,
                                          const std::string& /*message*/) {
    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.list_live_games(ctx, *sink);
}

// ── spectate (auth required) ────────────────────────────────────────

void QueryHandler::handle_spectate(chess::net::Connection& conn,
                                   const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    if (!identity_) {
        send_auth_error(conn, "Authentication is not configured on this server");
        return;
    }
    auto id_res = identity_->extract(msg);
    if (!id_res.is_ok()) {
        send_auth_error(conn, id_res.reason);
        return;
    }

    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.spectate(ctx, id_res.value.username, game_id, *sink);
}

// ── stop_spectating ─────────────────────────────────────────────────

void QueryHandler::handle_stop_spectating(chess::net::Connection& conn,
                                          const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.stop_spectating(ctx, game_id, *sink);
}

} // namespace chess::game::handlers
