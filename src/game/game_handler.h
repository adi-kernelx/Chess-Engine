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
 *     { "type": "create_game", "username": "Alice", "time_base": 600, "time_inc": 5 }
 *     { "type": "join_game",   "username": "Bob",   "game_id": 1 }
 *     { "type": "make_move",   "from": "e2", "to": "e4", "promotion": "q" }
 *     { "type": "resign" }
 *     { "type": "quick_play",  "username": "Alice", "elo": 1200, "time_base": 600, "time_inc": 5 }
 *     { "type": "cancel_queue" }
 *     { "type": "list_games" }
 *     { "type": "game_state" }
 *
 *   Server → Client:
 *     { "type": "game_created",  "game_id": 1, "color": "white" }
 *     { "type": "game_joined",   "game_id": 1, "color": "black", "opponent": "Alice", ... }
 *     { "type": "game_start",    "game_id": 1, "white": "Alice", "black": "Bob", ... }
 *     { "type": "move_made",     "from": "e2", "to": "e4", "san": "e4", ... }
 *     { "type": "move_rejected", "error": "Illegal move" }
 *     { "type": "game_over",     "result": "1-0", "reason": "checkmate" }
 *     { "type": "game_list",     "games": [...] }
 *     { "type": "game_state",    "fen": "...", "moves": [...], ... }
 *     { "type": "queued",        "queue_size": 3 }
 *     { "type": "match_found",   "game_id": 1, "color": "white", "opponent": "Bob" }
 *     { "type": "queue_cancelled" }
 *     { "type": "error",         "message": "..." }
 */

#pragma once

#include "game/room_manager.h"
#include "game/matchmaker.h"
#include "net/websocket.h"
#include "net/connection.h"
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

    // ── Helpers ──

    /// Send a JSON response to a single connection.
    void send_json(net::Connection& conn, const std::string& json);

    /// Send a JSON response to a connection identified by fd.
    /// Uses the connection lookup callback set during registration.
    void send_json_to_fd(int fd, const std::string& json);

    /// Build a JSON error response.
    static std::string make_error(const std::string& message);

    /// Convert a GameStatus enum to a human-readable reason string.
    static std::string status_to_reason(GameStatus status);

    /// Convert an algebraic square string ("e4") to a Square index.
    /// Returns NO_SQUARE on invalid input.
    static Square parse_square(const std::string& sq_str);

    // ── Data ──

    RoomManager&  room_mgr_;
    Matchmaker&   matchmaker_;
    std::atomic<PlayerId> next_player_id_{1}; // Temporary player IDs (until auth is added)

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

    /// Called when a player disconnects — cleans up queue and room state.
    void on_player_disconnect(int connection_fd);
};

} // namespace game
} // namespace chess
