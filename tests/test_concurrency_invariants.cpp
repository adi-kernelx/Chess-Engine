/**
 * test_concurrency_invariants.cpp — LLD-6.4.
 *
 * Transport and room invariants a future async slice will need in place
 * before touching the current serialized event loop:
 *
 *   1. `Connection::MAX_WRITE_BUFFER_BYTES` cap. A slow client
 *      accumulating unread frames used to grow the write buffer
 *      unboundedly. Now the append short-circuits at the cap and
 *      sets a sticky `write_buffer_overflowed()` flag; the transport
 *      closes the connection on the next drain check.
 *
 *   2. WebSocket heartbeat state. It is inactive before upgrade, permits
 *      one outstanding ping, expires at the timeout boundary, and a pong
 *      resets the next-ping interval.
 *
 *   3. `GameRoom::revision()`. Monotonically increases on every
 *      mutating public method (join / submit_move / submit_move_ai
 *      / resign / on_disconnect / on_reconnect / add_spectator /
 *      remove_spectator). Read-only accessors do not bump it. A
 *      GameSnapshot pins the revision it was built at, so a future
 *      async consumer can compare `snapshot.revision` against
 *      `room->revision()` to discard stale results.
 */

#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

#include "game/game_events.h"
#include "game/game_room.h"
#include "game/game_snapshot.h"
#include "game/room_manager.h"
#include "net/connection.h"

using chess::game::GameCompleted;
using chess::game::GameEventListener;
using chess::game::GameRoom;
using chess::game::GameSnapshot;
using chess::game::RoomManager;
using chess::game::TimeControl;
using chess::net::Connection;

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

