/**
 * game_repo.h — game persistence and history queries.
 *
 * The centrepiece is save_completed_game(), which runs a single atomic
 * transaction that inserts the game record, records per-ply think times,
 * calculates new ELO ratings, and updates both players' stats. Either
 * everything commits or nothing does.
 *
 * All data enters through the CompletedGame struct, which is a plain
 * value type built by the caller (Phase 8.3's handler integration) from
 * GameRoom data. This module knows nothing about GameRoom, Connection,
 * or WebSocket — it is a pure storage layer.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/database.h"
#include "storage/elo.h"

namespace chess {
namespace storage {

/// All data needed to persist a completed game. Built by the game handler
/// from GameRoom's public API (move history, time control, player slots).
struct CompletedGame {
    int64_t     white_id;
    int64_t     black_id;
    std::string moves;          ///< UCI notation: "e2e4 e7e5 g1f3 ..."
    std::string result;         ///< "1-0", "0-1", or "1/2-1/2"
    std::string termination;    ///< Wire value from status_to_reason()
    int         white_elo;      ///< Rating snapshot at game start
    int         black_elo;      ///< Rating snapshot at game start
    std::string time_control;   ///< "600+5" format
    std::string started_at;     ///< ISO 8601 timestamp
    std::string ended_at;       ///< ISO 8601 timestamp
    int         move_count;     ///< Total plies

    /// Per-ply think time for anti-cheat and analysis.
    struct PlyTime {
        int     ply_number;     ///< 1-based (ply, not chess move number)
        int64_t player_id;      ///< Who moved
        int     think_time_ms;  ///< Milliseconds spent
    };
    std::vector<PlyTime> think_times;
};

/// Outcome of save_completed_game(). On success, ok==true and game_id
/// holds the newly assigned BIGSERIAL. elo contains the calculated
/// rating changes for both players.
struct SaveGameResult {
    bool        ok = false;
    int64_t     game_id = 0;
    EloUpdate   elo{};
    std::string error;
};

/// A full game record retrieved from the database, including player names
/// resolved via JOIN.
struct StoredGame {
    int64_t     game_id;
    int64_t     white_id;
    int64_t     black_id;
    std::string white_name;
    std::string black_name;
    std::string moves;
    std::string result;
    std::string termination;
    int         white_elo;
    int         black_elo;
    std::string time_control;
    std::string started_at;
    std::string ended_at;
    int         move_count;
};

/// Compact game summary for "recent games" in a player's profile view.
/// Fields are oriented from the queried player's perspective.
struct GameSummary {
    int64_t     game_id;
    std::string opponent_name;
    int         opponent_elo;
    std::string player_result;  ///< "w", "l", or "d" from queried player's perspective
    std::string color;          ///< "w" or "b" — which side the queried player was on
    std::string termination;
    std::string started_at;
    int         move_count;
    std::string time_control;
};

/// Persist a completed game in a single atomic transaction:
///   1. Calculate new ELO ratings
///   2. INSERT INTO games RETURNING id
///   3. INSERT INTO move_times (per-ply think times)
///   4. UPDATE white player's elo_rating and win/loss/draw/games_played
///   5. UPDATE black player's elo_rating and win/loss/draw/games_played
///
/// On any failure, the entire transaction is rolled back. The ELO
/// calculation uses the pre-game ratings from CompletedGame, not a
/// live re-read from the DB (the pre-game snapshot is the correct input
/// for the formula).
///
/// @param k_factor  ELO sensitivity constant (default 32).
SaveGameResult save_completed_game(Database& db, const CompletedGame& game,
                                   int k_factor = 32);

/// Retrieve a full game record by its database ID. Returns nullopt if
/// no game exists with that ID. Player names are resolved via JOIN.
std::optional<StoredGame> find_game_by_id(Database& db, int64_t game_id);

/// Get a player's recent games, sorted by date descending. Uses UNION ALL
/// over idx_games_white and idx_games_black so both indexes are hit.
/// The player_result field is mapped from the game result and color in C++.
std::vector<GameSummary> get_player_games(Database& db, int64_t player_id,
                                          int limit = 20, int offset = 0);

} // namespace storage
} // namespace chess
