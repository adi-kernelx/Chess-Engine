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

#include "application/ports/clock.h"
#include "core/types.h"
#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "chess/notation.h"
#include "game/ai_player.h"
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <functional>

namespace chess {
namespace game {

// Forward declarations for LLD-4.1 listener plumbing. Full definitions
// live in `game/game_events.h`, which itself includes `game_snapshot.h`
// which needs `MoveRecord` + `TimeControl` from THIS header — so we
// break the cycle here.
class GameEventListener;
using GameEventListenerPtr = std::shared_ptr<GameEventListener>;
struct GameStarted;
struct GameCompleted;
struct GameSnapshot;

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

    bool operator==(const TimeControl& other) const {
        return base_time_ms == other.base_time_ms && increment_ms == other.increment_ms;
    }

    /// Format as "600+5" string (seconds)
    std::string to_string() const;
};

// ============================================================
// Player Slot — one side of the game
// ============================================================

struct PlayerSlot {
    int         connection_fd  = -1;       // File descriptor (-1 = empty slot)
    uint64_t    connection_generation = 0; // Prevent an older request reclaiming a refreshed seat
    PlayerId    player_id      = 0;        // Unique player identifier (local atomic counter)
    int64_t     db_player_id   = 0;        // Real database ID (0 = unauthenticated)
    std::string username;                  // Display name
    int         elo            = 1200;     // ELO rating snapshot at game start
    int         remaining_ms   = 0;        // Time remaining in milliseconds
    bool        connected      = false;    // Currently connected?
    bool        tournament_checked_in = false;

    /// Set when a live seat loses its transport connection. A reconnect
    /// clears it; the server's maintenance tick uses it to enforce the
    /// bounded disconnect grace period independently of the chess clock.
    std::chrono::steady_clock::time_point disconnected_at{};

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
             int creator_fd, const TimeControl& tc = TimeControl(),
             int64_t db_player_id = 0, int elo = 1200);

    /// Create an AI game room. Human plays White, AI plays Black.
    /// The game starts immediately (no WAITING state).
    GameRoom(GameId id, PlayerId creator_id, const std::string& creator_name,
             int creator_fd, const TimeControl& tc, AIDifficulty difficulty,
             int64_t db_player_id = 0, int elo = 1200);

    /// Create a tournament room with two durable reserved seats. Neither seat
    /// is connected initially and the clock cannot start until
    /// `open_reserved()` has been called and both identities have bound.
    GameRoom(GameId id, int64_t tournament_id, int64_t pairing_id,
             int64_t white_db_id, const std::string& white_name, int white_elo,
             int64_t black_db_id, const std::string& black_name, int black_elo,
             const TimeControl& tc);

    /// Out-of-line so `unique_ptr<GameCompleted>` (forward-declared in
    /// this header) sees the full type at destruction, which is defined
    /// in `game_events.h` included by game_room.cpp.
    ~GameRoom();

    // --------------------------------------------------------
    // Room lifecycle
    // --------------------------------------------------------

    /// Second player joins the room. Starts the game clock.
    /// Returns false if the room is full or already in progress.
    bool join(PlayerId player_id, const std::string& player_name, int connection_fd,
              int64_t db_player_id = 0, int elo = 1200);

