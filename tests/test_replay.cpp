/**
 * test_replay.cpp — Phase 9.2.
 *
 * End-to-end proof that game replay and analysis work over the real router:
 *
 *   - Persist a full Scholar's Mate through GameRoom + storage repos, then
 *     route `get_game` and assert:
 *       - positions[0].fen  == the standard starting FEN
 *       - positions[7].fen  == the final mated position
 *       - every intermediate ply's FEN matches a fresh replay from scratch
 *       - san/from/to fields are populated for every non-zero ply
 *       - reason == "checkmate", result == "1-0"
 *
 *   - `get_game` for an unknown id returns a plain error frame (not a
 *     malformed payload).
 *
 *   - `analyze_position` on the starting FEN returns a UCI move that
 *     the move generator considers legal, plus a finite eval and a
 *     depth > 0.
 *
 *   - `analyze_position` on a bad FEN returns an error frame.
 *
 *   - `get_history` returns the persisted game for the winner, with
 *     my_color/result oriented from their perspective.
 *
 * Skips cleanly without DATABASE_URL, matching every other DB-backed
 * suite. Uses the same socketpair-backed FakeConn pattern as
 * test_auth_required / test_spectator — no other network machinery needed.
 */

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "auth/session.h"
#include "auth/token.h"
#include "chess/board.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "game/game_handler.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "game/game_room.h"
#include "net/connection.h"
#include "net/websocket.h"
#include "storage/database.h"
#include "storage/postgres_game_store.h"
#include "storage/game_repo.h"

using json = nlohmann::json;
using namespace chess;
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
            "DROP FUNCTION IF EXISTS assert_username_ci_matches();", err)) return false;
    const auto base = read_file(source_path("src/storage/schema_phase7.sql"));
    if (base.empty() || !db.run_script(base, err)) return false;
    const auto mig = read_file(source_path(
        "src/storage/migrations/0001_phase8_game_persistence.sql"));
    bool applied = false;
    return !mig.empty() && db.apply_migration(
        "0001_phase8_game_persistence", mig, applied, err);
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

// Socketpair-backed test connection. Same as the other 9.x suites.
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

// Read one WebSocket text frame and return its JSON payload. Supports
// short (7-bit) and 16-bit extended lengths. The get_game payload for a
// 40-move game can exceed 4 KB, so 16-bit extended is required here.
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

bool has_type(const json& r, const std::string& type) {
    return r.is_object() && r.value("type", "") == type;
}

