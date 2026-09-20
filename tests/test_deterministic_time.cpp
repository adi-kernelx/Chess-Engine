/**
 * test_deterministic_time.cpp — LLD-6.3 clock-driven determinism.
 *
 * Before this slice, GameRoom's Fischer decrement and Matchmaker's
 * ELO-fence widening both read `std::chrono::steady_clock::now()`
 * directly. Any test that wanted to prove "after 15 seconds, the
 * fence widens by +50" had to actually sleep for 15 seconds — slow,
 * flaky under load. This test drives both consumers via a
 * FakeClock: `advance(seconds(N))` moves the clock instantaneously
 * and the timing behaviour lands exactly where the math says.
 */

#include <chrono>
#include <functional>
#include <iostream>
#include <string>

#include "application/ports/clock.h"
#include "game/game_room.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"

using chess::application::ports::FakeClock;
using chess::game::GameRoom;
using chess::game::Matchmaker;
using chess::game::RoomManager;
using chess::game::TimeControl;
using std::chrono::milliseconds;
using std::chrono::seconds;

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

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-6.3 — deterministic clock consumers\n";
    std::cout << "========================================\n";

    // ── GameRoom Fischer decrement under a FakeClock ────────────────

    run_test("GameRoom Fischer decrement matches advance(delta) exactly", []() {
        FakeClock clk;
        // 60s + 0s increment — makes the arithmetic obvious.
        RoomManager rm;
        auto room = rm.create_room(1, "alice", 100, TimeControl(60000, 0));
        room->set_clock(&clk);
        // Bob joins → game starts, White's clock begins running.
        room->join(2, "bob", 101);
        int w = 0, b = 0;
        room->get_remaining_times(w, b);
        if (!(w == 60000 && b == 60000)) return false;

        // 10 seconds pass with no move. White's clock should decrement,
        // Black's should not.
        clk.advance(seconds(10));
        room->get_remaining_times(w, b);
        return w == 50000 && b == 60000;
    });

    run_test("GameRoom clock is unchanged when advance is not called", []() {
        FakeClock clk;
        RoomManager rm;
        auto room = rm.create_room(2, "alice", 200, TimeControl(60000, 0));
        room->set_clock(&clk);
        room->join(3, "bob", 201);
        // No advance → active side's remaining stays flat.
        int w1 = 0, b1 = 0;
        room->get_remaining_times(w1, b1);
        int w2 = 0, b2 = 0;
        room->get_remaining_times(w2, b2);
        return w1 == w2 && b1 == b2;
    });

    // ── Matchmaker ELO widening under a FakeClock ───────────────────

    run_test("Matchmaker enqueue_time comes from the injected clock", []() {
        FakeClock clk;
        RoomManager rm;
        Matchmaker mm(rm);
        mm.set_clock(&clk);
        // A player enqueues at clock-instant 0; we don't observe the
        // queue entry directly, but try_match should NOT pair them at
        // this instant (no one else to pair with).
        (void)mm.enqueue(300, 10, "alice", 1200, TimeControl(600000, 5000));
        auto matches = mm.try_match();
        return matches.empty() && mm.queue_size() == 1;
    });

    run_test("Fence widens by exactly +50 every 10s (FakeClock advance)", []() {
        FakeClock clk;
        // Two players 250 ELO apart. Base fence is ±200; they should
        // NOT match at t=0 (250 > 200), NOT at t=9s (still 250 > 200),
        // but SHOULD match at t=10s once the fence widens to 250.
        RoomManager rm;
        Matchmaker mm(rm);
        mm.set_clock(&clk);
        TimeControl tc(600000, 5000);
        mm.enqueue(400, 20, "alice", 1200, tc);
        mm.enqueue(401, 21, "bob",   1450, tc);

        // t = 0: no match.
        if (!mm.try_match().empty()) return false;

        // t = 9s: still no match. Fence math: 9/10 = 0, widening = 0.
        clk.advance(seconds(9));
        if (!mm.try_match().empty()) return false;

        // t = 10s: widening = 50, fence = 250. 1450 - 1200 = 250. Match.
        clk.advance(seconds(1));
        auto m = mm.try_match();
        return m.size() == 1;
    });

    // ── summary ─────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
