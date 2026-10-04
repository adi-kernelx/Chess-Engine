/**
 * application/gameplay_service.cpp — see header for design.
 *
 * Every method here is a direct move from the corresponding
 * `GameHandler::handle_*` method in `src/game/game_handler.cpp`. The
 * behaviour is preserved bit-for-bit: same room-manager calls, same
 * error phrasings, same log lines, same fan-out order, same persist
 * gating. What changes is only where the code lives — inside a class
 * that can be constructed with fakes in a test, without a real
 * WebSocket, without a real Connection lookup.
 *
 * INVARIANTS this file preserves from the pre-refactor code
 *   1. Room lock is NEVER held across a send. Every send-to-fd or
 *      spectator broadcast happens after the room mutex is released.
 *   2. The caller's frame is emitted through `MessageSink`; foreign
 *      recipients (opponent, spectators) go through the injected
 *      callables. Two send paths, one for each recipient class.
 *   3. `persist_game` runs on the same worker thread that observed
 *      the terminal transition. That is the pre-refactor timing.
 *   4. AI-game and unauthenticated-seat guards on `persist_game`
 *      are unchanged.
 */

#include "application/gameplay_service.h"

#include <cassert>
#include <cstddef>
#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "chess/notation.h"
#include "core/logger.h"
#include "protocol/json_codec.h"
#include "protocol/response.h"

using nlohmann::json;

