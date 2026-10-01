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
                                                    const TimeControl& tc,
                                                    int64_t db_player_id,
                                                    int elo) {
    GameId id = next_id_.fetch_add(1);

    auto room = std::make_shared<GameRoom>(id, creator_id, creator_name, creator_fd, tc,
                                           db_player_id, elo);

    GameEventListenerPtr listener_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rooms_[id] = room;
        listener_copy = default_listener_;
    }
    // Attach outside the manager mutex — `add_listener` takes the room's
    // own mutex, and we never nest room-level acquires inside the
    // manager-level acquire.
    if (listener_copy) room->add_listener(listener_copy);
    return room;
}

std::shared_ptr<GameRoom> RoomManager::create_ai_room(PlayerId creator_id,
                                                       const std::string& creator_name,
                                                       int creator_fd,
                                                       const TimeControl& tc,
                                                       AIDifficulty difficulty,
                                                       int64_t db_player_id,
                                                       int elo) {
    GameId id = next_id_.fetch_add(1);

    auto room = std::make_shared<GameRoom>(id, creator_id, creator_name, creator_fd,
                                           tc, difficulty, db_player_id, elo);

    GameEventListenerPtr listener_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rooms_[id] = room;
        listener_copy = default_listener_;
    }
    if (listener_copy) room->add_listener(listener_copy);
    return room;
}

std::shared_ptr<GameRoom> RoomManager::create_reserved_tournament_room(
        int64_t tournament_id, int64_t pairing_id,
        int64_t white_db_id, const std::string& white_name, int white_elo,
        int64_t black_db_id, const std::string& black_name, int black_elo,
        const TimeControl& tc) {
    std::shared_ptr<GameRoom> room;
    GameEventListenerPtr listener_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = tournament_pairing_rooms_.find(pairing_id);
        if (existing != tournament_pairing_rooms_.end()) {
            auto it = rooms_.find(existing->second);
            if (it != rooms_.end()) return it->second;
            tournament_pairing_rooms_.erase(existing);
        }
        const GameId id = next_id_.fetch_add(1);
        room = std::make_shared<GameRoom>(id, tournament_id, pairing_id,
            white_db_id, white_name, white_elo,
            black_db_id, black_name, black_elo, tc);
        rooms_[id] = room;
        tournament_pairing_rooms_[pairing_id] = id;
        listener_copy = default_listener_;
    }
    if (listener_copy) room->add_listener(listener_copy);
    return room;
}

std::shared_ptr<GameRoom> RoomManager::find_room_by_pairing(int64_t pairing_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto map_it = tournament_pairing_rooms_.find(pairing_id);
    if (map_it == tournament_pairing_rooms_.end()) return nullptr;
    auto room_it = rooms_.find(map_it->second);
    return room_it == rooms_.end() ? nullptr : room_it->second;
}

void RoomManager::ensure_next_id_above(GameId existing_id) {
    GameId desired = existing_id + 1;
    GameId current = next_id_.load();
    while (current < desired && !next_id_.compare_exchange_weak(current, desired)) {}
}

void RoomManager::reserve_tournament_player(int64_t db_player_id) {
    if (db_player_id <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_tournament_players_.insert(db_player_id);
}

void RoomManager::release_tournament_player(int64_t db_player_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_tournament_players_.erase(db_player_id);
}

bool RoomManager::is_tournament_player_reserved(int64_t db_player_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reserved_tournament_players_.count(db_player_id) != 0;
}

void RoomManager::set_default_listener(GameEventListenerPtr listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_listener_ = std::move(listener);
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

std::shared_ptr<GameRoom> RoomManager::find_room_by_db_player(int64_t db_player_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, room] : rooms_) {
        (void)id;
        if (room->get_state() != RoomState::FINISHED
            && room->has_db_player_id(db_player_id)) {
            return room;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<GameRoom>> RoomManager::rooms_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<GameRoom>> result;
    result.reserve(rooms_.size());
    for (const auto& [id, room] : rooms_) {
        (void)id;
        result.push_back(room);
    }
    return result;
}

void RoomManager::remove_room(GameId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tournament_pairing_rooms_.begin();
         it != tournament_pairing_rooms_.end();) {
        if (it->second == id) it = tournament_pairing_rooms_.erase(it);
        else ++it;
    }
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
            info.id              = id;
            info.state           = RoomState::WAITING;
            info.time_control    = room->get_time_control().to_string();
            info.white_name      = room->get_username(Color::WHITE);
            info.black_name      = "";   // hasn't joined yet
            info.move_count      = static_cast<int>(room->get_move_history().size());
            info.spectator_count = static_cast<int>(room->spectator_count());
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
            info.id              = id;
            info.state           = RoomState::IN_PROGRESS;
            info.time_control    = room->get_time_control().to_string();
            info.white_name      = room->get_username(Color::WHITE);
            info.black_name      = room->get_username(Color::BLACK);
            info.move_count      = static_cast<int>(room->get_move_history().size());
            info.spectator_count = static_cast<int>(room->spectator_count());
            result.push_back(info);
        }
    }

    return result;
}

size_t RoomManager::remove_spectator_everywhere(int connection_fd) {
    // Copy shared_ptrs under the outer lock, then act on each room using its
    // own mutex outside — same discipline as the rest of this class (the
    // outer lock never nests calls that could deadlock on the room mutex).
    std::vector<std::shared_ptr<GameRoom>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(rooms_.size());
        for (const auto& [_id, room] : rooms_) snapshot.push_back(room);
    }
    size_t removed = 0;
    for (const auto& room : snapshot) {
        if (room->remove_spectator(connection_fd)) ++removed;
    }
    return removed;
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
            for (auto map_it = tournament_pairing_rooms_.begin();
                 map_it != tournament_pairing_rooms_.end();) {
                if (map_it->second == it->first) map_it = tournament_pairing_rooms_.erase(map_it);
                else ++map_it;
            }
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
