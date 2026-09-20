/**
 * game_room.cpp — Game room implementation
 *
 * Implements the authoritative game server logic:
 *   - Move validation using the chess rules engine
 *   - Fischer clock management (base time + increment per move)
 *   - Player connect/disconnect handling
 *   - Game state transitions (WAITING → IN_PROGRESS → FINISHED)
 *   - Move history tracking for PGN export
 */

#include "game/game_room.h"

#include <exception>
#include <sstream>
#include <iomanip>
#include <ctime>

#include "core/logger.h"
#include "game/game_events.h"
#include "game/game_snapshot.h"

namespace chess {
namespace game {

namespace {
/// Format a system_clock time_point as ISO 8601 UTC string.
std::string format_iso8601(const std::chrono::system_clock::time_point& tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
    gmtime_r(&time_t_val, &tm_val);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
    return std::string(buf);
}
} // namespace

// ============================================================
// TimeControl
// ============================================================

std::string TimeControl::to_string() const {
    return std::to_string(base_time_ms / 1000) + "+" +
           std::to_string(increment_ms / 1000);
}

// ============================================================
// GameRoom — Construction
// ============================================================

GameRoom::GameRoom(GameId id, PlayerId creator_id, const std::string& creator_name,
                   int creator_fd, const TimeControl& tc,
                   int64_t db_player_id, int elo)
    : id_(id)
    , state_(RoomState::WAITING)
    , board_(Board::starting_position())
    , time_control_(tc)
    , result_("*")
{
    // Creator is seated as White
    white_.connection_fd  = creator_fd;
    white_.player_id      = creator_id;
    white_.db_player_id   = db_player_id;
    white_.username       = creator_name;
    white_.elo            = elo;
    white_.remaining_ms   = tc.base_time_ms;
    white_.connected      = true;
}

GameRoom::GameRoom(GameId id, PlayerId creator_id, const std::string& creator_name,
                   int creator_fd, const TimeControl& tc, AIDifficulty difficulty)
    : id_(id)
    , state_(RoomState::IN_PROGRESS)  // AI games start immediately
    , board_(Board::starting_position())
    , time_control_(tc)
    , result_("*")
    , is_ai_(true)
    , ai_difficulty_(difficulty)
    , ai_color_(Color::BLACK)
{
    // Human is seated as White
    white_.connection_fd = creator_fd;
    white_.player_id     = creator_id;
    white_.username      = creator_name;
    white_.remaining_ms  = tc.base_time_ms;
    white_.connected     = true;

    // AI is seated as Black with a sentinel fd
    black_.connection_fd = -2;  // Sentinel: -2 = AI player (distinct from -1 = empty)
    black_.player_id     = 0;
    black_.username      = "AI (" + difficulty_name(difficulty) + ")";
    black_.remaining_ms  = tc.base_time_ms;
    black_.connected     = true;

    // Start White's clock
    game_start_time_ = std::chrono::steady_clock::now();
    wall_start_ = std::chrono::system_clock::now();
    white_.clock_start = game_start_time_;
}

GameRoom::~GameRoom() = default;

// ============================================================
// LLD-4.1 — LockAndDrain: per-method critical section + post-unlock
// listener emission for terminal-transition events.
// ============================================================

GameRoom::LockAndDrain::LockAndDrain(const GameRoom& room)
    : room_(room), lock_(room.mutex_) {}

GameRoom::LockAndDrain::~LockAndDrain() {
    // Snapshot any pending completion under the lock, then release.
    std::unique_ptr<GameCompleted>    ev;
    std::vector<GameEventListenerPtr> to_fire;
    if (room_.pending_completed_) {
        ev = std::move(room_.pending_completed_);
        to_fire = std::move(room_.pending_listeners_snapshot_);
    }
    lock_.unlock();

    if (!ev) return;
    for (const auto& listener : to_fire) {
        if (!listener) continue;
        try {
            listener->on_game_completed(*ev);
        } catch (const std::exception& e) {
            core::Logger::warn("game", "GameRoom",
                "Listener threw on on_game_completed for game "
                + std::to_string(ev->snapshot.room_id) + ": " + e.what());
        } catch (...) {
            core::Logger::warn("game", "GameRoom",
                "Listener threw non-standard exception on on_game_completed for game "
                + std::to_string(ev->snapshot.room_id));
        }
    }
}

// ============================================================
// LLD-4.1 — listener registration + snapshot + emit helpers
// ============================================================

void GameRoom::add_listener(GameEventListenerPtr listener) {
    if (!listener) return;
    LockAndDrain lock(*this);
    listeners_.push_back(std::move(listener));
}

bool GameRoom::remove_listener(const GameEventListenerPtr& listener) {
    LockAndDrain lock(*this);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->get() == listener.get()) {
            listeners_.erase(it);
            return true;
        }
    }
    return false;
}

