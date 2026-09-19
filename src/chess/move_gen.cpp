/**
 * move_gen.cpp — Chess move generation implementation
 */

#include "chess/move_gen.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace chess {
namespace move_gen {

// ============================================================
// Phase 10.2 — precomputed target lists for knight and king.
//
// Both pieces have a fixed pattern independent of surrounding pieces, so
// the eight bounds-checked (dr, df) probes we ran every time we generated
// or attacked-tested from a square are replaced by iterating a short
// precomputed list (length 2–8 for a knight, 3–8 for a king). It also
// deduplicates the four copies of those (dr, df) tables that used to sit
// inline in `is_square_attacked`, `generate_knight_moves`, and
// `generate_king_moves`.
//
// A word on the performance impact, honestly:  Phase 10.1 measured no
// improvement over the (dr, df) form on this codebase (388K vs 388K NPS
// startpos, 322K vs 317K kiwipete, 388K vs 384K middlegame, all Release
// with `-O2`, medians of three warm runs). The compiler was already
// unrolling and predicting the 8-iteration bounds check well; the array
// lookup does not beat what `-O2` was already producing. This lives here
// on readability grounds — one canonical geometry, four consumers — not
// as a performance win.
//
// Perft results (test_move_gen) remain the ground truth: this refactor
// touches no semantics.
// ============================================================

namespace {

struct AttackTables {
    std::array<std::array<Square, 8>, 64> knight_targets{};
    std::array<std::array<Square, 8>, 64> king_targets{};
    std::array<uint8_t, 64>               knight_count{};
    std::array<uint8_t, 64>               king_count{};

    AttackTables() {
        constexpr int kn_dr[8] = {-2, -2, -1, -1,  1,  1,  2,  2};
        constexpr int kn_df[8] = {-1,  1, -2,  2, -2,  2, -1,  1};

        for (int sq = 0; sq < 64; ++sq) {
            const int r = sq / 8;
            const int f = sq % 8;

            uint8_t kn = 0;
            for (int i = 0; i < 8; ++i) {
                const int nr = r + kn_dr[i];
                const int nf = f + kn_df[i];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    knight_targets[sq][kn++] = static_cast<Square>(nr * 8 + nf);
                }
            }
            knight_count[sq] = kn;

            uint8_t kg = 0;
            for (int dr = -1; dr <= 1; ++dr) {
                for (int df = -1; df <= 1; ++df) {
                    if (dr == 0 && df == 0) continue;
                    const int nr = r + dr;
                    const int nf = f + df;
                    if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                        king_targets[sq][kg++] = static_cast<Square>(nr * 8 + nf);
                    }
                }
            }
            king_count[sq] = kg;
        }
    }
};

inline const AttackTables& attack_tables() {
    static const AttackTables t;   // C++11: thread-safe initialization
    return t;
}

} // namespace

// ============================================================
// Attack Detection
// ============================================================

