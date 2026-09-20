/**
 * test_game_events.cpp — LLD-4.1.
 *
 * Verifies the listener plumbing on `GameRoom`:
 *
 *   1. `add_listener` / `remove_listener` are idempotent and return
 *      correct booleans.
 *   2. `GameStarted` fires on WAITING → IN_PROGRESS (join).
 *   3. `GameCompleted` fires on every terminal transition:
 *        - resignation, checkmate, timeout (via checked move + expired
 *          clock is out of scope for a fast test — we use resign).
 *      Snapshot fields match the room's own accessors and history.
 *   4. Snapshot is delivered AFTER the room mutex is released — a
 *      listener that re-enters the room does not deadlock.
 *   5. Listener failure isolation: one listener throwing doesn't skip
 *      others; the room keeps functioning.
 *   6. Snapshot is stable — a listener that inspects the snapshot
 *      later (after the room mutates) sees the terminal values, not
 *      whatever came next.
 *
 * No DB, no sockets — pure logic against a real GameRoom.
 */

#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"
#include "game/game_events.h"
#include "game/game_room.h"
#include "game/game_snapshot.h"

using namespace chess;
using namespace chess::game;

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

/// Capturing listener — records every event fired at it.
struct RecordingListener : GameEventListener {
    std::vector<GameStarted>   started;
    std::vector<GameCompleted> completed;

    void on_game_started(const GameStarted& ev) override {
        started.push_back(ev);
    }
    void on_game_completed(const GameCompleted& ev) override {
        completed.push_back(ev);
    }
};

/// Throws on every event. Used for listener-failure-isolation test.
struct ThrowingListener : GameEventListener {
    void on_game_started(const GameStarted&)   override { throw std::runtime_error("boom"); }
    void on_game_completed(const GameCompleted&) override { throw std::runtime_error("boom"); }
};

/// Re-enters the room from inside `on_game_completed`. Verifies
/// callbacks run outside the room lock (else this deadlocks).
struct ReentrantListener : GameEventListener {
    GameRoom* target = nullptr;
    bool re_entered  = false;
    void on_game_completed(const GameCompleted&) override {
        if (target) {
            // Any read accessor would do — spectator_count uses LockAndDrain.
            (void)target->spectator_count();
            re_entered = true;
        }
    }
};

/// Build + fill a room (two humans, both connected).
std::shared_ptr<GameRoom> make_room(GameId id = 1) {
    auto room = std::make_shared<GameRoom>(
        id, /*creator*/1, "alice", /*fd*/10, TimeControl{}, /*db*/100, /*elo*/1500);
    room->join(/*pid*/2, "bob", /*fd*/11, /*db*/200, /*elo*/1400);
    return room;
}

} // namespace

int main() {
    std::cout << "Running GameEvents tests (LLD-4.1)...\n";

    run_test("add_listener + remove_listener are idempotent", [] {
        auto room = std::make_shared<GameRoom>(
            42, 1, "alice", 10, TimeControl{}, 100, 1500);
        auto L = std::make_shared<RecordingListener>();
        room->add_listener(L);
        // remove twice — first true, second false.
        if (!room->remove_listener(L)) return false;
        if ( room->remove_listener(L)) return false;
        // remove of never-registered listener → false.
        auto L2 = std::make_shared<RecordingListener>();
        if (room->remove_listener(L2)) return false;
        return true;
    });

    run_test("GameStarted fires on join (WAITING → IN_PROGRESS)", [] {
        auto room = std::make_shared<GameRoom>(
            7, 1, "alice", 10, TimeControl{}, 100, 1500);
        auto L = std::make_shared<RecordingListener>();
        room->add_listener(L);
        room->join(2, "bob", 11, 200, 1400);
        if (L->started.size() != 1) return false;
        const auto& ev = L->started[0];
        return ev.room_id == 7 && ev.white_db_id == 100
            && ev.black_db_id == 200 && !ev.is_ai_game;
    });

    run_test("GameCompleted fires on resign; snapshot matches", [] {
        auto room = make_room(11);
        auto L = std::make_shared<RecordingListener>();
        room->add_listener(L);
        room->resign(/*white_fd*/10);
        if (L->completed.size() != 1) return false;
        const auto& s = L->completed[0].snapshot;
        return s.room_id == 11
            && s.status == GameStatus::RESIGNATION
            && s.result == "0-1"   // white resigned → black wins
            && s.white.username == "alice"
            && s.black.username == "bob"
            && s.white.db_player_id == 100
            && s.black.db_player_id == 200
            && s.move_count == 0
            && !s.time_control.to_string().empty();
    });

    run_test("GameCompleted fires on checkmate via submit_move", [] {
        // Fool's mate: 1.f3 e5 2.g4 Qh4#
        auto room = make_room(13);
        auto L = std::make_shared<RecordingListener>();
        room->add_listener(L);
        // white f2->f3
        auto r1 = room->submit_move(10, static_cast<Square>(13), static_cast<Square>(21));
        if (!r1.success) return false;
        // black e7->e5
        auto r2 = room->submit_move(11, static_cast<Square>(52), static_cast<Square>(36));
        if (!r2.success) return false;
        // white g2->g4
        auto r3 = room->submit_move(10, static_cast<Square>(14), static_cast<Square>(30));
        if (!r3.success) return false;
        // black Qd8-h4#
        auto r4 = room->submit_move(11, static_cast<Square>(59), static_cast<Square>(31));
        if (!r4.success) return false;
        if (r4.game_status != GameStatus::CHECKMATE) return false;
        if (L->completed.size() != 1) return false;
        const auto& s = L->completed[0].snapshot;
        return s.status == GameStatus::CHECKMATE
            && s.result == "0-1"   // white checkmated → black wins
            && s.move_count == 4;
    });

    run_test("Listener callback runs outside room mutex (no deadlock)", [] {
        auto room = make_room(17);
        auto R = std::make_shared<ReentrantListener>();
        R->target = room.get();
        room->add_listener(R);
        room->resign(10);
        return R->re_entered;
    });

    run_test("Listener failure isolation: throw does not skip others", [] {
        auto room = make_room(19);
        auto bad  = std::make_shared<ThrowingListener>();
        auto good = std::make_shared<RecordingListener>();
        room->add_listener(bad);   // fires first
        room->add_listener(good);  // still fires
        room->resign(10);
        return good->completed.size() == 1;
    });

    run_test("Snapshot is stable — captured at terminal transition", [] {
        auto room = make_room(23);
        auto L = std::make_shared<RecordingListener>();
        room->add_listener(L);
        room->resign(10);
        // Mutate the room (add a spectator) after the completion event.
        room->add_spectator(99);  // will refuse (state is FINISHED), OK
        // Snapshot recorded before mutation must still show the terminal state.
        return L->completed.size() == 1
            && L->completed[0].snapshot.status == GameStatus::RESIGNATION;
    });

    run_test("remove_listener during no-op drain does not disturb state", [] {
        auto room = std::make_shared<GameRoom>(
            29, 1, "alice", 10, TimeControl{}, 100, 1500);
        auto L = std::make_shared<RecordingListener>();
        room->add_listener(L);
        // spectator_count uses LockAndDrain but no pending event exists.
        (void)room->spectator_count();
        return L->started.empty() && L->completed.empty()
            && room->remove_listener(L) == true;
    });

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
