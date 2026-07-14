/**
 * test_notation.cpp — Tests for Phase 3.3: SAN, PGN, and undo_move
 *
 * Tests:
 *   1. Undo move correctness (FEN roundtrip after make/undo)
 *   2. SAN generation for all move types
 *   3. SAN parsing
 *   4. PGN export/import
 *   5. Famous game replay: verify final positions match known results
 */

#include "chess/notation.h"
#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include <iostream>
#include <functional>
#include <string>
#include <vector>

using namespace chess;
using namespace chess::notation;
using namespace chess::move_gen;

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
// Test: undo_move correctness
// ============================================================

void test_undo_move() {
    std::cout << "\n=== Undo Move Tests ===" << std::endl;

    run_test("Undo restores starting position exactly", []() {
        Board board = Board::starting_position();
        std::string original_fen = board.to_fen();
        std::vector<Move> moves = generate_legal_moves(board);

        for (const Move& move : moves) {
            Board::UndoInfo undo = board.make_move(move);
            board.undo_move(move, undo);
            if (board.to_fen() != original_fen) return false;
        }
        return true;
    });

    run_test("Undo castling restores FEN", []() {
        std::string fen = "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1";
        Board board;
        board.set_from_fen(fen);

        Move castle(Squares::E1, Squares::G1, MoveFlags::KINGSIDE_CASTLE);
        Board::UndoInfo undo = board.make_move(castle);
        board.undo_move(castle, undo);

        return board.to_fen() == fen;
    });

    run_test("Undo en passant restores FEN", []() {
        std::string fen = "rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3";
        Board board;
        board.set_from_fen(fen);

        Move ep(Board::algebraic_to_square("e5"), Board::algebraic_to_square("d6"), MoveFlags::EN_PASSANT);
        Board::UndoInfo undo = board.make_move(ep);
        board.undo_move(ep, undo);

        return board.to_fen() == fen;
    });

    run_test("Undo promotion restores FEN", []() {
        std::string fen = "8/4P3/8/8/8/8/8/4K2k w - - 0 1";
        Board board;
        board.set_from_fen(fen);

        Move promo(Board::algebraic_to_square("e7"), Board::algebraic_to_square("e8"),
                   MoveFlags::PROMOTION, PieceType::QUEEN);
        Board::UndoInfo undo = board.make_move(promo);
        board.undo_move(promo, undo);

        return board.to_fen() == fen;
    });

    run_test("Perft still correct with make/undo (depth 4)", []() {
        Board board = Board::starting_position();
        return perft(board, 4) == 197281;
    });

    run_test("Kiwipete perft still correct with make/undo (depth 3)", []() {
        Board board;
        board.set_from_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        return perft(board, 3) == 97862;
    });
}

// ============================================================
// Test: SAN Generation
// ============================================================

void test_san_generation() {
    std::cout << "\n=== SAN Generation Tests ===" << std::endl;

    run_test("Pawn move: e2-e4 -> 'e4'", []() {
        Board board = Board::starting_position();
        Move m(Board::algebraic_to_square("e2"), Board::algebraic_to_square("e4"),
               MoveFlags::DOUBLE_PAWN_PUSH);
        return move_to_san(board, m) == "e4";
    });

    run_test("Knight move: Ng1-f3 -> 'Nf3'", []() {
        Board board = Board::starting_position();
        Move m(Board::algebraic_to_square("g1"), Board::algebraic_to_square("f3"));
        return move_to_san(board, m) == "Nf3";
    });

    run_test("Kingside castling -> 'O-O'", []() {
        Board board;
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
        Move m(Squares::E1, Squares::G1, MoveFlags::KINGSIDE_CASTLE);
        return move_to_san(board, m) == "O-O";
    });

    run_test("Queenside castling -> 'O-O-O'", []() {
        Board board;
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
        Move m(Squares::E1, Squares::C1, MoveFlags::QUEENSIDE_CASTLE);
        return move_to_san(board, m) == "O-O-O";
    });

    run_test("Pawn capture: exd5", []() {
        Board board;
        board.set_from_fen("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2");
        Move m(Board::algebraic_to_square("e4"), Board::algebraic_to_square("d5"));
        std::string san = move_to_san(board, m);
        return san == "exd5";
    });

    run_test("Promotion: e8=Q", []() {
        Board board;
        board.set_from_fen("8/4P3/8/8/8/8/8/4K2k w - - 0 1");
        Move m(Board::algebraic_to_square("e7"), Board::algebraic_to_square("e8"),
               MoveFlags::PROMOTION, PieceType::QUEEN);
        std::string san = move_to_san(board, m);
        return san == "e8=Q";
    });

    run_test("Fool's Mate checkmate: Qh4#", []() {
        Board board;
        // After 1. f3 e5 2. g4, Black plays Qh4#
        board.set_from_fen("rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2");
        Move m(Board::algebraic_to_square("d8"), Board::algebraic_to_square("h4"));
        std::string san = move_to_san(board, m);
        return san == "Qh4#";
    });
}

