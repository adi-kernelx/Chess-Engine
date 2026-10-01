/**
 * test_anti_cheat.cpp — Phase 9.3.
 *
 * Three layers of tests:
 *
 *   1. Pure-analyzer unit tests. No DB, no engine. Tight math + threshold
 *      checks on `analyze_side`.
 *
 *   2. Repo round-trip. Save an AnalysisReport via save_cheat_report, read
 *      it back through get_reports_for_game, confirm every field survives,
 *      including NaN → NULL → optional::has_value() == false.
 *
 *   3. End-to-end through the router. Persist a real game (Scholar's Mate,
 *      seven plies), then route `analyze_game` and confirm the returned
 *      cheat_report frame has the expected shape and a `cheat_reports`
 *      row exists in the database.
 *
 * DB-backed layers skip cleanly without DATABASE_URL. Layer 1 always runs.
 */

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "analysis/anti_cheat.h"
#include "analysis/cheat_report_repo.h"
#include "application/auth/identity_extractor.h"
#include "auth/token.h"
#include "protocol/request_pipeline.h"
#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "game/game_handler.h"
#include "game/game_room.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/connection.h"
#include "net/websocket.h"
#include "storage/database.h"
#include "storage/postgres_game_store.h"
#include "storage/postgres_player_queries.h"
#include "storage/game_repo.h"

using json = nlohmann::json;
using namespace chess;
using namespace chess::analysis;
using namespace chess::game;
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

// Build a PlyData for tests. Named args via a small helper because the
// combinatorics get tedious otherwise.
PlyData ply(int think_ms, int legal, bool matched, bool terminal = false) {
    PlyData p;
    p.think_time_ms    = think_ms;
    p.legal_move_count = legal;
    p.matched_engine   = matched;
    p.terminal_after   = terminal;
    return p;
}

// ============================================================
// Layer 1 — pure analyzer
// ============================================================

