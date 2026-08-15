/**
 * test_ai_integration.cpp — Unit tests for Phase 6.4 (AI Player Integration)
 */

#include "chess/board.h"
#include "chess/move_gen.h"
#include "chess/engine.h"
#include "game/ai_player.h"
#include "game/game_room.h"
#include "game/room_manager.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace chess;
using namespace chess::game;
using namespace chess::engine;

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << name << " ... "

#define PASS() \
    do { std::cout << "PASS\n"; ++g_pass; } while(0)

#define FAIL(msg) \
    do { std::cout << "FAIL: " << msg << "\n"; ++g_fail; } while(0)

// ============================================================
// Test 1: AI Room Creation
// ============================================================
void test_ai_room_creation() {
    TEST("AI Room starts IN_PROGRESS with AI on Black");

    RoomManager mgr;
    TimeControl tc(600000, 5000);
    auto room = mgr.create_ai_room(1, "Alice", 42, tc, AIDifficulty::MEDIUM);

    if (room->get_state() != RoomState::IN_PROGRESS) {
        FAIL("AI room should start IN_PROGRESS");
        return;
    }

    if (!room->is_ai_game()) {
        FAIL("Room should be flagged as AI game");
        return;
    }

    if (room->ai_color() != Color::BLACK) {
        FAIL("AI should play Black");
        return;
    }

    if (room->get_player_fd(Color::WHITE) != 42) {
        FAIL("Human should be White with fd 42");
        return;
    }

    if (room->get_player_fd(Color::BLACK) != -2) {
        FAIL("AI should have sentinel fd -2");
        return;
    }

    PASS();
}

// ============================================================
// Test 2: AI Room NOT in Open Rooms
// ============================================================
void test_ai_room_not_listed() {
    TEST("AI rooms do not appear in list_open_rooms");

    RoomManager mgr;
    TimeControl tc(600000, 5000);

    // Create a human room (WAITING) and an AI room (IN_PROGRESS)
    mgr.create_room(1, "Alice", 42, tc);
    mgr.create_ai_room(2, "Bob", 43, tc, AIDifficulty::EASY);

    auto open_rooms = mgr.list_open_rooms();
    // Only the human room should be listed
    if (open_rooms.size() != 1) {
        FAIL("Expected 1 open room, got " + std::to_string(open_rooms.size()));
        return;
    }

    PASS();
}

// ============================================================
// Test 3: AIPlayer Compute Move Returns Legal Move
// ============================================================
void test_ai_compute_legal_move() {
    TEST("AIPlayer::compute_move returns a legal move");

    Board board = Board::starting_position();
    AIPlayer ai;
    AIMove move = ai.compute_move(board, AIDifficulty::EASY);

    // Verify the move is legal
    auto legal_moves = move_gen::generate_legal_moves(board);
    bool found = false;
    for (const auto& m : legal_moves) {
        if (m.from == move.from && m.to == move.to) {
            found = true;
            break;
        }
    }

    if (!found) {
        FAIL("AI produced an illegal move");
        return;
    }

    if (move.depth < 1) {
        FAIL("AI should reach at least depth 1");
        return;
    }

    PASS();
}

// ============================================================
// Test 4: submit_move_ai Works Correctly
// ============================================================
void test_submit_move_ai() {
    TEST("submit_move_ai applies AI's move on the authoritative board");

    RoomManager mgr;
    TimeControl tc(600000, 5000);
    auto room = mgr.create_ai_room(1, "Alice", 42, tc, AIDifficulty::EASY);

    // Human plays 1. e4
    Square e2 = Board::algebraic_to_square("e2");
    Square e4 = Board::algebraic_to_square("e4");
    auto human_result = room->submit_move(42, e2, e4);

    if (!human_result.success) {
        FAIL("Human move 1. e4 should succeed: " + human_result.error);
        return;
    }

    // Now it's Black's (AI's) turn
    if (room->side_to_move() != Color::BLACK) {
        FAIL("After 1. e4, it should be Black's turn");
        return;
    }

    // Compute AI move and submit
    AIPlayer ai;
    AIMove ai_move = ai.compute_move(room->get_board(), AIDifficulty::EASY);
    auto ai_result = room->submit_move_ai(ai_move.from, ai_move.to, ai_move.promotion);

    if (!ai_result.success) {
        FAIL("AI move should succeed: " + ai_result.error);
        return;
    }

    // After AI moves, it should be White's turn again
    if (room->side_to_move() != Color::WHITE) {
        FAIL("After AI's response, it should be White's turn");
        return;
    }

    PASS();
}

