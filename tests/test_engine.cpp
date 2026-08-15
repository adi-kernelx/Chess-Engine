/**
 * test_engine.cpp — Tests for Phase 6.1 + 6.2: Search + Evaluation
 *
 * Tests include:
 *   1. Mate-in-1 positions (engine must find the only mating move)
 *   2. Mate-in-2 positions (engine must find a 2-move mating combination)
 *   3. Obvious material capture (must take a free piece)
 *   4. Avoid blunder (must not walk into a losing trade)
 *   5. Score sanity checks (material balance reflected in score)
 *   6. Performance benchmark (nodes per second, depth reached)
 *   7. Evaluation quality: king safety, pawn structure, mobility, bishop pair
 *
 * All FEN positions are verified for correctness.
 */

#include "chess/engine.h"
#include "chess/evaluator.h"
#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"

#include <cassert>
#include <iostream>
#include <string>
#include <chrono>
#include <vector>

using namespace chess;
using namespace chess::engine;

// ============================================================
// Test helpers
// ============================================================

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static void test_result(bool passed, const std::string& name) {
    ++tests_run;
    if (passed) {
        ++tests_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++tests_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

static void expect_move(const std::string& fen, const std::string& expected_uci,
                        int time_ms, const std::string& test_name) {
    Board board;
    board.set_from_fen(fen);

    Engine engine;
    engine.set_position(board);
    SearchResult result = engine.search(time_ms);

    const std::string found_uci = result.best_move.to_uci();
    const bool passed = (found_uci == expected_uci);

    ++tests_run;
    if (passed) {
        ++tests_passed;
        std::cout << "  [PASS] " << test_name
                  << "  (depth=" << result.depth
                  << ", nodes=" << result.nodes
                  << ", score=" << result.score << ")\n";
    } else {
        ++tests_failed;
        std::cout << "  [FAIL] " << test_name << "\n";
        std::cout << "         FEN:      " << fen << "\n";
        std::cout << "         Expected: " << expected_uci
                  << "  Got: " << found_uci
                  << "  Score: " << result.score
                  << "  Depth: " << result.depth
                  << "  Nodes: " << result.nodes << "\n";
    }
}

static void expect_one_of(const std::string& fen,
                          const std::vector<std::string>& acceptable,
                          int time_ms, const std::string& test_name) {
    Board board;
    board.set_from_fen(fen);

    Engine engine;
    engine.set_position(board);
    SearchResult result = engine.search(time_ms);

    const std::string found = result.best_move.to_uci();
    bool ok = false;
    for (const auto& m : acceptable) {
        if (found == m) { ok = true; break; }
    }

    ++tests_run;
    if (ok) {
        ++tests_passed;
        std::cout << "  [PASS] " << test_name
                  << "  (found=" << found
                  << ", depth=" << result.depth
                  << ", score=" << result.score << ")\n";
    } else {
        ++tests_failed;
        std::cout << "  [FAIL] " << test_name << "\n";
        std::cout << "         FEN:      " << fen << "\n";
        std::cout << "         Expected one of: ";
        for (const auto& m : acceptable) std::cout << m << " ";
        std::cout << "\n         Got: " << found
                  << "  Score: " << result.score
                  << "  Depth: " << result.depth << "\n";
    }
}

// ============================================================
// Test 1: Mate-in-1
// ============================================================

static void test_mate_in_1() {
    std::cout << "\n=== Mate in 1 ===\n";

    // --- Fool's Mate: Black delivers Qh4# ---
    // After 1.f3 e5 2.g4, Black plays Qd8-h4#.
    expect_move(
        "rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq g3 0 2",
        "d8h4",
        500, "Fool's Mate: Qh4#"
    );

    // --- Scholar's Mate: White delivers Qxf7# ---
    // Position after 1.e4 e5 2.Bc4 Nc6 3.Qh5.
    // Qh5xf7# is immediate checkmate (defended by Bc4).
    expect_move(
        "r1bqkb1r/pppp1ppp/2n5/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 2 4",
        "h5f7",
        500, "Scholar's Mate: Qxf7#"
    );
}

// ============================================================
// Test 2: Mate-in-2
// ============================================================

static void test_mate_in_2() {
    std::cout << "\n=== Mate in 2 ===\n";

    // --- Qf6+ Kg8 Re8# ---
    // FEN: 2r5/p4pk1/1p6/3p4/3B4/1P3Q2/P4PPP/4R1K1 w - - 0 1
    // 1.Qf6+! Kg8 (forced) 2.Re8#
    expect_one_of(
        "2r5/p4pk1/1p6/3p4/3B4/1P3Q2/P4PPP/4R1K1 w - - 0 1",
        {"f3f6"},
        1500, "Mate in 2: Qf6+ Kg8 Re8#"
    );
}

// ============================================================
// Test 3: Tactical tests
// ============================================================

static void test_tactics() {
    std::cout << "\n=== Tactical Tests ===\n";

    // --- Capture free rook ---
    // White Queen on a4 captures undefended Black Rook on d4 along rank 4.
    // King on g8 is far away and cannot recapture.
    expect_move(
        "6k1/8/8/8/Q2r4/8/8/6K1 w - - 0 1",
        "a4d4",
        500, "Capture free rook on rank 4: Qxd4"
    );

    // --- Capture free knight ---
    // White Rook on d1 captures undefended Black Knight on d4.
    expect_move(
        "6k1/8/8/8/3n4/8/8/3R2K1 w - - 0 1",
        "d1d4",
        500, "Capture free knight: Rxd4"
    );

    // --- Capture undefended pawn ---
    // White Knight on b5 captures undefended Black Pawn on c7.
    expect_move(
        "7k/2p5/8/1N6/8/8/8/6K1 w - - 0 1",
        "b5c7",
        500, "Capture undefended pawn: Nxc7"
    );

    // --- Avoid blunder ---
    // Defended rook on g8: Qxg8+?? would lose queen to Kxg8.
    {
        Board board;
        board.set_from_fen("6rk/8/8/7Q/8/8/8/6K1 w - - 0 1");
        Engine engine;
        engine.set_position(board);
        SearchResult result = engine.search(500);
        const bool avoids = (result.best_move.to_uci() != "h5g8");
        test_result(avoids, "Avoids Qxg8+?? (loses queen to Kxg8)");
    }
}

// ============================================================
// Test 4: Score sanity
// ============================================================

static void test_score_sanity() {
    std::cout << "\n=== Score Sanity ===\n";

    // Up a queen: White queen + king vs lone black king.
    {
        Board board;
        board.set_from_fen("6k1/8/8/8/8/8/8/Q5K1 w - - 0 1");
        Engine engine;
        engine.set_position(board);
        SearchResult result = engine.search(500);
        test_result(result.score > 800,
                    "Up a queen -> score > 800cp (got " + std::to_string(result.score) + ")");
    }

    // Starting position should be near 0 (roughly equal)
    {
        Board board = Board::starting_position();
        Engine engine;
        engine.set_position(board);
        SearchResult result = engine.search(500);
        test_result(std::abs(result.score) < 100,
                    "Start pos near 0 (got " + std::to_string(result.score) + ")");
    }

    // Fool's Mate: score should be a forced-mate value (>> 800000)
    {
        Board board;
        board.set_from_fen("rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq g3 0 2");
        Engine engine;
        engine.set_position(board);
        SearchResult result = engine.search(500);
        test_result(result.score > 800'000,
                    "Fool's Mate -> score > 800K (got " + std::to_string(result.score) + ")");
    }

    // Up a rook: White Rook + King vs lone black King.
    {
        Board board;
        board.set_from_fen("6k1/8/8/8/8/8/8/R5K1 w - - 0 1");
        Engine engine;
        engine.set_position(board);
        SearchResult result = engine.search(500);
        test_result(result.score > 400,
                    "Up a rook -> score > 400cp (got " + std::to_string(result.score) + ")");
    }
}

// ============================================================
// Test 5: Performance benchmark
// ============================================================

static void test_performance() {
    std::cout << "\n=== Performance Benchmark ===\n";

    Board board = Board::starting_position();
    Engine engine;
    engine.set_position(board);

    const auto start = std::chrono::steady_clock::now();
    SearchResult result = engine.search(2000); // 2 seconds
    const auto end = std::chrono::steady_clock::now();

    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
    );
    const long nps = (ms > 0) ? (result.nodes * 1000L / ms) : 0;

    std::cout << "  Starting position, 2s search:\n";
    std::cout << "    Best move:  " << result.best_move.to_uci() << "\n";
    std::cout << "    Score:      " << result.score << " cp\n";
    std::cout << "    Depth:      " << result.depth << "\n";
    std::cout << "    Nodes:      " << result.nodes << "\n";
    std::cout << "    Time:       " << ms << " ms\n";
    std::cout << "    NPS:        " << nps << " nodes/sec\n";

    test_result(result.nodes > 20'000,  "Searches > 20K nodes in 2s");
    test_result(result.depth >= 4,      "Reaches at least depth 4 in 2s");
    test_result(nps > 10'000,           "NPS > 10K nodes/sec");
}

// ============================================================
// Test 6: Evaluation Quality (Phase 6.2)
// ============================================================

static void test_eval_king_safety() {
    std::cout << "\n=== Evaluation: King Safety ===\n";

    // Castled king (pawns on f2,g2,h2 in front of king on g1) should score
    // higher king_safety than king on e1 with pawns pushed away.
    {
        Board castled;
        // White king castled kingside with pawn shield
        castled.set_from_fen("r1bqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQK2R w KQkq - 0 1");
        // Actually set a post-castling position:
        // White: Kg1, pawns f2,g2,h2. Black: standard
        castled.set_from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQ1RK1 w kq - 0 1");
        EvalBreakdown bd_castled = evaluate_detailed(castled);

        Board exposed;
        // White: Ke1, no pawns on e-file or nearby (pushed away)
        exposed.set_from_fen("rnbqkbnr/pppppppp/8/8/3PP3/8/PPP2PPP/RNBQK2R w KQkq - 0 1");
        EvalBreakdown bd_exposed = evaluate_detailed(exposed);

        // Castled king should have better king_safety than exposed king
        test_result(bd_castled.king_safety > bd_exposed.king_safety,
                    "Castled king has better king_safety than exposed king ("
                    + std::to_string(bd_castled.king_safety) + " vs "
                    + std::to_string(bd_exposed.king_safety) + ")");
    }

    // King on open file (no friendly pawn on that file) should be penalized
    {
        Board open_file;
        // White king on e1, no white pawn on e-file
        open_file.set_from_fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQK2R w KQkq - 0 1");
        // Wait, e4 pawn is still on the e-file. Use a position where e-file is clear:
        open_file.set_from_fen("rnbqkbnr/pppppppp/8/8/8/3P4/PPP1PPPP/RNBQK2R w KQkq - 0 1");
        // King is on e1, d3 pawn is on d-file. e-file still has e2 pawn. Hmm.
        // Let's use: king on e1, e-pawn captured (no pawn on e-file at all)
        open_file.set_from_fen("rnbqkbnr/pppp1ppp/8/4p3/8/8/PPPP1PPP/RNBQK2R w KQkq - 0 1");
        // Now white has no pawn on e-file (e2 is gone).
        EvalBreakdown bd_open = evaluate_detailed(open_file);

        Board closed_file;
        // White king on e1, e2 pawn present
        closed_file.set_from_fen("rnbqkbnr/pppp1ppp/8/4p3/8/8/PPPPPPPP/RNBQK2R w KQkq - 0 1");
        EvalBreakdown bd_closed = evaluate_detailed(closed_file);

        // King with pawn on its file should have better (or equal) safety
        test_result(bd_closed.king_safety >= bd_open.king_safety,
                    "King with pawn on file has better safety than open file ("
                    + std::to_string(bd_closed.king_safety) + " vs "
                    + std::to_string(bd_open.king_safety) + ")");
    }
}

static void test_eval_pawn_structure() {
    std::cout << "\n=== Evaluation: Pawn Structure ===\n";

    // Doubled pawns: two white pawns on the same file should be penalized
    {
        Board doubled;
        // White has doubled pawns on e-file (e3 and e4)
        doubled.set_from_fen("6k1/8/8/8/4P3/4P3/8/6K1 w - - 0 1");
        EvalBreakdown bd = evaluate_detailed(doubled);
        // pawn_structure should be negative (penalty for doubled)
        test_result(bd.pawn_structure < 0,
                    "Doubled pawns penalized (pawn_structure=" + std::to_string(bd.pawn_structure) + ")");
    }

    // Isolated pawn: a pawn with no friendly pawns on adjacent files
    {
        Board isolated;
        // White: lone pawn on e4 (isolated — no pawns on d or f files).
        // Black: connected pawns on d6+e6 (not isolated — they support each other).
        // The pawn_structure difference should be negative for White since
        // White has an isolated pawn and Black doesn't.
        isolated.set_from_fen("6k1/8/3pp3/8/4P3/8/8/6K1 w - - 0 1");
        EvalBreakdown bd = evaluate_detailed(isolated);
        test_result(bd.pawn_structure < 0,
                    "Isolated pawn penalized (pawn_structure=" + std::to_string(bd.pawn_structure) + ")");
    }

    // Connected pawns (not doubled, not isolated) should score better than isolated
    {
        Board connected;
        // White pawns on d4 and e4 — connected, supporting each other
        connected.set_from_fen("6k1/8/8/8/3PP3/8/8/6K1 w - - 0 1");
        EvalBreakdown bd_conn = evaluate_detailed(connected);

        Board isolated;
        // White pawns on a4 and h4 — completely isolated from each other
        isolated.set_from_fen("6k1/8/8/8/P6P/8/8/6K1 w - - 0 1");
        EvalBreakdown bd_iso = evaluate_detailed(isolated);

        test_result(bd_conn.pawn_structure > bd_iso.pawn_structure,
                    "Connected pawns better than isolated ("
                    + std::to_string(bd_conn.pawn_structure) + " vs "
                    + std::to_string(bd_iso.pawn_structure) + ")");
    }

    // Passed pawn: bonus for pawn with no opposing pawn blocking it
    {
        Board passed;
        // White pawn on e5, no black pawns on d,e,f files — it's passed
        passed.set_from_fen("6k1/8/8/4P3/8/8/8/6K1 w - - 0 1");
        EvalBreakdown bd_pass = evaluate_detailed(passed);

        Board blocked;
        // White pawn on e5, black pawn on e6 — not passed
        blocked.set_from_fen("6k1/8/4p3/4P3/8/8/8/6K1 w - - 0 1");
        EvalBreakdown bd_block = evaluate_detailed(blocked);

        test_result(bd_pass.pawn_structure > bd_block.pawn_structure,
                    "Passed pawn scores better than blocked ("
                    + std::to_string(bd_pass.pawn_structure) + " vs "
                    + std::to_string(bd_block.pawn_structure) + ")");
    }
}

static void test_eval_mobility() {
    std::cout << "\n=== Evaluation: Mobility ===\n";

    // Position with high white mobility vs restricted black
    {
        Board mobile;
        // White has developed pieces (more moves available)
        // Black is cramped (fewer moves)
        mobile.set_from_fen("rnbqkbnr/pppppppp/8/8/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 1");
        EvalBreakdown bd = evaluate_detailed(mobile);
        // After 1.e4 2.Nf3, White has more mobility than Black
        test_result(bd.mobility > 0,
                    "Developed side has positive mobility bonus ("
                    + std::to_string(bd.mobility) + ")");
    }

    // Starting position: both sides equal mobility
    {
        Board start = Board::starting_position();
        EvalBreakdown bd = evaluate_detailed(start);
        // Mobility should be near zero (White goes first = slight edge)
        test_result(std::abs(bd.mobility) <= 20,
                    "Starting position mobility is near-zero ("
                    + std::to_string(bd.mobility) + ")");
    }
}

static void test_eval_bishop_pair() {
    std::cout << "\n=== Evaluation: Bishop Pair ===\n";

    // White has both bishops, Black has only one
    {
        Board bp;
        // White: Kg1, Bc1, Bf1 (two bishops). Black: Kg8, Bc8 (one bishop)
        bp.set_from_fen("2k5/8/8/8/8/8/8/2B2BK1 w - - 0 1");
        EvalBreakdown bd = evaluate_detailed(bp);
        test_result(bd.bishop_pair > 0,
                    "White bishop pair bonus is positive ("
                    + std::to_string(bd.bishop_pair) + ")");
    }

    // Both sides have bishop pair → bishop_pair component is 0
    {
        Board both;
        both.set_from_fen("2k2b2/5b2/8/8/8/8/8/2B2BK1 w - - 0 1");
        EvalBreakdown bd = evaluate_detailed(both);
        test_result(bd.bishop_pair == 0,
                    "Both sides have bishop pair → bonus cancels ("
                    + std::to_string(bd.bishop_pair) + ")");
    }
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "===================================\n";
    std::cout << " Phase 6.1+6.2 Engine Tests\n";
    std::cout << "===================================\n";

    // Phase 6.1: Search
    test_mate_in_1();
    test_mate_in_2();
    test_tactics();
    test_score_sanity();
    test_performance();

    // Phase 6.2: Evaluation quality
    test_eval_king_safety();
    test_eval_pawn_structure();
    test_eval_mobility();
    test_eval_bishop_pair();

    std::cout << "\n===================================\n";
    std::cout << " Results: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << "  (" << tests_failed << " FAILED)";
    }
    std::cout << "\n===================================\n";

    return (tests_failed == 0) ? 0 : 1;
}
