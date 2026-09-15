/**
 * player_repo.cpp — player profile queries and ELO updates.
 *
 * Every query goes through Database::exec() with parameterized bindings.
 * No SQL string concatenation anywhere — the to_lower_ascii() helper is
 * used only on the parameter value before binding, never spliced into SQL.
 */

#include "storage/player_repo.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace chess {
namespace storage {

namespace {

/// ASCII-only lowercase for username_ci lookup.
std::string to_lower_ascii(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

/// Parse a Row into a PlayerProfile. The query must select columns in this
/// order: id, username, elo_rating, games_played, wins, losses, draws.
PlayerProfile row_to_profile(const Row& row) {
    return PlayerProfile{
        std::stoll(row.at(0)),   // id
        row.at(1),               // username
        std::stoi(row.at(2)),    // elo_rating
        std::stoi(row.at(3)),    // games_played
        std::stoi(row.at(4)),    // wins
        std::stoi(row.at(5)),    // losses
        std::stoi(row.at(6))     // draws
    };
}

} // namespace

std::optional<PlayerProfile> find_player_by_id(Database& db, int64_t player_id) {
    auto result = db.exec(
        "SELECT id, username, elo_rating, games_played, wins, losses, draws"
        " FROM players WHERE id = $1",
        {Param::int64(player_id)});

    if (!result.ok || result.empty()) return std::nullopt;
    return row_to_profile(result.first());
}

std::optional<PlayerProfile> find_player_by_username(Database& db,
                                                     const std::string& username) {
    auto result = db.exec(
        "SELECT id, username, elo_rating, games_played, wins, losses, draws"
        " FROM players WHERE username_ci = $1",
        {Param::text(to_lower_ascii(username))});

    if (!result.ok || result.empty()) return std::nullopt;
    return row_to_profile(result.first());
}

std::vector<LeaderboardEntry> get_leaderboard(Database& db, int limit, int offset) {
    // ROW_NUMBER() is evaluated over ALL qualifying rows before LIMIT/OFFSET,
    // so ranks are globally correct even on page 2+. The window ORDER BY
    // matches idx_players_elo (elo_rating DESC, id ASC).
    auto result = db.exec(
        "SELECT id, username, elo_rating, games_played, wins, losses, draws,"
        " ROW_NUMBER() OVER (ORDER BY elo_rating DESC, id ASC) AS rank"
        " FROM players"
        " WHERE games_played > 0"
        " ORDER BY elo_rating DESC, id ASC"
        " LIMIT $1 OFFSET $2",
        {Param::int64(limit), Param::int64(offset)});

    std::vector<LeaderboardEntry> entries;
    if (!result.ok) return entries;
    entries.reserve(result.rows.size());

    for (const auto& row : result.rows) {
        LeaderboardEntry entry;
        static_cast<PlayerProfile&>(entry) = row_to_profile(row);
        entry.rank = std::stoi(row.at(7));  // rank column
        entries.push_back(std::move(entry));
    }

    return entries;
}

bool update_elo(Database& db, int64_t player_id, int new_elo) {
    auto result = db.exec(
        "UPDATE players SET elo_rating = $1 WHERE id = $2",
        {Param::int64(new_elo), Param::int64(player_id)});
    return result.ok && result.rows_affected > 0;
}

} // namespace storage
} // namespace chess
