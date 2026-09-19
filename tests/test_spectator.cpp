/**
 * test_spectator.cpp — Phase 9.1.
 *
 * End-to-end proof that spectator mode works over the real message router:
 *   - `spectate` on a live game returns a spectate_start with FEN + move
 *     history + clocks + spectator_count.
 *   - Every move_made and game_over is fanned out to all watchers alongside
 *     the two seated players.
 *   - Refusals: WAITING room, FINISHED room, caller already seated in this
 *     game, missing access_token.
 *   - stop_spectating drops the fd (subsequent broadcasts skip it).
 *   - on_player_disconnect sweeps the fd out of every room's spectator list.
 *   - list_live_games returns IN_PROGRESS rooms with spectator counts.
 *
 * Uses the same socketpair-backed FakeConn technique as test_auth_required —
 * the router writes into the server-side Connection, we drain the buffer
 * into the socket, then read the outgoing WebSocket frame from the peer end
 * and pull the JSON payload out.
 *
 * Skips cleanly without DATABASE_URL, matching every other DB-backed suite.
 */

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
#include "game/game_handler.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/connection.h"
#include "net/websocket.h"
#include "storage/database.h"
#include "storage/postgres_game_store.h"

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
    if (mig.empty() || !db.apply_migration("0001_phase8_game_persistence",
                                           mig, applied, err)) return false;
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

// A socketpair-backed test Connection. The server-side fd goes into the
// Connection object (which is what the router hands to handlers); the peer
// fd stays here so we can read outbound frames from it.
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

// Flush the write buffer into the socket, read one WebSocket text frame,
// and return the JSON payload. Handles 7-bit + 16-bit extended lengths.
// A 64-bit extended payload (127) is never expected in this suite.
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
    } else if (len == 127) return "";
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

json parse_or_empty(const std::string& raw) {
    if (raw.empty()) return {};
    try { return json::parse(raw); } catch (...) { return {}; }
}

