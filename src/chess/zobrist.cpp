/**
 * zobrist.cpp — Implementation of Zobrist keys and hash computation
 */

#include "chess/zobrist.h"
#include "chess/board.h"
#include <random>

namespace chess {
namespace zobrist {

namespace {

// Deterministic PRNG to ensure reproducible Zobrist keys
class PRNG64 {
public:
    explicit PRNG64(uint64_t seed = 0x18092004BEEFULL) : state_(seed) {}

    uint64_t next() {
        // SplitMix64 PRNG: fast and high statistical quality
        uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

private:
    uint64_t state_;
};

ZobristKeys init_keys() {
    ZobristKeys k{};
    PRNG64 rng(0x8A5CD7896342125ULL);

    for (int c = 0; c < 2; ++c) {
        for (int p = 0; p < 6; ++p) {
            for (int sq = 0; sq < 64; ++sq) {
                k.pieces[c][p][sq] = rng.next();
            }
        }
    }

    k.side_to_move = rng.next();

    for (int cr = 0; cr < 16; ++cr) {
        k.castling[cr] = rng.next();
    }

    for (int f = 0; f < 8; ++f) {
        k.en_passant[f] = rng.next();
    }

    return k;
}

} // anonymous namespace

const ZobristKeys& get_keys() {
    static const ZobristKeys keys = init_keys();
    return keys;
}

uint64_t compute_hash(const Board& board) {
    const ZobristKeys& k = get_keys();
    uint64_t hash = 0;

    // 1. Pieces on squares
    for (Square sq = 0; sq < NUM_SQUARES; ++sq) {
        const Piece p = board.piece_at(sq);
        if (!p.is_none() && p.color != Color::NONE && p.type != PieceType::NONE) {
            const int c = static_cast<int>(p.color);
            const int t = static_cast<int>(p.type);
            if (c >= 0 && c < 2 && t >= 0 && t < 6) {
                hash ^= k.pieces[c][t][sq];
            }
        }
    }

    // 2. Side to move (Black XORed)
    if (board.side_to_move() == Color::BLACK) {
        hash ^= k.side_to_move;
    }

    // 3. Castling rights (4-bit mask 0..15)
    uint8_t cr = board.castling_rights() & CastlingRights::ALL;
    hash ^= k.castling[cr];

    // 4. En passant target square
    Square ep = board.en_passant_square();
    if (ep != NO_SQUARE && is_valid_square(ep)) {
        int ep_file = file_of(ep);
        if (ep_file >= 0 && ep_file < 8) {
            hash ^= k.en_passant[ep_file];
        }
    }

    return hash;
}

} // namespace zobrist
} // namespace chess
