/**
 * test_schema_phase8.cpp — Phase 8.1 schema and migration verification.
 *
 * This suite starts from the Phase-7 players/sessions schema, applies the
 * tracked Phase-8 migration through Database::apply_migration(), and proves
 * both the relational invariants and the query-plan requirement from the
 * implementation plan.
 */

#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

#include "storage/database.h"

using namespace chess::storage;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (fn()) {
        std::cout << "PASS\n";
        ++g_passed;
    } else {
        std::cout << "FAIL\n";
        ++g_failed;
    }
}

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string source_path(const std::string& relative) {
#ifdef CHESS_SOURCE_DIR
    return std::string(CHESS_SOURCE_DIR) + "/" + relative;
#else
    return relative;
#endif
}

Database open_db() {
    Database db;
    std::string error;
    if (!db.connect_from_env(error)) {
        std::cerr << "\nconnect failed: " << error << '\n';
    }
    return db;
}

bool reset_to_phase7(Database& db) {
    std::string error;
    if (!db.run_script(
            "DROP TABLE IF EXISTS move_times;"
            "DROP TABLE IF EXISTS games;"
            "DROP TABLE IF EXISTS sessions;"
            "DROP TABLE IF EXISTS schema_migrations;"
            "DROP TABLE IF EXISTS players;"
            "DROP FUNCTION IF EXISTS assert_username_ci_matches();",
            error)) {
        std::cerr << "\nreset failed: " << error << '\n';
        return false;
    }

    const std::string base = read_file(
        source_path("src/storage/schema_phase7.sql"));
    if (base.empty() || !db.run_script(base, error)) {
        std::cerr << "\nPhase-7 schema load failed: " << error << '\n';
        return false;
    }
    return true;
}

bool apply_phase8(Database& db, bool& applied) {
    const std::string migration = read_file(source_path(
        "src/storage/migrations/0001_phase8_game_persistence.sql"));
    std::string error;
    if (migration.empty() ||
        !db.apply_migration("0001_phase8_game_persistence", migration,
                            applied, error)) {
        std::cerr << "\nmigration failed: " << error << '\n';
        return false;
    }
    return true;
}

bool prepare(Database& db) {
    if (!reset_to_phase7(db)) return false;
    bool applied = false;
    return apply_phase8(db, applied) && applied;
}

bool insert_two_players(Database& db, std::string& white_id,
                        std::string& black_id) {
    auto white = db.exec(
        "INSERT INTO players(username,username_ci,password_hash,elo_rating)"
        " VALUES($1,$2,$3,$4) RETURNING id",
        {Param::text("White"), Param::text("white"), Param::text("hash"),
         Param::int64(1500)});
    auto black = db.exec(
        "INSERT INTO players(username,username_ci,password_hash,elo_rating)"
        " VALUES($1,$2,$3,$4) RETURNING id",
        {Param::text("Black"), Param::text("black"), Param::text("hash"),
         Param::int64(1450)});
    if (!white.ok || !black.ok || white.empty() || black.empty()) return false;
    white_id = white.first().at(0);
    black_id = black.first().at(0);
    return true;
}

