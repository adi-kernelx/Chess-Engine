/**
 * elo.h — standard ELO rating calculation.
 *
 * Pure math, header-only. No database dependency — this is a function the
 * repository layer calls, but it can also be tested in isolation without
 * Postgres.
 *
 * Formula (FIDE-style):
 *   Expected score:  E_a = 1 / (1 + 10^((R_b - R_a) / 400))
 *   New rating:      R_a' = R_a + K * (S_a - E_a)
 *   where S_a = 1.0 (win), 0.5 (draw), 0.0 (loss).
 *
 * K-factor is uniformly 32 (standard for sub-2400 players). A future
 * enhancement could vary K by games played or rating band, but that adds
 * complexity with no immediate benefit for this project.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace chess {
namespace storage {

/// The outcome of an ELO recalculation after a game.
struct EloUpdate {
    int white_new;    ///< New white rating (floored at 0)
    int black_new;    ///< New black rating (floored at 0)
    int white_delta;  ///< Signed change for white (for display: "+18" or "-14")
    int black_delta;  ///< Signed change for black
};

/// Calculate new ELO ratings after a game.
///
/// @param white_elo  White's rating before the game.
/// @param black_elo  Black's rating before the game.
/// @param result     One of "1-0" (white wins), "0-1" (black wins),
///                   or "1/2-1/2" (draw).
/// @param k_factor   Sensitivity constant (default 32).
/// @return           New ratings and deltas for both players.
inline EloUpdate calculate_elo(int white_elo, int black_elo,
                               const std::string& result, int k_factor = 32) {
    // Expected scores
    const double e_white = 1.0 / (1.0 + std::pow(10.0, static_cast<double>(black_elo - white_elo) / 400.0));
    const double e_black = 1.0 - e_white;

    // Actual scores
    double s_white = 0.0;
    double s_black = 0.0;
    if (result == "1-0") {
        s_white = 1.0;
        s_black = 0.0;
    } else if (result == "0-1") {
        s_white = 0.0;
        s_black = 1.0;
    } else {
        // "1/2-1/2" or any draw variant
        s_white = 0.5;
        s_black = 0.5;
    }

    // New ratings, floored at 0
    const int white_new = std::max(0, white_elo + static_cast<int>(std::round(k_factor * (s_white - e_white))));
    const int black_new = std::max(0, black_elo + static_cast<int>(std::round(k_factor * (s_black - e_black))));

    return EloUpdate{
        white_new,
        black_new,
        white_new - white_elo,
        black_new - black_elo
    };
}

} // namespace storage
} // namespace chess