void run_analyzer_tests() {
    std::cout << "\n=== Layer 1: analyzer math ===\n";

    run_test("empty input → no verdict, no flags", [] {
        auto r = analyze_side({});
        return !r.flagged && r.plies_analyzed == 0
            && r.plies_matched_engine == 0 && r.reasons.empty();
    });

    run_test("below min_plies → never flagged, even on suspicious inputs", [] {
        // Constant times, 100% engine agreement — but only 5 plies.
        std::vector<PlyData> ps;
        for (int i = 0; i < 5; ++i) ps.push_back(ply(200, 20, true));
        auto r = analyze_side(ps);
        return !r.flagged && r.plies_analyzed == 5;
    });

    run_test("high engine agreement flags", [] {
        // 15 plies, 14 matched, variable times & complexity so only the
        // agreement signal trips.
        std::vector<PlyData> ps;
        for (int i = 0; i < 15; ++i) {
            const int t = 1000 + (i * 500);
            ps.push_back(ply(t, 10 + i, i != 3));  // 14/15 match
        }
        auto r = analyze_side(ps);
        // 14/15 = 93.3%, above 85 threshold.
        return r.flagged
            && r.plies_matched_engine == 14
            && r.engine_agreement_pct > 90.0
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "engine_agreement_high") != r.reasons.end();
    });

    run_test("low agreement, no flag on that signal", [] {
        // 15 plies, 6 matched (40%). Times vary so other signals stay clean.
        std::vector<PlyData> ps;
        for (int i = 0; i < 15; ++i) {
            const int t = 500 + (i * 700);
            ps.push_back(ply(t, 10 + i, i < 6));
        }
        auto r = analyze_side(ps);
        return !r.flagged
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "engine_agreement_high") == r.reasons.end();
    });

    run_test("low time-CV flags", [] {
        // 12 plies, times all within 5% of each other, complexity varies.
        std::vector<PlyData> ps;
        for (int i = 0; i < 12; ++i) {
            const int t = 1000 + (i % 3) * 20;   // 1000, 1020, 1040, 1000, …
            ps.push_back(ply(t, 15 + i, i < 3)); // low agreement
        }
        auto r = analyze_side(ps);
        return r.flagged
            && !std::isnan(r.time_cv) && r.time_cv < 0.10
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "time_cv_low") != r.reasons.end();
    });

    run_test("high time-CV does not flag on that signal", [] {
        // 12 plies, times swing across an order of magnitude.
        std::vector<PlyData> ps;
        const int times[] = {100, 5000, 200, 8000, 400, 6000,
                             150, 7500, 300, 4500, 250, 9000};
        for (int i = 0; i < 12; ++i) {
            ps.push_back(ply(times[i], 15 + i, i < 2));
        }
        auto r = analyze_side(ps);
        return std::find(r.reasons.begin(), r.reasons.end(),
                         "time_cv_low") == r.reasons.end();
    });

    run_test("no complexity correlation flags", [] {
        // 12 plies. Think times DO vary (defeats time_cv), but they are
        // uncorrelated with the complexity series — bot pattern.
        std::vector<PlyData> ps;
        const int times[]      = {200,  6000, 300,  5500, 250, 7000,
                                  180,  6500, 220,  4800, 260, 5700};
        const int complexity[] = {20,   10,   35,   15,   25,  8,
                                  40,   12,   28,   18,   22,  30};
        for (int i = 0; i < 12; ++i) {
            ps.push_back(ply(times[i], complexity[i], i < 2));
        }
        auto r = analyze_side(ps);
        // The times/complexity pairs are anticorrelated by construction;
        // Pearson will be negative, well below the 0.10 threshold.
        return r.flagged
            && !std::isnan(r.complexity_corr)
            && r.complexity_corr < 0.10
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "complexity_correlation_low") != r.reasons.end();
    });

    run_test("strong positive correlation does not flag", [] {
        // Times grow with complexity — the human pattern.
        std::vector<PlyData> ps;
        for (int i = 0; i < 12; ++i) {
            const int c = 8 + i * 2;
            const int t = 500 + c * 200;   // strictly increasing with c
            ps.push_back(ply(t, c, i < 3)); // low agreement
        }
        auto r = analyze_side(ps);
        return !r.flagged
            && !std::isnan(r.complexity_corr) && r.complexity_corr > 0.9;
    });

    run_test("terminal plies excluded from agreement denominator", [] {
        // 12 plies, one terminal. The terminal ply "matches" but must not
        // be counted — otherwise a single mating move inflates agreement.
        std::vector<PlyData> ps;
        for (int i = 0; i < 11; ++i) ps.push_back(ply(1000 + i * 300, 15 + i, false));
        ps.push_back(ply(2000, 12, true, /*terminal_after=*/true));
        auto r = analyze_side(ps);
        return r.plies_matched_engine == 0
            && !std::isnan(r.engine_agreement_pct)
            && r.engine_agreement_pct == 0.0;
    });

    run_test("all-terminal input → engine_agreement_pct NaN", [] {
        std::vector<PlyData> ps;
        for (int i = 0; i < 12; ++i) ps.push_back(ply(1000, 10, true, true));
        auto r = analyze_side(ps);
        return std::isnan(r.engine_agreement_pct)
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "engine_agreement_high") == r.reasons.end();
    });

    run_test("mean-of-zero times → time_cv NaN, no flag on it", [] {
        std::vector<PlyData> ps;
        for (int i = 0; i < 12; ++i) ps.push_back(ply(0, 15 + i, false));
        auto r = analyze_side(ps);
        // time_cv is NaN — must not fire the "time_cv_low" flag.
        return std::isnan(r.time_cv)
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "time_cv_low") == r.reasons.end();
    });

    run_test("constant-complexity input → correlation NaN, no flag on it", [] {
        // All positions have the same legal-move count. Correlation is
        // undefined (zero variance in X); we must not treat NaN as low.
        std::vector<PlyData> ps;
        for (int i = 0; i < 12; ++i) ps.push_back(ply(500 + i * 200, 20, false));
        auto r = analyze_side(ps);
        return std::isnan(r.complexity_corr)
            && std::find(r.reasons.begin(), r.reasons.end(),
                         "complexity_correlation_low") == r.reasons.end();
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
    if (!db.apply_migration("0004_lld4_completion_uuid",
            read_file(source_path("src/storage/migrations/0004_lld4_completion_uuid.sql")),
            applied, err)) return false;
    if (!db.apply_migration("0009_persist_unrated_ai_games",
            read_file(source_path("src/storage/migrations/0009_persist_unrated_ai_games.sql")),
            applied, err)) return false;
    return true;
}

int64_t insert_player(Database& db, const std::string& name, int elo = 1200) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    auto r = db.exec(
        "INSERT INTO players(username, username_ci, password_hash, elo_rating)"
        " VALUES($1,$2,$3,$4) RETURNING id",
        {Param::text(name), Param::text(lower),
         Param::text("hash"), Param::int64(elo)});
    if (!r.ok || r.empty()) return -1;
    return std::stoll(r.first().at(0));
}

