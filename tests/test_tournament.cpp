/**
 * test_tournament.cpp — Phase 9.4.
 *
 * Three layers:
 *
 *   1. Pure Swiss algorithm. Deterministic pairing on hand-crafted
 *      standings; asserts score-monotone pairing, rematch avoidance,
 *      bye assignment, and colour balance.
 *
 *   2. Repo round-trip. Create → find → add participants → insert
 *      pairing → set result. Purely storage; no manager.
 *
 *   3. End-to-end. An 8-player, 4-round Swiss driven through
 *      TournamentManager. Verifies no rematches over the whole event,
 *      every round is fully populated, and status transitions land
 *      registration → in_progress → completed.
 *
 * DB-backed layers skip cleanly without DATABASE_URL. Layer 1 always runs.
 */

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "tournament/swiss.h"
#include "tournament/tournament_manager.h"
#include "tournament/tournament_repo.h"
#include "storage/database.h"

using namespace chess;
using namespace chess::tournament;
using namespace chess::storage;

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

PlayerStanding P(int64_t id, int elo, double score = 0.0,
                 int whites = 0, bool bye = false, bool withdrawn = false) {
    PlayerStanding s;
    s.player_id     = id;
    s.elo           = elo;
    s.score         = score;
    s.whites_played = whites;
    s.received_bye  = bye;
    s.withdrawn     = withdrawn;
    return s;
}

// ============================================================
// Layer 1 — pure Swiss algorithm
// ============================================================