/// Listener that records the last GameCompleted event's snapshot.
struct SnapshotCapture final : GameEventListener {
    GameSnapshot last;
    int          fires = 0;
    void on_game_completed(const GameCompleted& ev) override {
        last = ev.snapshot;
        ++fires;
    }
};

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-6.4 — concurrency invariants\n";
    std::cout << "========================================\n";

    // ── Connection write-buffer cap ────────────────────────────────────

    run_test("Small writes do not trip the write-buffer cap", []() {
        Connection c(-1, "test");   // fd=-1: no real socket needed
        std::vector<uint8_t> payload(64 * 1024, 'x');  // 64 KB
        c.append_to_write_buffer(payload.data(), payload.size());
        return !c.write_buffer_overflowed()
            && c.write_buffer_bytes() == payload.size();
    });

    run_test("First append exceeding the cap flips the overflow flag", []() {
        Connection c(-1, "test");
        // 4 MB - 1 byte is fine.
        std::vector<uint8_t> almost(
            Connection::MAX_WRITE_BUFFER_BYTES - 1, 'x');
        c.append_to_write_buffer(almost.data(), almost.size());
        if (c.write_buffer_overflowed()) return false;

        // Two more bytes push us over; the append is dropped.
        uint8_t two[2] = {'y', 'y'};
        c.append_to_write_buffer(two, 2);
        return c.write_buffer_overflowed()
            && c.write_buffer_bytes() == almost.size();  // unchanged
    });

    run_test("Overflow flag is sticky — later writes stay dropped", []() {
        Connection c(-1, "test");
        std::vector<uint8_t> big(
            Connection::MAX_WRITE_BUFFER_BYTES + 1, 'x');
        c.append_to_write_buffer(big.data(), big.size());
        if (!c.write_buffer_overflowed()) return false;

        const size_t before = c.write_buffer_bytes();
        uint8_t small[1] = {'z'};
        c.append_to_write_buffer(small, 1);
        return c.write_buffer_bytes() == before
            && c.write_buffer_overflowed();
    });

    // ── WebSocket heartbeat state ─────────────────────────────────────

    run_test("Heartbeat starts only after WebSocket upgrade", []() {
        Connection c(-1, "test");
        const auto now = Connection::HeartbeatClock::now();
        return !c.heartbeat_due(now + std::chrono::hours(1),
                                std::chrono::seconds(10));
    });

    run_test("Heartbeat becomes due, waits for pong, then expires", []() {
        Connection c(-1, "test");
        c.set_upgraded(true);
        const auto base = Connection::HeartbeatClock::now();
        if (!c.heartbeat_due(base + std::chrono::seconds(11),
                             std::chrono::seconds(10))) return false;

        const auto sent = base + std::chrono::seconds(11);
        c.mark_ping_sent(sent);
        return c.awaiting_pong()
            && !c.heartbeat_due(sent + std::chrono::seconds(30),
                                std::chrono::seconds(10))
            && !c.heartbeat_expired(sent + std::chrono::seconds(9),
                                    std::chrono::seconds(10))
            && c.heartbeat_expired(sent + std::chrono::seconds(10),
                                   std::chrono::seconds(10));
    });

    run_test("Pong acknowledges heartbeat and schedules the next one", []() {
        Connection c(-1, "test");
        c.set_upgraded(true);
        const auto sent = Connection::HeartbeatClock::now();
        c.mark_ping_sent(sent);
        const auto pong = sent + std::chrono::seconds(2);
        c.mark_pong_received(pong);
        return !c.awaiting_pong()
            && !c.heartbeat_due(pong + std::chrono::seconds(9),
                                std::chrono::seconds(10))
            && c.heartbeat_due(pong + std::chrono::seconds(10),
                               std::chrono::seconds(10));
    });

    run_test("Any valid inbound frame acknowledges peer liveness", []() {
        Connection c(-1, "test");
        c.set_upgraded(true);
        const auto sent = Connection::HeartbeatClock::now();
        c.mark_ping_sent(sent);
        const auto activity = sent + std::chrono::seconds(12);
        c.mark_activity_received(activity);
        return !c.awaiting_pong()
            && !c.heartbeat_due(activity + std::chrono::seconds(9),
                                std::chrono::seconds(10))
            && c.heartbeat_due(activity + std::chrono::seconds(10),
                               std::chrono::seconds(10));
    });

    // ── GameRoom revision counter ──────────────────────────────────────

    run_test("GameRoom starts at revision 0", []() {
        RoomManager rm;
        auto room = rm.create_room(1, "alice", 100);
        return room->revision() == 0;
    });

    run_test("join bumps revision", []() {
        RoomManager rm;
        auto room = rm.create_room(2, "alice", 200);
        const uint64_t before = room->revision();
        room->join(3, "bob", 201);
        return room->revision() > before;
    });

    run_test("read-only accessors do NOT bump revision", []() {
        RoomManager rm;
        auto room = rm.create_room(4, "alice", 300);
        room->join(5, "bob", 301);
        const uint64_t after_join = room->revision();

        // A slew of read-only calls.
        (void)room->get_state();
        (void)room->side_to_move();
        (void)room->get_time_control();
        (void)room->get_game_status();
        (void)room->get_move_history();
        int w, b; room->get_remaining_times(w, b);
        (void)room->to_pgn();

        return room->revision() == after_join;
    });

    run_test("resign bumps revision at least once", []() {
        RoomManager rm;
        auto room = rm.create_room(6, "alice", 400);
        room->join(7, "bob", 401);
        const uint64_t before = room->revision();
        room->resign(400);
        return room->revision() > before;
    });

    run_test("add_spectator + remove_spectator each bump revision", []() {
        RoomManager rm;
        auto room = rm.create_room(8, "alice", 500);
        room->join(9, "bob", 501);
        const uint64_t r0 = room->revision();

        room->add_spectator(600);
        const uint64_t r1 = room->revision();
        if (!(r1 > r0)) return false;

        room->remove_spectator(600);
        const uint64_t r2 = room->revision();
        return r2 > r1;
    });

    // ── GameSnapshot carries the revision at build time ────────────────

    run_test("GameSnapshot.revision matches room->revision() at completion", []() {
        RoomManager rm;
        auto room = rm.create_room(10, "alice", 700);
        auto cap = std::make_shared<SnapshotCapture>();
        room->add_listener(cap);
        room->join(11, "bob", 701);
        // At completion the snapshot is built inside the mutex; the
        // room's revision must be >= the snapshot's (equal in the
        // synchronous case, could be > if another mutation raced —
        // it can't here because we're single-threaded).
        room->resign(700);
        return cap->fires == 1
            && cap->last.revision > 0
            && cap->last.revision == room->revision();
    });

    // ── summary ────────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
