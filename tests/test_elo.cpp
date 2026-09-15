/**
 * test_elo.cpp — ELO calculation tests.
 *
 * Pure math tests — no database, no Postgres, no environment variables
 * needed. Runs everywhere.
 *
 * The FIDE-style ELO formula is deterministic: given the same inputs, it
 * always produces the same output. These tests verify the formula against
 * hand-calculated expected values and edge cases (floor at zero, symmetric
 * results, K-factor variation).
 */

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

#include "storage/elo.h"

using namespace chess::storage;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (fn()) {
        std::cout << "PASS\n";
        ++g_passed;
    } else {
        std::cout << "FAIL\n";
        ++g_failed;
    }
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " ELO Calculation Tests\n";
    std::cout << "========================================\n";

    // ── Equal-rated tests ───────────────────────────────────────────

    run_test("White wins at equal ELO (1200 vs 1200)", [] {
        auto e = calculate_elo(1200, 1200, "1-0");
        // E = 0.5 for both. S_white=1.0. Delta = 32*(1.0-0.5) = 16.
        return e.white_delta == 16 && e.black_delta == -16 &&
               e.white_new == 1216 && e.black_new == 1184;
    });

    run_test("Black wins at equal ELO (1200 vs 1200)", [] {
        auto e = calculate_elo(1200, 1200, "0-1");
        // Symmetric to white-wins case.
        return e.white_delta == -16 && e.black_delta == 16 &&
               e.white_new == 1184 && e.black_new == 1216;
    });

    run_test("Draw at equal ELO: both unchanged", [] {
        auto e = calculate_elo(1200, 1200, "1/2-1/2");
        // E = 0.5, S = 0.5. Delta = 32*(0.5-0.5) = 0.
        return e.white_delta == 0 && e.black_delta == 0 &&
               e.white_new == 1200 && e.black_new == 1200;
    });

    // ── Upset tests ─────────────────────────────────────────────────

    run_test("Underdog wins (1000 beats 1600): large gain", [] {
        auto e = calculate_elo(1000, 1600, "1-0");
        // E_white ≈ 0.0307. S_white=1.0. Delta = round(32*(1.0-0.0307)) = 31.
        // Underdog gains ~31, favourite loses ~31.
        return e.white_delta >= 28 && e.white_delta <= 32 &&
               e.black_delta <= -28 && e.black_delta >= -32 &&
               e.white_new > e.black_new - 600;  // gap closes significantly
    });

    run_test("Expected result (1600 beats 1000): small gain", [] {
        auto e = calculate_elo(1600, 1000, "1-0");
        // E_white ≈ 0.9091. S_white=1.0. Delta ≈ 32*(1.0-0.9091) ≈ 3.
        return e.white_delta > 0 && e.white_delta <= 5 &&
               e.black_delta < 0 && e.black_delta >= -5;
    });

    run_test("Draw between unequals: both move toward each other", [] {
        auto e = calculate_elo(1600, 1000, "1/2-1/2");
        // E_white ≈ 0.9091, S_white = 0.5. Delta ≈ 32*(0.5-0.9091) ≈ -13.
        // Favourite loses rating, underdog gains — the draw was an upset.
        return e.white_delta < 0 && e.black_delta > 0 &&
               e.white_new < 1600 && e.black_new > 1000;
    });

    // ── Symmetry test ───────────────────────────────────────────────

    run_test("Symmetry: swapping sides and result swaps deltas", [] {
        // Same game from opposite perspectives:
        //   Game A: 1300 (white) beats 1500 (black)
        //   Game B: 1500 (white) loses to 1300 (black)
        // The deltas should swap: A.white_delta == B.black_delta, etc.
        auto a = calculate_elo(1300, 1500, "1-0");
        auto b = calculate_elo(1500, 1300, "0-1");
        return a.white_delta == b.black_delta &&
               a.black_delta == b.white_delta;
    });

    // ── Floor at zero ───────────────────────────────────────────────

    run_test("Floor at zero: very low rated player losing", [] {
        auto e = calculate_elo(10, 1600, "0-1");
        // White's new rating should be 0, not negative.
        return e.white_new >= 0;
    });

    run_test("Floor at zero: 0-rated player losing", [] {
        auto e = calculate_elo(0, 1200, "0-1");
        return e.white_new == 0 && e.white_delta == 0;
    });

    // ── Custom K-factor ─────────────────────────────────────────────

    run_test("K=16: deltas are half of K=32 at equal ratings", [] {
        auto k32 = calculate_elo(1200, 1200, "1-0", 32);
        auto k16 = calculate_elo(1200, 1200, "1-0", 16);
        // At equal ratings with K=32: delta=16. With K=16: delta=8.
        return k16.white_delta == k32.white_delta / 2 &&
               k16.black_delta == k32.black_delta / 2;
    });

    // ── Deltas sum to zero (conservation) ───────────────────────────

    run_test("Conservation: white_delta + black_delta == 0 (equal ratings)", [] {
        auto w = calculate_elo(1200, 1200, "1-0");
        auto d = calculate_elo(1200, 1200, "1/2-1/2");
        return (w.white_delta + w.black_delta == 0) &&
               (d.white_delta + d.black_delta == 0);
    });

    run_test("Conservation: white_delta + black_delta == 0 (unequal ratings)", [] {
        auto e = calculate_elo(1400, 1100, "0-1");
        // Due to rounding, the sum might be ±1 at most.
        return std::abs(e.white_delta + e.black_delta) <= 1;
    });

    // ── Summary ─────────────────────────────────────────────────────
    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed
              << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
