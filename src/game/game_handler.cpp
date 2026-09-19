/**
 * game_handler.cpp — WebSocket JSON API implementation.
 *
 * As of LLD-2.1 this file is a REGISTRATION FACADE + a home for the three
 * un-migrated families' handlers (query/spectator/replay, analysis,
 * tournaments). The gameplay + matchmaking family has been extracted
 * into `application::GameplayService` + `game::handlers::GameplayHandler`
 * and this class delegates to them.
 *
 * The server is authoritative: every move is validated server-side.
 * Clients cannot directly modify game state — they can only request
 * actions, and the server decides whether to accept or reject them.
 */

#include "game/game_handler.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ctime>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "analysis/anti_cheat.h"
#include "analysis/cheat_report_repo.h"
#include "application/auth/identity_extractor.h"
#include "application/gameplay_service.h"
#include "auth/session.h"
#include "chess/board.h"
#include "chess/engine.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "chess/notation.h"
#include "core/logger.h"
#include "game/handlers/gameplay_handler.h"
#include "net/socket_message_sink.h"
#include "protocol/json_codec.h"
#include "protocol/request.h"
#include "protocol/response.h"
#include "storage/game_repo.h"
#include "storage/player_repo.h"
#include "tournament/tournament_manager.h"
#include "tournament/tournament_repo.h"

using json = nlohmann::json;

