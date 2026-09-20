/**
 * game/handlers/tournament_handler.h — protocol adapter for the
 * tournaments family.
 *
 * LLD-5.2: registers on the shared `RequestPipeline`. Four auth-
 * required routes (create/join/start/report_result) and two public
 * routes (tournament_state / list_tournaments).
 */

#pragma once

#include <string>

#include "application/tournament_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "protocol/request_pipeline.h"

namespace chess::game::handlers {

class TournamentHandler {
public:
    TournamentHandler(chess::application::TournamentService& service,
                      chess::net::ConnectionLookup           lookup);

    void register_handlers(chess::protocol::RequestPipeline& pipeline);

private:
    // Auth-required
    void handle_create_tournament       (chess::application::RequestContext& ctx,
                                         const nlohmann::json&               msg,
                                         chess::application::MessageSink&    sink);
    void handle_join_tournament         (chess::application::RequestContext& ctx,
                                         const nlohmann::json&               msg,
                                         chess::application::MessageSink&    sink);
    void handle_start_tournament        (chess::application::RequestContext& ctx,
                                         const nlohmann::json&               msg,
                                         chess::application::MessageSink&    sink);
    void handle_report_tournament_result(chess::application::RequestContext& ctx,
                                         const nlohmann::json&               msg,
                                         chess::application::MessageSink&    sink);
    // Public
    void handle_tournament_state(chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_list_tournaments(chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);

    chess::application::TournamentService& service_;
    chess::net::ConnectionLookup           lookup_;
};

} // namespace chess::game::handlers
