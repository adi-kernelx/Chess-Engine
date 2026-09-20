/**
 * test_gameplay_service.cpp — LLD-2.1.
 *
 * Verifies that GameplayService routes each request to the right room
 * operations, fans out to the right recipients, and preserves the
 * wire error shapes GameHandler used before the extraction.
 *
 * These tests are LIGHTWEIGHT — no sockets, no auth stack, no
 * WebSocket. The service is constructed against a real RoomManager +
 * Matchmaker (they are simple, thread-safe, and cheap) with two fake
 * callables in place of the transport foreign-send / spectator-
 * broadcast paths. A capturing FakeMessageSink stands in for the
 * caller's connection.
 *
 * What is covered here (fast, no DB):
 *   1. list_games returns an empty list before any room is created.
 *   2. create_game creates a room and returns game_created frame with
 *      color=white and a positive game_id.
 *   3. Second create_game from the same caller yields an "already in
 *      a game" error frame (state machine invariant).
 *   4. join_game with an unknown game_id yields "not found".
 *   5. join_game success wires up both seats — joiner gets
 *      game_joined; creator gets game_start via the foreign sender.
 *   6. resign on a joined game yields game_over with reason=resignation
 *      to caller AND opponent (foreign sender).
 *   7. game_state on a room-less caller yields "not in a game".
 *   8. on_player_disconnect on a queued fd removes them from the
 *      matchmaker.
 *
 * Coverage tradeoff: the full move-through-checkmate / AI-move /
 * persist-game paths continue to be covered by the existing
 * test_game_persistence, test_auth_required, and test_replay
 * end-to-end suites — those exercise the service transitively through
 * the router. This file adds the SEAM-LEVEL confidence: the service
 * class behaves as an independent unit.
 */

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/gameplay_service.h"
#include "application/ports/message_sink.h"
#include "application/request_context.h"
#include "game/ai_player.h"
#include "game/matchmaker.h"
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

// Capturing MessageSink — every send() appends to a vector we can
// inspect after the call. `send` always returns true; a real socket
// might drop, but for these tests we care about what the service
// EMITS, not what the transport does with it.
struct FakeSink final : public MessageSink {
    std::vector<std::string> frames;
    bool send(std::string frame) override {
        frames.push_back(std::move(frame));
        return true;
    }
};

// Two callables the service pulls in. Both capture what the service
// tried to send to a foreign fd / broadcast to spectators.
struct FanoutCapture {
    std::vector<std::pair<int, std::string>> foreign;      // (fd, frame)
    std::vector<std::pair<int64_t, std::string>> spectate; // (room_id, frame)

    ForeignSender foreign_fn() {
        return [this](int fd, const std::string& frame) {
            foreign.emplace_back(fd, frame);
        };
    }
    SpectatorBroadcaster spectate_fn() {
        return [this](chess::game::GameRoom& room, const std::string& frame) {
            spectate.emplace_back(room.get_id(), frame);
        };
    }
};

// Build a RequestContext for the given fd. `generation` is nominal —
// the service uses `caller.fd` and never re-checks generation itself
// (SocketMessageSink does that on real sends; a FakeSink here needs
// nothing).
RequestContext make_ctx(int fd) {
    RequestContext ctx;
    ctx.caller.fd = fd;
    ctx.caller.generation = 1;
    return ctx;
}

AuthenticatedIdentity make_actor(int64_t player_id,
                                 const std::string& name,
                                 int elo = 1500) {
    AuthenticatedIdentity a;
    a.player_id  = player_id;
    a.username   = name;
    a.elo_rating = elo;
    return a;
}

// Snapshot which frame type a sink last received. Returns "" if empty.
std::string last_frame_type(const FakeSink& sink) {
    if (sink.frames.empty()) return "";
    try {
        auto j = json::parse(sink.frames.back());
        if (j.contains("type") && j["type"].is_string()) return j["type"].get<std::string>();
    } catch (...) {}
    return "";
}

