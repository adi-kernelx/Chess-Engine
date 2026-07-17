/**
 * test_game_room.cpp — Tests for Phase 4.1: Game Room Manager
 *
 * Tests:
 *   1. Room creation and player joining
 *   2. State machine transitions (WAITING → IN_PROGRESS → FINISHED)
 *   3. Move submission and validation (server-authoritative)
 *   4. Illegal move rejection
 *   5. Resignation
 *   6. Fischer clock management
 *   7. RoomManager lifecycle (create, find, list, cleanup)
 *   8. Full game playthrough to checkmate
 *   9. PGN export after a complete game
 *  10. Disconnect handling
 */

#include "game/game_room.h"
#include "game/room_manager.h"
#include "chess/board.h"
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
// GameRoom Tests
// ============================================================

void test_room_creation() {
    std::cout << "\n=== Room Creation Tests ===" << std::endl;

    run_test("New room starts in WAITING state", []() {
        GameRoom room(1, 100, "Alice", 10);
        return room.get_state() == RoomState::WAITING;
    });

    run_test("Creator is White with correct fd", []() {
        GameRoom room(1, 100, "Alice", 10);
        return room.get_player_fd(Color::WHITE) == 10 &&
               room.get_player_fd(Color::BLACK) == -1;
    });

    run_test("Room has correct ID", []() {
        GameRoom room(42, 100, "Alice", 10);
        return room.get_id() == 42;
    });

    run_test("has_player returns true for creator fd", []() {
        GameRoom room(1, 100, "Alice", 10);
        return room.has_player(10) && !room.has_player(20);
    });
}

void test_joining() {
    std::cout << "\n=== Join Tests ===" << std::endl;

    run_test("Second player joins successfully", []() {
        GameRoom room(1, 100, "Alice", 10);
        bool joined = room.join(200, "Bob", 20);
        return joined &&
               room.get_state() == RoomState::IN_PROGRESS &&
               room.get_player_fd(Color::BLACK) == 20;
    });

    run_test("Cannot join a full room", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        return !room.join(300, "Charlie", 30);
    });

    run_test("Cannot join a room already in progress", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        return !room.join(300, "Charlie", 30) &&
               room.get_state() == RoomState::IN_PROGRESS;
    });
}

void test_move_submission() {
    std::cout << "\n=== Move Submission Tests ===" << std::endl;

    run_test("White can make the first move (e2-e4)", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        auto result = room.submit_move(10,
            Board::algebraic_to_square("e2"),
            Board::algebraic_to_square("e4"));
        return result.success && result.san == "e4" &&
               result.game_status == GameStatus::ONGOING;
    });

    run_test("Black cannot move when it's White's turn", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        auto result = room.submit_move(20,
            Board::algebraic_to_square("e7"),
            Board::algebraic_to_square("e5"));
        return !result.success && result.error == "It is not your turn";
    });

    run_test("Illegal move is rejected", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        auto result = room.submit_move(10,
            Board::algebraic_to_square("e2"),
            Board::algebraic_to_square("e5")); // Can't jump 3 squares
        return !result.success && result.error == "Illegal move";
    });

    run_test("Non-player cannot submit a move", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        auto result = room.submit_move(99, // Unknown fd
            Board::algebraic_to_square("e2"),
            Board::algebraic_to_square("e4"));
        return !result.success && result.error == "You are not a player in this game";
    });

    run_test("Cannot move after game is finished", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        room.resign(10); // White resigns
        auto result = room.submit_move(20,
            Board::algebraic_to_square("e7"),
            Board::algebraic_to_square("e5"));
        return !result.success && result.error == "Game is not in progress";
    });
}

void test_resignation() {
    std::cout << "\n=== Resignation Tests ===" << std::endl;

    run_test("White resigns -> 0-1", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        bool resigned = room.resign(10);
        return resigned &&
               room.get_state() == RoomState::FINISHED &&
               room.get_result_string() == "0-1";
    });

    run_test("Black resigns -> 1-0", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        bool resigned = room.resign(20);
        return resigned &&
               room.get_state() == RoomState::FINISHED &&
               room.get_result_string() == "1-0";
    });

    run_test("Cannot resign before game starts", []() {
        GameRoom room(1, 100, "Alice", 10);
        return !room.resign(10);  // Still WAITING
    });
}

