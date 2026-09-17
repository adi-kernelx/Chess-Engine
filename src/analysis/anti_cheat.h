/**
 * anti_cheat.h — Phase 9.3.
 *
 * Statistical anti-cheat analyzer. Pure functions over per-ply data; no
 * database, no engine, no network. Composable with the game_handler that
 * feeds it and with the cheat_report_repo that stores its verdicts.
 *
 * Three signals, each hand-picked because a naive engine user leaks on it:
 *
 *   1. Engine agreement — %  of plies where the player's move matched the
 *      engine's best. A strong human averages 40-60%; a full-strength engine
 *      user reaches 85%+ almost by definition.
 *
 *   2. Time coefficient of variation (CV = stddev / mean). Humans spend more
 *      time on hard positions than easy ones, so their think-time series has
 *      significant variance. A bot that spends a constant time per move
 *      produces near-zero CV.
 *
 *   3. Complexity correlation — Pearson r between think-time and
 *      legal-move-count (the cheap complexity proxy). A human sees more
 *      candidate moves and thinks longer; a bot's compute cost does not
 *      correlate with the branching factor at the current node.
 *
 * All three are computed for one side only — the analyzer is invoked twice
 * per game (white plies, black plies), because a single cheater does not
 * necessarily face another cheater. Combining the sides would let a strong
 * opponent's clean play dilute a suspect's signal.
 *
 * NOT AN AUTO-BAN. The plan is explicit: flag, store, review. The verdict
 * is a report row that a human operator (or later, a moderation dashboard)
 * consults. False positives are a real hazard — a titled player in an
 * opening they know cold can hit 90%+ agreement for the first 15 plies
 * without any tool involved.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace chess {
namespace analysis {

/// Per-ply raw input. One entry per one player's ply (odd indices for
/// white, even for black, when built from a full-game sequence).
struct PlyData {
    int  think_time_ms       = 0;    ///< how long this side thought before the move
    int  legal_move_count    = 0;    ///< number of legal moves in the pre-move position
    bool matched_engine      = false;///< did the player's move == engine's best move?
    bool terminal_after      = false;///< did this ply produce mate / stalemate?
};

/// The verdict for one side of one game.
struct AnalysisReport {
    int    plies_analyzed        = 0;
    int    plies_matched_engine  = 0;

    /// 0.0 - 100.0. NaN when plies_analyzed == 0 (no verdict possible).
    double engine_agreement_pct  = 0.0;

    /// Coefficient of variation of think times. NaN when the mean is 0
    /// (which happens only for a null-input game — real games always have
    /// at least one non-zero think time recorded).
    double time_cv               = 0.0;

    /// Pearson r of (legal_move_count, think_time_ms). NaN when either
    /// series has zero variance — e.g., a game with only one legal move
    /// per position, or a bot with constant think times. Callers must
    /// treat NaN as "signal unavailable", not as "signal clean".
    double complexity_corr       = 0.0;

    bool                     flagged = false;
    std::vector<std::string> reasons;         ///< human-readable flag reasons
};

/// Thresholds for the flag decision. Defaults picked from the plan and
/// tightened against small hand-crafted test games; every threshold is
/// documented in place.
struct AnalysisThresholds {
    /// Below this many analyzed plies, no verdict is issued (flagged=false,
    /// no reasons). A 4-ply blitz miniature is not enough signal to accuse
    /// anyone. The plan uses 20 for the strong version of the engine-
    /// agreement check; we use 10 as the *floor* below which nothing is
    /// flagged at all.
    int min_plies                     = 10;

    /// Engine agreement % above which the report is flagged for
    /// engine-assisted-play suspicion. 85 is the plan's threshold; a strong
    /// human very rarely sustains this over 20+ plies.
    double engine_agreement_flag_pct  = 85.0;

    /// Time CV below which the "abnormally uniform think times" flag fires.
    /// A CV of 0.20 means the stddev is one-fifth of the mean — a human
    /// under time pressure will comfortably exceed this because they blitz
    /// obvious recaptures and stew on quiet moves.
    double time_cv_flag_below         = 0.20;

    /// Complexity correlation below which the "no correlation with position
    /// complexity" flag fires. A weakly-positive value (0.10) is enough to
    /// clear a human — humans reliably think longer on branchier positions.
    /// Negative or near-zero correlation is the tell.
    double complexity_corr_flag_below = 0.10;
};

/// Compute the report for ONE side of ONE game.
///
/// `plies` should contain only the target player's plies. Terminal plies
/// (mate / stalemate) are excluded from the engine-agreement denominator
/// because there is nothing to "agree with" — every legal move is a mate.
AnalysisReport analyze_side(const std::vector<PlyData>& plies,
                            const AnalysisThresholds& t = {});

} // namespace analysis
} // namespace chess
