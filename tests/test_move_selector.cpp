/**
 * test_move_selector.cpp — LLD-6.2 MoveSelector port + Engine adapter.
 *
 * Contract tests for the port. Three layers:
 *
 *   1. A stack-only `FakeMoveSelector` returns a canned move so
 *      service tests can substitute it without linking the engine.
 *      This is the pattern every future GameplayService test that
 *      wants to observe "AI slot was asked" will use.
 *
 *   2. `EngineMoveSelector` — the production adapter over
 *      `chess::engine::Engine`. Pinned to a small depth so this
 *      test stays fast, but the point is to prove the adapter
 *      wires `SearchLimits` through and returns a legal move for
 *      the start position.
 *
 *   3. `limits_for(AIDifficulty)` — the "difficulty as
 *      configuration" step. The four presets map to the pairs the
 *      pre-refactor `difficulty_params` switch produced; a test
 *      pins that mapping.
 */

#include <functional>
#include <iostream>
#include <string>

#include "application/ports/move_selector.h"
#include "chess/board.h"
#include "chess/engine_move_selector.h"
#include "chess/move_gen.h"
#include "game/ai_player.h"

using chess::application::ports::MoveChoice;
using chess::application::ports::MoveSelector;
using chess::application::ports::SearchLimits;
using chess::EngineMoveSelector;
using chess::Board;
using chess::game::AIDifficulty;
using chess::game::limits_for;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) { std::cout << "PASS\n"; ++g_passed; }
        else      { std::cout << "FAIL\n"; ++g_failed; }
    } catch (const std::exception& e) {
        std::cout << "FAIL (exception: " << e.what() << ")\n"; ++g_failed;
    }
}

/// Canned-move selector used to prove services can substitute the
/// engine cheaply. Also records the last limits it was called with.
struct FakeMoveSelector final : MoveSelector {
    MoveChoice next;
    SearchLimits last_limits;
    int         calls = 0;
    MoveChoice select(const Board&, const SearchLimits& lim) override {
        ++calls;
        last_limits = lim;
        return next;
    }
};

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-6.2 — MoveSelector port\n";
    std::cout << "========================================\n";

    // ── FakeMoveSelector ───────────────────────────────────────────────

    run_test("FakeMoveSelector returns the canned move", []() {
        FakeMoveSelector fake;
        fake.next.from      = 12;   // e2
        fake.next.to        = 28;   // e4
        fake.next.promotion = chess::PieceType::NONE;
        Board b = Board::starting_position();
        auto out = fake.select(b, SearchLimits{});
        return out.from == 12 && out.to == 28 && fake.calls == 1;
    });

    run_test("FakeMoveSelector remembers the limits it was called with", []() {
        FakeMoveSelector fake;
        Board b = Board::starting_position();
        fake.select(b, SearchLimits{6, 3000});
        return fake.last_limits.max_depth == 6
            && fake.last_limits.time_ms == 3000;
    });

    // ── limits_for(difficulty) ─────────────────────────────────────────

    run_test("limits_for(EASY) = {2, 200}", []() {
        auto l = limits_for(AIDifficulty::EASY);
        return l.max_depth == 2 && l.time_ms == 200;
    });
    run_test("limits_for(MEDIUM) = {4, 1000}", []() {
        auto l = limits_for(AIDifficulty::MEDIUM);
        return l.max_depth == 4 && l.time_ms == 1000;
    });
    run_test("limits_for(HARD) = {6, 3000}", []() {
        auto l = limits_for(AIDifficulty::HARD);
        return l.max_depth == 6 && l.time_ms == 3000;
    });
    run_test("limits_for(MAX) = {64, 5000}", []() {
        auto l = limits_for(AIDifficulty::MAX);
        return l.max_depth == 64 && l.time_ms == 5000;
    });

    // ── EngineMoveSelector adapter (real search, small depth) ──────────

    run_test("EngineMoveSelector returns a legal move for start position", []() {
        EngineMoveSelector sel(4);   // 4 MB TT is plenty for depth 2
        Board b = Board::starting_position();
        auto out = sel.select(b, SearchLimits{2, 200});
        if (out.from == chess::NO_SQUARE || out.to == chess::NO_SQUARE) {
            return false;
        }
        // Confirm the move is legal.
        auto legal = chess::move_gen::generate_legal_moves(b);
        for (const auto& m : legal) {
            if (m.from == out.from && m.to == out.to) return true;
        }
        return false;
    });

    run_test("EngineMoveSelector reports non-zero nodes for real search", []() {
        EngineMoveSelector sel(4);
        Board b = Board::starting_position();
        auto out = sel.select(b, SearchLimits{2, 200});
        return out.nodes > 0;
    });

    // ── summary ────────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