void GameRoom::build_snapshot_locked(GameSnapshot& out) const {
    // Caller holds mutex_.
    out.room_id            = id_;
    out.is_ai_game         = is_ai_;
    out.status             = game_status_;
    out.result             = result_;
    out.termination_reason.clear();  // populated by the caller/service layer
                                     // via status_to_reason (LLD-4.2 puts the
                                     // string on the snapshot too).
    out.time_control       = time_control_;
    out.started_at_iso     = format_iso8601(wall_start_);
    out.ended_at_iso       = format_iso8601(wall_end_);

    out.white.db_player_id = white_.db_player_id;
    out.white.player_id    = white_.player_id;
    out.white.username     = white_.username;
    out.white.elo          = white_.elo;
    out.white.remaining_ms = white_.remaining_ms;

    out.black.db_player_id = black_.db_player_id;
    out.black.player_id    = black_.player_id;
    out.black.username     = black_.username;
    out.black.elo          = black_.elo;
    out.black.remaining_ms = black_.remaining_ms;

    out.history            = move_history_;
    out.move_count         = static_cast<int>(move_history_.size());
    out.completion_uuid.clear();  // populated by LLD-4.2 completion service.
}

void GameRoom::emit_started(const GameStarted& ev) {
    // Snapshot listeners under the lock, invoke outside.
    std::vector<GameEventListenerPtr> to_fire;
    {
        LockAndDrain lock(*this);
        to_fire = listeners_;
    }
    for (const auto& listener : to_fire) {
        if (!listener) continue;
        try {
            listener->on_game_started(ev);
        } catch (const std::exception& e) {
            core::Logger::warn("game", "GameRoom",
                "Listener threw on on_game_started for game "
                + std::to_string(ev.room_id) + ": " + e.what());
        } catch (...) {
            core::Logger::warn("game", "GameRoom",
                "Listener threw non-standard exception on on_game_started for game "
                + std::to_string(ev.room_id));
        }
    }
}

void GameRoom::emit_completed(const GameCompleted& ev) {
    // Kept for symmetry / test use. In production the terminal event
    // flows through LockAndDrain instead — see finish_game.
    std::vector<GameEventListenerPtr> to_fire;
    {
        LockAndDrain lock(*this);
        to_fire = listeners_;
    }
    for (const auto& listener : to_fire) {
        if (!listener) continue;
        try { listener->on_game_completed(ev); }
        catch (const std::exception& e) {
            core::Logger::warn("game", "GameRoom",
                "Listener threw on on_game_completed for game "
                + std::to_string(ev.snapshot.room_id) + ": " + e.what());
        } catch (...) {}
    }
}

// ============================================================
// Join — Second player enters the room
// ============================================================

bool GameRoom::join(PlayerId player_id, const std::string& player_name, int connection_fd,
                    int64_t db_player_id, int elo) {
    GameStarted ev;
    {
        LockAndDrain lock(*this);

        if (state_ != RoomState::WAITING) return false;
        if (!black_.is_empty()) return false;

        // Seat the joiner as Black
        black_.connection_fd  = connection_fd;
        black_.player_id      = player_id;
        black_.db_player_id   = db_player_id;
        black_.username       = player_name;
        black_.elo            = elo;
        black_.remaining_ms   = time_control_.base_time_ms;
        black_.connected      = true;

        // Start the game — White's clock begins ticking
        state_ = RoomState::IN_PROGRESS;
        game_start_time_ = std::chrono::steady_clock::now();
        wall_start_ = std::chrono::system_clock::now();
        white_.clock_start = game_start_time_;

        // LLD-4.1 — assemble the event under the lock; fire callbacks
        // outside the lock a few lines below.
        ev.room_id     = id_;
        ev.white_db_id = white_.db_player_id;
        ev.black_db_id = black_.db_player_id;
        ev.is_ai_game  = false;
    }
    emit_started(ev);
    return true;
}

