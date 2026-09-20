/**
 * game/handlers/analysis_handler.h — protocol adapter for the analysis
 * family.
 *
 * LLD-5.2: registers on the shared `RequestPipeline`. Both routes are
 * public (no auth). The service owns every response frame.
 */

#pragma once

#include <string>

#include "application/analysis_service.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "protocol/request_pipeline.h"

namespace chess::game::handlers {

class AnalysisHandler {
public:
    AnalysisHandler(chess::application::AnalysisService& service,
                    chess::net::ConnectionLookup         lookup);

    void register_handlers(chess::protocol::RequestPipeline& pipeline);

private:
    void handle_analyze_position(chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);
    void handle_analyze_game    (chess::application::RequestContext& ctx,
                                 const nlohmann::json&               msg,
                                 chess::application::MessageSink&    sink);

    chess::application::AnalysisService& service_;
    chess::net::ConnectionLookup         lookup_;
};

} // namespace chess::game::handlers