    struct ReservedBindResult {
        bool ok = false;
        Color color = Color::NONE;
        bool started = false;
        std::string error;
    };
    ReservedBindResult bind_reserved_player(int64_t db_player_id,
                                            PlayerId local_player_id,
                                            int connection_fd,
                                            bool replace_existing = false,
                                            uint64_t generation = 0);
    bool allow_reserved_player(int64_t db_player_id);
    /// Mark the scheduled start as reached. Idempotent; starts exactly once
    /// when both reserved seats are connected.
    bool open_reserved();

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
        std::string fen;          // authoritative post-move position
        std::vector<std::string> legal_moves; // UCI moves for new side to move
        bool draw_offer_declined = false; // offeree made a move
    };
    MoveResult submit_move(int connection_fd, Square from, Square to,
                           PieceType promo_type = PieceType::NONE);

    /// Submit a move on behalf of the AI (bypasses connection_fd check).
    /// Only valid in AI games when it's the AI's turn.
    MoveResult submit_move_ai(Square from, Square to,
                              PieceType promo_type = PieceType::NONE);

    /// Player resigns (identified by their connection fd).
    /// The optional callback runs after the terminal state is committed and
    /// the room lock is released, but before completion listeners (including
    /// database persistence). This keeps result delivery off the DB latency
    /// path without exposing an uncommitted result.
    bool resign(int connection_fd, std::function<void()> on_committed = {});

    /// Offer a draw to the other human seat. `error` is populated on false.
    bool offer_draw(int connection_fd, std::string& error);

    /// Accept or decline the opponent's pending draw offer. Accepting performs
    /// the terminal DRAW_AGREEMENT transition and fires completion listeners.
    bool respond_to_draw(int connection_fd, bool accept, std::string& error);

    /// Remove and return the offerer when a pending draw offer reaches its
    /// server-authoritative lifetime. Returns Color::NONE while still valid or
    /// when no offer exists.
    Color expire_draw_offer(std::chrono::milliseconds ttl);

    /// Rematch negotiation is valid only after a completed human game. The
    /// service creates the replacement room after an accepted response.
    bool offer_rematch(int connection_fd, std::string& error);
    bool respond_to_rematch(int connection_fd, bool accept,
                            Color& offerer, std::string& error);
    Color expire_rematch_offer(std::chrono::milliseconds ttl);

    /// Finish an in-progress game when the active player's authoritative
    /// server clock reaches zero. Returns true exactly once, on the terminal
    /// transition. Periodic maintenance calls this so timeout does not depend
    /// on the flagged player attempting another move.
    bool expire_on_time();

    /// Handle a player disconnecting mid-game.
    void on_disconnect(int connection_fd);

    /// Handle a player reconnecting (with a new fd).
    bool on_reconnect(PlayerId player_id, int new_fd);

    /// Rebind an authenticated database player to a replacement socket.
    /// Active rooms use this for game recovery; finished rooms use it only so
    /// a still-pending rematch can be answered after navigation/reconnection.
    /// Local PlayerId values are process-only and never known by the browser.
    bool on_reconnect_db_player(int64_t db_player_id, int new_fd, uint64_t generation = 0);

    /// Finish the game when the oldest disconnected seat has exceeded the
    /// grace period. Returns true exactly once, on the terminal transition.
    bool expire_disconnected(std::chrono::milliseconds grace);

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
    bool                is_ai_game()       const;
    bool                is_tournament_game() const;
    int64_t             tournament_id() const;
    int64_t             pairing_id() const;
    AIDifficulty        ai_difficulty()    const;
    Color               ai_color()         const;

    /// Get the connection fd for a specific color. Returns -1 if empty.
    int  get_player_fd(Color color)  const;
    /// Get the opponent's connection fd. Returns -1 if no opponent.
    int  get_opponent_fd(int my_fd)  const;

    /// Check if a given fd belongs to this room.
    bool has_player(int connection_fd) const;
    /// Check if a given player id belongs to this room.
    bool has_player_id(PlayerId pid) const;
    /// Check whether a durable authenticated player belongs to this room.
    bool has_db_player_id(int64_t db_player_id) const;

    /// Whether the seat is currently connected. Color::NONE is false.
    bool is_connected(Color color) const;

    /// Get whose turn it is (connection fd). Returns -1 if game not in progress.
    int  current_turn_fd() const;

    /// Get remaining time for each player (updates the active clock).
    void get_remaining_times(int& white_ms, int& black_ms) const;

    /// Get the move history as a vector of MoveRecords.
    std::vector<MoveRecord> get_move_history() const;

    /// Legal moves for the current side, encoded as UCI strings. Empty when
    /// the room is not in progress. Used only for instant client highlights;
    /// submit_move remains the authority that accepts or rejects a move.
    std::vector<std::string> get_legal_moves_uci() const;

    /// Color that currently has an outstanding offer, or Color::NONE.
    Color draw_offer_from() const;
    /// Color that currently has an outstanding rematch offer, or Color::NONE.
    Color rematch_offer_from() const;

    /// Get the player ID (local) for a specific color.
    PlayerId get_player_id(Color color) const;
    /// Get the database player ID for a specific color (0 = unauthenticated).
    int64_t get_db_player_id(Color color) const;
    /// Get the username for a specific color.
    std::string get_username(Color color) const;
    /// Get the ELO snapshot for a specific color.
    int get_elo(Color color) const;
    /// Get the game start time as ISO 8601 string (UTC).
    std::string get_started_at_iso() const;
    /// Get the game end time as ISO 8601 string (UTC).
    std::string get_ended_at_iso() const;

    /// Export the game as PGN.
    std::string to_pgn() const;

    // --------------------------------------------------------
    // Spectators (Phase 9.1)
    // --------------------------------------------------------

    /// Register a spectator by its connection fd. Idempotent: adding the same
    /// fd twice is a no-op. Refuses if the fd is currently seated as one of
    /// this room's players (a player cannot also spectate their own game).
    /// Returns true on success, false if the fd is a player or the room is
    /// not IN_PROGRESS.
    bool add_spectator(int connection_fd);

    /// Remove a spectator by its connection fd. Returns true if the fd was
    /// present and removed, false otherwise. Idempotent.
    bool remove_spectator(int connection_fd);

    /// Snapshot the current spectator fds under the lock. Returned by value so
    /// the caller can iterate outside the lock (fan-out is a foreign-fd write
    /// that flushes inline — never hold this mutex across send_json_to_fd).
    std::vector<int> spectator_fds() const;

    /// Cheap accessor for the lobby's `spectator_count` field.
    size_t spectator_count() const;

    // --------------------------------------------------------
    // LLD-4.1 — typed event listeners
    // --------------------------------------------------------

    /// Register a listener. The room holds a `shared_ptr`; caller keeps
    /// a copy to control lifetime. Adding the same pointer twice is
    /// legal — it results in two callbacks per event. Idempotency
    /// (if wanted) is the caller's responsibility.
    void add_listener(GameEventListenerPtr listener);

    /// Remove by `shared_ptr` identity (owner_before equality). Returns
    /// true if a matching listener was found and removed, false
    /// otherwise. Safe to call from a listener callback — the room
    /// snapshots the listener list before invoking.
    bool remove_listener(const GameEventListenerPtr& listener);