void run_algorithm_tests() {
    std::cout << "\n=== Layer 1: Swiss algorithm ===\n";

    run_test("empty pool → empty pairings", [] {
        std::vector<PlayerStanding> s;
        auto out = pair_swiss_round(s, {});
        return out.empty();
    });

    run_test("two players → one pairing, no bye", [] {
        std::vector<PlayerStanding> s = {P(1, 1600), P(2, 1500)};
        auto out = pair_swiss_round(s, {});
        if (out.size() != 1 || out[0].is_bye) return false;
        // Higher-rated seed (id=1) gets White in round 1 (no prior colours).
        return out[0].white_id == 1 && out[0].black_id == 2;
    });

    run_test("odd count → lowest without bye receives the bye", [] {
        std::vector<PlayerStanding> s = {
            P(1, 1700, 2.0),
            P(2, 1600, 2.0),
            P(3, 1500, 1.0),
            P(4, 1400, 0.5),
            P(5, 1300, 0.0),
        };
        auto out = pair_swiss_round(s, {});
        // Expect two match-pairings and a bye for the lowest-scorer, id=5.
        if (out.size() != 3) return false;
        // Bye is last by convention.
        return out.back().is_bye && out.back().white_id == 5;
    });

    run_test("odd count with all-had-byes → falls back to lowest", [] {
        std::vector<PlayerStanding> s = {
            P(1, 1700, 3.0, 1, /*bye=*/true),
            P(2, 1600, 2.0, 1, /*bye=*/true),
            P(3, 1500, 1.0, 1, /*bye=*/true),
        };
        auto out = pair_swiss_round(s, {});
        // Everyone already had a bye — lowest (id=3) gets another.
        return out.back().is_bye && out.back().white_id == 3;
    });

    run_test("rematch is avoided when a fresh opponent exists", [] {
        // Four players tied at 1.0; (1,2) and (3,4) have already met once.
        // Round 1 result should NOT pair 1-2 again.
        std::vector<PlayerStanding> s = {
            P(1, 1600, 1.0), P(2, 1500, 1.0),
            P(3, 1400, 1.0), P(4, 1300, 1.0),
        };
        std::set<PlayerPair> played{PlayerPair(1, 2), PlayerPair(3, 4)};
        auto out = pair_swiss_round(s, played);
        if (out.size() != 2) return false;
        for (const auto& p : out) {
            PlayerPair pp(p.white_id, p.black_id);
            if (pp == PlayerPair(1, 2)) return false;
            if (pp == PlayerPair(3, 4)) return false;
        }
        return true;
    });

    run_test("rematch is accepted when every opponent has been played", [] {
        // 2 players who have already met — algorithm must still pair them.
        std::vector<PlayerStanding> s = {P(1, 1600, 1.0), P(2, 1500, 1.0)};
        std::set<PlayerPair> played{PlayerPair(1, 2)};
        auto out = pair_swiss_round(s, played);
        return out.size() == 1 && !out[0].is_bye
            && ((out[0].white_id == 1 && out[0].black_id == 2)
             || (out[0].white_id == 2 && out[0].black_id == 1));
    });

    run_test("colours balance — player with more whites so far gets black", [] {
        // Both tied at 1.0; id=1 has 2 whites, id=2 has 0. Round should
        // put id=2 as White.
        std::vector<PlayerStanding> s = {
            P(1, 1600, 1.0, /*whites=*/2),
            P(2, 1500, 1.0, /*whites=*/0),
        };
        auto out = pair_swiss_round(s, {});
        return out.size() == 1 && !out[0].is_bye
            && out[0].white_id == 2 && out[0].black_id == 1;
    });

    run_test("score-monotone: top scores paired first", [] {
        std::vector<PlayerStanding> s = {
            P(1, 1500, 2.0), P(2, 1500, 2.0),
            P(3, 1500, 1.0), P(4, 1500, 1.0),
            P(5, 1500, 0.0), P(6, 1500, 0.0),
        };
        auto out = pair_swiss_round(s, {});
        // First pairing should feature the two 2.0-scorers.
        if (out.size() != 3) return false;
        std::set<int64_t> top{out[0].white_id, out[0].black_id};
        return top == std::set<int64_t>{1, 2};
    });

    run_test("withdrawn players are dropped from pairings", [] {
        std::vector<PlayerStanding> s = {
            P(1, 1600, 1.0),
            P(2, 1500, 1.0),
            P(3, 1400, 1.0, 0, false, /*withdrawn=*/true),
        };
        auto out = pair_swiss_round(s, {});
        // Only 1 and 2 should feature (3 is out).
        if (out.size() != 1 || out[0].is_bye) return false;
        return (out[0].white_id != 3 && out[0].black_id != 3);
    });

    run_test("deterministic — identical inputs give identical outputs", [] {
        std::vector<PlayerStanding> s = {
            P(10, 1600, 1.5), P(20, 1500, 1.5),
            P(30, 1450, 1.0), P(40, 1400, 1.0),
        };
        auto a = pair_swiss_round(s, {});
        auto b = pair_swiss_round(s, {});
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].white_id != b[i].white_id) return false;
            if (a[i].black_id != b[i].black_id) return false;
            if (a[i].is_bye   != b[i].is_bye)   return false;
        }
        return true;
    });

    run_test("id-tiebreak — equal score+elo sorts by id", [] {
        // Two players share (score, elo). Their relative seat comes from id.
        std::vector<PlayerStanding> s = {
            P(200, 1500, 1.0), P(100, 1500, 1.0),
        };
        auto out = pair_swiss_round(s, {});
        return out.size() == 1 && !out[0].is_bye
            && out[0].white_id == 100 && out[0].black_id == 200;
    });

    run_test("four-round trace on 8 seeds — no rematches ever occur", [] {
        // Simulate a 4-round tournament with deterministic results: the
        // higher seed always wins. Verify the whole tournament has no
        // repeat pairing across all four rounds.
        std::vector<PlayerStanding> s;
        for (int64_t i = 1; i <= 8; ++i) s.push_back(P(i, 2000 - static_cast<int>(i) * 50));
        std::set<PlayerPair> played;

        for (int r = 0; r < 4; ++r) {
            auto out = pair_swiss_round(s, played);
            if (out.size() != 4) return false;
            for (const auto& p : out) {
                if (p.is_bye) return false;   // no byes with 8 players
                PlayerPair pp(p.white_id, p.black_id);
                if (played.count(pp)) return false;  // rematch guard
                played.insert(pp);

                // Apply "higher seed wins" — the seed order came from ids,
                // so the lower id wins. That gives us score updates for
                // the next round.
                auto bump_score = [&](int64_t id, double d) {
                    for (auto& x : s) if (x.player_id == id) x.score += d;
                };
                auto bump_white = [&](int64_t id) {
                    for (auto& x : s) if (x.player_id == id) ++x.whites_played;
                };
                if (p.white_id < p.black_id) bump_score(p.white_id, 1.0);
                else                         bump_score(p.black_id, 1.0);
                bump_white(p.white_id);
            }
        }
        return played.size() == 16;   // 4 rounds × 4 pairings, all unique
    });
}

