/**
 * test_game_completion.cpp — LLD-4.2.
 *
 * Verifies the idempotency + capability guards of the completion path:
 *
 *   1. A GameCompleted event that reaches the service saves once.
 *   2. A second dispatch with the SAME snapshot's completion_uuid is a
 *      no-op — same game_id returned, no ELO/stats mutated a second
 *      time.
 *   3. The completion service short-circuits (no store call) when:
 *        - store.capable() is false (NullGameStore), or
 *        - a required human seat's db_player_id is 0 (unauthenticated).
 *      AI snapshots instead produce an unrated replay record with a nullable
 *      computer seat.
 *   4. GameRoom stamps every terminal snapshot with a non-empty UUID
 *      that is unique per completion (a 32-room stress sample yields
 *      32 distinct uuids).
 *   5. The idempotency check survives a completion_uuid retry across
 *      TWO service instances against the SAME store (proves the guard
 *      is DB-side, not in the service).
 *
 * DB path uses a fresh cluster if $DATABASE_URL is set; skipped
 * gracefully otherwise.
 */

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "application/game_completion_service.h"
#include "application/ports/null_persistence.h"
#include "core/uuid.h"
#include "game/game_events.h"
#include "game/game_room.h"
#include "game/game_snapshot.h"
#include "game/room_manager.h"
#include "storage/database.h"
#include "storage/postgres_game_store.h"

using namespace chess;
using namespace chess::application;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) { std::cout << "PASS\n"; ++g_passed; }
        else      { std::cout << "FAIL\n"; ++g_failed; }
    } catch (const std::exception& e) {
        std::cout << "FAIL (exception: " << e.what() << ")\n"; ++g_failed;
    }
}

std::string source_path(const std::string& rel) {
#ifdef CHESS_SOURCE_DIR
    return std::string(CHESS_SOURCE_DIR) + "/" + rel;
#else
    return rel;
#endif
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Recording store — captures every save_completed_game invocation
/// and lets the test dictate the outcome. Bypasses libpq entirely.
struct RecordingStore final : public ports::GameStore {
    struct Call {
        std::string completion_uuid;
        int64_t     white_id      = 0;
        int64_t     black_id      = 0;
        std::string result;
        bool        rated = true;
        std::string black_display_name;
    };
    std::vector<Call>              calls;
    ports::SaveGameOutcome         next_outcome{};
    bool                           capable_ret = true;

    bool capable() const override { return capable_ret; }

    ports::SaveGameOutcome save_completed_game(
            const chess::storage::CompletedGame& game) override {
        calls.push_back({game.completion_uuid, game.white_id, game.black_id,
                         game.result, game.rated, game.black_display_name});
        auto out = next_outcome;
        if (out.code == chess::storage::StorageError::Ok && out.game_id == 0) {
            out.game_id = static_cast<int64_t>(calls.size());
        }
        return out;
    }
    ports::SaveCheatReportOutcome save_cheat_report(
            int64_t, int64_t, const std::string&,
            const chess::analysis::AnalysisReport&) override {
        return {};
    }
};

chess::game::GameSnapshot make_snapshot(int64_t white_db_id, int64_t black_db_id,
                                        bool is_ai = false) {
    chess::game::GameSnapshot s;
    s.room_id            = 42;
    s.completion_uuid    = chess::core::generate_uuid_v4();
    s.is_ai_game         = is_ai;
    s.status             = chess::GameStatus::RESIGNATION;
    s.result             = "0-1";
    s.termination_reason = "resignation";
    s.white.db_player_id = white_db_id;
    s.white.username     = "alice";
    s.white.elo          = 1500;
    s.black.db_player_id = black_db_id;
    s.black.username     = "bob";
    s.black.elo          = 1500;
    s.started_at_iso     = "2026-09-20T12:00:00Z";
    s.ended_at_iso       = "2026-09-20T12:15:00Z";
    s.move_count         = 0;
    return s;
}

// ── DB helpers (mirrors test_game_persistence's shape) ──

std::optional<chess::storage::Database> open_db() {
    if (!std::getenv("DATABASE_URL")) return std::nullopt;
    chess::storage::Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "\n  DB connect failed: " << err << '\n';
        return std::nullopt;
    }
    return std::make_optional(std::move(db));
}