bool is_square_attacked(const Board& board, Square sq, Color attacker_color) {
    int r = rank_of(sq);
    int f = file_of(sq);

    // 1. Pawn attacks
    // A square is attacked by a white pawn if there is a white pawn one rank below and one file left/right.
    int pawn_attack_dir = (attacker_color == Color::WHITE) ? -1 : 1;
    int pawn_r = r + pawn_attack_dir;
    if (pawn_r >= 0 && pawn_r < 8) {
        if (f > 0) {
            Piece p = board.piece_at(make_square(pawn_r, f - 1));
            if (p.type == PieceType::PAWN && p.color == attacker_color) return true;
        }
        if (f < 7) {
            Piece p = board.piece_at(make_square(pawn_r, f + 1));
            if (p.type == PieceType::PAWN && p.color == attacker_color) return true;
        }
    }

    // 2. Knight attacks — precomputed target list (Phase 10.2).
    const auto& tbl = attack_tables();
    const uint8_t kn_n = tbl.knight_count[sq];
    for (uint8_t i = 0; i < kn_n; ++i) {
        Piece p = board.piece_at(tbl.knight_targets[sq][i]);
        if (p.type == PieceType::KNIGHT && p.color == attacker_color) return true;
    }

    // 3. King attacks (for adjacent squares) — precomputed target list.
    const uint8_t kg_n = tbl.king_count[sq];
    for (uint8_t i = 0; i < kg_n; ++i) {
        Piece p = board.piece_at(tbl.king_targets[sq][i]);
        if (p.type == PieceType::KING && p.color == attacker_color) return true;
    }

    // 4. Ray attacks (Bishops, Rooks, Queens)
    // Directions: 0-3 are rook dirs, 4-7 are bishop dirs
    static const int ray_dr[] = {-1,  1,  0,  0, -1, -1,  1,  1};
    static const int ray_df[] = { 0,  0, -1,  1, -1,  1, -1,  1};

    for (int i = 0; i < 8; ++i) {
        int nr = r + ray_dr[i];
        int nf = f + ray_df[i];
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            Piece p = board.piece_at(make_square(nr, nf));
            if (!p.is_none()) {
                if (p.color == attacker_color) {
                    bool is_diagonal = (i >= 4);
                    if (p.type == PieceType::QUEEN) return true;
                    if (is_diagonal && p.type == PieceType::BISHOP) return true;
                    if (!is_diagonal && p.type == PieceType::ROOK) return true;
                }
                break; // Hit a piece, ray stops
            }
            nr += ray_dr[i];
            nf += ray_df[i];
        }
    }

    return false;
}

bool is_in_check(const Board& board, Color king_color) {
    Square king_sq = board.find_king(king_color);
    if (king_sq == NO_SQUARE) return false; // Should not happen in a valid game
    return is_square_attacked(board, king_sq, opposite_color(king_color));
}

// ============================================================
// Pseudo-Legal Move Generation
// ============================================================

static void generate_pawn_moves(const Board& board, Square sq, Color color, std::vector<Move>& moves) {
    int r = rank_of(sq);
    int f = file_of(sq);
    int dir = (color == Color::WHITE) ? 1 : -1;
    int start_rank = (color == Color::WHITE) ? 1 : 6;
    int promo_rank = (color == Color::WHITE) ? 7 : 0;

    auto add_pawn_move = [&](Square to, uint8_t flags = MoveFlags::NONE) {
        if (rank_of(to) == promo_rank) {
            moves.emplace_back(sq, to, flags | MoveFlags::PROMOTION, PieceType::QUEEN);
            moves.emplace_back(sq, to, flags | MoveFlags::PROMOTION, PieceType::ROOK);
            moves.emplace_back(sq, to, flags | MoveFlags::PROMOTION, PieceType::BISHOP);
            moves.emplace_back(sq, to, flags | MoveFlags::PROMOTION, PieceType::KNIGHT);
        } else {
            moves.emplace_back(sq, to, flags);
        }
    };

    // Single push
    int forward_r = r + dir;
    if (forward_r >= 0 && forward_r < 8) {
        Square forward_sq = make_square(forward_r, f);
        if (board.piece_at(forward_sq).is_none()) {
            add_pawn_move(forward_sq);

            // Double push (only if single push is valid and on start rank)
            if (r == start_rank) {
                Square double_sq = make_square(r + 2 * dir, f);
                if (board.piece_at(double_sq).is_none()) {
                    moves.emplace_back(sq, double_sq, MoveFlags::DOUBLE_PAWN_PUSH);
                }
            }
        }
    }

    // Captures
    for (int df : {-1, 1}) {
        int cap_f = f + df;
        if (cap_f >= 0 && cap_f < 8 && forward_r >= 0 && forward_r < 8) {
            Square cap_sq = make_square(forward_r, cap_f);
            Piece target = board.piece_at(cap_sq);
            if (!target.is_none() && target.color != color) {
                add_pawn_move(cap_sq);
            } else if (cap_sq == board.en_passant_square()) {
                add_pawn_move(cap_sq, MoveFlags::EN_PASSANT);
            }
        }
    }
}

