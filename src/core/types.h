#pragma once

#include <cstdint>
#include <string>

namespace chess {

// ============================================================
// Fundamental type aliases
// ============================================================

// Square index: 0..63, where a1=0, b1=1, ..., h1=7, a2=8, ..., h8=63
using Square = uint8_t;

// 64-bit bitboard for future optimization phases
using Bitboard = uint64_t;

// Server-level identifiers
using PlayerId = uint64_t;
using GameId = uint32_t;

// Sentinel value for "no square" (e.g., no en passant target)
constexpr Square NO_SQUARE = 64;

// Total squares on the board
constexpr int NUM_SQUARES = 64;
constexpr int NUM_RANKS = 8;
constexpr int NUM_FILES = 8;

// ============================================================
// Square index helpers
//
// Layout (rank-major, White's perspective):
//   a1=0,  b1=1,  c1=2,  ... h1=7
//   a2=8,  b2=9,  c2=10, ... h2=15
//   ...
//   a8=56, b8=57, c8=58, ... h8=63
// ============================================================

/// Construct a square index from rank [0..7] and file [0..7]
constexpr Square make_square(int rank, int file) {
    return static_cast<Square>(rank * 8 + file);
}

/// Extract rank [0..7] from a square index
constexpr int rank_of(Square sq) { return sq / 8; }

/// Extract file [0..7] from a square index
constexpr int file_of(Square sq) { return sq % 8; }

/// Check if a square index is within the valid range [0, 63]
constexpr bool is_valid_square(int sq) { return sq >= 0 && sq < NUM_SQUARES; }

// Named square constants for readability (used in castling logic, tests, etc.)
namespace Squares {
    constexpr Square A1 = 0,  B1 = 1,  C1 = 2,  D1 = 3,  E1 = 4,  F1 = 5,  G1 = 6,  H1 = 7;
    constexpr Square A2 = 8,  B2 = 9,  C2 = 10, D2 = 11, E2 = 12, F2 = 13, G2 = 14, H2 = 15;
    constexpr Square A7 = 48, B7 = 49, C7 = 50, D7 = 51, E7 = 52, F7 = 53, G7 = 54, H7 = 55;
    constexpr Square A8 = 56, B8 = 57, C8 = 58, D8 = 59, E8 = 60, F8 = 61, G8 = 62, H8 = 63;
} // namespace Squares

// ============================================================
// Piece representation
// ============================================================

enum class PieceType : uint8_t {
    PAWN = 0,
    KNIGHT,
    BISHOP,
    ROOK,
    QUEEN,
    KING,
    NONE
};

enum class Color : uint8_t {
    WHITE = 0,
    BLACK,
    NONE
};

/// Return the opposite color (WHITE <-> BLACK). Undefined for NONE.
constexpr Color opposite_color(Color c) {
    return (c == Color::WHITE) ? Color::BLACK : Color::WHITE;
}

/// A piece is a (type, color) pair.  NONE/NONE represents an empty square.
struct Piece {
    PieceType type  = PieceType::NONE;
    Color     color = Color::NONE;

    constexpr Piece() = default;
    constexpr Piece(PieceType t, Color c) : type(t), color(c) {}

    constexpr bool is_none() const { return type == PieceType::NONE; }

    constexpr bool operator==(const Piece& o) const {
        return type == o.type && color == o.color;
    }
    constexpr bool operator!=(const Piece& o) const { return !(*this == o); }

    /// Convert to single FEN character: 'P','N','B','R','Q','K' (white)
    ///                                   'p','n','b','r','q','k' (black)
    ///                                   '.' for empty
    char to_char() const;

    /// Construct a Piece from a FEN character. Returns NONE/NONE for invalid input.
    static Piece from_char(char c);
};

// ============================================================
// Castling rights — stored as a 4-bit bitmask
// ============================================================

namespace CastlingRights {
    constexpr uint8_t NONE           = 0;
    constexpr uint8_t WHITE_KINGSIDE  = 1 << 0;  // 'K' in FEN
    constexpr uint8_t WHITE_QUEENSIDE = 1 << 1;  // 'Q' in FEN
    constexpr uint8_t BLACK_KINGSIDE  = 1 << 2;  // 'k' in FEN
    constexpr uint8_t BLACK_QUEENSIDE = 1 << 3;  // 'q' in FEN
    constexpr uint8_t ALL            = 0x0F;
} // namespace CastlingRights

// ============================================================
// Game status
// ============================================================

enum class GameStatus : uint8_t {
    ONGOING,
    CHECKMATE,
    STALEMATE,
    DRAW_INSUFFICIENT_MATERIAL,
    DRAW_FIFTY_MOVE,
    DRAW_THREEFOLD_REPETITION,
    DRAW_AGREEMENT,
    RESIGNATION,
    TIMEOUT
};

// ============================================================
// Result pattern for error handling (no exceptions)
// ============================================================

template<typename T, typename E = std::string>
struct Result {
    T value;
    E error;
    bool success;

    static Result<T, E> Ok(T val) {
        return {val, "", true};
    }

    static Result<T, E> Err(E err) {
        return {T{}, err, false};
    }
};

} // namespace chess
