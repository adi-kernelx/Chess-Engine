/**
 * evaluator.cpp — Static position evaluation
 *
 * SIX evaluation components, each explained in detail:
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * 1. MATERIAL BALANCE (Phase 6.1)
 *    Standard centipawn values. King gets a sentinel value (20000)
 *    so material imbalances involving the king dominate the score.
 *
 * 2. PIECE-SQUARE TABLES (Phase 6.1)
 *    64-entry tables per piece type based on Michniewski's Simplified
 *    Evaluation Function. Mirrored for Black via sq^56.
 *
 * 3. KING SAFETY (Phase 6.2)
 *    Counts the pawn shield around the king. A castled king behind
 *    3 pawns gets a bonus; each missing pawn in the shield is penalized.
 *    Also penalizes kings on open/semi-open files (no friendly pawn).
 *
 * 4. PAWN STRUCTURE (Phase 6.2)
 *    Detects three structural weaknesses:
 *    - Doubled pawns: two+ pawns of same color on one file (-15cp each)
 *    - Isolated pawns: no friendly pawns on adjacent files (-20cp)
 *    - Passed pawns: no opposing pawn can block/capture (+20cp, scaled by rank)
 *
 * 5. MOBILITY (Phase 6.2)
 *    Counts pseudo-legal moves for each side. More mobility = more
 *    options = better position. Weighted at 4cp per move difference.
 *    Uses pseudo-legal (not legal) for speed, since generating legal
 *    moves would call make/undo internally.
 *
 * 6. BISHOP PAIR (Phase 6.2)
 *    Having both bishops gives a +30cp bonus. Two bishops cover all
 *    64 squares and complement each other in open positions.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 * Score is returned from the perspective of the side to move (negamax-style).
 */

#include "chess/evaluator.h"
#include "chess/board.h"
#include "chess/move_gen.h"
#include "core/types.h"