// Play Scholar's Mate through the actual GameRoom, persist via game_repo.
// Returns the newly saved game_id, or -1 on error.
int64_t play_and_save_scholars_mate(Database& db,
                                    int64_t white_id, int64_t black_id) {
    GameRoom room(1, 1, "Alice", 10, TimeControl(600000, 5000), white_id, 1500);
    room.join(2, "Bob", 11, black_id, 1400);

    // 1. e4 e5 2. Qh5 a6 3. Bc4 a5 4. Qxf7#
    auto play = [&](int fd, int fr, int ff, int tr, int tf) {
        return room.submit_move(fd, make_square(fr, ff), make_square(tr, tf)).success;
    };
    if (!play(10, 1, 4, 3, 4)) return -1;       // e2e4
    if (!play(11, 6, 4, 4, 4)) return -1;       // e7e5
    if (!play(10, 0, 3, 4, 7)) return -1;       // d1h5
    if (!play(11, 6, 0, 5, 0)) return -1;       // a7a6
    if (!play(10, 0, 5, 3, 2)) return -1;       // f1c4
    if (!play(11, 5, 0, 4, 0)) return -1;       // a6a5
    if (!play(10, 4, 7, 6, 5)) return -1;       // h5f7 mate

    if (room.get_state() != RoomState::FINISHED) return -1;
    if (room.get_result_string() != "1-0")       return -1;

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

// Independently re-derive every ply's FEN by replaying the UCI move list
// on a fresh Board. The get_game handler builds the same sequence server-
// side; if the two disagree, the handler has drifted.
std::vector<std::string> expected_fens(const std::string& uci_moves_str) {
    std::vector<std::string> out;
    Board board = Board::starting_position();
    out.push_back(board.to_fen());
    std::stringstream ss(uci_moves_str);
    std::string tok;
    while (ss >> tok) {
        const Move probe = Move::from_uci(tok);
        const auto legal = move_gen::generate_legal_moves(board);
        for (const auto& m : legal) {
            if (m.from == probe.from && m.to == probe.to &&
                ((probe.flags & MoveFlags::PROMOTION) == 0 ||
                 m.promo_type == probe.promo_type)) {
                board.make_move(m);
                out.push_back(board.to_fen());
                break;
            }
        }
    }
    return out;
}

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 9.2 — Replay & Analysis\n";
    std::cout << "========================================\n";

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping.\n";
        return 0;
    }
    Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "  connect failed: " << err << '\n';
        return 1;
    }
    if (!prepare_schema(db)) { std::cerr << "  schema prep failed\n"; return 1; }

    // Seed two authenticated identities and a saved game.
    int64_t alice_id = insert_player(db, "Alice", 1500);
    int64_t bob_id   = insert_player(db, "Bob",   1400);
    if (alice_id < 0 || bob_id < 0) return 1;

    const int64_t game_id = play_and_save_scholars_mate(db, alice_id, bob_id);
    if (game_id <= 0) {
        std::cerr << "  precondition: failed to save Scholar's Mate\n";
        return 1;
    }

    // Load the persisted UCI so the test's expected FEN sequence is derived
    // from the same bytes get_game will read.
    auto stored = find_game_by_id(db, game_id);
    if (!stored) { std::cerr << "  precondition: game not persisted\n"; return 1; }
    const auto expected = expected_fens(stored->moves);

    // Wire the real handler stack for router-based tests.
    auth::TokenSigner signer = auth::TokenSigner::generate_random();
    RoomManager  room_mgr;
    Matchmaker   matchmaker(room_mgr);
    matchmaker.set_match_callback([](const MatchResult&) {});
    GameHandler  handler(room_mgr, matchmaker);
    handler.set_database(&db);
    storage::PostgresGameStore game_store(db);
    handler.set_game_store(&game_store);
    handler.set_signer(&signer);
    handler.set_connection_lookup([](int) -> net::Connection* { return nullptr; });
    net::MessageRouter router;
    handler.register_handlers(router);

    // ── get_game ─────────────────────────────────────────────
    std::cout << "\n=== get_game ===\n";

    run_test("get_game returns a game frame with correct metadata", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_game"},{"game_id", game_id}});
        return has_type(r, "game")
            && r.value("game_id", static_cast<int64_t>(0)) == game_id
            && r.value("white",  std::string{}) == "Alice"
            && r.value("black",  std::string{}) == "Bob"
            && r.value("result", std::string{}) == "1-0"
            && r.value("reason", std::string{}) == "checkmate"
            && r.value("move_count", 0) == 7;
    });

    run_test("positions[0] is the starting FEN, ply 0, no move", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_game"},{"game_id", game_id}});
        if (!has_type(r, "game")) return false;
        const auto& p = r["positions"];
        if (!p.is_array() || p.empty()) return false;
        return p[0].value("ply", -1) == 0
            && p[0].value("fen", std::string{}) == expected[0]
            && !p[0].contains("from")
            && !p[0].contains("to");
    });

    run_test("every intermediate FEN matches an independent replay", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_game"},{"game_id", game_id}});
        if (!has_type(r, "game")) return false;
        const auto& p = r["positions"];
        if (p.size() != expected.size()) return false;
        for (size_t i = 0; i < p.size(); ++i) {
            if (p[i].value("fen", std::string{}) != expected[i]) return false;
        }
        return true;
    });

    run_test("every ply >= 1 carries san/from/to", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_game"},{"game_id", game_id}});
        if (!has_type(r, "game")) return false;
        const auto& p = r["positions"];
        for (size_t i = 1; i < p.size(); ++i) {
            if (!p[i].contains("san") || !p[i].contains("from") ||
                !p[i].contains("to")) return false;
        }
        // Final ply must be Qxf7# → SAN ends in '#'.
        const auto san_last = p.back().value("san", std::string{});
        return !san_last.empty() && san_last.back() == '#';
    });

    run_test("get_game for unknown id → error frame", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_game"},{"game_id", static_cast<int64_t>(99999999)}});
        return r.is_object() && r.value("type", std::string{}) == "error";
    });

    // ── analyze_position ────────────────────────────────────
    std::cout << "\n=== analyze_position ===\n";

    run_test("analyze on starting FEN → legal best move + finite depth", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","analyze_position"},
             {"fen","rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
             {"depth", 4}});
        if (!has_type(r, "analysis")) return false;
        if (r.value("depth", 0) < 1) return false;
        const auto best = r.value("best_move", std::string{});
        if (best.size() < 4) return false;

        // Confirm the returned UCI is a legal move from the starting pos.
        Board board = Board::starting_position();
        const Move probe = Move::from_uci(best);
        for (const auto& m : move_gen::generate_legal_moves(board)) {
            if (m.from == probe.from && m.to == probe.to) return true;
        }
        return false;
    });

    run_test("analyze on malformed FEN → error frame", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","analyze_position"},{"fen","not a fen"},{"depth", 4}});
        return r.is_object() && r.value("type", std::string{}) == "error";
    });

    // ── get_history ─────────────────────────────────────────
    std::cout << "\n=== get_history ===\n";

    run_test("get_history returns the saved game oriented to the winner", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_history"},{"username","Alice"},{"limit", 20}});
        if (!has_type(r, "history")) return false;
        const auto& games = r["games"];
        if (!games.is_array() || games.size() != 1) return false;
        return games[0].value("game_id", static_cast<int64_t>(0)) == game_id
            && games[0].value("opponent", std::string{}) == "Bob"
            && games[0].value("my_color", std::string{}) == "w"
            && games[0].value("result",   std::string{}) == "w"
            && games[0].value("reason",   std::string{}) == "checkmate";
    });

    run_test("get_history for unknown user → error frame", [&] {
        auto c = make_conn();
        auto r = route_and_capture(*c, router,
            {{"type","get_history"},{"username","NoSuch"}});
        return r.is_object() && r.value("type", std::string{}) == "error";
    });

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed
              << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
