/**
 * application/game_query_service.cpp — see header for design.
 *
 * Every method here is a direct move from the corresponding
 * `GameHandler::handle_*` method in `src/game/game_handler.cpp`. Wire
 * bytes are preserved bit-for-bit: same DB calls, same error phrasings,
 * same log lines, same spectator-add sequencing. What changes is only
 * where the code lives — inside a class that can be constructed with a
 * fake `MessageSink` and no real database or WebSocket.
 *
 * INVARIANTS preserved from the pre-refactor code
 *   1. `spectate` refuses BEFORE calling `add_spectator` if the caller
 *      is already seated in this room (the seat check happens on the
 *      caller's own fd, not the room's roster).
 *   2. The seat/room guard against watching while playing runs BEFORE
 *      any room lookup so a caller in their own game can't accidentally
 *      detach by clicking Watch.
 *   3. `get_game`'s replay walk bails out on the first unresolvable UCI
 *      token with a `Corrupt move at ply N` error — partial payloads
 *      are never returned silently.
 */

#include "application/game_query_service.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "chess/notation.h"
#include "core/logger.h"
#include "storage/game_repo.h"
#include "storage/player_repo.h"

using nlohmann::json;

namespace chess::application {

namespace {

// Same helper the pre-refactor handler used, relocated here so the
// legacy responses keep their exact wire shape (identical to
// GameHandler::make_error).
std::string make_error_frame(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
}

// Match a UCI (from,to,promo?) triple against the current legal-move list
// to recover a fully-flagged Move — the persisted "e2e4 e7e5 …" string only
// carries squares + optional promotion, but Board::make_move needs the
// castle / en-passant / double-push flags too. The move generator sets
// those correctly, so we filter its output by (from, to, promo_type).
//
// Returns a zero-initialised Move on miss (identifiable by move.from == 0
// AND move.to == 0 — no legal chess move ever has both squares equal, so
// callers can treat that as "not found").
chess::Move resolve_legal(const chess::Board& board, const chess::Move& uci_probe) {
    const auto legal = chess::move_gen::generate_legal_moves(board);
    for (const auto& m : legal) {
        if (m.from != uci_probe.from) continue;
        if (m.to   != uci_probe.to)   continue;
        if (uci_probe.flags & chess::MoveFlags::PROMOTION) {
            if (m.promo_type != uci_probe.promo_type) continue;
        }
        return m;
    }
    return chess::Move{};
}

// Split "e2e4 e7e5 g1f3" into ["e2e4", "e7e5", "g1f3"].
std::vector<std::string> split_uci_moves(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

} // namespace

GameQueryService::GameQueryService(chess::game::RoomManager&                 rooms,
                                   chess::application::ports::PlayerQueries& queries)
    : rooms_(rooms), queries_(queries) {}

// ── get_profile ─────────────────────────────────────────────────────

void GameQueryService::get_profile(const RequestContext& /*ctx*/,
                                   const std::string&    username,
                                   int                   offset,
                                   MessageSink&          caller_sink) {
    if (username.empty()) {
        caller_sink.send(make_error_frame("Missing 'username' field"));
        return;
    }

    auto profile_res = queries_.find_player_by_username(username);
    if (!profile_res.ok) {
        caller_sink.send(make_error_frame("Profiles are not available (no database)"));
        return;
    }
    const auto& profile = profile_res.value;
    if (!profile) {
        caller_sink.send(make_error_frame("Player '" + username + "' not found"));
        return;
    }

    auto recent_res = queries_.get_player_games(profile->player_id, 20, offset);
    // Read failure here is soft — same behaviour as the pre-refactor
    // free function which logs internally and returns empty on error.
    const auto& recent = recent_res.value;

    json response;
    response["type"]         = "profile";
    response["username"]     = profile->username;
    response["elo"]          = profile->elo_rating;
    response["games_played"] = profile->games_played;
    response["wins"]         = profile->wins;
    response["losses"]       = profile->losses;
    response["draws"]        = profile->draws;

    response["recent_games"] = json::array();
    for (const auto& g : recent) {
        json entry;
        entry["game_id"]       = g.game_id;
        entry["opponent"]      = g.opponent_name;
        entry["opponent_elo"]  = g.opponent_elo;
        entry["result"]        = g.player_result;
        entry["color"]         = g.color;
        entry["termination"]   = g.termination;
        entry["started_at"]    = g.started_at;
        entry["move_count"]    = g.move_count;
        entry["time_control"]  = g.time_control;
        response["recent_games"].push_back(entry);
    }

    caller_sink.send(response.dump());
}

// ── get_leaderboard ─────────────────────────────────────────────────

void GameQueryService::get_leaderboard(const RequestContext& /*ctx*/,
                                       int                   limit,
                                       int                   offset,
                                       MessageSink&          caller_sink) {
    // Clamp limit to a reasonable maximum — same policy as pre-refactor.
    if (limit > 100) limit = 100;
    if (limit < 1)   limit = 1;

    auto entries_res = queries_.get_leaderboard(limit, offset);
    if (!entries_res.ok) {
        caller_sink.send(make_error_frame("Leaderboard is not available (no database)"));
        return;
    }
    const auto& entries = entries_res.value;

    json response;
    response["type"]    = "leaderboard";
    response["players"] = json::array();

    for (const auto& e : entries) {
        json player;
        player["rank"]         = e.rank;
        player["username"]     = e.username;
        player["elo"]          = e.elo_rating;
        player["games_played"] = e.games_played;
        player["wins"]         = e.wins;
        player["losses"]       = e.losses;
        player["draws"]        = e.draws;
        response["players"].push_back(player);
    }

    caller_sink.send(response.dump());
}

// ── get_history ─────────────────────────────────────────────────────

void GameQueryService::get_history(const RequestContext& /*ctx*/,
                                   const std::string&    username,
                                   int                   limit,
                                   MessageSink&          caller_sink) {
    if (username.empty()) {
        caller_sink.send(make_error_frame("Missing 'username' field"));
        return;
    }
    limit = std::clamp(limit, 1, 100);

    auto profile_res = queries_.find_player_by_username(username);
    if (!profile_res.ok) {
        caller_sink.send(make_error_frame("History unavailable \xE2\x80\x94 no database"));
        return;
    }
    const auto& profile = profile_res.value;
    if (!profile) {
        caller_sink.send(make_error_frame("Player not found: " + username));
        return;
    }

    auto games_res = queries_.get_player_games(profile->player_id, limit, 0);
    const auto& games = games_res.value;

    json response;
    response["type"]     = "history";
    response["username"] = profile->username;
    response["games"]    = json::array();
    for (const auto& g : games) {
        json row;
        row["game_id"]      = g.game_id;
        row["opponent"]     = g.opponent_name;
        row["opponent_elo"] = g.opponent_elo;
        row["my_color"]     = g.color;           // "w" or "b"
        row["result"]       = g.player_result;   // "w"/"l"/"d" from this player's view
        row["reason"]       = g.termination;
        row["played_at"]    = g.started_at;
        row["move_count"]   = g.move_count;
        row["time_control"] = g.time_control;
        response["games"].push_back(row);
    }

    caller_sink.send(response.dump());
}

// ── get_game (replay) ───────────────────────────────────────────────

void GameQueryService::get_game(const RequestContext& /*ctx*/,
                                int64_t               game_id,
                                MessageSink&          caller_sink) {
    if (game_id <= 0) {
        caller_sink.send(make_error_frame("Missing or invalid game_id"));
        return;
    }

    auto stored_res = queries_.find_game_by_id(game_id);
    if (!stored_res.ok) {
        caller_sink.send(make_error_frame("Replay unavailable \xE2\x80\x94 server has no database"));
        return;
    }
    if (!stored_res.value.has_value()) {
        caller_sink.send(make_error_frame("Game " + std::to_string(game_id) + " not found"));
        return;
    }
    const auto& stored = stored_res.value;

    // Load per-ply think times as {ply → ms}. Missing rows → 0 ms.
    // Read failure here is soft — falls back to an empty timeline,
    // matching the pre-refactor inline QueryResult behaviour.
    auto times_res = queries_.get_move_times_by_ply(game_id);
    std::vector<int> think_by_ply = times_res.ok ? std::move(times_res.value)
                                                 : std::vector<int>{};

    // Reconstruct every board position by replaying the persisted UCI
    // string. Bail out on malformed data with a partial payload rather
    // than crashing — a database that got corrupted mid-write should
    // surface an obvious error string, not a silent truncation.
    chess::Board board = chess::Board::starting_position();

    json positions = json::array();
    {
        json p0;
        p0["ply"] = 0;
        p0["fen"] = board.to_fen();
        positions.push_back(p0);
    }

    const auto tokens = split_uci_moves(stored->moves);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const chess::Move probe = chess::Move::from_uci(tokens[i]);
        const chess::Move legal = resolve_legal(board, probe);
        if (legal.from == 0 && legal.to == 0) {
            // Corrupt or unrecognised move — stop the replay here and
            // return what we have so the frontend can still render a
            // partial reconstruction with a clear error.
            caller_sink.send(make_error_frame("Corrupt move at ply " +
                      std::to_string(i + 1) + ": " + tokens[i]));
            return;
        }
        const std::string san = chess::notation::move_to_san(board, legal);
        const std::string from_str = chess::Board::square_to_algebraic(legal.from);
        const std::string to_str   = chess::Board::square_to_algebraic(legal.to);
        board.make_move(legal);

        json p;
        p["ply"]      = static_cast<int>(i + 1);
        p["fen"]      = board.to_fen();
        p["san"]      = san;
        p["from"]     = from_str;
        p["to"]       = to_str;
        p["think_ms"] = (i + 1 < think_by_ply.size())
                         ? think_by_ply[i + 1] : 0;
        positions.push_back(p);
    }