QueryResult insert_game(Database& db, const std::string& white_id,
                        const std::string& black_id) {
    return db.exec(
        "INSERT INTO games(white_id,black_id,moves,result,termination,"
        " opening_eco,white_elo,black_elo,time_control,started_at,ended_at,move_count)"
        " VALUES($1::bigint,$2::bigint,$3,$4,$5,$6,$7::integer,$8::integer,$9,"
        " $10::timestamptz,$11::timestamptz,$12::integer) RETURNING id",
        {Param::text(white_id), Param::text(black_id),
         Param::text("e2e4 e7e5 g1f3"), Param::text("1-0"),
         Param::text("checkmate"), Param::text("C20"),
         Param::int64(1500), Param::int64(1450), Param::text("600+5"),
         Param::text("2026-09-12T10:00:00Z"),
         Param::text("2026-09-12T10:20:00Z"), Param::int64(3)});
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 8.1 - Postgres schema migration\n";
    std::cout << "========================================\n";

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping.\n";
        return 0;
    }

    Database db = open_db();
    if (!db.connected()) return 1;

    run_test("Migration creates tables and records its version", [&] {
        if (!prepare(db)) return false;
        auto objects = db.exec(
            "SELECT to_regclass('games'),to_regclass('move_times'),"
            " to_regclass('schema_migrations')");
        auto version = db.exec(
            "SELECT count(*) FROM schema_migrations WHERE version=$1",
            {Param::text("0001_phase8_game_persistence")});
        return objects.ok && objects.first().at(0) == "games" &&
               objects.first().at(1) == "move_times" &&
               objects.first().at(2) == "schema_migrations" &&
               version.ok && version.first().at(0) == "1";
    });

    run_test("Applying the same migration twice is a no-op", [&] {
        if (!prepare(db)) return false;
        bool applied = true;
        return apply_phase8(db, applied) && !applied;
    });

    run_test("Failed migration rolls back DDL and version record", [&] {
        if (!reset_to_phase7(db)) return false;
        bool applied = false;
        std::string error;
        const bool ok = db.apply_migration(
            "broken", "CREATE TABLE must_roll_back(id INTEGER);"
                      "SELECT * FROM table_that_does_not_exist;",
            applied, error);
        auto table = db.exec("SELECT to_regclass('must_roll_back') IS NULL");
        // schema_migrations was created inside the same transaction, so on a
        // pristine Phase-7 database the ledger itself must roll back too.
        auto ledger = db.exec("SELECT to_regclass('schema_migrations') IS NULL");
        return !ok && !applied && !error.empty() && table.ok &&
               table.first().at(0) == "t" && ledger.ok &&
               ledger.first().at(0) == "t";
    });

    run_test("Player statistics default to zero and stay consistent", [&] {
        if (!prepare(db)) return false;
        auto player = db.exec(
            "INSERT INTO players(username,username_ci,password_hash)"
            " VALUES($1,$2,$3)"
            " RETURNING games_played,wins,losses,draws",
            {Param::text("Stats"), Param::text("stats"), Param::text("hash")});
        if (!player.ok || player.first().at(0) != "0" ||
            player.first().at(1) != "0" || player.first().at(2) != "0" ||
            player.first().at(3) != "0") return false;
        auto invalid = db.exec(
            "UPDATE players SET games_played=$1,wins=$2 WHERE username_ci=$3",
            {Param::int64(1), Param::int64(2), Param::text("stats")});
        return !invalid.ok && invalid.sqlstate == pg_errors::CHECK_VIOLATION;
    });

    run_test("Game and per-ply think times round-trip", [&] {
        if (!prepare(db)) return false;
        std::string white_id;
        std::string black_id;
        if (!insert_two_players(db, white_id, black_id)) return false;
        auto game = insert_game(db, white_id, black_id);
        if (!game.ok || game.empty()) return false;
        const std::string game_id = game.first().at(0);
        auto move = db.exec(
            "INSERT INTO move_times(game_id,ply_number,player_id,think_time_ms)"
            " VALUES($1::bigint,$2::integer,$3::bigint,$4::integer)",
            {Param::text(game_id), Param::int64(1), Param::text(white_id),
             Param::int64(1234)});
        auto selected = db.exec(
            "SELECT g.result,g.termination,g.opening_eco,m.ply_number,m.think_time_ms"
            " FROM games g JOIN move_times m ON m.game_id=g.id WHERE g.id=$1::bigint",
            {Param::text(game_id)});
        return move.ok && selected.ok && selected.first().at(0) == "1-0" &&
               selected.first().at(1) == "checkmate" &&
               selected.first().at(2) == "C20" &&
               selected.first().at(3) == "1" &&
               selected.first().at(4) == "1234";
    });

    run_test("Game constraints reject impossible stored state", [&] {
        if (!prepare(db)) return false;
        std::string white_id;
        std::string black_id;
        if (!insert_two_players(db, white_id, black_id)) return false;
        auto same_player = insert_game(db, white_id, white_id);
        auto bad_result = db.exec(
            "INSERT INTO games(white_id,black_id,moves,result,termination,white_elo,"
            " black_elo,time_control,started_at,ended_at,move_count)"
            " VALUES($1::bigint,$2::bigint,$3,$4,$5,$6::integer,$7::integer,$8,"
            " $9::timestamptz,$10::timestamptz,$11::integer)",
            {Param::text(white_id), Param::text(black_id), Param::text(""),
             Param::text("white wins"), Param::text("checkmate"),
             Param::int64(1500), Param::int64(1450), Param::text("600+5"),
             Param::text("2026-09-12T10:00:00Z"),
             Param::text("2026-09-12T09:00:00Z"), Param::int64(0)});
        return !same_player.ok &&
               same_player.sqlstate == pg_errors::CHECK_VIOLATION &&
               !bad_result.ok && bad_result.sqlstate == pg_errors::CHECK_VIOLATION;
    });

    run_test("Deleting a game cascades its move-time rows", [&] {
        if (!prepare(db)) return false;
        std::string white_id;
        std::string black_id;
        if (!insert_two_players(db, white_id, black_id)) return false;
        auto game = insert_game(db, white_id, black_id);
        if (!game.ok) return false;
        const std::string game_id = game.first().at(0);
        db.exec(
            "INSERT INTO move_times(game_id,ply_number,player_id,think_time_ms)"
            " VALUES($1::bigint,1,$2::bigint,100)",
            {Param::text(game_id), Param::text(white_id)});
        db.exec("DELETE FROM games WHERE id=$1::bigint", {Param::text(game_id)});
        auto remaining = db.exec("SELECT count(*) FROM move_times");
        return remaining.ok && remaining.first().at(0) == "0";
    });

    run_test("All Phase-8 indexes exist", [&] {
        if (!prepare(db)) return false;
        auto indexes = db.exec(
            "SELECT indexname FROM pg_indexes"
            " WHERE schemaname=current_schema() AND indexname = ANY($1::text[])"
            " ORDER BY indexname",
            {Param::text("{idx_games_black,idx_games_opening,idx_games_white,idx_players_elo}")});
        return indexes.ok && indexes.rows.size() == 4 &&
               indexes.rows[0].at(0) == "idx_games_black" &&
               indexes.rows[1].at(0) == "idx_games_opening" &&
               indexes.rows[2].at(0) == "idx_games_white" &&
               indexes.rows[3].at(0) == "idx_players_elo";
    });

    run_test("1,000-player ELO range query uses idx_players_elo", [&] {
        if (!prepare(db)) return false;
        auto inserted = db.exec(
            "INSERT INTO players(username,username_ci,password_hash,elo_rating)"
            " SELECT $1 || n,lower($1 || n),$2,$3::integer + n"
            " FROM generate_series(1,1000) AS n",
            {Param::text("IndexedPlayer"), Param::text("hash"), Param::int64(800)});
        if (!inserted.ok || inserted.rows_affected != 1000) return false;
        if (!db.exec("ANALYZE players").ok) return false;

        auto plan = db.exec(
            "EXPLAIN (ANALYZE, COSTS OFF)"
            " SELECT id,username,elo_rating FROM players"
            " WHERE elo_rating BETWEEN $1::integer AND $2::integer"
            " ORDER BY elo_rating DESC,id ASC LIMIT 20",
            {Param::int64(1790), Param::int64(1800)});
        bool used_index = false;
        if (plan.ok) {
            for (const auto& row : plan.rows) {
                if (row.at(0).find("idx_players_elo") != std::string::npos) {
                    used_index = true;
                    break;
                }
            }
        }
        auto count = db.exec(
            "SELECT count(*) FROM players"
            " WHERE elo_rating BETWEEN $1::integer AND $2::integer",
            {Param::int64(1790), Param::int64(1800)});
        return used_index && count.ok && count.first().at(0) == "11";
    });

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed
              << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
