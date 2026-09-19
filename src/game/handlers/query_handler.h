/**
 * game/handlers/query_handler.h — protocol adapter for the query,
 * spectator, and replay family (LLD-2.2).
 *
 * Owns the WebSocket route registrations for its seven routes and does
 * exactly three things per request:
 *
 *   1. Parse the incoming JSON frame (raw string → nlohmann::json).
 *   2. For auth-required routes, run `IdentityExtractor::extract`. Only
 *      `spectate` requires auth in this family (mandatory since the
 *      Phase 9 pre-work migration).
 *   3. Build a `RequestContext` + caller `SocketMessageSink` and
 *      delegate to the matching `GameQueryService` method.
 *
 * The handler does NOT hold room state or database access — those are
 * all `GameQueryService`'s. It also does not build business responses;
 * the service owns every frame the client sees for these routes.
 *
 * `GameHandler` holds an instance of this class and forwards
 * `register_handlers` for the seven routes below.
 */

#pragma once

#include <memory>
#include <string>

#include "application/auth/identity_extractor.h"
#include "application/game_query_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "net/websocket.h"

namespace chess::game::handlers {

class QueryHandler {
public:
    /// All references must outlive the handler. `identity` may be null,
    /// in which case `spectate` replies with `auth_required` — every
    /// other route in this family is a public read and works without it.
    QueryHandler(chess::application::GameQueryService&              service,
                 const chess::application::auth::IdentityExtractor* identity,
                 chess::net::ConnectionLookup                       lookup);

    /// Register every query/spectator/replay route on the given router.
    void register_handlers(chess::net::MessageRouter& router);

private:
    // ── Per-route handlers ──
    void handle_get_profile     (chess::net::Connection& conn, const std::string& message);
    void handle_get_leaderboard (chess::net::Connection& conn, const std::string& message);
    void handle_get_history     (chess::net::Connection& conn, const std::string& message);
    void handle_get_game        (chess::net::Connection& conn, const std::string& message);
    void handle_list_live_games (chess::net::Connection& conn, const std::string& message);
    void handle_spectate        (chess::net::Connection& conn, const std::string& message);
    void handle_stop_spectating (chess::net::Connection& conn, const std::string& message);

    // ── Adapter helpers (mirror GameplayHandler exactly) ──

    chess::application::RequestContext make_ctx(chess::net::Connection& conn) const;
    std::unique_ptr<chess::net::SocketMessageSink>
        make_caller_sink(chess::net::Connection& conn) const;

    /// Bare `{ "type": "error", "message": "..." }` — matches
    /// `GameHandler::make_error` bit-for-bit so grep-based tests still
    /// match.
    void send_error(chess::net::Connection& conn, const std::string& message);

    /// `auth_required`-coded error, mirroring
    /// `GameHandler::send_error_code`.
    void send_auth_error(chess::net::Connection& conn, const std::string& message);

    chess::application::GameQueryService&               service_;
    const chess::application::auth::IdentityExtractor*  identity_ = nullptr;
    chess::net::ConnectionLookup                        lookup_;
};

} // namespace chess::game::handlers
