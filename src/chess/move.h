/**
 * move.h — Lightweight move representation
 *
 * A Move is a compact struct encoding:
 *   - Source square (0..63)
 *   - Destination square (0..63)
 *   - Flags bitmask (promotion, en passant, castling)
 *   - Promotion piece type (if applicable)
 *
 * Designed to be cheap to copy, compare, and store in move lists.
 * Total size: 4 bytes.
 */

#pragma once

#include "core/types.h"

namespace chess {

// ============================================================
// Move flags — packed into a single byte
// ============================================================

namespace MoveFlags {
    constexpr uint8_t NONE              = 0;
    constexpr uint8_t PROMOTION         = 1 << 0;  // Pawn promotes
    constexpr uint8_t EN_PASSANT        = 1 << 1;  // En passant capture
    constexpr uint8_t KINGSIDE_CASTLE   = 1 << 2;  // O-O
    constexpr uint8_t QUEENSIDE_CASTLE  = 1 << 3;  // O-O-O
    constexpr uint8_t DOUBLE_PAWN_PUSH  = 1 << 4;  // Pawn moves two squares forward
} // namespace MoveFlags

// ============================================================
// Move struct
// ============================================================

struct Move {
    Square    from       = 0;
    Square    to         = 0;
    uint8_t   flags      = MoveFlags::NONE;
    PieceType promo_type = PieceType::NONE;  // Only meaningful when PROMOTION flag is set

    constexpr Move() = default;
    constexpr Move(Square f, Square t, uint8_t fl = MoveFlags::NONE, PieceType pt = PieceType::NONE)
        : from(f), to(t), flags(fl), promo_type(pt) {}

    constexpr bool is_promotion()       const { return flags & MoveFlags::PROMOTION; }
    constexpr bool is_en_passant()      const { return flags & MoveFlags::EN_PASSANT; }
    constexpr bool is_kingside_castle() const { return flags & MoveFlags::KINGSIDE_CASTLE; }
    constexpr bool is_queenside_castle()const { return flags & MoveFlags::QUEENSIDE_CASTLE; }
    constexpr bool is_castle()          const { return is_kingside_castle() || is_queenside_castle(); }
    constexpr bool is_double_pawn_push()const { return flags & MoveFlags::DOUBLE_PAWN_PUSH; }

    constexpr bool operator==(const Move& o) const {
        return from == o.from && to == o.to && flags == o.flags && promo_type == o.promo_type;
    }
    constexpr bool operator!=(const Move& o) const { return !(*this == o); }

    /// Convert to UCI long-algebraic notation: "e2e4", "e7e8q"
    std::string to_uci() const;

    /// Parse from UCI long-algebraic notation. Returns a default Move on failure.
    static Move from_uci(const std::string& uci);
};

} // namespace chess
