#include "storage/postgres_game_store.h"

#include "analysis/cheat_report_repo.h"

namespace chess {
namespace storage {

namespace app_ports = chess::application::ports;

namespace {

// Build a synthetic QueryResult so we can reuse `classify()` — the free
// functions do not return a `QueryResult`, but they DO leave the last
// exec's sqlstate on the Database. That's the same source `classify()`
// consults for real QueryResults, so this stays in sync automatically.
StorageError classify_after(const Database& db, const std::string& error_text) {
    QueryResult synthetic;
    synthetic.ok       = false;
    synthetic.sqlstate = db.last_sqlstate();
    synthetic.error    = error_text;
    return classify(synthetic);
}

} // namespace

PostgresGameStore::PostgresGameStore(Database& db) : db_(db) {}

app_ports::SaveGameOutcome
PostgresGameStore::save_completed_game(const CompletedGame& game) {
    // Delegate to the existing free function. It runs its own multi-
    // statement transaction (BEGIN + inserts + UPDATEs + COMMIT) and
    // returns `SaveGameResult { ok, game_id, elo, error }`.
    auto res = chess::storage::save_completed_game(db_, game);

    app_ports::SaveGameOutcome out;
    if (res.ok) {
        out.code              = StorageError::Ok;
        out.game_id           = res.game_id;
        out.elo               = res.elo;
        out.already_persisted = res.already_persisted;
    } else {
        // A concurrent duplicate that lost the partial-unique-index
        // race arrives here as a UniqueViolation. Reclassify it as
        // "already persisted": the caller's retry policy is satisfied
        // because the row now exists (the winning writer inserted it),
        // and the caller only needs the game_id — fetch it explicitly.
        auto code = classify_after(db_, res.error);
        if (code == StorageError::UniqueViolation && !game.completion_uuid.empty()) {
            auto probe = db_.exec(
                "SELECT id FROM games WHERE completion_uuid = $1",
                {Param::text(game.completion_uuid)});
            if (probe.ok && !probe.empty()) {
                out.code              = StorageError::Ok;
                out.already_persisted = true;
                out.game_id           = std::stoll(probe.first().at(0));
                return out;
            }
        }
        out.code  = code;
        out.error = res.error;
    }
    return out;
}

app_ports::SaveCheatReportOutcome
PostgresGameStore::save_cheat_report(
        int64_t                                game_id,
        int64_t                                player_id,
        const std::string&                     side,
        const chess::analysis::AnalysisReport& report) {
    auto res = chess::analysis::save_cheat_report(db_, game_id, player_id,
                                                  side, report);

    app_ports::SaveCheatReportOutcome out;
    if (res.ok) {
        out.code = StorageError::Ok;
        out.id   = res.id;
    } else {
        out.code  = classify_after(db_, res.error);
        out.error = res.error;
    }
    return out;
}

} // namespace storage
} // namespace chess
