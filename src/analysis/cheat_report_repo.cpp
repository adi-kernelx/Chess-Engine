/**
 * cheat_report_repo.cpp — see header for API contract.
 *
 * Every mutation goes through `Database::exec()` with `PQexecParams`. No
 * string concatenation of values into SQL, same rule the rest of Phase 7-9
 * follows.
 *
 * NaN → NULL discipline: the analyzer emits NaN when a signal is not
 * computable ("we cannot say"); the DB stores NULL for it and readers get
 * `std::optional<double>::has_value() == false`. Never silently coerce NaN
 * to 0 — a downstream reviewer would treat that as "signal was clean."
 */

#include "analysis/cheat_report_repo.h"

#include <cmath>
#include <sstream>

using chess::storage::Database;
using chess::storage::Param;
using chess::storage::QueryResult;

namespace chess {
namespace analysis {

namespace {

// Postgres accepts double values in text mode; NaN becomes NULL.
Param optional_double_param(double d) {
    if (std::isnan(d)) return Param::null();
    std::ostringstream oss;
    // Full precision so a read-back exactly reproduces the analyzer's value
    // (matters for tests that compare `time_cv` byte-for-byte across a save-
    // then-load round-trip).
    oss.precision(17);
    oss << d;
    return Param::text(oss.str());
}

// Join a reasons vector into the newline-separated column format.
std::string join_reasons(const std::vector<std::string>& reasons) {
    std::string out;
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out += reasons[i];
    }
    return out;
}

std::vector<std::string> split_reasons(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(std::move(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(std::move(cur));
    return out;
}

// Fill a StoredCheatReport from a row. Column order MUST match the SELECT
// column list below in both read functions; keeping the row_to_stored
// helper in one place makes that harder to drift.
StoredCheatReport row_to_stored(const storage::Row& row) {
    StoredCheatReport s;
    s.id                   = std::stoll(row.at(0));
    s.game_id              = std::stoll(row.at(1));
    s.player_id            = std::stoll(row.at(2));
    s.side                 = row.at(3);
    s.plies_analyzed       = std::stoi (row.at(4));
    s.plies_matched_engine = std::stoi (row.at(5));

    if (!row.is_null(6)) s.engine_agreement_pct = std::stod(row.at(6));
    if (!row.is_null(7)) s.time_cv              = std::stod(row.at(7));
    if (!row.is_null(8)) s.complexity_corr      = std::stod(row.at(8));

    s.flagged    = (row.at(9) == "t" || row.at(9) == "true");
    s.reasons    = split_reasons(row.at(10));
    s.created_at = row.at(11);
    return s;
}

constexpr const char* SELECT_COLUMNS =
    "id, game_id, player_id, side, "
    "plies_analyzed, plies_matched_engine, "
    "engine_agreement_pct, time_cv, complexity_corr, "
    "flagged, reasons, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";

} // namespace

SaveCheatReportResult save_cheat_report(Database& db,
                                        int64_t game_id,
                                        int64_t player_id,
                                        const std::string& side,
                                        const AnalysisReport& report) {
    SaveCheatReportResult out;

    // The plies_matched_engine field is emitted as-is (analyzer counted
    // only non-terminal plies into it). The engine_agreement_pct value
    // may be NaN — optional_double_param converts to NULL.
    QueryResult r = db.exec(
        "INSERT INTO cheat_reports "
        "(game_id, player_id, side, plies_analyzed, plies_matched_engine, "
        " engine_agreement_pct, time_cv, complexity_corr, flagged, reasons) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
        "ON CONFLICT (game_id, player_id) DO UPDATE SET "
        "  side                 = EXCLUDED.side, "
        "  plies_analyzed       = EXCLUDED.plies_analyzed, "
        "  plies_matched_engine = EXCLUDED.plies_matched_engine, "
        "  engine_agreement_pct = EXCLUDED.engine_agreement_pct, "
        "  time_cv              = EXCLUDED.time_cv, "
        "  complexity_corr      = EXCLUDED.complexity_corr, "
        "  flagged              = EXCLUDED.flagged, "
        "  reasons              = EXCLUDED.reasons, "
        "  created_at           = now() "
        "RETURNING id",
        {Param::int64(game_id),
         Param::int64(player_id),
         Param::text(side),
         Param::int64(report.plies_analyzed),
         Param::int64(report.plies_matched_engine),
         optional_double_param(report.engine_agreement_pct),
         optional_double_param(report.time_cv),
         optional_double_param(report.complexity_corr),
         Param::boolean(report.flagged),
         Param::text(join_reasons(report.reasons))});

    if (!r.ok) {
        out.error = r.error;
        return out;
    }
    if (r.rows.empty()) {
        out.error = "INSERT ... RETURNING returned no row";
        return out;
    }
    out.id = std::stoll(r.rows[0].at(0));
    out.ok = true;
    return out;
}

std::vector<StoredCheatReport> get_reports_for_game(Database& db,
                                                    int64_t game_id) {
    std::vector<StoredCheatReport> out;
    QueryResult r = db.exec(
        std::string("SELECT ") + SELECT_COLUMNS +
        " FROM cheat_reports WHERE game_id = $1 ORDER BY side ASC",
        {Param::int64(game_id)});
    if (!r.ok) return out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row_to_stored(row));
    return out;
}

std::vector<StoredCheatReport> get_recent_flagged(Database& db, int limit) {
    std::vector<StoredCheatReport> out;
    if (limit <= 0) return out;
    QueryResult r = db.exec(
        std::string("SELECT ") + SELECT_COLUMNS +
        " FROM cheat_reports WHERE flagged = TRUE "
        " ORDER BY created_at DESC LIMIT $1",
        {Param::int64(limit)});
    if (!r.ok) return out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row_to_stored(row));
    return out;
}

} // namespace analysis
} // namespace chess
