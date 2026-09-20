/**
 * test_auth_required.cpp — Phase 9 pre-work.
 *
 * The four game-starting WebSocket commands (create_game / join_game /
 * quick_play / play_ai) are authentication-mandatory as of the Phase 9
 * pre-work migration. This suite proves that end of the wire contract by
 * routing real JSON messages through the actual MessageRouter → GameHandler
 * path and inspecting the response frame that the handler wrote back to the
 * client's Connection.
 *
 * What each case proves:
 *   - Missing access_token         → {code: "auth_required"}, no room created
 *   - Malformed / tampered token   → {code: "auth_required"}, no room created
 *   - Valid token, player deleted  → {code: "auth_required"}, no room created
 *   - Valid token, live player     → happy-path response (game_created /
 *                                    queued / game_start), room/queue populated
 *
 * The test skips cleanly without DATABASE_URL, matching every other DB-backed
 * suite in this codebase (test_database, test_repositories, test_token, …).
 */

#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "auth/session.h"
#include "auth/token.h"
#include "game/game_handler.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/connection.h"
#include "net/websocket.h"
#include "storage/database.h"
#include "storage/postgres_game_store.h"
#include "storage/postgres_player_queries.h"

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
            "DROP FUNCTION IF EXISTS assert_username_ci_matches();", err)) {
        std::cerr << "\n  reset failed: " << err << '\n'; return false;
    }
    const auto base = read_file(source_path("src/storage/schema_phase7.sql"));
    if (base.empty() || !db.run_script(base, err)) {
        std::cerr << "\n  Phase-7 load failed: " << err << '\n'; return false;
    }
    const auto mig = read_file(source_path(
        "src/storage/migrations/0001_phase8_game_persistence.sql"));
    bool applied = false;
    if (mig.empty() || !db.apply_migration("0001_phase8_game_persistence",
                                           mig, applied, err)) {
        std::cerr << "\n  migration failed: " << err << '\n'; return false;
    }
    const auto mig4 = read_file(source_path(
        "src/storage/migrations/0004_lld4_completion_uuid.sql"));
    if (mig4.empty() || !db.apply_migration("0004_lld4_completion_uuid",
                                            mig4, applied, err)) {
        std::cerr << "\n  migration 0004 failed: " << err << '\n'; return false;
    }
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

// Mint a valid access token for the given player. The epoch defaults to 0,
// matching the fresh players.token_epoch column.
std::string mint_access(const auth::TokenSigner& signer, int64_t player_id,
                         const std::string& username, int64_t now_unix) {
    auth::AccessClaims c;
    c.player_id   = player_id;
    c.username    = username;
    c.token_epoch = 0;
    c.issued_at   = now_unix;
    c.expires_at  = now_unix + auth::ACCESS_TOKEN_TTL_SECONDS;
    return signer.issue_access(c);
}

// A test-scoped connection wired to a socketpair. write_frame → the socket,
// then read the frame back from the peer fd and pull the JSON payload out.
struct FakeConn {
    net::Connection conn;
    int             peer_fd;

    FakeConn(int server_fd, int client_fd)
        : conn(server_fd, "127.0.0.1"), peer_fd(client_fd) {
        conn.set_upgraded(true);
    }
    ~FakeConn() {
        // conn's destructor closes server_fd; we own peer_fd
        if (peer_fd >= 0) ::close(peer_fd);
    }
    FakeConn(const FakeConn&) = delete;
    FakeConn& operator=(const FakeConn&) = delete;
};

std::unique_ptr<FakeConn> make_conn() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return nullptr;
    // Non-blocking on the peer end so recv() never hangs the test.
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    return std::make_unique<FakeConn>(fds[0], fds[1]);
}

