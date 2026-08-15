/**
 * room_manager.cpp — Thread-safe game room registry
 *
 * The RoomManager uses a single mutex for the rooms map and relies on
 * each GameRoom's internal mutex for per-game thread safety. This means:
 *   - Lookups are serialized (short critical section)
 *   - Game operations (moves, clock updates) happen in parallel
 *
 * GameId allocation uses std::atomic for lock-free increment.
 */

#include "game/room_manager.h"

namespace chess {
namespace game {

// ============================================================
// Room Lifecycle
// ============================================================

std::shared_ptr<GameRoom> RoomManager::create_room(PlayerId creator_id,
                                                    const std::string& creator_name,
                                                    int creator_fd,
                                                    const TimeControl& tc) {
    GameId id = next_id_.fetch_add(1);

    auto room = std::make_shared<GameRoom>(id, creator_id, creator_name, creator_fd, tc);

    std::lock_guard<std::mutex> lock(mutex_);
    rooms_[id] = room;
    return room;
}

std::shared_ptr<GameRoom> RoomManager::create_ai_room(PlayerId creator_id,
                                                       const std::string& creator_name,
                                                       int creator_fd,
                                                       const TimeControl& tc,
                                                       AIDifficulty difficulty) {
    GameId id = next_id_.fetch_add(1);

    auto room = std::make_shared<GameRoom>(id, creator_id, creator_name, creator_fd, tc, difficulty);

    std::lock_guard<std::mutex> lock(mutex_);
    rooms_[id] = room;
    return room;
}

std::shared_ptr<GameRoom> RoomManager::find_room(GameId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rooms_.find(id);
    if (it != rooms_.end()) return it->second;
    return nullptr;
}

std::shared_ptr<GameRoom> RoomManager::find_room_by_fd(int connection_fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<GameRoom> finished_room = nullptr;
    for (const auto& [id, room] : rooms_) {
        if (room->has_player(connection_fd)) {
            if (room->get_state() != RoomState::FINISHED) {
                return room;
            }
            finished_room = room;
        }
    }
    return finished_room;
}

std::shared_ptr<GameRoom> RoomManager::find_room_by_player(PlayerId player_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, room] : rooms_) {
        if (room->has_player_id(player_id)) {
            return room;
        }
    }
    return nullptr;
}

void RoomManager::remove_room(GameId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    rooms_.erase(id);
}

// ============================================================
// Queries
// ============================================================

std::vector<RoomInfo> RoomManager::list_open_rooms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoomInfo> result;

    for (const auto& [id, room] : rooms_) {
        if (room->get_state() == RoomState::WAITING) {
            RoomInfo info;
            info.id           = id;
            info.state        = RoomState::WAITING;
            info.time_control = room->get_time_control().to_string();
            info.move_count   = 0;
            // White name is available; Black hasn't joined yet
            auto history = room->get_move_history();
            info.move_count = static_cast<int>(history.size());
            result.push_back(info);
        }
    }

    return result;
}

std::vector<RoomInfo> RoomManager::list_active_rooms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoomInfo> result;

    for (const auto& [id, room] : rooms_) {
        if (room->get_state() == RoomState::IN_PROGRESS) {
            RoomInfo info;
            info.id           = id;
            info.state        = RoomState::IN_PROGRESS;
            info.time_control = room->get_time_control().to_string();
            auto history = room->get_move_history();
            info.move_count = static_cast<int>(history.size());
            result.push_back(info);
        }
    }

    return result;
}

size_t RoomManager::room_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rooms_.size();
}

// ============================================================
// Cleanup
// ============================================================

size_t RoomManager::cleanup_finished_rooms() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;

    auto it = rooms_.begin();
    while (it != rooms_.end()) {
        if (it->second->get_state() == RoomState::FINISHED) {
            it = rooms_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    return removed;
}

} // namespace game
} // namespace chess