private:
    // --------------------------------------------------------
    // Internal helpers
    // --------------------------------------------------------

    /// Determine which color a connection fd corresponds to.
    /// Returns Color::NONE if the fd isn't in this room.
    Color color_of(int connection_fd) const;

    /// End the game with a result. Builds the terminal `GameSnapshot`
    /// under `mutex_`, snapshots the listener list, releases the lock,
    /// then invokes each listener's `on_game_completed` outside the
    /// lock. Listener callbacks that throw are caught + logged; other
    /// listeners still fire.
    void finish_game(GameStatus status, const std::string& result);

    /// Fire `on_game_started` on every listener. Called from `join()`
    /// (and from the AI-game constructor path via a direct emit).
    /// Same mutex discipline as `emit_completed` below.
    void emit_started(const GameStarted& ev);

    /// Fire `on_game_completed` on every listener. See `finish_game`.
    void emit_completed(const GameCompleted& ev);

    /// Build the immutable snapshot of this room's terminal state
    /// into `out`. Out-param (rather than return-by-value) so this
    /// header can keep `GameSnapshot` forward-declared and avoid a
    /// game_room.h ⇄ game_snapshot.h include cycle. MUST be called
    /// with `mutex_` held.
    void build_snapshot_locked(GameSnapshot& out) const;

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

    // AI game fields
    bool                is_ai_        = false;
    bool                is_tournament_reserved_ = false;
    bool                reserved_open_ = false;
    int64_t             tournament_id_ = 0;
    int64_t             pairing_id_ = 0;
    AIDifficulty        ai_difficulty_ = AIDifficulty::MEDIUM;
    Color               ai_color_     = Color::BLACK;

    PlayerSlot          white_;
    PlayerSlot          black_;

    std::vector<MoveRecord> move_history_;
    std::optional<Color> draw_offer_from_;
    std::chrono::steady_clock::time_point draw_offer_created_at_{};
    std::optional<Color> rematch_offer_from_;
    std::chrono::steady_clock::time_point rematch_offer_created_at_{};

    /// Connection fds currently watching this room. Guarded by mutex_. Kept
    /// as std::vector because spectator counts stay small; a set-based lookup
    /// only wins past a few hundred watchers per room.
    std::vector<int>    spectator_fds_;

    /// Timestamp when the game started (first move)
    std::chrono::steady_clock::time_point game_start_time_;

    /// Wall-clock timestamps for ISO 8601 persistence.
    std::chrono::system_clock::time_point wall_start_;
    std::chrono::system_clock::time_point wall_end_;

    /// LLD-6.3: injectable time source. Defaults to the process-wide
    /// SystemClock — production behaviour is byte-identical. Tests
    /// call `set_clock(&fake)` to drive Fischer decrement and
    /// timeout detection deterministically.
    application::ports::Clock* clock_ = &application::ports::default_clock();

    /// LLD-6.4: monotonically-increasing revision. Bumped inside the
    /// room mutex by every mutating public method. Read via
    /// `revision()` (atomic acquire; no mutex required).
    std::atomic<uint64_t> revision_{0};

