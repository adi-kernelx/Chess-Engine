/**
 * test_transposition_table.cpp — Unit tests for Phase 6.3 (Zobrist & TT)
 */

#include "chess/board.h"
#include "chess/zobrist.h"
#include "chess/transposition_table.h"
#include "chess/engine.h"
#include "chess/move_gen.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace chess;
using namespace chess::engine;

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << name << " ... "

#define PASS() \
    do { std::cout << "PASS\n"; ++g_pass; } while(0)

#define FAIL(msg) \
    do { std::cout << "FAIL: " << msg << "\n"; ++g_fail; } while(0)

// Helper for squares
static inline Square sq(const std::string& str) {
    return Board::algebraic_to_square(str);
}

// ============================================================
// Test 1: Zobrist Determinism & FEN Consistency
// ============================================================
void test_zobrist_determinism() {
    TEST("Zobrist Determinism across board instances");
    Board b1 = Board::starting_position();
    Board b2 = Board::starting_position();

    if (b1.zobrist_key() == 0) {
        FAIL("Zobrist key for starting position is 0");
        return;
    }

    if (b1.zobrist_key() != b2.zobrist_key()) {
        FAIL("Identical boards have different Zobrist keys");
        return;
    }

    // Different positions must produce different keys
    Board b3;
    b3.set_from_fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    if (b1.zobrist_key() == b3.zobrist_key()) {
        FAIL("Different positions produced colliding Zobrist keys");
        return;
    }

    PASS();
}

// ============================================================
// Test 2: Incremental Make/Undo Hash Matches Full Recompute
// ============================================================
void test_incremental_zobrist() {
    TEST("Incremental Zobrist update matches full compute on make/undo");
    Board board = Board::starting_position();

    // 1. Normal quiet move 1. e4
    Move e2e4(sq("e2"), sq("e4"), MoveFlags::DOUBLE_PAWN_PUSH);
    uint64_t initial_key = board.zobrist_key();
    auto u1 = board.make_move(e2e4);
    if (board.zobrist_key() != zobrist::compute_hash(board)) {
        FAIL("Incremental hash mismatch after 1. e4");
        return;
    }

    // 2. Black move 1... e5
    Move e7e5(sq("e7"), sq("e5"), MoveFlags::DOUBLE_PAWN_PUSH);
    auto u2 = board.make_move(e7e5);
    if (board.zobrist_key() != zobrist::compute_hash(board)) {
        FAIL("Incremental hash mismatch after 1... e5");
        return;
    }

    // 3. Knight move 2. Nf3
    Move g1f3(sq("g1"), sq("f3"));
    auto u3 = board.make_move(g1f3);
    if (board.zobrist_key() != zobrist::compute_hash(board)) {
        FAIL("Incremental hash mismatch after 2. Nf3");
        return;
    }

    // Undo all moves and check that key returns to initial
    board.undo_move(g1f3, u3);
    if (board.zobrist_key() != zobrist::compute_hash(board)) {
        FAIL("Incremental hash mismatch after undoing 2. Nf3");
        return;
    }

    board.undo_move(e7e5, u2);
    if (board.zobrist_key() != zobrist::compute_hash(board)) {
        FAIL("Incremental hash mismatch after undoing 1... e5");
        return;
    }

    board.undo_move(e2e4, u1);
    if (board.zobrist_key() != initial_key) {
        FAIL("Zobrist key did not restore to exact initial starting key");
        return;
    }

    PASS();
}

