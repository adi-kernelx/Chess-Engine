/**
 * matchmaker.cpp — Bucketed ELO-tree matchmaking implementation
 *
 * Architecture:
 *   Primary index:  unordered_map<TimeControl, multimap<int, QueueEntry>>
 *   Reverse index:  unordered_map<int, QueueLocation>
 *
 * Matching algorithm (per bucket):
 *   The multimap (Red-Black Tree) keeps players sorted by ELO at all times.
 *   try_match() performs a single linear pass through each bucket's tree,
 *   checking adjacent pairs for ELO compatibility. No sorting is ever needed.
 *
 *   This is analogous to a ride-sharing app: players are grouped by their
 *   "ride type" (TimeControl), then matched to the nearest neighbor within
 *   an expanding "fence" (ELO range that widens over time).
 *
 * Complexity:
 *   enqueue:    O(log N) — Red-Black Tree insert
 *   dequeue:    O(1)     — stored iterator erase
 *   is_queued:  O(1)     — hash map lookup
 *   try_match:  O(N)     — linear pass, no sorting
 */

#include "game/matchmaker.h"
#include "core/logger.h"
#include <cmath>

namespace chess {
namespace game {

Matchmaker::Matchmaker(RoomManager& room_mgr)
    : room_mgr_(room_mgr) {}

// ============================================================
// Enqueue — O(log N) insert into the correct ELO tree
// ============================================================

bool Matchmaker::enqueue(int connection_fd, PlayerId player_id,
                         const std::string& username, int elo,
                         const TimeControl& tc) {
    std::lock_guard<std::mutex> lock(mutex_);

    // O(1) duplicate check via the reverse index
    if (fd_index_.count(connection_fd)) {
        return false;  // Already queued
    }

    // Build the queue entry
    QueueEntry entry;
    entry.connection_fd = connection_fd;
    entry.player_id     = player_id;
    entry.username      = username;
    entry.elo           = elo;
    entry.time_control  = tc;
    entry.enqueue_time  = std::chrono::steady_clock::now();

    // Insert into the ELO tree for this time control bucket — O(log N)
    // multimap::insert returns an iterator to the inserted element
    auto& bucket = buckets_[tc];
    auto it = bucket.insert({elo, entry});

    // Store reverse index: fd → {TimeControl, iterator} — O(1) amortized
    fd_index_[connection_fd] = {tc, it};
    total_size_++;

    core::Logger::info("game", "Matchmaker",
        username + " (ELO " + std::to_string(elo) + ") joined queue. Queue size: " +
        std::to_string(total_size_));

    return true;
}

// ============================================================
// Dequeue — O(1) removal using stored iterator
// ============================================================

bool Matchmaker::dequeue(int connection_fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    // O(1) lookup via the reverse index
    auto idx_it = fd_index_.find(connection_fd);
    if (idx_it == fd_index_.end()) return false;

    const auto& loc = idx_it->second;
    std::string name = loc.it->second.username;

    // Erase from the ELO tree using the stored iterator — O(1)
    auto bucket_it = buckets_.find(loc.tc);
    if (bucket_it != buckets_.end()) {
        bucket_it->second.erase(loc.it);
        // Clean up empty buckets to avoid stale hash entries
        if (bucket_it->second.empty()) {
            buckets_.erase(bucket_it);
        }
    }

    // Remove from reverse index — O(1)
    fd_index_.erase(idx_it);
    total_size_--;

    core::Logger::info("game", "Matchmaker",
        name + " left queue. Queue size: " + std::to_string(total_size_));

    return true;
}

// ============================================================
// try_match — O(N) linear sweep through pre-sorted ELO trees
// ============================================================

std::vector<MatchResult> Matchmaker::try_match() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MatchResult> results;

    // Iterate over each time control bucket independently.
    // Players in the "10+5" bucket will never be matched with "3+2" players.
    for (auto& [tc, bucket] : buckets_) {
        if (bucket.size() < 2) continue;

        // The multimap is already sorted by ELO (Red-Black Tree property).
        // Walk adjacent pairs: the closest ELO neighbors are always adjacent.
        auto it = bucket.begin();
        while (it != bucket.end()) {
            auto next = std::next(it);
            if (next == bucket.end()) break;

            auto& a = it->second;
            auto& b = next->second;

            int elo_diff = std::abs(a.elo - b.elo);

            // Both players must accept the ELO difference (fence check)
            if (elo_diff <= a.acceptable_range() &&
                elo_diff <= b.acceptable_range()) {

                // ── Match found! Create a game room. ──
                auto room = room_mgr_.create_room(
                    a.player_id, a.username, a.connection_fd, a.time_control);
                room->join(b.player_id, b.username, b.connection_fd);

                MatchResult result;
                result.white_fd     = a.connection_fd;
                result.black_fd     = b.connection_fd;
                result.white_id     = a.player_id;
                result.black_id     = b.player_id;
                result.white_name   = a.username;
                result.black_name   = b.username;
                result.game_id      = room->get_id();
                result.time_control = a.time_control;

                core::Logger::info("game", "Matchmaker",
                    "Matched: " + a.username + " (" + std::to_string(a.elo) + ") vs " +
                    b.username + " (" + std::to_string(b.elo) + ") → Game " +
                    std::to_string(result.game_id));

                // Remove both from the reverse index — O(1) each
                fd_index_.erase(a.connection_fd);
                fd_index_.erase(b.connection_fd);
                total_size_ -= 2;

                // Erase both from the tree.
                // multimap::erase(iterator) returns the next valid iterator.
                it = bucket.erase(it);   // erases 'a', returns iterator to 'b'
                it = bucket.erase(it);   // erases 'b', returns iterator to next

                results.push_back(result);

                // Fire the callback (e.g., send WebSocket notifications)
                if (match_cb_) {
                    match_cb_(result);
                }
            } else {
                // No match — advance to the next pair
                ++it;
            }
        }
    }

    return results;
}

// ============================================================
// Queries — O(1) via reverse index and cached size
// ============================================================

bool Matchmaker::is_queued(int connection_fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_index_.count(connection_fd) > 0;
}

size_t Matchmaker::queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_size_;
}

} // namespace game
} // namespace chess