// ============================================================
// Layer 2 & 3 — DB and end-to-end
// ============================================================

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream buf; buf << f.rdbuf();
    return buf.str();
}
std::string source_path(const std::string& rel) {
#ifdef CHESS_SOURCE_DIR
    return std::string(CHESS_SOURCE_DIR) + "/" + rel;
#else
    return rel;
#endif
}

bool prepare_schema(Database& db) {
    std::string err;
    if (!db.run_script(
            "DROP TABLE IF EXISTS tournament_pairings;"
            "DROP TABLE IF EXISTS tournament_players;"
            "DROP TABLE IF EXISTS tournaments;"
            "DROP TABLE IF EXISTS cheat_reports;"
            "DROP TABLE IF EXISTS move_times;"
            "DROP TABLE IF EXISTS games;"
            "DROP TABLE IF EXISTS sessions;"
            "DROP TABLE IF EXISTS schema_migrations;"
            "DROP TABLE IF EXISTS players;"
            "DROP FUNCTION IF EXISTS assert_username_ci_matches();", err))
        return false;
    if (!db.run_script(read_file(source_path("src/storage/schema_phase7.sql")), err))
        return false;
    bool applied = false;
    if (!db.apply_migration("0001_phase8_game_persistence",
            read_file(source_path("src/storage/migrations/0001_phase8_game_persistence.sql")),
            applied, err)) return false;
    if (!db.apply_migration("0002_phase9_3_cheat_reports",
            read_file(source_path("src/storage/migrations/0002_phase9_3_cheat_reports.sql")),
            applied, err)) return false;
    if (!db.apply_migration("0003_phase9_4_tournaments",
            read_file(source_path("src/storage/migrations/0003_phase9_4_tournaments.sql")),
            applied, err)) return false;
    return true;
}

int64_t insert_player(Database& db, const std::string& name, int elo = 1200) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto r = db.exec(
        "INSERT INTO players(username, username_ci, password_hash, elo_rating)"
        " VALUES($1,$2,$3,$4) RETURNING id",
        {Param::text(name), Param::text(lower),
         Param::text("hash"), Param::int64(elo)});
    if (!r.ok || r.empty()) return -1;
    return std::stoll(r.first().at(0));
}

