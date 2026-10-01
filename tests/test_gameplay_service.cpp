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

#include <chrono>
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
#include "game/game_events.h"
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

struct DeliveryOrderListener final : public chess::game::GameEventListener {
    const FakeSink* caller = nullptr;
    bool result_was_delivered_before_persistence = false;
    void on_game_completed(const chess::game::GameCompleted&) override {
        result_was_delivered_before_persistence = caller && !caller->frames.empty();
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
        auto order = std::make_shared<DeliveryOrderListener>();
        order->caller = &resign_sink;
        auto room = rooms.find_room(static_cast<chess::GameId>(gid));
        if (!room) return false;
        room->add_listener(order);
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

        return caller_ok && opp_ok
            && order->result_was_delivered_before_persistence;
    });

    run_test("draw offer reaches opponent and acceptance ends by agreement", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker mm(rooms);
        chess::game::AIPlayer ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink white, black;
        svc.create_game(make_ctx(62), make_actor(310, "Alice"), 600, 5, white);
        const auto gid = json::parse(white.frames.back()).value("game_id", int64_t{0});
        svc.join_game(make_ctx(63), make_actor(311, "Bob"), gid, black);
        cap.foreign.clear();

        FakeSink offer_sink;
        svc.offer_draw(make_ctx(62), offer_sink);
        if (last_frame_type(offer_sink) != "draw_offer_sent"
            || cap.foreign.size() != 1 || cap.foreign[0].first != 63
            || json::parse(cap.foreign[0].second).value("type", "") != "draw_offered") {
            return false;
        }

        cap.foreign.clear();
        FakeSink accept_sink;
        svc.respond_to_draw(make_ctx(63), true, accept_sink);
        return last_frame_type(accept_sink) == "game_over"
            && json::parse(accept_sink.frames.back()).value("reason", "") == "draw_agreement"
            && cap.foreign.size() == 1 && cap.foreign[0].first == 62
            && json::parse(cap.foreign[0].second).value("result", "") == "1/2-1/2";
    });

    run_test("offeree move notifies offerer that draw was declined", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker mm(rooms);
        chess::game::AIPlayer ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink white, black, offer_sink, move_sink;
        svc.create_game(make_ctx(64), make_actor(320, "Alice"), 600, 5, white);
        const auto gid = json::parse(white.frames.back()).value("game_id", int64_t{0});
        svc.join_game(make_ctx(65), make_actor(321, "Bob"), gid, black);
        cap.foreign.clear();
        svc.offer_draw(make_ctx(65), offer_sink); // black offers; white is to move
        cap.foreign.clear();

        chess::protocol::MakeMoveRequest req;
        req.from = "e2";
        req.to = "e4";
        svc.make_move(make_ctx(64), req, move_sink);
        if (last_frame_type(move_sink) != "move_made" || cap.foreign.size() != 2) return false;
        const auto move = json::parse(cap.foreign[0].second);
        const auto declined = json::parse(cap.foreign[1].second);
        return cap.foreign[0].first == 65 && move.value("type", "") == "move_made"
            && move.contains("legal_moves") && move["legal_moves"].is_array()
            && cap.foreign[1].first == 65 && declined.value("type", "") == "draw_declined"
            && declined.value("reason", "") == "move_made";
    });

    run_test("delayed acceptance after offeree move is rejected", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker mm(rooms);
        chess::game::AIPlayer ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink white, black, offer_sink, white_move_sink, black_move_sink, late_accept;
        svc.create_game(make_ctx(66), make_actor(330, "Alice"), 600, 5, white);
        const auto gid = json::parse(white.frames.back()).value("game_id", int64_t{0});
        svc.join_game(make_ctx(67), make_actor(331, "Bob"), gid, black);
        svc.offer_draw(make_ctx(66), offer_sink);

        chess::protocol::MakeMoveRequest white_move;
        white_move.from = "e2";
        white_move.to = "e4";
        svc.make_move(make_ctx(66), white_move, white_move_sink);

        chess::protocol::MakeMoveRequest black_move;
        black_move.from = "e7";
        black_move.to = "e5";
        svc.make_move(make_ctx(67), black_move, black_move_sink);
        svc.respond_to_draw(make_ctx(67), true, late_accept);

        if (late_accept.frames.size() != 1) return false;
        const auto response = json::parse(late_accept.frames[0]);
        auto room = rooms.find_room(gid);
        return response.value("type", "") == "error"
            && response.value("message", "") == "There is no pending draw offer"
            && room && room->get_state() == chess::game::RoomState::IN_PROGRESS;
    });

    run_test("maintenance expires draw offers after 45 seconds for both players", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker mm(rooms);
        chess::game::AIPlayer ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());
        chess::application::ports::FakeClock clock;

        FakeSink white, black, offer_sink;
        svc.create_game(make_ctx(68), make_actor(340, "Alice"), 600, 5, white);
        const auto gid = json::parse(white.frames.back()).value("game_id", int64_t{0});
        auto room = rooms.find_room(gid);
        if (!room) return false;
        room->set_clock(&clock);
        svc.join_game(make_ctx(69), make_actor(341, "Bob"), gid, black);
        svc.offer_draw(make_ctx(68), offer_sink);
        cap.foreign.clear();

        clock.advance(std::chrono::seconds(44));
        svc.expire_disconnected_games();
        if (!cap.foreign.empty() || room->draw_offer_from() != chess::Color::WHITE) {
            return false;
        }

        clock.advance(std::chrono::seconds(1));
        svc.expire_disconnected_games();
        if (cap.foreign.size() != 2 || room->draw_offer_from() != chess::Color::NONE) {
            return false;
        }
        const auto first = json::parse(cap.foreign[0].second);
        const auto second = json::parse(cap.foreign[1].second);
        return cap.foreign[0].first == 68
            && first.value("type", "") == "draw_declined"
            && first.value("reason", "") == "expired"
            && cap.foreign[1].first == 69
            && second.value("type", "") == "draw_offer_resolved"
            && second.value("reason", "") == "expired";
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

    run_test("authenticated AI game is recoverable by durable player identity", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        const auto actor = make_actor(4501, "Alice", 800);
        FakeSink start_sink;
        svc.play_ai(make_ctx(700), actor, "easy", 600, 5, start_sink);
        if (start_sink.frames.size() != 1) return false;

        const auto started = json::parse(start_sink.frames[0]);
        if (started.value("type", "") != "game_start"
            || !started.value("ai_game", false)) {
            return false;
        }

        const auto game_id = started.value("game_id", int64_t{0});
        auto room = rooms.find_room(game_id);
        if (!room
            || room->get_db_player_id(chess::Color::WHITE) != actor.player_id
            || room->get_elo(chess::Color::WHITE) != actor.elo_rating) {
            return false;
        }

        // The browser asks for game_state immediately after game_start.  The
        // authenticated path deliberately resolves by durable DB identity,
        // not only by the current socket fd.
        RequestContext state_ctx = make_ctx(700);
        state_ctx.identity = actor;
        FakeSink state_sink;
        svc.game_state(state_ctx, chess::protocol::GameStateRequest{}, state_sink);
        if (state_sink.frames.size() != 1) return false;

        const auto state = json::parse(state_sink.frames[0]);
        return state.value("type", "") == "game_state"
            && state.value("game_id", int64_t{0}) == game_id
            && state.value("state", "") == "in_progress";
    });

    run_test("authenticated game_state rebinds a disconnected player", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink white_sink, black_sink, active_sink, state_sink;
        svc.create_game(make_ctx(71), make_actor(501, "Alice"), 600, 5, white_sink);
        const auto created = json::parse(white_sink.frames.back());
        svc.join_game(make_ctx(72), make_actor(502, "Bob"),
                      created.value("game_id", 0), black_sink);
        svc.get_active_game(make_ctx(72), make_actor(502, "Bob"), active_sink);
        if (active_sink.frames.size() != 1) return false;
        const auto active = json::parse(active_sink.frames[0]);
        if (active.value("type", "") != "active_game"
            || active["game"].value("game_id", 0) != created.value("game_id", 0)
            || active["game"].value("color", "") != "black"
            || active["game"].value("opponent", "") != "Alice"
            || active["game"].value("state", "") != "in_progress") {
            return false;
        }
        cap.foreign.clear();

        svc.on_player_disconnect(72);
        if (cap.foreign.size() != 1) return false;
        auto disconnected = json::parse(cap.foreign.back().second);
        if (cap.foreign.back().first != 71
            || disconnected.value("type", "") != "opponent_disconnected"
            || disconnected.value("grace_ms", 0) != GameplayService::DISCONNECT_GRACE_MS) {
            return false;
        }

        RequestContext reconnect_ctx = make_ctx(73);
        reconnect_ctx.identity = make_actor(502, "Bob");
        svc.game_state(reconnect_ctx, chess::protocol::GameStateRequest{}, state_sink);
        if (state_sink.frames.size() != 1
            || json::parse(state_sink.frames[0]).value("type", "") != "game_state") {
            return false;
        }
        if (cap.foreign.size() != 2 || cap.foreign.back().first != 71) return false;
        return json::parse(cap.foreign.back().second).value("type", "")
            == "opponent_reconnected";
    });

    run_test("get_active_game rebinds a disconnected seat before Resume", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        const auto alice = make_actor(511, "Alice");
        const auto bob = make_actor(512, "Bob");
        FakeSink white_sink, black_sink, active_sink, state_sink;
        svc.create_game(make_ctx(171), alice, 600, 5, white_sink);
        const auto game_id = json::parse(white_sink.frames.back()).value("game_id", 0);
        svc.join_game(make_ctx(172), bob, game_id, black_sink);

        svc.on_player_disconnect(172);
        cap.foreign.clear();
        svc.get_active_game(make_ctx(173), bob, active_sink);
        auto room = rooms.find_room(game_id);
        if (!room || room->get_player_fd(chess::Color::BLACK) != 173
            || !room->is_connected(chess::Color::BLACK)) return false;
        if (active_sink.frames.size() != 1) return false;
        const auto active = json::parse(active_sink.frames[0]);
        if (active.value("type", "") != "active_game"
            || !active.contains("game") || active["game"].is_null()
            || active["game"].value("game_id", 0) != game_id) return false;
        if (cap.foreign.size() != 1 || cap.foreign[0].first != 171
            || json::parse(cap.foreign[0].second).value("type", "")
                != "opponent_reconnected") return false;

        RequestContext state_ctx = make_ctx(173);
        state_ctx.identity = bob;
        svc.game_state(state_ctx, chess::protocol::GameStateRequest{}, state_sink);
        return state_sink.frames.size() == 1
            && json::parse(state_sink.frames[0]).value("type", "") == "game_state";
    });

    run_test("same-fd reuse clears the old disconnect deadline", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());
        chess::application::ports::FakeClock clock;

        FakeSink white_sink, black_sink, state_sink;
        svc.create_game(make_ctx(76), make_actor(701, "Alice"), 600, 5, white_sink);
        const auto created = json::parse(white_sink.frames.back());
        auto room = rooms.find_room(created.value("game_id", 0));
        if (!room) return false;
        room->set_clock(&clock);
        svc.join_game(make_ctx(77), make_actor(702, "Bob"),
                      created.value("game_id", 0), black_sink);

        svc.on_player_disconnect(76);
        clock.advance(std::chrono::seconds(60));

        RequestContext reused_fd = make_ctx(76);
        reused_fd.identity = make_actor(701, "Alice");
        svc.game_state(reused_fd, chess::protocol::GameStateRequest{}, state_sink);
        if (state_sink.frames.empty()
            || json::parse(state_sink.frames.back()).value("type", "") != "game_state") {
            return false;
        }

        chess::protocol::MakeMoveRequest move;
        move.from = "e2";
        move.to = "e4";
        FakeSink move_sink;
        svc.make_move(reused_fd, move, move_sink);
        if (last_frame_type(move_sink) != "move_made") return false;

        clock.advance(std::chrono::seconds(120));
        svc.expire_disconnected_games();
        return room->get_state() == chess::game::RoomState::IN_PROGRESS
            && room->is_connected(chess::Color::WHITE);
    });

    run_test("maintenance broadcasts timeout without waiting for another move", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());
        chess::application::ports::FakeClock clock;

        FakeSink white_sink, black_sink;
        svc.create_game(make_ctx(78), make_actor(801, "Alice"), 60, 0, white_sink);
        const auto created = json::parse(white_sink.frames.back());
        auto room = rooms.find_room(created.value("game_id", 0));
        if (!room) return false;
        room->set_clock(&clock);
        svc.join_game(make_ctx(79), make_actor(802, "Bob"),
                      created.value("game_id", 0), black_sink);
        cap.foreign.clear();
        cap.spectate.clear();

        clock.advance(std::chrono::milliseconds(59999));
        svc.expire_disconnected_games();
        if (!cap.foreign.empty() || room->get_state() != chess::game::RoomState::IN_PROGRESS) {
            return false;
        }

        clock.advance(std::chrono::milliseconds(1));
        svc.expire_disconnected_games();
        if (cap.foreign.size() != 2 || cap.spectate.size() != 1) return false;
        const auto white_over = json::parse(cap.foreign[0].second);
        const auto black_over = json::parse(cap.foreign[1].second);
        return cap.foreign[0].first == 78
            && cap.foreign[1].first == 79
            && white_over.value("type", "") == "game_over"
            && black_over.value("type", "") == "game_over"
            && white_over.value("reason", "") == "timeout"
            && white_over.value("result", "") == "0-1"
            && room->get_game_status() == chess::GameStatus::TIMEOUT;
    });

    run_test("accepted rematch starts a color-swapped game for both players", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker mm(rooms);
        chess::game::AIPlayer ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());

        FakeSink white, black, resigned, offered, pending, accepted;
        svc.create_game(make_ctx(81), make_actor(901, "Alice", 810), 300, 3, white);
        const auto old_id = json::parse(white.frames.back()).value("game_id", int64_t{0});
        svc.join_game(make_ctx(82), make_actor(902, "Bob", 820), old_id, black);
        svc.resign(make_ctx(81), chess::protocol::ResignRequest{}, resigned);
        cap.foreign.clear();

        svc.offer_rematch(make_ctx(81), make_actor(901, "Alice", 810), old_id, offered);
        if (last_frame_type(offered) != "rematch_offer_sent" || cap.foreign.size() != 1) {
            return false;
        }
        const auto incoming = json::parse(cap.foreign.back().second);
        if (cap.foreign.back().first != 82
            || incoming.value("type", "") != "rematch_offered") return false;
        cap.foreign.clear();

        // Simulate leaving/reloading the completed-game screen: the old
        // transport disappears and Replay recovers the offer by DB identity.
        svc.on_player_disconnect(82);
        svc.get_pending_rematch(
            make_ctx(85), make_actor(902, "Bob", 820), pending);
        if (pending.frames.size() != 1) return false;
        const auto pending_frame = json::parse(pending.frames.back());
        if (pending_frame.value("type", "") != "pending_rematch"
            || !pending_frame.contains("offer")
            || pending_frame["offer"].value("game_id", int64_t{0}) != old_id
            || pending_frame["offer"].value("role", "") != "recipient") {
            return false;
        }

        svc.respond_to_rematch(
            make_ctx(85), make_actor(902, "Bob", 820), old_id, true, accepted);
        if (last_frame_type(accepted) != "rematch_started" || cap.foreign.size() != 1) {
            return false;
        }
        const auto bob_start = json::parse(accepted.frames.back());
        const auto alice_start = json::parse(cap.foreign.back().second);
        const auto new_id = bob_start.value("game_id", int64_t{0});
        auto rematch = rooms.find_room(new_id);
        return new_id > old_id && rematch
            && bob_start.value("color", "") == "white"
            && bob_start.value("opponent", "") == "Alice"
            && cap.foreign.back().first == 81
            && alice_start.value("type", "") == "rematch_started"
            && alice_start.value("color", "") == "black"
            && alice_start.value("opponent", "") == "Bob"
            && rematch->get_db_player_id(chess::Color::WHITE) == 902
            && rematch->get_db_player_id(chess::Color::BLACK) == 901
            && rematch->get_time_control().base_time_ms == 300000
            && rematch->get_time_control().increment_ms == 3000;
    });

    run_test("maintenance expires unanswered rematch for both players", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker mm(rooms);
        chess::game::AIPlayer ai;
        FanoutCapture cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());
        chess::application::ports::FakeClock clock;

        FakeSink white, black, resigned, offered;
        svc.create_game(make_ctx(83), make_actor(903, "Alice"), 300, 3, white);
        const auto old_id = json::parse(white.frames.back()).value("game_id", int64_t{0});
        auto room = rooms.find_room(old_id);
        if (!room) return false;
        room->set_clock(&clock);
        svc.join_game(make_ctx(84), make_actor(904, "Bob"), old_id, black);
        svc.resign(make_ctx(83), chess::protocol::ResignRequest{}, resigned);
        cap.foreign.clear();
        svc.offer_rematch(make_ctx(83), make_actor(903, "Alice"), old_id, offered);
        cap.foreign.clear();

        clock.advance(std::chrono::seconds(45));
        svc.expire_disconnected_games();
        if (cap.foreign.size() != 2) return false;
        const auto first = json::parse(cap.foreign[0].second);
        const auto second = json::parse(cap.foreign[1].second);
        if (cap.foreign[0].first != 83
            || first.value("type", "") != "rematch_declined"
            || first.value("reason", "") != "expired"
            || cap.foreign[1].first != 84
            || second.value("type", "") != "rematch_offer_resolved"
            || second.value("reason", "") != "expired") return false;

        FakeSink next_game;
        svc.create_game(make_ctx(83), make_actor(903, "Alice"), 600, 5, next_game);
        return last_frame_type(next_game) == "game_created";
    });

    run_test("maintenance tick broadcasts abandonment after 120 seconds", [] {
        chess::game::RoomManager rooms;
        chess::game::Matchmaker  mm(rooms);
        chess::game::AIPlayer    ai;
        FanoutCapture            cap;
        GameplayService svc(rooms, mm, ai, cap.foreign_fn(), cap.spectate_fn());
        chess::application::ports::FakeClock clock;

        FakeSink white_sink, black_sink;
        svc.create_game(make_ctx(74), make_actor(601, "Alice"), 600, 5, white_sink);
        const auto created = json::parse(white_sink.frames.back());
        auto room = rooms.find_room(created.value("game_id", 0));
        if (!room) return false;
        room->set_clock(&clock);
        svc.join_game(make_ctx(75), make_actor(602, "Bob"),
                      created.value("game_id", 0), black_sink);
        cap.foreign.clear();

        svc.on_player_disconnect(75);
        cap.foreign.clear();
        clock.advance(std::chrono::seconds(119));
        svc.expire_disconnected_games();
        if (!cap.foreign.empty() || room->get_state() != chess::game::RoomState::IN_PROGRESS) {
            return false;
        }

        clock.advance(std::chrono::seconds(1));
        svc.expire_disconnected_games();
        if (cap.foreign.size() != 1 || cap.foreign[0].first != 74) return false;
        const auto over = json::parse(cap.foreign[0].second);
        return over.value("type", "") == "game_over"
            && over.value("reason", "") == "abandonment"
            && over.value("result", "") == "1-0";
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
