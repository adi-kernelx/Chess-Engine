/**
 * application/ports/game_store.h — the write side of persistence (LLD-3.2).
 *
 * WHAT THIS PORT REPRESENTS
 *
 * Everything the application layer needs to persist about a completed game:
 * the game record itself and per-side anti-cheat verdicts. Two operations,
 * no more:
 *
 *   1. `save_completed_game` — atomic INSERT games + move_times + ELO/stats
 *      updates for both players. Owned in production by `PostgresGameStore`,
 *      which delegates to `storage::save_completed_game`.
 *
 *   2. `save_cheat_report`   — upsert-on-(game_id, player_id). Delegates
 *      to `analysis::save_cheat_report`.
 *
 * WHY A PORT
 *
 * `GameplayService::persist_game` and `AnalysisService::analyze_game` are the
 * only two writers in the codebase. Before this port, both held a raw
 * `storage::Database*` and called the free functions directly, which meant
 * (a) every application-level test that wanted to verify persistence had to
 * link against libpq and spin a real cluster, and (b) the services carried
 * an implicit dependency on Postgres-shaped errors (bool + string) and had
 * to log them without any typed classification. The port fixes both: the
 * outcome types carry a `storage::StorageError` (LLD-3.1), and tests can
 * substitute a `RecordingGameStore` in-memory fake with zero libpq.
 *
 * READ VS WRITE SPLIT
 *
 * Reads (`find_game_by_id`, `get_player_games`, profile queries, etc.) belong
 * to a separate `PlayerQueries` port introduced in LLD-3.3. Splitting reads
 * from writes here is deliberate: the write port sees no `Database*` at all,
 * which keeps the read/write blast radius separable when a fake needs to
 * pretend one direction works and the other does not (e.g. simulating a
 * hot-standby read replica going stale).
 *
 * FAILURE SHAPE
 *
 * Both outcomes carry `storage::StorageError code` from `classify()`; the
 * raw adapter message stays available in `error` for logs. `ok()` is the
 * boolean shorthand — `code == StorageError::Ok`.
 */

#pragma once

#include <cstdint>
#include <string>

#include "analysis/anti_cheat.h"       // AnalysisReport — value input
#include "storage/elo.h"               // EloUpdate — value output
#include "storage/game_repo.h"         // CompletedGame — value input
#include "storage/storage_error.h"

namespace chess {
namespace application {
namespace ports {

struct SaveGameOutcome {
    chess::storage::StorageError code = chess::storage::StorageError::Ok;
    int64_t                      game_id = 0;
    chess::storage::EloUpdate    elo{};
    std::string                  error;  ///< adapter's raw message for logs

    /// LLD-4.2: true when the store found an existing row under
    /// `CompletedGame::completion_uuid` and the call was a no-op —
    /// `game_id` is the pre-existing row's id and no ELO/stat updates
    /// happened. Distinct from `!ok()`: the call still succeeded.
    bool already_persisted = false;

    bool ok() const { return code == chess::storage::StorageError::Ok; }
};

struct SaveCheatReportOutcome {
    chess::storage::StorageError code = chess::storage::StorageError::Ok;
    int64_t                      id   = 0;
    std::string                  error;
    bool ok() const { return code == chess::storage::StorageError::Ok; }
};

class GameStore {
public:
    virtual ~GameStore() = default;

    /// True when this store can actually persist. Used by
    /// `GameplayService::persist_game` to short-circuit silently in
    /// capability-disabled mode (no `DATABASE_URL` at boot): calling
    /// through to a `NullGameStore` would work but would log a spurious
    /// "Failed to persist … (disconnected)" line for every finished
    /// game. The read side has no equivalent because reads translate
    /// their `Disconnected` outcome into a client-facing error the
    /// user sees; a silent no-op is only correct for the write side.
    virtual bool capable() const = 0;

    /// Persist a completed game atomically. Returns an outcome carrying
    /// the assigned `game_id` and the calculated `EloUpdate` on success,
    /// or a typed `StorageError` on failure. Idempotency on retry is
    /// NOT guaranteed at this layer — the two callers today (persist_game,
    /// end-of-tournament reporter) know their retry policy.
    virtual SaveGameOutcome save_completed_game(
        const chess::storage::CompletedGame& game) = 0;

    /// Upsert the per-side anti-cheat verdict. Safe to call twice with
    /// the same (game_id, player_id) — the ON CONFLICT rewrites the row.
    virtual SaveCheatReportOutcome save_cheat_report(
        int64_t                                game_id,
        int64_t                                player_id,
        const std::string&                     side,
        const chess::analysis::AnalysisReport& report) = 0;
};

} // namespace ports
} // namespace application
} // namespace chess
