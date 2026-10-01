/**
 * application/ports/null_persistence.h — capability-disabled adapters
 * for the `GameStore` and `PlayerQueries` ports (LLD-3.3).
 *
 * WHY THESE EXIST
 *
 * The plan doc §LLD-3 is explicit: "Required persistence is not
 * represented by an optional pointer that silently does nothing;
 * capability-disabled mode is explicit." Pre-LLD-3.3 the pattern was:
 *
 *     if (!db_) {
 *         caller_sink.send(make_error_frame("Profiles are not
 *                                            available (no database)"));
 *         return;
 *     }
 *
 * — a raw-pointer null check duplicated across every DB-touching
 * service method, silently downgrading the operation to an error. It
 * worked, but the "no DB" mode was implicit in the pointer being null
 * and there was no single object representing "persistence is off".
 *
 * `NullGameStore` and `NullPlayerQueries` are that single object.
 * `main.cpp` constructs them when `DATABASE_URL` is unset (or the
 * connection fails), and hands the SAME `PlayerQueries&` /
 * `GameStore&` references to services in both modes. Services stop
 * doing null checks; they call through the port and translate the
 * `StorageError::Disconnected` outcome into the pre-refactor wire
 * error string. Every service method still emits the same client-
 * visible frame it used to; only the code path that decides the
 * frame changes.
 *
 * WHY Disconnected (not a dedicated Unavailable code)
 *
 * `StorageError` from LLD-3.1 already distinguishes "the connection is
 * gone" from every other class of failure. "Persistence is disabled at
 * boot" is operationally the same thing — the DB is not reachable
 * from this process. Adding a separate `Unavailable` enum value would
 * force every caller `switch` to add a case that behaves identically
 * to `Disconnected`. Reused enum, single branch.
 *
 * WRITES ON A NULL STORE
 *
 * `NullGameStore::save_completed_game` and `save_cheat_report` return
 * `Disconnected` too. That is a policy DIFFERENCE from LLD-3.2:
 * previously a null `GameStore*` short-circuited persistence to a
 * silent no-op inside `GameplayService::persist_game`. Now the
 * service unconditionally calls the port, gets `Disconnected`, and
 * logs it at INFO — which is a bit noisier when persistence is off,
 * but it makes the operational mode legible in the log stream instead
 * of invisible. The old wire behaviour is preserved: no client sees
 * anything different when a completed game fails to persist because
 * persistence is off; the log line is internal.
 */

#pragma once

#include "application/ports/game_store.h"
#include "application/ports/player_queries.h"
#include "storage/storage_error.h"

namespace chess {
namespace application {
namespace ports {

class NullGameStore final : public GameStore {
public:
    bool capable() const override { return false; }

    SaveGameOutcome save_completed_game(
            const chess::storage::CompletedGame& /*game*/) override {
        SaveGameOutcome out;
        out.code  = chess::storage::StorageError::Disconnected;
        out.error = "persistence disabled";
        return out;
    }

    SaveCheatReportOutcome save_cheat_report(
            int64_t /*game_id*/,
            int64_t /*player_id*/,
            const std::string& /*side*/,
            const chess::analysis::AnalysisReport& /*report*/) override {
        SaveCheatReportOutcome out;
        out.code  = chess::storage::StorageError::Disconnected;
        out.error = "persistence disabled";
        return out;
    }
};

class NullPlayerQueries final : public PlayerQueries {
public:
    ReadOutcome<std::optional<chess::storage::PlayerProfile>>
    find_player_by_username(const std::string& /*username*/) override {
        return { false, chess::storage::StorageError::Disconnected,
                 std::nullopt, "persistence disabled" };
    }

    ReadOutcome<std::vector<chess::storage::LeaderboardEntry>>
    get_leaderboard(int /*limit*/, int /*offset*/) override {
        return { false, chess::storage::StorageError::Disconnected,
                 {}, "persistence disabled" };
    }

    ReadOutcome<std::vector<chess::storage::GameSummary>>
    get_player_games(int64_t /*player_id*/, int /*limit*/, int /*offset*/) override {
        return { false, chess::storage::StorageError::Disconnected,
                 {}, "persistence disabled" };
    }

    ReadOutcome<std::vector<chess::storage::RatingHistoryPoint>>
    get_rating_history(int64_t /*player_id*/, int /*current_elo*/) override {
        return { false, chess::storage::StorageError::Disconnected,
                 {}, "persistence disabled" };
    }

    ReadOutcome<std::optional<chess::storage::StoredGame>>
    find_game_by_id(int64_t /*game_id*/) override {
        return { false, chess::storage::StorageError::Disconnected,
                 std::nullopt, "persistence disabled" };
    }

    ReadOutcome<std::vector<int>>
    get_move_times_by_ply(int64_t /*game_id*/) override {
        return { false, chess::storage::StorageError::Disconnected,
                 {}, "persistence disabled" };
    }
};

} // namespace ports
} // namespace application
} // namespace chess
