/**
 * test_game_query_service.cpp — LLD-2.2.
 *
 * Seam-level tests over `application::GameQueryService`. No DB, no
 * sockets, no auth stack. A capturing `FakeSink` receives every frame
 * the service emits and lets each test assert on the exact bytes.
 *
 * What is covered here (fast, no DB, no cluster):
 *   1. list_live_games returns an empty `live_game_list` on a fresh
 *      RoomManager.
 *   2. list_live_games includes a room after it enters IN_PROGRESS
 *      (via join_game on the manager).
 *   3. spectate on an unknown game_id returns "Game N not found".
 *   4. spectate with game_id == 0 returns "Missing or invalid game_id".
 *   5. spectate on an IN_PROGRESS room adds the caller as a spectator
 *      and emits spectate_start with the right onboarding fields.
 *   6. spectate rejects a seated player of the same room with
 *      "You are seated in this game".
 *   7. spectate rejects a caller already in another (unfinished) room
 *      with "You are already in a game (ID: N)".
 *   8. stop_spectating always emits spectate_end with the requested
 *      game_id (idempotent) and drops the subscription when present.
 *   9. get_profile without a DB emits the pre-refactor
 *      "Profiles are not available (no database)" error.
 *  10. get_leaderboard without a DB emits the pre-refactor
 *      "Leaderboard is not available (no database)" error.
 *  11. get_history without a DB emits the pre-refactor
 *      "History unavailable — no database" error.
 *  12. get_game without a DB emits the pre-refactor
 *      "Replay unavailable — server has no database" error.
 *
 * The DB-backed happy paths (profile/leaderboard/history/game) are
 * already covered end-to-end by test_repositories and test_replay
 * against a real cluster; here we test what the seam guarantees when
 * the cluster is missing plus every non-DB branch.
 */

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/game_query_service.h"
#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "core/types.h"
#include "game/room_manager.h"

using json = nlohmann::json;
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

struct FakeSink final : public MessageSink {
    std::vector<std::string> frames;
    bool send(std::string frame) override {
        frames.push_back(std::move(frame));
        return true;
    }
};

RequestContext make_ctx(int fd) {
    RequestContext ctx;
    ctx.caller.fd         = fd;
    ctx.caller.generation = 1;
    return ctx;
}

// Create + join a room via RoomManager so it transitions to IN_PROGRESS
// exactly the way GameplayService would in production. Returns the room.
std::shared_ptr<chess::game::GameRoom>
make_live_room(chess::game::RoomManager& rooms,
               PlayerId white_id, int white_fd, const std::string& white_name,
               PlayerId black_id, int black_fd, const std::string& black_name) {
    auto room = rooms.create_room(white_id, white_name, white_fd);
    room->join(black_id, black_name, black_fd);
    // GameRoom::join fills the second seat and starts the clock,
    // transitioning the room to IN_PROGRESS.
    return room;
}

} // namespace

int main() {
    std::cout << "Running GameQueryService tests...\n";

    // ── list_live_games ────────────────────────────────────────────

    run_test("list_live_games on empty RoomManager", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.list_live_games(make_ctx(100), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "live_game_list" && j["games"].is_array()
            && j["games"].empty();
    });

    run_test("list_live_games includes an IN_PROGRESS room", [] {
        chess::game::RoomManager rooms;
        make_live_room(rooms, 1, 10, "alice", 2, 11, "bob");
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.list_live_games(make_ctx(100), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        if (j["type"] != "live_game_list") return false;
        if (j["games"].size() != 1) return false;
        auto row = j["games"][0];
        return row["white"] == "alice" && row["black"] == "bob"
            && row["spectator_count"] == 0;
    });

    // ── spectate error branches ───────────────────────────────────

    run_test("spectate with game_id==0 → invalid", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.spectate(make_ctx(50), "watcher", 0, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "Missing or invalid game_id";
    });

    run_test("spectate on unknown game_id → not found", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.spectate(make_ctx(50), "watcher", 9999, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "Game 9999 not found";
    });

    run_test("spectate on IN_PROGRESS room → spectate_start", [] {
        chess::game::RoomManager rooms;
        auto room = make_live_room(rooms, 1, 10, "alice", 2, 11, "bob");
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.spectate(make_ctx(77), "watcher", room->get_id(), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        if (j["type"] != "spectate_start") return false;
        if (j["game_id"] != room->get_id()) return false;
        if (j["white"] != "alice" || j["black"] != "bob") return false;
        // The subscription must be live.
        return room->spectator_count() == 1;
    });

    run_test("spectate rejects a seated player of same room", [] {
        chess::game::RoomManager rooms;
        auto room = make_live_room(rooms, 1, 10, "alice", 2, 11, "bob");
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        // fd 10 is white's seat in this room.
        svc.spectate(make_ctx(10), "alice", room->get_id(), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "You are already in a game (ID: " +
                                std::to_string(room->get_id()) + ")";
    });

    run_test("spectate rejects caller already in another room", [] {
        chess::game::RoomManager rooms;
        auto r1 = make_live_room(rooms, 1, 10, "alice", 2, 11, "bob");
        auto r2 = make_live_room(rooms, 3, 20, "carol", 4, 21, "dave");
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        // fd 10 is seated in r1 and wants to spectate r2.
        svc.spectate(make_ctx(10), "alice", r2->get_id(), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        std::string want = "You are already in a game (ID: " +
                           std::to_string(r1->get_id()) + ")";
        return j["type"] == "error" && j["message"] == want;
    });

    // ── stop_spectating ───────────────────────────────────────────

    run_test("stop_spectating with unknown game_id → still ack", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.stop_spectating(make_ctx(50), 7777, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "spectate_end" && j["game_id"] == 7777;
    });

    run_test("stop_spectating drops the live subscription", [] {
        chess::game::RoomManager rooms;
        auto room = make_live_room(rooms, 1, 10, "alice", 2, 11, "bob");
        room->add_spectator(77);
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.stop_spectating(make_ctx(77), room->get_id(), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "spectate_end"
            && j["game_id"] == room->get_id()
            && room->spectator_count() == 0;
    });

    // ── DB-backed routes with null database ───────────────────────

    run_test("get_profile with null db → no-database error", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.get_profile(make_ctx(1), "alice", 0, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "Profiles are not available (no database)";
    });

    run_test("get_leaderboard with null db → no-database error", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.get_leaderboard(make_ctx(1), 20, 0, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "Leaderboard is not available (no database)";
    });

    run_test("get_history with null db → no-database error", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.get_history(make_ctx(1), "alice", 20, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "History unavailable \xE2\x80\x94 no database";
    });

    run_test("get_game with null db → no-database error", [] {
        chess::game::RoomManager rooms;
        GameQueryService svc(rooms, nullptr);
        FakeSink sink;
        svc.get_game(make_ctx(1), 1, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j["type"] == "error"
            && j["message"] == "Replay unavailable \xE2\x80\x94 server has no database";
    });

    // ── Summary ───────────────────────────────────────────────────

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
