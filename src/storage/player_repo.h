/**
 * player_repo.h — read-only player queries and ELO updates.
 *
 * This module handles game-related reads (profiles, leaderboards) and
 * rating updates. It does NOT handle account creation or credential
 * verification — that belongs to auth/service.h. The separation is
 * intentional: auth creates and authenticates, this module reads and
 * updates game data.
 *
 * All functions are free functions in the chess::storage namespace,
 * matching the convention established by auth/service.h. No stateful
 * class is needed — the Database reference is passed per call.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/database.h"

namespace chess {
namespace storage {

/// Game-related player data for profile display and leaderboards.
/// Deliberately omits password_hash, token_epoch, google_sub — those
/// belong to the auth layer and should never leak into game code.
struct PlayerProfile {
    int64_t     player_id;
    std::string username;
    int         elo_rating;
    int         games_played;
    int         wins;
    int         losses;
    int         draws;
};

/// A leaderboard row: PlayerProfile plus a 1-based rank computed via
/// ROW_NUMBER() in the query, so pagination gives globally correct ranks.
struct LeaderboardEntry : PlayerProfile {
    int rank;
};

/// Find a player by their database ID. Returns nullopt if no player exists
/// with that ID.
std::optional<PlayerProfile> find_player_by_id(Database& db, int64_t player_id);

/// Find a player by username (case-insensitive lookup via username_ci).
/// Returns nullopt if no matching player exists.
std::optional<PlayerProfile> find_player_by_username(Database& db,
                                                     const std::string& username);

/// Return the top players sorted by ELO descending. Players with zero
/// games played are excluded (they'd clutter the board with default-1200
/// accounts that never played). Ranks are globally correct even with
/// offset-based pagination.
std::vector<LeaderboardEntry> get_leaderboard(Database& db,
                                              int limit = 50,
                                              int offset = 0);

/// Update a single player's ELO rating. Returns true if the player existed
/// and was updated, false otherwise. This is a standalone utility; the
/// normal path is save_completed_game() in game_repo.h, which updates ELO
/// atomically alongside game insertion and stat counters.
bool update_elo(Database& db, int64_t player_id, int new_elo);

} // namespace storage
} // namespace chess
