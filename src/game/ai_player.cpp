/**
 * ai_player.cpp — AI player implementation
 */

#include "game/ai_player.h"
#include <algorithm>

namespace chess {
namespace game {

// ============================================================
// Difficulty helpers
// ============================================================

AIDifficulty parse_difficulty(const std::string& str) {
    if (str == "easy")   return AIDifficulty::EASY;
    if (str == "medium") return AIDifficulty::MEDIUM;
    if (str == "hard")   return AIDifficulty::HARD;
    if (str == "max")    return AIDifficulty::MAX;
    return AIDifficulty::MEDIUM;
}

std::string difficulty_name(AIDifficulty diff) {
    switch (diff) {
        case AIDifficulty::EASY:   return "Easy";
        case AIDifficulty::MEDIUM: return "Medium";
        case AIDifficulty::HARD:   return "Hard";
        case AIDifficulty::MAX:    return "Max";
        default:                   return "Medium";
    }
}

// Difficulty → (max_depth, time_limit_ms)
static std::pair<int, int> difficulty_params(AIDifficulty diff) {
    switch (diff) {
        case AIDifficulty::EASY:   return {2,   200};
        case AIDifficulty::MEDIUM: return {4,  1000};
        case AIDifficulty::HARD:   return {6,  3000};
        case AIDifficulty::MAX:    return {64, 5000};
        default:                   return {4,  1000};
    }
}

// ============================================================
// AIPlayer
// ============================================================

AIPlayer::AIPlayer() : engine_(16) {}  // 16 MB TT for AI

AIMove AIPlayer::compute_move(const Board& board, AIDifficulty difficulty) {
    auto [max_depth, time_ms] = difficulty_params(difficulty);

    engine_.set_position(board);
    auto result = engine_.search(time_ms, max_depth);

    AIMove ai_move;
    ai_move.from       = result.best_move.from;
    ai_move.to         = result.best_move.to;
    ai_move.promotion  = result.best_move.promo_type;
    ai_move.score      = result.score;
    ai_move.depth      = result.depth;
    ai_move.nodes      = result.nodes;
    ai_move.elapsed_ms = result.elapsed_ms;

    return ai_move;
}

} // namespace game
} // namespace chess
