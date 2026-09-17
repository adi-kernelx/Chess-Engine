/**
 * anti_cheat.cpp — the math behind the header.
 *
 * Three helpers (mean, sample-population stddev, Pearson correlation) plus
 * one orchestrating function. Deliberately terse — every branch is exercised
 * by test_anti_cheat, and there is nothing here that would benefit from an
 * abstraction layer.
 */

#include "analysis/anti_cheat.h"

#include <cmath>
#include <limits>

namespace chess {
namespace analysis {

namespace {

// Population stddev of `xs` (divides by N, not N-1). Population is right
// here because the plies of one game are the whole population we care
// about; there is no larger set we are trying to estimate a stddev of.
double population_stddev(const std::vector<double>& xs, double mean) {
    if (xs.empty()) return std::numeric_limits<double>::quiet_NaN();
    double sq = 0.0;
    for (double x : xs) sq += (x - mean) * (x - mean);
    return std::sqrt(sq / static_cast<double>(xs.size()));
}

double mean_of(const std::vector<double>& xs) {
    if (xs.empty()) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for (double x : xs) s += x;
    return s / static_cast<double>(xs.size());
}

// Pearson r. Returns NaN if either input series has zero variance — that
// case is meaningful ("we cannot say", not "the correlation is zero") and
// the caller MUST NOT treat NaN as a clean signal.
double pearson_correlation(const std::vector<double>& xs,
                           const std::vector<double>& ys) {
    if (xs.size() != ys.size() || xs.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double mx = mean_of(xs);
    const double my = mean_of(ys);
    double num = 0.0, denx = 0.0, deny = 0.0;
    for (size_t i = 0; i < xs.size(); ++i) {
        const double dx = xs[i] - mx;
        const double dy = ys[i] - my;
        num  += dx * dy;
        denx += dx * dx;
        deny += dy * dy;
    }
    if (denx == 0.0 || deny == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return num / std::sqrt(denx * deny);
}

} // namespace

AnalysisReport analyze_side(const std::vector<PlyData>& plies,
                            const AnalysisThresholds& t) {
    AnalysisReport r;
    r.plies_analyzed = static_cast<int>(plies.size());

    // ── Engine agreement — exclude terminal-after plies from the denominator.
    //
    // A terminal ply has every legal move landing in the same result state
    // (mate is mate regardless of the checkmating piece choice, if any),
    // so an "agreement" there is not really information about play strength.
    int agreement_denom = 0;
    for (const auto& p : plies) {
        if (p.terminal_after) continue;
        ++agreement_denom;
        if (p.matched_engine) ++r.plies_matched_engine;
    }
    if (agreement_denom > 0) {
        r.engine_agreement_pct =
            100.0 * static_cast<double>(r.plies_matched_engine)
                  / static_cast<double>(agreement_denom);
    } else {
        r.engine_agreement_pct = std::numeric_limits<double>::quiet_NaN();
    }

    // ── Time CV.
    std::vector<double> times;
    times.reserve(plies.size());
    for (const auto& p : plies) times.push_back(static_cast<double>(p.think_time_ms));
    const double time_mean = mean_of(times);
    const double time_sd   = population_stddev(times, time_mean);
    if (time_mean > 0.0) {
        r.time_cv = time_sd / time_mean;
    } else {
        r.time_cv = std::numeric_limits<double>::quiet_NaN();
    }

    // ── Complexity correlation.
    std::vector<double> complexity;
    complexity.reserve(plies.size());
    for (const auto& p : plies) complexity.push_back(static_cast<double>(p.legal_move_count));
    r.complexity_corr = pearson_correlation(complexity, times);

    // ── Flag decision — only past the min_plies floor.
    if (r.plies_analyzed < t.min_plies) {
        r.flagged = false;
        return r;
    }

    // Engine agreement. NaN means agreement_denom was 0 (every ply
    // terminal) — that IS a degenerate case, so we do not flag on it.
    if (!std::isnan(r.engine_agreement_pct)
        && r.engine_agreement_pct >= t.engine_agreement_flag_pct)
    {
        r.flagged = true;
        r.reasons.push_back("engine_agreement_high");
    }

    // Time CV. NaN means the mean was 0 (a degenerate all-zero-times game
    // — cannot happen in practice because move_times are recorded from
    // steady_clock, which yields >= 1 ms per non-instant move). We do NOT
    // flag on NaN.
    if (!std::isnan(r.time_cv) && r.time_cv < t.time_cv_flag_below) {
        r.flagged = true;
        r.reasons.push_back("time_cv_low");
    }

    // Complexity correlation. NaN means one of the series had zero variance
    // (only one legal move per ply, or perfectly constant think times).
    // A game with truly constant complexity is playable but rare — we do
    // NOT flag on NaN, because "we could not compute" is not evidence.
    if (!std::isnan(r.complexity_corr)
        && r.complexity_corr < t.complexity_corr_flag_below)
    {
        r.flagged = true;
        r.reasons.push_back("complexity_correlation_low");
    }

    return r;
}

} // namespace analysis
} // namespace chess