// ============================================================
// Submit Move — The core game loop
// ============================================================

GameRoom::MoveResult GameRoom::submit_move(int connection_fd, Square from, Square to,
                                           PieceType promo_type) {
    LockAndDrain lock(*this);

    MoveResult result;
    result.success = false;
    result.game_status = game_status_;

    // --- Precondition checks ---
    if (state_ != RoomState::IN_PROGRESS) {
        result.error = "Game is not in progress";
        return result;
    }

    Color player_color = color_of(connection_fd);
    if (player_color == Color::NONE) {
        result.error = "You are not a player in this game";
        return result;
    }

    if (player_color != board_.side_to_move()) {
        result.error = "It is not your turn";
        return result;
    }

    // --- Check the clock before accepting the move ---
    if (check_flag()) {
        std::string winner = (player_color == Color::WHITE) ? "0-1" : "1-0";
        finish_game(GameStatus::TIMEOUT, winner);
        result.game_status = GameStatus::TIMEOUT;
        result.error = "You have run out of time";
        return result;
    }

    // --- Find the matching legal move ---
    std::vector<Move> legal_moves = move_gen::generate_legal_moves(board_);
    Move matched_move;
    bool found = false;

    for (const Move& candidate : legal_moves) {
        if (candidate.from != from || candidate.to != to) continue;

        // If it's a promotion, match the promotion piece
        if (candidate.is_promotion()) {
            PieceType pt = (promo_type == PieceType::NONE) ? PieceType::QUEEN : promo_type;
            if (candidate.promo_type != pt) continue;
        }

        matched_move = candidate;
        found = true;
        break;
    }

    if (!found) {
        result.error = "Illegal move";
        return result;
    }

    // --- Generate SAN before making the move (SAN needs pre-move board) ---
    std::string san = notation::move_to_san(board_, matched_move);

    // --- Calculate think time ---
    auto now = std::chrono::steady_clock::now();
    PlayerSlot& current_player = (player_color == Color::WHITE) ? white_ : black_;
    int think_time_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - current_player.clock_start).count()
    );

    // --- Execute the move on the authoritative board ---
    board_.make_move(matched_move);

    // --- Record the move ---
    move_history_.push_back({matched_move, san, think_time_ms});

    // --- Switch the clock ---
    switch_clock();

    // --- Check game status after the move ---
    GameStatus status = move_gen::get_game_status(board_);

    if (status != GameStatus::ONGOING) {
        std::string game_result;
        switch (status) {
            case GameStatus::CHECKMATE:
                // The side to move has been checkmated — the other side wins
                game_result = (board_.side_to_move() == Color::WHITE) ? "0-1" : "1-0";
                break;
            case GameStatus::STALEMATE:
            case GameStatus::DRAW_FIFTY_MOVE:
            case GameStatus::DRAW_INSUFFICIENT_MATERIAL:
            case GameStatus::DRAW_THREEFOLD_REPETITION:
                game_result = "1/2-1/2";
                break;
            default:
                game_result = "*";
                break;
        }
        finish_game(status, game_result);
    }

    // --- Populate the result ---
    result.success      = true;
    result.san          = san;
    result.game_status  = status;
    result.white_time_ms = white_.remaining_ms;
    result.black_time_ms = black_.remaining_ms;

    return result;
}

// ============================================================
// Submit Move (AI) — bypasses connection_fd check
// ============================================================

