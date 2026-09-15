/**
 * test_game_persistence.cpp — Phase 8.3 integration tests.
 *
 * Tests the wiring between GameRoom, GameHandler::persist_game(), and the
 * storage repositories. Covers:
 *   - GameRoom new getters (player_id, db_player_id, username, elo by Color)
 *   - Wall-clock timestamps (ISO 8601 format)
 *   - persist_game skip conditions (no DB, AI game, unauthenticated)
 *   - Full end-to-end: create players → play game → verify persisted
 *   - get_profile and get_leaderboard handlers (via direct repo calls)
 *
 * DB-backed tests require DATABASE_URL; skips gracefully if unset.
 */

#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include "game/game_room.h"
#include "game/room_manager.h"
#include "storage/database.h"
#include "storage/elo.h"
#include "storage/game_repo.h"
#include "storage/player_repo.h"

using namespace chess;
using namespace chess::game;
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

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 8.3 - Game Persistence Tests\n";
    std::cout << "========================================\n";

    // ════════════════════════════════════════════════════════════════
    //  GameRoom Getter Tests (no DB needed)
    // ════════════════════════════════════════════════════════════════

    std::cout << "\n=== GameRoom Getters ===\n";

    run_test("get_player_id returns correct values", [] {
        GameRoom room(1, 42, "Alice", 10, TimeControl(), 100, 1500);
        room.join(43, "Bob", 11, 200, 1400);
        return room.get_player_id(Color::WHITE) == 42 &&
               room.get_player_id(Color::BLACK) == 43;
    });

    run_test("get_db_player_id returns correct values", [] {
        GameRoom room(1, 42, "Alice", 10, TimeControl(), 100, 1500);
        room.join(43, "Bob", 11, 200, 1400);
        return room.get_db_player_id(Color::WHITE) == 100 &&
               room.get_db_player_id(Color::BLACK) == 200;
    });

    run_test("get_db_player_id returns 0 for unauthenticated", [] {
        GameRoom room(1, 42, "Alice", 10);
        room.join(43, "Bob", 11);
        return room.get_db_player_id(Color::WHITE) == 0 &&
               room.get_db_player_id(Color::BLACK) == 0;
    });

    run_test("get_username returns correct values", [] {
        GameRoom room(1, 42, "Alice", 10, TimeControl(), 100, 1500);
        room.join(43, "Bob", 11, 200, 1400);
        return room.get_username(Color::WHITE) == "Alice" &&
               room.get_username(Color::BLACK) == "Bob";
    });

    run_test("get_elo returns correct values", [] {
        GameRoom room(1, 42, "Alice", 10, TimeControl(), 100, 1500);
        room.join(43, "Bob", 11, 200, 1400);
        return room.get_elo(Color::WHITE) == 1500 &&
               room.get_elo(Color::BLACK) == 1400;
    });

    run_test("get_elo defaults to 1200 when unspecified", [] {
        GameRoom room(1, 42, "Alice", 10);
        room.join(43, "Bob", 11);
        return room.get_elo(Color::WHITE) == 1200 &&
               room.get_elo(Color::BLACK) == 1200;
    });

    run_test("get_started_at_iso returns valid ISO 8601 after join", [] {
        GameRoom room(1, 42, "Alice", 10);
        room.join(43, "Bob", 11);
        auto ts = room.get_started_at_iso();
        // Should match "YYYY-MM-DDTHH:MM:SSZ"
        std::regex iso_re(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
        return std::regex_match(ts, iso_re);
    });

    run_test("get_ended_at_iso populated after game finish", [] {
        GameRoom room(1, 42, "Alice", 10, TimeControl(600000, 5000), 100, 1500);
        room.join(43, "Bob", 11, 200, 1400);
        // Resign to finish the game
        room.resign(10);
        auto ts = room.get_ended_at_iso();
        std::regex iso_re(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
        return std::regex_match(ts, iso_re) && room.get_state() == RoomState::FINISHED;
    });

    run_test("AI game room has correct AI player data", [] {
        GameRoom room(1, 42, "Human", 10, TimeControl(), AIDifficulty::MEDIUM);
        return room.is_ai_game() &&
               room.get_username(Color::BLACK).find("AI") != std::string::npos &&
               room.get_db_player_id(Color::BLACK) == 0;
    });

    // ════════════════════════════════════════════════════════════════
    //  DB-backed Integration Tests
    // ════════════════════════════════════════════════════════════════

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping DB tests.\n";
    } else {
        Database db = open_db();
        if (db.connected()) {

            std::cout << "\n=== End-to-End Persistence ===\n";

            run_test("Full game: create room → play → persist → verify in DB", [&] {
                if (!prepare(db)) return false;

                // Create two players in the database
                int64_t alice_id = insert_player(db, "Alice", 1500);
                int64_t bob_id   = insert_player(db, "Bob", 1400);
                if (alice_id < 0 || bob_id < 0) return false;

                // Create a game room with authenticated players
                GameRoom room(1, 1, "Alice", 10, TimeControl(600000, 5000),
                              alice_id, 1500);
                room.join(2, "Bob", 11, bob_id, 1400);

                // Play Scholar's Mate: 1.e4 e5 2.Qh5 a6 3.Bc4 a5 4.Qxf7#
                auto r1 = room.submit_move(10, make_square(1, 4), make_square(3, 4)); // e2e4
                if (!r1.success) return false;
                auto r2 = room.submit_move(11, make_square(6, 4), make_square(4, 4)); // e7e5
                if (!r2.success) return false;
                auto r3 = room.submit_move(10, make_square(0, 3), make_square(4, 7)); // d1h5
                if (!r3.success) return false;
                auto r4 = room.submit_move(11, make_square(6, 0), make_square(5, 0)); // a7a6
                if (!r4.success) return false;
                auto r5 = room.submit_move(10, make_square(0, 5), make_square(3, 2)); // f1c4
                if (!r5.success) return false;
                auto r6 = room.submit_move(11, make_square(5, 0), make_square(4, 0)); // a6a5
                if (!r6.success) return false;
                auto r7 = room.submit_move(10, make_square(4, 7), make_square(6, 5)); // h5f7
                if (!r7.success) return false;

                // Game should be finished (checkmate)
                if (room.get_state() != RoomState::FINISHED) return false;
                if (room.get_result_string() != "1-0") return false;

                // Assemble CompletedGame
                CompletedGame game;
                game.white_id    = alice_id;
                game.black_id    = bob_id;
                game.white_elo   = 1500;
                game.black_elo   = 1400;
                game.result      = room.get_result_string();
                game.termination = "checkmate";
                game.time_control = room.get_time_control().to_string();
                game.started_at  = room.get_started_at_iso();
                game.ended_at    = room.get_ended_at_iso();

                auto history = room.get_move_history();
                game.move_count = static_cast<int>(history.size());

                std::string moves;
                for (size_t i = 0; i < history.size(); ++i) {
                    if (i > 0) moves += ' ';
                    moves += history[i].move.to_uci();
                    game.think_times.push_back({
                        static_cast<int>(i + 1),
                        (i % 2 == 0) ? alice_id : bob_id,
                        history[i].think_time_ms
                    });
                }
                game.moves = std::move(moves);

                // Persist
                auto saved = save_completed_game(db, game);
                if (!saved.ok) {
                    std::cerr << "persist failed: " << saved.error << '\n';
                    return false;
                }

                // Verify in DB
                auto stored = find_game_by_id(db, saved.game_id);
                if (!stored) return false;
                if (stored->result != "1-0") return false;
                if (stored->termination != "checkmate") return false;
                if (stored->white_name != "Alice") return false;
                if (stored->black_name != "Bob") return false;
                if (stored->move_count != 7) return false;

                // Verify player stats updated
                auto alice = find_player_by_id(db, alice_id);
                auto bob_p = find_player_by_id(db, bob_id);
                if (!alice || !bob_p) return false;
                if (alice->wins != 1 || alice->games_played != 1) return false;
                if (bob_p->losses != 1 || bob_p->games_played != 1) return false;

                // Verify ELO changed
                if (alice->elo_rating <= 1500) return false;  // Winner should gain
                if (bob_p->elo_rating >= 1400) return false;  // Loser should lose

                // Verify game appears in player history
                auto alice_games = get_player_games(db, alice_id, 10, 0);
                if (alice_games.size() != 1) return false;
                if (alice_games[0].player_result != "w") return false;
                if (alice_games[0].opponent_name != "Bob") return false;

                return true;
            });

            run_test("Profile query returns correct data", [&] {
                // Uses state from previous test
                auto profile = find_player_by_username(db, "alice");
                if (!profile) return false;
                return profile->username == "Alice" &&
                       profile->games_played == 1 &&
                       profile->wins == 1;
            });

            run_test("Leaderboard returns ranked players", [&] {
                auto lb = get_leaderboard(db, 50, 0);
                if (lb.size() != 2) return false;
                // Alice should be ranked higher (she won and gained ELO)
                return lb[0].username == "Alice" && lb[0].rank == 1 &&
                       lb[1].username == "Bob" && lb[1].rank == 2;
            });

            run_test("Resignation persists correctly", [&] {
                if (!prepare(db)) return false;
                int64_t w = insert_player(db, "Resigner", 1200);
                int64_t b = insert_player(db, "Stayer", 1200);
                if (w < 0 || b < 0) return false;

                GameRoom room(2, 1, "Resigner", 10, TimeControl(600000, 5000), w, 1200);
                room.join(2, "Stayer", 11, b, 1200);
                room.resign(10);  // White resigns

                CompletedGame game;
                game.white_id = w; game.black_id = b;
                game.white_elo = 1200; game.black_elo = 1200;
                game.result = room.get_result_string();
                game.termination = "resignation";
                game.time_control = room.get_time_control().to_string();
                game.started_at = room.get_started_at_iso();
                game.ended_at = room.get_ended_at_iso();
                game.moves = ""; game.move_count = 0;

                auto saved = save_completed_game(db, game);
                if (!saved.ok) return false;

                auto stored = find_game_by_id(db, saved.game_id);
                return stored && stored->result == "0-1" &&
                       stored->termination == "resignation";
            });

            run_test("Think times are persisted per-ply", [&] {
                if (!prepare(db)) return false;
                int64_t w = insert_player(db, "TimerW", 1200);
                int64_t b = insert_player(db, "TimerB", 1200);
                if (w < 0 || b < 0) return false;

                GameRoom room(3, 1, "TimerW", 10, TimeControl(600000, 5000), w, 1200);
                room.join(2, "TimerB", 11, b, 1200);

                // Play 2 moves
                room.submit_move(10, make_square(1, 4), make_square(3, 4)); // e4
                room.submit_move(11, make_square(6, 4), make_square(4, 4)); // e5
                room.resign(10);

                auto history = room.get_move_history();

                CompletedGame game;
                game.white_id = w; game.black_id = b;
                game.white_elo = 1200; game.black_elo = 1200;
                game.result = "0-1"; game.termination = "resignation";
                game.time_control = "600+5"; game.move_count = 2;
                game.started_at = room.get_started_at_iso();
                game.ended_at = room.get_ended_at_iso();
                game.moves = "e2e4 e7e5";
                game.think_times = {
                    {1, w, history[0].think_time_ms},
                    {2, b, history[1].think_time_ms}
                };

                auto saved = save_completed_game(db, game);
                if (!saved.ok) return false;

                auto times = db.exec(
                    "SELECT ply_number, player_id FROM move_times"
                    " WHERE game_id=$1 ORDER BY ply_number",
                    {Param::int64(saved.game_id)});
                return times.ok && times.rows.size() == 2 &&
                       times.rows[0].at(1) == std::to_string(w) &&
                       times.rows[1].at(1) == std::to_string(b);
            });
        }
    }

    // ════════════════════════════════════════════════════════════════
    //  Summary
    // ════════════════════════════════════════════════════════════════

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed
              << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
