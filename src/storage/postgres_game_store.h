/**
 * storage/postgres_game_store.h — the Postgres adapter for the write
 * side of persistence (LLD-3.2).
 *
 * Implements `application::ports::GameStore` by delegating to the two
 * existing free functions:
 *
 *   save_completed_game → storage::save_completed_game
 *   save_cheat_report   → analysis::save_cheat_report
 *
 * Neither free function is rewritten in this slice — the plan doc says
 * "wrap proven SQL functions initially; avoid rewriting all queries."
 * The adapter's only real work is to translate the old `bool ok +
 * string error` shape into a typed `SaveGameOutcome` /
 * `SaveCheatReportOutcome`, using the connection's `last_sqlstate` to
 * populate `StorageError` via `classify()`.
 *
 * A future slice that pushes the whole-transaction ownership rewrite
 * (moving `save_completed_game`'s six manual ROLLBACK branches onto
 * `Transaction`) lives INSIDE this adapter — that's the SQL-layer
 * refactor, invisible to every application-level caller through the
 * port.
 */

#pragma once

#include "application/ports/game_store.h"
#include "storage/database.h"

namespace chess {
namespace storage {

class PostgresGameStore final : public chess::application::ports::GameStore {
public:
    /// `db` must outlive the store. The lifetime story is the same as
    /// the previous raw `Database*` pattern: `main.cpp` owns the
    /// Database, constructs the store beside it, and hands the store's
    /// pointer to the services.
    explicit PostgresGameStore(Database& db);

    bool capable() const override { return true; }

    chess::application::ports::SaveGameOutcome save_completed_game(
        const CompletedGame& game) override;

    chess::application::ports::SaveCheatReportOutcome save_cheat_report(
        int64_t                                game_id,
        int64_t                                player_id,
        const std::string&                     side,
        const chess::analysis::AnalysisReport& report) override;

private:
    Database& db_;
};

} // namespace storage
} // namespace chess