    json response;
    response["type"]         = "game";
    response["game_id"]      = stored->game_id;
    response["white"]        = stored->white_name;
    response["black"]        = stored->black_name;
    response["white_elo"]    = stored->white_elo;
    response["black_elo"]    = stored->black_elo;
    response["result"]       = stored->result;
    response["reason"]       = stored->termination;
    response["time_control"] = stored->time_control;
    response["started_at"]   = stored->started_at;
    response["ended_at"]     = stored->ended_at;
    response["move_count"]   = stored->move_count;
    response["positions"]    = positions;

    caller_sink.send(response.dump());
}

// ── list_live_games ─────────────────────────────────────────────────

void GameQueryService::list_live_games(const RequestContext& /*ctx*/,
                                       MessageSink&          caller_sink) {
    auto rooms = rooms_.list_active_rooms();

    json response;
    response["type"]  = "live_game_list";
    response["games"] = json::array();
    for (const auto& info : rooms) {
        json row;
        row["game_id"]         = info.id;
        row["white"]           = info.white_name;
        row["black"]           = info.black_name;
        row["time_control"]    = info.time_control;
        row["spectator_count"] = info.spectator_count;
        row["move_count"]      = info.move_count;
        response["games"].push_back(row);
    }
    caller_sink.send(response.dump());
}

// ── spectate ────────────────────────────────────────────────────────

void GameQueryService::spectate(const RequestContext& ctx,
                                const std::string&    spectator_username,
                                int64_t               game_id,
                                MessageSink&          caller_sink) {
    if (game_id == 0) {
        caller_sink.send(make_error_frame("Missing or invalid game_id"));
        return;
    }

    const int caller_fd = ctx.caller.fd;

    // You cannot spectate while seated in another game — a click on
    // "Watch" from a player's own move screen would silently detach
    // them from their board otherwise. Force an explicit resign first.
    auto own_room = rooms_.find_room_by_fd(caller_fd);
    if (own_room && own_room->get_state() != chess::game::RoomState::FINISHED) {
        caller_sink.send(make_error_frame("You are already in a game (ID: " +
                      std::to_string(own_room->get_id()) + ")"));
        return;
    }

    auto room = rooms_.find_room(static_cast<chess::GameId>(game_id));
    if (!room) {
        caller_sink.send(make_error_frame("Game " + std::to_string(game_id) + " not found"));
        return;
    }
    if (room->get_state() != chess::game::RoomState::IN_PROGRESS) {
        caller_sink.send(make_error_frame("Game is not live"));
        return;
    }

    // add_spectator refuses if the caller is one of this room's seats.
    // Distinct error message so the client can present it differently
    // from "the room is not accepting spectators."
    if (room->has_player(caller_fd)) {
        caller_sink.send(make_error_frame("You are seated in this game"));
        return;
    }
    if (!room->add_spectator(caller_fd)) {
        caller_sink.send(make_error_frame("Cannot spectate this game"));
        return;
    }

    // Full onboarding payload: FEN + moves + clocks + spectator count.
    // The client can rebuild the board exactly from here without waiting
    // for the next move_made frame.
    int white_ms = 0, black_ms = 0;
    room->get_remaining_times(white_ms, black_ms);
    auto history = room->get_move_history();

    json start;
    start["type"]            = "spectate_start";
    start["game_id"]         = room->get_id();
    start["white"]           = room->get_username(chess::Color::WHITE);
    start["black"]           = room->get_username(chess::Color::BLACK);
    start["fen"]             = room->get_board().to_fen();
    start["white_time"]      = white_ms;
    start["black_time"]      = black_ms;
    start["time_control"]    = room->get_time_control().to_string();
    start["spectator_count"] = static_cast<int>(room->spectator_count());

    start["moves"] = json::array();
    for (const auto& record : history) {
        json m;
        m["san"]      = record.san;
        m["think_ms"] = record.think_time_ms;
        start["moves"].push_back(m);
    }

    caller_sink.send(start.dump());

    chess::core::Logger::info("game", "GameHandler",
        spectator_username + " is spectating game " + std::to_string(game_id) +
        " (spectator_count=" + std::to_string(room->spectator_count()) + ")");
}

// ── stop_spectating ─────────────────────────────────────────────────

void GameQueryService::stop_spectating(const RequestContext& ctx,
                                       int64_t               game_id,
                                       MessageSink&          caller_sink) {
    if (game_id != 0) {
        auto room = rooms_.find_room(static_cast<chess::GameId>(game_id));
        if (room) room->remove_spectator(ctx.caller.fd);
    }
    json ack;
    ack["type"]    = "spectate_end";
    ack["game_id"] = game_id;
    caller_sink.send(ack.dump());
}

} // namespace chess::application