public:
    /// Inject a Clock (test seam). Must be called before any move
    /// timing runs. The pointer must outlive the room.
    void set_clock(application::ports::Clock* c) { clock_ = c; }

    /// LLD-6.4: monotonically-increasing revision. Bumped by every
    /// mutating public method (join, submit_move, submit_move_ai,
    /// resign, on_disconnect, on_reconnect, add_spectator,
    /// remove_spectator, finish_game). A caller that queues a job
    /// with a snapshot in hand can re-read this before applying the
    /// result and discard on mismatch. Public because a follow-up
    /// slice that introduces async move selection reads it from
    /// outside the room; the field itself is atomic so the read
    /// need not take the room mutex.
    uint64_t revision() const { return revision_.load(std::memory_order_acquire); }

private:

    /// LLD-4.1 — event listeners. Snapshot-copied before invoke so a
    /// listener callback can safely mutate this vector without racing
    /// the iteration loop.
    std::vector<GameEventListenerPtr> listeners_;

    /// LLD-4.1 — a terminal-transition event queued by `finish_game`
    /// under the lock, drained by `LockAndDrain::~LockAndDrain` after
    /// the lock releases. `mutable` so const accessors (which cannot
    /// call `finish_game` but still use `LockAndDrain`) can pass a
    /// `const GameRoom&` to the guard; drain will find these fields
    /// empty in that case and do nothing.
    mutable std::unique_ptr<GameCompleted>    pending_completed_;
    mutable std::vector<GameEventListenerPtr> pending_listeners_snapshot_;
    mutable std::function<void()>             pending_before_completion_;

    /// RAII helper: every public GameRoom method uses this instead of a
    /// plain `std::lock_guard`. On destruction it (1) captures any
    /// pending completion event into locals, (2) releases the mutex,
    /// (3) invokes each listener's `on_game_completed`. This guarantees
    /// listener callbacks run OUTSIDE the room lock without every
    /// caller having to remember an "unlock-then-emit" dance. A
    /// listener that throws is caught + logged; other listeners still
    /// fire (plan-doc "listener failure isolation" requirement).
    class LockAndDrain {
    public:
        explicit LockAndDrain(const GameRoom& room);
        ~LockAndDrain();
        LockAndDrain(const LockAndDrain&) = delete;
        LockAndDrain& operator=(const LockAndDrain&) = delete;
    private:
        const GameRoom&              room_;
        std::unique_lock<std::mutex> lock_;
    };
    friend class LockAndDrain;
};

} // namespace game
} // namespace chess
