/**
 * game_room.h — A single game instance between two players
 *
 * A GameRoom encapsulates the full lifecycle of one chess game:
 *   WAITING      → One player created the room, waiting for an opponent
 *   IN_PROGRESS  → Both players connected, moves are being exchanged
 *   FINISHED     → Game ended (checkmate, resignation, timeout, draw, etc.)
 *
 * The GameRoom is the authoritative source of truth for the game state.
 * All move validation happens here — clients submit moves, the server
 * validates them against the chess rules engine, and broadcasts the result.
 *
 * Thread safety: GameRoom is protected by its own mutex. The RoomManager
 * holds a shared_ptr to each room, and individual operations lock the
 * room's mutex. This allows different games to proceed in parallel.
 */

#pragma once

#include "core/types.h"
#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "chess/notation.h"
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>

namespace chess {
namespace game {

// ============================================================
// Game Room State Machine
// ============================================================

enum class RoomState : uint8_t {
    WAITING,       // Created, waiting for second player
    IN_PROGRESS,   // Both players connected, game is live
    FINISHED       // Game over — result is final
};

// ============================================================
// Time Control — Fischer increment (e.g., 10+5 = 10 min + 5 sec/move)
// ============================================================

struct TimeControl {
    int base_time_ms;     // Initial time in milliseconds (e.g., 600000 = 10 min)
    int increment_ms;     // Time added per move in milliseconds (e.g., 5000 = 5 sec)

    TimeControl() : base_time_ms(600000), increment_ms(5000) {}
    TimeControl(int base_ms, int inc_ms) : base_time_ms(base_ms), increment_ms(inc_ms) {}

    /// Format as "600+5" string (seconds)
    std::string to_string() const;
};

// ============================================================
// Player Slot — one side of the game
// ============================================================

struct PlayerSlot {
    int         connection_fd = -1;       // File descriptor (-1 = empty slot)
    PlayerId    player_id     = 0;        // Unique player identifier
    std::string username;                 // Display name
    int         remaining_ms  = 0;        // Time remaining in milliseconds
    bool        connected     = false;    // Currently connected?

    /// Clock timestamp: when this player's clock started ticking
    std::chrono::steady_clock::time_point clock_start;

    bool is_empty() const { return connection_fd == -1; }
};

// ============================================================
// Move Record — stored for each move in the game history
// ============================================================

struct MoveRecord {
    Move        move;
    std::string san;          // SAN notation (e.g., "Nf3")
    int         think_time_ms; // How long the player spent on this move
};

// ============================================================
// GameRoom — the core game instance
// ============================================================

class GameRoom {
public:
    /// Create a new game room with the given ID and time control.
    /// The creating player is automatically seated as White.
    GameRoom(GameId id, PlayerId creator_id, const std::string& creator_name,
             int creator_fd, const TimeControl& tc = TimeControl());

    // --------------------------------------------------------
    // Room lifecycle
    // --------------------------------------------------------

    /// Second player joins the room. Starts the game clock.
    /// Returns false if the room is full or already in progress.
    bool join(PlayerId player_id, const std::string& player_name, int connection_fd);

    /// A player submits a move (from their connection fd).
    /// Validates legality, updates the board, toggles clocks.
    /// Returns a result describing success/failure.
    struct MoveResult {
        bool        success;
        std::string error;       // Error message if success == false
        std::string san;         // SAN notation of the accepted move
        GameStatus  game_status; // Status after the move
        int         white_time_ms;
        int         black_time_ms;
    };
    MoveResult submit_move(int connection_fd, Square from, Square to,
                           PieceType promo_type = PieceType::NONE);

    /// Player resigns (identified by their connection fd).
    bool resign(int connection_fd);

    /// Handle a player disconnecting mid-game.
    void on_disconnect(int connection_fd);

    /// Handle a player reconnecting (with a new fd).
    bool on_reconnect(PlayerId player_id, int new_fd);

    // --------------------------------------------------------
    // Accessors (all thread-safe via internal mutex)
    // --------------------------------------------------------

    GameId              get_id()           const;
    RoomState           get_state()        const;
    const Board&        get_board()        const;
    Color               side_to_move()     const;
    const TimeControl&  get_time_control() const;
    std::string         get_result_string() const; // "1-0", "0-1", "1/2-1/2", "*"
    GameStatus          get_game_status()  const;

    /// Get the connection fd for a specific color. Returns -1 if empty.
    int  get_player_fd(Color color)  const;
    /// Get the opponent's connection fd. Returns -1 if no opponent.
    int  get_opponent_fd(int my_fd)  const;

    /// Check if a given fd belongs to this room.
    bool has_player(int connection_fd) const;
    /// Check if a given player id belongs to this room.
    bool has_player_id(PlayerId pid) const;

    /// Get whose turn it is (connection fd). Returns -1 if game not in progress.
    int  current_turn_fd() const;

    /// Get remaining time for each player (updates the active clock).
    void get_remaining_times(int& white_ms, int& black_ms) const;

    /// Get the move history as a vector of MoveRecords.
    std::vector<MoveRecord> get_move_history() const;

    /// Export the game as PGN.
    std::string to_pgn() const;

private:
    // --------------------------------------------------------
    // Internal helpers
    // --------------------------------------------------------

    /// Determine which color a connection fd corresponds to.
    /// Returns Color::NONE if the fd isn't in this room.
    Color color_of(int connection_fd) const;

    /// End the game with a result.
    void finish_game(GameStatus status, const std::string& result);

    /// Update the clock: stop the current player's clock, deduct elapsed time,
    /// add increment, and start the opponent's clock.
    void switch_clock();

    /// Check if the current player has flagged (ran out of time).
    bool check_flag() const;

    // --------------------------------------------------------
    // Data
    // --------------------------------------------------------

    mutable std::mutex  mutex_;

    GameId              id_;
    RoomState           state_        = RoomState::WAITING;
    Board               board_;
    TimeControl         time_control_;
    std::string         result_;       // "1-0", "0-1", "1/2-1/2", "*"
    GameStatus          game_status_  = GameStatus::ONGOING;

    PlayerSlot          white_;
    PlayerSlot          black_;

    std::vector<MoveRecord> move_history_;

    /// Timestamp when the game started (first move)
    std::chrono::steady_clock::time_point game_start_time_;
};

} // namespace game
} // namespace chess
