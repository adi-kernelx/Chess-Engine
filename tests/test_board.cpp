/**
 * test_board.cpp — Unit tests for the Board class (Phase 3.1 verification)
 *
 * Tests:
 *   1. Starting position: all 32 pieces in correct squares
 *   2. FEN roundtrip: parse -> export -> parse produces identical state
 *   3. Algebraic notation conversion
 *   4. Move execution: basic move, capture, castling, en passant, promotion
 *   5. ASCII display sanity check
 */

#include "chess/board.h"
#include <iostream>
#include <cassert>
#include <functional>
#include <vector>
#include <string>

using namespace chess;

// ============================================================
// Simple test harness
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void run_test(const std::string& name, std::function<bool()> test_fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (test_fn()) {
        std::cout << "PASS" << std::endl;
        g_passed++;
    } else {
        std::cout << "FAIL" << std::endl;
        g_failed++;
    }
}

// ============================================================
// Test: Starting position has all 32 pieces correct
// ============================================================

void test_starting_position() {
    std::cout << "\n=== Starting Position Tests ===" << std::endl;

    run_test("White pawns on rank 2", []() {
        Board board = Board::starting_position();
        for (int file = 0; file < 8; file++) {
            Piece p = board.piece_at(make_square(1, file));
            if (p.type != PieceType::PAWN || p.color != Color::WHITE) return false;
        }
        return true;
    });

    run_test("Black pawns on rank 7", []() {
        Board board = Board::starting_position();
        for (int file = 0; file < 8; file++) {
            Piece p = board.piece_at(make_square(6, file));
            if (p.type != PieceType::PAWN || p.color != Color::BLACK) return false;
        }
        return true;
    });

    run_test("White back rank pieces", []() {
        Board board = Board::starting_position();
        PieceType expected[] = {
            PieceType::ROOK, PieceType::KNIGHT, PieceType::BISHOP, PieceType::QUEEN,
            PieceType::KING, PieceType::BISHOP, PieceType::KNIGHT, PieceType::ROOK
        };
        for (int file = 0; file < 8; file++) {
            Piece p = board.piece_at(make_square(0, file));
            if (p.type != expected[file] || p.color != Color::WHITE) return false;
        }
        return true;
    });

    run_test("Black back rank pieces", []() {
        Board board = Board::starting_position();
        PieceType expected[] = {
            PieceType::ROOK, PieceType::KNIGHT, PieceType::BISHOP, PieceType::QUEEN,
            PieceType::KING, PieceType::BISHOP, PieceType::KNIGHT, PieceType::ROOK
        };
        for (int file = 0; file < 8; file++) {
            Piece p = board.piece_at(make_square(7, file));
            if (p.type != expected[file] || p.color != Color::BLACK) return false;
        }
        return true;
    });

    run_test("Empty squares on ranks 3-6", []() {
        Board board = Board::starting_position();
        for (int rank = 2; rank <= 5; rank++) {
            for (int file = 0; file < 8; file++) {
                if (!board.piece_at(make_square(rank, file)).is_none()) return false;
            }
        }
        return true;
    });

    run_test("Side to move is White", []() {
        Board board = Board::starting_position();
        return board.side_to_move() == Color::WHITE;
    });

    run_test("All castling rights available", []() {
        Board board = Board::starting_position();
        return board.castling_rights() == CastlingRights::ALL;
    });

    run_test("No en passant square", []() {
        Board board = Board::starting_position();
        return board.en_passant_square() == NO_SQUARE;
    });

    run_test("Halfmove clock is 0", []() {
        Board board = Board::starting_position();
        return board.halfmove_clock() == 0;
    });

    run_test("Fullmove number is 1", []() {
        Board board = Board::starting_position();
        return board.fullmove_number() == 1;
    });

    run_test("White king on e1", []() {
        Board board = Board::starting_position();
        return board.find_king(Color::WHITE) == Squares::E1;
    });

    run_test("Black king on e8", []() {
        Board board = Board::starting_position();
        return board.find_king(Color::BLACK) == Squares::E8;
    });
}

// ============================================================
// Test: FEN roundtrip
// ============================================================

