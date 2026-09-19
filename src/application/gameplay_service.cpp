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
#include "storage/game_repo.h"

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
                                 SpectatorBroadcaster                   spectator_broadcaster,
                                 chess::application::ports::GameStore*  game_store)
    : rooms_(rooms), matchmaker_(matchmaker), ai_(ai),
      foreign_sender_(std::move(foreign_sender)),
      spectator_broadcaster_(std::move(spectator_broadcaster)),
      game_store_(game_store) {
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
            persist_game(room.get(), GameStatus::TIMEOUT);
        } else {
            caller_sink.send(reject);
        }
        return;
    }

    chess::protocol::MoveMadeResponse mm;
    mm.from          = req.from;
    mm.to            = req.to;
    mm.san           = result.san;
    mm.fen           = room->get_board().to_fen();
    mm.white_time_ms = result.white_time_ms;
    mm.black_time_ms = result.black_time_ms;
    if (promo != PieceType::NONE) mm.promotion = promo_wire;
    const std::string move_msg = chess::protocol::codec::encode_move_made(mm);

    caller_sink.send(move_msg);

    int opp_fd = room->get_opponent_fd(ctx.caller.fd);
    if (opp_fd >= 0) foreign_sender_(opp_fd, move_msg);
    spectator_broadcaster_(*room, move_msg);

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
        persist_game(room.get(), result.game_status);
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
    if (!room->resign(ctx.caller.fd)) {
        caller_sink.send(chess::protocol::codec::encode_error(
            {"", "Cannot resign — game is not in progress"}));
        return;
    }

    const std::string game_over = chess::protocol::codec::encode_game_over(
        {room->get_result_string(), "resignation"});

    caller_sink.send(game_over);
    int opp_fd = room->get_opponent_fd(ctx.caller.fd);
    if (opp_fd >= 0) foreign_sender_(opp_fd, game_over);
    spectator_broadcaster_(*room, game_over);

    chess::core::Logger::info("game", "GameplayService",
        "Game " + std::to_string(room->get_id()) + ": player resigned → " +
        room->get_result_string());

    persist_game(room.get(), chess::GameStatus::RESIGNATION);
}

// ── game_state ────────────────────────────────────────────────────────

void GameplayService::game_state(const RequestContext&                    ctx,
                                 const chess::protocol::GameStateRequest&,
                                 MessageSink&                             caller_sink) {
    using chess::game::RoomState;

    auto room = rooms_.find_room_by_fd(ctx.caller.fd);
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

    if (state == RoomState::FINISHED) {
        resp.result = room->get_result_string();
        resp.reason = status_to_reason(room->get_game_status());
    }

    caller_sink.send(chess::protocol::codec::encode_game_state(resp));
}

// ── create_game ───────────────────────────────────────────────────────

void GameplayService::create_game(const RequestContext&        ctx,
                                  const AuthenticatedIdentity& actor,
                                  int                          time_base_sec,
                                  int                          time_inc_sec,
                                  MessageSink&                 caller_sink) {
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
                                      tc, difficulty);

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
        room->on_disconnect(fd);
    }

    // Phase 9.1 sweep: a disconnected fd cannot receive broadcasts, and if
    // the OS recycles the fd the next owner would silently start receiving
    // move_made frames for a room they never asked to watch.
    rooms_.remove_spectator_everywhere(fd);
}

// ── trigger_ai_move (private) ─────────────────────────────────────────

void GameplayService::trigger_ai_move(std::shared_ptr<chess::game::GameRoom> room,
                                      int human_fd) {
    if (!room || !room->is_ai_game()) return;
    if (room->get_state() != chess::game::RoomState::IN_PROGRESS) return;

    const chess::Board& board = room->get_board();
    chess::game::AIDifficulty diff = room->ai_difficulty();

    chess::game::AIMove ai_move = ai_.compute_move(board, diff);

    auto result = room->submit_move_ai(ai_move.from, ai_move.to, ai_move.promotion);
    if (!result.success) {
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
    move_msg["fen"]        = room->get_board().to_fen();

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

        persist_game(room.get(), result.game_status);
    }
}

// ── persist_game (private) ────────────────────────────────────────────

void GameplayService::persist_game(chess::game::GameRoom* room,
                                   chess::GameStatus status) {
    if (!game_store_) return;                                 // Persistence disabled
    if (room->is_ai_game()) return;                           // AI games are not persisted

    int64_t w_id = room->get_db_player_id(chess::Color::WHITE);
    int64_t b_id = room->get_db_player_id(chess::Color::BLACK);
    if (w_id <= 0 || b_id <= 0) return;                       // Unauthenticated players

    chess::storage::CompletedGame game;
    game.white_id     = w_id;
    game.black_id     = b_id;
    game.white_elo    = room->get_elo(chess::Color::WHITE);
    game.black_elo    = room->get_elo(chess::Color::BLACK);
    game.result       = room->get_result_string();
    game.termination  = status_to_reason(status);
    game.time_control = room->get_time_control().to_string();
    game.started_at   = room->get_started_at_iso();
    game.ended_at     = room->get_ended_at_iso();

    auto history = room->get_move_history();
    game.move_count = static_cast<int>(history.size());

    std::string moves;
    for (size_t i = 0; i < history.size(); ++i) {
        if (i > 0) moves += ' ';
        moves += history[i].move.to_uci();
        game.think_times.push_back({
            static_cast<int>(i + 1),
            (i % 2 == 0) ? w_id : b_id,
            history[i].think_time_ms
        });
    }
    game.moves = std::move(moves);

    auto result = game_store_->save_completed_game(game);

    if (result.ok()) {
        chess::core::Logger::info("game", "GameplayService",
            "Game " + std::to_string(room->get_id()) +
            " persisted (DB id=" + std::to_string(result.game_id) +
            ", white ELO " + std::to_string(game.white_elo) + "→" +
            std::to_string(result.elo.white_new) +
            ", black ELO " + std::to_string(game.black_elo) + "→" +
            std::to_string(result.elo.black_new) + ")");
    } else {
        chess::core::Logger::error("game", "GameplayService",
            "Failed to persist game " + std::to_string(room->get_id()) +
            " (" + chess::storage::to_string(result.code) + "): " + result.error);
    }
}

} // namespace chess::application
