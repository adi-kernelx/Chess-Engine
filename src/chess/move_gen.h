/**
 * move_gen.h — Chess move generation
 *
 * Provides functions to generate legal and pseudo-legal moves for a given board position.
 * Also provides functions for checking if a square is attacked, which is essential
 * for determining check and validating castling.
 */

#pragma once

#include "chess/board.h"
#include "chess/move.h"
#include <vector>

namespace chess {
namespace move_gen {

// ============================================================
// Move Generation
// ============================================================

/// Generates all strictly legal moves for the side to move.
/// This includes castling, en passant, and underpromotions.
std::vector<Move> generate_legal_moves(const Board& board);

/// Generates all pseudo-legal moves. 
/// A pseudo-legal move follows piece movement rules but might leave the king in check.
std::vector<Move> generate_pseudo_legal_moves(const Board& board);

// ============================================================
// Attack Detection
// ============================================================

/// Checks if a specific square is attacked by any piece of the given color.
bool is_square_attacked(const Board& board, Square sq, Color attacker_color);

/// Checks if the king of the given color is currently in check.
bool is_in_check(const Board& board, Color king_color);

/// Evaluates the current game status (ongoing, checkmate, stalemate, draws).
GameStatus get_game_status(const Board& board);

// ============================================================
// Perft (Performance Test)
// ============================================================

/// Recursively counts the number of legal leaf nodes at a given depth.
/// Used to verify the correctness of the move generator.
uint64_t perft(Board& board, int depth);

/// Prints the perft result for each initial legal move, followed by the total.
void divide(Board& board, int depth);

} // namespace move_gen
} // namespace chess
