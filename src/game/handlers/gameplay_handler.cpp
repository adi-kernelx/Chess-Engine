/**
 * game/handlers/gameplay_handler.cpp — see header for role.
 *
 * Every handler follows the same three-line pattern:
 *
 *     parse JSON  → auth?  → build ctx + sink  → delegate to service
 *
 * The service does the business work and every send. This file has no
 * knowledge of RoomManager, Matchmaker, or Database — it only glues
 * the transport surface to the application boundary.
 *
 * WIRE PARITY
 *   The error phrasings and JSON shapes are preserved bit-for-bit from
 *   the pre-refactor `GameHandler`. `make_error` and `send_error_code`
 *   have equivalent private helpers on this class; the migrated
 *   `MessageSink` routes flow through the same `protocol::codec` the
 *   old GameHandler used post-LLD-1.
 */

#include "game/handlers/gameplay_handler.h"

#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

#include "core/logger.h"
#include "protocol/json_codec.h"
#include "protocol/request.h"

using nlohmann::json;

namespace chess::game::handlers {

GameplayHandler::GameplayHandler(chess::application::GameplayService&           service,
                                 const chess::application::auth::IdentityExtractor* identity,
                                 chess::net::ConnectionLookup                   lookup)
    : service_(service), identity_(identity), lookup_(std::move(lookup)) {}

void GameplayHandler::register_handlers(chess::net::MessageRouter& router) {
    router.register_handler("create_game",
        [this](chess::net::Connection& c, const std::string& m) { handle_create_game(c, m); });
    router.register_handler("join_game",
        [this](chess::net::Connection& c, const std::string& m) { handle_join_game(c, m); });
    router.register_handler("make_move",
        [this](chess::net::Connection& c, const std::string& m) { handle_make_move(c, m); });
    router.register_handler("resign",
        [this](chess::net::Connection& c, const std::string& m) { handle_resign(c, m); });
    router.register_handler("game_state",
        [this](chess::net::Connection& c, const std::string& m) { handle_game_state(c, m); });
    router.register_handler("quick_play",
        [this](chess::net::Connection& c, const std::string& m) { handle_quick_play(c, m); });
    router.register_handler("cancel_queue",
        [this](chess::net::Connection& c, const std::string& m) { handle_cancel_queue(c, m); });
    router.register_handler("list_games",
        [this](chess::net::Connection& c, const std::string& m) { handle_list_games(c, m); });
    router.register_handler("play_ai",
        [this](chess::net::Connection& c, const std::string& m) { handle_play_ai(c, m); });
}

void GameplayHandler::on_player_disconnect(int fd) {
    service_.on_player_disconnect(fd);
}

// ── Adapter helpers ─────────────────────────────────────────────────

chess::application::RequestContext
GameplayHandler::make_ctx(chess::net::Connection& conn) const {
    chess::application::RequestContext ctx;
    ctx.caller           = conn.handle();
    ctx.received_at_unix = static_cast<int64_t>(std::time(nullptr));
    return ctx;
}

std::unique_ptr<chess::net::SocketMessageSink>
GameplayHandler::make_caller_sink(chess::net::Connection& conn) const {
    // Direct-ctor sink — we own the caller's Connection reference for
    // the duration of this handler call, so we bypass the fd lookup
    // that a foreign-fd sink would need. See socket_message_sink.h
    // for the two constructor variants and their contract.
    return std::make_unique<chess::net::SocketMessageSink>(conn);
}

void GameplayHandler::send_error(chess::net::Connection& conn,
                                 const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

void GameplayHandler::send_auth_error(chess::net::Connection& conn,
                                      const std::string& message) {
    json err;
    err["type"]    = "error";
    err["code"]    = chess::application::auth::kAuthRequiredCode;
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

// ── create_game ─────────────────────────────────────────────────────

void GameplayHandler::handle_create_game(chess::net::Connection& conn,
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

    int time_base_sec = msg.value("time_base", 600);
    int time_inc_sec  = msg.value("time_inc", 5);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.create_game(ctx, id_res.value, time_base_sec, time_inc_sec, *sink);
}

// ── join_game ───────────────────────────────────────────────────────

void GameplayHandler::handle_join_game(chess::net::Connection& conn,
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
    service_.join_game(ctx, id_res.value, game_id, *sink);
}

// ── make_move ───────────────────────────────────────────────────────

void GameplayHandler::handle_make_move(chess::net::Connection& conn,
                                       const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    auto req = chess::protocol::codec::decode_make_move(msg);
    if (!req.has_value()) {
        send_error(conn, "Missing 'from'/'to' or invalid square / promotion");
        return;
    }

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.make_move(ctx, *req, *sink);
}

// ── resign ──────────────────────────────────────────────────────────

void GameplayHandler::handle_resign(chess::net::Connection& conn,
                                    const std::string& /*message*/) {
    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.resign(ctx, chess::protocol::ResignRequest{}, *sink);
}

// ── game_state ──────────────────────────────────────────────────────

void GameplayHandler::handle_game_state(chess::net::Connection& conn,
                                        const std::string& /*message*/) {
    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.game_state(ctx, chess::protocol::GameStateRequest{}, *sink);
}

// ── quick_play ──────────────────────────────────────────────────────

void GameplayHandler::handle_quick_play(chess::net::Connection& conn,
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

    int time_base_sec = msg.value("time_base", 600);
    int time_inc_sec  = msg.value("time_inc", 5);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.quick_play(ctx, id_res.value, time_base_sec, time_inc_sec, *sink);
}

// ── cancel_queue ────────────────────────────────────────────────────

void GameplayHandler::handle_cancel_queue(chess::net::Connection& conn,
                                          const std::string& /*message*/) {
    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.cancel_queue(ctx, *sink);
}

// ── list_games ──────────────────────────────────────────────────────

void GameplayHandler::handle_list_games(chess::net::Connection& conn,
                                        const std::string& /*message*/) {
    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.list_games(ctx, *sink);
}

// ── play_ai ─────────────────────────────────────────────────────────

void GameplayHandler::handle_play_ai(chess::net::Connection& conn,
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

    std::string difficulty = msg.value("difficulty", "medium");
    int time_base_sec      = msg.value("time_base", 600);
    int time_inc_sec       = msg.value("time_inc", 5);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.play_ai(ctx, id_res.value, difficulty, time_base_sec, time_inc_sec, *sink);
}

} // namespace chess::game::handlers
