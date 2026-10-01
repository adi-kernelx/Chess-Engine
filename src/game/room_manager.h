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
#include <unordered_set>
#include <vector>
#include <atomic>

// Forward declarations for LLD-4.2 default-listener attach hook.
// GameEventListenerPtr is defined in game/game_events.h, which pulls in
// game/game_snapshot.h — leaving that include here would drag both
// through every translation unit that already sees game_room.h. Every
// site that attaches a listener already includes the events header
// directly.

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
    int         spectator_count = 0;   ///< Phase 9.1 — how many watchers this room currently has
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
                                           const TimeControl& tc = TimeControl(),
                                           int64_t db_player_id = 0,
                                           int elo = 1200);

    /// Create an AI game room. Human plays White, AI plays Black.
    /// The game starts immediately in IN_PROGRESS state.
    std::shared_ptr<GameRoom> create_ai_room(PlayerId creator_id,
                                              const std::string& creator_name,
                                              int creator_fd,
                                              const TimeControl& tc,
                                              AIDifficulty difficulty,
                                              int64_t db_player_id = 0,
                                              int elo = 1200);

    std::shared_ptr<GameRoom> create_reserved_tournament_room(
        int64_t tournament_id, int64_t pairing_id,
        int64_t white_db_id, const std::string& white_name, int white_elo,
        int64_t black_db_id, const std::string& black_name, int black_elo,
        const TimeControl& tc);

    std::shared_ptr<GameRoom> find_room_by_pairing(int64_t pairing_id) const;

    /// Ensure newly allocated runtime ids cannot collide with mappings loaded
    /// from an earlier process. `next_id` itself is never allocated here.
    void ensure_next_id_above(GameId existing_id);
    void reserve_tournament_player(int64_t db_player_id);
    void release_tournament_player(int64_t db_player_id);
    bool is_tournament_player_reserved(int64_t db_player_id) const;

    /// Find a room by its ID. Returns nullptr if not found.
    std::shared_ptr<GameRoom> find_room(GameId id) const;

    /// Find the room that a given connection fd belongs to.
    /// Returns nullptr if the player is not in any room.
    std::shared_ptr<GameRoom> find_room_by_fd(int connection_fd) const;

    /// Find the room that a given player ID belongs to.
    std::shared_ptr<GameRoom> find_room_by_player(PlayerId player_id) const;

    /// Find a non-finished room by durable database player identity.
    std::shared_ptr<GameRoom> find_room_by_db_player(int64_t db_player_id) const;

    /// Snapshot room ownership for maintenance passes. Returned shared_ptrs
    /// keep rooms alive after the manager lock is released.
    std::vector<std::shared_ptr<GameRoom>> rooms_snapshot() const;

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

    // --------------------------------------------------------
    // Spectators (Phase 9.1)
    // --------------------------------------------------------

    /// Drop `fd` from every room's spectator list. Called by GameHandler on
    /// player disconnect so a spectator who closes the tab or drops the
    /// socket stops accumulating undelivered broadcasts in its dead write
    /// buffer. Returns the number of rooms that actually removed the fd.
    size_t remove_spectator_everywhere(int connection_fd);

    // --------------------------------------------------------
    // LLD-4.2 default listener
    // --------------------------------------------------------

    /// Register a listener that will be automatically attached to every
    /// room created AFTER this call via `create_room` / `create_ai_room`.
    /// Passing a `nullptr` clears the default. Composition-time hook:
    /// production wires the `GameCompletionService` here so persistence
    /// runs off the `GameCompleted` event without every service having
    /// to remember to add it. Only ever expected to be called once per
    /// manager (composition root), so a plain reassign suffices.
    void set_default_listener(GameEventListenerPtr listener);

private:
    mutable std::mutex mutex_;
    std::unordered_map<GameId, std::shared_ptr<GameRoom>> rooms_;
    std::unordered_map<int64_t, GameId> tournament_pairing_rooms_;
    std::unordered_set<int64_t> reserved_tournament_players_;
    std::atomic<GameId> next_id_{1};  // Monotonically increasing room ID counter

    /// LLD-4.2 default listener — attached to every future room.
    /// Guarded by `mutex_` alongside the rooms map. May be null (no
    /// default listener wired, e.g. in an in-process test that doesn't
    /// exercise the completion pipeline).
    GameEventListenerPtr default_listener_;
};

} // namespace game
} // namespace chess
