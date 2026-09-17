/**
 * cheat_report_repo.h — persistence layer for Phase 9.3 analyzer verdicts.
 *
 * Upserts on (game_id, player_id) so re-running the analyzer on the same
 * game refreshes the row rather than creating duplicates. This matches the
 * intent of the flag decision: it is a rolling verdict, not an event log.
 * If we later want an event log, that's a `cheat_report_history` table —
 * one more row on each upsert — layered on top.
 */

#pragma once

#include "analysis/anti_cheat.h"
#include "storage/database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chess {
namespace analysis {

/// One row of `cheat_reports`, as retrieved.
struct StoredCheatReport {
    int64_t     id                    = 0;
    int64_t     game_id               = 0;
    int64_t     player_id             = 0;
    std::string side;                       ///< "w" or "b"
    int         plies_analyzed        = 0;
    int         plies_matched_engine  = 0;

    // std::optional<double> — represents SQL NULL for analyzer-NaN cases.
    std::optional<double> engine_agreement_pct;
    std::optional<double> time_cv;
    std::optional<double> complexity_corr;

    bool                     flagged     = false;
    std::vector<std::string> reasons;   ///< parsed from the newline-joined column
    std::string              created_at; ///< ISO 8601 UTC as read from Postgres
};

struct SaveCheatReportResult {
    bool        ok       = false;
    std::string error;
    int64_t     id       = 0;   ///< generated primary key on success
};

/// Insert or update the report for (game_id, player_id). Uses ON CONFLICT
/// so retries after a transient DB failure do not create duplicates.
SaveCheatReportResult save_cheat_report(storage::Database& db,
                                        int64_t game_id,
                                        int64_t player_id,
                                        const std::string& side,
                                        const AnalysisReport& report);

/// Retrieve reports for one game. Zero, one, or two rows (per side).
/// Ordered by side so a caller can rely on a stable enumeration.
std::vector<StoredCheatReport> get_reports_for_game(storage::Database& db,
                                                    int64_t game_id);

/// Retrieve the most-recent flagged reports across the whole table, up to
/// `limit`. Uses the partial index `idx_cheat_reports_flagged`. Intended
/// for a moderation dashboard, not for a per-player query.
std::vector<StoredCheatReport> get_recent_flagged(storage::Database& db,
                                                   int limit);

} // namespace analysis
} // namespace chess