namespace chess::application {

namespace {

// Same helper the pre-refactor handler used, relocated here so legacy
// (non-codec) routes keep their exact wire shape. `type` is emitted first
// via dump()'s ordered output over the constructed object.
std::string make_error_frame(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
}

// The same status → reason mapping GameHandler::status_to_reason emits.
// Pulling it into the codec would drag GameStatus (a chess/domain type)
// into the protocol layer, which is a boundary violation this refactor
// is trying to REDUCE, not add.
std::string status_to_reason(chess::GameStatus status) {
    using chess::GameStatus;
    switch (status) {
        case GameStatus::CHECKMATE:                  return "checkmate";
        case GameStatus::STALEMATE:                  return "stalemate";
        case GameStatus::DRAW_FIFTY_MOVE:            return "fifty_move_rule";
        case GameStatus::DRAW_INSUFFICIENT_MATERIAL: return "insufficient_material";
        case GameStatus::DRAW_THREEFOLD_REPETITION:  return "threefold_repetition";
        case GameStatus::DRAW_AGREEMENT:             return "draw_agreement";
        case GameStatus::RESIGNATION:                return "resignation";
        case GameStatus::TIMEOUT:                    return "timeout";
        case GameStatus::ABANDONMENT:                return "abandonment";
        default:                                     return "unknown";
    }
}

chess::Square parse_square(const std::string& sq_str) {
    if (sq_str.size() != 2) return chess::NO_SQUARE;
    int file = sq_str[0] - 'a';
    int rank = sq_str[1] - '1';
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return chess::NO_SQUARE;
    return chess::make_square(rank, file);
}

} // namespace

GameplayService::GameplayService(chess::game::RoomManager&              rooms,
                                 chess::game::Matchmaker&               matchmaker,
                                 chess::game::AIPlayer&                 ai,
                                 ForeignSender                          foreign_sender,
                                 SpectatorBroadcaster                   spectator_broadcaster)
    : rooms_(rooms), matchmaker_(matchmaker), ai_(ai),
      foreign_sender_(std::move(foreign_sender)),
      spectator_broadcaster_(std::move(spectator_broadcaster)) {
    // Both callables MUST be non-null. A default-constructed std::function
    // would let a routine silently drop a foreign notification — the exact
    // silent-failure mode LLD-1's generation guard also tries to prevent.
    assert(foreign_sender_ && "GameplayService requires a foreign sender");
    assert(spectator_broadcaster_ && "GameplayService requires a spectator broadcaster");
}

// ── make_move ─────────────────────────────────────────────────────────

void GameplayService::make_move(const RequestContext&                    ctx,
                                const chess::protocol::MakeMoveRequest&  req,
                                MessageSink&                             caller_sink) {
    using chess::PieceType;
    using chess::GameStatus;

    chess::Square from_sq = parse_square(req.from);
    chess::Square to_sq   = parse_square(req.to);
    if (from_sq == chess::NO_SQUARE || to_sq == chess::NO_SQUARE) {
        caller_sink.send(chess::protocol::codec::encode_error(
            {"", "Invalid square: '" + req.from + "' or '" + req.to + "'"}));
        return;
    }

    PieceType promo = PieceType::NONE;
    std::string promo_wire;
    if (req.promotion.has_value()) {
        promo_wire = std::string(1, *req.promotion);
        switch (*req.promotion) {
            case 'q': promo = PieceType::QUEEN;  break;
            case 'r': promo = PieceType::ROOK;   break;
            case 'b': promo = PieceType::BISHOP; break;
            case 'n': promo = PieceType::KNIGHT; break;
            default: break;   // codec guarantees this can't happen
        }
    }

    auto room = rooms_.find_room_by_fd(ctx.caller.fd);
    if (!room) {
        caller_sink.send(chess::protocol::codec::encode_error(
            {"", "You are not in a game"}));
        return;
    }

    auto result = room->submit_move(ctx.caller.fd, from_sq, to_sq, promo);

    if (!result.success) {
        const std::string reject = chess::protocol::codec::encode_move_rejected(
            {result.error});

        if (result.game_status == GameStatus::TIMEOUT) {
            const std::string game_over = chess::protocol::codec::encode_game_over(
                {room->get_result_string(), "timeout"});

            caller_sink.send(reject);
            caller_sink.send(game_over);

            int opp_fd = room->get_opponent_fd(ctx.caller.fd);
            if (opp_fd >= 0) foreign_sender_(opp_fd, game_over);
            spectator_broadcaster_(*room, game_over);
            // LLD-4.2: persistence runs via `GameCompletionService`,
            // fired from the `GameCompleted` event emitted by
            // `finish_game` inside `submit_move`'s critical section.
        } else {
            caller_sink.send(reject);
        }
        return;
    }

    chess::protocol::MoveMadeResponse mm;
    mm.from          = req.from;
    mm.to            = req.to;
    mm.san           = result.san;
    mm.fen           = result.fen;
    mm.white_time_ms = result.white_time_ms;
    mm.black_time_ms = result.black_time_ms;
    mm.legal_moves   = result.legal_moves;
    if (promo != PieceType::NONE) mm.promotion = promo_wire;
    const std::string move_msg = chess::protocol::codec::encode_move_made(mm);

    caller_sink.send(move_msg);

    int opp_fd = room->get_opponent_fd(ctx.caller.fd);
    if (opp_fd >= 0) foreign_sender_(opp_fd, move_msg);
    spectator_broadcaster_(*room, move_msg);

    if (result.draw_offer_declined && opp_fd >= 0) {
        json declined;
        declined["type"] = "draw_declined";
        declined["reason"] = "move_made";
        foreign_sender_(opp_fd, declined.dump());
    }

    if (result.game_status != GameStatus::ONGOING) {
        const std::string game_over = chess::protocol::codec::encode_game_over(
            {room->get_result_string(), status_to_reason(result.game_status)});
        caller_sink.send(game_over);
        if (opp_fd >= 0) foreign_sender_(opp_fd, game_over);
        spectator_broadcaster_(*room, game_over);

        chess::core::Logger::info("game", "GameplayService",
            "Game " + std::to_string(room->get_id()) + " ended: " +
            room->get_result_string() + " (" +
            status_to_reason(result.game_status) + ")");
        // LLD-4.2: persistence via GameCompletionService listener.
    }

    if (result.game_status == GameStatus::ONGOING && room->is_ai_game()) {
        trigger_ai_move(room, ctx.caller.fd);
    }
}

// ── resign ────────────────────────────────────────────────────────────

void GameplayService::resign(const RequestContext&                 ctx,
                             const chess::protocol::ResignRequest&,
                             MessageSink&                          caller_sink) {
    auto room = rooms_.find_room_by_fd(ctx.caller.fd);
    if (!room) {
        caller_sink.send(chess::protocol::codec::encode_error(
            {"", "You are not in a game"}));
        return;
    }
    const int opponent_fd = room->get_opponent_fd(ctx.caller.fd);
    const bool resigned = room->resign(ctx.caller.fd,
        [&, room, opponent_fd]() {
            const std::string game_over = chess::protocol::codec::encode_game_over(
                {room->get_result_string(), "resignation"});
            caller_sink.send(game_over);
            if (opponent_fd >= 0) foreign_sender_(opponent_fd, game_over);
            spectator_broadcaster_(*room, game_over);
        });
    if (!resigned) {
        caller_sink.send(chess::protocol::codec::encode_error(
            {"", "Cannot resign — game is not in progress"}));
        return;
    }

    chess::core::Logger::info("game", "GameplayService",
        "Game " + std::to_string(room->get_id()) + ": player resigned → " +
        room->get_result_string());
    // LLD-4.2: persistence via GameCompletionService listener.
}

void GameplayService::offer_draw(const RequestContext& ctx,
                                 MessageSink& caller_sink) {
    auto room = rooms_.find_room_by_fd(ctx.caller.fd);
    if (!room) {
        caller_sink.send(make_error_frame("You are not in a game"));
        return;
    }

    std::string error;
    if (!room->offer_draw(ctx.caller.fd, error)) {
        caller_sink.send(make_error_frame(error));
        return;
    }

    json sent;
    sent["type"] = "draw_offer_sent";
    caller_sink.send(sent.dump());

    const int opponent_fd = room->get_opponent_fd(ctx.caller.fd);
    if (opponent_fd >= 0) {
        json offered;
        offered["type"] = "draw_offered";
        const auto offerer_color = room->draw_offer_from();
        offered["from"] = room->get_username(offerer_color);
        foreign_sender_(opponent_fd, offered.dump());
    }
}

void GameplayService::respond_to_draw(const RequestContext& ctx, bool accept,
                                      MessageSink& caller_sink) {
    auto room = rooms_.find_room_by_fd(ctx.caller.fd);
    if (!room) {
        caller_sink.send(make_error_frame("You are not in a game"));
        return;
    }
    const auto expired_offer = room->expire_draw_offer(
        std::chrono::milliseconds(DRAW_OFFER_TTL_MS));
    if (expired_offer != chess::Color::NONE) {
        json resolved;
        resolved["type"] = "draw_offer_resolved";
        resolved["accepted"] = false;
        resolved["reason"] = "expired";
        caller_sink.send(resolved.dump());

        const int offerer_fd = room->get_player_fd(expired_offer);
        if (offerer_fd >= 0 && offerer_fd != ctx.caller.fd) {
            json expired;
            expired["type"] = "draw_declined";
            expired["reason"] = "expired";
            foreign_sender_(offerer_fd, expired.dump());
        }
        return;
    }
    const int opponent_fd = room->get_opponent_fd(ctx.caller.fd);
    std::string error;
    if (!room->respond_to_draw(ctx.caller.fd, accept, error)) {
        caller_sink.send(make_error_frame(error));
        return;
    }

    if (!accept) {
        json resolved;
        resolved["type"] = "draw_offer_resolved";
        resolved["accepted"] = false;
        caller_sink.send(resolved.dump());
        if (opponent_fd >= 0) {
            json declined;
            declined["type"] = "draw_declined";
            declined["reason"] = "declined";
            foreign_sender_(opponent_fd, declined.dump());
        }
        return;
    }

    const std::string game_over = chess::protocol::codec::encode_game_over(
        {"1/2-1/2", "draw_agreement"});
    caller_sink.send(game_over);
    if (opponent_fd >= 0) foreign_sender_(opponent_fd, game_over);
    spectator_broadcaster_(*room, game_over);
}

void GameplayService::offer_rematch(const RequestContext& ctx,
                                    const AuthenticatedIdentity& actor,
                                    int64_t game_id,
                                    MessageSink& caller_sink) {
    if (game_id <= 0) {
        caller_sink.send(make_error_frame("Missing or invalid game_id"));
        return;
    }
    auto room = rooms_.find_room(static_cast<chess::GameId>(game_id));
    if (!room || !room->has_player(ctx.caller.fd)
        || (room->get_db_player_id(chess::Color::WHITE) != actor.player_id
            && room->get_db_player_id(chess::Color::BLACK) != actor.player_id)) {
        caller_sink.send(make_error_frame("Finished game not found for this player"));
        return;
    }

    const int64_t white_id = room->get_db_player_id(chess::Color::WHITE);
    const int64_t black_id = room->get_db_player_id(chess::Color::BLACK);
    if (rooms_.find_room_by_db_player(white_id)
        || rooms_.find_room_by_db_player(black_id)) {
        caller_sink.send(make_error_frame("A player is already in another game"));
        return;
    }

    std::string error;
    if (!room->offer_rematch(ctx.caller.fd, error)) {
        caller_sink.send(make_error_frame(error));
        return;
    }

    json sent;
    sent["type"] = "rematch_offer_sent";
    sent["game_id"] = game_id;
    sent["expires_in_ms"] = REMATCH_OFFER_TTL_MS;
    caller_sink.send(sent.dump());

    const int opponent_fd = room->get_opponent_fd(ctx.caller.fd);
    if (opponent_fd >= 0) {
        json offered;
        offered["type"] = "rematch_offered";
        offered["game_id"] = game_id;
        offered["from"] = actor.username;
        offered["expires_in_ms"] = REMATCH_OFFER_TTL_MS;
        foreign_sender_(opponent_fd, offered.dump());
    }
}

void GameplayService::respond_to_rematch(const RequestContext& ctx,
                                         const AuthenticatedIdentity& actor,
                                         int64_t game_id, bool accept,
                                         MessageSink& caller_sink) {
    if (game_id <= 0) {
        caller_sink.send(make_error_frame("Missing or invalid game_id"));
        return;
    }
    auto old_room = rooms_.find_room(static_cast<chess::GameId>(game_id));
    if (!old_room || !old_room->has_player(ctx.caller.fd)
        || (old_room->get_db_player_id(chess::Color::WHITE) != actor.player_id
            && old_room->get_db_player_id(chess::Color::BLACK) != actor.player_id)) {
        caller_sink.send(make_error_frame("Finished game not found for this player"));
        return;
    }

    const auto pending_from = old_room->rematch_offer_from();
    const int offerer_fd = old_room->get_player_fd(pending_from);

    if (accept) {
        const int64_t white_id = old_room->get_db_player_id(chess::Color::WHITE);
        const int64_t black_id = old_room->get_db_player_id(chess::Color::BLACK);
        if (rooms_.find_room_by_db_player(white_id)
            || rooms_.find_room_by_db_player(black_id)) {
            chess::Color discarded = chess::Color::NONE;
            std::string discard_error;
            (void)old_room->respond_to_rematch(
                ctx.caller.fd, false, discarded, discard_error);
            caller_sink.send(make_error_frame(
                "Rematch unavailable because a player joined another game"));
            if (offerer_fd >= 0) {
                json declined;
                declined["type"] = "rematch_declined";
                declined["reason"] = "unavailable";
                foreign_sender_(offerer_fd, declined.dump());
            }
            return;
        }
    }

    chess::Color offerer = chess::Color::NONE;
    std::string error;
    if (!old_room->respond_to_rematch(
            ctx.caller.fd, accept, offerer, error)) {
        caller_sink.send(make_error_frame(error));
        return;
    }

    if (!accept) {
        json resolved;
        resolved["type"] = "rematch_offer_resolved";
        resolved["accepted"] = false;
        resolved["reason"] = "declined";
        caller_sink.send(resolved.dump());
        if (offerer_fd >= 0) {
            json declined;
            declined["type"] = "rematch_declined";
            declined["reason"] = "declined";
            foreign_sender_(offerer_fd, declined.dump());
        }
        return;
    }

    // A rematch alternates colors while preserving the prior time control.
    // The new room receives fresh local IDs but retains durable player IDs,
    // usernames, and ELO snapshots for auth/reconnect/persistence.
    const int old_white_fd = old_room->get_player_fd(chess::Color::WHITE);
    const int old_black_fd = old_room->get_player_fd(chess::Color::BLACK);
    if (old_white_fd < 0 || old_black_fd < 0
        || !old_room->is_connected(chess::Color::WHITE)
        || !old_room->is_connected(chess::Color::BLACK)) {
        caller_sink.send(make_error_frame("Both players must remain connected for a rematch"));
        return;
    }

    const auto tc = old_room->get_time_control();
    auto new_room = rooms_.create_room(
        next_player_id_.fetch_add(1),
        old_room->get_username(chess::Color::BLACK), old_black_fd, tc,
        old_room->get_db_player_id(chess::Color::BLACK),
        old_room->get_elo(chess::Color::BLACK));
    if (!new_room->join(
            next_player_id_.fetch_add(1),
            old_room->get_username(chess::Color::WHITE), old_white_fd,
            old_room->get_db_player_id(chess::Color::WHITE),
            old_room->get_elo(chess::Color::WHITE))) {
        rooms_.remove_room(new_room->get_id());
        caller_sink.send(make_error_frame("Could not start the rematch"));
        return;
    }

    int white_ms = 0, black_ms = 0;
    new_room->get_remaining_times(white_ms, black_ms);
    auto started = [&](const char* color, const std::string& opponent) {
        json frame;
        frame["type"] = "rematch_started";
        frame["game_id"] = new_room->get_id();
        frame["color"] = color;
        frame["opponent"] = opponent;
        frame["white_time"] = white_ms;
        frame["black_time"] = black_ms;
        frame["time_base"] = tc.base_time_ms / 1000;
        frame["time_inc"] = tc.increment_ms / 1000;
        frame["ai_game"] = false;
        return frame.dump();
    };

    const std::string white_start = started(
        "white", old_room->get_username(chess::Color::WHITE));
    const std::string black_start = started(
        "black", old_room->get_username(chess::Color::BLACK));
    if (ctx.caller.fd == old_black_fd) {
        caller_sink.send(white_start);
        foreign_sender_(old_white_fd, black_start);
    } else {
        caller_sink.send(black_start);
        foreign_sender_(old_black_fd, white_start);
    }

    chess::core::Logger::info("game", "GameplayService",
        "Rematch of game " + std::to_string(game_id) + " started as game "
        + std::to_string(new_room->get_id()));
}

void GameplayService::get_pending_rematch(const RequestContext& ctx,
                                          const AuthenticatedIdentity& actor,
                                          MessageSink& caller_sink) {
    std::shared_ptr<chess::game::GameRoom> selected;
    chess::Color selected_actor_color = chess::Color::NONE;

    for (const auto& room : rooms_.rooms_snapshot()) {
        if (!room || room->get_state() != chess::game::RoomState::FINISHED
            || room->rematch_offer_from() == chess::Color::NONE) continue;

        chess::Color actor_color = chess::Color::NONE;
        if (room->get_db_player_id(chess::Color::WHITE) == actor.player_id) {
            actor_color = chess::Color::WHITE;
        } else if (room->get_db_player_id(chess::Color::BLACK) == actor.player_id) {
            actor_color = chess::Color::BLACK;
        }
        if (actor_color == chess::Color::NONE) continue;
        if (!selected || room->get_id() > selected->get_id()) {
            selected = room;
            selected_actor_color = actor_color;
        }
    }

    json response;
    response["type"] = "pending_rematch";
    if (!selected) {
        response["offer"] = nullptr;
        caller_sink.send(response.dump());
        return;
    }

    // Navigation can replace the game screen without closing the WebSocket;
    // a refresh can also replace the descriptor. Authenticated durable
    // identity safely restores the finished-room seat for this response.
    if (!selected->on_reconnect_db_player(actor.player_id, ctx.caller.fd, ctx.caller.generation)) {
        response["offer"] = nullptr;
        caller_sink.send(response.dump());
        return;
    }

    const auto offerer = selected->rematch_offer_from();
    response["offer"] = {
        {"game_id", selected->get_id()},
        {"from", selected->get_username(offerer)},
        {"role", offerer == selected_actor_color ? "sender" : "recipient"},
        {"expires_in_ms", REMATCH_OFFER_TTL_MS}
    };
    caller_sink.send(response.dump());
}

// ── game_state ────────────────────────────────────────────────────────

void GameplayService::game_state(const RequestContext&                    ctx,
                                 const chess::protocol::GameStateRequest&,
                                 MessageSink&                             caller_sink) {
    using chess::game::RoomState;

    std::shared_ptr<chess::game::GameRoom> room;
    bool reconnected = false;
    if (ctx.identity.has_value()) {
        room = rooms_.find_room_by_db_player(ctx.identity->player_id);
        if (room) {
            const int prior_fd = room->get_player_fd(
                room->get_db_player_id(chess::Color::WHITE) == ctx.identity->player_id
                    ? chess::Color::WHITE : chess::Color::BLACK);
            const bool bound = room->on_reconnect_db_player(
                ctx.identity->player_id, ctx.caller.fd, ctx.caller.generation);
            // An idempotent request on the current socket is not a reconnect
            // event and must not repeatedly notify the opponent.
            reconnected = bound && prior_fd != ctx.caller.fd;
            if (!bound) room.reset();
        }
    } else {
        room = rooms_.find_room_by_fd(ctx.caller.fd);
    }
    if (!room) {
        caller_sink.send(chess::protocol::codec::encode_error(
            {"", "You are not in a game"}));
        return;
    }

    int white_ms = 0, black_ms = 0;
    room->get_remaining_times(white_ms, black_ms);
    auto history = room->get_move_history();

    chess::protocol::GameStateResponse resp;
    resp.game_id       = room->get_id();
    resp.white_username = room->get_username(chess::Color::WHITE);
    resp.black_username = room->get_username(chess::Color::BLACK);
    resp.fen           = room->get_board().to_fen();
    resp.white_time_ms = white_ms;
    resp.black_time_ms = black_ms;

    const auto state = room->get_state();
    switch (state) {
        case RoomState::WAITING:     resp.state = "waiting";     break;
        case RoomState::IN_PROGRESS: resp.state = "in_progress"; break;
        case RoomState::FINISHED:    resp.state = "finished";    break;
    }

    resp.moves.reserve(history.size());
    for (const auto& record : history) {
        resp.moves.push_back({record.san, record.think_time_ms});
    }
    if (state == RoomState::IN_PROGRESS) {
        resp.legal_moves = room->get_legal_moves_uci();
        const auto offerer = room->draw_offer_from();
        if (offerer == chess::Color::WHITE) resp.draw_offer_from = "white";
        else if (offerer == chess::Color::BLACK) resp.draw_offer_from = "black";
    }

    if (state == RoomState::FINISHED) {
        resp.result = room->get_result_string();
        resp.reason = status_to_reason(room->get_game_status());
    }

    caller_sink.send(chess::protocol::codec::encode_game_state(resp));

    if (reconnected) {
        const int opponent_fd = room->get_opponent_fd(ctx.caller.fd);
        if (opponent_fd >= 0) {
            json notice;
            notice["type"] = "opponent_reconnected";
            foreign_sender_(opponent_fd, notice.dump());
        }
    }
}

void GameplayService::get_active_game(const RequestContext& ctx,
                                      const AuthenticatedIdentity& actor,
                                      MessageSink& caller_sink) {
    json response;
    response["type"] = "active_game";

    auto room = rooms_.find_room_by_db_player(actor.player_id);
    if (!room || room->get_state() == chess::game::RoomState::FINISHED) {
        response["game"] = nullptr;
        caller_sink.send(response.dump());
        return;
    }

    const bool is_white = room->get_db_player_id(chess::Color::WHITE) == actor.player_id;
    const chess::Color actor_color = is_white ? chess::Color::WHITE : chess::Color::BLACK;
    const int prior_fd = room->get_player_fd(actor_color);

    // Discovery and recovery are intentionally one atomic user-level action:
    // if the browser reached Replays/Game through a replaced WebSocket, merely
    // reporting the room would leave its seat bound to the dead descriptor.
    if (!room->on_reconnect_db_player(actor.player_id, ctx.caller.fd, ctx.caller.generation)) {
        response["game"] = nullptr;
        caller_sink.send(response.dump());
        return;
    }

    const chess::Color opponent_color = is_white ? chess::Color::BLACK : chess::Color::WHITE;
    int white_ms = 0;
    int black_ms = 0;
    room->get_remaining_times(white_ms, black_ms);
    const auto& tc = room->get_time_control();
    const char* state = room->get_state() == chess::game::RoomState::WAITING
        ? "waiting" : "in_progress";

    response["game"] = {
        {"game_id", room->get_id()},
        {"color", is_white ? "white" : "black"},
        {"opponent", room->get_username(opponent_color)},
        {"white_time", white_ms},
        {"black_time", black_ms},
        {"time_base", tc.base_time_ms / 1000},
        {"time_inc", tc.increment_ms / 1000},
        {"ai_game", room->is_ai_game()},
        {"tournament_id", room->tournament_id()},
        {"pairing_id", room->pairing_id()},
        {"state", state}
    };
    caller_sink.send(response.dump());

    if (prior_fd != ctx.caller.fd) {
        const int opponent_fd = room->get_player_fd(opponent_color);
        if (opponent_fd >= 0) {
            json notice;
            notice["type"] = "opponent_reconnected";
            foreign_sender_(opponent_fd, notice.dump());
        }
    }
}

// ── create_game ───────────────────────────────────────────────────────

void GameplayService::create_game(const RequestContext&        ctx,
                                  const AuthenticatedIdentity& actor,
                                  int                          time_base_sec,
                                  int                          time_inc_sec,
                                  MessageSink&                 caller_sink) {
    if (rooms_.is_tournament_player_reserved(actor.player_id)) {
        caller_sink.send(make_error_frame("You are checked in for a tournament round"));
        return;
    }
    auto existing = rooms_.find_room_by_fd(ctx.caller.fd);
    if (existing && existing->get_state() != chess::game::RoomState::FINISHED) {
        caller_sink.send(make_error_frame("You are already in a game (ID: " +
            std::to_string(existing->get_id()) + ")"));
        return;
    }

    chess::PlayerId pid = next_player_id_.fetch_add(1);
    chess::game::TimeControl tc(time_base_sec * 1000, time_inc_sec * 1000);

    auto room = rooms_.create_room(pid, actor.username, ctx.caller.fd, tc,
                                   actor.player_id, actor.elo_rating);

    json response;
    response["type"]    = "game_created";
    response["game_id"] = room->get_id();
    response["color"]   = "white";

    chess::core::Logger::info("game", "GameplayService",
        actor.username + " created game " + std::to_string(room->get_id()) +
        " (" + tc.to_string() + ")");

    caller_sink.send(response.dump());
}

// ── join_game ─────────────────────────────────────────────────────────

void GameplayService::join_game(const RequestContext&        ctx,
                                const AuthenticatedIdentity& actor,
                                int64_t                      game_id,
                                MessageSink&                 caller_sink) {
    if (rooms_.is_tournament_player_reserved(actor.player_id)) {
        caller_sink.send(make_error_frame("You are checked in for a tournament round"));
        return;
    }
    if (game_id <= 0) {
        caller_sink.send(make_error_frame("Missing or invalid game_id"));
        return;
    }

    auto existing = rooms_.find_room_by_fd(ctx.caller.fd);
    if (existing && existing->get_state() != chess::game::RoomState::FINISHED) {
        caller_sink.send(make_error_frame("You are already in a game (ID: " +
            std::to_string(existing->get_id()) + ")"));
        return;
    }

    auto room = rooms_.find_room(static_cast<chess::GameId>(game_id));
    if (!room) {
        caller_sink.send(make_error_frame("Game " + std::to_string(game_id) +
            " not found"));
        return;
    }

    chess::PlayerId pid = next_player_id_.fetch_add(1);

    if (!room->join(pid, actor.username, ctx.caller.fd,
                    actor.player_id, actor.elo_rating)) {
        caller_sink.send(make_error_frame("Cannot join game " +
            std::to_string(game_id) + " — it may be full or already started"));
        return;
    }

    int white_ms = 0, black_ms = 0;
    room->get_remaining_times(white_ms, black_ms);

    json join_response;
    join_response["type"]        = "game_joined";
    join_response["game_id"]     = game_id;
    join_response["color"]       = "black";
    join_response["white_time"]  = white_ms;
    join_response["black_time"]  = black_ms;

    caller_sink.send(join_response.dump());

    int white_fd = room->get_player_fd(chess::Color::WHITE);
    if (white_fd >= 0) {
        json start_notification;
        start_notification["type"]       = "game_start";
        start_notification["game_id"]    = game_id;
        start_notification["opponent"]   = actor.username;
        start_notification["color"]      = "white";
        start_notification["white_time"] = white_ms;
        start_notification["black_time"] = black_ms;
        foreign_sender_(white_fd, start_notification.dump());
    }

    chess::core::Logger::info("game", "GameplayService",
        actor.username + " joined game " + std::to_string(game_id));
}

// ── quick_play ────────────────────────────────────────────────────────

void GameplayService::quick_play(const RequestContext&        ctx,
                                 const AuthenticatedIdentity& actor,
                                 int                          time_base_sec,
                                 int                          time_inc_sec,
                                 MessageSink&                 caller_sink) {
    if (rooms_.is_tournament_player_reserved(actor.player_id)) {
        caller_sink.send(make_error_frame("You are checked in for a tournament round"));
        return;
    }
    auto existing = rooms_.find_room_by_fd(ctx.caller.fd);
    if (existing && existing->get_state() != chess::game::RoomState::FINISHED) {
        caller_sink.send(make_error_frame("You are already in a game (ID: " +
            std::to_string(existing->get_id()) + ")"));
        return;
    }
    if (matchmaker_.is_queued(ctx.caller.fd)) {
        caller_sink.send(make_error_frame("You are already in the matchmaking queue"));
        return;
    }

    chess::PlayerId pid = next_player_id_.fetch_add(1);
    chess::game::TimeControl tc(time_base_sec * 1000, time_inc_sec * 1000);

    matchmaker_.enqueue(ctx.caller.fd, pid, actor.username,
                        actor.elo_rating, tc, actor.player_id);

    // Try to match immediately. If we got matched, notifications are
    // sent via the match callback wired in main.cpp; if not, we send a
    // "queued" confirmation from here.
    (void)matchmaker_.try_match();

    if (matchmaker_.is_queued(ctx.caller.fd)) {
        json response;
        response["type"]       = "queued";
        response["queue_size"] = static_cast<int>(matchmaker_.queue_size());
        caller_sink.send(response.dump());
    }
}

// ── cancel_queue ──────────────────────────────────────────────────────

void GameplayService::cancel_queue(const RequestContext& ctx,
                                   MessageSink&          caller_sink) {
    if (matchmaker_.dequeue(ctx.caller.fd)) {
        json response;
        response["type"] = "queue_cancelled";
        caller_sink.send(response.dump());
    } else {
        caller_sink.send(make_error_frame("You are not in the matchmaking queue"));
    }
}

// ── list_games ────────────────────────────────────────────────────────

void GameplayService::list_games(const RequestContext& /*ctx*/,
                                 MessageSink&          caller_sink) {
    auto open_rooms = rooms_.list_open_rooms();

    json response;
    response["type"]  = "game_list";
    response["games"] = json::array();

    for (const auto& info : open_rooms) {
        json room_info;
        room_info["game_id"]      = info.id;
        room_info["host"]         = info.white_name;
        room_info["time_control"] = info.time_control;
        response["games"].push_back(room_info);
    }

    caller_sink.send(response.dump());
}

// ── play_ai ───────────────────────────────────────────────────────────

void GameplayService::play_ai(const RequestContext&        ctx,
                              const AuthenticatedIdentity& actor,
                              const std::string&           difficulty_str,
                              int                          time_base_sec,
                              int                          time_inc_sec,
                              MessageSink&                 caller_sink) {
    if (rooms_.is_tournament_player_reserved(actor.player_id)) {
        caller_sink.send(make_error_frame("You are checked in for a tournament round"));
        return;
    }
    auto existing = rooms_.find_room_by_fd(ctx.caller.fd);
    if (existing && existing->get_state() != chess::game::RoomState::FINISHED) {
        caller_sink.send(make_error_frame("You are already in a game (ID: " +
            std::to_string(existing->get_id()) + ")"));
        return;
    }

    chess::PlayerId pid = next_player_id_.fetch_add(1);
    chess::game::TimeControl tc(time_base_sec * 1000, time_inc_sec * 1000);
    chess::game::AIDifficulty difficulty = chess::game::parse_difficulty(difficulty_str);

    auto room = rooms_.create_ai_room(pid, actor.username, ctx.caller.fd,
                                      tc, difficulty, actor.player_id,
                                      actor.elo_rating);

    int white_ms = 0, black_ms = 0;
    room->get_remaining_times(white_ms, black_ms);

    json response;
    response["type"]       = "game_start";
    response["game_id"]    = room->get_id();
    response["color"]      = "white";
    response["opponent"]   = "AI (" + chess::game::difficulty_name(difficulty) + ")";
    response["white_time"] = white_ms;
    response["black_time"] = black_ms;
    response["ai_game"]    = true;

    caller_sink.send(response.dump());

    chess::core::Logger::info("game", "GameplayService",
        actor.username + " started AI game " + std::to_string(room->get_id()) +
        " (" + chess::game::difficulty_name(difficulty) + ", " + tc.to_string() + ")");
}

// ── on_player_disconnect ──────────────────────────────────────────────

void GameplayService::on_player_disconnect(int fd) {
    matchmaker_.dequeue(fd);

    auto room = rooms_.find_room_by_fd(fd);
    if (room) {
        // Capture before GameRoom clears the transient fd to prevent OS fd
        // reuse from binding an unrelated socket to the disconnected seat.
        const int opponent_fd = room->get_opponent_fd(fd);
        room->on_disconnect(fd);
        if (room->get_state() == chess::game::RoomState::IN_PROGRESS) {
            if (opponent_fd >= 0) {
                json notice;
                notice["type"] = "opponent_disconnected";
                notice["grace_ms"] = DISCONNECT_GRACE_MS;
                foreign_sender_(opponent_fd, notice.dump());
            }
        }
    }

    // Phase 9.1 sweep: a disconnected fd cannot receive broadcasts, and if
    // the OS recycles the fd the next owner would silently start receiving
    // move_made frames for a room they never asked to watch.
    rooms_.remove_spectator_everywhere(fd);
}

void GameplayService::expire_disconnected_games() {
    const auto grace = std::chrono::milliseconds(DISCONNECT_GRACE_MS);
    const auto draw_ttl = std::chrono::milliseconds(DRAW_OFFER_TTL_MS);
    for (const auto& room : rooms_.rooms_snapshot()) {
        if (!room) continue;

        if (room->expire_on_time()) {
            const std::string game_over = chess::protocol::codec::encode_game_over(
                {room->get_result_string(), "timeout"});
            const int white_fd = room->get_player_fd(chess::Color::WHITE);
            const int black_fd = room->get_player_fd(chess::Color::BLACK);
            if (room->is_connected(chess::Color::WHITE) && white_fd >= 0) {
                foreign_sender_(white_fd, game_over);
            }
            if (room->is_connected(chess::Color::BLACK) && black_fd >= 0) {
                foreign_sender_(black_fd, game_over);
            }
            spectator_broadcaster_(*room, game_over);

            chess::core::Logger::info("game", "GameplayService",
                "Game " + std::to_string(room->get_id())
                + ": clock expired -> " + room->get_result_string());
            continue;
        }

        const auto expired_offer = room->expire_draw_offer(draw_ttl);
        if (expired_offer != chess::Color::NONE) {
            const auto offeree = expired_offer == chess::Color::WHITE
                ? chess::Color::BLACK : chess::Color::WHITE;
            const int offerer_fd = room->get_player_fd(expired_offer);
            const int offeree_fd = room->get_player_fd(offeree);
            if (offerer_fd >= 0 && room->is_connected(expired_offer)) {
                json expired;
                expired["type"] = "draw_declined";
                expired["reason"] = "expired";
                foreign_sender_(offerer_fd, expired.dump());
            }
            if (offeree_fd >= 0 && room->is_connected(offeree)) {
                json resolved;
                resolved["type"] = "draw_offer_resolved";
                resolved["accepted"] = false;
                resolved["reason"] = "expired";
                foreign_sender_(offeree_fd, resolved.dump());
            }
        }

        const auto expired_rematch = room->expire_rematch_offer(
            std::chrono::milliseconds(REMATCH_OFFER_TTL_MS));
        if (expired_rematch != chess::Color::NONE) {
            const auto offeree = expired_rematch == chess::Color::WHITE
                ? chess::Color::BLACK : chess::Color::WHITE;
            const int offerer_fd = room->get_player_fd(expired_rematch);
            const int offeree_fd = room->get_player_fd(offeree);
            if (offerer_fd >= 0 && room->is_connected(expired_rematch)) {
                json expired;
                expired["type"] = "rematch_declined";
                expired["reason"] = "expired";
                foreign_sender_(offerer_fd, expired.dump());
            }
            if (offeree_fd >= 0 && room->is_connected(offeree)) {
                json resolved;
                resolved["type"] = "rematch_offer_resolved";
                resolved["accepted"] = false;
                resolved["reason"] = "expired";
                foreign_sender_(offeree_fd, resolved.dump());
            }
        }

        if (!room->expire_disconnected(grace)) continue;

        const std::string game_over = chess::protocol::codec::encode_game_over(
            {room->get_result_string(), "abandonment"});
        const int white_fd = room->get_player_fd(chess::Color::WHITE);
        const int black_fd = room->get_player_fd(chess::Color::BLACK);
        if (room->is_connected(chess::Color::WHITE) && white_fd >= 0) {
            foreign_sender_(white_fd, game_over);
        }
        if (room->is_connected(chess::Color::BLACK) && black_fd >= 0) {
            foreign_sender_(black_fd, game_over);
        }
        spectator_broadcaster_(*room, game_over);

        chess::core::Logger::info("game", "GameplayService",
            "Game " + std::to_string(room->get_id())
            + ": disconnect grace expired → " + room->get_result_string());
    }
}

// ── trigger_ai_move (private) ─────────────────────────────────────────

void GameplayService::trigger_ai_move(std::shared_ptr<chess::game::GameRoom> room,
                                      int human_fd) {
    if (!room || !room->is_ai_game()) return;
    if (room->get_state() != chess::game::RoomState::IN_PROGRESS) return;

    const chess::Board& board = room->get_board();
    chess::game::AIDifficulty diff = room->ai_difficulty();

    int white_ms = 0, black_ms = 0;
    room->get_remaining_times(white_ms, black_ms);
    chess::game::AIMove ai_move = ai_.compute_move(board, diff,
        board.side_to_move() == chess::Color::WHITE ? white_ms : black_ms,
        room->get_time_control().increment_ms);

    auto result = room->submit_move_ai(ai_move.from, ai_move.to, ai_move.promotion);
    if (!result.success) {
        if (result.game_status == chess::GameStatus::TIMEOUT) {
            const std::string frame = json{{"type", "game_over"},
                {"result", room->get_result_string()}, {"reason", "timeout"}}.dump();
            foreign_sender_(human_fd, frame);
            spectator_broadcaster_(*room, frame);
            return;
        }
        chess::core::Logger::error("game", "GameplayService",
            "AI move failed in game " + std::to_string(room->get_id()) +
            ": " + result.error);
        return;
    }

    std::string from_str = chess::Board::square_to_algebraic(ai_move.from);
    std::string to_str   = chess::Board::square_to_algebraic(ai_move.to);

    json move_msg;
    move_msg["type"]       = "move_made";
    move_msg["from"]       = from_str;
    move_msg["to"]         = to_str;
    move_msg["san"]        = result.san;
    move_msg["white_time"] = result.white_time_ms;
    move_msg["black_time"] = result.black_time_ms;
    move_msg["fen"]        = result.fen;
    move_msg["legal_moves"] = result.legal_moves;

    if (ai_move.promotion != chess::PieceType::NONE) {
        char promo_char = 'q';
        switch (ai_move.promotion) {
            case chess::PieceType::ROOK:   promo_char = 'r'; break;
            case chess::PieceType::BISHOP: promo_char = 'b'; break;
            case chess::PieceType::KNIGHT: promo_char = 'n'; break;
            default: break;
        }
        move_msg["promotion"] = std::string(1, promo_char);
    }

    const std::string move_msg_str = move_msg.dump();
    foreign_sender_(human_fd, move_msg_str);
    spectator_broadcaster_(*room, move_msg_str);

    if (result.game_status != chess::GameStatus::ONGOING) {
        json game_over;
        game_over["type"]   = "game_over";
        game_over["result"] = room->get_result_string();
        game_over["reason"] = status_to_reason(result.game_status);

        const std::string game_over_str = game_over.dump();
        foreign_sender_(human_fd, game_over_str);
        spectator_broadcaster_(*room, game_over_str);

        chess::core::Logger::info("game", "GameplayService",
            "AI Game " + std::to_string(room->get_id()) + " ended: " +
            room->get_result_string() + " (" + status_to_reason(result.game_status) + ")");
        // LLD-4.2: persistence via GameCompletionService listener. AI games
        // are stored as unrated replay records and never update player stats.
    }
}

} // namespace chess::application