void test_fen_roundtrip() {
    std::cout << "\n=== FEN Roundtrip Tests ===" << std::endl;

    run_test("Starting position FEN roundtrip", []() {
        Board board = Board::starting_position();
        return board.to_fen() == STARTING_FEN;
    });

    run_test("Custom position FEN roundtrip (after 1.e4 e5 2.Nf3)", []() {
        std::string fen = "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2";
        Board board;
        if (!board.set_from_fen(fen)) return false;
        return board.to_fen() == fen;
    });

    run_test("Position with en passant target", []() {
        std::string fen = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";
        Board board;
        if (!board.set_from_fen(fen)) return false;
        if (board.en_passant_square() != Board::algebraic_to_square("e3")) return false;
        return board.to_fen() == fen;
    });

    run_test("Partial castling rights (Kq)", []() {
        std::string fen = "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w Kq - 0 1";
        Board board;
        if (!board.set_from_fen(fen)) return false;
        uint8_t expected = CastlingRights::WHITE_KINGSIDE | CastlingRights::BLACK_QUEENSIDE;
        if (board.castling_rights() != expected) return false;
        return board.to_fen() == fen;
    });

    run_test("No castling rights", []() {
        std::string fen = "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w - - 0 1";
        Board board;
        if (!board.set_from_fen(fen)) return false;
        if (board.castling_rights() != CastlingRights::NONE) return false;
        return board.to_fen() == fen;
    });

    run_test("Reject malformed FEN (too few fields)", []() {
        Board board;
        return !board.set_from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w");
    });

    run_test("Reject malformed FEN (invalid side to move)", []() {
        Board board;
        return !board.set_from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1");
    });
}

// ============================================================
// Test: Square notation conversion
// ============================================================

void test_algebraic_notation() {
    std::cout << "\n=== Algebraic Notation Tests ===" << std::endl;

    run_test("a1 = square 0", []() {
        return Board::algebraic_to_square("a1") == 0 &&
               Board::square_to_algebraic(0) == "a1";
    });

    run_test("h8 = square 63", []() {
        return Board::algebraic_to_square("h8") == 63 &&
               Board::square_to_algebraic(63) == "h8";
    });

    run_test("e4 = square 28", []() {
        Square e4 = make_square(3, 4);
        return Board::algebraic_to_square("e4") == e4 &&
               Board::square_to_algebraic(e4) == "e4";
    });

    run_test("Invalid input returns NO_SQUARE", []() {
        return Board::algebraic_to_square("z9") == NO_SQUARE &&
               Board::algebraic_to_square("") == NO_SQUARE &&
               Board::algebraic_to_square("a") == NO_SQUARE;
    });
}

// ============================================================
// Test: Move execution
// ============================================================

