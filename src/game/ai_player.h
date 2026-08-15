/**
 * ai_player.h — AI chess player wrapping the Engine for game room integration
 *
 * Provides configurable difficulty levels that map to search depth and time
 * limits. Used by GameHandler to compute AI moves when it's the bot's turn.
 */

#pragma once

#include "chess/board.h"
#include "chess/engine.h"
#include "core/types.h"
#include <string>

namespace chess {
namespace game {

/// Difficulty presets for the AI player
enum class AIDifficulty : uint8_t {
    EASY   = 0,  // Depth 2, 200ms
    MEDIUM = 1,  // Depth 4, 1000ms
    HARD   = 2,  // Depth 6, 3000ms
    MAX    = 3   // Depth 64, 5000ms
};

/// Convert a string ("easy", "medium", etc.) to AIDifficulty.
/// Returns MEDIUM on unrecognized input.
AIDifficulty parse_difficulty(const std::string& str);

/// Human-readable name for a difficulty level (e.g. "Medium").
std::string difficulty_name(AIDifficulty diff);

/// Result of an AI move computation
struct AIMove {
    Square    from      = NO_SQUARE;
    Square    to        = NO_SQUARE;
    PieceType promotion = PieceType::NONE;
    int       score     = 0;
    int       depth     = 0;
    long      nodes     = 0;
    int       elapsed_ms = 0;
};

/**
 * AIPlayer — wraps the chess Engine for use in game rooms.
 *
 * Owns a single Engine instance. Not thread-safe: create one per
 * AI game, or protect with external synchronization.
 */
class AIPlayer {
public:
    AIPlayer();

    /// Compute the best move for the given position and difficulty.
    /// Blocking call — returns when the engine finishes searching.
    AIMove compute_move(const Board& board, AIDifficulty difficulty);

private:
    engine::Engine engine_;
};

} // namespace game
} // namespace chess