std::string first_frame_type(const FakeSink& sink) {
    if (sink.frames.empty()) return "";
    try {
        auto j = json::parse(sink.frames.front());
        if (j.contains("type") && j["type"].is_string()) return j["type"].get<std::string>();
    } catch (...) {}
    return "";
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-2.1 — GameplayService\n";
    std::cout << "========================================\n";

    run_test("list_games returns empty on fresh RoomManager", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.list_games(make_ctx(10), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j.value("type", "") == "game_list"
            && j.contains("games") && j["games"].is_array() && j["games"].empty();
    });

    run_test("create_game emits game_created with color=white and a positive game_id", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.create_game(make_ctx(20), make_actor(101, "Alice"), 600, 5, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j.value("type", "") == "game_created"
            && j.value("color", "") == "white"
            && j.value("game_id", 0) > 0;
    });

    run_test("second create_game on same fd errors with 'already in a game'", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.create_game(make_ctx(30), make_actor(102, "Alice"), 600, 5, sink);
        svc.create_game(make_ctx(30), make_actor(102, "Alice"), 600, 5, sink);
        if (sink.frames.size() != 2) return false;
        auto j = json::parse(sink.frames.back());
        // Same wire shape as the pre-refactor make_error frame:
        //   { "type": "error", "message": "..." }  (no code)
        return j.value("type", "") == "error"
            && j.value("message", "").find("already in a game") != std::string::npos
            && !j.contains("code");
    });

    run_test("join_game with unknown game_id emits 'not found'", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.join_game(make_ctx(40), make_actor(103, "Bob"), /*game_id=*/9999, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j.value("type", "") == "error"
            && j.value("message", "").find("not found") != std::string::npos;
    });

    run_test("join_game success — joiner gets game_joined, creator gets game_start via foreign sender", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink creator_sink;
        svc.create_game(make_ctx(50), make_actor(200, "Alice"), 600, 5, creator_sink);
        auto j = json::parse(creator_sink.frames[0]);
        int64_t gid = j.value("game_id", 0);
        if (gid <= 0) return false;

        FakeSink joiner_sink;
        svc.join_game(make_ctx(51), make_actor(201, "Bob"), gid, joiner_sink);

        // Joiner's sink should carry game_joined with color=black.
        if (joiner_sink.frames.size() != 1) return false;
        auto jj = json::parse(joiner_sink.frames[0]);
        bool joiner_ok = jj.value("type", "") == "game_joined"
                      && jj.value("color", "") == "black"
                      && jj.value("game_id", int64_t{0}) == gid;

        // The foreign sender should have delivered a game_start to fd=50.
        if (cap.foreign.size() != 1) return false;
        if (cap.foreign[0].first != 50) return false;
        auto gs = json::parse(cap.foreign[0].second);
        bool creator_ok = gs.value("type", "") == "game_start"
                       && gs.value("opponent", "") == "Bob"
                       && gs.value("color", "") == "white";

        return joiner_ok && creator_ok;
    });

    run_test("resign after join — caller AND opponent receive game_over/resignation", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink white_sink;
        svc.create_game(make_ctx(60), make_actor(300, "Alice"), 600, 5, white_sink);
        auto gid = json::parse(white_sink.frames[0]).value("game_id", int64_t{0});

        FakeSink black_sink;
        svc.join_game(make_ctx(61), make_actor(301, "Bob"), gid, black_sink);
        cap.foreign.clear();  // discard the join's game_start delivery

        // Black resigns.
        FakeSink resign_sink;
        svc.resign(make_ctx(61), chess::protocol::ResignRequest{}, resign_sink);

        if (resign_sink.frames.size() != 1) return false;
        auto ro = json::parse(resign_sink.frames[0]);
        bool caller_ok = ro.value("type", "") == "game_over"
                      && ro.value("reason", "") == "resignation";

        // Foreign sender should have delivered game_over to white (fd=60).
        if (cap.foreign.size() != 1) return false;
        auto fo = json::parse(cap.foreign[0].second);
        bool opp_ok = cap.foreign[0].first == 60
                   && fo.value("type", "") == "game_over"
                   && fo.value("reason", "") == "resignation";

        return caller_ok && opp_ok;
    });

    run_test("game_state on room-less caller emits 'You are not in a game'", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.game_state(make_ctx(70), chess::protocol::GameStateRequest{}, sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j.value("type", "") == "error"
            && j.value("message", "").find("not in a game") != std::string::npos;
    });

    run_test("on_player_disconnect removes a queued fd from the matchmaker", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.quick_play(make_ctx(80), make_actor(400, "Carol"), 600, 5, sink);
        if (!mm.is_queued(80)) return false;
        svc.on_player_disconnect(80);
        return !mm.is_queued(80);
    });

    run_test("cancel_queue on not-queued fd emits 'not in the matchmaking queue'", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink sink;
        svc.cancel_queue(make_ctx(90), sink);
        if (sink.frames.size() != 1) return false;
        auto j = json::parse(sink.frames[0]);
        return j.value("type", "") == "error"
            && j.value("message", "").find("not in the matchmaking queue") != std::string::npos;
    });

    (void)first_frame_type;
    (void)last_frame_type;

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed\n";
    return (g_failed > 0) ? 1 : 0;
}