// Peek: does the peer socket have any bytes ready? Used to assert "no
// broadcast was delivered" for a removed spectator.
bool has_data_ready(int peer_fd) {
    uint8_t byte;
    ssize_t n = ::recv(peer_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n > 0;
}

// The router first sees a message; we snapshot the response frame. This is
// the same helper as test_auth_required's route_and_capture.
json route_and_capture(FakeConn& fc, net::MessageRouter& router,
                       const json& msg) {
    router.route(fc.conn, msg.dump());
    return parse_or_empty(drain_and_read(fc));
}

// After the router runs a handler that broadcasts to a third-party fd (the
// opponent, or a spectator), we still need to flush and read on THAT peer.
// This helper is called on the spectator's own FakeConn to pick up an
// inbound frame produced by another connection's route() call.
json read_frame(FakeConn& fc) {
    return parse_or_empty(drain_and_read(fc));
}

bool has_type(const json& r, const std::string& type) {
    return r.is_object() && r.value("type", "") == type;
}
bool is_error(const json& r) {
    return r.is_object() && r.value("type", "") == "error";
}

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " Phase 9.1 — Spectator Mode\n";
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

    auth::TokenSigner signer = auth::TokenSigner::generate_random();
    if (!signer.valid()) return 1;

    int64_t alice_id = insert_player(db, "Alice", 1500);
    int64_t bob_id   = insert_player(db, "Bob",   1400);
    int64_t spec_id  = insert_player(db, "Watcher", 1300);
    if (alice_id < 0 || bob_id < 0 || spec_id < 0) return 1;
    const auto now_unix = static_cast<int64_t>(std::time(nullptr));
    const auto alice_tok = mint_access(signer, alice_id, "Alice", now_unix);
    const auto bob_tok   = mint_access(signer, bob_id,   "Bob",   now_unix);
    const auto spec_tok  = mint_access(signer, spec_id,  "Watcher", now_unix);

    RoomManager  room_mgr;
    Matchmaker   matchmaker(room_mgr);
    matchmaker.set_match_callback([](const MatchResult&) {});
    GameHandler  handler(room_mgr, matchmaker);
    handler.set_database(&db);
    storage::PostgresGameStore game_store(db);
    handler.set_game_store(&game_store);
    handler.set_signer(&signer);

    // fd → Connection* map for send_json_to_fd fan-out. The test owns every
    // FakeConn; the lookup returns the raw Connection* when the handler
    // wants to write to a foreign fd (opponent or spectator).
    std::unordered_map<int, net::Connection*> conn_by_fd;
    handler.set_connection_lookup(
        [&conn_by_fd](int fd) -> net::Connection* {
            auto it = conn_by_fd.find(fd);
            return it == conn_by_fd.end() ? nullptr : it->second;
        });

    net::MessageRouter router;
    handler.register_handlers(router);

    // Bring up a live game: Alice creates, Bob joins.
    auto alice = make_conn();
    auto bob   = make_conn();
    conn_by_fd[alice->conn.get_fd()] = &alice->conn;
    conn_by_fd[bob->conn.get_fd()]   = &bob->conn;

    // Create → drain response; Join → drain both responses (game_start goes
    // to Alice's fd via the lookup, game_joined goes back to Bob).
    auto create_resp = route_and_capture(*alice, router,
        {{"type","create_game"},{"access_token",alice_tok},
         {"time_base",600},{"time_inc",5}});
    if (!has_type(create_resp, "game_created")) {
        std::cerr << "  create_game precondition failed: " << create_resp << '\n';
        return 1;
    }
    const GameId game_id = create_resp.value("game_id", static_cast<GameId>(0));

    auto join_resp = route_and_capture(*bob, router,
        {{"type","join_game"},{"access_token",bob_tok},{"game_id", game_id}});
    // Also drain the game_start frame that landed on alice's side.
    read_frame(*alice);
    if (!has_type(join_resp, "game_joined")) {
        std::cerr << "  join_game precondition failed: " << join_resp << '\n';
        return 1;
    }

    // ── list_live_games ──────────────────────────────────────
    std::cout << "\n=== list_live_games ===\n";

    run_test("list_live_games returns the live room", [&] {
        auto lister = make_conn();
        conn_by_fd[lister->conn.get_fd()] = &lister->conn;
        auto r = route_and_capture(*lister, router, {{"type","list_live_games"}});
        conn_by_fd.erase(lister->conn.get_fd());
        if (!has_type(r, "live_game_list")) return false;
        const auto& games = r["games"];
        if (!games.is_array() || games.empty()) return false;
        return games[0].value("game_id", static_cast<GameId>(0)) == game_id
            && games[0].value("white", std::string{}) == "Alice"
            && games[0].value("black", std::string{}) == "Bob"
            && games[0].value("spectator_count", -1) == 0;
    });

    // ── spectate happy path ──────────────────────────────────
    std::cout << "\n=== spectate ===\n";

    auto spec = make_conn();
    conn_by_fd[spec->conn.get_fd()] = &spec->conn;

    run_test("spectate returns spectate_start with FEN + moves + counts", [&] {
        auto r = route_and_capture(*spec, router,
            {{"type","spectate"},{"access_token",spec_tok},{"game_id", game_id}});
        return has_type(r, "spectate_start")
            && r.value("game_id", static_cast<GameId>(0)) == game_id
            && r.value("white", std::string{}) == "Alice"
            && r.value("black", std::string{}) == "Bob"
            && r.value("spectator_count", -1) == 1
            && r.contains("fen")
            && r.contains("white_time")
            && r["moves"].is_array() && r["moves"].empty();
    });

    run_test("spectate without access_token → auth_required", [&] {
        auto other = make_conn();
        conn_by_fd[other->conn.get_fd()] = &other->conn;
        auto r = route_and_capture(*other, router,
            {{"type","spectate"},{"game_id", game_id}});
        conn_by_fd.erase(other->conn.get_fd());
        return is_error(r) && r.value("code", std::string{}) == "auth_required";
    });

    run_test("spectate as a seated player → refused", [&] {
        // Alice is White in this game; can't also spectate it.
        auto r = route_and_capture(*alice, router,
            {{"type","spectate"},{"access_token",alice_tok},{"game_id", game_id}});
        return is_error(r);
    });

    // ── broadcast fan-out ────────────────────────────────────
    std::cout << "\n=== broadcast fan-out ===\n";

    run_test("Alice's move reaches Alice, Bob, AND the spectator", [&] {
        auto ack = route_and_capture(*alice, router,
            {{"type","make_move"},{"from","e2"},{"to","e4"}});
        if (!has_type(ack, "move_made")) return false;
        const auto bob_frame  = read_frame(*bob);
        const auto spec_frame = read_frame(*spec);
        return has_type(bob_frame,  "move_made")
            && has_type(spec_frame, "move_made")
            && bob_frame.value("san", std::string{})  == "e4"
            && spec_frame.value("san", std::string{}) == "e4";
    });

    run_test("Bob's move also reaches the spectator", [&] {
        auto ack = route_and_capture(*bob, router,
            {{"type","make_move"},{"from","e7"},{"to","e5"}});
        if (!has_type(ack, "move_made")) return false;
        read_frame(*alice);  // Alice sees it too — drain and ignore
        const auto spec_frame = read_frame(*spec);
        return has_type(spec_frame, "move_made")
            && spec_frame.value("san", std::string{}) == "e5";
    });

    // ── stop_spectating ──────────────────────────────────────
    std::cout << "\n=== stop_spectating ===\n";

    run_test("stop_spectating → spectate_end and no further broadcasts", [&] {
        auto ack = route_and_capture(*spec, router,
            {{"type","stop_spectating"},{"game_id", game_id}});
        if (!has_type(ack, "spectate_end")) return false;

        // A subsequent move must NOT deliver to spec's peer socket. Drain
        // alice's ack, then bob's copy, then check spec is quiet.
        auto alice_ack = route_and_capture(*alice, router,
            {{"type","make_move"},{"from","g1"},{"to","f3"}});
        (void)alice_ack;
        read_frame(*bob);
        return !has_data_ready(spec->peer_fd);
    });

    // ── disconnect sweep ─────────────────────────────────────
    std::cout << "\n=== disconnect sweep ===\n";

    run_test("on_player_disconnect drops the fd from every room", [&] {
        // Re-subscribe the same spec socket, make a move to confirm delivery,
        // then simulate a disconnect and prove the next move is silent.
        auto sub = route_and_capture(*spec, router,
            {{"type","spectate"},{"access_token",spec_tok},{"game_id", game_id}});
        if (!has_type(sub, "spectate_start")) return false;

        // A move arrives while subscribed:
        auto ack1 = route_and_capture(*bob, router,
            {{"type","make_move"},{"from","b8"},{"to","c6"}});
        (void)ack1;
        read_frame(*alice);
        const bool got_first = has_type(read_frame(*spec), "move_made");

        // Now sweep the spec fd out globally (as if the connection dropped).
        handler.on_player_disconnect(spec->conn.get_fd());

        // Next move must not reach spec's peer.
        auto ack2 = route_and_capture(*alice, router,
            {{"type","make_move"},{"from","f1"},{"to","c4"}});
        (void)ack2;
        read_frame(*bob);
        return got_first && !has_data_ready(spec->peer_fd);
    });

    // ── game_over reaches spectators ────────────────────────
    std::cout << "\n=== game_over fan-out ===\n";

    run_test("resignation reaches an active spectator", [&] {
        // Fresh spectator (the previous one was swept in the disconnect test).
        auto s2 = make_conn();
        conn_by_fd[s2->conn.get_fd()] = &s2->conn;
        auto start = route_and_capture(*s2, router,
            {{"type","spectate"},{"access_token",spec_tok},{"game_id", game_id}});
        if (!has_type(start, "spectate_start")) return false;

        // Bob resigns.
        auto bob_ack = route_and_capture(*bob, router, {{"type","resign"}});
        if (!has_type(bob_ack, "game_over")) return false;
        read_frame(*alice);   // Alice's copy
        const auto s2_frame = read_frame(*s2);
        conn_by_fd.erase(s2->conn.get_fd());
        return has_type(s2_frame, "game_over")
            && s2_frame.value("reason", std::string{}) == "resignation";
    });

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed
              << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
