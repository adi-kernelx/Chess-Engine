/**
 * game_handler.h — WebSocket API handler for chess game actions
 *
 * This module is the bridge between the network layer (WebSocket + JSON)
 * and the game layer (GameRoom + RoomManager). It translates incoming
 * JSON messages from browser clients into GameRoom operations.
 *
 * WebSocket JSON API:
 *
 *   Client → Server:
 *     { "type": "create_game",     "access_token": "…", "time_base": 600, "time_inc": 5 }
 *     { "type": "join_game",       "access_token": "…", "game_id": 1 }
 *     { "type": "make_move",       "from": "e2", "to": "e4", "promotion": "q" }
 *     { "type": "resign" }
 *     { "type": "quick_play",      "access_token": "…", "time_base": 600, "time_inc": 5 }
 *     { "type": "cancel_queue" }
 *     { "type": "play_ai",         "access_token": "…", "difficulty": "medium", "time_base": 600, "time_inc": 5 }
 *     { "type": "list_games" }
 *     { "type": "game_state" }
 *     { "type": "list_live_games" }                                     // Phase 9.1
 *     { "type": "spectate",        "access_token": "…", "game_id": 1 }  // Phase 9.1
 *     { "type": "stop_spectating", "game_id": 1 }                       // Phase 9.1
 *
 *   The four game-starting commands (create_game/join_game/quick_play/play_ai)
 *   REQUIRE a valid access_token as of Phase 9 pre-work. The username and ELO
 *   used for the room come from the authenticated players row — the client no
 *   longer supplies them. A missing, malformed, expired, revoked, or unknown-
 *   player token gets:
 *
 *     { "type": "error", "code": "auth_required", "message": "…" }
 *
 *   Server → Client:
 *     { "type": "game_created",   "game_id": 1, "color": "white" }
 *     { "type": "game_joined",    "game_id": 1, "color": "black", "opponent": "Alice", ... }
 *     { "type": "game_start",     "game_id": 1, "white": "Alice", "black": "Bob", ... }
 *     { "type": "move_made",      "from": "e2", "to": "e4", "san": "e4", ... }
 *     { "type": "move_rejected",  "error": "Illegal move" }
 *     { "type": "game_over",      "result": "1-0", "reason": "checkmate" }
 *     { "type": "game_list",      "games": [...] }
 *     { "type": "game_state",     "fen": "...", "moves": [...], ... }
 *     { "type": "queued",         "queue_size": 3 }
 *     { "type": "match_found",    "game_id": 1, "color": "white", "opponent": "Bob" }
 *     { "type": "queue_cancelled" }
 *     { "type": "error",          "message": "..." }
 *     // Phase 9.1 — spectating
 *     { "type": "live_game_list", "games": [{"game_id","white","black","time_control","spectator_count","move_count"}] }
 *     { "type": "spectate_start", "game_id":1, "white":"Alice","black":"Bob","fen":"…","moves":[…],"white_time":…,"black_time":…,"spectator_count":N }
 *     { "type": "spectate_end",   "game_id": 1 }
 *     // Phase 9.2 — replay + analysis
 *     { "type": "game",     "game_id":1, "white":"…","black":"…","result":"1-0","reason":"checkmate",
 *                           "time_control":"600+5","started_at":"…","ended_at":"…",
 *                           "positions": [{ply,fen,san?,from?,to?,think_ms}, …] }
 *     { "type": "history",  "username":"Alice", "games":[{game_id, opponent, my_color, result, ...}] }
 *     { "type": "analysis", "fen":"…", "eval_cp": 42, "best_move":"e2e4", "depth": 8 }
 *
 *   Phase 9.2 client → server (additional):
 *     { "type": "get_game",          "game_id": 1 }
 *     { "type": "get_history",       "username": "Alice", "limit": 20 }
 *     { "type": "analyze_position",  "fen": "…", "depth": 8 }
 *
 *   Phase 9.3 client → server (additional):
 *     { "type": "analyze_game",      "game_id": 1 }
 *   Phase 9.3 server → client:
 *     { "type": "cheat_report", "game_id":1,
 *                               "white": { "plies_analyzed":…, "plies_matched_engine":…,
 *                                          "engine_agreement_pct":…, "time_cv":…,
 *                                          "complexity_corr":…, "flagged":true/false,
 *                                          "reasons":[…] },
 *                               "black": {   …same shape…   } }
 *
 *   Phase 9.4 client → server (additional):
 *     { "type": "create_tournament",   "access_token":"…", "name":"Sat Blitz",
 *                                       "rounds": 4, "time_base": 300, "time_inc": 3 }
 *     { "type": "join_tournament",     "access_token":"…", "tournament_id": 1 }
 *     { "type": "start_tournament",    "access_token":"…", "tournament_id": 1 }
 *     { "type": "tournament_state",    "tournament_id": 1 }
 *     { "type": "list_tournaments",    "status": "registration"  // optional filter
 *                                       "limit":  25 }           // optional
 *     { "type": "report_tournament_result", "access_token":"…",  // creator-only dev hook
 *                                            "pairing_id": 42, "result": "1-0" }
 *   Phase 9.4 server → client:
 *     { "type": "tournament_created", "tournament_id": 1 }
 *     { "type": "tournament_joined",  "tournament_id": 1 }
 *     { "type": "tournament_started", "tournament_id": 1, "round": 1 }
 *     { "type": "tournament_result_recorded",
 *                                      "pairing_id": 42, "result": "1-0" }
 *     { "type": "tournament_state",   "tournament": {…},
 *                                      "standings": [{player_id, elo, score,
 *                                                      buchholz, whites_played,
 *                                                      received_bye, withdrawn}, …],
 *                                      "pairings":  [{id, round, white_player_id,
 *                                                      black_player_id?, game_id?,
 *                                                      result}, …] }
 *     { "type": "tournament_list",    "tournaments": [ {id, name, rounds,
 *                                                        current_round, status,
 *                                                        time_base, time_inc,
 *                                                        created_at}, … ] }
 */