namespace chess {
namespace engine {

// ============================================================
// Material values (centipawns)
// ============================================================

static const int PIECE_VALUE[7] = {
    100,   // PAWN
    320,   // KNIGHT
    330,   // BISHOP
    500,   // ROOK
    900,   // QUEEN
    20000, // KING  (never captured; large to dominate search)
    0      // NONE
};

// ============================================================
// Piece-Square Tables — from White's perspective
//
// Layout: index 0 = a1, index 7 = h1, index 56 = a8, index 63 = h8
// (identical to the Board's square layout)
// ============================================================

// Pawns: reward advancing, penalize non-developing edge pawns
static const int PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

// Knights: reward centralization, penalize rim and corner
static const int PST_KNIGHT[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50
};

// Bishops: reward diagonals and development, avoid edges
static const int PST_BISHOP[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20
};

// Rooks: reward 7th rank, open files
static const int PST_ROOK[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0
};

// Queens: avoid early development to the edge
static const int PST_QUEEN[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20
};

// King — middlegame: castle and stay safe, avoid center
static const int PST_KING_MG[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20
};

// ============================================================
// Evaluation tuning constants
// ============================================================

// King safety
static const int PAWN_SHIELD_BONUS     = 10;   // Per pawn in the king's shield
static const int KING_OPEN_FILE_PENALTY = -25;  // King on a file with no friendly pawn

// Pawn structure
static const int DOUBLED_PAWN_PENALTY  = -15;  // Per extra pawn on same file
static const int ISOLATED_PAWN_PENALTY = -20;  // Pawn with no friendly pawns on adjacent files
static const int PASSED_PAWN_BONUS[8]  = {      // Bonus by rank (from White's perspective)
    0, 10, 15, 25, 40, 60, 90, 0              // rank 0 and 7 are never occupied by pawns
};

// Mobility
static const int MOBILITY_WEIGHT       = 4;    // Centipawns per pseudo-legal move

// Bishop pair
static const int BISHOP_PAIR_BONUS     = 30;

// ============================================================
// PST lookup helpers
// ============================================================

static int pst_bonus(PieceType type, Color color, Square sq) {
    const int idx = (color == Color::BLACK) ? (sq ^ 56) : static_cast<int>(sq);

    switch (type) {
        case PieceType::PAWN:   return PST_PAWN[idx];
        case PieceType::KNIGHT: return PST_KNIGHT[idx];
        case PieceType::BISHOP: return PST_BISHOP[idx];
        case PieceType::ROOK:   return PST_ROOK[idx];
        case PieceType::QUEEN:  return PST_QUEEN[idx];
        case PieceType::KING:   return PST_KING_MG[idx];
        default:                return 0;
    }
}

// ============================================================
// King Safety
//
// The king's pawn shield consists of the 2-3 pawns directly in
// front of it. For a castled king on g1, the shield is f2, g2, h2.
// Missing shield pawns expose the king to attacks.
//
// Additionally, a king sitting on an open file (no friendly pawn
// on that file at all) is vulnerable to rook/queen attacks.
// ============================================================

/**
 * Evaluate king safety for one side.
 * @param board     Current position
 * @param color     Side whose king safety to evaluate
 * @param king_sq   Square the king is on
 * @return          King safety bonus (positive = safer)
 */
static int evaluate_king_safety(const Board& board, Color color, Square king_sq) {
    int score = 0;
    const int king_file = file_of(king_sq);
    const int king_rank = rank_of(king_sq);

    // Direction of the pawn shield: White pawns are "ahead" (higher rank), Black "ahead" (lower rank)
    const int shield_rank = (color == Color::WHITE) ? (king_rank + 1) : (king_rank - 1);

    // Only evaluate pawn shield if king is on ranks 0-1 (White) or 6-7 (Black),
    // i.e., the king hasn't wandered into the center
    const bool king_is_home = (color == Color::WHITE) ? (king_rank <= 1) : (king_rank >= 6);

    if (king_is_home && shield_rank >= 0 && shield_rank <= 7) {
        // Check the 2-3 squares directly in front of the king
        for (int f = std::max(0, king_file - 1); f <= std::min(7, king_file + 1); ++f) {
            const Square shield_sq = make_square(shield_rank, f);
            const Piece p = board.piece_at(shield_sq);
            if (p.type == PieceType::PAWN && p.color == color) {
                score += PAWN_SHIELD_BONUS;
            }
        }
    }

    // Open file penalty: check if any friendly pawn exists on the king's file
    bool has_pawn_on_file = false;
    for (int r = 0; r < 8; ++r) {
        const Piece p = board.piece_at(make_square(r, king_file));
        if (p.type == PieceType::PAWN && p.color == color) {
            has_pawn_on_file = true;
            break;
        }
    }
    if (!has_pawn_on_file) {
        score += KING_OPEN_FILE_PENALTY;
    }

    return score;
}

// ============================================================
// Pawn Structure
//
// Structural weaknesses in pawn formations are long-term problems
// that persist for many moves. Even a single isolated pawn can be
// a liability in the endgame.
//
// Doubled pawns: Two or more pawns of the same color on the same file.
//   They block each other from advancing and can't protect each other.
//
// Isolated pawns: A pawn with no friendly pawns on either adjacent file.
//   It must be defended by pieces (expensive) rather than pawns (free).
//
// Passed pawns: A pawn with no opposing pawns on the same or adjacent files
//   that could block or capture it. Passed pawns can promote and are very
//   dangerous, especially as they advance toward the 8th rank.
// ============================================================

/**
 * Evaluate pawn structure for one side.
 * @param board  Current position
 * @param color  Side whose pawns to evaluate
 * @return       Pawn structure score (positive = good structure)
 */
static int evaluate_pawn_structure(const Board& board, Color color) {
    int score = 0;

    // Count pawns per file for both colors
    int own_pawns_on_file[8] = {0};
    int opp_pawns_on_file[8] = {0};
    const Color opp_color = opposite_color(color);

    for (Square sq = 0; sq < NUM_SQUARES; ++sq) {
        const Piece p = board.piece_at(sq);
        if (p.type != PieceType::PAWN) continue;
        const int f = file_of(sq);
        if (p.color == color) {
            own_pawns_on_file[f]++;
        } else {
            opp_pawns_on_file[f]++;
        }
    }

    // Now scan each pawn of this color for structural features
    for (Square sq = 0; sq < NUM_SQUARES; ++sq) {
        const Piece p = board.piece_at(sq);
        if (p.type != PieceType::PAWN || p.color != color) continue;

        const int f = file_of(sq);
        const int r = rank_of(sq);

        // Doubled pawn: if there are 2+ pawns on this file, each extra is penalized.
        // We penalize each pawn beyond the first (so 2 pawns = 1 penalty, 3 pawns = 2 penalties).
        // To avoid double-counting, only penalize if this isn't the "first" pawn on the file.
        // Simple approach: penalize every pawn on a file that has >1 pawn, then divide by count.
        // Even simpler: just penalize if own_pawns_on_file[f] > 1 (each doubled pawn gets the penalty).
        if (own_pawns_on_file[f] > 1) {
            score += DOUBLED_PAWN_PENALTY;
        }

        // Isolated pawn: no friendly pawns on adjacent files
        const bool has_left  = (f > 0) && (own_pawns_on_file[f - 1] > 0);
        const bool has_right = (f < 7) && (own_pawns_on_file[f + 1] > 0);
        if (!has_left && !has_right) {
            score += ISOLATED_PAWN_PENALTY;
        }

        // Passed pawn: no opposing pawns on this file or adjacent files
        // that are ahead of (or equal to) this pawn's rank.
        // For White: "ahead" means higher rank. For Black: lower rank.
        bool is_passed = true;
        for (int cf = std::max(0, f - 1); cf <= std::min(7, f + 1); ++cf) {
            // Check all squares on this file ahead of the pawn
            if (color == Color::WHITE) {
                for (int cr = r + 1; cr <= 7; ++cr) {
                    const Piece blocker = board.piece_at(make_square(cr, cf));
                    if (blocker.type == PieceType::PAWN && blocker.color == opp_color) {
                        is_passed = false;
                        break;
                    }
                }
            } else {
                for (int cr = r - 1; cr >= 0; --cr) {
                    const Piece blocker = board.piece_at(make_square(cr, cf));
                    if (blocker.type == PieceType::PAWN && blocker.color == opp_color) {
                        is_passed = false;
                        break;
                    }
                }
            }
            if (!is_passed) break;
        }

        if (is_passed) {
            // Bonus scaled by how far the pawn has advanced
            const int effective_rank = (color == Color::WHITE) ? r : (7 - r);
            score += PASSED_PAWN_BONUS[effective_rank];
        }
    }

    return score;
}

// ============================================================
// Mobility
//
// The number of squares a side can reach (pseudo-legal moves)
// correlates with piece activity and control. A side with more
// mobility has more tactical and strategic options.
//
// We use pseudo-legal move count (not legal) because generating
// legal moves is expensive (requires make/undo per move). The
// approximation is good enough for evaluation purposes.
// ============================================================

/**
 * Count pseudo-legal moves for a given side.
 * Temporarily sets side_to_move to the requested color.
 * @param board  Board position (will be modified and restored)
 * @param color  Side to count mobility for
 * @return       Number of pseudo-legal moves
 */
static int count_mobility(Board& board, Color color) {
    const Color original = board.side_to_move();
    board.set_side_to_move(color);
    const int count = static_cast<int>(move_gen::generate_pseudo_legal_moves(board).size());
    board.set_side_to_move(original);
    return count;
}

// ============================================================
// Bishop Pair
//
// Two bishops complement each other — one controls light squares,
// the other dark squares. Together they cover the entire board.
// This is especially valuable in open positions. A typical bonus
// is 25-50 centipawns; we use 30.
// ============================================================

static int count_bishops(const Board& board, Color color) {
    int count = 0;
    for (Square sq = 0; sq < NUM_SQUARES; ++sq) {
        const Piece p = board.piece_at(sq);
        if (p.type == PieceType::BISHOP && p.color == color) {
            ++count;
        }
    }
    return count;
}

// ============================================================
// Main evaluation function — detailed breakdown
// ============================================================

EvalBreakdown evaluate_detailed(const Board& board) {
    EvalBreakdown result;

    int white_material = 0, black_material = 0;
    int white_pst = 0, black_pst = 0;

    // ── Pass 1: Material + PST ──
    for (Square sq = 0; sq < NUM_SQUARES; ++sq) {
        const Piece p = board.piece_at(sq);
        if (p.is_none()) continue;

        const int type_idx = static_cast<int>(p.type);
        const int material = PIECE_VALUE[type_idx];
        const int pst      = pst_bonus(p.type, p.color, sq);

        if (p.color == Color::WHITE) {
            white_material += material;
            white_pst += pst;
        } else {
            black_material += material;
            black_pst += pst;
        }
    }
    result.material = white_material - black_material;
    result.pst      = white_pst - black_pst;

    // ── Pass 2: King Safety ──
    const Square white_king = board.find_king(Color::WHITE);
    const Square black_king = board.find_king(Color::BLACK);

    const int white_king_safety = (white_king != NO_SQUARE)
        ? evaluate_king_safety(board, Color::WHITE, white_king) : 0;
    const int black_king_safety = (black_king != NO_SQUARE)
        ? evaluate_king_safety(board, Color::BLACK, black_king) : 0;
    result.king_safety = white_king_safety - black_king_safety;

    // ── Pass 3: Pawn Structure ──
    const int white_pawn_struct = evaluate_pawn_structure(board, Color::WHITE);
    const int black_pawn_struct = evaluate_pawn_structure(board, Color::BLACK);
    result.pawn_structure = white_pawn_struct - black_pawn_struct;

    // ── Pass 4: Mobility ──
    // Create a mutable copy for mobility counting (we need to flip side_to_move)
    Board board_copy = board;
    const int white_mobility = count_mobility(board_copy, Color::WHITE);
    const int black_mobility = count_mobility(board_copy, Color::BLACK);
    result.mobility = (white_mobility - black_mobility) * MOBILITY_WEIGHT;

    // ── Pass 5: Bishop Pair ──
    const int white_bishops = count_bishops(board, Color::WHITE);
    const int black_bishops = count_bishops(board, Color::BLACK);
    const int white_bp = (white_bishops >= 2) ? BISHOP_PAIR_BONUS : 0;
    const int black_bp = (black_bishops >= 2) ? BISHOP_PAIR_BONUS : 0;
    result.bishop_pair = white_bp - black_bp;

    // ── Total ──
    result.total = result.material + result.pst + result.king_safety
                 + result.pawn_structure + result.mobility + result.bishop_pair;

    return result;
}

// ============================================================
// Main evaluation function — returns score from side-to-move's perspective
// ============================================================

int evaluate(const Board& board) {
    const EvalBreakdown breakdown = evaluate_detailed(board);
    // Return score from the perspective of the side to move
    return (board.side_to_move() == Color::WHITE) ? breakdown.total : -breakdown.total;
}

} // namespace engine
} // namespace chess