// ============================================================
// Test 3: Castling & En Passant Incremental Hash
// ============================================================
void test_castling_and_ep_hashing() {
    TEST("Castling & En Passant incremental Zobrist hashing");

    // White Kingside Castling
    Board castling_board;
    castling_board.set_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    Move castle_white_k(sq("e1"), sq("g1"), MoveFlags::KINGSIDE_CASTLE);
    auto undo_castle = castling_board.make_move(castle_white_k);
    if (castling_board.zobrist_key() != zobrist::compute_hash(castling_board)) {
        FAIL("Incremental hash mismatch after white kingside castling");
        return;
    }
    castling_board.undo_move(castle_white_k, undo_castle);
    if (castling_board.zobrist_key() != zobrist::compute_hash(castling_board)) {
        FAIL("Incremental hash mismatch after undoing castling");
        return;
    }

    // En Passant Capture
    Board ep_board;
    ep_board.set_from_fen("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
    Move ep_move(sq("e5"), sq("f6"), MoveFlags::EN_PASSANT);
    auto undo_ep = ep_board.make_move(ep_move);
    if (ep_board.zobrist_key() != zobrist::compute_hash(ep_board)) {
        FAIL("Incremental hash mismatch after en passant capture");
        return;
    }
    ep_board.undo_move(ep_move, undo_ep);
    if (ep_board.zobrist_key() != zobrist::compute_hash(ep_board)) {
        FAIL("Incremental hash mismatch after undoing en passant capture");
        return;
    }

    PASS();
}

// ============================================================
// Test 4: Transposition Table Store & Probe
// ============================================================
void test_tt_store_and_probe() {
    TEST("Transposition Table basic store and probe");

    TranspositionTable tt(1); // 1 MB
    uint64_t key = 0x123456789ABCDEF0ULL;
    Move best(sq("e2"), sq("e4"));

    tt.store(key, 4, 2, 150, TTNodeType::EXACT, best);

    int out_score = 0;
    Move out_move;

    // Exact probe with depth <= 4 should succeed
    bool hit = tt.probe(key, 4, 2, -1000, 1000, out_score, out_move);
    if (!hit || out_score != 150 || out_move.from != sq("e2") || out_move.to != sq("e4")) {
        FAIL("Probe failed to retrieve exact match");
        return;
    }

    // Probe with depth > 4 should return false for cutoff, but still return out_move
    Move deeper_move;
    int deeper_score = 0;
    bool deeper_hit = tt.probe(key, 5, 2, -1000, 1000, deeper_score, deeper_move);
    if (deeper_hit) {
        FAIL("Probe returned true cutoff for insufficient depth");
        return;
    }
    if (deeper_move.from != sq("e2") || deeper_move.to != sq("e4")) {
        FAIL("Probe failed to provide TT move when depth was insufficient");
        return;
    }

    PASS();
}

// ============================================================
// Test 5: TT Bounds (Cutoffs)
// ============================================================
void test_tt_bounds() {
    TEST("Transposition Table bounds cutoff logic");

    TranspositionTable tt(1);
    uint64_t key1 = 0xAAAAAAAAAAAAAAAAULL;
    uint64_t key2 = 0xBBBBBBBBBBBBBBBBULL;
    Move m(sq("d2"), sq("d4"));

    // LOWER_BOUND (beta cutoff: real score >= 200)
    tt.store(key1, 3, 0, 200, TTNodeType::LOWER_BOUND, m);
    int score = 0;
    Move out_m;

    // If beta is 150, stored score (200) >= beta (150) -> should cutoff
    if (!tt.probe(key1, 3, 0, 100, 150, score, out_m) || score != 200) {
        FAIL("LOWER_BOUND failed to trigger cutoff when score >= beta");
        return;
    }
    // If beta is 250, stored score (200) < beta (250) -> should not cutoff
    if (tt.probe(key1, 3, 0, 100, 250, score, out_m)) {
        FAIL("LOWER_BOUND triggered cutoff when score < beta");
        return;
    }

    // UPPER_BOUND (all-node fail-low: real score <= -50)
    tt.store(key2, 3, 0, -50, TTNodeType::UPPER_BOUND, m);
    // If alpha is 0, stored score (-50) <= alpha (0) -> should cutoff
    if (!tt.probe(key2, 3, 0, 0, 100, score, out_m) || score != -50) {
        FAIL("UPPER_BOUND failed to trigger cutoff when score <= alpha");
        return;
    }
    // If alpha is -100, stored score (-50) > alpha (-100) -> should not cutoff
    if (tt.probe(key2, 3, 0, -100, 100, score, out_m)) {
        FAIL("UPPER_BOUND triggered cutoff when score > alpha");
        return;
    }

    PASS();
}

// ============================================================
// Test 6: Mate Score Normalization across Plies
// ============================================================
void test_mate_score_normalization() {
    TEST("Transposition Table mate score normalization");

    int mate_score_ply2 = SCORE_MATE - 2; // Mate in 2 from root (ply 2)
    int stored = TranspositionTable::score_to_tt(mate_score_ply2, 2);
    // score_to_tt adds ply for positive mate -> SCORE_MATE
    if (stored != SCORE_MATE) {
        FAIL("score_to_tt did not normalize mate score correctly");
        return;
    }

    // When retrieved at ply 4, it should be SCORE_MATE - 4
    int retrieved_ply4 = TranspositionTable::score_from_tt(stored, 4);
    if (retrieved_ply4 != SCORE_MATE - 4) {
        FAIL("score_from_tt did not adjust mate score for new ply");
        return;
    }

    PASS();
}

// ============================================================
// Test 7: Transposition Search Efficiency (TT Cuts Search Nodes)
// ============================================================
void test_engine_tt_speedup() {
    TEST("Engine search node reduction with Transposition Table");

    Board board;
    board.set_from_fen("r1bqkb1r/pppp1ppp/2n5/4p3/2B1n3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 5");

    Engine engine(32);
    engine.set_position(board);

    // First search: populates the TT
    auto res1 = engine.search(500, 4);

    // Second search on identical position: TT should yield instant hits and fewer nodes
    auto res2 = engine.search(500, 4);

    if (res1.best_move.from == res1.best_move.to) {
        FAIL("Engine failed to find a valid move");
        return;
    }

    if (res2.nodes >= res1.nodes) {
        FAIL("Transposition table did not reduce nodes searched on repeat evaluation");
        return;
    }

    PASS();
}

int main() {
    std::cout << "=================================================\n";
    std::cout << "   Phase 6.3: Transposition Table & Zobrist Tests\n";
    std::cout << "=================================================\n";

    test_zobrist_determinism();
    test_incremental_zobrist();
    test_castling_and_ep_hashing();
    test_tt_store_and_probe();
    test_tt_bounds();
    test_mate_score_normalization();
    test_engine_tt_speedup();

    std::cout << "\n-------------------------------------------------\n";
    std::cout << "Results: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "=================================================\n";

    return (g_fail == 0) ? 0 : 1;
}
