/**
 * game/handlers/tournament_handler.h — protocol adapter for the
 * tournaments family (LLD-2.4).
 *
 * Owns the WebSocket route registrations for the six tournament routes:
 *
 *      create_tournament / join_tournament / start_tournament   (auth)
 *      report_tournament_result                                 (auth, creator-only)
 *      tournament_state / list_tournaments                      (public)
 *
 * Same three-line pattern as the sibling handlers:
 *
 *     parse JSON  →  auth?  →  build ctx + sink  →  delegate to service
 *
 * The handler does NOT hold DB access, tournament state, or build any
 * response body — the service owns every frame the client sees for
 * these routes.
 */

#pragma once

#include <memory>
#include <string>

#include "application/auth/identity_extractor.h"
#include "application/tournament_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "net/websocket.h"

namespace chess::game::handlers {

class TournamentHandler {
public:
    /// All references must outlive the handler. `identity` may be null,
    /// in which case the four auth-required routes reply with
    /// `auth_required`; the two public routes work regardless.
    TournamentHandler(chess::application::TournamentService&             service,
                      const chess::application::auth::IdentityExtractor* identity,
                      chess::net::ConnectionLookup                       lookup);

    void register_handlers(chess::net::MessageRouter& router);

private:
    // Auth-required
    void handle_create_tournament       (chess::net::Connection& conn, const std::string& message);
    void handle_join_tournament         (chess::net::Connection& conn, const std::string& message);
    void handle_start_tournament        (chess::net::Connection& conn, const std::string& message);
    void handle_report_tournament_result(chess::net::Connection& conn, const std::string& message);

    // Public
    void handle_tournament_state(chess::net::Connection& conn, const std::string& message);
    void handle_list_tournaments(chess::net::Connection& conn, const std::string& message);

    // ── Adapter helpers (mirror the other handlers) ──

    chess::application::RequestContext make_ctx(chess::net::Connection& conn) const;
    std::unique_ptr<chess::net::SocketMessageSink>
        make_caller_sink(chess::net::Connection& conn) const;
    void send_error(chess::net::Connection& conn, const std::string& message);
    void send_auth_error(chess::net::Connection& conn, const std::string& message);

    chess::application::TournamentService&              service_;
    const chess::application::auth::IdentityExtractor*  identity_ = nullptr;
    chess::net::ConnectionLookup                        lookup_;
};

} // namespace chess::game::handlers
