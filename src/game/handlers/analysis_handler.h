/**
 * game/handlers/analysis_handler.h — protocol adapter for the analysis
 * family (LLD-2.3).
 *
 * Owns the WebSocket route registrations for `analyze_position` and
 * `analyze_game`. Same three-line pattern as the sibling handlers:
 *
 *     parse JSON  →  build ctx + sink  →  delegate to service
 *
 * Both routes are public (no auth). The handler does NOT hold engine
 * state, database access, or build any response body — the service
 * owns every frame the client sees for this family.
 */

#pragma once

#include <memory>
#include <string>

#include "application/analysis_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "net/websocket.h"

namespace chess::game::handlers {

class AnalysisHandler {
public:
    AnalysisHandler(chess::application::AnalysisService& service,
                    chess::net::ConnectionLookup         lookup);

    void register_handlers(chess::net::MessageRouter& router);

private:
    void handle_analyze_position(chess::net::Connection& conn, const std::string& message);
    void handle_analyze_game    (chess::net::Connection& conn, const std::string& message);

    chess::application::RequestContext make_ctx(chess::net::Connection& conn) const;
    std::unique_ptr<chess::net::SocketMessageSink>
        make_caller_sink(chess::net::Connection& conn) const;

    /// Bare `{ "type": "error", "message": "..." }` — matches
    /// `GameHandler::make_error` bit-for-bit so grep-based tests still
    /// match.
    void send_error(chess::net::Connection& conn, const std::string& message);

    chess::application::AnalysisService& service_;
    chess::net::ConnectionLookup         lookup_;
};

} // namespace chess::game::handlers
