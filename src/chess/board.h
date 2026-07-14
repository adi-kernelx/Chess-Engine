/**
 * board.h — Chess board representation and state management
 *
 * Uses an 8x8 array (mailbox) representation for clarity and correctness.
 * Bitboard optimization can be layered on top in Phase 10 without changing
 * the public interface.
 *
 * Board state includes:
 *   - 64-square piece array
 *   - Side to move
 *   - Castling rights (4-bit mask)
 *   - En passant target square
 *   - Halfmove clock (for 50-move rule)
 *   - Fullmove counter
 *
 * Supports FEN import/export for testing and game serialization.
 */

#pragma once

#include "core/types.h"
#include "chess/move.h"
#include <string>
#include <array>

namespace chess {

/// The standard starting position in FEN notation
constexpr const char* STARTING_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

class Board {
public:
    // ============================================================
    // Construction
    // ============================================================

    /// Default-constructs an empty board (all squares NONE).
    /// Call set_from_fen() or set_starting_position() to populate.
    Board();

    /// Construct the standard starting position
    static Board starting_position();

    // ============================================================
    // FEN — Forsyth-Edwards Notation
    // ============================================================

    /// Load board state from a FEN string.
    /// Returns false and leaves the board in an unspecified state on parse failure.
    bool set_from_fen(const std::string& fen);

    /// Export the current board state as a FEN string.
    std::string to_fen() const;

    // ============================================================
    // Piece access
    // ============================================================

    /// Get the piece at a given square. Returns NONE/NONE for empty squares.
    Piece piece_at(Square sq) const;

    /// Place a piece on a square (overwrites whatever was there).
    void set_piece(Square sq, Piece piece);

    /// Remove a piece from a square (sets it to NONE/NONE).
    void clear_square(Square sq);

    // ============================================================
    // Board state accessors
    // ============================================================

    Color    side_to_move()        const { return side_to_move_; }
    uint8_t  castling_rights()     const { return castling_rights_; }
    Square   en_passant_square()   const { return en_passant_sq_; }
    int      halfmove_clock()      const { return halfmove_clock_; }
    int      fullmove_number()     const { return fullmove_number_; }

    /// Find the square of the king for the given color.
    /// Returns NO_SQUARE if no king is found (should never happen in a valid game).
    Square find_king(Color color) const;

    // ============================================================
    // Board state mutators
    // ============================================================

    void set_side_to_move(Color c)        { side_to_move_ = c; }
    void set_castling_rights(uint8_t cr)  { castling_rights_ = cr; }
    void set_en_passant_square(Square sq) { en_passant_sq_ = sq; }
    void set_halfmove_clock(int hmc)      { halfmove_clock_ = hmc; }
    void set_fullmove_number(int fmn)     { fullmove_number_ = fmn; }

    // ============================================================
    // Move execution
    // ============================================================

    /// Information needed to undo a move.  Stored before make_move() so
    /// undo_move() can restore the exact previous state.
    /// This avoids deep-copying the board during AI search.
    struct UndoInfo {
        Piece    captured       = Piece();     // Piece that was on the 'to' square (or NONE)
        uint8_t  castling_rights = CastlingRights::NONE;
        Square   en_passant_sq  = NO_SQUARE;
        int      halfmove_clock = 0;
    };

    /// Execute a move on the board.  Updates all state: piece positions,
    /// side to move, castling rights, en passant square, and clocks.
    /// Does NOT validate legality — caller must ensure the move is legal.
    /// Returns UndoInfo that can be passed to undo_move() to reverse this move.
    UndoInfo make_move(const Move& move);

    /// Reverse a previously applied move, restoring the board to its exact
    /// prior state using the UndoInfo returned by make_move().
    void undo_move(const Move& move, const UndoInfo& undo);

    // ============================================================
    // Display
    // ============================================================

    /// Return a human-readable ASCII art representation of the board.
    /// Oriented from White's perspective (rank 8 at top).
    ///
    /// Example output:
    ///   8 | r  n  b  q  k  b  n  r
    ///   7 | p  p  p  p  p  p  p  p
    ///   6 | .  .  .  .  .  .  .  .
    ///   5 | .  .  .  .  .  .  .  .
    ///   4 | .  .  .  .  .  .  .  .
    ///   3 | .  .  .  .  .  .  .  .
    ///   2 | P  P  P  P  P  P  P  P
    ///   1 | R  N  B  Q  K  B  N  R
    ///     +-------------------------
    ///       a  b  c  d  e  f  g  h
    std::string to_string() const;

    /// Convert a square index to algebraic notation (e.g., 0 -> "a1", 63 -> "h8")
    static std::string square_to_algebraic(Square sq);

    /// Convert algebraic notation to a square index (e.g., "e4" -> 28).
    /// Returns NO_SQUARE on invalid input.
    static Square algebraic_to_square(const std::string& alg);

private:
    // The mailbox: squares_[0] = a1, squares_[63] = h8
    std::array<Piece, NUM_SQUARES> squares_;

    Color    side_to_move_    = Color::WHITE;
    uint8_t  castling_rights_ = CastlingRights::ALL;
    Square   en_passant_sq_   = NO_SQUARE;
    int      halfmove_clock_  = 0;
    int      fullmove_number_ = 1;

    /// Clear the board to all NONE pieces and reset state to defaults
    void clear();

    /// Update castling rights after a move from/to a given square.
    /// Called internally by make_move().
    void update_castling_rights(Square from, Square to);
};

} // namespace chess
