/**
 * test_repositories.cpp — database-backed tests for PlayerRepository and
 * GameRepository.
 *
 * Requires a running Postgres instance. Skips gracefully if DATABASE_URL
 * is not set (returns 0 so CI without Postgres still passes). Each test
 * resets the database to a clean Phase 7 + Phase 8 schema before running.
 *
 * Tests cover:
 *   - PlayerRepository: find, leaderboard, ELO update
 *   - GameRepository: atomic save_completed_game, find, player history
 *   - Integrity: 500 sequential games, concurrent writer safety, index usage
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "storage/database.h"
#include "storage/elo.h"
#include "storage/game_repo.h"
#include "storage/player_repo.h"

using namespace chess::storage;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) {
            std::cout << "PASS\n";
            ++g_passed;
        } else {
            std::cout << "FAIL\n";
            ++g_failed;
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL (exception: " << e.what() << ")\n";
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

/// Reset to clean Phase 7 schema, then apply Phase 8 migration.
bool prepare(Database& db) {
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

    const std::string migration = read_file(source_path(
        "src/storage/migrations/0001_phase8_game_persistence.sql"));
    bool applied = false;
    if (migration.empty() ||
        !db.apply_migration("0001_phase8_game_persistence", migration,
                            applied, error)) {
        std::cerr << "\nmigration failed: " << error << '\n';
        return false;
    }
    return true;
}

/// Insert a player with the given username and elo. Returns the player_id.
int64_t insert_player(Database& db, const std::string& name, int elo = 1200) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto result = db.exec(
        "INSERT INTO players(username, username_ci, password_hash, elo_rating)"
        " VALUES($1, $2, $3, $4) RETURNING id",
        {Param::text(name), Param::text(lower), Param::text("hash"),
         Param::int64(elo)});

    if (!result.ok || result.empty()) return -1;
    return std::stoll(result.first().at(0));
}

/// Build a minimal CompletedGame for testing.
CompletedGame make_game(int64_t white_id, int64_t black_id,
                        int white_elo, int black_elo,
                        const std::string& result = "1-0",
                        const std::string& termination = "checkmate") {
    CompletedGame g;
    g.white_id    = white_id;
    g.black_id    = black_id;
    g.moves       = "e2e4 e7e5 d1h5 a7a6 f1c4 a6a5 h5f7";
    g.result      = result;
    g.termination = termination;
    g.white_elo   = white_elo;
    g.black_elo   = black_elo;
    g.time_control = "600+5";
    g.started_at  = "2026-09-12T10:00:00Z";
    g.ended_at    = "2026-09-12T10:20:00Z";
    g.move_count  = 7;
    g.think_times = {
        {1, white_id, 500},
        {2, black_id, 800},
        {3, white_id, 300},
        {4, black_id, 1200},
        {5, white_id, 400},
        {6, black_id, 600},
        {7, white_id, 200}
    };
    return g;
}

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 8.2 - Repository Layer Tests\n";
    std::cout << "========================================\n";

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping.\n";
        return 0;
    }

    Database db = open_db();
    if (!db.connected()) return 1;

    // ════════════════════════════════════════════════════════════════
    //  PlayerRepository Tests
    // ════════════════════════════════════════════════════════════════

    std::cout << "\n=== PlayerRepository ===\n";

    run_test("find_player_by_id returns correct profile", [&] {
        if (!prepare(db)) return false;
        int64_t id = insert_player(db, "Alice", 1500);
        if (id < 0) return false;
        auto p = find_player_by_id(db, id);
        return p.has_value() &&
               p->player_id == id &&
               p->username == "Alice" &&
               p->elo_rating == 1500 &&
               p->games_played == 0 && p->wins == 0 &&
               p->losses == 0 && p->draws == 0;
    });

    run_test("find_player_by_id unknown returns nullopt", [&] {
        if (!prepare(db)) return false;
        return !find_player_by_id(db, 999999).has_value();
    });

    run_test("find_player_by_username case-insensitive", [&] {
        if (!prepare(db)) return false;
        int64_t id = insert_player(db, "CamelCase", 1300);
        if (id < 0) return false;
        auto p = find_player_by_username(db, "camelcase");
        return p.has_value() && p->player_id == id &&
               p->username == "CamelCase";
    });

    run_test("find_player_by_username unknown returns nullopt", [&] {
        if (!prepare(db)) return false;
        return !find_player_by_username(db, "nonexistent").has_value();
    });

    run_test("get_leaderboard sorted by ELO DESC", [&] {
        if (!prepare(db)) return false;
        int64_t a = insert_player(db, "Low", 1000);
        int64_t b = insert_player(db, "Mid", 1400);
        int64_t c = insert_player(db, "High", 1800);
        if (a < 0 || b < 0 || c < 0) return false;
        // Give each player at least 1 game so they appear on leaderboard
        for (int64_t id : {a, b, c}) {
            db.exec("UPDATE players SET games_played=1, wins=1 WHERE id=$1",
                    {Param::int64(id)});
        }
        auto lb = get_leaderboard(db, 50, 0);
        return lb.size() == 3 &&
               lb[0].username == "High" && lb[0].rank == 1 &&
               lb[1].username == "Mid"  && lb[1].rank == 2 &&
               lb[2].username == "Low"  && lb[2].rank == 3;
    });

    run_test("get_leaderboard respects limit and offset", [&] {
        if (!prepare(db)) return false;
        for (int i = 1; i <= 5; ++i) {
            int64_t id = insert_player(db, "P" + std::to_string(i), 1000 + i * 100);
            if (id < 0) return false;
            db.exec("UPDATE players SET games_played=1, wins=1 WHERE id=$1",
                    {Param::int64(id)});
        }
        auto page1 = get_leaderboard(db, 2, 0);
        auto page2 = get_leaderboard(db, 2, 2);
        return page1.size() == 2 && page2.size() == 2 &&
               page1[0].rank == 1 && page1[1].rank == 2 &&
               page2[0].rank == 3 && page2[1].rank == 4;
    });

    run_test("get_leaderboard excludes players with 0 games", [&] {
        if (!prepare(db)) return false;
        insert_player(db, "Active", 1500);
        insert_player(db, "Inactive", 1600);
        // Only Active gets a game
        auto active = find_player_by_username(db, "active");
        if (!active) return false;
        db.exec("UPDATE players SET games_played=1, wins=1 WHERE id=$1",
                {Param::int64(active->player_id)});
        auto lb = get_leaderboard(db);
        return lb.size() == 1 && lb[0].username == "Active";
    });

    run_test("update_elo changes rating", [&] {
        if (!prepare(db)) return false;
        int64_t id = insert_player(db, "EloTest", 1200);
        if (id < 0) return false;
        bool ok = update_elo(db, id, 1350);
        auto p = find_player_by_id(db, id);
        return ok && p.has_value() && p->elo_rating == 1350;
    });

    run_test("update_elo unknown player returns false", [&] {
        if (!prepare(db)) return false;
        return !update_elo(db, 999999, 1500);
    });

    run_test("Leaderboard rank numbering is globally stable", [&] {
        if (!prepare(db)) return false;
        // Create 10 players with equal ELO — rank should be deterministic by id
        for (int i = 1; i <= 10; ++i) {
            int64_t id = insert_player(db, "Eq" + std::to_string(i), 1200);
            if (id < 0) return false;
            db.exec("UPDATE players SET games_played=1, wins=1 WHERE id=$1",
                    {Param::int64(id)});
        }
        auto lb = get_leaderboard(db, 10, 0);
        if (lb.size() != 10) return false;
        // Ranks should be 1..10 with no duplicates
        for (int i = 0; i < 10; ++i) {
            if (lb[static_cast<size_t>(i)].rank != i + 1) return false;
        }
        return true;
    });

    // ════════════════════════════════════════════════════════════════
    //  GameRepository Tests
    // ════════════════════════════════════════════════════════════════

    std::cout << "\n=== GameRepository ===\n";

    run_test("save_completed_game white wins: stats and ELO correct", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "Winner", 1200);
        int64_t l = insert_player(db, "Loser", 1200);
        if (w < 0 || l < 0) return false;

        auto result = save_completed_game(db, make_game(w, l, 1200, 1200, "1-0"));
        if (!result.ok) { std::cerr << result.error << '\n'; return false; }

        auto wp = find_player_by_id(db, w);
        auto lp = find_player_by_id(db, l);
        return wp && lp &&
               wp->wins == 1 && wp->losses == 0 && wp->games_played == 1 &&
               lp->wins == 0 && lp->losses == 1 && lp->games_played == 1 &&
               wp->elo_rating == result.elo.white_new &&
               lp->elo_rating == result.elo.black_new &&
               result.game_id > 0;
    });

    run_test("save_completed_game black wins: symmetric", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "WhitePlayer", 1200);
        int64_t b = insert_player(db, "BlackPlayer", 1200);
        if (w < 0 || b < 0) return false;

        auto result = save_completed_game(db, make_game(w, b, 1200, 1200, "0-1", "resignation"));
        if (!result.ok) return false;

        auto wp = find_player_by_id(db, w);
        auto bp = find_player_by_id(db, b);
        return wp && bp &&
               wp->losses == 1 && wp->wins == 0 && wp->games_played == 1 &&
               bp->wins == 1 && bp->losses == 0 && bp->games_played == 1;
    });

    run_test("save_completed_game draw: both +1D/+1GP", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "Draw1", 1200);
        int64_t b = insert_player(db, "Draw2", 1200);
        if (w < 0 || b < 0) return false;

        auto result = save_completed_game(db,
            make_game(w, b, 1200, 1200, "1/2-1/2", "stalemate"));
        if (!result.ok) return false;

        auto wp = find_player_by_id(db, w);
        auto bp = find_player_by_id(db, b);
        return wp && bp &&
               wp->draws == 1 && wp->games_played == 1 && wp->wins == 0 &&
               bp->draws == 1 && bp->games_played == 1 && bp->wins == 0 &&
               result.elo.white_delta == 0 && result.elo.black_delta == 0;
    });

    run_test("save_completed_game persists think times", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "TimerW", 1200);
        int64_t b = insert_player(db, "TimerB", 1200);
        if (w < 0 || b < 0) return false;

        auto result = save_completed_game(db, make_game(w, b, 1200, 1200));
        if (!result.ok) return false;

        auto times = db.exec(
            "SELECT ply_number, player_id, think_time_ms FROM move_times"
            " WHERE game_id = $1 ORDER BY ply_number",
            {Param::int64(result.game_id)});

        return times.ok && times.rows.size() == 7 &&
               times.rows[0].at(0) == "1" &&
               times.rows[0].at(1) == std::to_string(w) &&
               times.rows[0].at(2) == "500" &&
               times.rows[1].at(0) == "2" &&
               times.rows[1].at(1) == std::to_string(b) &&
               times.rows[1].at(2) == "800";
    });

    run_test("save_completed_game invalid result rolls back", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "RollW", 1200);
        int64_t b = insert_player(db, "RollB", 1200);
        if (w < 0 || b < 0) return false;

        auto game = make_game(w, b, 1200, 1200, "white-wins", "checkmate");
        auto result = save_completed_game(db, game);

        // Should fail — no partial state
        auto wp = find_player_by_id(db, w);
        auto games = db.exec("SELECT count(*) FROM games");
        return !result.ok &&
               wp && wp->games_played == 0 &&
               games.ok && games.first().at(0) == "0";
    });

    run_test("save_completed_game same player both sides rejected", [&] {
        if (!prepare(db)) return false;
        int64_t p = insert_player(db, "SelfPlay", 1200);
        if (p < 0) return false;

        auto result = save_completed_game(db, make_game(p, p, 1200, 1200));
        return !result.ok;
    });

    run_test("find_game_by_id returns full record with names", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "FindW", 1500);
        int64_t b = insert_player(db, "FindB", 1400);
        if (w < 0 || b < 0) return false;

        auto saved = save_completed_game(db, make_game(w, b, 1500, 1400));
        if (!saved.ok) return false;

        auto game = find_game_by_id(db, saved.game_id);
        return game.has_value() &&
               game->game_id == saved.game_id &&
               game->white_name == "FindW" &&
               game->black_name == "FindB" &&
               game->result == "1-0" &&
               game->termination == "checkmate" &&
               game->white_elo == 1500 &&
               game->black_elo == 1400 &&
               game->move_count == 7;
    });

    run_test("find_game_by_id unknown returns nullopt", [&] {
        if (!prepare(db)) return false;
        return !find_game_by_id(db, 999999).has_value();
    });

    run_test("get_player_games returns sorted by date DESC", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "HistW", 1200);
        int64_t b = insert_player(db, "HistB", 1200);
        if (w < 0 || b < 0) return false;

        // Insert 3 games with different timestamps
        for (int i = 1; i <= 3; ++i) {
            auto g = make_game(w, b, 1200, 1200);
            g.started_at = "2026-09-1" + std::to_string(i) + "T10:00:00Z";
            g.ended_at   = "2026-09-1" + std::to_string(i) + "T10:20:00Z";
            auto r = save_completed_game(db, g);
            if (!r.ok) return false;
        }

        auto games = get_player_games(db, w, 10, 0);
        if (games.size() != 3) return false;
        // Most recent first
        return games[0].started_at > games[1].started_at &&
               games[1].started_at > games[2].started_at;
    });

    run_test("get_player_games limit and offset", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "PageW", 1200);
        int64_t b = insert_player(db, "PageB", 1200);
        if (w < 0 || b < 0) return false;

        for (int i = 0; i < 5; ++i) {
            auto g = make_game(w, b, 1200, 1200);
            g.started_at = "2026-09-1" + std::to_string(i) + "T10:00:00Z";
            g.ended_at   = "2026-09-1" + std::to_string(i) + "T10:20:00Z";
            if (!save_completed_game(db, g).ok) return false;
        }

        auto page1 = get_player_games(db, w, 2, 0);
        auto page2 = get_player_games(db, w, 2, 2);
        return page1.size() == 2 && page2.size() == 2;
    });

    run_test("get_player_games result mapping (w/l/d)", [&] {
        if (!prepare(db)) return false;
        int64_t a = insert_player(db, "MapA", 1200);
        int64_t b = insert_player(db, "MapB", 1200);
        if (a < 0 || b < 0) return false;

        // Game 1: A (white) wins
        auto g1 = make_game(a, b, 1200, 1200, "1-0", "checkmate");
        g1.started_at = "2026-09-13T10:00:00Z";
        g1.ended_at   = "2026-09-13T10:20:00Z";
        if (!save_completed_game(db, g1).ok) return false;

        // Game 2: A (white) loses
        auto g2 = make_game(a, b, 1200, 1200, "0-1", "resignation");
        g2.started_at = "2026-09-12T10:00:00Z";
        g2.ended_at   = "2026-09-12T10:20:00Z";
        if (!save_completed_game(db, g2).ok) return false;

        // Game 3: draw, A is black
        auto g3 = make_game(b, a, 1200, 1200, "1/2-1/2", "stalemate");
        g3.started_at = "2026-09-11T10:00:00Z";
        g3.ended_at   = "2026-09-11T10:20:00Z";
        if (!save_completed_game(db, g3).ok) return false;

        auto games = get_player_games(db, a, 10, 0);
        if (games.size() != 3) return false;
        // Sorted by date DESC: game1 (Sept 13), game2 (Sept 12), game3 (Sept 11)
        return games[0].player_result == "w" && games[0].color == "w" &&
               games[1].player_result == "l" && games[1].color == "w" &&
               games[2].player_result == "d" && games[2].color == "b";
    });

    run_test("get_player_games empty history", [&] {
        if (!prepare(db)) return false;
        int64_t p = insert_player(db, "NoGames", 1200);
        if (p < 0) return false;
        auto games = get_player_games(db, p, 10, 0);
        return games.empty();
    });

    // ════════════════════════════════════════════════════════════════
    //  Integrity & Concurrency Tests
    // ════════════════════════════════════════════════════════════════

    std::cout << "\n=== Integrity & Concurrency ===\n";

    run_test("500 sequential games: all stats add up", [&] {
        if (!prepare(db)) return false;

        // Create 20 players with staggered ratings
        std::vector<int64_t> ids;
        for (int i = 0; i < 20; ++i) {
            int64_t id = insert_player(db, "Bulk" + std::to_string(i),
                                       1000 + i * 50);
            if (id < 0) return false;
            ids.push_back(id);
        }

        // Play 500 games: round-robin-ish
        int total_white_wins = 0;
        int total_black_wins = 0;
        int total_draws      = 0;

        for (int i = 0; i < 500; ++i) {
            int64_t w_id = ids[static_cast<size_t>(i % 20)];
            int64_t b_id = ids[static_cast<size_t>((i + 1 + i / 20) % 20)];
            if (w_id == b_id) b_id = ids[static_cast<size_t>((i + 2) % 20)];

            // Read current ELOs
            auto wp = find_player_by_id(db, w_id);
            auto bp = find_player_by_id(db, b_id);
            if (!wp || !bp) return false;

            // Rotate results
            std::string result;
            std::string term;
            if (i % 3 == 0) {
                result = "1-0"; term = "checkmate";
                ++total_white_wins;
            } else if (i % 3 == 1) {
                result = "0-1"; term = "resignation";
                ++total_black_wins;
            } else {
                result = "1/2-1/2"; term = "stalemate";
                ++total_draws;
            }

            auto g = make_game(w_id, b_id, wp->elo_rating, bp->elo_rating,
                               result, term);
            // Use unique timestamps
            g.started_at = "2026-01-01T" +
                           std::to_string(10 + i / 3600) + ":" +
                           std::to_string((i / 60) % 60) + ":" +
                           std::to_string(i % 60) + "Z";
            g.ended_at = g.started_at;  // simplification
            g.think_times.clear();  // skip for speed

            auto saved = save_completed_game(db, g);
            if (!saved.ok) {
                std::cerr << "Game " << i << " failed: " << saved.error << '\n';
                return false;
            }
        }

        // Verify: sum(wins), sum(losses), sum(draws) across all players
        auto stats = db.exec(
            "SELECT SUM(wins), SUM(losses), SUM(draws), SUM(games_played)"
            " FROM players WHERE username LIKE 'Bulk%'");
        if (!stats.ok || stats.empty()) return false;

        int sum_wins   = std::stoi(stats.first().at(0));
        int sum_losses = std::stoi(stats.first().at(1));
        int sum_draws  = std::stoi(stats.first().at(2));
        int sum_gp     = std::stoi(stats.first().at(3));

        // Each game produces exactly 1 win + 1 loss, OR 2 draws
        // Total sides played = 500 * 2 = 1000
        bool sides_ok = sum_gp == 1000;
        bool wins_match = sum_wins == (total_white_wins + total_black_wins);
        bool losses_match = sum_losses == (total_white_wins + total_black_wins);
        bool draws_match = sum_draws == total_draws * 2;

        // Every player's stats_add_up constraint held (would have thrown CHECK_VIOLATION)
        auto game_count = db.exec("SELECT count(*) FROM games");

        return sides_ok && wins_match && losses_match && draws_match &&
               game_count.ok && game_count.first().at(0) == "500";
    });

    run_test("Concurrent game saves: no constraint violations", [&] {
        if (!prepare(db)) return false;

        // Create 10 players
        std::vector<int64_t> ids;
        for (int i = 0; i < 10; ++i) {
            int64_t id = insert_player(db, "Conc" + std::to_string(i), 1200);
            if (id < 0) return false;
            ids.push_back(id);
        }

        std::atomic<int> successes{0};
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;

        // 50 games across 5 threads (10 games each)
        for (int t = 0; t < 5; ++t) {
            threads.emplace_back([&, t] {
                Database tdb;
                std::string err;
                if (!tdb.connect_from_env(err)) {
                    ++failures;
                    return;
                }

                for (int i = 0; i < 10; ++i) {
                    int idx = t * 10 + i;
                    int64_t w = ids[static_cast<size_t>(idx % 10)];
                    int64_t b = ids[static_cast<size_t>((idx + 1) % 10)];
                    if (w == b) b = ids[static_cast<size_t>((idx + 2) % 10)];

                    auto wp = find_player_by_id(tdb, w);
                    auto bp = find_player_by_id(tdb, b);
                    if (!wp || !bp) { ++failures; continue; }

                    auto g = make_game(w, b, wp->elo_rating, bp->elo_rating,
                                       "1-0", "checkmate");
                    g.started_at = "2026-09-15T10:" +
                                   std::to_string(t) + ":" +
                                   std::to_string(i) + "Z";
                    g.ended_at = g.started_at;
                    g.think_times.clear();

                    auto r = save_completed_game(tdb, g);
                    if (r.ok) ++successes;
                    else ++failures;
                }
            });
        }

        for (auto& th : threads) th.join();

        // All games should succeed — Postgres serializes row-level UPDATEs
        auto gcount = db.exec("SELECT count(*) FROM games");

        // Verify stats still add up (constraint would have fired on violation)
        auto bad = db.exec(
            "SELECT count(*) FROM players"
            " WHERE username LIKE 'Conc%'"
            " AND games_played <> wins + losses + draws");

        return successes.load() == 50 && failures.load() == 0 &&
               gcount.ok && gcount.first().at(0) == "50" &&
               bad.ok && bad.first().at(0) == "0";
    });

    run_test("ELO floor: series of losses never goes negative", [&] {
        if (!prepare(db)) return false;
        int64_t weak   = insert_player(db, "Weak", 50);
        int64_t strong = insert_player(db, "Strong", 2000);
        if (weak < 0 || strong < 0) return false;

        // 10 losses in a row for the weak player
        for (int i = 0; i < 10; ++i) {
            auto wp = find_player_by_id(db, weak);
            auto sp = find_player_by_id(db, strong);
            if (!wp || !sp) return false;

            auto g = make_game(weak, strong, wp->elo_rating, sp->elo_rating,
                               "0-1", "checkmate");
            g.started_at = "2026-09-15T10:0" + std::to_string(i) + ":00Z";
            g.ended_at = g.started_at;
            g.think_times.clear();
            if (!save_completed_game(db, g).ok) return false;
        }

        auto wp = find_player_by_id(db, weak);
        return wp && wp->elo_rating >= 0;
    });

    run_test("Stats constraint: manual inconsistency rejected", [&] {
        if (!prepare(db)) return false;
        int64_t id = insert_player(db, "BadStats", 1200);
        if (id < 0) return false;
        // Try to set wins=999 without matching games_played
        auto r = db.exec(
            "UPDATE players SET wins = 999 WHERE id = $1",
            {Param::int64(id)});
        return !r.ok && r.sqlstate == pg_errors::CHECK_VIOLATION;
    });

    run_test("Leaderboard after mass games uses idx_players_elo", [&] {
        // Re-use the state from the 500-game test (or re-prepare)
        if (!prepare(db)) return false;
        for (int i = 0; i < 50; ++i) {
            int64_t id = insert_player(db, "Idx" + std::to_string(i),
                                       1000 + i * 20);
            if (id < 0) return false;
            db.exec("UPDATE players SET games_played=1, wins=1 WHERE id=$1",
                    {Param::int64(id)});
        }
        db.exec("ANALYZE players");

        auto plan = db.exec(
            "EXPLAIN (ANALYZE, COSTS OFF)"
            " SELECT id, username, elo_rating FROM players"
            " WHERE games_played > 0"
            " ORDER BY elo_rating DESC, id ASC"
            " LIMIT 20");

        bool used_index = false;
        if (plan.ok) {
            for (const auto& row : plan.rows) {
                if (row.at(0).find("idx_players_elo") != std::string::npos) {
                    used_index = true;
                    break;
                }
            }
        }
        return used_index;
    });

    run_test("get_player_games uses game indexes", [&] {
        if (!prepare(db)) return false;
        int64_t w = insert_player(db, "IdxW", 1200);
        int64_t b = insert_player(db, "IdxB", 1200);
        if (w < 0 || b < 0) return false;

        // Insert enough games for Postgres to prefer the index
        for (int i = 0; i < 50; ++i) {
            auto g = make_game(w, b, 1200, 1200);
            g.started_at = "2026-09-0" + std::to_string(1 + i % 9) + "T10:00:00Z";
            g.ended_at = g.started_at;
            g.think_times.clear();
            if (!save_completed_game(db, g).ok) return false;
        }
        db.exec("ANALYZE games");

        auto plan = db.exec(
            "EXPLAIN (ANALYZE, COSTS OFF)"
            " (SELECT g.id FROM games g WHERE g.white_id = $1"
            "  UNION ALL"
            "  SELECT g.id FROM games g WHERE g.black_id = $1)"
            " ORDER BY id DESC LIMIT 20",
            {Param::int64(w)});

        bool used_index = false;
        if (plan.ok) {
            for (const auto& row : plan.rows) {
                if (row.at(0).find("idx_games_white") != std::string::npos ||
                    row.at(0).find("idx_games_black") != std::string::npos) {
                    used_index = true;
                    break;
                }
            }
        }
        return used_index;
    });

    // ════════════════════════════════════════════════════════════════
    //  Summary
    // ════════════════════════════════════════════════════════════════

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed
              << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
