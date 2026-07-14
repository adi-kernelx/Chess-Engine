/**
 * notation.h — Chess notation parsing and generation
 *
 * Supports:
 *   - SAN (Standard Algebraic Notation): "Nf3", "exd5", "O-O", "e8=Q", "Qd1+"
 *   - PGN (Portable Game Notation): full game import/export
 *
 * SAN parsing requires a Board and legal move list to disambiguate (e.g.,
 * when two rooks can reach the same square, SAN uses "Rae1" vs "Rfe1").
 */

#pragma once

#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include <string>
#include <vector>

namespace chess {
namespace notation {

// ============================================================
// SAN — Standard Algebraic Notation
// ============================================================

/// Convert a Move to SAN string (e.g., "Nf3", "exd5", "O-O-O", "e8=Q+").
/// Requires the board state BEFORE the move is applied (to determine
/// piece type, disambiguation, check/checkmate indicators).
std::string move_to_san(const Board& board, const Move& move);

/// Parse a SAN string into a Move, using the board's current position
/// to resolve ambiguities. Returns a null Move (from==to==0) on failure.
Move san_to_move(const Board& board, const std::string& san);

// ============================================================
// PGN — Portable Game Notation
// ============================================================

/// Header tag pair (e.g., [Event "Casual Game"])
struct PgnTag {
    std::string name;
    std::string value;
};

/// A complete PGN game: headers + move list
struct PgnGame {
    std::vector<PgnTag> tags;
    std::vector<Move>   moves;
    std::string         result;  // "1-0", "0-1", "1/2-1/2", "*"
};

/// Export a game (from the starting position) to PGN format.
/// The moves vector should contain legal moves to be played in sequence.
std::string export_pgn(const std::vector<PgnTag>& tags,
                       const std::vector<Move>& moves,
                       const std::string& result);

/// Parse a PGN string into a PgnGame.  Replays all SAN moves on a board
/// starting from the standard initial position (or a FEN tag if present).
/// Returns an empty PgnGame on parse failure.
PgnGame import_pgn(const std::string& pgn);

} // namespace notation
} // namespace chess