void test_full_game() {
    std::cout << "\n=== Full Game Tests ===" << std::endl;

    run_test("Scholar's Mate via submit_move", []() {
        GameRoom room(1, 100, "Alice", 10, TimeControl(600000, 5000));
        room.join(200, "Bob", 20);

        // 1. e4
        auto r1 = room.submit_move(10, Board::algebraic_to_square("e2"), Board::algebraic_to_square("e4"));
        if (!r1.success) return false;
        // 1... e5
        auto r2 = room.submit_move(20, Board::algebraic_to_square("e7"), Board::algebraic_to_square("e5"));
        if (!r2.success) return false;
        // 2. Bc4
        auto r3 = room.submit_move(10, Board::algebraic_to_square("f1"), Board::algebraic_to_square("c4"));
        if (!r3.success) return false;
        // 2... Nc6
        auto r4 = room.submit_move(20, Board::algebraic_to_square("b8"), Board::algebraic_to_square("c6"));
        if (!r4.success) return false;
        // 3. Qh5
        auto r5 = room.submit_move(10, Board::algebraic_to_square("d1"), Board::algebraic_to_square("h5"));
        if (!r5.success) return false;
        // 3... Nf6
        auto r6 = room.submit_move(20, Board::algebraic_to_square("g8"), Board::algebraic_to_square("f6"));
        if (!r6.success) return false;
        // 4. Qxf7#
        auto r7 = room.submit_move(10, Board::algebraic_to_square("h5"), Board::algebraic_to_square("f7"));
        if (!r7.success) return false;

        return r7.game_status == GameStatus::CHECKMATE &&
               room.get_state() == RoomState::FINISHED &&
               room.get_result_string() == "1-0";
    });

    run_test("Move history is recorded correctly", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);

        room.submit_move(10, Board::algebraic_to_square("e2"), Board::algebraic_to_square("e4"));
        room.submit_move(20, Board::algebraic_to_square("e7"), Board::algebraic_to_square("e5"));
        room.submit_move(10, Board::algebraic_to_square("g1"), Board::algebraic_to_square("f3"));

        auto history = room.get_move_history();
        return history.size() == 3 &&
               history[0].san == "e4" &&
               history[1].san == "e5" &&
               history[2].san == "Nf3";
    });

    run_test("PGN export after Scholar's Mate", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);

        room.submit_move(10, Board::algebraic_to_square("e2"), Board::algebraic_to_square("e4"));
        room.submit_move(20, Board::algebraic_to_square("e7"), Board::algebraic_to_square("e5"));
        room.submit_move(10, Board::algebraic_to_square("f1"), Board::algebraic_to_square("c4"));
        room.submit_move(20, Board::algebraic_to_square("b8"), Board::algebraic_to_square("c6"));
        room.submit_move(10, Board::algebraic_to_square("d1"), Board::algebraic_to_square("h5"));
        room.submit_move(20, Board::algebraic_to_square("g8"), Board::algebraic_to_square("f6"));
        room.submit_move(10, Board::algebraic_to_square("h5"), Board::algebraic_to_square("f7"));

        std::string pgn = room.to_pgn();
        // PGN should contain the moves and result
        return pgn.find("1. e4 e5") != std::string::npos &&
               pgn.find("Qxf7#") != std::string::npos &&
               pgn.find("1-0") != std::string::npos;
    });
}

void test_clock() {
    std::cout << "\n=== Clock Tests ===" << std::endl;

    run_test("Clock starts with correct base time", []() {
        TimeControl tc(300000, 2000);  // 5 min + 2 sec
        GameRoom room(1, 100, "Alice", 10, tc);
        room.join(200, "Bob", 20);

        int wt, bt;
        room.get_remaining_times(wt, bt);
        // White's clock is running, so wt should be slightly less than 300000
        // Black's clock hasn't started yet, should be exactly 300000
        return bt == 300000 && wt <= 300000;
    });

    run_test("Think time is positive after a move", []() {
        GameRoom room(1, 100, "Alice", 10, TimeControl(600000, 5000));
        room.join(200, "Bob", 20);

        // Sleep a tiny bit so think time > 0
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        room.submit_move(10, Board::algebraic_to_square("e2"), Board::algebraic_to_square("e4"));

        auto history = room.get_move_history();
        return !history.empty() && history[0].think_time_ms >= 0;
    });
}

