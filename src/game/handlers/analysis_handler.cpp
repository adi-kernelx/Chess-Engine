/**
 * game/handlers/analysis_handler.cpp — see header.
 *
 * Pipeline has parsed the JSON; each route just extracts primitives
 * and delegates.
 */

#include "game/handlers/analysis_handler.h"

#include <nlohmann/json.hpp>
#include <utility>

#include "protocol/route_policy.h"

using nlohmann::json;

namespace chess::game::handlers {

using chess::application::RequestContext;
using chess::application::MessageSink;
using chess::protocol::RoutePolicy;

AnalysisHandler::AnalysisHandler(chess::application::AnalysisService& service,
                                 chess::net::ConnectionLookup         lookup)
    : service_(service), lookup_(std::move(lookup)) {}

void AnalysisHandler::register_handlers(chess::protocol::RequestPipeline& pipeline) {
    pipeline.register_route(
        RoutePolicy{"analyze_position"},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_analyze_position(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"analyze_game"},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_analyze_game(c, m, s);
        });
}

void AnalysisHandler::handle_analyze_position(RequestContext& ctx,
                                              const json& msg,
                                              MessageSink& sink) {
    std::string fen = msg.value("fen", std::string{});
    int depth       = msg.value("depth", 8);
    service_.analyze_position(ctx, fen, depth, sink);
}

void AnalysisHandler::handle_analyze_game(RequestContext& ctx,
                                          const json& msg,
                                          MessageSink& sink) {
    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));
    service_.analyze_game(ctx, game_id, sink);
}

} // namespace chess::game::handlers
