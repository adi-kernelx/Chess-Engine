/**
 * application/ports/player_queries.h — the read side of persistence (LLD-3.3).
 *
 * The counterpart to `GameStore` (LLD-3.2). Everything the application
 * layer needs to READ from persistence lives behind this port:
 *
 *   - Player profiles + leaderboards (browse endpoints).
 *   - A game's full stored record (replay).
 *   - A game's per-ply think-time series (replay + analyze_game).
 *   - A player's recent games (history endpoint + profile card).
 *
 * WHY A PORT
 *
 * `GameQueryService` and `AnalysisService` were reaching into
 * `Database*` directly for reads. That coupled every application-level
 * test to libpq and forced the "no database configured" branch to be
 * a null-pointer check on the raw `Database*` — the plan doc calls that
 * out explicitly: "Required persistence is not represented by an
 * optional pointer that silently does nothing; capability-disabled mode
 * is explicit." This port lets us give services a real non-null
 * reference in every mode: `PostgresPlayerQueries` in production,
 * `NullPlayerQueries` when persistence is disabled. The latter returns
 * `ReadOutcome{ok:false, code:Disconnected}` for every call, which the
 * service translates into the pre-refactor "unavailable" wire error
 * that clients already know how to render.
 *
 * FAILURE VS EMPTINESS
 *
 * `ReadOutcome<T>` carries a `bool ok` + typed `StorageError code` + a
 * `T value`. Read failures (`Disconnected`, `Timeout`, etc.) are
 * distinguishable from a successful read that returned zero rows: the
 * plan requires this. A "no matching row" is `ok == true` with an
 * `optional<T>::nullopt` or an empty vector; only a real storage-layer
 * problem is `ok == false`.
 *
 * NOT COVERED HERE
 *
 * Tournament reads (find_tournament / list_tournaments / get_state)
 * stay on Database for now — the plan names two ports (game_store,
 * player_queries) and tournaments will get their own port in a later
 * slice when the tournament family gets the same treatment. Auth
 * queries (find_player_by_id used by the token layer) also stay
 * direct — auth is out of scope for LLD-3 per the plan doc.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/game_repo.h"      // StoredGame, GameSummary
#include "storage/player_repo.h"    // PlayerProfile, LeaderboardEntry
#include "storage/storage_error.h"

namespace chess {
namespace application {
namespace ports {

/// One read's outcome: success + value, or a typed failure. Empty rows
/// are `{ok:true, value:{}}` — never `ok:false`. `code == Ok` iff
/// `ok == true`. `error` is the adapter's raw message for logs.
template <typename T>
struct ReadOutcome {
    bool                         ok    = false;
    chess::storage::StorageError code  = chess::storage::StorageError::Disconnected;
    T                            value{};
    std::string                  error;
};

class PlayerQueries {
public:
    virtual ~PlayerQueries() = default;

    /// Case-insensitive username lookup. `value.has_value()` is false
    /// when no matching row exists (that's a successful read). `ok` is
    /// false only for storage-level failures.
    virtual ReadOutcome<std::optional<chess::storage::PlayerProfile>>
        find_player_by_username(const std::string& username) = 0;

    /// Top players by ELO. `limit` is clamped to a sane bound by the
    /// caller; the port trusts what it is handed. An empty leaderboard
    /// (no players with games played) is `{ok:true, value:{}}`.
    virtual ReadOutcome<std::vector<chess::storage::LeaderboardEntry>>
        get_leaderboard(int limit, int offset) = 0;

    /// One player's recent games. Empty history is `{ok:true, value:{}}`.
    virtual ReadOutcome<std::vector<chess::storage::GameSummary>>
        get_player_games(int64_t player_id, int limit, int offset) = 0;

    /// Durable rating progression reconstructed from rated game snapshots.
    virtual ReadOutcome<std::vector<chess::storage::RatingHistoryPoint>>
        get_rating_history(int64_t player_id, int current_elo) = 0;

    /// One game's stored record. Unknown id is `{ok:true, value:nullopt}`.
    virtual ReadOutcome<std::optional<chess::storage::StoredGame>>
        find_game_by_id(int64_t game_id) = 0;

    /// Per-ply think times for one game, keyed by ply_number. Returned
    /// as a `vector<int>` indexed by ply (position 0 unused; ply
    /// numbers are 1-based, matching the current
    /// `analyze_game`/`get_game` reconstruction). Zero-filled for any
    /// missing rows (analyzer needs a dense array). Empty when the
    /// game has no move_times rows at all.
    virtual ReadOutcome<std::vector<int>>
        get_move_times_by_ply(int64_t game_id) = 0;
};

} // namespace ports
} // namespace application
} // namespace chess
