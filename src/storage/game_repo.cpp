/**
 * game_repo.cpp — game persistence with atomic ELO transactions.
 *
 * The save_completed_game() function is the single most important piece
 * in this file. It runs a BEGIN/COMMIT transaction that inserts the game,
 * records think times, and updates both players' stats and ELO ratings.
 * Any failure triggers ROLLBACK — no partial state.
 *
 * The player UPDATE queries use relative increments (wins = wins + 1),
 * not absolute values. This means concurrent transactions for different
 * games touching the same player don't clobber each other — Postgres
 * serializes the row-level UPDATEs internally.
 *
 * No SQL string concatenation: the three stat-update variants (win/loss/
 * draw) are separate constant query strings selected by a switch, not
 * built by concatenating column names into SQL.
 */

#include "storage/game_repo.h"

#include <string>

namespace chess {
namespace storage {

namespace {

// ── Player stat UPDATE queries ──────────────────────────────────────────
//
// Three fixed SQL strings, one per outcome. The result determines which
// one fires for each player. No dynamic SQL construction.

static const char* SQL_UPDATE_WINNER =
    "UPDATE players SET elo_rating = $1,"
    " wins = wins + 1, games_played = games_played + 1"
    " WHERE id = $2";

static const char* SQL_UPDATE_LOSER =
    "UPDATE players SET elo_rating = $1,"
    " losses = losses + 1, games_played = games_played + 1"
    " WHERE id = $2";

static const char* SQL_UPDATE_DRAWER =
    "UPDATE players SET elo_rating = $1,"
    " draws = draws + 1, games_played = games_played + 1"
    " WHERE id = $2";

/// Map game result + player color to the player-perspective result.
///   "1-0" + "w" → "w" (white won, queried player was white)
///   "1-0" + "b" → "l" (white won, queried player was black)
///   "0-1" + "w" → "l"   etc.
///   "1/2-1/2"   → "d" regardless of color
std::string to_player_result(const std::string& result,
                             const std::string& color) {
    if (result == "1/2-1/2") return "d";
    if ((result == "1-0" && color == "w") ||
        (result == "0-1" && color == "b")) {
        return "w";
    }
    return "l";
}

} // namespace


SaveGameResult save_completed_game(Database& db, const CompletedGame& game,
                                   int k_factor) {
    SaveGameResult out;

    // ── Pre-calculate ELO before touching the database ──────────────
    out.elo = calculate_elo(game.white_elo, game.black_elo,
                            game.result, k_factor);

    // ── BEGIN transaction ────────────────────────────────────────────
    auto begin_result = db.exec("BEGIN");
    if (!begin_result.ok) {
        out.error = "Failed to begin transaction: " + begin_result.error;
        return out;
    }

    // ── 1. INSERT the game record ───────────────────────────────────
    auto game_insert = db.exec(
        "INSERT INTO games(white_id, black_id, moves, result, termination,"
        " opening_eco, white_elo, black_elo, time_control,"
        " started_at, ended_at, move_count)"
        " VALUES($1::bigint, $2::bigint, $3, $4, $5, $6,"
        " $7::integer, $8::integer, $9,"
        " $10::timestamptz, $11::timestamptz, $12::integer)"
        " RETURNING id",
        {Param::int64(game.white_id), Param::int64(game.black_id),
         Param::text(game.moves), Param::text(game.result),
         Param::text(game.termination),
         Param::null(),  // opening_eco — Phase 9 populates this
         Param::int64(game.white_elo), Param::int64(game.black_elo),
         Param::text(game.time_control),
         Param::text(game.started_at), Param::text(game.ended_at),
         Param::int64(game.move_count)});

    if (!game_insert.ok || game_insert.empty()) {
        out.error = "Failed to insert game: " + game_insert.error;
        db.exec("ROLLBACK");
        return out;
    }

    out.game_id = std::stoll(game_insert.first().at(0));

    // ── 2. INSERT per-ply think times ───────────────────────────────
    for (const auto& ply : game.think_times) {
        auto mt = db.exec(
            "INSERT INTO move_times(game_id, ply_number, player_id, think_time_ms)"
            " VALUES($1::bigint, $2::integer, $3::bigint, $4::integer)",
            {Param::int64(out.game_id), Param::int64(ply.ply_number),
             Param::int64(ply.player_id), Param::int64(ply.think_time_ms)});

        if (!mt.ok) {
            out.error = "Failed to insert move time (ply "
                        + std::to_string(ply.ply_number) + "): " + mt.error;
            db.exec("ROLLBACK");
            return out;
        }
    }

    // ── 3. Select the correct UPDATE queries per result ──────────────
    const char* white_sql = nullptr;
    const char* black_sql = nullptr;

    if (game.result == "1-0") {
        white_sql = SQL_UPDATE_WINNER;
        black_sql = SQL_UPDATE_LOSER;
    } else if (game.result == "0-1") {
        white_sql = SQL_UPDATE_LOSER;
        black_sql = SQL_UPDATE_WINNER;
    } else if (game.result == "1/2-1/2") {
        white_sql = SQL_UPDATE_DRAWER;
        black_sql = SQL_UPDATE_DRAWER;
    } else {
        out.error = "Unknown result: " + game.result;
        db.exec("ROLLBACK");
        return out;
    }

    // ── 4. UPDATE white player ──────────────────────────────────────
    auto white_update = db.exec(
        white_sql,
        {Param::int64(out.elo.white_new), Param::int64(game.white_id)});

    if (!white_update.ok || white_update.rows_affected == 0) {
        out.error = "Failed to update white player (id="
                    + std::to_string(game.white_id) + "): " + white_update.error;
        db.exec("ROLLBACK");
        return out;
    }

    // ── 5. UPDATE black player ──────────────────────────────────────
    auto black_update = db.exec(
        black_sql,
        {Param::int64(out.elo.black_new), Param::int64(game.black_id)});

    if (!black_update.ok || black_update.rows_affected == 0) {
        out.error = "Failed to update black player (id="
                    + std::to_string(game.black_id) + "): " + black_update.error;
        db.exec("ROLLBACK");
        return out;
    }

    // ── COMMIT ──────────────────────────────────────────────────────
    auto commit_result = db.exec("COMMIT");
    if (!commit_result.ok) {
        out.error = "Failed to commit: " + commit_result.error;
        db.exec("ROLLBACK");
        return out;
    }

    out.ok = true;
    return out;
}


std::optional<StoredGame> find_game_by_id(Database& db, int64_t game_id) {
    auto result = db.exec(
        "SELECT g.id, g.white_id, g.black_id,"
        " pw.username AS white_name, pb.username AS black_name,"
        " g.moves, g.result, g.termination,"
        " g.white_elo, g.black_elo, g.time_control,"
        " g.started_at::text, g.ended_at::text, g.move_count"
        " FROM games g"
        " JOIN players pw ON pw.id = g.white_id"
        " JOIN players pb ON pb.id = g.black_id"
        " WHERE g.id = $1",
        {Param::int64(game_id)});

    if (!result.ok || result.empty()) return std::nullopt;

    const auto& row = result.first();
    return StoredGame{
        std::stoll(row.at(0)),    // game_id
        std::stoll(row.at(1)),    // white_id
        std::stoll(row.at(2)),    // black_id
        row.at(3),                // white_name
        row.at(4),                // black_name
        row.at(5),                // moves
        row.at(6),                // result
        row.at(7),                // termination
        std::stoi(row.at(8)),     // white_elo
        std::stoi(row.at(9)),     // black_elo
        row.at(10),               // time_control
        row.at(11),               // started_at
        row.at(12),               // ended_at
        std::stoi(row.at(13))     // move_count
    };
}


std::vector<GameSummary> get_player_games(Database& db, int64_t player_id,
                                          int limit, int offset) {
    // UNION ALL ensures each branch uses its own index:
    //   - First branch: idx_games_white (white_id, started_at DESC)
    //   - Second branch: idx_games_black (black_id, started_at DESC)
    // An OR condition would risk a sequential scan or bitmap merge.
    auto result = db.exec(
        "(SELECT g.id, p.username AS opponent_name,"
        "  g.black_elo AS opponent_elo,"
        "  g.result, g.termination, g.started_at::text,"
        "  g.move_count, g.time_control, 'w' AS color"
        " FROM games g"
        " JOIN players p ON p.id = g.black_id"
        " WHERE g.white_id = $1)"
        " UNION ALL"
        " (SELECT g.id, p.username AS opponent_name,"
        "  g.white_elo AS opponent_elo,"
        "  g.result, g.termination, g.started_at::text,"
        "  g.move_count, g.time_control, 'b' AS color"
        " FROM games g"
        " JOIN players p ON p.id = g.white_id"
        " WHERE g.black_id = $1)"
        " ORDER BY started_at DESC"
        " LIMIT $2 OFFSET $3",
        {Param::int64(player_id), Param::int64(limit), Param::int64(offset)});

    std::vector<GameSummary> games;
    if (!result.ok) return games;
    games.reserve(result.rows.size());

    for (const auto& row : result.rows) {
        GameSummary g;
        g.game_id       = std::stoll(row.at(0));
        g.opponent_name = row.at(1);
        g.opponent_elo  = std::stoi(row.at(2));
        g.color         = row.at(8);
        g.player_result = to_player_result(row.at(3), g.color);
        g.termination   = row.at(4);
        g.started_at    = row.at(5);
        g.move_count    = std::stoi(row.at(6));
        g.time_control  = row.at(7);
        games.push_back(std::move(g));
    }

    return games;
}

} // namespace storage
} // namespace chess
