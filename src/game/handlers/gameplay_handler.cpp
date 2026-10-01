/**
 * game/handlers/gameplay_handler.cpp — see header.
 *
 * Every route is a two-liner after LLD-5.1: pull whatever primitives
 * the service needs out of the pre-parsed `msg`, then delegate. The
 * pipeline has already checked auth and JSON parsability, so no
 * error-path noise here except for one wire-parity `send_error`
 * used by no current path (kept for future use / safety).
 */

#include "game/handlers/gameplay_handler.h"

#include <nlohmann/json.hpp>
#include <utility>

#include "core/logger.h"
#include "net/websocket.h"
#include "protocol/json_codec.h"
#include "protocol/request.h"
#include "protocol/route_policy.h"

using nlohmann::json;

namespace chess::game::handlers {

using chess::application::RequestContext;
using chess::application::MessageSink;
using chess::protocol::AuthRequirement;
using chess::protocol::RoutePolicy;

GameplayHandler::GameplayHandler(chess::application::GameplayService& service,
                                 chess::net::ConnectionLookup         lookup)
    : service_(service), lookup_(std::move(lookup)) {}

void GameplayHandler::register_handlers(chess::protocol::RequestPipeline& pipeline) {
    // Auth-required routes.
    pipeline.register_route(
        RoutePolicy{"create_game", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_create_game(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"join_game", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_join_game(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"quick_play", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_quick_play(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"play_ai", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_play_ai(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"offer_draw", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_offer_draw(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"draw_response", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_draw_response(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"offer_rematch", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_offer_rematch(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"rematch_response", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_rematch_response(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"get_pending_rematch", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_get_pending_rematch(ctx, m, s);
        });

    // Public routes (no auth): the request either carries no user
    // identity (make_move and resign — the caller's seat is implied by
    // the fd) or is a lookup that anyone can perform
    // (list_games, cancel_queue).
    pipeline.register_route(
        RoutePolicy{"make_move"},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_make_move(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"resign"},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_resign(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"game_state", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_game_state(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"get_active_game", AuthRequirement::Required},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_get_active_game(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"cancel_queue"},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_cancel_queue(ctx, m, s);
        });
    pipeline.register_route(
        RoutePolicy{"list_games"},
        [this](RequestContext& ctx, const json& m, MessageSink& s) {
            handle_list_games(ctx, m, s);
        });
}

void GameplayHandler::on_player_disconnect(int fd) {
    service_.on_player_disconnect(fd);
}

void GameplayHandler::send_error(MessageSink& sink, const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    sink.send(err.dump());
}

// ── auth-required routes ────────────────────────────────────────────

void GameplayHandler::handle_create_game(RequestContext& ctx,
                                         const json& msg,
                                         MessageSink& sink) {
    int time_base_sec = msg.value("time_base", 600);
    int time_inc_sec  = msg.value("time_inc", 5);
    service_.create_game(ctx, *ctx.identity, time_base_sec, time_inc_sec, sink);
}

void GameplayHandler::handle_join_game(RequestContext& ctx,
                                       const json& msg,
                                       MessageSink& sink) {
    const auto game_id_it = msg.find("game_id");
    if (game_id_it == msg.end() || !game_id_it->is_number_integer()) {
        send_error(sink, "Missing or invalid game_id");
        return;
    }
    const int64_t game_id = game_id_it->get<int64_t>();
    service_.join_game(ctx, *ctx.identity, game_id, sink);
}

void GameplayHandler::handle_quick_play(RequestContext& ctx,
                                        const json& msg,
                                        MessageSink& sink) {
    int time_base_sec = msg.value("time_base", 600);
    int time_inc_sec  = msg.value("time_inc", 5);
    service_.quick_play(ctx, *ctx.identity, time_base_sec, time_inc_sec, sink);
}

void GameplayHandler::handle_play_ai(RequestContext& ctx,
                                     const json& msg,
                                     MessageSink& sink) {
    std::string difficulty = msg.value("difficulty", "medium");
    int time_base_sec      = msg.value("time_base", 600);
    int time_inc_sec       = msg.value("time_inc", 5);
    service_.play_ai(ctx, *ctx.identity, difficulty, time_base_sec, time_inc_sec, sink);
}

// ── public routes ────────────────────────────────────────────────────

void GameplayHandler::handle_make_move(RequestContext& ctx,
                                       const json& msg,
                                       MessageSink& sink) {
    auto req = chess::protocol::codec::decode_make_move(msg);
    if (!req.has_value()) {
        send_error(sink, "Missing 'from'/'to' or invalid square / promotion");
        return;
    }
    service_.make_move(ctx, *req, sink);
}

void GameplayHandler::handle_resign(RequestContext& ctx,
                                    const json& /*msg*/,
                                    MessageSink& sink) {
    service_.resign(ctx, chess::protocol::ResignRequest{}, sink);
}

void GameplayHandler::handle_offer_draw(RequestContext& ctx,
                                        const json& /*msg*/,
                                        MessageSink& sink) {
    service_.offer_draw(ctx, sink);
}

void GameplayHandler::handle_draw_response(RequestContext& ctx,
                                           const json& msg,
                                           MessageSink& sink) {
    if (!msg.contains("accept") || !msg["accept"].is_boolean()) {
        send_error(sink, "Missing boolean 'accept'");
        return;
    }
    service_.respond_to_draw(ctx, msg["accept"].get<bool>(), sink);
}

void GameplayHandler::handle_offer_rematch(RequestContext& ctx,
                                           const json& msg,
                                           MessageSink& sink) {
    const int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));
    service_.offer_rematch(ctx, *ctx.identity, game_id, sink);
}

void GameplayHandler::handle_rematch_response(RequestContext& ctx,
                                              const json& msg,
                                              MessageSink& sink) {
    if (!msg.contains("accept") || !msg["accept"].is_boolean()) {
        send_error(sink, "Missing boolean 'accept'");
        return;
    }
    const int64_t game_id = msg.value("game_id", static_cast<int64_t>(0));
    service_.respond_to_rematch(
        ctx, *ctx.identity, game_id, msg["accept"].get<bool>(), sink);
}

void GameplayHandler::handle_get_pending_rematch(RequestContext& ctx,
                                                 const json& /*msg*/,
                                                 MessageSink& sink) {
    service_.get_pending_rematch(ctx, *ctx.identity, sink);
}

void GameplayHandler::handle_game_state(RequestContext& ctx,
                                        const json& /*msg*/,
                                        MessageSink& sink) {
    service_.game_state(ctx, chess::protocol::GameStateRequest{}, sink);
}

void GameplayHandler::handle_get_active_game(RequestContext& ctx,
                                             const json& /*msg*/,
                                             MessageSink& sink) {
    service_.get_active_game(ctx, *ctx.identity, sink);
}

void GameplayHandler::handle_cancel_queue(RequestContext& ctx,
                                          const json& /*msg*/,
                                          MessageSink& sink) {
    service_.cancel_queue(ctx, sink);
}

void GameplayHandler::handle_list_games(RequestContext& ctx,
                                        const json& /*msg*/,
                                        MessageSink& sink) {
    service_.list_games(ctx, sink);
}

} // namespace chess::game::handlers
