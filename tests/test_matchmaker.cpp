/**
 * test_matchmaker.cpp — Tests for Phase 4.3: Quick-Play Matchmaking
 *
 * Tests:
 *   1. Enqueueing and dequeueing players
 *   2. Matching two players with same ELO
 *   3. Matching with ELO difference within range
 *   4. Rejecting matches outside ELO range
 *   5. Range widening over time
 *   6. Time control compatibility (must match exactly)
 *   7. Multiple simultaneous matches (6 players → 3 games)
 *   8. Duplicate enqueue prevention
 *   9. Match callback invocation
 *  10. Dequeue on disconnect
 */

#include "game/matchmaker.h"
#include "game/room_manager.h"
#include <iostream>
#include <functional>
#include <string>
#include <thread>
#include <chrono>

using namespace chess;
using namespace chess::game;

static int g_passed = 0;
static int g_failed = 0;

void run_test(const std::string& name, std::function<bool()> test_fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (test_fn()) {
        std::cout << "PASS" << std::endl;
        g_passed++;
    } else {
        std::cout << "FAIL" << std::endl;
        g_failed++;
    }
}

// ============================================================
// Matchmaker Tests
// ============================================================

void test_enqueue_dequeue() {
    std::cout << "\n=== Enqueue / Dequeue Tests ===" << std::endl;

    run_test("Enqueue a player", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        bool ok = mm.enqueue(10, 1, "Alice", 1200);
        return ok && mm.queue_size() == 1;
    });

    run_test("Duplicate enqueue is rejected", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        bool dup = mm.enqueue(10, 1, "Alice", 1200);
        return !dup && mm.queue_size() == 1;
    });

    run_test("Dequeue removes the player", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        bool ok = mm.dequeue(10);
        return ok && mm.queue_size() == 0;
    });

    run_test("Dequeue non-existent player returns false", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        return !mm.dequeue(99);
    });

    run_test("is_queued works correctly", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        return mm.is_queued(10) && !mm.is_queued(20);
    });
}

void test_matching() {
    std::cout << "\n=== Matching Tests ===" << std::endl;

    run_test("Two players with same ELO are matched", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        mm.enqueue(20, 2, "Bob",   1200);
        auto matches = mm.try_match();
        return matches.size() == 1 &&
               mm.queue_size() == 0 &&
               mgr.room_count() == 1;
    });

    run_test("Matched game is IN_PROGRESS", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        mm.enqueue(20, 2, "Bob",   1200);
        auto matches = mm.try_match();
        auto room = mgr.find_room(matches[0].game_id);
        return room != nullptr && room->get_state() == RoomState::IN_PROGRESS;
    });

    run_test("Match result has correct player info", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        mm.enqueue(20, 2, "Bob",   1200);
        auto matches = mm.try_match();
        return matches[0].white_fd == 10 &&
               matches[0].black_fd == 20 &&
               matches[0].white_name == "Alice" &&
               matches[0].black_name == "Bob";
    });

    run_test("Players within ELO range are matched", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1100);
        mm.enqueue(20, 2, "Bob",   1250);  // diff = 150, within ±200
        auto matches = mm.try_match();
        return matches.size() == 1;
    });

    run_test("Players outside ELO range are NOT matched", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 800);
        mm.enqueue(20, 2, "Bob",   1500);  // diff = 700, outside ±200
        auto matches = mm.try_match();
        return matches.empty() && mm.queue_size() == 2;
    });

    run_test("Single player in queue - no match", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200);
        auto matches = mm.try_match();
        return matches.empty() && mm.queue_size() == 1;
    });
}

void test_time_control_compat() {
    std::cout << "\n=== Time Control Compatibility Tests ===" << std::endl;

    run_test("Same time control matches", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200, TimeControl(600000, 5000));
        mm.enqueue(20, 2, "Bob",   1200, TimeControl(600000, 5000));
        auto matches = mm.try_match();
        return matches.size() == 1;
    });

    run_test("Different time controls do NOT match", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice", 1200, TimeControl(600000, 5000));   // 10+5
        mm.enqueue(20, 2, "Bob",   1200, TimeControl(180000, 2000));   // 3+2
        auto matches = mm.try_match();
        return matches.empty() && mm.queue_size() == 2;
    });
}

void test_multiple_matches() {
    std::cout << "\n=== Multiple Match Tests ===" << std::endl;

    run_test("6 players → 3 games", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice",   1200);
        mm.enqueue(20, 2, "Bob",     1210);
        mm.enqueue(30, 3, "Charlie", 1190);
        mm.enqueue(40, 4, "Diana",   1205);
        mm.enqueue(50, 5, "Eve",     1195);
        mm.enqueue(60, 6, "Frank",   1215);
        auto matches = mm.try_match();
        return matches.size() == 3 &&
               mm.queue_size() == 0 &&
               mgr.room_count() == 3;
    });

    run_test("5 players → 2 games, 1 left over", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);
        mm.enqueue(10, 1, "Alice",   1200);
        mm.enqueue(20, 2, "Bob",     1200);
        mm.enqueue(30, 3, "Charlie", 1200);
        mm.enqueue(40, 4, "Diana",   1200);
        mm.enqueue(50, 5, "Eve",     1200);
        auto matches = mm.try_match();
        return matches.size() == 2 &&
               mm.queue_size() == 1 &&
               mgr.room_count() == 2;
    });
}

void test_match_callback() {
    std::cout << "\n=== Match Callback Tests ===" << std::endl;

    run_test("Callback is invoked on match", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);

        bool callback_fired = false;
        GameId callback_game_id = 0;
        mm.set_match_callback([&](const MatchResult& result) {
            callback_fired = true;
            callback_game_id = result.game_id;
        });

        mm.enqueue(10, 1, "Alice", 1200);
        mm.enqueue(20, 2, "Bob",   1200);
        mm.try_match();

        return callback_fired && callback_game_id > 0;
    });

    run_test("Callback is NOT invoked when no match", []() {
        RoomManager mgr;
        Matchmaker mm(mgr);

        bool callback_fired = false;
        mm.set_match_callback([&](const MatchResult&) {
            callback_fired = true;
        });

        mm.enqueue(10, 1, "Alice", 1200);
        mm.try_match();  // Only 1 player, no match

        return !callback_fired;
    });
}

void test_elo_range_widening() {
    std::cout << "\n=== ELO Range Widening Tests ===" << std::endl;

    run_test("QueueEntry range widens over time", []() {
        QueueEntry entry;
        entry.elo = 1200;
        // Simulate enqueuing 20 seconds ago
        entry.enqueue_time = std::chrono::steady_clock::now() - std::chrono::seconds(20);

        // After 20 seconds: base 200 + (20/10)*50 = 200 + 100 = 300
        int range = entry.acceptable_range();
        return range == 300;
    });

    run_test("Fresh entry has base range", []() {
        QueueEntry entry;
        entry.elo = 1200;
        entry.enqueue_time = std::chrono::steady_clock::now();

        return entry.acceptable_range() == 200;
    });
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Matchmaker Tests - Phase 4.3" << std::endl;
    std::cout << "========================================" << std::endl;

    test_enqueue_dequeue();
    test_matching();
    test_time_control_compat();
    test_multiple_matches();
    test_match_callback();
    test_elo_range_widening();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