GameRoom::MoveResult GameRoom::submit_move_ai(Square from, Square to,
                                               PieceType promo_type) {
    LockAndDrain lock(*this);

    MoveResult result;
    result.success = false;
    result.game_status = game_status_;

    if (state_ != RoomState::IN_PROGRESS) {
        result.error = "Game is not in progress";
        return result;
    }

    if (!is_ai_) {
        result.error = "Not an AI game";
        return result;
    }

    if (board_.side_to_move() != ai_color_) {
        result.error = "It is not the AI's turn";
        return result;
    }

    // Find the matching legal move
    std::vector<Move> legal_moves = move_gen::generate_legal_moves(board_);
    Move matched_move;
    bool found = false;

    for (const Move& candidate : legal_moves) {
        if (candidate.from != from || candidate.to != to) continue;
        if (candidate.is_promotion()) {
            PieceType pt = (promo_type == PieceType::NONE) ? PieceType::QUEEN : promo_type;
            if (candidate.promo_type != pt) continue;
        }
        matched_move = candidate;
        found = true;
        break;
    }

    if (!found) {
        result.error = "AI produced illegal move";
        return result;
    }

    // Generate SAN before making the move
    std::string san = notation::move_to_san(board_, matched_move);

    // Calculate think time for the AI
    auto now = std::chrono::steady_clock::now();
    PlayerSlot& ai_slot = (ai_color_ == Color::WHITE) ? white_ : black_;
    int think_time_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ai_slot.clock_start).count()
    );

    // Execute the move
    board_.make_move(matched_move);
    move_history_.push_back({matched_move, san, think_time_ms});
    switch_clock();

    // Check game status
    GameStatus status = move_gen::get_game_status(board_);
    if (status != GameStatus::ONGOING) {
        std::string game_result;
        switch (status) {
            case GameStatus::CHECKMATE:
                game_result = (board_.side_to_move() == Color::WHITE) ? "0-1" : "1-0";
                break;
            case GameStatus::STALEMATE:
            case GameStatus::DRAW_FIFTY_MOVE:
            case GameStatus::DRAW_INSUFFICIENT_MATERIAL:
            case GameStatus::DRAW_THREEFOLD_REPETITION:
                game_result = "1/2-1/2";
                break;
            default:
                game_result = "*";
                break;
        }
        finish_game(status, game_result);
    }

    result.success      = true;
    result.san          = san;
    result.game_status  = status;
    result.white_time_ms = white_.remaining_ms;
    result.black_time_ms = black_.remaining_ms;

    return result;
}

// ============================================================
// Resign
// ============================================================

bool GameRoom::resign(int connection_fd) {
    LockAndDrain lock(*this);

    if (state_ != RoomState::IN_PROGRESS) return false;

    Color player_color = color_of(connection_fd);
    if (player_color == Color::NONE) return false;

    std::string game_result = (player_color == Color::WHITE) ? "0-1" : "1-0";
    finish_game(GameStatus::RESIGNATION, game_result);
    return true;
}

// ============================================================
// Disconnect / Reconnect handling
// ============================================================

void GameRoom::on_disconnect(int connection_fd) {
    LockAndDrain lock(*this);

    if (white_.connection_fd == connection_fd) {
        white_.connected = false;
    } else if (black_.connection_fd == connection_fd) {
        black_.connected = false;
    }

    // If the game hasn't started yet and the creator leaves, mark finished
    if (state_ == RoomState::WAITING) {
        finish_game(GameStatus::RESIGNATION, "*");
    }
    // If the game is in progress, the clock keeps running.
    // If the player doesn't reconnect before their time runs out, they lose.
}

bool GameRoom::on_reconnect(PlayerId player_id, int new_fd) {
    LockAndDrain lock(*this);

    if (state_ == RoomState::FINISHED) return false;

    if (white_.player_id == player_id) {
        white_.connection_fd = new_fd;
        white_.connected = true;
        return true;
    } else if (black_.player_id == player_id) {
        black_.connection_fd = new_fd;
        black_.connected = true;
        return true;
    }

    return false;
}

// ============================================================
// Accessors
// ============================================================

GameId GameRoom::get_id() const {
    LockAndDrain lock(*this);
    return id_;
}

RoomState GameRoom::get_state() const {
    LockAndDrain lock(*this);
    return state_;
}

const Board& GameRoom::get_board() const {
    // Note: caller should hold the room's lock or ensure no concurrent modification.
    // For thread-safe access patterns, use the MoveResult returned by submit_move().
    return board_;
}

Color GameRoom::side_to_move() const {
    LockAndDrain lock(*this);
    return board_.side_to_move();
}

const TimeControl& GameRoom::get_time_control() const {
    return time_control_;  // Immutable after construction
}

std::string GameRoom::get_result_string() const {
    LockAndDrain lock(*this);
    return result_;
}

GameStatus GameRoom::get_game_status() const {
    LockAndDrain lock(*this);
    return game_status_;
}

bool GameRoom::is_ai_game() const {
    LockAndDrain lock(*this);
    return is_ai_;
}