// Run the 8-player 4-round Swiss to completion. `deciders` maps
// (round → white_wins?) — used to walk pairings in a predictable order.
struct TourneyOutcome {
    bool                       ok = false;
    std::vector<StoredPairing> all_pairings;
    StoredTournament           final_state;
};
TourneyOutcome run_full_8_player_swiss(Database& db,
                                       const std::vector<int64_t>& players,
                                       int64_t creator) {
    TourneyOutcome out;

    auto cr = create_tournament(db, "Test Swiss", /*rounds=*/4,
                                /*tc_init=*/300000, /*tc_inc=*/3000, creator);
    if (!cr.ok) return out;

    for (int64_t pid : players) {
        auto j = add_participant(db, cr.id, pid, /*elo=*/1500);
        if (!j.ok) return out;
    }

    TournamentManager tm(db);
    auto s = tm.start(cr.id, creator);
    if (!s.ok) return out;

    // Play through rounds. Each round, walk that round's pairings and
    // record "1-0" — the deterministic decider. Skip byes (already
    // scored during pairing).
    for (int round = 1; round <= 4; ++round) {
        auto pairings = get_pairings_for_round(db, cr.id, round);
        if (pairings.empty()) return out;
        for (const auto& p : pairings) {
            if (p.result != "pending") continue;
            auto r = tm.report_result(p.id, "1-0");
            if (!r.ok) return out;
        }
    }

    out.all_pairings = get_pairings(db, cr.id);
    auto final = find_tournament(db, cr.id);
    if (!final) return out;
    out.final_state = *final;
    out.ok = true;
    return out;
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 9.4 — Swiss tournament system\n";
    std::cout << "========================================\n";

    run_algorithm_tests();

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL not set — skipping DB & end-to-end layers.\n";
        std::cout << "\n========================================\n";
        std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed\n";
        return (g_failed > 0) ? 1 : 0;
    }

    Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "  connect failed: " << err << '\n';
        return 1;
    }
    if (!prepare_schema(db)) { std::cerr << "  schema prep failed\n"; return 1; }

    // ────────────────────────────────────────────────────────
    // Layer 2 — tournament_repo
    // ────────────────────────────────────────────────────────
    std::cout << "\n=== Layer 2: tournament_repo ===\n";

    int64_t alice_id = insert_player(db, "Alice", 1500);
    int64_t bob_id   = insert_player(db, "Bob",   1400);
    if (alice_id < 0 || bob_id < 0) return 1;

    int64_t created_tid = 0;
    run_test("create_tournament writes and returns an id", [&] {
        auto cr = create_tournament(db, "Repo Test", 3, 180000, 2000, alice_id);
        if (!cr.ok) return false;
        created_tid = cr.id;
        return created_tid > 0;
    });

    run_test("find_tournament reads back every field", [&] {
        auto t = find_tournament(db, created_tid);
        if (!t) return false;
        return t->name == "Repo Test"
            && t->format == "swiss"
            && t->rounds == 3
            && t->current_round == 0
            && t->time_control_initial_ms == 180000
            && t->time_control_increment_ms == 2000
            && t->status == "registration"
            && t->created_by == alice_id;
    });

    run_test("add_participant is idempotent on duplicate join", [&] {
        auto a = add_participant(db, created_tid, alice_id, 1500);
        auto b = add_participant(db, created_tid, alice_id, 1500);
        if (!a.ok || !b.ok) return false;
        auto ps = get_participants(db, created_tid);
        return ps.size() == 1 && ps[0].player_id == alice_id;
    });

    run_test("insert_pairing stores a bye with NULL black + result='bye'", [&] {
        auto ir = insert_pairing(db, created_tid, /*round=*/1,
                                 alice_id, std::nullopt, "bye");
        if (!ir.ok) return false;
        auto ps = get_pairings(db, created_tid);
        if (ps.size() != 1) return false;
        return !ps[0].black_player_id.has_value()
            && ps[0].result == "bye"
            && ps[0].round == 1;
    });

    run_test("set_pairing_result flips pending → 1-0", [&] {
        // Add bob and a real match pairing. Use round 2 so it does not
        // collide with the round-1 bye row inserted above on the
        // (tournament_id, round, white_player_id) UNIQUE.
        auto j = add_participant(db, created_tid, bob_id, 1400);
        if (!j.ok) return false;
        auto ir = insert_pairing(db, created_tid, /*round=*/2,
                                 alice_id, bob_id, "pending");
        if (!ir.ok) return false;
        if (!set_pairing_result(db, ir.id, "1-0")) return false;
        auto ps = get_pairings(db, created_tid);
        for (const auto& p : ps) if (p.id == ir.id) return p.result == "1-0";
        return false;
    });

    // ────────────────────────────────────────────────────────
    // Layer 3 — end-to-end 8-player, 4-round Swiss
    // ────────────────────────────────────────────────────────
    std::cout << "\n=== Layer 3: 8-player 4-round Swiss e2e ===\n";

    // Wipe the schema again so the layer-3 tests start from a clean state.
    if (!prepare_schema(db)) { std::cerr << "  schema reset failed\n"; return 1; }

    std::vector<int64_t> players;
    const char* names[] = {"P1","P2","P3","P4","P5","P6","P7","P8"};
    for (int i = 0; i < 8; ++i) {
        players.push_back(insert_player(db, names[i], 1600 - i * 30));
    }
    // The creator of the tournament must also be one of the players'
    // records (or any real player). Use the first as creator.
    const int64_t creator = players[0];

    TourneyOutcome out;
    run_test("full 8-player 4-round Swiss completes end-to-end", [&] {
        out = run_full_8_player_swiss(db, players, creator);
        return out.ok
            && out.final_state.status == "completed"
            && out.final_state.current_round == 4;
    });

    run_test("no rematches across the full event", [&] {
        std::set<PlayerPair> seen;
        for (const auto& p : out.all_pairings) {
            if (!p.black_player_id.has_value()) continue;
            PlayerPair pp(p.white_player_id, *p.black_player_id);
            if (seen.count(pp)) return false;
            seen.insert(pp);
        }
        return true;
    });

    run_test("every round has exactly 4 pairings", [&] {
        int per_round[5] = {0,0,0,0,0};
        for (const auto& p : out.all_pairings) {
            if (p.round >= 1 && p.round <= 4) ++per_round[p.round];
        }
        return per_round[1] == 4 && per_round[2] == 4
            && per_round[3] == 4 && per_round[4] == 4;
    });

    run_test("every pairing has a decisive result (no bye — 8 is even)", [&] {
        for (const auto& p : out.all_pairings) {
            if (p.result == "pending") return false;
            if (p.result == "bye") return false;
        }
        return true;
    });

    run_test("get_state returns standings sorted best-first", [&] {
        // Rebuild manager to read final state.
        TournamentManager tm(db);
        auto st = tm.get_state(out.final_state.id);
        if (!st) return false;
        // Standings size = 8; each score in [0..4].
        if (st->standings.size() != 8) return false;
        for (size_t i = 1; i < st->standings.size(); ++i) {
            if (st->standings[i-1].score < st->standings[i].score) return false;
        }
        return true;
    });

    run_test("report_result rejects a different result on a decided pairing", [&] {
        // Find any decisive pairing; try to set it to something else.
        int64_t target = 0;
        for (const auto& p : out.all_pairings) {
            if (p.result == "1-0") { target = p.id; break; }
        }
        if (!target) return false;

        TournamentManager tm(db);
        auto r = tm.report_result(target, "0-1");
        return !r.ok && r.error == "result_already_recorded";
    });

    run_test("report_result is idempotent on the same result", [&] {
        int64_t target = 0;
        for (const auto& p : out.all_pairings) {
            if (p.result == "1-0") { target = p.id; break; }
        }
        if (!target) return false;

        TournamentManager tm(db);
        auto r = tm.report_result(target, "1-0");
        return r.ok;
    });

    // Manager-level negative paths
    run_test("start refuses when caller is not creator", [&] {
        auto cr = create_tournament(db, "Perm Test", 2, 60000, 0, creator);
        if (!cr.ok) return false;
        (void)add_participant(db, cr.id, creator, 1500);
        (void)add_participant(db, cr.id, players[1], 1500);
        TournamentManager tm(db);
        auto r = tm.start(cr.id, players[1]);
        return !r.ok && r.error == "not_creator";
    });

    run_test("start refuses when < 2 participants", [&] {
        auto cr = create_tournament(db, "Empty Test", 2, 60000, 0, creator);
        if (!cr.ok) return false;
        (void)add_participant(db, cr.id, creator, 1500);
        TournamentManager tm(db);
        auto r = tm.start(cr.id, creator);
        return !r.ok && r.error == "not_enough_players";
    });

    run_test("join rejected after tournament left registration", [&] {
        // Reuse the completed 8-player tournament.
        TournamentManager tm(db);
        auto r = tm.join(out.final_state.id, alice_id, 1500);
        return !r.ok && r.error == "tournament_not_in_registration";
    });

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed\n";
    return (g_failed > 0) ? 1 : 0;
}