// ============================================================
// Test 5: Difficulty Levels Differ in Search Depth
// ============================================================
void test_difficulty_levels() {
    TEST("EASY searches fewer nodes than HARD");

    Board board = Board::starting_position();
    AIPlayer ai;

    AIMove easy_move = ai.compute_move(board, AIDifficulty::EASY);
    AIMove hard_move = ai.compute_move(board, AIDifficulty::HARD);

    if (easy_move.nodes >= hard_move.nodes) {
        FAIL("EASY (" + std::to_string(easy_move.nodes) + " nodes) should search fewer than HARD (" +
             std::to_string(hard_move.nodes) + " nodes)");
        return;
    }

    if (easy_move.depth >= hard_move.depth) {
        FAIL("EASY (depth " + std::to_string(easy_move.depth) + ") should reach lower depth than HARD (depth " +
             std::to_string(hard_move.depth) + ")");
        return;
    }

    PASS();
}

// ============================================================
// Test 6: Difficulty Parsing
// ============================================================
void test_difficulty_parsing() {
    TEST("parse_difficulty and difficulty_name roundtrip");

    if (parse_difficulty("easy") != AIDifficulty::EASY) {
        FAIL("parse_difficulty(\"easy\") failed");
        return;
    }
    if (parse_difficulty("medium") != AIDifficulty::MEDIUM) {
        FAIL("parse_difficulty(\"medium\") failed");
        return;
    }
    if (parse_difficulty("hard") != AIDifficulty::HARD) {
        FAIL("parse_difficulty(\"hard\") failed");
        return;
    }
    if (parse_difficulty("max") != AIDifficulty::MAX) {
        FAIL("parse_difficulty(\"max\") failed");
        return;
    }
    if (parse_difficulty("garbage") != AIDifficulty::MEDIUM) {
        FAIL("parse_difficulty default should be MEDIUM");
        return;
    }

    if (difficulty_name(AIDifficulty::EASY) != "Easy") {
        FAIL("difficulty_name(EASY) should be \"Easy\"");
        return;
    }
    if (difficulty_name(AIDifficulty::MAX) != "Max") {
        FAIL("difficulty_name(MAX) should be \"Max\"");
        return;
    }

    PASS();
}

// ============================================================
// Test 7: Full AI Game Loop (Fool's Mate Setup)
// ============================================================
void test_full_ai_game_loop() {
    TEST("Full game loop: human makes moves, AI responds each time");

    RoomManager mgr;
    TimeControl tc(600000, 5000);
    auto room = mgr.create_ai_room(1, "Alice", 42, tc, AIDifficulty::EASY);
    AIPlayer ai;

    // Play a few moves back and forth
    const int max_half_moves = 10;
    for (int i = 0; i < max_half_moves; ++i) {
        if (room->get_state() != RoomState::IN_PROGRESS) break;

        // Human's turn (White)
        Board board_copy = room->get_board();
        auto human_moves = move_gen::generate_legal_moves(board_copy);
        if (human_moves.empty()) break;

        // Just play the first legal move
        Move hm = human_moves[0];
        auto hr = room->submit_move(42, hm.from, hm.to, hm.promo_type);
        if (!hr.success) {
            FAIL("Human move failed at half-move " + std::to_string(i) + ": " + hr.error);
            return;
        }

        if (hr.game_status != GameStatus::ONGOING) break;

        // AI's turn (Black)
        AIMove am = ai.compute_move(room->get_board(), AIDifficulty::EASY);
        auto ar = room->submit_move_ai(am.from, am.to, am.promotion);
        if (!ar.success) {
            FAIL("AI move failed at half-move " + std::to_string(i) + ": " + ar.error);
            return;
        }

        if (ar.game_status != GameStatus::ONGOING) break;
    }

    // Game should have progressed (some moves should be in history)
    auto history = room->get_move_history();
    if (history.size() < 2) {
        FAIL("Expected at least 2 moves in history, got " + std::to_string(history.size()));
        return;
    }

    PASS();
}