void test_move_execution() {
    std::cout << "\n=== Move Execution Tests ===" << std::endl;

    run_test("Pawn double push: e2-e4 sets en passant on e3", []() {
        Board board = Board::starting_position();
        Square e2 = Board::algebraic_to_square("e2");
        Square e4 = Board::algebraic_to_square("e4");
        board.make_move(Move(e2, e4, MoveFlags::DOUBLE_PAWN_PUSH));

        if (!board.piece_at(e2).is_none()) return false;
        if (board.piece_at(e4).type != PieceType::PAWN) return false;
        if (board.piece_at(e4).color != Color::WHITE) return false;
        if (board.en_passant_square() != Board::algebraic_to_square("e3")) return false;
        if (board.side_to_move() != Color::BLACK) return false;
        if (board.halfmove_clock() != 0) return false;
        return true;
    });

    run_test("Knight move: Ng1-f3 increments halfmove clock", []() {
        Board board = Board::starting_position();
        Square g1 = Board::algebraic_to_square("g1");
        Square f3 = Board::algebraic_to_square("f3");
        board.make_move(Move(g1, f3));

        if (!board.piece_at(g1).is_none()) return false;
        if (board.piece_at(f3).type != PieceType::KNIGHT) return false;
        if (board.halfmove_clock() != 1) return false;
        return true;
    });

    run_test("Kingside castling: White O-O", []() {
        Board board;
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
        board.make_move(Move(Squares::E1, Squares::G1, MoveFlags::KINGSIDE_CASTLE));

        if (board.piece_at(Squares::G1).type != PieceType::KING) return false;
        if (board.piece_at(Squares::F1).type != PieceType::ROOK) return false;
        if (!board.piece_at(Squares::E1).is_none()) return false;
        if (!board.piece_at(Squares::H1).is_none()) return false;
        // White castling rights revoked
        if (board.castling_rights() & CastlingRights::WHITE_KINGSIDE) return false;
        if (board.castling_rights() & CastlingRights::WHITE_QUEENSIDE) return false;
        return true;
    });

    run_test("Queenside castling: White O-O-O", []() {
        Board board;
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
        board.make_move(Move(Squares::E1, Squares::C1, MoveFlags::QUEENSIDE_CASTLE));

        if (board.piece_at(Squares::C1).type != PieceType::KING) return false;
        if (board.piece_at(Squares::D1).type != PieceType::ROOK) return false;
        if (!board.piece_at(Squares::E1).is_none()) return false;
        if (!board.piece_at(Squares::A1).is_none()) return false;
        return true;
    });

    run_test("En passant capture removes captured pawn", []() {
        Board board;
        board.set_from_fen("rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");

        Square e5 = Board::algebraic_to_square("e5");
        Square d6 = Board::algebraic_to_square("d6");
        Square d5 = Board::algebraic_to_square("d5");
        board.make_move(Move(e5, d6, MoveFlags::EN_PASSANT));

        if (board.piece_at(d6).type != PieceType::PAWN) return false;
        if (board.piece_at(d6).color != Color::WHITE) return false;
        if (!board.piece_at(e5).is_none()) return false;
        if (!board.piece_at(d5).is_none()) return false;  // The captured pawn
        return true;
    });

    run_test("Pawn promotion to Queen", []() {
        Board board;
        board.set_from_fen("8/4P3/8/8/8/8/8/4K2k w - - 0 1");

        Square e7 = Board::algebraic_to_square("e7");
        Square e8 = Board::algebraic_to_square("e8");
        board.make_move(Move(e7, e8, MoveFlags::PROMOTION, PieceType::QUEEN));

        if (board.piece_at(e8).type != PieceType::QUEEN) return false;
        if (board.piece_at(e8).color != Color::WHITE) return false;
        if (!board.piece_at(e7).is_none()) return false;
        return true;
    });

    run_test("Fullmove counter increments after Black moves", []() {
        Board board = Board::starting_position();
        if (board.fullmove_number() != 1) return false;

        // White moves e2-e4
        board.make_move(Move(Board::algebraic_to_square("e2"),
                             Board::algebraic_to_square("e4"),
                             MoveFlags::DOUBLE_PAWN_PUSH));
        if (board.fullmove_number() != 1) return false;

        // Black moves e7-e5
        board.make_move(Move(Board::algebraic_to_square("e7"),
                             Board::algebraic_to_square("e5"),
                             MoveFlags::DOUBLE_PAWN_PUSH));
        if (board.fullmove_number() != 2) return false;

        return true;
    });

    run_test("Rook capture revokes opponent castling right", []() {
        Board board;
        // White rook on a8 about to capture black's rook
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");

        // White rook captures black's a8 rook
        board.make_move(Move(Squares::A1, Squares::A8));

        // Black's queenside castling should be revoked (rook on a8 is gone)
        if (board.castling_rights() & CastlingRights::BLACK_QUEENSIDE) return false;
        // White's queenside also revoked (rook left a1)
        if (board.castling_rights() & CastlingRights::WHITE_QUEENSIDE) return false;
        return true;
    });
}

// ============================================================
// Test: UCI notation
// ============================================================

void test_uci_notation() {
    std::cout << "\n=== UCI Notation Tests ===" << std::endl;

    run_test("Basic move to_uci: e2e4", []() {
        Move m(Board::algebraic_to_square("e2"), Board::algebraic_to_square("e4"));
        return m.to_uci() == "e2e4";
    });

    run_test("Promotion to_uci: e7e8q", []() {
        Move m(Board::algebraic_to_square("e7"), Board::algebraic_to_square("e8"),
               MoveFlags::PROMOTION, PieceType::QUEEN);
        return m.to_uci() == "e7e8q";
    });

    run_test("Parse from_uci: e2e4", []() {
        Move m = Move::from_uci("e2e4");
        return m.from == Board::algebraic_to_square("e2") &&
               m.to == Board::algebraic_to_square("e4");
    });

    run_test("Parse promotion from_uci: e7e8n", []() {
        Move m = Move::from_uci("e7e8n");
        return m.from == Board::algebraic_to_square("e7") &&
               m.to == Board::algebraic_to_square("e8") &&
               m.is_promotion() &&
               m.promo_type == PieceType::KNIGHT;
    });
}

// ============================================================
// Test: ASCII display
// ============================================================

void test_ascii_display() {
    std::cout << "\n=== ASCII Display Test ===" << std::endl;

    run_test("Display contains rank numbers and file labels", []() {
        Board board = Board::starting_position();
        std::string display = board.to_string();
        return display.find("8 |") != std::string::npos &&
               display.find("1 |") != std::string::npos &&
               display.find("a  b  c  d  e  f  g  h") != std::string::npos;
    });

    // Print the board for visual inspection
    Board board = Board::starting_position();
    std::cout << "\n" << board.to_string() << std::endl;
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Chess Board Tests - Phase 3.1" << std::endl;
    std::cout << "========================================" << std::endl;

    test_starting_position();
    test_fen_roundtrip();
    test_algebraic_notation();
    test_move_execution();
    test_uci_notation();
    test_ascii_display();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