AIDifficulty GameRoom::ai_difficulty() const {
    LockAndDrain lock(*this);
    return ai_difficulty_;
}

Color GameRoom::ai_color() const {
    LockAndDrain lock(*this);
    return ai_color_;
}

int GameRoom::get_player_fd(Color color) const {
    LockAndDrain lock(*this);
    if (color == Color::WHITE) return white_.connection_fd;
    if (color == Color::BLACK) return black_.connection_fd;
    return -1;
}

int GameRoom::get_opponent_fd(int my_fd) const {
    LockAndDrain lock(*this);
    if (white_.connection_fd == my_fd) return black_.connection_fd;
    if (black_.connection_fd == my_fd) return white_.connection_fd;
    return -1;
}

bool GameRoom::has_player(int connection_fd) const {
    LockAndDrain lock(*this);
    return white_.connection_fd == connection_fd || black_.connection_fd == connection_fd;
}

bool GameRoom::has_player_id(PlayerId pid) const {
    LockAndDrain lock(*this);
    return white_.player_id == pid || black_.player_id == pid;
}

int GameRoom::current_turn_fd() const {
    LockAndDrain lock(*this);
    if (state_ != RoomState::IN_PROGRESS) return -1;
    return (board_.side_to_move() == Color::WHITE) ? white_.connection_fd : black_.connection_fd;
}

void GameRoom::get_remaining_times(int& white_ms, int& black_ms) const {
    LockAndDrain lock(*this);

    white_ms = white_.remaining_ms;
    black_ms = black_.remaining_ms;

    // If the game is in progress, deduct elapsed time from the active player
    if (state_ == RoomState::IN_PROGRESS) {
        auto now = std::chrono::steady_clock::now();
        const PlayerSlot& active = (board_.side_to_move() == Color::WHITE) ? white_ : black_;
        int elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - active.clock_start).count()
        );

        if (board_.side_to_move() == Color::WHITE) {
            white_ms -= elapsed;
            if (white_ms < 0) white_ms = 0;
        } else {
            black_ms -= elapsed;
            if (black_ms < 0) black_ms = 0;
        }
    }
}

std::vector<MoveRecord> GameRoom::get_move_history() const {
    LockAndDrain lock(*this);
    return move_history_;
}

PlayerId GameRoom::get_player_id(Color color) const {
    LockAndDrain lock(*this);
    if (color == Color::WHITE) return white_.player_id;
    if (color == Color::BLACK) return black_.player_id;
    return 0;
}

int64_t GameRoom::get_db_player_id(Color color) const {
    LockAndDrain lock(*this);
    if (color == Color::WHITE) return white_.db_player_id;
    if (color == Color::BLACK) return black_.db_player_id;
    return 0;
}

std::string GameRoom::get_username(Color color) const {
    LockAndDrain lock(*this);
    if (color == Color::WHITE) return white_.username;
    if (color == Color::BLACK) return black_.username;
    return "";
}

int GameRoom::get_elo(Color color) const {
    LockAndDrain lock(*this);
    if (color == Color::WHITE) return white_.elo;
    if (color == Color::BLACK) return black_.elo;
    return 0;
}

std::string GameRoom::get_started_at_iso() const {
    LockAndDrain lock(*this);
    return format_iso8601(wall_start_);
}

std::string GameRoom::get_ended_at_iso() const {
    LockAndDrain lock(*this);
    return format_iso8601(wall_end_);
}

// ============================================================
// PGN Export
// ============================================================

std::string GameRoom::to_pgn() const {
    LockAndDrain lock(*this);

    std::vector<notation::PgnTag> tags;
    tags.push_back({"Event", "Online Game"});
    tags.push_back({"White", white_.username.empty() ? "Player 1" : white_.username});
    tags.push_back({"Black", black_.username.empty() ? "Player 2" : black_.username});
    tags.push_back({"Result", result_});
    tags.push_back({"TimeControl", time_control_.to_string()});

    // Collect raw moves
    std::vector<Move> moves;
    moves.reserve(move_history_.size());
    for (const auto& record : move_history_) {
        moves.push_back(record.move);
    }

    return notation::export_pgn(tags, moves, result_);
}

// ============================================================
// Spectators (Phase 9.1)
// ============================================================