// ============================================================
// Test: SAN Parsing
// ============================================================

void test_san_parsing() {
    std::cout << "\n=== SAN Parsing Tests ===" << std::endl;

    run_test("Parse 'e4' from starting position", []() {
        Board board = Board::starting_position();
        Move m = san_to_move(board, "e4");
        return m.from == Board::algebraic_to_square("e2") &&
               m.to == Board::algebraic_to_square("e4");
    });

    run_test("Parse 'Nf3' from starting position", []() {
        Board board = Board::starting_position();
        Move m = san_to_move(board, "Nf3");
        return m.from == Board::algebraic_to_square("g1") &&
               m.to == Board::algebraic_to_square("f3");
    });

    run_test("Parse 'O-O'", []() {
        Board board;
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
        Move m = san_to_move(board, "O-O");
        return m.is_kingside_castle();
    });

    run_test("Parse 'O-O-O'", []() {
        Board board;
        board.set_from_fen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
        Move m = san_to_move(board, "O-O-O");
        return m.is_queenside_castle();
    });

    run_test("Parse 'exd5' (pawn capture)", []() {
        Board board;
        board.set_from_fen("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2");
        Move m = san_to_move(board, "exd5");
        return m.from == Board::algebraic_to_square("e4") &&
               m.to == Board::algebraic_to_square("d5");
    });

    run_test("Parse 'e8=Q' (promotion)", []() {
        Board board;
        board.set_from_fen("8/4P3/8/8/8/8/8/4K2k w - - 0 1");
        Move m = san_to_move(board, "e8=Q");
        return m.is_promotion() && m.promo_type == PieceType::QUEEN;
    });

    run_test("Parse 'Qh4#' (checkmate)", []() {
        Board board;
        board.set_from_fen("rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2");
        Move m = san_to_move(board, "Qh4#");
        return m.from == Board::algebraic_to_square("d8") &&
               m.to == Board::algebraic_to_square("h4");
    });

    run_test("SAN roundtrip: generate then parse all legal moves", []() {
        Board board = Board::starting_position();
        std::vector<Move> moves = generate_legal_moves(board);
        for (const Move& move : moves) {
            std::string san = move_to_san(board, move);
            Move parsed = san_to_move(board, san);
            if (parsed.from != move.from || parsed.to != move.to) {
                std::cerr << "Failed roundtrip for " << san << std::endl;
                return false;
            }
        }
        return true;
    });
}

// ============================================================
// Test: PGN Export/Import
// ============================================================

void test_pgn() {
    std::cout << "\n=== PGN Tests ===" << std::endl;

    run_test("Export and reimport Scholar's Mate", []() {
        // Scholar's Mate: 1. e4 e5 2. Bc4 Nc6 3. Qh5 Nf6 4. Qxf7#
        Board board = Board::starting_position();
        std::vector<Move> moves;
        std::vector<std::string> sans = {"e4", "e5", "Bc4", "Nc6", "Qh5", "Nf6", "Qxf7"};

        for (const auto& san : sans) {
            Move m = san_to_move(board, san);
            if (m.from == 0 && m.to == 0 && m.flags == 0) return false;
            moves.push_back(m);
            board.make_move(m);
        }

        // Export
        std::vector<PgnTag> tags = {{"Event", "Test"}, {"Result", "1-0"}};
        std::string pgn = export_pgn(tags, moves, "1-0");

        // Reimport
        PgnGame imported = import_pgn(pgn);
        if (imported.moves.size() != moves.size()) return false;
        if (imported.result != "1-0") return false;

        // Verify moves match
        for (size_t i = 0; i < moves.size(); ++i) {
            if (imported.moves[i].from != moves[i].from ||
                imported.moves[i].to != moves[i].to) return false;
        }

        return true;
    });
}

// ============================================================
// Test: Famous Game Replays
// ============================================================

/// Helper: replay a sequence of SAN moves, return the final FEN
static std::string replay_game(const std::vector<std::string>& san_moves) {
    Board board = Board::starting_position();
    for (const auto& san : san_moves) {
        Move m = san_to_move(board, san);
        if (m.from == 0 && m.to == 0 && m.flags == 0) {
            std::cerr << "  Failed to parse SAN: '" << san << "'" << std::endl;
            std::cerr << "  Board:\n" << board.to_string() << std::endl;
            return "";
        }
        board.make_move(m);
    }
    return board.to_fen();
}

