/**
 * game/handlers/tournament_handler.cpp — see header for role.
 *
 * Same three-line pattern established by the sibling handlers:
 *
 *     parse JSON  →  auth?  →  build ctx + sink  →  delegate to service
 *
 * The service does the business work and every send. This file has no
 * knowledge of TournamentManager or Database — it only glues the
 * transport surface to the application boundary.
 *
 * The four auth-required routes (create/join/start/report_result)
 * extract the identity here and pass primitives to the service; the
 * two public routes (tournament_state / list_tournaments) skip auth.
 * `tournament_id` and `pairing_id` are checked for integer shape here
 * and passed as (has_flag, value) pairs so the service can emit the
 * pre-refactor "Missing or invalid …" error without re-reaching the
 * JSON object.
 */

#include "game/handlers/tournament_handler.h"

#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

using nlohmann::json;

namespace chess::game::handlers {

TournamentHandler::TournamentHandler(
    chess::application::TournamentService&             service,
    const chess::application::auth::IdentityExtractor* identity,
    chess::net::ConnectionLookup                       lookup)
    : service_(service), identity_(identity), lookup_(std::move(lookup)) {}

void TournamentHandler::register_handlers(chess::net::MessageRouter& router) {
    router.register_handler("create_tournament",
        [this](chess::net::Connection& c, const std::string& m) { handle_create_tournament(c, m); });
    router.register_handler("join_tournament",
        [this](chess::net::Connection& c, const std::string& m) { handle_join_tournament(c, m); });
    router.register_handler("start_tournament",
        [this](chess::net::Connection& c, const std::string& m) { handle_start_tournament(c, m); });
    router.register_handler("tournament_state",
        [this](chess::net::Connection& c, const std::string& m) { handle_tournament_state(c, m); });
    router.register_handler("list_tournaments",
        [this](chess::net::Connection& c, const std::string& m) { handle_list_tournaments(c, m); });
    router.register_handler("report_tournament_result",
        [this](chess::net::Connection& c, const std::string& m) { handle_report_tournament_result(c, m); });
}

// ── Adapter helpers ─────────────────────────────────────────────────

chess::application::RequestContext
TournamentHandler::make_ctx(chess::net::Connection& conn) const {
    chess::application::RequestContext ctx;
    ctx.caller           = conn.handle();
    ctx.received_at_unix = static_cast<int64_t>(std::time(nullptr));
    return ctx;
}

std::unique_ptr<chess::net::SocketMessageSink>
TournamentHandler::make_caller_sink(chess::net::Connection& conn) const {
    return std::make_unique<chess::net::SocketMessageSink>(conn);
}

void TournamentHandler::send_error(chess::net::Connection& conn,
                                   const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

void TournamentHandler::send_auth_error(chess::net::Connection& conn,
                                        const std::string& message) {
    json err;
    err["type"]    = "error";
    err["code"]    = chess::application::auth::kAuthRequiredCode;
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

// ── create_tournament ───────────────────────────────────────────────

void TournamentHandler::handle_create_tournament(chess::net::Connection& conn,
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

    const std::string name = msg.value("name", std::string{});
    const int rounds    = msg.value("rounds",    4);
    const int time_base = msg.value("time_base", 300);
    const int time_inc  = msg.value("time_inc",  3);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.create_tournament(ctx, id_res.value.player_id,
                               name, rounds, time_base, time_inc, *sink);
}

// ── join_tournament ─────────────────────────────────────────────────

void TournamentHandler::handle_join_tournament(chess::net::Connection& conn,
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

    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    const int64_t tid = has_tid ? msg["tournament_id"].get<int64_t>() : 0;

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.join_tournament(ctx, id_res.value.player_id, id_res.value.elo_rating,
                             has_tid, tid, *sink);
}

// ── start_tournament ────────────────────────────────────────────────

void TournamentHandler::handle_start_tournament(chess::net::Connection& conn,
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

    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    const int64_t tid = has_tid ? msg["tournament_id"].get<int64_t>() : 0;

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.start_tournament(ctx, id_res.value.player_id, has_tid, tid, *sink);
}

// ── tournament_state (public) ───────────────────────────────────────

void TournamentHandler::handle_tournament_state(chess::net::Connection& conn,
                                                const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    const int64_t tid = has_tid ? msg["tournament_id"].get<int64_t>() : 0;

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.tournament_state(ctx, has_tid, tid, *sink);
}

// ── list_tournaments (public) ───────────────────────────────────────

void TournamentHandler::handle_list_tournaments(chess::net::Connection& conn,
                                                const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    std::string status = msg.value("status", std::string{});
    int limit          = msg.value("limit", 25);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.list_tournaments(ctx, status, limit, *sink);
}

// ── report_tournament_result ────────────────────────────────────────

void TournamentHandler::handle_report_tournament_result(chess::net::Connection& conn,
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

    const bool has_pid = msg.contains("pairing_id")
                      && msg["pairing_id"].is_number_integer();
    const int64_t pid = has_pid ? msg["pairing_id"].get<int64_t>() : 0;
    const std::string result = msg.value("result", std::string{});

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.report_tournament_result(ctx, id_res.value.player_id,
                                      has_pid, pid, result, *sink);
}

} // namespace chess::game::handlers