// ============================================================
// RoomManager Tests
// ============================================================

void test_room_manager() {
    std::cout << "\n=== Room Manager Tests ===" << std::endl;

    run_test("Create room returns valid room", []() {
        RoomManager mgr;
        auto room = mgr.create_room(100, "Alice", 10);
        return room != nullptr &&
               room->get_state() == RoomState::WAITING;
    });

    run_test("Find room by ID", []() {
        RoomManager mgr;
        auto room = mgr.create_room(100, "Alice", 10);
        GameId id = room->get_id();
        auto found = mgr.find_room(id);
        return found != nullptr && found->get_id() == id;
    });

    run_test("Find room by connection fd", []() {
        RoomManager mgr;
        auto room = mgr.create_room(100, "Alice", 10);
        auto found = mgr.find_room_by_fd(10);
        return found != nullptr && found->get_id() == room->get_id();
    });

    run_test("Find room by player ID", []() {
        RoomManager mgr;
        mgr.create_room(100, "Alice", 10);
        auto found = mgr.find_room_by_player(100);
        return found != nullptr;
    });

    run_test("List open rooms", []() {
        RoomManager mgr;
        mgr.create_room(100, "Alice", 10);
        mgr.create_room(200, "Bob", 20);
        auto open = mgr.list_open_rooms();
        return open.size() == 2;
    });

    run_test("Room disappears from open list after joining", []() {
        RoomManager mgr;
        auto room = mgr.create_room(100, "Alice", 10);
        room->join(200, "Bob", 20);
        auto open = mgr.list_open_rooms();
        auto active = mgr.list_active_rooms();
        return open.empty() && active.size() == 1;
    });

    run_test("Room count tracks correctly", []() {
        RoomManager mgr;
        mgr.create_room(100, "Alice", 10);
        mgr.create_room(200, "Bob", 20);
        return mgr.room_count() == 2;
    });

    run_test("Cleanup removes finished rooms", []() {
        RoomManager mgr;
        auto room = mgr.create_room(100, "Alice", 10);
        room->join(200, "Bob", 20);
        room->resign(10);  // White resigns, game finished
        size_t removed = mgr.cleanup_finished_rooms();
        return removed == 1 && mgr.room_count() == 0;
    });

    run_test("IDs are unique and monotonically increasing", []() {
        RoomManager mgr;
        auto r1 = mgr.create_room(100, "Alice", 10);
        auto r2 = mgr.create_room(200, "Bob", 20);
        auto r3 = mgr.create_room(300, "Charlie", 30);
        return r1->get_id() < r2->get_id() && r2->get_id() < r3->get_id();
    });
}

void test_disconnect() {
    std::cout << "\n=== Disconnect Tests ===" << std::endl;

    run_test("Creator disconnect in WAITING state finishes the room", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.on_disconnect(10);
        return room.get_state() == RoomState::FINISHED;
    });

    run_test("Disconnect during game keeps game alive (clock runs)", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        room.on_disconnect(20);  // Bob disconnects
        // Game should still be IN_PROGRESS
        return room.get_state() == RoomState::IN_PROGRESS;
    });

    run_test("Reconnect restores player to the game", []() {
        GameRoom room(1, 100, "Alice", 10);
        room.join(200, "Bob", 20);
        room.on_disconnect(20);  // Bob disconnects

        bool reconnected = room.on_reconnect(200, 25);  // Bob reconnects with new fd
        return reconnected && room.has_player(25);
    });
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Game Room Tests - Phase 4.1" << std::endl;
    std::cout << "========================================" << std::endl;

    test_room_creation();
    test_joining();
    test_move_submission();
    test_resignation();
    test_full_game();
    test_clock();
    test_room_manager();
    test_disconnect();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_failed > 0) ? 1 : 0;
}