// ============================================================
// Test 8: AI Wins Against Random Mover
// ============================================================
void test_ai_vs_random() {
    TEST("AI (MEDIUM) beats random mover in at least 2/3 games");

    int ai_wins = 0;
    const int num_games = 3;

    for (int game_idx = 0; game_idx < num_games; ++game_idx) {
        Board board = Board::starting_position();
        AIPlayer ai;

        int half_moves = 0;
        const int max_half_moves = 200;
        GameStatus status = GameStatus::ONGOING;

        while (half_moves < max_half_moves && status == GameStatus::ONGOING) {
            auto legal = move_gen::generate_legal_moves(board);
            if (legal.empty()) {
                status = move_gen::get_game_status(board);
                break;
            }

            if (board.side_to_move() == Color::WHITE) {
                // Random mover (White)
                Move m = legal[half_moves % legal.size()];
                board.make_move(m);
            } else {
                // AI (Black)
                AIMove am = ai.compute_move(board, AIDifficulty::MEDIUM);
                // Find and apply the matching move
                for (const auto& m : legal) {
                    if (m.from == am.from && m.to == am.to) {
                        board.make_move(m);
                        break;
                    }
                }
            }

            status = move_gen::get_game_status(board);
            ++half_moves;
        }

        // AI (Black) wins if result is 0-1 (checkmate with White to move means Black won)
        if (status == GameStatus::CHECKMATE && board.side_to_move() == Color::WHITE) {
            ++ai_wins;
        }
    }

    if (ai_wins < 2) {
        FAIL("AI won only " + std::to_string(ai_wins) + "/" + std::to_string(num_games) + " games against random mover");
        return;
    }

    PASS();
}

// ============================================================
// Test 9: Performance Benchmark
// ============================================================
void test_ai_benchmark() {
    TEST("AI performance benchmark at each difficulty");

    Board board;
    board.set_from_fen("r1bqkb1r/pppp1ppp/2n5/4p3/2B1n3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 5");
    AIPlayer ai;

    struct BenchResult {
        std::string name;
        int depth;
        long nodes;
        int ms;
    };

    std::vector<BenchResult> results;
    AIDifficulty levels[] = {AIDifficulty::EASY, AIDifficulty::MEDIUM, AIDifficulty::HARD};
    std::string names[] = {"EASY", "MEDIUM", "HARD"};

    for (int i = 0; i < 3; ++i) {
        AIMove m = ai.compute_move(board, levels[i]);
        results.push_back({names[i], m.depth, m.nodes, m.elapsed_ms});
    }

    std::cout << "PASS\n"; ++g_pass;
    std::cout << "    Benchmark results:\n";
    for (const auto& r : results) {
        long nps = (r.ms > 0) ? (r.nodes * 1000L / r.ms) : 0;
        std::cout << "      " << r.name
                  << ": depth=" << r.depth
                  << ", nodes=" << r.nodes
                  << ", time=" << r.ms << "ms"
                  << ", nps=" << nps << "\n";
    }
}

int main() {
    std::cout << "=================================================\n";
    std::cout << "   Phase 6.4: AI Player Integration Tests\n";
    std::cout << "=================================================\n";

    test_ai_room_creation();
    test_ai_room_not_listed();
    test_ai_compute_legal_move();
    test_submit_move_ai();
    test_difficulty_levels();
    test_difficulty_parsing();
    test_full_ai_game_loop();
    test_ai_vs_random();
    test_ai_benchmark();

    std::cout << "\n-------------------------------------------------\n";
    std::cout << "Results: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "=================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