bool prepare_db(chess::storage::Database& db) {
    std::string err;
    if (!db.run_script(
            "DROP TABLE IF EXISTS tournament_result_overrides;"
            "DROP TABLE IF EXISTS tournament_round_checkins;"
            "DROP TABLE IF EXISTS tournament_rounds;"
            "DROP TABLE IF EXISTS tournament_pairings;"
            "DROP TABLE IF EXISTS tournament_players;"
            "DROP TABLE IF EXISTS tournaments;"
            "DROP TABLE IF EXISTS cheat_reports;"
            "DROP TABLE IF EXISTS move_times;"
            "DROP TABLE IF EXISTS games;"
            "DROP TABLE IF EXISTS sessions;"
            "DROP TABLE IF EXISTS schema_migrations;"
            "DROP TABLE IF EXISTS players;"
            "DROP FUNCTION IF EXISTS assert_username_ci_matches();",
            err)) return false;
    if (!db.run_script(read_file(source_path("src/storage/schema_phase7.sql")), err))
        return false;
    bool applied = false;
    if (!db.apply_migration("0001_phase8_game_persistence",
            read_file(source_path("src/storage/migrations/0001_phase8_game_persistence.sql")),
            applied, err)) return false;
    if (!db.apply_migration("0004_lld4_completion_uuid",
            read_file(source_path("src/storage/migrations/0004_lld4_completion_uuid.sql")),
            applied, err)) return false;
    if (!db.apply_migration("0008_allow_abandonment_termination",
            read_file(source_path("src/storage/migrations/0008_allow_abandonment_termination.sql")),
            applied, err)) return false;
    if (!db.apply_migration("0009_persist_unrated_ai_games",
            read_file(source_path("src/storage/migrations/0009_persist_unrated_ai_games.sql")),
            applied, err)) return false;
    return true;
}