// Same socketpair harness as test_replay.cpp — a fresh copy so this file
// stays self-contained, matching the discipline the other 9.x tests use.
struct FakeConn {
    net::Connection conn;
    int             peer_fd;
    FakeConn(int server_fd, int client_fd)
        : conn(server_fd, "127.0.0.1"), peer_fd(client_fd) {
        conn.set_upgraded(true);
    }
    ~FakeConn() { if (peer_fd >= 0) ::close(peer_fd); }
    FakeConn(const FakeConn&) = delete;
    FakeConn& operator=(const FakeConn&) = delete;
};

std::unique_ptr<FakeConn> make_conn() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return nullptr;
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    return std::make_unique<FakeConn>(fds[0], fds[1]);
}

std::string drain_and_read(FakeConn& fc) {
    while (fc.conn.has_data_to_write()) {
        int w = fc.conn.write_to_socket();
        if (w <= 0) break;
    }
    uint8_t hdr[10] = {};
    ssize_t n = ::recv(fc.peer_fd, hdr, 2, 0);
    if (n < 2) return "";
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        if (::recv(fc.peer_fd, hdr + 2, 2, 0) < 2) return "";
        len = (uint64_t(hdr[2]) << 8) | uint64_t(hdr[3]);
    } else if (len == 127) {
        if (::recv(fc.peer_fd, hdr + 2, 8, 0) < 8) return "";
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | hdr[2 + i];
    }
    std::string payload(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t k = ::recv(fc.peer_fd, payload.data() + got, len - got, 0);
        if (k <= 0) break;
        got += static_cast<size_t>(k);
    }
    payload.resize(got);
    return payload;
}

json route_and_capture(FakeConn& fc, net::MessageRouter& router, const json& msg) {
    router.route(fc.conn, msg.dump());
    const auto raw = drain_and_read(fc);
    if (raw.empty()) return {};
    try { return json::parse(raw); } catch (...) { return {}; }
}