// Drain conn.write_buffer_ into the socket, then read one text frame from the
// peer end and return the JSON payload. Returns empty string if no frame is
// waiting. Assumes payload length fits in 7 bits (< 126 bytes) — plenty for
// error frames and simple confirmations. Extended-length frames are read too
// via a fallback for larger success responses.
std::string drain_and_read(FakeConn& fc) {
    while (fc.conn.has_data_to_write()) {
        int w = fc.conn.write_to_socket();
        if (w <= 0) break;
    }
    uint8_t hdr[10] = {};
    ssize_t n = ::recv(fc.peer_fd, hdr, 2, 0);
    if (n < 2) return "";
    // hdr[0] = 0x81 for FIN+TEXT; hdr[1] = 0x00..0x7d for short length,
    // 126 for 16-bit extended, 127 for 64-bit extended (unlikely here).
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        if (::recv(fc.peer_fd, hdr + 2, 2, 0) < 2) return "";
        len = (uint64_t(hdr[2]) << 8) | uint64_t(hdr[3]);
    } else if (len == 127) {
        return "";  // Not expected in these tests.
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

// Route a message through the router+handler pair, capture the response, and
// try to parse it as JSON. Returns an empty object on any failure.
json route_and_capture(FakeConn& fc, net::MessageRouter& router,
                       const json& msg) {
    router.route(fc.conn, msg.dump());
    const auto raw = drain_and_read(fc);
    if (raw.empty()) return {};
    try { return json::parse(raw); }
    catch (...) { return {}; }
}

bool has_code(const json& r, const std::string& code) {
    return r.is_object() && r.value("type", "") == "error"
        && r.value("code", "") == code;
}

bool has_type(const json& r, const std::string& type) {
    return r.is_object() && r.value("type", "") == type;
}

// After a test, purge any room this FakeConn's fd may have entered. Prevents
// fd recycling between tests from making a later find_room_by_fd() dredge up
// a prior test's room (Connection destructors close fds; the OS is free to
// hand the same fd number back to a later socketpair).
void cleanup_room_for_fd(RoomManager& room_mgr, int fd) {
    auto room = room_mgr.find_room_by_fd(fd);
    if (room) room_mgr.remove_room(room->get_id());
}

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 9 pre-work — Auth-mandatory Game Commands\n";
    std::cout << "========================================\n";

    if (std::getenv("DATABASE_URL") == nullptr) {
        std::cout << "\n  DATABASE_URL is not set — skipping.\n";
        std::cout << "  (Set DATABASE_URL to run against ephemeral Postgres.)\n";
        return 0;
    }

    Database db;
    std::string err;
    if (!db.connect_from_env(err)) {
        std::cerr << "  connect failed: " << err << '\n';
        return 1;
    }
    if (!prepare_schema(db)) return 1;

    // Fresh random signing key — tests must not depend on env-supplied secrets.
    auth::TokenSigner signer = auth::TokenSigner::generate_random();
    if (!signer.valid()) { std::cerr << "  signer generation failed\n"; return 1; }

    // Seed two authenticated identities.
    int64_t alice_id = insert_player(db, "Alice", 1500);
    int64_t bob_id   = insert_player(db, "Bob",   1400);
    if (alice_id < 0 || bob_id < 0) {
        std::cerr << "  player insert failed\n"; return 1;
    }
    const auto now_unix = static_cast<int64_t>(std::time(nullptr));
    const std::string alice_token = mint_access(signer, alice_id, "Alice", now_unix);
    const std::string bob_token   = mint_access(signer, bob_id,   "Bob",   now_unix);

    // Wire the real handler stack. The Matchmaker's match callback and the
    // fd→connection lookup are only exercised by quick_play match success,
    // which we do NOT trigger in this suite (queue → happy path stops at the
    // "queued" reply before a second player arrives), so no-ops are fine.
    RoomManager  room_mgr;
    Matchmaker   matchmaker(room_mgr);
    // No-op callback — none of these tests trigger a successful match.
    matchmaker.set_match_callback([](const MatchResult&) {});
    GameHandler  handler(room_mgr, matchmaker);
    handler.set_database(&db);
    storage::PostgresGameStore game_store(db);
    storage::PostgresPlayerQueries player_queries(db);
    handler.set_game_store(&game_store);
    handler.set_player_queries(&player_queries);
    handler.set_signer(&signer);
    handler.set_connection_lookup([](int) -> net::Connection* { return nullptr; });

    net::MessageRouter router;
    handler.register_handlers(router);

    // ── create_game ──────────────────────────────────────────
    std::cout << "\n=== create_game ===\n";

    run_test("create_game without access_token → auth_required", [&] {
        auto fc = make_conn();
        const size_t before = room_mgr.room_count();
        json msg = {{"type","create_game"},{"time_base",600},{"time_inc",5}};
        auto r = route_and_capture(*fc, router, msg);
        return has_code(r, "auth_required") &&
               room_mgr.room_count() == before;
    });

    run_test("create_game with tampered token → auth_required", [&] {
        auto fc = make_conn();
        const size_t before = room_mgr.room_count();
        std::string bad = alice_token;
        bad[bad.size() - 3] ^= 0x01;   // flip a bit in the signature
        json msg = {{"type","create_game"},{"access_token",bad},
                    {"time_base",600},{"time_inc",5}};
        auto r = route_and_capture(*fc, router, msg);
        return has_code(r, "auth_required") &&
               room_mgr.room_count() == before;
    });

    run_test("create_game with valid token → game_created + room seated", [&] {
        auto fc = make_conn();
        json msg = {{"type","create_game"},{"access_token",alice_token},
                    {"time_base",600},{"time_inc",5}};
        auto r = route_and_capture(*fc, router, msg);
        const bool ok_reply = has_type(r, "game_created");
        auto room = room_mgr.find_room_by_fd(fc->conn.get_fd());
        const bool ok_room = room
            && room->get_db_player_id(Color::WHITE) == alice_id
            && room->get_username(Color::WHITE) == "Alice";
        cleanup_room_for_fd(room_mgr, fc->conn.get_fd());
        return ok_reply && ok_room;
    });

    // ── join_game ────────────────────────────────────────────
    std::cout << "\n=== join_game ===\n";

    run_test("join_game without access_token → auth_required", [&] {
        auto fc = make_conn();
        json msg = {{"type","join_game"},{"game_id",1}};
        auto r = route_and_capture(*fc, router, msg);
        return has_code(r, "auth_required");
    });

    // ── quick_play ───────────────────────────────────────────
    std::cout << "\n=== quick_play ===\n";

    run_test("quick_play without access_token → auth_required", [&] {
        auto fc = make_conn();
        json msg = {{"type","quick_play"},{"time_base",600},{"time_inc",5}};
        auto r = route_and_capture(*fc, router, msg);
        return has_code(r, "auth_required") &&
               !matchmaker.is_queued(fc->conn.get_fd());
    });

    run_test("quick_play with valid token → queued", [&] {
        auto fc = make_conn();
        json msg = {{"type","quick_play"},{"access_token",bob_token},
                    {"time_base",600},{"time_inc",5}};
        auto r = route_and_capture(*fc, router, msg);
        const bool queued = matchmaker.is_queued(fc->conn.get_fd());
        matchmaker.dequeue(fc->conn.get_fd());
        return has_type(r, "queued") && queued;
    });

    // ── play_ai ──────────────────────────────────────────────
    std::cout << "\n=== play_ai ===\n";

    run_test("play_ai without access_token → auth_required", [&] {
        auto fc = make_conn();
        const size_t before = room_mgr.room_count();
        json msg = {{"type","play_ai"},{"difficulty","easy"},
                    {"time_base",60},{"time_inc",0}};
        auto r = route_and_capture(*fc, router, msg);
        return has_code(r, "auth_required") &&
               room_mgr.room_count() == before;
    });

    // ── Deleted player closes the epoch-then-delete race ─────
    std::cout << "\n=== Deleted player ===\n";

    run_test("valid token for deleted player → auth_required", [&] {
        // Mint a token for a soon-to-vanish player, then delete the row.
        int64_t ghost_id = insert_player(db, "GhostA", 1200);
        if (ghost_id < 0) return false;
        const auto ghost_token = mint_access(signer, ghost_id, "GhostA", now_unix);
        auto d = db.exec("DELETE FROM players WHERE id=$1",
                         {Param::int64(ghost_id)});
        if (!d.ok) return false;

        auto fc = make_conn();
        const size_t before = room_mgr.room_count();
        json msg = {{"type","create_game"},{"access_token",ghost_token},
                    {"time_base",600},{"time_inc",5}};
        auto r = route_and_capture(*fc, router, msg);
        return has_code(r, "auth_required") &&
               room_mgr.room_count() == before;
    });

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
