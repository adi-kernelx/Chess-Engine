/**
 * zobrist.h — Zobrist Hashing for Chess Board State
 *
 * Zobrist hashing assigns a unique, pseudorandom 64-bit integer to every:
 *   - (Color, PieceType, Square) combination: 2 * 6 * 64 = 768 keys
 *   - Side to move (Black to move): 1 key
 *   - Castling rights combinations: 16 keys (4-bit bitmask 0..15)
 *   - En passant target file: 8 keys (files 0..7)
 *
 * The board hash is the XOR sum of all present elements. This allows O(1)
 * incremental hash updates when making/undoing moves using XOR operations.
 */

#pragma once

#include "core/types.h"
#include <cstdint>
#include <array>

namespace chess {

class Board;

namespace zobrist {

/// 64-bit random keys for all board state components
struct ZobristKeys {
    // pieces[color 0..1][piece_type 0..5][square 0..63]
    uint64_t pieces[2][6][64];
    // Key toggled when Black is to move (White = 0)
    uint64_t side_to_move;
    // Castling availability keys indexed by 4-bit bitmask [0..15]
    uint64_t castling[16];
    // En passant keys indexed by file [0..7] (a..h)
    uint64_t en_passant[8];
};

/// Retrieve reference to the singleton Zobrist keys table
const ZobristKeys& get_keys();

/// Compute full 64-bit Zobrist hash of a board position from scratch
uint64_t compute_hash(const Board& board);

} // namespace zobrist
} // namespace chess