// Play Scholar's Mate and persist it. Returns the game_id.
int64_t play_and_save_scholars_mate(Database& db,
                                    int64_t white_id, int64_t black_id) {
    GameRoom room(1, 1, "Alice", 10, TimeControl(600000, 5000), white_id, 1500);
    room.join(2, "Bob", 11, black_id, 1400);
    auto play = [&](int fd, int fr, int ff, int tr, int tf) {
        return room.submit_move(fd, make_square(fr, ff), make_square(tr, tf)).success;
    };
    if (!play(10, 1, 4, 3, 4)) return -1;
    if (!play(11, 6, 4, 4, 4)) return -1;
    if (!play(10, 0, 3, 4, 7)) return -1;
    if (!play(11, 6, 0, 5, 0)) return -1;
    if (!play(10, 0, 5, 3, 2)) return -1;
    if (!play(11, 5, 0, 4, 0)) return -1;
    if (!play(10, 4, 7, 6, 5)) return -1;
    if (room.get_state() != RoomState::FINISHED) return -1;

    CompletedGame g;
    g.white_id = white_id; g.black_id = black_id;
    g.white_elo = 1500; g.black_elo = 1400;
    g.result = "1-0"; g.termination = "checkmate";
    g.time_control = room.get_time_control().to_string();
    g.started_at = room.get_started_at_iso();
    g.ended_at   = room.get_ended_at_iso();
    const auto history = room.get_move_history();
    g.move_count = static_cast<int>(history.size());
    std::string moves;
    for (size_t i = 0; i < history.size(); ++i) {
        if (i > 0) moves += ' ';
        moves += history[i].move.to_uci();
        g.think_times.push_back({static_cast<int>(i + 1),
            (i % 2 == 0) ? white_id : black_id,
            history[i].think_time_ms});
    }
    g.moves = std::move(moves);
    const auto saved = save_completed_game(db, g);
    return saved.ok ? saved.game_id : -1;
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 9.3 — Anti-cheat analyzer\n";
    std::cout << "========================================\n";

    run_analyzer_tests();

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
    // Layer 2 — repo round-trip
    // ────────────────────────────────────────────────────────
    std::cout << "\n=== Layer 2: cheat_report_repo ===\n";

    int64_t alice_id = insert_player(db, "Alice", 1500);
    int64_t bob_id   = insert_player(db, "Bob",   1400);
    if (alice_id < 0 || bob_id < 0) return 1;

    const int64_t game_id = play_and_save_scholars_mate(db, alice_id, bob_id);
    if (game_id <= 0) { std::cerr << "  precondition: save failed\n"; return 1; }

    run_test("save_cheat_report writes and returns an id", [&] {
        AnalysisReport r;
        r.plies_analyzed = 15;
        r.plies_matched_engine = 6;
        r.engine_agreement_pct = 40.0;
        r.time_cv = 0.42;
        r.complexity_corr = 0.55;
        r.flagged = false;
        r.reasons = {};
        auto out = save_cheat_report(db, game_id, alice_id, "w", r);
        return out.ok && out.id > 0;
    });

    run_test("read back preserves every field", [&] {
        auto rows = get_reports_for_game(db, game_id);
        if (rows.size() != 1) return false;
        const auto& s = rows[0];
        return s.game_id == game_id
            && s.player_id == alice_id
            && s.side == "w"
            && s.plies_analyzed == 15
            && s.plies_matched_engine == 6
            && s.engine_agreement_pct.has_value()
            && std::abs(*s.engine_agreement_pct - 40.0) < 1e-9
            && s.time_cv.has_value()
            && std::abs(*s.time_cv - 0.42) < 1e-9
            && s.complexity_corr.has_value()
            && std::abs(*s.complexity_corr - 0.55) < 1e-9
            && !s.flagged
            && s.reasons.empty();
    });

    run_test("upsert on (game_id, player_id) refreshes rather than duplicates", [&] {
        AnalysisReport r;
        r.plies_analyzed = 20;
        r.plies_matched_engine = 18;
        r.engine_agreement_pct = 90.0;
        r.time_cv = 0.08;
        r.complexity_corr = -0.10;
        r.flagged = true;
        r.reasons = {"engine_agreement_high", "time_cv_low"};
        auto out = save_cheat_report(db, game_id, alice_id, "w", r);
        if (!out.ok) return false;
        auto rows = get_reports_for_game(db, game_id);
        if (rows.size() != 1) return false;
        return rows[0].flagged
            && rows[0].plies_matched_engine == 18
            && rows[0].reasons.size() == 2
            && rows[0].reasons[0] == "engine_agreement_high"
            && rows[0].reasons[1] == "time_cv_low";
    });

    run_test("NaN fields stored as SQL NULL and read back as empty optional", [&] {
        AnalysisReport r;
        r.plies_analyzed = 12;
        r.plies_matched_engine = 0;
        r.engine_agreement_pct = std::numeric_limits<double>::quiet_NaN();
        r.time_cv              = std::numeric_limits<double>::quiet_NaN();
        r.complexity_corr      = std::numeric_limits<double>::quiet_NaN();
        r.flagged = false;
        auto out = save_cheat_report(db, game_id, bob_id, "b", r);
        if (!out.ok) return false;
        auto rows = get_reports_for_game(db, game_id);
        // Ordered by side ASC — 'b' < 'w', so Bob's row is index 0.
        if (rows.size() != 2) return false;
        const auto& bob_row = rows[0];
        return bob_row.side == "b"
            && !bob_row.engine_agreement_pct.has_value()
            && !bob_row.time_cv.has_value()
            && !bob_row.complexity_corr.has_value();
    });

    run_test("get_recent_flagged returns flagged rows", [&] {
        auto rows = get_recent_flagged(db, 10);
        // Alice's upserted row (from above) is flagged.
        bool found_alice = false;
        for (const auto& s : rows) {
            if (s.game_id == game_id && s.player_id == alice_id && s.flagged) {
                found_alice = true; break;
            }
        }
        return found_alice;
    });

    // ────────────────────────────────────────────────────────
    // Layer 3 — end-to-end via the router
    // ────────────────────────────────────────────────────────
    std::cout << "\n=== Layer 3: analyze_game via router ===\n";

    // Rebuild the DB fresh so the earlier upsert doesn't pollute assertions
    // (the handler will insert its own row for this game).
    if (!prepare_schema(db)) { std::cerr << "  reschema failed\n"; return 1; }
    alice_id = insert_player(db, "Alice", 1500);
    bob_id   = insert_player(db, "Bob",   1400);
    const int64_t game_id2 = play_and_save_scholars_mate(db, alice_id, bob_id);
    if (game_id2 <= 0) return 1;

    auth::TokenSigner signer = auth::TokenSigner::generate_random();
    RoomManager  room_mgr;
    Matchmaker   matchmaker(room_mgr);
    matchmaker.set_match_callback([](const MatchResult&) {});
    GameHandler  handler(room_mgr, matchmaker);
    handler.set_database(&db);
    storage::PostgresGameStore game_store(db);
    storage::PostgresPlayerQueries player_queries(db);
    handler.set_game_store(&game_store);
    handler.set_player_queries(&player_queries);
    handler.set_connection_lookup([](int) -> net::Connection* { return nullptr; });
    net::MessageRouter router;
    application::auth::IdentityExtractor extractor(&db, &signer);
    protocol::RequestPipeline pipeline(
        &extractor, nullptr,
        [](int) -> net::Connection* { return nullptr; });
    handler.register_handlers(pipeline);
    pipeline.install_on_router(router);

    run_test("analyze_game returns cheat_report frame with white+black blocks", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","analyze_game"},{"game_id", game_id2}});
        if (!r.is_object()) return false;
        if (r.value("type", std::string{}) != "cheat_report") return false;
        if (r.value("game_id", static_cast<int64_t>(0)) != game_id2) return false;
        if (!r.contains("white") || !r["white"].is_object()) return false;
        if (!r.contains("black") || !r["black"].is_object()) return false;
        // Scholar's Mate is 7 plies: white gets plies 1,3,5,7 (4);
        // black gets plies 2,4,6 (3).
        return r["white"].value("plies_analyzed", 0) == 4
            && r["black"].value("plies_analyzed", 0) == 3;
    });

    run_test("terminal ply excluded from white's agreement denominator", [&] {
        // White's plies are 4 total; the last one (Qxf7#) is terminal.
        // The engine-agreement denominator on White must therefore be 3.
        // We assert that plies_matched_engine <= 3 (it can be 0 or up to 3
        // depending on engine luck at depth 6). The stronger claim is
        // engine_agreement_pct = plies_matched_engine / 3 * 100 when the
        // handler correctly excludes the terminal ply.
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","analyze_game"},{"game_id", game_id2}});
        if (!r.is_object() || !r.contains("white")) return false;
        const int matched = r["white"].value("plies_matched_engine", -1);
        const auto pct_j  = r["white"]["engine_agreement_pct"];
        // pct is either a number or null (all-terminal), and matched∈[0,3].
        if (matched < 0 || matched > 3) return false;
        if (pct_j.is_null()) return matched == 0;
        const double pct = pct_j.get<double>();
        // Denominator is 3 (4 plies − 1 terminal). Match ratio = matched/3.
        return std::abs(pct - (100.0 * matched / 3.0)) < 1e-9;
    });

    run_test("analyze_game persists a row per side (ordered by side ASC)", [&] {
        auto c = make_conn();
        (void)route_and_capture(*c, router,
            {{"type","analyze_game"},{"game_id", game_id2}});
        auto rows = get_reports_for_game(db, game_id2);
        // 'b' < 'w', so black comes first.
        return rows.size() == 2
            && rows[0].side == "b"
            && rows[1].side == "w"
            && rows[0].player_id == bob_id
            && rows[1].player_id == alice_id;
    });

    run_test("analyze_game upsert on repeated invocation", [&] {
        auto c = make_conn();
        (void)route_and_capture(*c, router,
            {{"type","analyze_game"},{"game_id", game_id2}});
        (void)route_and_capture(*c, router,
            {{"type","analyze_game"},{"game_id", game_id2}});
        auto rows = get_reports_for_game(db, game_id2);
        return rows.size() == 2;
    });

    run_test("analyze_game on unknown id returns error, no rows written", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","analyze_game"},{"game_id", 999999}});
        return r.is_object()
            && r.value("type", std::string{}) == "error"
            && get_reports_for_game(db, 999999).empty();
    });

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed\n";
    std::cout << "========================================\n";
    return (g_failed > 0) ? 1 : 0;
}