#pragma once

#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "game/room_manager.h"
#include "game/matchmaker.h"
#include "net/websocket.h"
#include "net/connection.h"
#include "protocol/request.h"
#include "storage/database.h"
#include "auth/token.h"
#include <nlohmann/json.hpp>
#include <string>
#include <atomic>

namespace chess {
namespace game {

class GameHandler {
public:
    /// Construct a GameHandler with references to the shared RoomManager and Matchmaker.
    GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker);

    /// Register all game-related message handlers on the WebSocket router.
    void register_handlers(net::MessageRouter& router);

private:
    // ── Individual message handlers ──
    void handle_create_game(net::Connection& conn, const std::string& message);
    void handle_join_game(net::Connection& conn, const std::string& message);
    void handle_make_move(net::Connection& conn, const std::string& message);
    void handle_resign(net::Connection& conn, const std::string& message);
    void handle_quick_play(net::Connection& conn, const std::string& message);
    void handle_cancel_queue(net::Connection& conn, const std::string& message);
    void handle_list_games(net::Connection& conn, const std::string& message);
    void handle_game_state(net::Connection& conn, const std::string& message);
    void handle_play_ai(net::Connection& conn, const std::string& message);
    void handle_get_profile(net::Connection& conn, const std::string& message);
    void handle_get_leaderboard(net::Connection& conn, const std::string& message);
    // Phase 9.1 — spectating
    void handle_spectate(net::Connection& conn, const std::string& message);
    void handle_stop_spectating(net::Connection& conn, const std::string& message);
    void handle_list_live_games(net::Connection& conn, const std::string& message);
    // Phase 9.2 — replay + analysis
    void handle_get_game(net::Connection& conn, const std::string& message);
    void handle_analyze_position(net::Connection& conn, const std::string& message);
    void handle_get_history(net::Connection& conn, const std::string& message);
    // Phase 9.3 — anti-cheat
    void handle_analyze_game(net::Connection& conn, const std::string& message);
    // Phase 9.4 — tournaments
    void handle_create_tournament(net::Connection& conn, const std::string& message);
    void handle_join_tournament(net::Connection& conn, const std::string& message);
    void handle_start_tournament(net::Connection& conn, const std::string& message);
    void handle_tournament_state(net::Connection& conn, const std::string& message);
    void handle_list_tournaments(net::Connection& conn, const std::string& message);
    void handle_report_tournament_result(net::Connection& conn, const std::string& message);

