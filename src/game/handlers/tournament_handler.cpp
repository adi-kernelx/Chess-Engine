/**
 * game/handlers/tournament_handler.cpp — see header.
 *
 * Every route is a two-liner after LLD-5.2: extract primitives from
 * the pre-parsed msg, then delegate. Auth-required routes read
 * `ctx.identity` — the pipeline guarantees it is set.
 */

#include "game/handlers/tournament_handler.h"

#include <nlohmann/json.hpp>
#include <utility>

#include "protocol/route_policy.h"

using nlohmann::json;

namespace chess::game::handlers {

using chess::application::RequestContext;
using chess::application::MessageSink;
using chess::protocol::AuthRequirement;
using chess::protocol::RoutePolicy;

TournamentHandler::TournamentHandler(
    chess::application::TournamentService& service,
    chess::net::ConnectionLookup           lookup)
    : service_(service), lookup_(std::move(lookup)) {}

void TournamentHandler::register_handlers(chess::protocol::RequestPipeline& pipeline) {
    pipeline.register_route(
        RoutePolicy{"create_tournament", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_create_tournament(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"join_tournament", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_join_tournament(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"leave_tournament", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_leave_tournament(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"start_tournament", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_start_tournament(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"report_tournament_result", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_report_tournament_result(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"set_tournament_registration", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_set_registration(c,m,s); });
    pipeline.register_route(
        RoutePolicy{"check_in_tournament_round", AuthRequirement::Required},
        [this](RequestContext& c, const json& m, MessageSink& s) { handle_check_in_round(c,m,s); });
    pipeline.register_route(
        RoutePolicy{"tournament_state"},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_tournament_state(c, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"list_tournaments"},
        [this](RequestContext& c, const json& m, MessageSink& s) {
            handle_list_tournaments(c, m, s);
        });
}

// ── auth-required routes ────────────────────────────────────────────

void TournamentHandler::handle_create_tournament(RequestContext& ctx,
                                                 const json& msg,
                                                 MessageSink& sink) {
    const std::string name = msg.value("name", std::string{});
    const int rounds    = msg.value("rounds",    4);
    const int time_base = msg.value("time_base", 300);
    const int time_inc  = msg.value("time_inc",  3);
    const int64_t registration_deadline = msg.value("registration_deadline", int64_t{0});
    const int64_t first_round_starts_at = msg.value("first_round_starts_at", int64_t{0});
    const int round_duration = msg.value("round_duration_seconds", 3600);
    service_.create_tournament(ctx, ctx.identity->player_id,
                               name, rounds, time_base, time_inc,
                               registration_deadline, first_round_starts_at,
                               round_duration, sink);
}

void TournamentHandler::handle_join_tournament(RequestContext& ctx,
                                               const json& msg,
                                               MessageSink& sink) {
    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    const int64_t tid = has_tid ? msg["tournament_id"].get<int64_t>() : 0;
    service_.join_tournament(ctx, ctx.identity->player_id, ctx.identity->elo_rating,
                             has_tid, tid, sink);
}

void TournamentHandler::handle_leave_tournament(RequestContext& ctx,
                                                const json& msg,
                                                MessageSink& sink) {
    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    service_.leave_tournament(ctx, ctx.identity->player_id, has_tid,
        has_tid ? msg["tournament_id"].get<int64_t>() : 0, sink);
}

void TournamentHandler::handle_start_tournament(RequestContext& ctx,
                                                const json& msg,
                                                MessageSink& sink) {
    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    const int64_t tid = has_tid ? msg["tournament_id"].get<int64_t>() : 0;
    service_.start_tournament(ctx, ctx.identity->player_id, has_tid, tid, sink);
}

void TournamentHandler::handle_report_tournament_result(RequestContext& ctx,
                                                        const json& msg,
                                                        MessageSink& sink) {
    const bool has_pid = msg.contains("pairing_id")
                      && msg["pairing_id"].is_number_integer();
    const int64_t pid = has_pid ? msg["pairing_id"].get<int64_t>() : 0;
    const std::string result = msg.value("result", std::string{});
    const std::string reason = msg.value("reason", std::string{});
    service_.report_tournament_result(ctx, ctx.identity->player_id,
                                      has_pid, pid, result, reason, sink);
}

void TournamentHandler::handle_set_registration(RequestContext& ctx,
                                                const json& msg, MessageSink& sink) {
    const bool has_tid = msg.contains("tournament_id") && msg["tournament_id"].is_number_integer();
    service_.set_registration(ctx, ctx.identity->player_id, has_tid,
        has_tid ? msg["tournament_id"].get<int64_t>() : 0,
        msg.value("open", false), sink);
}

void TournamentHandler::handle_check_in_round(RequestContext& ctx,
                                              const json& msg, MessageSink& sink) {
    const bool has_tid = msg.contains("tournament_id") && msg["tournament_id"].is_number_integer();
    service_.check_in_round(ctx, ctx.identity->player_id, has_tid,
        has_tid ? msg["tournament_id"].get<int64_t>() : 0,
        msg.value("round", 0), sink);
}

// ── public routes ────────────────────────────────────────────────────

void TournamentHandler::handle_tournament_state(RequestContext& ctx,
                                                const json& msg,
                                                MessageSink& sink) {
    const bool has_tid = msg.contains("tournament_id")
                      && msg["tournament_id"].is_number_integer();
    const int64_t tid = has_tid ? msg["tournament_id"].get<int64_t>() : 0;
    service_.tournament_state(ctx, has_tid, tid, sink);
}

void TournamentHandler::handle_list_tournaments(RequestContext& ctx,
                                                const json& msg,
                                                MessageSink& sink) {
    std::string status = msg.value("status", std::string{});
    int limit          = msg.value("limit", 25);
    service_.list_tournaments(ctx, status, limit, sink);
}

} // namespace chess::game::handlers
