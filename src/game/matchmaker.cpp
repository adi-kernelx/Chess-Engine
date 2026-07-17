/**
 * matchmaker.cpp — ELO-based matchmaking implementation
 *
 * Matching algorithm:
 *   1. Sort the queue by ELO (ascending).
 *   2. Walk through adjacent pairs. For each pair (A, B):
 *      a. Check if |A.elo - B.elo| is within BOTH players' acceptable range.
 *      b. Check if their time controls are compatible.
 *      c. If yes, remove both from the queue and create a GameRoom.
 *   3. Return all matches found.
 *
 * Sorting by ELO ensures adjacent players are the closest in rating,
 * which makes the greedy pairing optimal for minimizing ELO difference.
 *
 * Time controls must match exactly (same base time and increment).
 * A future improvement could group by time control category (bullet/blitz/rapid).
 */

#include "game/matchmaker.h"
#include "core/logger.h"
#include <algorithm>
#include <cmath>

namespace chess {
namespace game {

Matchmaker::Matchmaker(RoomManager& room_mgr)
    : room_mgr_(room_mgr) {}

// ============================================================
// Enqueue — add a player to the matchmaking queue
// ============================================================

bool Matchmaker::enqueue(int connection_fd, PlayerId player_id,
                         const std::string& username, int elo,
                         const TimeControl& tc) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for duplicates
    for (const auto& entry : queue_) {
        if (entry.connection_fd == connection_fd) {
            return false;  // Already queued
        }
    }

    QueueEntry entry;
    entry.connection_fd = connection_fd;
    entry.player_id     = player_id;
    entry.username      = username;
    entry.elo           = elo;
    entry.time_control  = tc;
    entry.enqueue_time  = std::chrono::steady_clock::now();

    queue_.push_back(entry);

    core::Logger::info("game", "Matchmaker",
        username + " (ELO " + std::to_string(elo) + ") joined queue. Queue size: " +
        std::to_string(queue_.size()));

    return true;
}

// ============================================================
// Dequeue — remove a player (disconnect or cancel)
// ============================================================

bool Matchmaker::dequeue(int connection_fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(queue_.begin(), queue_.end(),
        [connection_fd](const QueueEntry& e) {
            return e.connection_fd == connection_fd;
        });

    if (it != queue_.end()) {
        core::Logger::info("game", "Matchmaker",
            it->username + " left queue. Queue size: " +
            std::to_string(queue_.size() - 1));
        queue_.erase(it);
        return true;
    }
    return false;
}

// ============================================================
// try_match — scan the queue and pair compatible players
// ============================================================

std::vector<MatchResult> Matchmaker::try_match() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MatchResult> matches;

    if (queue_.size() < 2) return matches;

    // Sort by ELO so adjacent players are closest in rating
    std::sort(queue_.begin(), queue_.end(),
        [](const QueueEntry& a, const QueueEntry& b) {
            return a.elo < b.elo;
        });

    // Track which indices have been matched (to avoid double-matching)
    std::vector<bool> matched(queue_.size(), false);

    for (size_t i = 0; i + 1 < queue_.size(); ++i) {
        if (matched[i]) continue;

        for (size_t j = i + 1; j < queue_.size(); ++j) {
            if (matched[j]) continue;

            const auto& a = queue_[i];
            const auto& b = queue_[j];

            // Check ELO compatibility (both players must accept each other)
            int elo_diff = std::abs(a.elo - b.elo);
            if (elo_diff > a.acceptable_range() || elo_diff > b.acceptable_range()) {
                continue;
            }

            // Check time control compatibility (must be exact match)
            if (a.time_control.base_time_ms != b.time_control.base_time_ms ||
                a.time_control.increment_ms != b.time_control.increment_ms) {
                continue;
            }

            // Match found! Create a game room.
            // Higher-rated player gets a random color; for simplicity,
            // the first player in the queue gets White.
            auto room = room_mgr_.create_room(
                a.player_id, a.username, a.connection_fd, a.time_control);

            room->join(b.player_id, b.username, b.connection_fd);

            MatchResult result;
            result.white_fd    = a.connection_fd;
            result.black_fd    = b.connection_fd;
            result.white_id    = a.player_id;
            result.black_id    = b.player_id;
            result.white_name  = a.username;
            result.black_name  = b.username;
            result.game_id     = room->get_id();
            result.time_control = a.time_control;

            matches.push_back(result);

            matched[i] = true;
            matched[j] = true;

            core::Logger::info("game", "Matchmaker",
                "Matched: " + a.username + " (" + std::to_string(a.elo) + ") vs " +
                b.username + " (" + std::to_string(b.elo) + ") → Game " +
                std::to_string(room->get_id()));

            // Fire the callback if set
            if (match_cb_) {
                match_cb_(result);
            }

            break;  // Move to next unmatched player
        }
    }

    // Remove matched entries from the queue (iterate in reverse to preserve indices)
    for (int i = static_cast<int>(queue_.size()) - 1; i >= 0; --i) {
        if (matched[static_cast<size_t>(i)]) {
            queue_.erase(queue_.begin() + i);
        }
    }

    return matches;
}

// ============================================================
// Queries
// ============================================================

bool Matchmaker::is_queued(int connection_fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : queue_) {
        if (entry.connection_fd == connection_fd) return true;
    }
    return false;
}

size_t Matchmaker::queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

} // namespace game
} // namespace chess
