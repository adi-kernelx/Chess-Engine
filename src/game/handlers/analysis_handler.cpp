/**
 * game/handlers/analysis_handler.cpp — see header for role.
 *
 * Parse-then-delegate for both analysis routes. No auth, no service
 * state — just JSON extraction and a call into AnalysisService.
 */

#include "game/handlers/analysis_handler.h"

#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

using nlohmann::json;

namespace chess::game::handlers {

AnalysisHandler::AnalysisHandler(chess::application::AnalysisService& service,
                                 chess::net::ConnectionLookup         lookup)
    : service_(service), lookup_(std::move(lookup)) {}

void AnalysisHandler::register_handlers(chess::net::MessageRouter& router) {
    router.register_handler("analyze_position",
        [this](chess::net::Connection& c, const std::string& m) { handle_analyze_position(c, m); });
    router.register_handler("analyze_game",
        [this](chess::net::Connection& c, const std::string& m) { handle_analyze_game(c, m); });
}

chess::application::RequestContext
AnalysisHandler::make_ctx(chess::net::Connection& conn) const {
    chess::application::RequestContext ctx;
    ctx.caller           = conn.handle();
    ctx.received_at_unix = static_cast<int64_t>(std::time(nullptr));
    return ctx;
}

std::unique_ptr<chess::net::SocketMessageSink>
AnalysisHandler::make_caller_sink(chess::net::Connection& conn) const {
    return std::make_unique<chess::net::SocketMessageSink>(conn);
}

void AnalysisHandler::send_error(chess::net::Connection& conn,
                                 const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, err.dump());
}

// ── analyze_position ────────────────────────────────────────────────

void AnalysisHandler::handle_analyze_position(chess::net::Connection& conn,
                                              const std::string& message) {
    json msg;
    try { msg = json::parse(message); }
    catch (const json::exception& e) {
        send_error(conn, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    std::string fen = msg.value("fen", std::string{});
    int depth       = msg.value("depth", 8);

    auto ctx  = make_ctx(conn);
    auto sink = make_caller_sink(conn);
    service_.analyze_position(ctx, fen, depth, *sink);
}

// ── analyze_game ────────────────────────────────────────────────────

void AnalysisHandler::handle_analyze_game(chess::net::Connection& conn,
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
    service_.analyze_game(ctx, game_id, *sink);
}

} // namespace chess::game::handlers
