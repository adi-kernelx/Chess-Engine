/**
 * game/handlers/query_handler.cpp — see header.
 *
 * Every route is a two-liner after LLD-5.2: pull whatever primitives
 * the service needs out of the pre-parsed `msg`, then delegate.
 */

#include "game/handlers/query_handler.h"

#include <nlohmann/json.hpp>
#include <utility>

#include "protocol/route_policy.h"

using nlohmann::json;

namespace chess::game::handlers {

using chess::application::RequestContext;
using chess::application::MessageSink;
using chess::protocol::AuthRequirement;
using chess::protocol::RoutePolicy;

QueryHandler::QueryHandler(chess::application::GameQueryService& service,
                           chess::net::ConnectionLookup          lookup)
    : service_(service), lookup_(std::move(lookup)) {}

void QueryHandler::register_handlers(chess::protocol::RequestPipeline& pipeline) {
    pipeline.register_route(
        RoutePolicy{"get_profile"},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_get_profile(c, m, s); });
    pipeline.register_route(
        RoutePolicy{"get_leaderboard"},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_get_leaderboard(c, m, s); });
    pipeline.register_route(
        RoutePolicy{"get_history"},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_get_history(c, m, s); });
    pipeline.register_route(
        RoutePolicy{"get_game"},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_get_game(c, m, s); });
    pipeline.register_route(
        RoutePolicy{"list_live_games"},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_list_live_games(c, m, s); });
    pipeline.register_route(
        RoutePolicy{"spectate", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_spectate(c, m, s); });
    pipeline.register_route(
        RoutePolicy{"stop_spectating"},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_stop_spectating(c, m, s); });
}

// ── get_profile ─────────────────────────────────────────────────────

void QueryHandler::handle_get_profile(RequestContext& ctx,
                                      const json& msg,
                                      MessageSink& sink) {
    std::string username = msg.value("username", std::string{});
    int offset           = msg.value("offset", 0);
    service_.get_profile(ctx, username, offset, sink);
}

// ── get_leaderboard ─────────────────────────────────────────────────

void QueryHandler::handle_get_leaderboard(RequestContext& ctx,
                                          const json& msg,
                                          MessageSink& sink) {
    int limit  = msg.value("limit", 50);
    int offset = msg.value("offset", 0);
    service_.get_leaderboard(ctx, limit, offset, sink);
}

// ── get_history ─────────────────────────────────────────────────────

void QueryHandler::handle_get_history(RequestContext& ctx,
                                      const json& msg,
                                      MessageSink& sink) {
    std::string username = msg.value("username", std::string{});
    int limit            = msg.value("limit", 20);
    service_.get_history(ctx, username, limit, sink);
}

// ── get_game ────────────────────────────────────────────────────────

void QueryHandler::handle_get_game(RequestContext& ctx,
                                   const json& msg,
                                   MessageSink& sink) {
    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));
    service_.get_game(ctx, game_id, sink);
}

// ── list_live_games ─────────────────────────────────────────────────

void QueryHandler::handle_list_live_games(RequestContext& ctx,
                                          const json& /*msg*/,
                                          MessageSink& sink) {
    service_.list_live_games(ctx, sink);
}

// ── spectate (auth required — identity in ctx) ──────────────────────

void QueryHandler::handle_spectate(RequestContext& ctx,
                                   const json& msg,
                                   MessageSink& sink) {
    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));
    service_.spectate(ctx, ctx.identity->username, game_id, sink);
}

// ── stop_spectating ─────────────────────────────────────────────────

void QueryHandler::handle_stop_spectating(RequestContext& ctx,
                                          const json& msg,
                                          MessageSink& sink) {
    int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));
    service_.stop_spectating(ctx, game_id, sink);
}

} // namespace chess::game::handlers
