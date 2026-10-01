/**
 * storage/postgres_player_queries.h — Postgres adapter for the read
 * side of persistence (LLD-3.3).
 *
 * Implements `application::ports::PlayerQueries` by delegating to the
 * existing free functions in `player_repo` and `game_repo`, plus one
 * inline query for the `move_times` timeline (kept inline because it's
 * the only place that reads that table).
 *
 * As with `PostgresGameStore`, the SQL functions themselves are NOT
 * rewritten in this slice — the plan doc says "wrap proven SQL
 * functions initially; avoid rewriting all queries." Adapters translate
 * shape only.
 *
 * FAILURE CLASSIFICATION
 *
 * The free-function readers return their empty/optional result without
 * distinguishing "SQL error" from "no rows" — they log the error and
 * return an empty vector. That's fine for reads but hides real DB
 * outages behind an empty payload. This adapter fixes that by running
 * a lightweight preflight (`SELECT 1`) before each read; if the
 * preflight fails the adapter reports the typed StorageError via
 * `classify()` and returns without invoking the reader. If preflight
 * succeeds the reader runs and its result is wrapped as `ok`. A future
 * slice that pushes typed failures into the readers themselves can
 * drop the preflight — this is deliberately conservative so we neither
 * lose the "distinguishable failure" invariant nor rewrite every
 * reader's error handling in this slice.
 */

#pragma once

#include "application/ports/player_queries.h"
#include "storage/database.h"

namespace chess {
namespace storage {

class PostgresPlayerQueries final
    : public chess::application::ports::PlayerQueries {
public:
    explicit PostgresPlayerQueries(Database& db);

    chess::application::ports::ReadOutcome<std::optional<PlayerProfile>>
        find_player_by_username(const std::string& username) override;

    chess::application::ports::ReadOutcome<std::vector<LeaderboardEntry>>
        get_leaderboard(int limit, int offset) override;

    chess::application::ports::ReadOutcome<std::vector<GameSummary>>
        get_player_games(int64_t player_id, int limit, int offset) override;

    chess::application::ports::ReadOutcome<std::vector<RatingHistoryPoint>>
        get_rating_history(int64_t player_id, int current_elo) override;

    chess::application::ports::ReadOutcome<std::optional<StoredGame>>
        find_game_by_id(int64_t game_id) override;

    chess::application::ports::ReadOutcome<std::vector<int>>
        get_move_times_by_ply(int64_t game_id) override;

private:
    Database& db_;
};

} // namespace storage
} // namespace chess