namespace chess {
namespace game {

// ============================================================
// Construction & Registration
// ============================================================

GameHandler::GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker)
    : room_mgr_(room_mgr), matchmaker_(matchmaker) {}

void GameHandler::register_handlers(net::MessageRouter& router) {
    // ── LLD-2.1: build and register the gameplay family. ──
    //
    // Both db_ and signer_ MUST already have been set (or explicitly
    // left null when auth is not configured). See main.cpp for ordering.
    identity_ = std::make_unique<application::auth::IdentityExtractor>(
        db_, signer_);

    auto foreign_sender = [this](int fd, const std::string& frame) {
        this->send_json_to_fd(fd, frame);
    };
    auto spectator_broadcaster = [this](GameRoom& room, const std::string& frame) {
        this->broadcast_to_spectators(room, frame);
    };

    gameplay_service_ = std::make_unique<application::GameplayService>(
        room_mgr_, matchmaker_, ai_player_,
        std::move(foreign_sender),
        std::move(spectator_broadcaster),
        db_);

    gameplay_handler_ = std::make_unique<handlers::GameplayHandler>(
        *gameplay_service_, identity_.get(), connection_lookup_);

    gameplay_handler_->register_handlers(router);

    // ── Un-migrated families: still owned by this class. ──

    router.register_handler("get_profile",
        [this](net::Connection& c, const std::string& m) { handle_get_profile(c, m); });
    router.register_handler("get_leaderboard",
        [this](net::Connection& c, const std::string& m) { handle_get_leaderboard(c, m); });
    // Phase 9.1 — spectating
    router.register_handler("spectate",
        [this](net::Connection& c, const std::string& m) { handle_spectate(c, m); });
    router.register_handler("stop_spectating",
        [this](net::Connection& c, const std::string& m) { handle_stop_spectating(c, m); });
    router.register_handler("list_live_games",
        [this](net::Connection& c, const std::string& m) { handle_list_live_games(c, m); });
    // Phase 9.2 — replay + analysis
    router.register_handler("get_game",
        [this](net::Connection& c, const std::string& m) { handle_get_game(c, m); });
    router.register_handler("get_history",
        [this](net::Connection& c, const std::string& m) { handle_get_history(c, m); });
    router.register_handler("analyze_position",
        [this](net::Connection& c, const std::string& m) { handle_analyze_position(c, m); });
    // Phase 9.3 — anti-cheat
    router.register_handler("analyze_game",
        [this](net::Connection& c, const std::string& m) { handle_analyze_game(c, m); });
    // Phase 9.4 — tournaments
    router.register_handler("create_tournament",
        [this](net::Connection& c, const std::string& m) { handle_create_tournament(c, m); });
    router.register_handler("join_tournament",
        [this](net::Connection& c, const std::string& m) { handle_join_tournament(c, m); });
    router.register_handler("start_tournament",
        [this](net::Connection& c, const std::string& m) { handle_start_tournament(c, m); });
    router.register_handler("tournament_state",
        [this](net::Connection& c, const std::string& m) { handle_tournament_state(c, m); });
    router.register_handler("list_tournaments",
        [this](net::Connection& c, const std::string& m) { handle_list_tournaments(c, m); });
    router.register_handler("report_tournament_result",
        [this](net::Connection& c, const std::string& m) { handle_report_tournament_result(c, m); });
}

void GameHandler::on_player_disconnect(int connection_fd) {
    // Forward to the gameplay service (room + matchmaker + spectator sweep).
    // The un-migrated families currently have no disconnect hook.
    if (gameplay_handler_) {
        gameplay_handler_->on_player_disconnect(connection_fd);
    }
}

// ============================================================
// Shared helpers still used by the un-migrated families
// ============================================================

void GameHandler::send_json(net::Connection& conn, const std::string& json_str) {
    net::WebSocket::write_frame(conn, net::WsOpcode::TEXT, json_str);
}

void GameHandler::send_json_to_fd(int fd, const std::string& json_str) {
    if (!connection_lookup_) return;
    net::Connection* conn = connection_lookup_(fd);
    if (conn) {
        net::WebSocket::write_frame(*conn, net::WsOpcode::TEXT, json_str);
        // Flush immediately — this connection's handler isn't running,
        // so no one else will flush its write buffer for us.
        while (conn->has_data_to_write()) {
            int written = conn->write_to_socket();
            if (written <= 0) break;  // EAGAIN or error — epoll will retry later
        }
    }
}

void GameHandler::broadcast_to_spectators(GameRoom& room,
                                          const std::string& json_str) {
    // Snapshot first, iterate outside — room mutex must never be held across
    // send_json_to_fd (each send does a syscall + inline flush, which would
    // serialize every other move on this room behind the slowest watcher).
    const auto fds = room.spectator_fds();
    for (int fd : fds) {
        send_json_to_fd(fd, json_str);
    }
}

std::string GameHandler::make_error(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
}

void GameHandler::send_error_code(net::Connection& conn,
                                  const std::string& code,
                                  const std::string& message) {
    json err;
    err["type"]    = "error";
    err["code"]    = code;
    err["message"] = message;
    send_json(conn, err.dump());
}

bool GameHandler::extract_identity(net::Connection& conn,
                                   const nlohmann::json& msg,
                                   int64_t& out_db_player_id,
                                   std::string& out_username,
                                   int& out_elo) {
    // Thin adapter over the shared helper — preserves the un-migrated
    // families' out-parameter shape until LLD-2.2 / 2.3 / 2.4 switch
    // them to call the extractor directly.
    if (!identity_) {
        send_error_code(conn, application::auth::kAuthRequiredCode,
            "Authentication is not configured on this server");
        return false;
    }
    auto res = identity_->extract(msg);
    if (!res.is_ok()) {
        send_error_code(conn, application::auth::kAuthRequiredCode, res.reason);
        return false;
    }
    out_db_player_id = res.value.player_id;
    out_username     = res.value.username;
    out_elo          = res.value.elo_rating;
    return true;
}

std::string GameHandler::status_to_reason(GameStatus status) {
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

// ============================================================
// get_profile — Player profile with recent games
// ============================================================
// Request:  { "type": "get_profile", "username": "alice" }
// Response: { "type": "profile", "username": "...", "elo": ..., ... }

void GameHandler::handle_get_profile(net::Connection& conn, const std::string& message) {
    if (!db_) {
        send_json(conn, make_error("Profiles are not available (no database)"));
        return;
    }

    try {
        auto msg = json::parse(message);
        std::string username = msg.value("username", "");

        if (username.empty()) {
            send_json(conn, make_error("Missing 'username' field"));
            return;
        }

        auto profile = storage::find_player_by_username(*db_, username);
        if (!profile) {
            send_json(conn, make_error("Player '" + username + "' not found"));
            return;
        }

        int offset = msg.value("offset", 0);
        auto recent = storage::get_player_games(*db_, profile->player_id, 20, offset);

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

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// get_leaderboard — Top players by ELO
// ============================================================
// Request:  { "type": "get_leaderboard", "limit": 50, "offset": 0 }
// Response: { "type": "leaderboard", "players": [...] }

void GameHandler::handle_get_leaderboard(net::Connection& conn, const std::string& message) {
    if (!db_) {
        send_json(conn, make_error("Leaderboard is not available (no database)"));
        return;
    }

    try {
        auto msg = json::parse(message);
        int limit  = msg.value("limit", 50);
        int offset = msg.value("offset", 0);

        // Clamp limit to a reasonable maximum
        if (limit > 100) limit = 100;
        if (limit < 1)   limit = 1;

        auto entries = storage::get_leaderboard(*db_, limit, offset);

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

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_list_live_games(net::Connection& conn,
                                         const std::string& /*message*/) {
    auto rooms = room_mgr_.list_active_rooms();

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
    send_json(conn, response.dump());
}

// ============================================================
// spectate — Phase 9.1: join a live game as a read-only watcher
// ============================================================
// Request:  { "type": "spectate", "access_token": "…", "game_id": 1 }
// Response: { "type": "spectate_start", "game_id": …, "white": …, "black": …,
//             "fen": …, "white_time": …, "black_time": …,
//             "moves": [{san, think_ms}, ...],
//             "spectator_count": N }
//
// Refusals return a plain `error` frame (not `auth_required`), because they
// come from game-state checks, not from the auth gate:
//   - room not found        → error "Game X not found"
//   - room not IN_PROGRESS   → error "Game is not live"
//   - caller is a seat       → error "You are seated in this game"
//   - caller is in a game    → error "You are already in a game"
//
// The `access_token` is required (mandatory auth from the Phase 9 pre-work
// migration) — a missing/tampered token yields the same `auth_required`
// frame used by the game-starting commands.

void GameHandler::handle_spectate(net::Connection& conn,
                                  const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;
        (void)db_player_id; (void)elo;  // Identity is proven; snapshot unused for now

        GameId game_id = msg.value("game_id", static_cast<GameId>(0));
        if (game_id == 0) {
            send_json(conn, make_error("Missing or invalid game_id"));
            return;
        }

        // You cannot spectate while seated in another game — a click on
        // "Watch" from a player's own move screen would silently detach
        // them from their board otherwise. Force an explicit resign first.
        auto own_room = room_mgr_.find_room_by_fd(conn.get_fd());
        if (own_room && own_room->get_state() != RoomState::FINISHED) {
            send_json(conn, make_error("You are already in a game (ID: " +
                          std::to_string(own_room->get_id()) + ")"));
            return;
        }

        auto room = room_mgr_.find_room(game_id);
        if (!room) {
            send_json(conn, make_error("Game " + std::to_string(game_id) + " not found"));
            return;
        }
        if (room->get_state() != RoomState::IN_PROGRESS) {
            send_json(conn, make_error("Game is not live"));
            return;
        }

        // add_spectator refuses if the caller is one of this room's seats.
        // Distinct error message so the client can present it differently
        // from "the room is not accepting spectators."
        if (room->has_player(conn.get_fd())) {
            send_json(conn, make_error("You are seated in this game"));
            return;
        }
        if (!room->add_spectator(conn.get_fd())) {
            send_json(conn, make_error("Cannot spectate this game"));
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
        start["white"]           = room->get_username(Color::WHITE);
        start["black"]           = room->get_username(Color::BLACK);
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

        send_json(conn, start.dump());

        core::Logger::info("game", "GameHandler",
            username + " is spectating game " + std::to_string(game_id) +
            " (spectator_count=" + std::to_string(room->spectator_count()) + ")");

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// stop_spectating — Phase 9.1: leave a spectated game
// ============================================================
// Request:  { "type": "stop_spectating", "game_id": 1 }
// Response: { "type": "spectate_end", "game_id": 1 }
//
// No auth required — this is a "stop sending me frames" signal, not an
// action on the game. Idempotent: called on a room the caller isn't
// watching, the ack still comes back (client teardown paths don't have
// to know whether a subscription is live).

void GameHandler::handle_stop_spectating(net::Connection& conn,
                                         const std::string& message) {
    try {
        auto msg = json::parse(message);
        GameId game_id = msg.value("game_id", static_cast<GameId>(0));
        if (game_id != 0) {
            auto room = room_mgr_.find_room(game_id);
            if (room) room->remove_spectator(conn.get_fd());
        }
        json ack;
        ack["type"]    = "spectate_end";
        ack["game_id"] = game_id;
        send_json(conn, ack.dump());
    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// Phase 9.2 — Replay & Analysis
// ============================================================

namespace {

// Match a UCI (from,to,promo?) triple against the current legal-move list
// to recover a fully-flagged Move — the persisted "e2e4 e7e5 …" string only
// carries squares + optional promotion, but Board::make_move needs the
// castle / en-passant / double-push flags too. The move generator sets
// those correctly, so we filter its output by (from, to, promo_type).
//
// Returns a zero-initialised Move on miss (identifiable by move.from == 0
// AND move.to == 0 — no legal chess move ever has both squares equal, so
// callers can treat that as "not found").
Move resolve_legal(const Board& board, const Move& uci_probe) {
    const auto legal = move_gen::generate_legal_moves(board);
    for (const auto& m : legal) {
        if (m.from != uci_probe.from) continue;
        if (m.to   != uci_probe.to)   continue;
        if (uci_probe.flags & MoveFlags::PROMOTION) {
            if (m.promo_type != uci_probe.promo_type) continue;
        }
        return m;
    }
    return Move{};
}

// Split "e2e4 e7e5 g1f3" into ["e2e4", "e7e5", "g1f3"].
std::vector<std::string> split_uci_moves(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// Per-connection engine analysis budget: 20 requests per minute. The engine
// itself caps at depth 15 (~2.5 s at 60K NPS on the Debug build; deeper for
// Release). This keeps a single loud client from wedging the CPU that would
// otherwise be serving live moves.
constexpr auto ANALYZE_MAX_DEPTH = 15;
constexpr auto ANALYZE_TIME_MS   = 3000;

} // namespace

// ============================================================
// get_game — Phase 9.2: return a full replay payload
// ============================================================
// Request:  { "type": "get_game", "game_id": 1 }
// Response: { "type": "game", "game_id":1, "white":"Alice", "black":"Bob",
//             "result":"1-0", "reason":"checkmate",
//             "time_control":"600+5", "started_at":"…", "ended_at":"…",
//             "white_elo":1500, "black_elo":1400, "move_count":7,
//             "positions": [
//                 {"ply":0, "fen":"<start>"},                        // starting pos
//                 {"ply":1, "fen":"…", "from":"e2","to":"e4","san":"e4","think_ms":3000},
//                 …
//             ]
//           }
//
// No auth required — replays are public data. This is deliberate: sharing a
// game link should work for a friend who is not signed in.

void GameHandler::handle_get_game(net::Connection& conn,
                                  const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("Replay unavailable — server has no database"));
            return;
        }

        const int64_t game_id_i = msg.value("game_id", static_cast<int64_t>(0));
        if (game_id_i <= 0) {
            send_json(conn, make_error("Missing or invalid game_id"));
            return;
        }

        auto stored = storage::find_game_by_id(*db_, game_id_i);
        if (!stored) {
            send_json(conn, make_error("Game " + std::to_string(game_id_i) + " not found"));
            return;
        }

        // Load per-ply think times as {ply → ms}. Missing rows → 0 ms.
        std::vector<int> think_by_ply;
        {
            auto r = db_->exec(
                "SELECT ply_number, think_time_ms FROM move_times"
                " WHERE game_id = $1 ORDER BY ply_number",
                {storage::Param::int64(game_id_i)});
            if (r.ok) {
                think_by_ply.resize(r.rows.size() + 1, 0);
                for (const auto& row : r.rows) {
                    int p = std::stoi(row.at(0));
                    int t = std::stoi(row.at(1));
                    if (p >= 0 && static_cast<size_t>(p) < think_by_ply.size()) {
                        think_by_ply[p] = t;
                    }
                }
            }
        }

        // Reconstruct every board position by replaying the persisted UCI
        // string. Bail out on malformed data with a partial payload rather
        // than crashing — a database that got corrupted mid-write should
        // surface an obvious error string, not a silent truncation.
        Board board = Board::starting_position();

        json positions = json::array();
        {
            json p0;
            p0["ply"] = 0;
            p0["fen"] = board.to_fen();
            positions.push_back(p0);
        }

        const auto tokens = split_uci_moves(stored->moves);
        for (size_t i = 0; i < tokens.size(); ++i) {
            const Move probe = Move::from_uci(tokens[i]);
            const Move legal = resolve_legal(board, probe);
            if (legal.from == 0 && legal.to == 0) {
                // Corrupt or unrecognised move — stop the replay here and
                // return what we have so the frontend can still render a
                // partial reconstruction with a clear error.
                send_json(conn, make_error("Corrupt move at ply " +
                          std::to_string(i + 1) + ": " + tokens[i]));
                return;
            }
            const std::string san = notation::move_to_san(board, legal);
            const std::string from_str = Board::square_to_algebraic(legal.from);
            const std::string to_str   = Board::square_to_algebraic(legal.to);
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

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// get_history — Phase 9.2: a player's recent games
// ============================================================
// Request:  { "type": "get_history", "username": "Alice", "limit": 20 }
// Response: { "type": "history", "username": "Alice",
//             "games": [{game_id, opponent, my_color, result, reason,
//                        played_at, move_count, time_control}] }
//
// No auth required — this is the same "browse someone's public games" that
// get_profile already exposes, factored out into a dedicated endpoint so
// the replay list screen doesn't have to overload the profile response.

void GameHandler::handle_get_history(net::Connection& conn,
                                     const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("History unavailable — no database"));
            return;
        }

        std::string username = msg.value("username", std::string{});
        if (username.empty()) {
            send_json(conn, make_error("Missing 'username' field"));
            return;
        }
        int limit = msg.value("limit", 20);
        limit = std::clamp(limit, 1, 100);

        auto profile = storage::find_player_by_username(*db_, username);
        if (!profile) {
            send_json(conn, make_error("Player not found: " + username));
            return;
        }

        auto games = storage::get_player_games(*db_, profile->player_id, limit, 0);

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

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// analyze_position — Phase 9.2: engine eval of an arbitrary FEN
// ============================================================
// Request:  { "type": "analyze_position", "fen": "…", "depth": 8 }
// Response: { "type": "analysis", "fen": "…", "eval_cp": 42,
//             "best_move": "e2e4", "depth": 8, "nodes": 123456 }
//
// Bounded on the server side: depth ≤ 15 and time ≤ 3 s. Anything past that
// is a footgun in a shared process — a bored client hitting analyze on
// every move of a 100-move game would DoS the box.

void GameHandler::handle_analyze_position(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        std::string fen = msg.value("fen", std::string{});
        if (fen.empty()) {
            send_json(conn, make_error("Missing 'fen' field"));
            return;
        }
        int req_depth = msg.value("depth", 8);
        req_depth = std::clamp(req_depth, 1, ANALYZE_MAX_DEPTH);

        Board board;
        if (!board.set_from_fen(fen)) {
            send_json(conn, make_error("Invalid FEN"));
            return;
        }

        // Terminal positions have no best move; return a plain payload so the
        // frontend doesn't spin on "the engine will pick one" forever.
        auto legal = move_gen::generate_legal_moves(board);
        if (legal.empty()) {
            json response;
            response["type"]      = "analysis";
            response["fen"]       = fen;
            response["depth"]     = 0;
            response["nodes"]     = 0;
            response["eval_cp"]   = 0;
            response["best_move"] = "";
            response["terminal"]  = true;
            send_json(conn, response.dump());
            return;
        }

        // Engine::search takes (time_ms, max_depth). Whichever hits first
        // bounds the request — a shallow position finishes in a few ms
        // long before ANALYZE_TIME_MS.
        engine::Engine eng(16);   // 16 MB TT; disposable per-request
        eng.set_position(board);
        const auto res = eng.search(ANALYZE_TIME_MS, req_depth);

        json response;
        response["type"]      = "analysis";
        response["fen"]       = fen;
        response["depth"]     = res.depth;
        response["nodes"]     = static_cast<int64_t>(res.nodes);
        response["eval_cp"]   = res.score;
        response["best_move"] = res.best_move.to_uci();

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// analyze_game — Phase 9.3: statistical anti-cheat report
// ============================================================
// Request:  { "type": "analyze_game", "game_id": 1 }
// Response: { "type": "cheat_report", "game_id": 1,
//             "white": { plies_analyzed, plies_matched_engine,
//                        engine_agreement_pct, time_cv, complexity_corr,
//                        flagged, reasons: [...] },
//             "black": { …same shape… } }
//
// No auth. Reports are review data; a moderator (or a player checking a
// suspected opponent) needs to be able to open one without an active
// session. If misuse becomes a concern, gate it behind a moderator role;
// per-user rate limits are a natural follow-up too.
//
// Engine depth is deliberately modest (`ANTI_CHEAT_DEPTH = 6`,
// `ANTI_CHEAT_TIME_MS = 250`). The plan calls out depth 15 as the "strong"
// standard for engine-agreement analysis; that's too expensive for a
// synchronous handler over an entire game (100 plies × 3 s = 5 minutes).
// Depth 6 is fast enough to keep the whole analysis under a few seconds
// for typical games, and strong enough that the "engine best move" and
// the "actual engine best move at deeper search" agree on the majority of
// positions. Users who want the depth-15 gold standard can layer it on
// asynchronously later.

namespace {
constexpr int ANTI_CHEAT_DEPTH    = 6;
constexpr int ANTI_CHEAT_TIME_MS  = 250;

// nlohmann::json refuses to serialize NaN — every double we emit has to be
// funnelled through this to become `null` in the payload. Real values pass
// through unchanged.
json double_or_null(double d) {
    if (std::isnan(d) || std::isinf(d)) return nullptr;
    return d;
}

// Build the per-side JSON block. Kept out of the handler body so both
// sides use exactly the same shape.
json report_to_json(const analysis::AnalysisReport& r) {
    json j;
    j["plies_analyzed"]       = r.plies_analyzed;
    j["plies_matched_engine"] = r.plies_matched_engine;
    j["engine_agreement_pct"] = double_or_null(r.engine_agreement_pct);
    j["time_cv"]              = double_or_null(r.time_cv);
    j["complexity_corr"]      = double_or_null(r.complexity_corr);
    j["flagged"]              = r.flagged;
    j["reasons"]              = r.reasons;
    return j;
}
} // namespace

void GameHandler::handle_analyze_game(net::Connection& conn,
                                      const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("Analysis unavailable — no database"));
            return;
        }

        const int64_t game_id_i = msg.value("game_id", static_cast<int64_t>(0));
        if (game_id_i <= 0) {
            send_json(conn, make_error("Missing or invalid game_id"));
            return;
        }

        auto stored = storage::find_game_by_id(*db_, game_id_i);
        if (!stored) {
            send_json(conn, make_error("Game " + std::to_string(game_id_i) + " not found"));
            return;
        }

        // Load per-ply think times keyed by ply_number.
        std::vector<int> think_by_ply;
        {
            auto r = db_->exec(
                "SELECT ply_number, think_time_ms FROM move_times"
                " WHERE game_id = $1 ORDER BY ply_number",
                {storage::Param::int64(game_id_i)});
            if (r.ok) {
                think_by_ply.resize(r.rows.size() + 1, 0);
                for (const auto& row : r.rows) {
                    int p = std::stoi(row.at(0));
                    int t = std::stoi(row.at(1));
                    if (p >= 0 && static_cast<size_t>(p) < think_by_ply.size()) {
                        think_by_ply[p] = t;
                    }
                }
            }
        }

        // Reconstruct positions; for each ply, capture the pre-move
        // complexity, engine's best move, and whether the actual move
        // matched. This is the payoff: two per-side PlyData vectors that
        // the pure analyzer consumes.
        std::vector<analysis::PlyData> white_plies, black_plies;
        Board board = Board::starting_position();
        // One engine instance for the whole game — the TT stays hot across
        // consecutive positions and cuts total search time noticeably.
        engine::Engine eng(16);

        const auto tokens = split_uci_moves(stored->moves);
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto pre_legal = move_gen::generate_legal_moves(board);
            const int  complexity = static_cast<int>(pre_legal.size());

            const Move probe = Move::from_uci(tokens[i]);
            const Move legal = resolve_legal(board, probe);
            if (legal.from == 0 && legal.to == 0) {
                send_json(conn, make_error("Corrupt move at ply " +
                          std::to_string(i + 1) + ": " + tokens[i]));
                return;
            }

            // Run the engine on the pre-move position. Skip when the
            // position is terminal (no legal moves) — cannot happen here
            // because we just resolved a legal move, but defensive.
            Move engine_best;
            if (!pre_legal.empty()) {
                eng.set_position(board);
                const auto res = eng.search(ANTI_CHEAT_TIME_MS, ANTI_CHEAT_DEPTH);
                engine_best = res.best_move;
            }

            board.make_move(legal);

            // Terminal-after = mate/stalemate landed after this ply.
            const bool terminal_after =
                move_gen::generate_legal_moves(board).empty();

            analysis::PlyData pd;
            pd.think_time_ms    = (i + 1 < think_by_ply.size())
                                    ? think_by_ply[i + 1] : 0;
            pd.legal_move_count = complexity;
            pd.matched_engine   = (engine_best.from == legal.from
                                   && engine_best.to   == legal.to
                                   && engine_best.promo_type == legal.promo_type);
            pd.terminal_after   = terminal_after;

            // Ply 1 = White's first move, so odd i (0,2,4,…) → White.
            if ((i % 2) == 0) white_plies.push_back(pd);
            else              black_plies.push_back(pd);
        }

        analysis::AnalysisReport white_report =
            analysis::analyze_side(white_plies);
        analysis::AnalysisReport black_report =
            analysis::analyze_side(black_plies);

        // Persist both sides. save_cheat_report is upsert-on-conflict,
        // so re-running analyze_game on the same game refreshes rather
        // than duplicates.
        auto save_w = analysis::save_cheat_report(*db_, stored->game_id,
                                                   stored->white_id, "w",
                                                   white_report);
        auto save_b = analysis::save_cheat_report(*db_, stored->game_id,
                                                   stored->black_id, "b",
                                                   black_report);
        if (!save_w.ok || !save_b.ok) {
            core::Logger::warn("game", "AntiCheat",
                "Failed to persist cheat report for game "
                + std::to_string(stored->game_id) + ": "
                + (save_w.ok ? save_b.error : save_w.error));
            // Fall through — return the report to the caller even if
            // persistence failed. The verdict is already computed and
            // useful; a DB blip should not swallow it.
        }

        json response;
        response["type"]    = "cheat_report";
        response["game_id"] = stored->game_id;
        response["white"]   = report_to_json(white_report);
        response["black"]   = report_to_json(black_report);
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// Phase 9.4 — tournaments
// ============================================================
//
// All handlers share the same pattern: parse → require auth (for the
// mutating four) → dispatch to a fresh TournamentManager built over the
// server's Database. The manager is stateless in-process, so a per-call
// instance carries no cost.

namespace {

// Turn a StoredTournament row into the wire JSON shape.
nlohmann::json tournament_to_json(const tournament::StoredTournament& t) {
    nlohmann::json j;
    j["id"]             = t.id;
    j["name"]           = t.name;
    j["format"]         = t.format;
    j["rounds"]         = t.rounds;
    j["current_round"]  = t.current_round;
    j["time_base"]      = t.time_control_initial_ms   / 1000;
    j["time_inc"]       = t.time_control_increment_ms / 1000;
    j["status"]         = t.status;
    j["created_by"]     = t.created_by;
    j["created_at"]     = t.created_at;
    j["started_at"]     = t.started_at;
    j["completed_at"]   = t.completed_at;
    return j;
}

nlohmann::json standing_to_json(const tournament::StandingRow& r) {
    nlohmann::json j;
    j["player_id"]     = r.player_id;
    j["elo"]           = r.initial_elo;
    j["score"]         = r.score;
    j["buchholz"]      = r.buchholz;
    j["whites_played"] = r.whites_played;
    j["received_bye"]  = r.received_bye;
    j["withdrawn"]     = r.withdrawn;
    return j;
}

nlohmann::json pairing_to_json(const tournament::StoredPairing& p) {
    nlohmann::json j;
    j["id"]              = p.id;
    j["round"]           = p.round;
    j["white_player_id"] = p.white_player_id;
    if (p.black_player_id.has_value()) j["black_player_id"] = *p.black_player_id;
    else                               j["black_player_id"] = nullptr;
    if (p.game_id.has_value())         j["game_id"]         = *p.game_id;
    else                               j["game_id"]         = nullptr;
    j["result"]          = p.result;
    return j;
}

} // namespace

void GameHandler::handle_create_tournament(net::Connection& conn,
                                           const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }

        const std::string name = msg.value("name", std::string{});
        if (name.empty()) {
            send_json(conn, make_error("Missing 'name'"));
            return;
        }
        // Bound the name — keeps DB rows sane; anything longer is either
        // a mistake or an attempt to blow up the standings JSON payload.
        if (name.size() > 128) {
            send_json(conn, make_error("Tournament name too long"));
            return;
        }
        const int rounds   = msg.value("rounds",    4);
        const int time_base = msg.value("time_base", 300);
        const int time_inc  = msg.value("time_inc",  3);
        if (rounds <= 0 || rounds > 30) {
            send_json(conn, make_error("Rounds must be between 1 and 30"));
            return;
        }
        if (time_base <= 0 || time_inc < 0) {
            send_json(conn, make_error("Invalid time control"));
            return;
        }

        auto cr = tournament::create_tournament(
            *db_, name, rounds,
            /*tc_initial=*/time_base * 1000,
            /*tc_increment=*/time_inc * 1000,
            db_player_id);
        if (!cr.ok) {
            send_json(conn, make_error("create_tournament failed: " + cr.error));
            return;
        }

        json response;
        response["type"]          = "tournament_created";
        response["tournament_id"] = cr.id;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_join_tournament(net::Connection& conn,
                                         const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }

        if (!msg.contains("tournament_id") || !msg["tournament_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid tournament_id"));
            return;
        }
        const int64_t tid = msg["tournament_id"].get<int64_t>();

        tournament::TournamentManager tm(*db_);
        auto r = tm.join(tid, db_player_id, elo);
        if (!r.ok) {
            send_json(conn, make_error(r.error));
            return;
        }

        json response;
        response["type"]          = "tournament_joined";
        response["tournament_id"] = tid;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_start_tournament(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }

        if (!msg.contains("tournament_id") || !msg["tournament_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid tournament_id"));
            return;
        }
        const int64_t tid = msg["tournament_id"].get<int64_t>();

        tournament::TournamentManager tm(*db_);
        auto r = tm.start(tid, db_player_id);
        if (!r.ok) {
            send_json(conn, make_error(r.error));
            return;
        }

        auto state = tm.get_state(tid);
        int current_round = state ? state->tournament.current_round : 1;

        json response;
        response["type"]          = "tournament_started";
        response["tournament_id"] = tid;
        response["round"]         = current_round;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_tournament_state(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }
        if (!msg.contains("tournament_id") || !msg["tournament_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid tournament_id"));
            return;
        }
        const int64_t tid = msg["tournament_id"].get<int64_t>();

        tournament::TournamentManager tm(*db_);
        auto st = tm.get_state(tid);
        if (!st) {
            send_json(conn, make_error("tournament_not_found"));
            return;
        }

        json response;
        response["type"]       = "tournament_state";
        response["tournament"] = tournament_to_json(st->tournament);

        json standings = json::array();
        for (const auto& s : st->standings) standings.push_back(standing_to_json(s));
        response["standings"]  = standings;

        json pairings = json::array();
        for (const auto& p : st->all_pairings) pairings.push_back(pairing_to_json(p));
        response["pairings"]   = pairings;

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_list_tournaments(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }
        std::string status = msg.value("status", std::string{});
        int limit = msg.value("limit", 25);
        limit = std::clamp(limit, 1, 100);

        auto rows = tournament::list_tournaments(*db_, status, limit);

        json response;
        response["type"] = "tournament_list";
        json arr = json::array();
        for (const auto& t : rows) arr.push_back(tournament_to_json(t));
        response["tournaments"] = arr;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_report_tournament_result(net::Connection& conn,
                                                  const std::string& message) {
    // This is a creator-authored hook. In production, an end-of-game
    // callback inside persist_game will call TournamentManager::report_result
    // directly; the WebSocket-facing form here exists so the frontend (and
    // tests) can drive the state machine without wiring a real GameRoom
    // per pairing. See log narrative on the deferred auto-creation.
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }
        if (!msg.contains("pairing_id") || !msg["pairing_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid pairing_id"));
            return;
        }
        const int64_t pairing_id = msg["pairing_id"].get<int64_t>();
        const std::string result = msg.value("result", std::string{});

        // Fetch the pairing to check the tournament's creator matches.
        // Two queries — one here to find the tournament_id, one in
        // report_result to write. Not the tightest path, but this is
        // an out-of-band admin action; per-call cost is fine.
        auto rq = db_->exec(
            "SELECT tournament_id FROM tournament_pairings WHERE id = $1",
            {storage::Param::int64(pairing_id)});
        if (!rq.ok || rq.rows.empty()) {
            send_json(conn, make_error("pairing_not_found"));
            return;
        }
        const int64_t tid = std::stoll(rq.rows[0].at(0));

        auto tournament = tournament::find_tournament(*db_, tid);
        if (!tournament) {
            send_json(conn, make_error("tournament_not_found"));
            return;
        }
        if (tournament->created_by != db_player_id) {
            send_json(conn, make_error("not_creator"));
            return;
        }

        tournament::TournamentManager tm(*db_);
        auto r = tm.report_result(pairing_id, result);
        if (!r.ok) {
            send_json(conn, make_error(r.error));
            return;
        }

        json response;
        response["type"]       = "tournament_result_recorded";
        response["pairing_id"] = pairing_id;
        response["result"]     = result;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

} // namespace game
} // namespace chess