bool GameRoom::add_spectator(int connection_fd) {
    LockAndDrain lock(*this);

    // Spectating is only meaningful once the game is live. A WAITING room
    // has no board activity to broadcast; a FINISHED room has nothing more
    // to send. The lobby's live-games listing already filters to
    // IN_PROGRESS, so a client hitting either edge is either racing or
    // misbehaving — refuse in both cases.
    if (state_ != RoomState::IN_PROGRESS) return false;

    // A player cannot spectate their own game. Silently refuse rather than
    // creating a duplicate broadcast recipient that would double-send every
    // move_made frame to a seat that already receives it.
    if (connection_fd == white_.connection_fd) return false;
    if (connection_fd == black_.connection_fd) return false;

    // Idempotent: re-adding is fine. This matters for the "spectate the same
    // game twice" corner case (a browser tab reconnecting behind a reload
    // may re-send spectate before its previous fd has been swept).
    for (int fd : spectator_fds_) {
        if (fd == connection_fd) return true;
    }
    spectator_fds_.push_back(connection_fd);
    return true;
}

bool GameRoom::remove_spectator(int connection_fd) {
    LockAndDrain lock(*this);
    for (auto it = spectator_fds_.begin(); it != spectator_fds_.end(); ++it) {
        if (*it == connection_fd) {
            spectator_fds_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<int> GameRoom::spectator_fds() const {
    // Copy under the lock, iterate outside. The alternative — exposing a
    // pointer/iterator under the lock — would tempt callers to fan out
    // send_json_to_fd() while still holding the room mutex, which would
    // block every other move on this room for the duration of the flush
    // (each flush is a syscall per spectator).
    LockAndDrain lock(*this);
    return spectator_fds_;
}

size_t GameRoom::spectator_count() const {
    LockAndDrain lock(*this);
    return spectator_fds_.size();
}

// ============================================================
// Internal helpers
// ============================================================

Color GameRoom::color_of(int connection_fd) const {
    // Note: caller must hold mutex_
    if (white_.connection_fd == connection_fd) return Color::WHITE;
    if (black_.connection_fd == connection_fd) return Color::BLACK;
    return Color::NONE;
}

void GameRoom::finish_game(GameStatus status, const std::string& result) {
    // Note: caller must hold mutex_ (via LockAndDrain in every public
    // caller). Queues a GameCompleted event; LockAndDrain's dtor fires
    // it AFTER the lock is released — see the class body in game_room.h
    // and its dtor above.
    state_       = RoomState::FINISHED;
    game_status_ = status;
    result_      = result;
    wall_end_    = std::chrono::system_clock::now();

    auto ev = std::make_unique<GameCompleted>();
    build_snapshot_locked(ev->snapshot);
    pending_completed_          = std::move(ev);
    pending_listeners_snapshot_ = listeners_;
}

void GameRoom::switch_clock() {
    // Note: caller must hold mutex_
    auto now = std::chrono::steady_clock::now();

    // The side that JUST moved (before board_.make_move toggled the turn)
    // is now board_.side_to_move()'s OPPONENT, because make_move already
    // flipped the turn. So the clock we need to stop belongs to the
    // opponent of the current side_to_move.
    Color just_moved = opposite_color(board_.side_to_move());

    PlayerSlot& mover = (just_moved == Color::WHITE) ? white_ : black_;
    PlayerSlot& next  = (just_moved == Color::WHITE) ? black_ : white_;

    // Deduct elapsed time from the mover's clock
    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mover.clock_start).count()
    );
    mover.remaining_ms -= elapsed;

    // Add increment
    mover.remaining_ms += time_control_.increment_ms;

    // Clamp to zero (shouldn't happen if we check_flag before, but safety)
    if (mover.remaining_ms < 0) mover.remaining_ms = 0;

    // Start the next player's clock
    next.clock_start = now;
}

bool GameRoom::check_flag() const {
    // Note: caller must hold mutex_
    if (state_ != RoomState::IN_PROGRESS) return false;

    auto now = std::chrono::steady_clock::now();
    const PlayerSlot& active = (board_.side_to_move() == Color::WHITE) ? white_ : black_;

    int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - active.clock_start).count()
    );

    return (active.remaining_ms - elapsed) <= 0;
}

} // namespace game
} // namespace chess
