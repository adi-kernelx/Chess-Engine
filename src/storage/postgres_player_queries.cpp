#include "storage/postgres_player_queries.h"

#include <cstddef>

#include "storage/game_repo.h"
#include "storage/player_repo.h"

namespace chess {
namespace storage {

namespace app_ports = chess::application::ports;

namespace {

// Wrap an existing free-function reader that returns T. On preflight
// failure (i.e. the connection is not healthy or the trivial `SELECT 1`
// fails), report the typed StorageError without invoking the reader.
// Otherwise the reader runs and its result becomes the outcome value.
template <typename T, typename F>
app_ports::ReadOutcome<T> guarded(Database& db, F&& reader) {
    app_ports::ReadOutcome<T> out;
    auto ping = db.exec("SELECT 1");
    if (!ping.ok) {
        out.ok    = false;
        out.code  = classify(ping);
        out.error = ping.error;
        return out;
    }
    out.value = reader();
    out.ok    = true;
    out.code  = StorageError::Ok;
    return out;
}

} // namespace

PostgresPlayerQueries::PostgresPlayerQueries(Database& db) : db_(db) {}

app_ports::ReadOutcome<std::optional<PlayerProfile>>
PostgresPlayerQueries::find_player_by_username(const std::string& username) {
    return guarded<std::optional<PlayerProfile>>(db_,
        [&] { return chess::storage::find_player_by_username(db_, username); });
}

app_ports::ReadOutcome<std::vector<LeaderboardEntry>>
PostgresPlayerQueries::get_leaderboard(int limit, int offset) {
    return guarded<std::vector<LeaderboardEntry>>(db_,
        [&] { return chess::storage::get_leaderboard(db_, limit, offset); });
}

app_ports::ReadOutcome<std::vector<GameSummary>>
PostgresPlayerQueries::get_player_games(int64_t player_id, int limit, int offset) {
    return guarded<std::vector<GameSummary>>(db_,
        [&] { return chess::storage::get_player_games(db_, player_id, limit, offset); });
}

app_ports::ReadOutcome<std::vector<RatingHistoryPoint>>
PostgresPlayerQueries::get_rating_history(int64_t player_id, int current_elo) {
    return guarded<std::vector<RatingHistoryPoint>>(db_,
        [&] { return chess::storage::get_rating_history(db_, player_id, current_elo); });
}

app_ports::ReadOutcome<std::optional<StoredGame>>
PostgresPlayerQueries::find_game_by_id(int64_t game_id) {
    return guarded<std::optional<StoredGame>>(db_,
        [&] { return chess::storage::find_game_by_id(db_, game_id); });
}

app_ports::ReadOutcome<std::vector<int>>
PostgresPlayerQueries::get_move_times_by_ply(int64_t game_id) {
    // Direct query (no free-function reader) — the move_times table is
    // used only here and by the anti-cheat pipeline. Same
    // preflight-then-exec shape as the guarded helper, but the reader
    // is inline so we can consult the QueryResult's ok flag directly
    // without a second SELECT 1.
    app_ports::ReadOutcome<std::vector<int>> out;
    auto r = db_.exec(
        "SELECT ply_number, think_time_ms FROM move_times"
        " WHERE game_id = $1 ORDER BY ply_number",
        {Param::int64(game_id)});
    if (!r.ok) {
        out.ok    = false;
        out.code  = classify(r);
        out.error = r.error;
        return out;
    }
    // Same zero-filled dense layout the pre-refactor code built inline.
    // Position 0 unused; ply numbers are 1-based.
    std::vector<int> think_by_ply(r.rows.size() + 1, 0);
    for (const auto& row : r.rows) {
        int p = std::stoi(row.at(0));
        int t = std::stoi(row.at(1));
        if (p >= 0 && static_cast<size_t>(p) < think_by_ply.size()) {
            think_by_ply[p] = t;
        }
    }
    out.value = std::move(think_by_ply);
    out.ok    = true;
    out.code  = StorageError::Ok;
    return out;
}

} // namespace storage
} // namespace chess
