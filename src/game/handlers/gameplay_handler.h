/**
 * game/handlers/gameplay_handler.h — protocol adapter for the gameplay +
 * matchmaking family (LLD-2.1).
 *
 * Owns the WebSocket route registrations for its nine routes and does
 * exactly three things per request:
 *
 *   1. Parse the incoming JSON frame (raw string → nlohmann::json).
 *   2. For auth-required routes, run `IdentityExtractor::extract`.
 *   3. Build a `RequestContext` + caller `SocketMessageSink` and
 *      delegate to the matching `GameplayService` method.
 *
 * The handler does NOT hold room state, matchmaker state, or database
 * access — those are all `GameplayService`'s. It also does not build
 * business responses; the service owns every frame the client sees for
 * these routes.
 *
 * The old `GameHandler` will hold an instance of this class and forward
 * `register_handlers` to it for the routes listed below; other families
 * (query/analysis/tournaments) migrate in LLD-2.2 / 2.3 / 2.4.
 */

#pragma once

#include <memory>
#include <string>

#include "application/auth/identity_extractor.h"
#include "application/gameplay_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "net/websocket.h"

namespace chess::game::handlers {

class GameplayHandler {
public:
    /// All references must outlive the handler. `identity` may be null,
    /// in which case every auth-required route replies with the same
    /// `auth_required` frame `GameHandler::extract_identity` produced
    /// when auth was not wired.
    GameplayHandler(chess::application::GameplayService&           service,
                    const chess::application::auth::IdentityExtractor* identity,
                    chess::net::ConnectionLookup                   lookup);

    /// Register every gameplay-family route on the given router.
    void register_handlers(chess::net::MessageRouter& router);

    /// Forwarded from the transport when a connection closes.
    void on_player_disconnect(int fd);

private:
    // ── Per-route handlers ──
    void handle_create_game (chess::net::Connection& conn, const std::string& message);
    void handle_join_game   (chess::net::Connection& conn, const std::string& message);
    void handle_make_move   (chess::net::Connection& conn, const std::string& message);
    void handle_resign      (chess::net::Connection& conn, const std::string& message);
    void handle_game_state  (chess::net::Connection& conn, const std::string& message);
    void handle_quick_play  (chess::net::Connection& conn, const std::string& message);
    void handle_cancel_queue(chess::net::Connection& conn, const std::string& message);
    void handle_list_games  (chess::net::Connection& conn, const std::string& message);
    void handle_play_ai     (chess::net::Connection& conn, const std::string& message);

    // ── Adapter helpers ──

    /// Build a fully-populated RequestContext + a SocketMessageSink
    /// targeting the caller. Both flow through the shared boundary
    /// types so downstream code never sees Connection* or nlohmann::json.
    chess::application::RequestContext make_ctx(chess::net::Connection& conn) const;
    std::unique_ptr<chess::net::SocketMessageSink>
        make_caller_sink(chess::net::Connection& conn) const;

    /// Emit a bare `{ "type": "error", "message": "..." }` frame — the
    /// exact shape `GameHandler::make_error` used, so tests that grep
    /// for error text still match.
    void send_error(chess::net::Connection& conn, const std::string& message);

    /// Emit an `auth_required`-coded error, mirroring
    /// `GameHandler::send_error_code`.
    void send_auth_error(chess::net::Connection& conn, const std::string& message);

    chess::application::GameplayService&                service_;
    const chess::application::auth::IdentityExtractor*  identity_ = nullptr;
    chess::net::ConnectionLookup                        lookup_;
};

} // namespace chess::game::handlers
