/**
 * evaluator.h — Static position evaluator for the chess AI engine
 *
 * Returns a centipawn score from the perspective of the side to move:
 *   > 0  → position is good for side to move
 *   < 0  → position is bad for side to move
 *   = 0  → roughly equal
 *
 * Evaluation components:
 *   1. Material balance (weighted piece values)              — Phase 6.1
 *   2. Piece-square tables (reward good piece placement)     — Phase 6.1
 *   3. King safety (pawn shield, exposed king penalty)       — Phase 6.2
 *   4. Pawn structure (doubled, isolated, backward pawns)    — Phase 6.2
 *   5. Mobility (pseudo-legal move count bonus)              — Phase 6.2
 *   6. Bishop pair bonus                                     — Phase 6.2
 *
 * Centipawn values: pawn=100, knight=320, bishop=330, rook=500, queen=900
 */

#pragma once

#include "chess/board.h"

namespace chess {
namespace engine {

/**
 * Evaluate the position from the side-to-move's perspective.
 *
 * @param board  Current board position (read-only)
 * @return       Score in centipawns, positive = good for side to move
 */
int evaluate(const Board& board);

/**
 * Detailed evaluation breakdown for testing and tuning.
 * All values are WHITE_SCORE - BLACK_SCORE (absolute, not relative to side to move).
 */
struct EvalBreakdown {
    int material       = 0;   ///< Raw material balance (centipawns)
    int pst            = 0;   ///< Piece-square table bonuses
    int king_safety    = 0;   ///< Pawn shield, open file penalties
    int pawn_structure = 0;   ///< Doubled, isolated, backward pawn penalties; passed pawn bonus
    int mobility       = 0;   ///< Pseudo-legal move count difference
    int bishop_pair    = 0;   ///< Bonus for having both bishops
    int total          = 0;   ///< Sum of all components (white - black perspective)
};

/**
 * Evaluate with a detailed breakdown (used for testing and tuning).
 *
 * @param board  Current board position (read-only)
 * @return       EvalBreakdown with per-component scores (absolute, white-relative)
 */
EvalBreakdown evaluate_detailed(const Board& board);

} // namespace engine
} // namespace chess