static void generate_knight_moves(const Board& board, Square sq, Color color, std::vector<Move>& moves) {
    // Phase 10.2: iterate the precomputed target list instead of running
    // eight bounds-checked (dr, df) probes on every call.
    const auto& tbl = attack_tables();
    const uint8_t n = tbl.knight_count[sq];
    for (uint8_t i = 0; i < n; ++i) {
        const Square to_sq = tbl.knight_targets[sq][i];
        const Piece target = board.piece_at(to_sq);
        if (target.is_none() || target.color != color) {
            moves.emplace_back(sq, to_sq);
        }
    }
}

static void generate_ray_moves(const Board& board, Square sq, Color color, bool orthogonal, bool diagonal, std::vector<Move>& moves) {
    int r = rank_of(sq);
    int f = file_of(sq);
    static const int dr[] = {-1,  1,  0,  0, -1, -1,  1,  1};
    static const int df[] = { 0,  0, -1,  1, -1,  1, -1,  1};

    int start_idx = orthogonal ? 0 : 4;
    int end_idx   = diagonal ? 8 : 4;

    for (int i = start_idx; i < end_idx; ++i) {
        int nr = r + dr[i];
        int nf = f + df[i];
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            Square to_sq = make_square(nr, nf);
            Piece target = board.piece_at(to_sq);
            if (target.is_none()) {
                moves.emplace_back(sq, to_sq);
            } else {
                if (target.color != color) {
                    moves.emplace_back(sq, to_sq);
                }
                break; // Stop sliding when hitting a piece
            }
            nr += dr[i];
            nf += df[i];
        }
    }
}

static void generate_king_moves(const Board& board, Square sq, Color color, std::vector<Move>& moves) {
    // Phase 10.2: precomputed 8-direction list.
    const auto& tbl = attack_tables();
    const uint8_t n = tbl.king_count[sq];
    for (uint8_t i = 0; i < n; ++i) {
        const Square to_sq = tbl.king_targets[sq][i];
        const Piece target = board.piece_at(to_sq);
        if (target.is_none() || target.color != color) {
            moves.emplace_back(sq, to_sq);
        }
    }

    // Castling
    uint8_t rights = board.castling_rights();
    Color opp_color = opposite_color(color);
    if (color == Color::WHITE) {
        if ((rights & CastlingRights::WHITE_KINGSIDE) &&
            board.piece_at(Squares::F1).is_none() && board.piece_at(Squares::G1).is_none()) {
            if (!is_square_attacked(board, Squares::E1, opp_color) &&
                !is_square_attacked(board, Squares::F1, opp_color)) {
                // We don't need to check G1 for attacks here, that's legal move generation's job 
                // to verify if the king ends up in check. Wait, FIDE rules say king cannot pass 
                // through or end up in check. We check E1 and F1 here. G1 will be checked 
                // by the standard `is_in_check` logic after making the move.
                moves.emplace_back(Squares::E1, Squares::G1, MoveFlags::KINGSIDE_CASTLE);
            }
        }
        if ((rights & CastlingRights::WHITE_QUEENSIDE) &&
            board.piece_at(Squares::D1).is_none() && board.piece_at(Squares::C1).is_none() && board.piece_at(Squares::B1).is_none()) {
            if (!is_square_attacked(board, Squares::E1, opp_color) &&
                !is_square_attacked(board, Squares::D1, opp_color)) {
                moves.emplace_back(Squares::E1, Squares::C1, MoveFlags::QUEENSIDE_CASTLE);
            }
        }
    } else {
        if ((rights & CastlingRights::BLACK_KINGSIDE) &&
            board.piece_at(Squares::F8).is_none() && board.piece_at(Squares::G8).is_none()) {
            if (!is_square_attacked(board, Squares::E8, opp_color) &&
                !is_square_attacked(board, Squares::F8, opp_color)) {
                moves.emplace_back(Squares::E8, Squares::G8, MoveFlags::KINGSIDE_CASTLE);
            }
        }
        if ((rights & CastlingRights::BLACK_QUEENSIDE) &&
            board.piece_at(Squares::D8).is_none() && board.piece_at(Squares::C8).is_none() && board.piece_at(Squares::B8).is_none()) {
            if (!is_square_attacked(board, Squares::E8, opp_color) &&
                !is_square_attacked(board, Squares::D8, opp_color)) {
                moves.emplace_back(Squares::E8, Squares::C8, MoveFlags::QUEENSIDE_CASTLE);
            }
        }
    }
}