    // ── LLD-1 typed impls ──
    //
    // The entry points above still bind to the router with the raw
    // (Connection&, std::string) signature. Inside, they parse the JSON,
    // decode into a typed request via `protocol::codec`, build a
    // RequestContext + a caller-side MessageSink for the mover, and
    // delegate to the impl below. Broadcast fan-out (opponent + spectators)
    // still uses the pre-existing inline flush pattern in LLD-1 — the plan
    // schedules a full-sink broadcast for LLD-4.
    void handle_make_move_impl(const application::RequestContext& ctx,
                               const protocol::MakeMoveRequest&   req,
                               application::MessageSink&          caller_sink);
    void handle_resign_impl(const application::RequestContext& ctx,
                            const protocol::ResignRequest&     req,
                            application::MessageSink&          caller_sink);
    void handle_game_state_impl(const application::RequestContext& ctx,
                                const protocol::GameStateRequest&  req,
                                application::MessageSink&          caller_sink);

    /// After a human makes a move in an AI game, compute and submit the AI's response.
    void trigger_ai_move(std::shared_ptr<GameRoom> room, int human_fd);

    // ── Helpers ──

    /// Send a JSON response to a single connection.
    void send_json(net::Connection& conn, const std::string& json);

    /// Send a JSON response to a connection identified by fd.
    /// Uses the connection lookup callback set during registration.
    void send_json_to_fd(int fd, const std::string& json);

    /// Fan out a JSON frame to every spectator of the given room. Broadcasts
    /// are flushed inline (see the flush discipline note on send_json_to_fd).
    /// The spectator list is snapshotted under the room lock, then iterated
    /// outside — so a dying spectator can never wedge the room mutex.
    void broadcast_to_spectators(GameRoom& room, const std::string& json_str);

    /// Build a JSON error response.
    static std::string make_error(const std::string& message);

    /// Convert a GameStatus enum to a human-readable reason string.
    static std::string status_to_reason(GameStatus status);

    /// Convert an algebraic square string ("e4") to a Square index.
    /// Returns NO_SQUARE on invalid input.
    static Square parse_square(const std::string& sq_str);

    /// Persist a finished game to the database (ELO, stats, move times).
    /// No-op if db_ is null, game is AI, or players are unauthenticated.
    void persist_game(GameRoom* room, GameStatus status);

    /// Run the full auth gate on an access_token embedded in `msg` and snapshot
    /// the caller's identity for use as a room seat. On success, fills the out
    /// params and returns true. On any failure (missing token, bad signature,
    /// expired, revoked epoch, unknown player, or auth not wired), sends an
    /// auth_required error frame to `conn` and returns false.
    ///
    /// The username and ELO come from the authenticated players row — the
    /// client never supplies them on the wire. `find_player_by_id` failure
    /// (deleted account after the token was minted) is treated as auth_required
    /// too, closing the "logout_all beat the room start" race.
    bool extract_identity(net::Connection& conn,
                          const nlohmann::json& msg,
                          int64_t& out_db_player_id,
                          std::string& out_username,
                          int& out_elo);

    /// Send an {"type":"error","code":<code>,"message":<msg>} frame.
    void send_error_code(net::Connection& conn,
                         const std::string& code,
                         const std::string& message);

    // ── Data ──

    RoomManager&  room_mgr_;
    Matchmaker&   matchmaker_;
    AIPlayer      ai_player_;
    std::atomic<PlayerId> next_player_id_{1}; // Temporary player IDs (until auth is added)

    storage::Database*    db_     = nullptr;   // Optional — null when DB not configured
    auth::TokenSigner*    signer_ = nullptr;   // Optional — null when auth not configured

    // Callback to look up a Connection by fd (set by TcpServer integration)
    // This allows us to send messages to the opponent without having
    // a direct Connection& reference.
    std::function<net::Connection*(int fd)> connection_lookup_;

public:
    /// Set the callback used to look up connections by fd.
    /// Must be called before handling any messages.
    void set_connection_lookup(std::function<net::Connection*(int fd)> lookup) {
        connection_lookup_ = std::move(lookup);
    }

    /// Set the database for game persistence (optional).
    void set_database(storage::Database* db) { db_ = db; }

    /// Set the token signer for auth extraction (optional).
    void set_signer(auth::TokenSigner* s) { signer_ = s; }

    /// Called when a player disconnects — cleans up queue and room state.
    void on_player_disconnect(int connection_fd);
};

} // namespace game
} // namespace chess
