/**
 * game/handlers/query_handler.h — protocol adapter for the query,
 * spectator, and replay family.
 *
 * LLD-5.2: registers on the shared `RequestPipeline`. The pipeline
 * runs parse-json + (when required) auth-extract before any route
 * function here executes, so per-route methods take the already-
 * parsed `nlohmann::json` and a populated `RequestContext`.
 *
 * Only `spectate` requires auth in this family; the other six routes
 * are public reads.
 */

#pragma once

#include <string>

#include "application/game_query_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "protocol/request_pipeline.h"

namespace chess::game::handlers {

class QueryHandler {
public:
    QueryHandler(chess::application::GameQueryService& service,
                 chess::net::ConnectionLookup          lookup);

    /// Register every query/spectator/replay route on the pipeline.
    void register_handlers(chess::protocol::RequestPipeline& pipeline);

private:
    // ── Typed per-route handlers ──
    void handle_get_profile     (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_get_leaderboard (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_get_history     (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_get_game        (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_list_live_games (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_spectate        (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_stop_spectating (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);

    chess::application::GameQueryService& service_;
    chess::net::ConnectionLookup          lookup_;
};

} // namespace chess::game::handlers
