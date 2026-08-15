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
#include <sstream>

namespace chess {
namespace game {

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
                   int creator_fd, const TimeControl& tc)
    : id_(id)
    , state_(RoomState::WAITING)
    , board_(Board::starting_position())
    , time_control_(tc)
    , result_("*")
{
    // Creator is seated as White
    white_.connection_fd = creator_fd;
    white_.player_id     = creator_id;
    white_.username      = creator_name;
    white_.remaining_ms  = tc.base_time_ms;
    white_.connected     = true;
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
    white_.clock_start = game_start_time_;
}

// ============================================================
// Join — Second player enters the room
// ============================================================

bool GameRoom::join(PlayerId player_id, const std::string& player_name, int connection_fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != RoomState::WAITING) return false;
    if (!black_.is_empty()) return false;

    // Seat the joiner as Black
    black_.connection_fd = connection_fd;
    black_.player_id     = player_id;
    black_.username      = player_name;
    black_.remaining_ms  = time_control_.base_time_ms;
    black_.connected     = true;

    // Start the game — White's clock begins ticking
    state_ = RoomState::IN_PROGRESS;
    game_start_time_ = std::chrono::steady_clock::now();
    white_.clock_start = game_start_time_;

    return true;
}

// ============================================================
// Submit Move — The core game loop
// ============================================================

GameRoom::MoveResult GameRoom::submit_move(int connection_fd, Square from, Square to,
                                           PieceType promo_type) {
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);
    return id_;
}

RoomState GameRoom::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

const Board& GameRoom::get_board() const {
    // Note: caller should hold the room's lock or ensure no concurrent modification.
    // For thread-safe access patterns, use the MoveResult returned by submit_move().
    return board_;
}

Color GameRoom::side_to_move() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return board_.side_to_move();
}

const TimeControl& GameRoom::get_time_control() const {
    return time_control_;  // Immutable after construction
}

std::string GameRoom::get_result_string() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

GameStatus GameRoom::get_game_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return game_status_;
}

bool GameRoom::is_ai_game() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_ai_;
}

AIDifficulty GameRoom::ai_difficulty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ai_difficulty_;
}

Color GameRoom::ai_color() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ai_color_;
}

int GameRoom::get_player_fd(Color color) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (color == Color::WHITE) return white_.connection_fd;
    if (color == Color::BLACK) return black_.connection_fd;
    return -1;
}

int GameRoom::get_opponent_fd(int my_fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (white_.connection_fd == my_fd) return black_.connection_fd;
    if (black_.connection_fd == my_fd) return white_.connection_fd;
    return -1;
}

bool GameRoom::has_player(int connection_fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return white_.connection_fd == connection_fd || black_.connection_fd == connection_fd;
}

bool GameRoom::has_player_id(PlayerId pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return white_.player_id == pid || black_.player_id == pid;
}

int GameRoom::current_turn_fd() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RoomState::IN_PROGRESS) return -1;
    return (board_.side_to_move() == Color::WHITE) ? white_.connection_fd : black_.connection_fd;
}

void GameRoom::get_remaining_times(int& white_ms, int& black_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);

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
    std::lock_guard<std::mutex> lock(mutex_);
    return move_history_;
}

// ============================================================
// PGN Export
// ============================================================

std::string GameRoom::to_pgn() const {
    std::lock_guard<std::mutex> lock(mutex_);

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
// Internal helpers
// ============================================================

Color GameRoom::color_of(int connection_fd) const {
    // Note: caller must hold mutex_
    if (white_.connection_fd == connection_fd) return Color::WHITE;
    if (black_.connection_fd == connection_fd) return Color::BLACK;
    return Color::NONE;
}

void GameRoom::finish_game(GameStatus status, const std::string& result) {
    // Note: caller must hold mutex_
    state_       = RoomState::FINISHED;
    game_status_ = status;
    result_      = result;
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