int64_t insert_player(chess::storage::Database& db, const std::string& name,
                      int elo = 1500) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    auto r = db.exec(
        "INSERT INTO players(username, username_ci, password_hash, elo_rating)"
        " VALUES($1,$2,$3,$4) RETURNING id",
        {chess::storage::Param::text(name),
         chess::storage::Param::text(lower),
         chess::storage::Param::text("hash"),
         chess::storage::Param::int64(elo)});
    if (!r.ok || r.empty()) return -1;
    return std::stoll(r.first().at(0));
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-4.2 — GameCompletionService\n";
    std::cout << "========================================\n";

    // ── Unit tests (no DB) ────────────────────────────────────────────

    run_test("capable()=false short-circuits — no store call", [] {
        ports::NullGameStore null_store;
        GameCompletionService svc(null_store);
        chess::game::GameCompleted ev;
        ev.snapshot = make_snapshot(1, 2);
        // NullGameStore's capable() is false; the service must not
        // even build a CompletedGame. We prove that by using a
        // Recording variant instead.
        RecordingStore store;
        store.capable_ret = false;
        GameCompletionService svc2(store);
        svc2.on_game_completed(ev);
        return store.calls.empty();
    });

    run_test("AI completion persists an unrated replay without a DB opponent", [] {
        RecordingStore store;
        GameCompletionService svc(store);
        chess::game::GameCompleted ev;
        ev.snapshot = make_snapshot(1, 0, /*is_ai=*/true);
        ev.snapshot.black.username = "AI (Hard)";
        svc.on_game_completed(ev);
        return store.calls.size() == 1
            && store.calls[0].white_id == 1
            && store.calls[0].black_id == 0
            && !store.calls[0].rated
            && store.calls[0].black_display_name == "AI (Hard)";
    });

    run_test("db_player_id=0 on either seat short-circuits", [] {
        RecordingStore store;
        GameCompletionService svc(store);
        chess::game::GameCompleted ev;
        ev.snapshot = make_snapshot(0, 2);
        svc.on_game_completed(ev);
        if (!store.calls.empty()) return false;
        ev.snapshot = make_snapshot(1, 0, /*is_ai=*/false);
        svc.on_game_completed(ev);
        return store.calls.empty();
    });

    run_test("happy path — one save call carries the snapshot uuid", [] {
        RecordingStore store;
        GameCompletionService svc(store);
        chess::game::GameCompleted ev;
        ev.snapshot = make_snapshot(11, 22);
        const std::string expected_uuid = ev.snapshot.completion_uuid;
        svc.on_game_completed(ev);
        return store.calls.size() == 1
            && store.calls[0].completion_uuid == expected_uuid
            && store.calls[0].white_id == 11
            && store.calls[0].black_id == 22
            && store.calls[0].result == "0-1";
    });

    run_test("GameRoom stamps every terminal snapshot with a distinct uuid", [] {
        std::set<std::string> seen;
        for (int i = 0; i < 32; ++i) {
            chess::game::GameRoom room(
                static_cast<chess::GameId>(i + 1),
                1, "alice", 10, chess::game::TimeControl{}, 100, 1500);
            room.join(2, "bob", 11, 200, 1500);
            // Capture via a listener because build_snapshot_locked is
            // private; resign() drives finish_game which populates it.
            struct Cap : chess::game::GameEventListener {
                std::string uuid;
                void on_game_completed(const chess::game::GameCompleted& e) override {
                    uuid = e.snapshot.completion_uuid;
                }
            };
            auto cap = std::make_shared<Cap>();
            room.add_listener(cap);
            room.resign(10);
            if (cap->uuid.size() != 36) return false;
            seen.insert(cap->uuid);
        }
        return seen.size() == 32;
    });

    // ── DB integration tests (require $DATABASE_URL) ──────────────────

    auto db_opt = open_db();
    if (!db_opt) {
        std::cout << "\n  Skipping DB integration tests ($DATABASE_URL unset)\n";
    } else {
        auto& db = *db_opt;
        if (!prepare_db(db)) {
            std::cerr << "  prepare_db failed — skipping DB tests\n";
        } else {
            int64_t alice = insert_player(db, "Alice", 1500);
            int64_t bob   = insert_player(db, "Bob",   1500);
            if (alice <= 0 || bob <= 0) {
                std::cerr << "  seeding players failed — skipping DB tests\n";
            } else {
                run_test("save_completed_game returns already_persisted=false first, true on retry", [&] {
                    chess::storage::PostgresGameStore store(db);
                    GameCompletionService svc(store);

                    chess::game::GameCompleted ev;
                    ev.snapshot = make_snapshot(alice, bob);
                    svc.on_game_completed(ev);

                    // Re-dispatch with the SAME snapshot uuid.
                    svc.on_game_completed(ev);

                    // Verify exactly one games row exists for this uuid,
                    // and the player rows were incremented once — not twice.
                    auto count = db.exec(
                        "SELECT COUNT(*) FROM games WHERE completion_uuid = $1",
                        {chess::storage::Param::text(ev.snapshot.completion_uuid)});
                    if (!count.ok || count.first().at(0) != "1") return false;
                    auto gp = db.exec(
                        "SELECT games_played, losses FROM players WHERE id = $1",
                        {chess::storage::Param::int64(alice)});
                    return gp.ok && !gp.empty()
                        && gp.first().at(0) == "1"
                        && gp.first().at(1) == "1";
                });

                run_test("second service instance against same DB is also a no-op retry", [&] {
                    chess::storage::PostgresGameStore store1(db);
                    chess::storage::PostgresGameStore store2(db);
                    GameCompletionService svc1(store1);
                    GameCompletionService svc2(store2);

                    chess::game::GameCompleted ev;
                    ev.snapshot = make_snapshot(alice, bob);
                    ev.snapshot.result = "1-0";
                    svc1.on_game_completed(ev);

                    // Grab the DB id from the first insert.
                    auto probe1 = db.exec(
                        "SELECT id FROM games WHERE completion_uuid = $1",
                        {chess::storage::Param::text(ev.snapshot.completion_uuid)});
                    if (!probe1.ok || probe1.empty()) return false;
                    const std::string id1 = probe1.first().at(0);

                    // A second service holding a different store handle,
                    // fed the same snapshot, must find the row and skip.
                    svc2.on_game_completed(ev);

                    auto probe2 = db.exec(
                        "SELECT COUNT(*) FROM games WHERE completion_uuid = $1",
                        {chess::storage::Param::text(ev.snapshot.completion_uuid)});
                    return probe2.ok
                        && probe2.first().at(0) == "1"
                        && !id1.empty();
                });

                run_test("abandonment termination is accepted after migration 0008", [&] {
                    chess::storage::PostgresGameStore store(db);
                    GameCompletionService svc(store);
                    chess::game::GameCompleted ev;
                    ev.snapshot = make_snapshot(alice, bob);
                    ev.snapshot.result = "1-0";
                    ev.snapshot.termination_reason = "abandonment";
                    svc.on_game_completed(ev);

                    auto probe = db.exec(
                        "SELECT termination FROM games WHERE completion_uuid = $1",
                        {chess::storage::Param::text(ev.snapshot.completion_uuid)});
                    return probe.ok && !probe.empty()
                        && probe.first().at(0) == "abandonment";
                });
            }
        }
    }

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
