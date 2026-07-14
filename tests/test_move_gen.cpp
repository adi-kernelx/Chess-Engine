/**
 * test_move_gen.cpp — Unit tests and perft for move generation
 */

#include "chess/move_gen.h"
#include "chess/board.h"
#include <iostream>
#include <functional>
#include <vector>
#include <string>

using namespace chess;
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
// Perft tests against known values
// ============================================================

void test_perft() {
    std::cout << "\n=== Perft Tests ===" << std::endl;

    // Position 1: Starting Position
    // Depth 1: 20
    // Depth 2: 400
    // Depth 3: 8902
    // Depth 4: 197281
    run_test("Starting Position - Depth 1", []() {
        Board board = Board::starting_position();
        return perft(board, 1) == 20;
    });
    run_test("Starting Position - Depth 2", []() {
        Board board = Board::starting_position();
        return perft(board, 2) == 400;
    });
    run_test("Starting Position - Depth 3", []() {
        Board board = Board::starting_position();
        return perft(board, 3) == 8902;
    });
    run_test("Starting Position - Depth 4", []() {
        Board board = Board::starting_position();
        return perft(board, 4) == 197281;
    });

    // Position 2: Kiwipete
    // r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1
    // Depth 1: 48
    // Depth 2: 2039
    // Depth 3: 97862
    run_test("Kiwipete - Depth 1", []() {
        Board board;
        board.set_from_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        return perft(board, 1) == 48;
    });
    run_test("Kiwipete - Depth 2", []() {
        Board board;
        board.set_from_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        return perft(board, 2) == 2039;
    });
    run_test("Kiwipete - Depth 3", []() {
        Board board;
        board.set_from_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        return perft(board, 3) == 97862;
    });

    // Position 3: Position 3
    // 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1
    // Depth 1: 14
    // Depth 2: 191
    // Depth 3: 2812
    // Depth 4: 43238
    run_test("Position 3 - Depth 1", []() {
        Board board;
        board.set_from_fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
        return perft(board, 1) == 14;
    });
    run_test("Position 3 - Depth 4", []() {
        Board board;
        board.set_from_fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
        return perft(board, 4) == 43238;
    });

    // Position 4: Position 4
    // r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1
    // Depth 1: 6
    // Depth 2: 264
    // Depth 3: 9467
    run_test("Position 4 - Depth 1", []() {
        Board board;
        board.set_from_fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
        return perft(board, 1) == 6;
    });
    run_test("Position 4 - Depth 3", []() {
        Board board;
        board.set_from_fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
        return perft(board, 3) == 9467;
    });
}

// ============================================================
// Game Status Tests
// ============================================================

void test_game_status() {
    std::cout << "\n=== Game Status Tests ===" << std::endl;

    run_test("Ongoing game (Starting Position)", []() {
        Board board = Board::starting_position();
        return get_game_status(board) == GameStatus::ONGOING;
    });

    run_test("Checkmate (Fool's Mate)", []() {
        Board board;
        // 1. f3 e5 2. g4 Qh4#
        board.set_from_fen("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
        return get_game_status(board) == GameStatus::CHECKMATE;
    });

    run_test("Stalemate", []() {
        Board board;
        board.set_from_fen("k7/8/8/8/8/8/5q2/7K w - - 0 1"); // White king on h1, black queen on f2, no legal moves but not in check
        return get_game_status(board) == GameStatus::STALEMATE;
    });

    run_test("50-Move Rule Draw", []() {
        Board board;
        board.set_from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 100 51");
        return get_game_status(board) == GameStatus::DRAW_FIFTY_MOVE;
    });
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Chess Move Gen Tests - Phase 3.2" << std::endl;
    std::cout << "========================================" << std::endl;

    test_perft();
    test_game_status();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