std::vector<Move> generate_pseudo_legal_moves(const Board& board) {
    std::vector<Move> moves;
    moves.reserve(64); // Reasonable capacity to avoid reallocations

    Color color = board.side_to_move();

    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        Piece p = board.piece_at(static_cast<Square>(sq));
        if (p.is_none() || p.color != color) continue;

        switch (p.type) {
            case PieceType::PAWN:   generate_pawn_moves(board, sq, color, moves); break;
            case PieceType::KNIGHT: generate_knight_moves(board, sq, color, moves); break;
            case PieceType::BISHOP: generate_ray_moves(board, sq, color, false, true, moves); break;
            case PieceType::ROOK:   generate_ray_moves(board, sq, color, true, false, moves); break;
            case PieceType::QUEEN:  generate_ray_moves(board, sq, color, true, true, moves); break;
            case PieceType::KING:   generate_king_moves(board, sq, color, moves); break;
            default: break;
        }
    }

    return moves;
}

// ============================================================
// Legal Move Generation
// ============================================================

std::vector<Move> generate_legal_moves(const Board& board) {
    std::vector<Move> pseudo_moves = generate_pseudo_legal_moves(board);
    std::vector<Move> legal_moves;
    legal_moves.reserve(pseudo_moves.size());

    Color color = board.side_to_move();

    // We need a mutable copy to do make/undo on
    Board mutable_board = board;

    for (const Move& move : pseudo_moves) {
        Board::UndoInfo undo = mutable_board.make_move(move);
        // After make_move, the side_to_move flips. So the color we want to check is still `color`.
        if (!is_in_check(mutable_board, color)) {
            legal_moves.push_back(move);
        }
        mutable_board.undo_move(move, undo);
    }

    return legal_moves;
}

GameStatus get_game_status(const Board& board) {
    // 50-move rule (100 halfmoves)
    if (board.halfmove_clock() >= 100) {
        return GameStatus::DRAW_FIFTY_MOVE;
    }

    std::vector<Move> legal_moves = generate_legal_moves(board);

    if (legal_moves.empty()) {
        if (is_in_check(board, board.side_to_move())) {
            return GameStatus::CHECKMATE;
        } else {
            return GameStatus::STALEMATE;
        }
    }

    return GameStatus::ONGOING;
}

// ============================================================
// Perft — uses make/undo instead of board copies for speed
// ============================================================

uint64_t perft(Board& board, int depth) {
    if (depth == 0) {
        return 1ULL;
    }

    std::vector<Move> moves = generate_legal_moves(board);
    if (depth == 1) {
        return moves.size();
    }

    uint64_t nodes = 0;
    for (const Move& move : moves) {
        Board::UndoInfo undo = board.make_move(move);
        nodes += perft(board, depth - 1);
        board.undo_move(move, undo);
    }
    return nodes;
}

void divide(Board& board, int depth) {
    if (depth == 0) return;

    std::vector<Move> moves = generate_legal_moves(board);
    uint64_t total_nodes = 0;

    for (const Move& move : moves) {
        Board::UndoInfo undo = board.make_move(move);
        uint64_t nodes = perft(board, depth - 1);
        board.undo_move(move, undo);
        std::cout << move.to_uci() << ": " << nodes << std::endl;
        total_nodes += nodes;
    }
    std::cout << "\nNodes searched: " << total_nodes << std::endl;
}

} // namespace move_gen
} // namespace chess

