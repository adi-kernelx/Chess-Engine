/**
 * game/handlers/gameplay_handler.h — protocol adapter for the gameplay +
 * matchmaking family.
 *
 * LLD-5.1 change: routes are now registered on the shared
 * `RequestPipeline` instead of directly on the router. The pipeline
 * owns the parse-json and auth-extract stages, so each route function
 * here starts from an already-parsed `nlohmann::json` and — for auth-
 * required routes — an `AuthenticatedIdentity` already sitting in
 * `ctx.identity`. This removes ~seven copies of the
 * `try/catch json::parse` + `if (!identity_) send_auth_error` +
 * `identity_->extract` block that used to live at the top of every
 * per-route method.
 *
 * The handler still owns the disconnect callback that flows to
 * `GameplayService::on_player_disconnect`.
 */

#pragma once

#include <memory>
#include <string>

#include "application/gameplay_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "protocol/request_pipeline.h"

namespace chess::game::handlers {

class GameplayHandler {
public:
    /// The pipeline reference must outlive this handler; `service` too.
    /// `lookup` is retained for the disconnect callback path.
    GameplayHandler(chess::application::GameplayService& service,
                    chess::net::ConnectionLookup         lookup);

    /// Register every gameplay-family route on the shared pipeline.
    /// Each registration is a `(RoutePolicy, TypedRouteFn)` pair.
    void register_handlers(chess::protocol::RequestPipeline& pipeline);

    /// Forwarded from the transport when a connection closes.
    void on_player_disconnect(int fd);

private:
    // ── Typed per-route handlers (all take pre-parsed msg + populated ctx) ──

    void handle_create_game (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_join_game   (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_make_move   (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_resign      (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_offer_draw  (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_draw_response(chess::application::RequestContext& ctx,
                              const nlohmann::json&               msg,
                              chess::application::MessageSink&    sink);
    void handle_offer_rematch(chess::application::RequestContext& ctx,
                              const nlohmann::json&               msg,
                              chess::application::MessageSink&    sink);
    void handle_rematch_response(chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_get_pending_rematch(chess::application::RequestContext& ctx,
                                    const nlohmann::json&               msg,
                                    chess::application::MessageSink&    sink);
    void handle_game_state  (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_get_active_game(chess::application::RequestContext& ctx,
                                const nlohmann::json&               msg,
                                chess::application::MessageSink&    sink);
    void handle_quick_play  (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_cancel_queue(chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_list_games  (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);
    void handle_play_ai     (chess::application::RequestContext& ctx,
                             const nlohmann::json&               msg,
                             chess::application::MessageSink&    sink);

    /// Emit `{ "type": "error", "message": ... }` — the pre-refactor
    /// error shape preserved for wire parity in the few routes that
    /// still surface non-pipeline error phrasings (invalid JSON is
    /// handled by the pipeline itself).
    void send_error(chess::application::MessageSink& sink,
                    const std::string&               message);

    chess::application::GameplayService& service_;
    chess::net::ConnectionLookup         lookup_;
};

} // namespace chess::game::handlers