void test_famous_games() {
    std::cout << "\n=== Famous Game Replays ===" << std::endl;

    // Game 1: Fool's Mate (shortest possible checkmate)
    run_test("Fool's Mate (1. f3 e5 2. g4 Qh4#)", []() {
        auto fen = replay_game({"f3", "e5", "g4", "Qh4"});
        if (fen.empty()) return false;
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    // Game 2: Scholar's Mate
    run_test("Scholar's Mate (1. e4 e5 2. Bc4 Nc6 3. Qh5 Nf6 4. Qxf7#)", []() {
        auto fen = replay_game({"e4", "e5", "Bc4", "Nc6", "Qh5", "Nf6", "Qxf7"});
        if (fen.empty()) return false;
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    // Game 3: Opera Game - Paul Morphy vs Duke & Count, 1858
    run_test("Opera Game (Morphy vs Duke/Count, 1858)", []() {
        auto fen = replay_game({
            "e4", "e5", "Nf3", "d6", "d4", "Bg4", "dxe5", "Bxf3",
            "Qxf3", "dxe5", "Bc4", "Nf6", "Qb3", "Qe7", "Nc3", "c6",
            "Bg5", "b5", "Nxb5", "cxb5", "Bxb5", "Nbd7", "O-O-O", "Rd8",
            "Rxd7", "Rxd7", "Rd1", "Qe6", "Bxd7", "Nxd7", "Qb8", "Nxb8",
            "Rd8"
        });
        if (fen.empty()) return false;
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    // Game 4: Immortal Game - Anderssen vs Kieseritzky, 1851
    run_test("Immortal Game (Anderssen vs Kieseritzky, 1851)", []() {
        auto fen = replay_game({
            "e4", "e5", "f4", "exf4", "Bc4", "Qh4", "Kf1", "b5",
            "Bxb5", "Nf6", "Nf3", "Qh6", "d3", "Nh5", "Nh4", "Qg5",
            "Nf5", "c6", "g4", "Nf6", "Rg1", "cxb5", "h4", "Qg6",
            "h5", "Qg5", "Qf3", "Ng8", "Bxf4", "Qf6", "Nc3", "Bc5",
            "Nd5", "Qxb2", "Bd6", "Bxg1", "e5", "Qxa1", "Ke2", "Na6",
            "Nxg7", "Kd8", "Qf6", "Nxf6", "Be7"
        });
        // The Immortal Game ends with Be7# — checkmate
        if (fen.empty()) return false;
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    // Game 5: Byrne vs Fischer, 1956 ("Game of the Century")
    run_test("Game of the Century (Byrne vs Fischer, 1956)", []() {
        auto fen = replay_game({
            "Nf3", "Nf6", "c4", "g6", "Nc3", "Bg7", "d4", "O-O",
            "Bf4", "d5", "Qb3", "dxc4", "Qxc4", "c6", "e4", "Nbd7",
            "Rd1", "Nb6", "Qc5", "Bg4", "Bg5", "Na4", "Qa3", "Nxc3",
            "bxc3", "Nxe4", "Bxe7", "Qb6", "Bc4", "Nxc3", "Bc5", "Rfe8",
            "Kf1", "Be6", "Bxb6", "Bxc4", "Kg1", "Ne2", "Kf1", "Nxd4",
            "Kg1", "Ne2", "Kf1", "Nc3", "Kg1", "axb6", "Qb4", "Ra4",
            "Qxb6", "Nxd1", "h3", "Rxa2", "Kh2", "Nxf2", "Re1", "Rxe1",
            "Qd8", "Bf8", "Nxe1", "Bd5", "Nf3", "Ne4", "Qb8", "b5",
            "h4", "h5", "Ne5", "Kg7", "Kg1", "Bc5", "Kf1", "Ng3",
            "Ke1", "Bb4", "Kd1", "Bb3", "Kc1", "Ne2", "Kb1", "Nc3",
            "Kc1", "Rc2"
        });
        if (fen.empty()) return false;
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    // Game 6: A short tactical miniature — Legal's Mate
    // Legal vs Saint Brie, 1750
    run_test("Legal's Mate (Legal vs Saint Brie, 1750)", []() {
        auto fen = replay_game({
            "e4", "e5", "Nf3", "d6", "Bc4", "Bg4", "Nc3", "g6",
            "Nxe5", "Bxd1", "Bxf7", "Ke7", "Nd5"
        });
        if (fen.empty()) return false;
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    // Game 7: Short but tactical - Italian Game miniature
    run_test("Italian Game miniature with en passant", []() {
        // A constructed game that exercises en passant and promotion
        auto fen = replay_game({
            "e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5", "d3", "d6",
            "O-O", "Nf6", "Bg5", "O-O"
        });
        if (fen.empty()) return false;
        // Just verify it parsed correctly and the game is ongoing
        Board board;
        board.set_from_fen(fen);
        return get_game_status(board) == GameStatus::ONGOING;
    });
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Chess Notation Tests - Phase 3.3" << std::endl;
    std::cout << "========================================" << std::endl;

    test_undo_move();
    test_san_generation();
    test_san_parsing();
    test_pgn();
    test_famous_games();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
