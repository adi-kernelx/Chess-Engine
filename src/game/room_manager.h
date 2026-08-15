/**
 * room_manager.h — Thread-safe manager for all active game rooms
 *
 * The RoomManager is the central registry of all games on the server.
 * It provides:
 *   - Room creation (returns a unique GameId)
 *   - Room lookup by ID
 *   - Room lookup by player connection fd
 *   - Room listing (for lobby display)
 *   - Room cleanup (remove finished/abandoned rooms)
 *
 * Thread safety: Uses a mutex to protect the rooms map. Each GameRoom
 * also has its own internal mutex, so operations on different rooms can
 * proceed in parallel — only the map lookup/insertion is serialized.
 *
 * Uses shared_ptr so rooms can be referenced safely by multiple threads
 * (e.g., the epoll thread looking up a room while a worker thread is
 * processing a move in the same room).
 */

#pragma once

#include "game/game_room.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace chess {
namespace game {

/// Summary info for a room (used in lobby listings)
struct RoomInfo {
    GameId      id;
    RoomState   state;
    std::string white_name;
    std::string black_name;
    std::string time_control;
    int         move_count;
};

class RoomManager {
public:
    RoomManager() = default;

    // --------------------------------------------------------
    // Room lifecycle
    // --------------------------------------------------------

    /// Create a new game room. Returns the room (already contains the creator as White).
    std::shared_ptr<GameRoom> create_room(PlayerId creator_id,
                                          const std::string& creator_name,
                                          int creator_fd,
                                          const TimeControl& tc = TimeControl());

    /// Create an AI game room. Human plays White, AI plays Black.
    /// The game starts immediately in IN_PROGRESS state.
    std::shared_ptr<GameRoom> create_ai_room(PlayerId creator_id,
                                              const std::string& creator_name,
                                              int creator_fd,
                                              const TimeControl& tc,
                                              AIDifficulty difficulty);

    /// Find a room by its ID. Returns nullptr if not found.
    std::shared_ptr<GameRoom> find_room(GameId id) const;

    /// Find the room that a given connection fd belongs to.
    /// Returns nullptr if the player is not in any room.
    std::shared_ptr<GameRoom> find_room_by_fd(int connection_fd) const;

    /// Find the room that a given player ID belongs to.
    std::shared_ptr<GameRoom> find_room_by_player(PlayerId player_id) const;

    /// Remove a room from the manager (e.g., after game is finished and saved).
    void remove_room(GameId id);

    // --------------------------------------------------------
    // Queries
    // --------------------------------------------------------

    /// List all rooms that are waiting for a second player (for lobby display).
    std::vector<RoomInfo> list_open_rooms() const;

    /// List all rooms currently in progress.
    std::vector<RoomInfo> list_active_rooms() const;

    /// Get the total number of rooms (all states).
    size_t room_count() const;

    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    /// Remove all rooms in FINISHED state. Returns number of rooms removed.
    size_t cleanup_finished_rooms();

private:
    mutable std::mutex mutex_;
    std::unordered_map<GameId, std::shared_ptr<GameRoom>> rooms_;
    std::atomic<GameId> next_id_{1};  // Monotonically increasing room ID counter
};

} // namespace game
} // namespace chess
